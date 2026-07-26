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
} kmem[NCPU];

#define STEAL_BATCH 256

void
kinit()
{
  for(int i = 0; i < NCPU; i++)
    initlock(&kmem[i].lock, "kmem");

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

  push_off();
  int id = cpuid();

  acquire(&kmem[id].lock);
  r->next = kmem[id].freelist;
  kmem[id].freelist = r;
  release(&kmem[id].lock);

  pop_off();
}

// Try to obtain one page for CPU id by taking a small batch from
// another CPU. No two kmem locks are held at the same time.
static struct run *
steal(int id)
{
  for(int step = 1; step < NCPU; step++){
    int donor = (id + step) % NCPU;
    struct run *batch;
    struct run *tail;
    struct run *r;
    struct run *rest;

    /*
     * 此时不持有本地 kmem[id].lock。
     * 每次只获取一把 donor 锁，避免锁顺序死锁。
     */
    acquire(&kmem[donor].lock);

    batch = kmem[donor].freelist;
    if(batch == 0){
      release(&kmem[donor].lock);
      continue;
    }

    /*
     * 从 donor 链表头部拆下最多 STEAL_BATCH 页。
     */
    tail = batch;
    int n = 1;

    while(n < STEAL_BATCH && tail->next != 0){
      tail = tail->next;
      n++;
    }

    kmem[donor].freelist = tail->next;
    tail->next = 0;

    release(&kmem[donor].lock);

    /*
     * 第一页直接返回给本次 kalloc。
     * 剩余页面放入当前 CPU 的本地 freelist。
     */
    r = batch;
    rest = r->next;
    r->next = 0;

    if(rest != 0){
      acquire(&kmem[id].lock);

      /*
       * tail 是当前批次最后一个节点。
       * 将剩余批次接到本地链表前面。
       */
      tail->next = kmem[id].freelist;
      kmem[id].freelist = rest;

      release(&kmem[id].lock);
    }

    return r;
  }

  return 0;
}

// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
void *
kalloc(void)
{
  struct run *r;
  int id;

  push_off();
  id = cpuid();

  acquire(&kmem[id].lock);
  r = kmem[id].freelist;

  if(r != 0)
    kmem[id].freelist = r->next;
  release(&kmem[id].lock);

 if(r == 0)
    r = steal(id);

  pop_off();

  if(r != 0)
    memset((char*)r, 5, PGSIZE);

  return (void*)r;
}
