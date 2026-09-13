#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <assert.h>
#include <sys/types.h>

#define stat xv6_stat  // avoid clash with host struct stat
#include "types.h"
#include "fs.h"
#include "stat.h"
#include "param.h"

#ifndef static_assert
#define static_assert(a, b) do { switch (0) case 0: case (a): ; } while (0)
#endif

#define NINODES 200

// Disk layout:
// [ boot block | sb block | log | inode blocks | free bit map | data blocks ]

int nbitmap = FSSIZE/(BSIZE*8) + 1;
int ninodeblocks = NINODES / IPB + 1;
int nlog = LOGSIZE;
int nmeta;    // Number of meta blocks (boot, sb, nlog, inode, bitmap)
int nblocks;  // Number of data blocks

int fsfd;
struct superblock sb;
char zeroes[BSIZE];
uint freeinode = 1;
uint freeblock;


void balloc(int);
void wsect(uint, void*);
void winode(uint, struct dinode*);
void rinode(uint inum, struct dinode *ip);
void rsect(uint sec, void *buf);
uint ialloc(ushort type);
void iappend(uint inum, void *p, int n);
uint appendblock(struct dinode *din, uint fbn);
uint indexblock(uint parent, uint index);

// convert to intel byte order
ushort
xshort(ushort x)
{
  ushort y;
  uchar *a = (uchar*)&y;
  a[0] = x;
  a[1] = x >> 8;
  return y;
}

uint
xint(uint x)
{
  uint y;
  uchar *a = (uchar*)&y;
  a[0] = x;
  a[1] = x >> 8;
  a[2] = x >> 16;
  a[3] = x >> 24;
  return y;
}

int
main(int argc, char *argv[])
{
  int i, cc, fd;
  uint rootino, inum, off;
  struct dirent de;
  char buf[BSIZE];
  struct dinode din;


  static_assert(sizeof(int) == 4, "Integers must be 4 bytes!");

  if(argc < 2){
    fprintf(stderr, "Usage: mkfs fs.img files...\n");
    exit(1);
  }

  assert((BSIZE % sizeof(struct dinode)) == 0);
  assert((BSIZE % sizeof(struct dirent)) == 0);

  fsfd = open(argv[1], O_RDWR|O_CREAT|O_TRUNC, 0666);
  if(fsfd < 0){
    perror(argv[1]);
    exit(1);
  }

  // 1 fs block = 1 disk sector
  nmeta = 2 + nlog + ninodeblocks + nbitmap;
  nblocks = FSSIZE - nmeta;

  sb.size = xint(FSSIZE);
  sb.nblocks = xint(nblocks);
  sb.ninodes = xint(NINODES);
  sb.nlog = xint(nlog);
  sb.logstart = xint(2);
  sb.inodestart = xint(2+nlog);
  sb.bmapstart = xint(2+nlog+ninodeblocks);

  printf("nmeta %d (boot, super, log blocks %u inode blocks %u, bitmap blocks %u) blocks %d total %d\n",
         nmeta, nlog, ninodeblocks, nbitmap, nblocks, FSSIZE);

  freeblock = nmeta;     // the first free block that we can allocate

  // A 2,500,000-block image is about 1.2 GiB.  Create it sparsely; newly
  // extended regions read as zero and are populated below as needed.
  if(ftruncate(fsfd, (off_t)FSSIZE * BSIZE) < 0){
    perror("ftruncate");
    exit(1);
  }

  memset(buf, 0, sizeof(buf));
  memmove(buf, &sb, sizeof(sb));
  wsect(1, buf);

  rootino = ialloc(T_DIR);
  assert(rootino == ROOTINO);

  bzero(&de, sizeof(de));
  de.inum = xshort(rootino);
  strcpy(de.name, ".");
  iappend(rootino, &de, sizeof(de));

  bzero(&de, sizeof(de));
  de.inum = xshort(rootino);
  strcpy(de.name, "..");
  iappend(rootino, &de, sizeof(de));

  for(i = 2; i < argc; i++){
    assert(index(argv[i], '/') == 0);

    if((fd = open(argv[i], 0)) < 0){
      perror(argv[i]);
      exit(1);
    }

    // Skip leading _ in name when writing to file system.
    // The binaries are named _rm, _cat, etc. to keep the
    // build operating system from trying to execute them
    // in place of system binaries like rm and cat.
    if(argv[i][0] == '_')
      ++argv[i];

    inum = ialloc(T_FILE);

    bzero(&de, sizeof(de));
    de.inum = xshort(inum);
    strncpy(de.name, argv[i], DIRSIZ);
    iappend(rootino, &de, sizeof(de));

    while((cc = read(fd, buf, sizeof(buf))) > 0)
      iappend(inum, buf, cc);

    close(fd);
  }

  // fix size of root inode dir
  rinode(rootino, &din);
  off = xint(din.size);
  off = ((off/BSIZE) + 1) * BSIZE;
  din.size = xint(off);
  winode(rootino, &din);

  balloc(freeblock);

  exit(0);
}

void
wsect(uint sec, void *buf)
{
  if(lseek(fsfd, sec * BSIZE, 0) != sec * BSIZE){
    perror("lseek");
    exit(1);
  }
  if(write(fsfd, buf, BSIZE) != BSIZE){
    perror("write");
    exit(1);
  }
}

void
winode(uint inum, struct dinode *ip)
{
  char buf[BSIZE];
  uint bn;
  struct dinode *dip;

  bn = IBLOCK(inum, sb);
  rsect(bn, buf);
  dip = ((struct dinode*)buf) + (inum % IPB);
  *dip = *ip;
  wsect(bn, buf);
}

void
rinode(uint inum, struct dinode *ip)
{
  char buf[BSIZE];
  uint bn;
  struct dinode *dip;

  bn = IBLOCK(inum, sb);
  rsect(bn, buf);
  dip = ((struct dinode*)buf) + (inum % IPB);
  *ip = *dip;
}

void
rsect(uint sec, void *buf)
{
  if(lseek(fsfd, sec * BSIZE, 0) != sec * BSIZE){
    perror("lseek");
    exit(1);
  }
  if(read(fsfd, buf, BSIZE) != BSIZE){
    perror("read");
    exit(1);
  }
}

uint
ialloc(ushort type)
{
  uint inum = freeinode++;
  struct dinode din;

  bzero(&din, sizeof(din));
  din.type = xshort(type);
  din.nlink = xshort(1);
  din.size = xint(0);
  winode(inum, &din);
  return inum;
}

void
balloc(int used)
{
  uchar buf[BSIZE];
  int b, i, n;

  printf("balloc: first %d blocks have been allocated\n", used);
  assert(used < FSSIZE);
  for(b = 0; b < used; b += BPB){
    bzero(buf, BSIZE);
    n = used - b;
    if(n > BPB)
      n = BPB;
    for(i = 0; i < n; i++)
      buf[i/8] = buf[i/8] | (0x1 << (i%8));
    printf("balloc: write bitmap block at sector %d\n",
           xint(sb.bmapstart) + b/BPB);
    wsect(xint(sb.bmapstart) + b/BPB, buf);
  }
}

#define min(a, b) ((a) < (b) ? (a) : (b))

// Return (and if needed allocate) one child pointer from an on-disk
// index block. Values in the image use xv6 little-endian byte order.
uint
indexblock(uint parent, uint index)
{
  uint entries[NINDIRECT];

  assert(index < NINDIRECT);
  rsect(parent, entries);
  if(entries[index] == 0){
    entries[index] = xint(freeblock++);
    wsect(parent, entries);
  }
  return xint(entries[index]);
}

uint
appendblock(struct dinode *din, uint fbn)
{
  uint index, offset, first, second, third, addr;

  assert(fbn < MAXFILE);
  if(fbn < NDIRECT){
    if(xint(din->addrs[fbn]) == 0)
      din->addrs[fbn] = xint(freeblock++);
    return xint(din->addrs[fbn]);
  }
  fbn -= NDIRECT;

  if(fbn < NSINDIRECT * NINDIRECT){
    index = fbn / NINDIRECT;
    offset = fbn % NINDIRECT;
    if(xint(din->addrs[NDIRECT + index]) == 0)
      din->addrs[NDIRECT + index] = xint(freeblock++);
    return indexblock(xint(din->addrs[NDIRECT + index]), offset);
  }
  fbn -= NSINDIRECT * NINDIRECT;

  if(fbn < NDINDIRECT * NDOUBLE){
    index = fbn / NDOUBLE;
    offset = fbn % NDOUBLE;
    first = offset / NINDIRECT;
    second = offset % NINDIRECT;
    if(xint(din->addrs[NDIRECT + NSINDIRECT + index]) == 0)
      din->addrs[NDIRECT + NSINDIRECT + index] = xint(freeblock++);
    addr = indexblock(xint(din->addrs[NDIRECT + NSINDIRECT + index]),
                      first);
    return indexblock(addr, second);
  }
  fbn -= NDINDIRECT * NDOUBLE;

  first = fbn / NDOUBLE;
  offset = fbn % NDOUBLE;
  second = offset / NINDIRECT;
  third = offset % NINDIRECT;
  if(xint(din->addrs[NDIRECT + NSINDIRECT + NDINDIRECT]) == 0)
    din->addrs[NDIRECT + NSINDIRECT + NDINDIRECT] = xint(freeblock++);
  addr = indexblock(
      xint(din->addrs[NDIRECT + NSINDIRECT + NDINDIRECT]), first);
  addr = indexblock(addr, second);
  return indexblock(addr, third);
}

void
iappend(uint inum, void *xp, int n)
{
  char *p = (char*)xp;
  uint fbn, off, n1;
  struct dinode din;
  char buf[BSIZE];
  uint x;

  rinode(inum, &din);
  off = xint(din.size);
  // printf("append inum %d at off %d sz %d\n", inum, off, n);
  while(n > 0){
    fbn = off / BSIZE;
    x = appendblock(&din, fbn);
    n1 = min(n, (fbn + 1) * BSIZE - off);
    rsect(x, buf);
    bcopy(p, buf + off - (fbn * BSIZE), n1);
    wsect(x, buf);
    n -= n1;
    off += n1;
    p += n1;
  }
  din.size = xint(off);
  winode(inum, &din);
}
