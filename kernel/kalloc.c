// Physical memory allocator, for user processes,
// kernel stacks, page-table pages, and pipe buffers.
// Allocates whole 4096-byte pages.

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "defs.h"

void freerange(void *pa_start, void *pa_end);

extern char end[];  // 内核结束后的第一个地址，由 kernel.ld 定义

// 空闲页本身被当作链表节点使用。
struct run {
  struct run *next;
};

// 每个 CPU 都拥有一把独立的锁和一条独立的空闲页链表。
struct {
  struct spinlock lock;
  struct run *freelist;
} kmem[NCPU];

// 初始化每 CPU 内存分配器。
void
kinit(void)
{
  // 所有锁名必须以 "kmem" 开头，kalloctest 才会统计它们。
  // 使用字符串常量可避免局部字符数组生命周期结束的问题。
  for(int i = 0; i < NCPU; i++)
    initlock(&kmem[i].lock, "kmem");

  // 启动阶段只有一个 CPU 执行这里。
  // freerange() 会通过 kfree() 把所有空闲页放到该 CPU 的链表。
  freerange(end, (void *)PHYSTOP);
}

// 将 [pa_start, pa_end) 中的每个完整页面交给 kfree()。
void
freerange(void *pa_start, void *pa_end)
{
  char *p;

  p = (char *)PGROUNDUP((uint64)pa_start);
  for(; p + PGSIZE <= (char *)pa_end; p += PGSIZE)
    kfree(p);
}

// 释放 pa 指向的物理页。
void
kfree(void *pa)
{
  struct run *r;
  int id;

  if(((uint64)pa % PGSIZE) != 0 ||
     (char *)pa < end ||
     (uint64)pa >= PHYSTOP)
    panic("kfree");

  // 填充整页是较慢操作，必须放在 kmem 锁外。
  memset(pa, 1, PGSIZE);
  r = (struct run *)pa;

  // cpuid() 及其返回值只可在关闭中断期间安全使用。
  push_off();
  id = cpuid();

  // 常见释放路径只访问当前 CPU 的本地链表。
  // 临界区只有两个指针操作，持锁时间非常短。
  acquire(&kmem[id].lock);
  r->next = kmem[id].freelist;
  kmem[id].freelist = r;
  release(&kmem[id].lock);

  pop_off();
}

// 当前 CPU 的链表为空时，从其他 CPU 偷取一页。
// 重点不是减少 acquire() 次数，而是让每次远程持锁时间保持 O(1)。
static struct run *
steal_one_page(int id)
{
  for(int step = 1; step < NCPU; step++){
    int victim = (id + step) % NCPU;
    struct run *r;

    // 此时不持有 kmem[id].lock，因此不会形成双锁环路。
    acquire(&kmem[victim].lock);

    r = kmem[victim].freelist;
    if(r != 0)
      kmem[victim].freelist = r->next;

    release(&kmem[victim].lock);

    if(r != 0){
      r->next = 0;
      return r;
    }
  }

  // 所有 CPU 的链表都为空。
  return 0;
}

// 分配一个 4096 字节物理页。
void *
kalloc(void)
{
  struct run *r;
  int id;

  push_off();
  id = cpuid();

  // 快速路径：只从当前 CPU 的 freelist 取一页。
  acquire(&kmem[id].lock);
  r = kmem[id].freelist;
  if(r != 0)
    kmem[id].freelist = r->next;
  release(&kmem[id].lock);

  // 仅在本地链表为空时访问其他 CPU。
  if(r == 0)
    r = steal_one_page(id);

  pop_off();

  // 页面填充同样必须放在所有 kmem 锁之外。
  if(r != 0)
    memset((char *)r, 5, PGSIZE);

  return (void *)r;
}
