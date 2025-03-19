#include "param.h"
#include "types.h"
#include "memlayout.h"
#include "elf.h"
#include "riscv.h"
#include "defs.h"
#include "kalloc.h"
// Define PTE_PS for superpage
#define PTE_PS (1L << 7)
#include "fs.h"
#include <stdio.h>

/*
 * the kernel's page table.
 */
pagetable_t kernel_pagetable;

extern char etext[];  // kernel.ld sets this to end of kernel code.

extern char trampoline[]; // trampoline.S

// Make a direct-map page table for the kernel.
pagetable_t
kvmmake(void)
{
  pagetable_t kpgtbl;

  kpgtbl = (pagetable_t) kalloc();
  memset(kpgtbl, 0, PGSIZE);

  // uart registers
  kvmmap(kpgtbl, UART0, UART0, PGSIZE, PTE_R | PTE_W);

  // virtio mmio disk interface
  kvmmap(kpgtbl, VIRTIO0, VIRTIO0, PGSIZE, PTE_R | PTE_W);

  // PLIC
  kvmmap(kpgtbl, PLIC, PLIC, 0x4000000, PTE_R | PTE_W);

  // map kernel text executable and read-only.
  kvmmap(kpgtbl, KERNBASE, KERNBASE, (uint64)etext-KERNBASE, PTE_R | PTE_X);

  // map kernel data and the physical RAM we'll make use of.
  kvmmap(kpgtbl, (uint64)etext, (uint64)etext, PHYSTOP-(uint64)etext, PTE_R | PTE_W);

  // map the trampoline for trap entry/exit to
  // the highest virtual address in the kernel.
  kvmmap(kpgtbl, TRAMPOLINE, (uint64)trampoline, PGSIZE, PTE_R | PTE_X);

  // allocate and map a kernel stack for each process.
  proc_mapstacks(kpgtbl);
  
  return kpgtbl;
}

// Initialize the one kernel_pagetable
void
kvminit(void)
{
  kernel_pagetable = kvmmake();
}

// Switch h/w page table register to the kernel's page table,
// and enable paging.
void
kvminithart()
{
  // wait for any previous writes to the page table memory to finish.
  sfence_vma();

  w_satp(MAKE_SATP(kernel_pagetable));

  // flush stale entries from the TLB.
  sfence_vma();
}

// Return the address of the PTE in page table pagetable
// that corresponds to virtual address va.  If alloc!=0,
// create any required page-table pages.
//
// The risc-v Sv39 scheme has three levels of page-table
// pages. A page-table page contains 512 64-bit PTEs.
// A 64-bit virtual address is split into five fields:
//   39..63 -- must be zero.
//   30..38 -- 9 bits of level-2 index.
//   21..29 -- 9 bits of level-1 index.
//   12..20 -- 9 bits of level-0 index.
//    0..11 -- 12 bits of byte offset within the page.
pte_t *
walk(pagetable_t pagetable, uint64 va, int alloc)
{
  pte_t *pte;
  pagetable_t pagetable2;

  for(int level = 2; level > 0; level--){
    pte = &pagetable[PX(level, va)];
    if(*pte & PTE_V){
      pagetable = (pagetable_t)PTE2PA(*pte);
    } else {
      if(!alloc || (pagetable2 = (pde_t *)kalloc()) == 0)
        return 0;
      memset(pagetable2, 0, PGSIZE);
      *pte = PA2PTE(pagetable2) | PTE_V;
      pagetable = pagetable2;
    }
  }
  return &pagetable[PX(0, va)];
}

// Look up a virtual address, return the physical address,
// or 0 if not mapped.
// Can only be used to look up user pages.
uint64 walkaddr(pagetable_t pagetable, uint64 va) {
  pte_t *pte;
  uint64 pa;

  if (va >= MAXVA)
    return 0;

  pte = walk(pagetable, va, 0);
  if (pte == 0)
    return 0;
  if ((*pte & PTE_V) == 0)
    return 0;
  if ((*pte & PTE_U) == 0)
    return 0;

  // Check if this is a superpage mapping
  if (*pte & PTE_PS) {
    uint64 superpage_base = PTE2PA(*pte);
    uint64 offset = va & (2 * 1024 * 1024 - 1);  // Offset within a 2MB page
    return superpage_base + offset;
  }

  // Regular 4KB page mapping
  pa = PTE2PA(*pte);
  return pa;
}

// add a mapping to the kernel page table.
// only used when booting.
// does not flush TLB or enable paging.
void
kvmmap(pagetable_t kpgtbl, uint64 va, uint64 pa, uint64 sz, int perm)
{
  if(mappages(kpgtbl, va, sz, pa, perm, PGSIZE) != 0)
    panic("kvmmap");
}
// Create PTEs for virtual addresses starting at va that refer to
// physical addresses starting at pa.
// va and size MUST be page-aligned.
// Returns 0 on success, -1 if walk() couldn't
// allocate a needed page-table page.
#define MB2SIZE (2 * 1024 * 1024) // 2MB
#define GBSIZE  (1 * 1024 * 1024 * 1024) // 1GB
int
mappages(pagetable_t pagetable, uint64 va, uint64 size, uint64 pa, int perm, int pagesize)
{
  uint64 a, last;
  pte_t *pte;

  if((va % pagesize) != 0)
    panic("mappages: va not aligned");

  if((size % pagesize) != 0)
    panic("mappages: size not aligned");

  if(size == 0)
    panic("mappages: size");
  
  a = va;
  last = va + size - pagesize;
  for(;;){
    if ((a % GBSIZE == 0) && (last - a + pagesize >= GBSIZE)) {
      // Map a full 1GB page if aligned and enough space
      if((pte = walk(pagetable, a, 1)) == 0)
        return -1;
      if(*pte & PTE_V)
        panic("mappages: remap");
      *pte = PA2PTE(pa) | perm | PTE_V | PTE_PS;
      //intf("Mapped 1GB page at va: %p\n", (void*)a);
      a += GBSIZE;
      pa += GBSIZE;
    } else if ((a % MB2SIZE == 0) && (last - a + pagesize >= MB2SIZE)) {
      // Map a full 2MB superpage if aligned and enough space
      if((pte = walk(pagetable, a, 1)) == 0)
        return -1;
      if(*pte & PTE_V)
        panic("mappages: remap");
      *pte = PA2PTE(pa) | perm | PTE_V | PTE_PS;
     //rintf("Mapped 2MB superpage at va: %p\n", (void*)a);
      a += MB2SIZE;
      pa += MB2SIZE;
    } else {
      // Map a regular 4KB page
      if((pte = walk(pagetable, a, 1)) == 0)
        return -1;
      if(*pte & PTE_V)
        panic("mappages: remap");
      *pte = PA2PTE(pa) | perm | PTE_V;
      //intf("Mapped 4KB page at va: %p\n", (void*)a);
      a += PGSIZE;
      pa += PGSIZE;
    }
    if(a > last)
      break;
  }
  return 0;
}
// Remove npages of mappings starting from va. va must be
// page-aligned. The mappings must exist.
// Optionally free the physical memory.
void uvmunmap(pagetable_t pagetable, uint64 va, uint64 npages, int do_free) {
  uint64 a, pa;
  pte_t *pte;
  uint64 end = va + npages * PGSIZE;

  if ((va % PGSIZE) != 0)
    panic("uvmunmap: not aligned");

  for (a = va; a < end; ) {
    pte = walk(pagetable, a, 0);
    if (pte == 0)
      panic("uvmunmap: walk");
    if ((*pte & PTE_V) == 0)
      panic("uvmunmap: not mapped");
    if (PTE_FLAGS(*pte) == PTE_V)
      panic("uvmunmap: not a leaf");

    uint64 pagesize = PGSIZE;  // Default to 4KB
    if (*pte & PTE_PS) {  
      pagesize = 2 * 1024 * 1024; // 2MB superpage
    }

    if (do_free) {
      pa = PTE2PA(*pte);
      kfree((void*)pa); // Free the entire allocated block
    }

    *pte = 0; // Clear the PTE

    a += pagesize; // Skip entire page (either 4KB or 2MB)
  }
}

// create an empty user page table.
// returns 0 if out of memory.
pagetable_t
uvmcreate()
{
  pagetable_t pagetable;
  pagetable = (pagetable_t) kalloc();
  if(pagetable == 0)
    return 0;
  memset(pagetable, 0, PGSIZE);
  return pagetable;
}

// Load the user initcode into address 0 of pagetable,
// for the very first process.
// sz must be less than a page.
void
uvmfirst(pagetable_t pagetable, uchar *src, uint sz)
{
  char *mem;

  if(sz >= PGSIZE)
    panic("uvmfirst: more than a page");
  mem = kalloc();
  memset(mem, 0, PGSIZE);
  mappages(pagetable, 0, PGSIZE, (uint64)mem, PTE_W|PTE_R|PTE_X|PTE_U, PGSIZE);
  memmove(mem, src, sz);
}

// Allocate PTEs and physical memory to grow process from oldsz to
// newsz, which need not be page aligned.  Returns new size or 0 on error.
uint64
uvmalloc(pagetable_t pagetable, uint64 oldsz, uint64 newsz, int xperm)
{
  char *mem;
  uint64 a;
  uint64 superpage_size = 2 * 1024 * 1024; // 2MB
  uint64 alloc_start = oldsz; // Track where allocation begins

  if (newsz < oldsz)
      return oldsz;

  oldsz = PGROUNDUP(oldsz);
  for (a = oldsz; a < newsz; a += PGSIZE) {
      if ((a % superpage_size == 0) && (newsz - a >= superpage_size)) {
          // Try to allocate a full 2MB superpage
          mem = superalloc();
          if (!mem) {
              printf("superalloc failed at va: %p\n", (void*)a);
              fflush(stdout);
              break; // Avoid partial allocation
          }
          memset(mem, 0, superpage_size);
          if (mappages(pagetable, a, superpage_size, (uint64)mem, PTE_R | PTE_U | xperm, superpage_size) != 0) {
              printf("mappages failed for superpage at va: %p\n", (void*)a);
              fflush(stdout);
              superfree(mem); // Free the failed superpage allocation
              break;
          }
          printf("Mapped 2MB superpage at va: %p, pa: %p\n", (void*)a, (void*)mem);
          fflush(stdout);
          a += superpage_size - PGSIZE; // Move ahead by the remaining size of the superpage
      } else {
          // Allocate regular 4KB page
          mem = kalloc();
          if (!mem) {
              printf("kalloc failed at va: %p\n", (void*)a);
              fflush(stdout);
              break;
          }
          memset(mem, 0, PGSIZE);
          if (mappages(pagetable, a, PGSIZE, (uint64)mem, PTE_R | PTE_U | xperm, PGSIZE) != 0) {
              printf("mappages failed for 4KB page at va: %p\n", (void*)a);
              fflush(stdout);
              kfree(mem);
              break;
          }
          printf("Mapped 4KB page at va: %p, pa: %p\n", (void*)a, (void*)mem);
          fflush(stdout);
      }
  }

  if (a < newsz) {
      // Rollback allocation on failure
      printf("uvmalloc failed, rolling back allocation\n");
      uvmdealloc(pagetable, alloc_start, oldsz);
      return 0;
  }

  return newsz;
}

// Deallocate user pages to bring the process size from oldsz to
// newsz.  oldsz and newsz need not be page-aligned, nor does newsz
// need to be less than oldsz.  oldsz can be larger than the actual
// process size.  Returns the new process size.
uint64
uvmdealloc(pagetable_t pagetable, uint64 oldsz, uint64 newsz)
{
  uint64 a;
  uint64 superpage_size = 2 * 1024 * 1024; // 2MB

  if (newsz >= oldsz)
      return oldsz;

  newsz = PGROUNDUP(newsz);
  oldsz = PGROUNDUP(oldsz);

  for (a = newsz; a < oldsz; a += PGSIZE) {
      if ((a % superpage_size == 0) && ((oldsz - a) >= superpage_size)) {
          // Unmap full 2MB superpage if aligned
          printf("Unmapping 2MB superpage at va: %p\n", (void*)a);
          uvmunmap(pagetable, a, superpage_size / PGSIZE, 1);
          a += superpage_size - PGSIZE; // Skip remaining part of the superpage
      } else {
          // Unmap regular 4KB page
          printf("Unmapping 4KB page at va: %p\n", (void*)a);
          uvmunmap(pagetable, a, 1, 1);
      }
  }

  return newsz;
}

// Recursively free page-table pages.
// All leaf mappings must already have been removed.
void freewalk(pagetable_t pagetable)
{
  for(int i = 0; i < 512; i++){
    pte_t pte = pagetable[i];
    if((pte & PTE_V) && (pte & (PTE_R|PTE_W|PTE_X)) == 0){
      // This PTE points to a lower-level page table.
      uint64 child = PTE2PA(pte);
      freewalk((pagetable_t)child);
      pagetable[i] = 0;
    } else if (pte & PTE_V) {
      // If this is a superpage, free it directly
      if (pte & PTE_PS) {
        uint64 pa = PTE2PA(pte);
        kfree((void*)pa);
      } else {
        panic("freewalk: leaf");
      }
    }
  }
  kfree((void*)pagetable);
}

// Free user memory pages,
// then free page-table pages.
void
uvmfree(pagetable_t pagetable, uint64 sz)
{
  if(sz > 0)
    uvmunmap(pagetable, 0, PGROUNDUP(sz)/PGSIZE, 1);
  freewalk(pagetable);
}

// Given a parent process's page table, copy
// its memory into a child's page table.
// Copies both the page table and the
// physical memory.
// returns 0 on success, -1 on failure.
// frees any allocated pages on failure.
int uvmcopy(pagetable_t old, pagetable_t new, uint64 sz) {
  pte_t *pte;
  uint64 pa, i;
  uint flags;
  char *mem;
  uint64 pagesize;

  for (i = 0; i < sz; i += pagesize) {
    if ((pte = walk(old, i, 0)) == 0)
      panic("uvmcopy: pte should exist");
    if ((*pte & PTE_V) == 0)
      panic("uvmcopy: page not present");

    pa = PTE2PA(*pte);
    flags = PTE_FLAGS(*pte);

    pagesize = PGSIZE; // Default to 4KB
    if (*pte & PTE_PS) {
      pagesize = 2 * 1024 * 1024; // 2MB Superpage
    }

    // Allocate the correct size
    if ((mem = kalloc_size(pagesize)) == 0)  // Use a size-aware allocator
      goto err;

    memmove(mem, (char*)pa, pagesize);

    if (mappages(new, i, pagesize, (uint64)mem, flags, pagesize) != 0) {
      kfree_size(mem, pagesize); // Free correctly
      goto err;
    }
  }
  return 0;

err:
  uvmunmap(new, 0, i / PGSIZE, 1);
  return -1;
}

// mark a PTE invalid for user access.
// used by exec for the user stack guard page.
void
uvmclear(pagetable_t pagetable, uint64 va)
{
  pte_t *pte;
  
  pte = walk(pagetable, va, 0);
  if(pte == 0)
    panic("uvmclear");
  *pte &= ~PTE_U;
}

// Copy from kernel to user.
// Copy len bytes from src to virtual address dstva in a given page table.
// Return 0 on success, -1 on error.
int copyout(pagetable_t pagetable, uint64 dstva, char *src, uint64 len) {
  uint64 n, va0;
  pte_t *pte;
  uint64 pa, pagesize;

  while (len > 0) {
    va0 = PGROUNDDOWN(dstva);
    pte = walk(pagetable, va0, 0);
    if (pte == 0 || (*pte & PTE_V) == 0)
      return -1;

    pa = PTE2PA(*pte);
    pagesize = (*pte & PTE_PS) ? (2 * 1024 * 1024) : PGSIZE;  // Detect 2MB page
    n = pagesize - (dstva - va0);
    if (n > len)
      n = len;

    memmove((void *)(pa + (dstva - va0)), src, n);

    len -= n;
    src += n;
    dstva = va0 + pagesize;  // Move to the next page or superpage
  }
  return 0;
}

// Copy from user to kernel.
// Copy len bytes to dst from virtual address srcva in a given page table.
// Return 0 on success, -1 on error.
int copyin(pagetable_t pagetable, char *dst, uint64 srcva, uint64 len) {
  uint64 n, va0;
  pte_t *pte;
  uint64 pa, pagesize;

  while (len > 0) {
    va0 = PGROUNDDOWN(srcva);
    pte = walk(pagetable, va0, 0);
    if (pte == 0 || (*pte & PTE_V) == 0)
      return -1;

    pa = PTE2PA(*pte);
    pagesize = (*pte & PTE_PS) ? (2 * 1024 * 1024) : PGSIZE;
    n = pagesize - (srcva - va0);
    if (n > len)
      n = len;

    memmove(dst, (void *)(pa + (srcva - va0)), n);

    len -= n;
    dst += n;
    srcva = va0 + pagesize;
  }
  return 0;
}

// Copy a null-terminated string from user to kernel.
// Copy bytes to dst from virtual address srcva in a given page table,
// until a '\0', or max.
// Return 0 on success, -1 on error.
int copyinstr(pagetable_t pagetable, char *dst, uint64 srcva, uint64 max) {
  uint64 n, va0, pa0, pagesize;
  int got_null = 0;

  while (got_null == 0 && max > 0) {
    va0 = PGROUNDDOWN(srcva);
    pte_t *pte = walk(pagetable, va0, 0);
    if (pte == 0 || (*pte & PTE_V) == 0)
      return -1;

    pa0 = PTE2PA(*pte);
    pagesize = (*pte & PTE_PS) ? (2 * 1024 * 1024) : PGSIZE;
    n = pagesize - (srcva - va0);
    if (n > max)
      n = max;

    char *p = (char *)(pa0 + (srcva - va0));
    while (n > 0) {
      if (*p == '\0') {
        *dst = '\0';
        got_null = 1;
        break;
      } else {
        *dst = *p;
      }
      --n;
      --max;
      p++;
      dst++;
    }

    srcva = va0 + pagesize;
  }

  return got_null ? 0 : -1;
}
