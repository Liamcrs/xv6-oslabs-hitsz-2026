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

void
kinit()
{
  for(int i = 0; i < NCPU; i++){
    initlock(&kmem[i].lock, "kmem");
  }
  //initlock(&kmem.lock, "kmem");
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

// Free the page of physical memory pointed at by v,
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
  // 关闭中断，防止读取 CPU 编号后发生迁移
  push_off();
  int id = cpuid();

  // 获取当前 CPU 的内存分配器锁
  acquire(&kmem[id].lock);

  // 将释放的物理页插入当前 CPU 的空闲链表
  r->next = kmem[id].freelist;
  kmem[id].freelist = r;

  // 释放锁
  release(&kmem[id].lock);

  // 恢复中断状态
  pop_off();
}

// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.

void *
kalloc(void)
{
  struct run *r;

  // 关闭中断，安全获取当前 CPU 编号
  push_off();
  int id = cpuid();

  // 获取当前 CPU 的锁
  acquire(&kmem[id].lock);

  // 从当前 CPU 的空闲链表中取出一页
  r = kmem[id].freelist;
  if(r)
    kmem[id].freelist = r->next;

  // 释放当前 CPU 的锁
  release(&kmem[id].lock);

  //本地没有空闲页，尝试从其他 CPU 借页
  if(r == 0){
    for(int i = 0; i < NCPU; i++){
      if(i == id)
        continue;

      acquire(&kmem[i].lock);

      r = kmem[i].freelist;
      if(r)
        kmem[i].freelist = r->next;

      release(&kmem[i].lock);

      if(r)
        break;
    }
  }

  pop_off();

  // 初始化分配出的物理页
  if(r)
    memset((char*)r, 5, PGSIZE);

  return (void*)r;
}
