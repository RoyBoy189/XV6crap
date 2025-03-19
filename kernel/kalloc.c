// Physical memory allocator, for user processes,
// kernel stacks, page-table pages,
// and pipe buffers. Allocates whole 4096-byte pages.

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "defs.h"

void freerange(void *pa_start, void *pa_end);

extern char end[]; // first address after kernel.
                   // defined by kernel.ld.

struct run {
  struct run *next;
};

struct {
  struct spinlock lock;
  struct run *freelist;
} kmem;

void
kinit()
{
  initlock(&kmem.lock, "kmem");
  freerange(end, (void*)PHYSTOP);
}

void
freerange(void *pa_start, void *pa_end)
{
  char *p;
  p = (char*)PGROUNDUP((uint64)pa_start);
  for(; p + PGSIZE <= (char*)pa_end; p += PGSIZE)
    kfree(p);
}

// Free the page of physical memory pointed at by pa,
// which normally should have been returned by a
// call to kalloc().  (The exception is when
// initializing the allocator; see kinit above.)
void
kfree(void *pa)
{
  struct run *r;

  if(((uint64)pa % PGSIZE) != 0 || (char*)pa < end || (uint64)pa >= PHYSTOP)
    panic("kfree");

  // Fill with junk to catch dangling refs.
  memset(pa, 1, PGSIZE);

  r = (struct run*)pa;

  acquire(&kmem.lock);
  r->next = kmem.freelist;
  kmem.freelist = r;
  release(&kmem.lock);
}

// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
void *
kalloc(void)
{
  struct run *r;

  acquire(&kmem.lock);
  r = kmem.freelist;
  if(r)
    kmem.freelist = r->next;
  release(&kmem.lock);

  if(r)
    memset((char*)r, 5, PGSIZE); // fill with junk
  return (void*)r;
}

void *
kalloc_superpage(void)
{
  char *mem = kalloc();
  if (mem == 0)
    return 0;

  // Allocate additional pages to form a 2MB superpage
  for (int i = 1; i < (2 * 1024 * 1024) / PGSIZE; i++) {
    char *page = kalloc();
    if (page == 0) {
      // Free already allocated pages if allocation fails
      for (int j = 0; j < i; j++) {
        kfree(mem + j * PGSIZE);
      }
      return 0;
    }
    // Ensure contiguous allocation
    if (page != mem + i * PGSIZE) {
      // Free already allocated pages if not contiguous
      for (int j = 0; j <= i; j++) {
        kfree(mem + j * PGSIZE);
      }
      return 0;
    }
  }
  return mem;
}


void *superalloc(void) {
  // Request a contiguous 2MB block from kalloc
  void *mem = kalloc_superpage();
  if (mem == 0)
      return 0;

  // Zero out the entire 2MB region
  memset(mem, 0, 2 * 1024 * 1024);

  return mem;
}

void superfree(void *mem) {
  if ((uint64)mem % (2 * 1024 * 1024) != 0) {
      panic("superfree: unaligned superpage");
  }
  kfree(mem);
}

void *
kalloc_size(int size)
{
  if (size == PGSIZE) {
    return kalloc();
  } else if (size == 2 * 1024 * 1024) {
    return kalloc_superpage();
  } else {
    panic("kalloc_size: unsupported size");
    return 0;
  }
}

void
kfree_superpage(void *mem)
{
  // Ensure the memory is aligned to 2MB
  if ((uint64)mem % (2 * 1024 * 1024) != 0)
    panic("kfree_superpage: not aligned");

  // Free each 4KB page within the 2MB superpage
  for (int i = 0; i < (2 * 1024 * 1024) / PGSIZE; i++) {
    kfree((char*)mem + i * PGSIZE);
  }
}


void
kfree_size(void *mem, int size)
{
  if (size == PGSIZE) {
    kfree(mem);
  } else if (size == 2 * 1024 * 1024) {
    kfree_superpage(mem);
  } else {
    panic("kfree_size: unsupported size");
  }
}

