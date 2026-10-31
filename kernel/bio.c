// Buffer cache.
//
// The buffer cache is a linked list of buf structures holding
// cached copies of disk block contents.  Caching disk blocks
// in memory reduces the number of disk reads and also provides
// a synchronization point for disk blocks used by multiple processes.
//
// Interface:
// * To get a buffer for a particular disk block, call bread.
// * After changing buffer data, call bwrite to write it to disk.
// * When done with the buffer, call brelse.
// * Do not use the buffer after calling brelse.
// * Only one process at a time can use a buffer,
//     so do not keep them longer than necessary.


#include "types.h"
#include "param.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "riscv.h"
#include "defs.h"
#include "fs.h"
#include "buf.h"

#define NBUCKETS 13

struct {
  // 用于协调缓存未命中时的跨桶回收
  struct spinlock eviction_lock;

  // 实际缓存缓冲区，数量保持不变
  struct buf buf[NBUF];

  // 每个哈希桶都有独立的锁与双向链表
  struct {
    struct spinlock lock;
    struct buf head;
  } bucket[NBUCKETS];

} bcache;

void
binit(void)
{
  struct buf *b;

  // 初始化跨桶回收锁
  initlock(&bcache.eviction_lock, "bcache.evict");

  // 初始化所有哈希桶
  for(int i = 0; i < NBUCKETS; i++){
    initlock(&bcache.bucket[i].lock, "bcache.bucket");

    struct buf *head = &bcache.bucket[i].head;
    head->prev = head;
    head->next = head;
  }

  // 初始化所有缓存缓冲区
  for(int i = 0; i < NBUF; i++){
    b = &bcache.buf[i];

    initsleeplock(&b->lock, "buffer");
    b->refcnt = 0;
    b->valid = 0;

    // 标记尚未分配给具体磁盘块
    b->dev = (uint)-1;
    b->blockno = 0;

    // 初始化时将未使用的缓冲区分散放入各桶
    struct buf *head = &bcache.bucket[i % NBUCKETS].head;

    b->next = head->next;
    b->prev = head;
    head->next->prev = b;
    head->next = b;
  }
}

static int
bhash(uint blockno)
{
  return blockno % NBUCKETS;
}

// Look through buffer cache for block on device dev.
// If not found, allocate a buffer.
// In either case, return locked buffer.

static struct buf*
bget(uint dev, uint blockno)
{
  struct buf *b;
  struct buf *head;
  int id = bhash(blockno);

  // 1. 优先检查目标哈希桶
  acquire(&bcache.bucket[id].lock);

  head = &bcache.bucket[id].head;

  for(b = head->next; b != head; b = b->next){
    if(b->dev == dev && b->blockno == blockno){
      b->refcnt++;
      release(&bcache.bucket[id].lock);

      acquiresleep(&b->lock);
      return b;
    }
  }

  release(&bcache.bucket[id].lock);

  // 2. 缓存未命中：串行化缓存分配
  acquire(&bcache.eviction_lock);

  // 3. 必须再次检查目标桶
  acquire(&bcache.bucket[id].lock);

  head = &bcache.bucket[id].head;

  for(b = head->next; b != head; b = b->next){
    if(b->dev == dev && b->blockno == blockno){
      b->refcnt++;
      release(&bcache.bucket[id].lock);
      release(&bcache.eviction_lock);

      acquiresleep(&b->lock);
      return b;
    }
  }

  release(&bcache.bucket[id].lock);

  // 4. 遍历所有桶，寻找可回收的 buf
  for(int i = 0; i < NBUCKETS; i++){

    acquire(&bcache.bucket[i].lock);

    struct buf *h = &bcache.bucket[i].head;

    for(b = h->prev; b != h; b = b->prev){

      if(b->refcnt == 0){

        // 从原来的桶中移除
        b->prev->next = b->next;
        b->next->prev = b->prev;

        release(&bcache.bucket[i].lock);

        // 5. 将 buf 插入目标桶
        acquire(&bcache.bucket[id].lock);

        head = &bcache.bucket[id].head;

        b->dev = dev;
        b->blockno = blockno;
        b->valid = 0;
        b->refcnt = 1;

        b->next = head->next;
        b->prev = head;
        head->next->prev = b;
        head->next = b;

        release(&bcache.bucket[id].lock);
        release(&bcache.eviction_lock);

        acquiresleep(&b->lock);
        return b;
      }
    }

    release(&bcache.bucket[i].lock);
  }

  release(&bcache.eviction_lock);
  panic("bget: no buffers");
}

// Return a locked buf with the contents of the indicated block.
struct buf*
bread(uint dev, uint blockno)
{
  struct buf *b;

  b = bget(dev, blockno);
  if(!b->valid) {
    virtio_disk_rw(b, 0);
    b->valid = 1;
  }
  return b;
}

// Write b's contents to disk.  Must be locked.
void
bwrite(struct buf *b)
{
  if(!holdingsleep(&b->lock))
    panic("bwrite");
  virtio_disk_rw(b, 1);
}

// Release a locked buffer.
// Move to the head of the most-recently-used list.

void
brelse(struct buf *b)
{
  if(!holdingsleep(&b->lock))
    panic("brelse");

  releasesleep(&b->lock);

  // 计算当前磁盘块所属的哈希桶
  int id = bhash(b->blockno);

  acquire(&bcache.bucket[id].lock);

  // 减少引用计数
  b->refcnt--;

  if(b->refcnt == 0){
    // 当前 buf 已经没有使用者
    // 从原来的链表位置移除
    b->next->prev = b->prev;
    b->prev->next = b->next;

    // 插入当前桶的链表头部
    struct buf *head = &bcache.bucket[id].head;

    b->next = head->next;
    b->prev = head;
    head->next->prev = b;
    head->next = b;
  }

  release(&bcache.bucket[id].lock);
}


void
bpin(struct buf *b)
{
  int id = bhash(b->blockno);

  acquire(&bcache.bucket[id].lock);
  b->refcnt++;
  release(&bcache.bucket[id].lock);
}

void
bunpin(struct buf *b)
{
  int id = bhash(b->blockno);

  acquire(&bcache.bucket[id].lock);
  b->refcnt--;
  release(&bcache.bucket[id].lock);
}


