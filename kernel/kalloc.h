// filepath: kernel/kalloc.h
#ifndef KALLOC_H
#define KALLOC_H

// Declare the memory allocation functions
void *kalloc_superpage(void);
void kfree_superpage(void *mem);
void *superalloc(void);
void superfree(void *mem);
void *kalloc_size(int size);
void kfree_size(void *mem, int size);

#endif // KALLOC_H