#include "types.h"
#include "stat.h"
#include "user.h"
#include "fcntl.h"
#include "fs.h"

static char block[BSIZE];

static void
setblock(int n)
{
  int i;

  for(i = 0; i < BSIZE; i++)
    block[i] = (n * 17 + i) & 0xff;
}

static void
failed(int test, char *step)
{
  printf(1, "### test%d failed: %s\n", test, step);
  exit();
}

static void
runtest(int test, char *name, int blocks)
{
  int fd, i, j;

  printf(1, "### test%d start\n", test);
  unlink(name);
  fd = open(name, O_CREATE | O_RDWR);
  if(fd < 0)
    failed(test, "create");

  printf(1, "create and write %d blocks... ", blocks);
  for(i = 0; i < blocks; i++){
    setblock(i);
    if(write(fd, block, BSIZE) != BSIZE)
      failed(test, "write");
  }
  printf(1, "ok\n");

  if(close(fd) < 0)
    failed(test, "close");
  printf(1, "close file descriptor... ok\n");

  fd = open(name, O_RDONLY);
  if(fd < 0)
    failed(test, "reopen");
  for(i = 0; i < blocks; i++){
    if(read(fd, block, BSIZE) != BSIZE)
      failed(test, "read");
    for(j = 0; j < BSIZE; j++)
      if((uchar)block[j] != (uchar)((i * 17 + j) & 0xff))
        failed(test, "data mismatch");
  }
  close(fd);
  printf(1, "open and read file... ok\n");

  if(unlink(name) < 0)
    failed(test, "unlink");
  printf(1, "unlink %s... ok\n", name);
  fd = open(name, O_RDONLY);
  if(fd >= 0){
    close(fd);
    failed(test, "file remained after unlink");
  }
  printf(1, "open %s again... failed\n", name);
  printf(1, "### test%d passed...\n\n", test);
}

int
main(void)
{
  runtest(1, "file1", 5);
  runtest(2, "file2", 500);
  runtest(3, "file3", 5000);
  runtest(4, "file4", 50000);
  exit();
}
