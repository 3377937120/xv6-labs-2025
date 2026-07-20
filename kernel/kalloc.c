// Physical memory allocators.
//
// kmem manages ordinary 4096-byte pages below SUPERBASE.
// supermem manages a disjoint set of aligned 2-MiB regions at the top
// of physical memory. Keeping the pools disjoint prevents one physical
// page from being allocated simultaneously as a normal page and as part
// of a superpage.

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "defs.h"

void freerange(void *pa_start, void *pa_end);

extern char end[]; // first address after kernel, defined by kernel.ld.

// A handful is sufficient for the lab, while leaving most RAM available
// to the ordinary allocator. 16 regions reserve 32 MiB of 128 MiB RAM.
#define NSUPERPAGES 16
#define SUPERBASE (PHYSTOP - NSUPERPAGES * SUPERPGSIZE)

struct run {
  struct run *next;
};

struct superrun {
  struct superrun *next;
};

struct {
  struct spinlock lock;
  struct run *freelist;
} kmem;

struct {
  struct spinlock lock;
  struct superrun *freelist;
} supermem;

void
kinit(void)
{
  initlock(&kmem.lock, "kmem");
  initlock(&supermem.lock, "supermem");

  // These ranges must not overlap.
  freerange(end, (void *)SUPERBASE);

  for(uint64 pa = SUPERBASE;
      pa + SUPERPGSIZE <= PHYSTOP;
      pa += SUPERPGSIZE)
    superfree((void *)pa);
}

void
freerange(void *pa_start, void *pa_end)
{
  char *p = (char *)PGROUNDUP((uint64)pa_start);
  for(; p + PGSIZE <= (char *)pa_end; p += PGSIZE)
    kfree(p);
}

// Free one ordinary 4-KiB page.
void
kfree(void *pa)
{
  struct run *r;

  if(((uint64)pa % PGSIZE) != 0 ||
     (char *)pa < end ||
     (uint64)pa >= SUPERBASE)
    panic("kfree");

#ifndef LAB_SYSCALL
  // Fill with junk to catch dangling references.
  memset(pa, 1, PGSIZE);
#endif

  r = (struct run *)pa;

  acquire(&kmem.lock);
  r->next = kmem.freelist;
  kmem.freelist = r;
  release(&kmem.lock);
}

// Allocate one ordinary 4-KiB page.
void *
kalloc(void)
{
  struct run *r;

  acquire(&kmem.lock);
  r = kmem.freelist;
  if(r)
    kmem.freelist = r->next;
  release(&kmem.lock);

#ifndef LAB_SYSCALL
  if(r)
    memset((char *)r, 5, PGSIZE);
#endif
  return (void *)r;
}

// Free one aligned 2-MiB physical region.
void
superfree(void *pa)
{
  struct superrun *r;

  if(((uint64)pa % SUPERPGSIZE) != 0 ||
     (uint64)pa < SUPERBASE ||
     (uint64)pa + SUPERPGSIZE > PHYSTOP)
    panic("superfree");

  memset(pa, 1, SUPERPGSIZE);
  r = (struct superrun *)pa;

  acquire(&supermem.lock);
  r->next = supermem.freelist;
  supermem.freelist = r;
  release(&supermem.lock);
}

// Allocate one aligned 2-MiB physical region.
void *
superalloc(void)
{
  struct superrun *r;

  acquire(&supermem.lock);
  r = supermem.freelist;
  if(r)
    supermem.freelist = r->next;
  release(&supermem.lock);

  if(r)
    memset((char *)r, 5, SUPERPGSIZE);
  return (void *)r;
}
