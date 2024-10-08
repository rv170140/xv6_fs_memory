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


struct {
  struct spinlock lock;
  struct buf buf[NBUF];

  // Linked list of all buffers, through prev/next.
  // Sorted by how recently the buffer was used.
  // head.next is most recent, head.prev is least.
  struct buf head;
} bcache;
//mod_start
extern struct superblock sb;
uchar init_flag = 0;
struct buf *user_fs;
struct  buf *get_mod_buf(int dev,uint blockno);
void relse_mod_buf(struct buf *buf);
uint8 is_user_prog_block(uint dev, uint blockno);

//mod_finish
void
binit(void)
{
  struct buf *b;

  initlock(&bcache.lock, "bcache");

  // Create linked list of buffers
  bcache.head.prev = &bcache.head;
  bcache.head.next = &bcache.head;
  for(b = bcache.buf; b < bcache.buf+NBUF; b++){
    b->next = bcache.head.next;
    b->prev = &bcache.head;
    initsleeplock(&b->lock, "buffer");
    bcache.head.next->prev = b;
    bcache.head.next = b;
    b->user_flag = 0;
  }


}

// Look through buffer cache for block on device dev.
// If not found, allocate a buffer.
// In either case, return locked buffer.
static struct buf*
bget(uint dev, uint blockno)
{
  struct buf *b;
  if(init_flag && blockno>=sb.dstart && blockno< sb.dfinish&&is_user_prog_block(dev,blockno))
{

    b =  get_mod_buf(dev, blockno);

    return b;
}

  acquire(&bcache.lock);

  // Is the block already cached?
  for(b = bcache.head.next; b != &bcache.head; b = b->next){
    if(b->dev == dev && b->blockno == blockno){
      b->refcnt++;
      release(&bcache.lock);
      acquiresleep(&b->lock);
      return b;
    }
  }

  // Not cached.
  // Recycle the least recently used (LRU) unused buffer.
  for(b = bcache.head.prev; b != &bcache.head; b = b->prev){
    if(b->refcnt == 0) {
      b->dev = dev;
      b->blockno = blockno;
      b->valid = 0;
      b->refcnt = 1;
      release(&bcache.lock);
      acquiresleep(&b->lock);
      return b;
    }
  }
  panic("bget: no buffers");
}

// Return a locked buf with the contents of the indicated block.
struct buf*
bread(uint dev, uint blockno)
{
  struct buf *b;

  b = bget(dev, blockno);
  if(!b->valid) {
    virtio_disk_rw(VIRTIO0_ID, b, 0, 0);
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
  virtio_disk_rw(VIRTIO0_ID, b, 1, 0);
}

// Release a locked buffer.
// Move to the head of the most-recently-used list.
void
brelse(struct buf *b)
{
  if(!holdingsleep(&b->lock))
    panic("brelse");

  releasesleep(&b->lock);
  if(b->user_flag)return;
  acquire(&bcache.lock);
  b->refcnt--;
  if (b->refcnt == 0) {
    // no one is waiting for it.
    b->next->prev = b->prev;
    b->prev->next = b->next;
    b->next = bcache.head.next;
    b->prev = &bcache.head;
    bcache.head.next->prev = b;
    bcache.head.next = b;
  }
  
  release(&bcache.lock);
}

void
bpin(struct buf *b) {
  if(b->user_flag)return;
  acquire(&bcache.lock);
  b->refcnt++;
  release(&bcache.lock);
}

void
bunpin(struct buf *b) {
  if(b->user_flag)return;
  acquire(&bcache.lock);
  b->refcnt--;
  release(&bcache.lock);
}

struct  buf *get_indexed_mod(uint index)
{
if(index >= sb.userbcount) panic("los index bloka u mod fs-u");

  uint page_num = (index) / (PGSIZE / sizeof(struct buf));
  uint page_pos = (index) % (PGSIZE / sizeof(struct buf));


  return (struct buf *)((char*)user_fs + PGSIZE * page_num + page_pos*sizeof(struct buf));


}



struct buf* get_mod_buf(int dev, uint blockno)
{
    struct buf *b;
    for(uint i = 0; i < sb.userbcount; i++)
      {
        b = get_indexed_mod(i);
        if(b->blockno == blockno)
          {
          acquiresleep(&b->lock);
          return b;
          }
    }
      panic("get_mod_buf: no buffers"); // u funkciju se ulazi samo nakon provere
}



uint8 is_user_prog_block(uint dev, uint blockno)
{

struct buf *u_bitmap;
uint8 temp = 0;
  u_bitmap = bread(dev, UBBLOCK(blockno, sb));
uint bi = blockno%(8*BSIZE);

  if( u_bitmap->data[bi/8] & (1<<(bi%8)))
    temp = 1;

brelse(u_bitmap);
return temp;

}

void relse_mod_buf(struct buf *buf)
{
  releasesleep(&buf->lock);
}

void init_mod_fs(int dev)
{


  uint n_blocks_to_cache = sb.userbcount;
  uint buf_per_page = PGSIZE / sizeof(struct buf);
  uint pages_to_allocate = n_blocks_to_cache/buf_per_page + 1;



  for(uint i = 0; i < pages_to_allocate; i++)user_fs = (struct buf *)kalloc();




  struct buf * temp;

  int nbitmap = FSSIZE/(BSIZE*8) + 1;
  uint count = 0;

  for(uint i = 0 ; i < nbitmap &&count < sb.userbcount; i++)
    {
        struct buf *u_bitmap = bread(dev, sb.userbmapstart + i);
        for (uint j = 0; j < BPB && count < sb.userbcount; j++)
            if(u_bitmap->data[j/8] & (1<<(j%8)))
              {
                  struct buf * usr_block = get_indexed_mod(count);
                  temp = bread(dev, i*BPB + j);
                  for(int k = 0; k < BSIZE;k ++)
                    usr_block->data[k] = temp->data[k];
                  usr_block->blockno = temp->blockno;
                  usr_block->dev = dev;
                  usr_block->valid=1;
                  usr_block->user_flag= 1;
                  initsleeplock(&usr_block->lock,"user_block");
                  get_indexed_mod(count)->user_flag =1;
                  count++;
                  brelse(temp);

              }
        brelse(u_bitmap);
    }

if(count != sb.userbcount) panic("losa inicijalna slika fs-a");
init_flag = 1;

}

//  for(uint i = 0; i < sb.userbcount; i++)
//  {
//
//    temp = get_indexed_mod(i);
//
//
//    initsleeplock(&temp->lock,"alt_sleeplock");
//    temp->blockno = 0;
//    temp->dev = dev;
//
//
//  }







