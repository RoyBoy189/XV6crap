// filepath: user/pgtbltest.c
#include "kernel/types.h"
#include "kernel/stat.h"
#include "kernel/riscv.h"
#include "user/user.h"

int
main(void)
{
  char *brk;
  int i;

  printf("Testing superpages...\n");

  // Allocate 2MB of memory
  brk = sbrk(2 * 1024 * 1024);
  if (brk == (char *)-1) {
    printf("sbrk failed\n");
    exit(1);
  }

  // Write to allocated memory
  for (i = 0; i < 2 * 1024 * 1024; i++) {
    brk[i] = i % 256;
  }

  // Verify written data
  for (i = 0; i < 2 * 1024 * 1024; i++) {
    if (brk[i] != i % 256) {
      printf("Memory verification failed at %d\n", i);
      exit(1);
    }
  }

  printf("Superpage test passed\n");
  exit(0);
}