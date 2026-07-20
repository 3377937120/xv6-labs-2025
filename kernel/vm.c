#include "param.h"
#include "types.h"
#include "memlayout.h"
#include "elf.h"
#include "riscv.h"
#include "defs.h"
#include "spinlock.h"
#include "proc.h"
#include "fs.h"

/*
 * The kernel's page table.
 */
pagetable_t kernel_pagetable;

extern char etext[];      // kernel.ld sets this to end of kernel code.
extern char trampoline[]; // trampoline.S

static int
pte_is_leaf(pte_t pte)
{
  return (pte & (PTE_R | PTE_W | PTE_X)) != 0;
}

/*
 * Return the address of the PTE that maps va.
 *
 * For an ordinary page, the returned PTE is at level 0.
 * For a 2-MiB superpage, the returned PTE is the level-1 leaf itself.
 * If alloc is non-zero, missing intermediate page-table pages are created.
 * level_out may be null; otherwise it receives the returned PTE's level.
 */
pte_t *
walk(pagetable_t pagetable, uint64 va, int alloc, int *level_out)
{
  if(va >= MAXVA)
    panic("walk");

  for(int level = 2; level > 0; level--){
    pte_t *pte = &pagetable[PX(level, va)];

    if(*pte & PTE_V){
      if(pte_is_leaf(*pte)){
        if(level_out)
          *level_out = level;
        return pte;
      }
      pagetable = (pagetable_t)PTE2PA(*pte);
    } else {
      if(!alloc)
        return 0;
      pagetable = (pagetable_t)kalloc();
      if(pagetable == 0)
        return 0;
      memset(pagetable, 0, PGSIZE);
      *pte = PA2PTE(pagetable) | PTE_V;
    }
  }

  if(level_out)
    *level_out = 0;
  return &pagetable[PX(0, va)];
}

/*
 * Return the level-1 PTE for va, allocating the level-1 page-table page
 * when alloc is non-zero. The returned PTE may be invalid, a non-leaf, or
 * a 2-MiB leaf; the caller decides which states are acceptable.
 */
static pte_t *
superwalk(pagetable_t pagetable, uint64 va, int alloc)
{
  if(va >= MAXVA)
    panic("superwalk");

  pte_t *l2pte = &pagetable[PX(2, va)];
  if(*l2pte & PTE_V){
    if(pte_is_leaf(*l2pte))
      return 0; // This implementation does not create 1-GiB leaves.
    pagetable = (pagetable_t)PTE2PA(*l2pte);
  } else {
    if(!alloc)
      return 0;
    pagetable = (pagetable_t)kalloc();
    if(pagetable == 0)
      return 0;
    memset(pagetable, 0, PGSIZE);
    *l2pte = PA2PTE(pagetable) | PTE_V;
  }

  return &pagetable[PX(1, va)];
}

// Make a direct-map page table for the kernel.
pagetable_t
kvmmake(void)
{
  pagetable_t kpgtbl;

  kpgtbl = (pagetable_t)kalloc();
  if(kpgtbl == 0)
    panic("kvmmake");
  memset(kpgtbl, 0, PGSIZE);

  // uart registers
  kvmmap(kpgtbl, UART0, UART0, PGSIZE, PTE_R | PTE_W);

  // virtio mmio disk interface
  kvmmap(kpgtbl, VIRTIO0, VIRTIO0, PGSIZE, PTE_R | PTE_W);

#ifdef LAB_NET
  // PCI-E ECAM (configuration space), for pci.c.
  kvmmap(kpgtbl, 0x30000000L, 0x30000000L,
         0x10000000, PTE_R | PTE_W);
  // pci.c maps the e1000's registers here.
  kvmmap(kpgtbl, 0x40000000L, 0x40000000L,
         0x20000, PTE_R | PTE_W);
#endif

  // PLIC
  kvmmap(kpgtbl, PLIC, PLIC, 0x4000000, PTE_R | PTE_W);

  // map kernel text executable and read-only.
  kvmmap(kpgtbl, KERNBASE, KERNBASE,
         (uint64)etext - KERNBASE, PTE_R | PTE_X);

  // map kernel data and the physical RAM we'll make use of.
  kvmmap(kpgtbl, (uint64)etext, (uint64)etext,
         PHYSTOP - (uint64)etext, PTE_R | PTE_W);

  // map the trampoline for trap entry/exit to the highest virtual address.
  kvmmap(kpgtbl, TRAMPOLINE, (uint64)trampoline,
         PGSIZE, PTE_R | PTE_X);

  // allocate and map a kernel stack for each process.
  proc_mapstacks(kpgtbl);

  return kpgtbl;
}

// Add a mapping to the kernel page table. Only used when booting.
void
kvmmap(pagetable_t kpgtbl, uint64 va, uint64 pa, uint64 sz, int perm)
{
  if(mappages(kpgtbl, va, sz, pa, perm) != 0)
    panic("kvmmap");
}

// Initialize the kernel_pagetable, shared by all CPUs.
void
kvminit(void)
{
  kernel_pagetable = kvmmake();
}

// Switch the current CPU's h/w page table register to the kernel page table.
void
kvminithart(void)
{
  sfence_vma();
  w_satp(MAKE_SATP(kernel_pagetable));
  sfence_vma();
}

/*
 * Look up a user virtual address and return its exact physical address.
 * For a superpage this includes the offset within the 2-MiB region.
 */
uint64
walkaddr(pagetable_t pagetable, uint64 va)
{
  int level;
  pte_t *pte;
  uint64 pa;

  if(va >= MAXVA)
    return 0;

  pte = walk(pagetable, va, 0, &level);
  if(pte == 0 || (*pte & PTE_V) == 0 || (*pte & PTE_U) == 0)
    return 0;
  if(!pte_is_leaf(*pte))
    return 0;

  pa = PTE2PA(*pte);
  uint64 mappingsz = 1L << PXSHIFT(level);
  return pa + (va & (mappingsz - 1));
}

static void
vmprintwalk(pagetable_t pagetable, int level, uint64 va_prefix)
{
  for(int i = 0; i < 512; i++){
    pte_t pte = pagetable[i];
    if((pte & PTE_V) == 0)
      continue;

    uint64 va = va_prefix | ((uint64)i << PXSHIFT(level));
    uint64 pa = PTE2PA(pte);

    for(int depth = 0; depth < 3 - level; depth++)
      printf(" ..");
    printf("%p: pte %p pa %p\n",
           (void *)va, (void *)pte, (void *)pa);

    if(level > 0 && !pte_is_leaf(pte))
      vmprintwalk((pagetable_t)pa, level - 1, va);
  }
}

void
vmprint(pagetable_t pagetable)
{
  printf("page table %p\n", (void *)pagetable);
  vmprintwalk(pagetable, 2, 0);
}

// Create ordinary 4-KiB PTE mappings.
int
mappages(pagetable_t pagetable, uint64 va, uint64 size,
         uint64 pa, int perm)
{
  uint64 a, last;
  pte_t *pte;
  int level;

  if((va % PGSIZE) != 0)
    panic("mappages: va not aligned");
  if((size % PGSIZE) != 0)
    panic("mappages: size not aligned");
  if(size == 0)
    panic("mappages: size");

  a = va;
  last = va + size - PGSIZE;
  for(;;){
    pte = walk(pagetable, a, 1, &level);
    if(pte == 0)
      return -1;
    if(level != 0 || (*pte & PTE_V))
      panic("mappages: remap");
    *pte = PA2PTE(pa) | perm | PTE_V;
    if(a == last)
      break;
    a += PGSIZE;
    pa += PGSIZE;
  }
  return 0;
}

// Map one 2-MiB physical region with a level-1 leaf PTE.
static int
supermappage(pagetable_t pagetable, uint64 va, uint64 pa, int perm)
{
  if((va % SUPERPGSIZE) != 0 || (pa % SUPERPGSIZE) != 0)
    return -1;

  pte_t *pte = superwalk(pagetable, va, 1);
  if(pte == 0 || (*pte & PTE_V))
    return -1;

  *pte = PA2PTE(pa) | perm | PTE_V;
  return 0;
}

// Create an empty user page table.
pagetable_t
uvmcreate(void)
{
  pagetable_t pagetable;

  pagetable = (pagetable_t)kalloc();
  if(pagetable == 0)
    return 0;
  memset(pagetable, 0, PGSIZE);
  return pagetable;
}

static int
pagetable_empty(pagetable_t pagetable)
{
  for(int i = 0; i < 512; i++)
    if(pagetable[i] & PTE_V)
      return 0;
  return 1;
}

/*
 * If va's level-1 entry points to an empty level-0 table, reclaim that
 * page-table page and clear the level-1 entry. This permits a later sbrk()
 * to place a superpage in the same 2-MiB virtual-address slot.
 */
static void
prune_empty_level0(pagetable_t pagetable, uint64 va)
{
  pte_t *l1pte = superwalk(pagetable, va, 0);
  if(l1pte == 0 || (*l1pte & PTE_V) == 0 || pte_is_leaf(*l1pte))
    return;

  pagetable_t l0 = (pagetable_t)PTE2PA(*l1pte);
  if(!pagetable_empty(l0))
    return;

  kfree((void *)l0);
  *l1pte = 0;
}

/*
 * Demote a 2-MiB level-1 leaf into 512 independent 4-KiB pages.
 *
 * The old superpage belongs to the superpage allocator, while ordinary
 * pages belong to kalloc(). To avoid mixed ownership, copy the data into
 * 512 newly allocated ordinary pages, install a new level-0 page table,
 * then return the old 2-MiB region to superfree().
 */
static int
demotesuperpage(pte_t *superpte)
{
  if(superpte == 0 || (*superpte & PTE_V) == 0 || !pte_is_leaf(*superpte))
    return -1;

  uint64 oldpa = PTE2PA(*superpte);
  uint flags = PTE_FLAGS(*superpte);
  if((oldpa % SUPERPGSIZE) != 0)
    return -1;

  pagetable_t l0 = (pagetable_t)kalloc();
  if(l0 == 0)
    return -1;
  memset(l0, 0, PGSIZE);

  int made = 0;
  for(int i = 0; i < 512; i++){
    char *mem = kalloc();
    if(mem == 0)
      goto fail;

    memmove(mem, (void *)(oldpa + (uint64)i * PGSIZE), PGSIZE);
    l0[i] = PA2PTE(mem) | flags;
    made++;
  }

  *superpte = PA2PTE(l0) | PTE_V;
  sfence_vma();
  superfree((void *)oldpa);
  return 0;

fail:
  for(int i = 0; i < made; i++)
    kfree((void *)PTE2PA(l0[i]));
  kfree((void *)l0);
  return -1;
}

// Remove npages of mappings starting from va. Missing mappings are allowed.
void
uvmunmap(pagetable_t pagetable, uint64 va, uint64 npages, int do_free)
{
  if((va % PGSIZE) != 0)
    panic("uvmunmap: not aligned");

  uint64 end = va + npages * PGSIZE;
  uint64 a = va;

  while(a < end){
    int level;
    pte_t *pte = walk(pagetable, a, 0, &level);

    if(pte == 0 || (*pte & PTE_V) == 0){
      a += PGSIZE;
      continue;
    }
    if(!pte_is_leaf(*pte))
      panic("uvmunmap: not a leaf");

    if(level == 1){
      uint64 superbase = SUPERPGROUNDDOWN(a);
      uint64 superend = superbase + SUPERPGSIZE;

      if(a == superbase && end >= superend){
        uint64 pa = PTE2PA(*pte);
        *pte = 0;
        sfence_vma();
        if(do_free)
          superfree((void *)pa);
        a += SUPERPGSIZE;
        continue;
      }

      // A tail of this superpage is being removed. Demote, then retry a.
      if(demotesuperpage(pte) < 0)
        panic("uvmunmap: demote");
      continue;
    }

    if(level != 0)
      panic("uvmunmap: unsupported leaf");

    if(do_free)
      kfree((void *)PTE2PA(*pte));
    *pte = 0;
    prune_empty_level0(pagetable, a);
    a += PGSIZE;
  }

  sfence_vma();
}

// Allocate PTEs and physical memory to grow a process from oldsz to newsz.
uint64
uvmalloc(pagetable_t pagetable, uint64 oldsz, uint64 newsz, int xperm)
{
  char *mem;
  uint64 a;

  if(newsz < oldsz)
    return oldsz;

  oldsz = PGROUNDUP(oldsz);
  a = oldsz;

  while(a < newsz){
    if((a % SUPERPGSIZE) == 0 && newsz - a >= SUPERPGSIZE){
      /*
       * An existing lower-level table means this virtual slot has ordinary
       * mappings. In that case keep using ordinary pages instead of
       * overwriting the table with a level-1 leaf.
       */
      pte_t *existing = superwalk(pagetable, a, 0);
      if(existing == 0 || (*existing & PTE_V) == 0){
        mem = superalloc();
        if(mem == 0){
          uvmdealloc(pagetable, a, oldsz);
          return 0;
        }
        memset(mem, 0, SUPERPGSIZE);
        if(supermappage(pagetable, a, (uint64)mem,
                        PTE_R | PTE_U | xperm) != 0){
          superfree(mem);
          uvmdealloc(pagetable, a, oldsz);
          return 0;
        }
        a += SUPERPGSIZE;
        continue;
      }
    }

    mem = kalloc();
    if(mem == 0){
      uvmdealloc(pagetable, a, oldsz);
      return 0;
    }
#ifndef LAB_SYSCALL
    memset(mem, 0, PGSIZE);
#endif
    if(mappages(pagetable, a, PGSIZE, (uint64)mem,
                PTE_R | PTE_U | xperm) != 0){
      kfree(mem);
      uvmdealloc(pagetable, a, oldsz);
      return 0;
    }
    a += PGSIZE;
  }

  return newsz;
}

// Deallocate user pages to bring the process size from oldsz down to newsz.
uint64
uvmdealloc(pagetable_t pagetable, uint64 oldsz, uint64 newsz)
{
  if(newsz >= oldsz)
    return oldsz;

  if(PGROUNDUP(newsz) < PGROUNDUP(oldsz)){
    uint64 npages =
      (PGROUNDUP(oldsz) - PGROUNDUP(newsz)) / PGSIZE;
    uvmunmap(pagetable, PGROUNDUP(newsz), npages, 1);
  }

  return newsz;
}

// Recursively free page-table pages. All leaf mappings must be gone.
void
freewalk(pagetable_t pagetable)
{
  for(int i = 0; i < 512; i++){
    pte_t pte = pagetable[i];
    if((pte & PTE_V) && !pte_is_leaf(pte)){
      uint64 child = PTE2PA(pte);
      freewalk((pagetable_t)child);
      pagetable[i] = 0;
    } else if(pte & PTE_V){
      panic("freewalk: leaf");
    }
  }
  kfree((void *)pagetable);
}

// Free user memory pages, then free page-table pages.
void
uvmfree(pagetable_t pagetable, uint64 sz)
{
  if(sz > 0)
    uvmunmap(pagetable, 0, PGROUNDUP(sz) / PGSIZE, 1);
  freewalk(pagetable);
}

// Copy a parent's user memory into a child, preserving superpages.
int
uvmcopy(pagetable_t old, pagetable_t new, uint64 sz)
{
  uint64 i = 0;

  while(i < sz){
    int level;
    pte_t *pte = walk(old, i, 0, &level);

    if(pte == 0 || (*pte & PTE_V) == 0){
      i += PGSIZE;
      continue;
    }
    if(!pte_is_leaf(*pte))
      panic("uvmcopy: not leaf");

    uint64 pa = PTE2PA(*pte);
    uint flags = PTE_FLAGS(*pte);
    char *mem;

    if(level == 1){
      if((i % SUPERPGSIZE) != 0)
        panic("uvmcopy: unaligned superpage");

      mem = superalloc();
      if(mem == 0)
        goto err;
      memmove(mem, (void *)pa, SUPERPGSIZE);
      if(supermappage(new, i, (uint64)mem,
                      flags & ~PTE_V) != 0){
        superfree(mem);
        goto err;
      }
      i += SUPERPGSIZE;
      continue;
    }

    if(level != 0)
      panic("uvmcopy: unsupported leaf");

    mem = kalloc();
    if(mem == 0)
      goto err;
    memmove(mem, (void *)pa, PGSIZE);
    if(mappages(new, i, PGSIZE, (uint64)mem,
                flags & ~PTE_V) != 0){
      kfree(mem);
      goto err;
    }
    i += PGSIZE;
  }

  return 0;

err:
  if(i > 0)
    uvmunmap(new, 0, i / PGSIZE, 1);
  return -1;
}

// Mark a PTE invalid for user access; used for the stack guard page.
void
uvmclear(pagetable_t pagetable, uint64 va)
{
  int level;
  pte_t *pte = walk(pagetable, va, 0, &level);
  if(pte == 0 || (*pte & PTE_V) == 0)
    panic("uvmclear");
  *pte &= ~PTE_U;
}

// Copy from kernel to user.
int
copyout(pagetable_t pagetable, uint64 dstva, char *src, uint64 len)
{
  uint64 n, va0, pa0;

  while(len > 0){
    va0 = PGROUNDDOWN(dstva);
    if(va0 >= MAXVA)
      return -1;

    pa0 = walkaddr(pagetable, va0);
    if(pa0 == 0){
      if((pa0 = vmfault(pagetable, va0, 0)) == 0)
        return -1;
    }

    int level;
    pte_t *pte = walk(pagetable, va0, 0, &level);
    if(pte == 0 || (*pte & PTE_V) == 0 || (*pte & PTE_W) == 0)
      return -1;

    n = PGSIZE - (dstva - va0);
    if(n > len)
      n = len;
    memmove((void *)(pa0 + (dstva - va0)), src, n);

    len -= n;
    src += n;
    dstva = va0 + PGSIZE;
  }
  return 0;
}

// Copy from user to kernel.
int
copyin(pagetable_t pagetable, char *dst, uint64 srcva, uint64 len)
{
  uint64 n, va0, pa0;

  while(len > 0){
    va0 = PGROUNDDOWN(srcva);
    pa0 = walkaddr(pagetable, va0);
    if(pa0 == 0){
      if((pa0 = vmfault(pagetable, va0, 0)) == 0)
        return -1;
    }
    n = PGSIZE - (srcva - va0);
    if(n > len)
      n = len;
    memmove(dst, (void *)(pa0 + (srcva - va0)), n);

    len -= n;
    dst += n;
    srcva = va0 + PGSIZE;
  }
  return 0;
}

// Copy a null-terminated string from user to kernel.
int
copyinstr(pagetable_t pagetable, char *dst, uint64 srcva, uint64 max)
{
  int got_null = 0;

  while(!got_null && max > 0){
    uint64 va0 = PGROUNDDOWN(srcva);
    uint64 pa0 = walkaddr(pagetable, va0);
    if(pa0 == 0)
      return -1;

    uint64 n = PGSIZE - (srcva - va0);
    if(n > max)
      n = max;

    char *p = (char *)(pa0 + (srcva - va0));
    while(n > 0){
      if(*p == '\0'){
        *dst = '\0';
        got_null = 1;
        break;
      }
      *dst++ = *p++;
      n--;
      max--;
    }
    srcva = va0 + PGSIZE;
  }

  return got_null ? 0 : -1;
}

// Allocate and map one ordinary page for a lazily allocated user address.
uint64
vmfault(pagetable_t pagetable, uint64 va, int read)
{
  uint64 mem;
  struct proc *p = myproc();
  (void)read;

  if(va >= p->sz)
    return 0;
  va = PGROUNDDOWN(va);
  if(ismapped(pagetable, va))
    return 0;

  mem = (uint64)kalloc();
  if(mem == 0)
    return 0;
  memset((void *)mem, 0, PGSIZE);
  if(mappages(pagetable, va, PGSIZE, mem,
              PTE_W | PTE_U | PTE_R) != 0){
    kfree((void *)mem);
    return 0;
  }
  return mem;
}

int
ismapped(pagetable_t pagetable, uint64 va)
{
  int level;
  pte_t *pte = walk(pagetable, va, 0, &level);
  return pte != 0 && (*pte & PTE_V) != 0;
}

#ifdef LAB_PGTBL
/*
 * Return the PTE that covers va. For every 4-KiB address inside one
 * superpage this returns the same level-1 PTE, which is exactly what the
 * pgtbltest supercheck() and superpg_free() tests require.
 */
pte_t *
pgpte(pagetable_t pagetable, uint64 va)
{
  int level;
  return walk(pagetable, va, 0, &level);
}
#endif
