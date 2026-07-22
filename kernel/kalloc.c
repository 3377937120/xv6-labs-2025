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

#define NPHYPAGES ((PHYSTOP - KERNBASE) / PGSIZE)

struct {
  struct spinlock lock;
  int count[NPHYPAGES];
} krefs;

static int
paindex(uint64 pa)
{
  if(pa < KERNBASE || pa >= PHYSTOP || (pa % PGSIZE) != 0)
    panic("paindex");
  return (pa - KERNBASE) / PGSIZE;
}

static void
krefset(void *pa, int value)
{
  int index = paindex((uint64)pa);

  acquire(&krefs.lock);
  krefs.count[index] = value;
  release(&krefs.lock);
}

void
krefinc(void *pa)
{
  int index = paindex((uint64)pa);

  acquire(&krefs.lock);
  if(krefs.count[index] < 1)
    panic("krefinc");
  krefs.count[index]++;
  release(&krefs.lock);
}

int
krefcnt(void *pa)
{
  int index = paindex((uint64)pa);
  int value;

  acquire(&krefs.lock);
  value = krefs.count[index];
  release(&krefs.lock);

  return value;
}

void
kinit()
{
  initlock(&kmem.lock, "kmem");
  initlock(&krefs.lock, "krefs");
  freerange(end, (void*)PHYSTOP);
}

void
freerange(void *pa_start, void *pa_end)
{
  char *p;
  p = (char*)PGROUNDUP((uint64)pa_start);
  for(; p + PGSIZE <= (char*)pa_end; p += PGSIZE){
    krefset(p, 1);
    kfree(p);
  }
}

// Free the page of physical memory pointed at by pa,
// which normally should have been returned by a
// call to kalloc().  (The exception is when
// initializing the allocator; see kinit above.)
void
kfree(void *pa)
{
  struct run *r;
  int index;
  int release_page = 0;

  if(((uint64)pa % PGSIZE) != 0 || (char*)pa < end || (uint64)pa >= PHYSTOP)
    panic("kfree");

  index = paindex((uint64)pa);

  acquire(&krefs.lock);
  if(krefs.count[index] < 1)
    panic("kfree ref");
  krefs.count[index]--;
  if(krefs.count[index] == 0)
    release_page = 1;
  release(&krefs.lock);

  if(release_page == 0)
    return;

  // Fill with junk only when no valid reference remains.
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

  if(r){
    // A page removed from the freelist must have no references.
    int index = paindex((uint64)r);

    acquire(&krefs.lock);
    if(krefs.count[index] != 0)
      panic("kalloc ref");
    krefs.count[index] = 1;
    release(&krefs.lock);

    memset((char*)r, 5, PGSIZE); // fill with junk
  }

  return (void*)r;
}

