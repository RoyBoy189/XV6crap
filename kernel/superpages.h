// filepath: kernel/superpages.h
#ifndef SUPERPAGES_H
#define SUPERPAGES_H

// Declare the superpage-related functions
int supercopy(pagetable_t old, pagetable_t new, uint64 sz);
void superfree(pagetable_t pagetable, uint64 oldsz, uint64 newsz);
int supergrowproc(int n);

#endif // SUPERPAGES_H