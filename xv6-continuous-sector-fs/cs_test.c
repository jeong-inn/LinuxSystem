#include "types.h"
#include "stat.h"
#include "user.h"
#include "fcntl.h"

#define CHUNK 1024

static char writebuf[CHUNK];
static char readbuf[CHUNK];

static void
die(char *message)
{
  printf(2, "cs_test: %s\n", message);
  exit();
}

static void
fill(char value)
{
  int i;

  for(i = 0; i < CHUNK; i++)
    writebuf[i] = value;
}

static void
write_chunks(int fd, int count, char value)
{
  int i;

  fill(value);
  for(i = 0; i < count; i++){
    if(write(fd, writebuf, sizeof(writebuf)) != sizeof(writebuf))
      die("short write");
  }
}

static void
verify(char *path, int chunks, char value)
{
  int fd, i, j, n;

  if((fd = open(path, O_RDONLY)) < 0)
    die("cannot reopen file");
  for(i = 0; i < chunks; i++){
    n = read(fd, readbuf, sizeof(readbuf));
    if(n != sizeof(readbuf))
      die("short read");
    for(j = 0; j < n; j++){
      if(readbuf[j] != value)
        die("data mismatch");
    }
  }
  if(read(fd, readbuf, 1) != 0)
    die("file is larger than expected");
  close(fd);
}

static void
continuous_case(void)
{
  int fd;

  unlink("test_cs_cont");
  if((fd = open("test_cs_cont", O_CREATE | O_CS | O_RDWR)) < 0)
    die("cannot create continuous CS file");

  write_chunks(fd, 130, 'C');
  printinfo(fd);
  close(fd);
  verify("test_cs_cont", 130, 'C');

  // Deletion exercises CS-aware itrunc().
  if(unlink("test_cs_cont") < 0)
    die("cannot delete continuous CS file");
}

static void
discontinuous_case(void)
{
  int csfd, normalfd;

  unlink("test_cs");
  unlink("test_norm");
  if((csfd = open("test_cs", O_CREATE | O_CS | O_RDWR)) < 0)
    die("cannot create discontinuous CS file");

  write_chunks(csfd, 51, 'D');

  if((normalfd = open("test_norm", O_CREATE | O_RDWR)) < 0)
    die("cannot create normal file");
  write_chunks(normalfd, 2, 'N');
  printinfo(normalfd);

  write_chunks(csfd, 79, 'D');
  printinfo(csfd);

  close(normalfd);
  close(csfd);
  verify("test_norm", 2, 'N');
  verify("test_cs", 130, 'D');

  if(unlink("test_norm") < 0 || unlink("test_cs") < 0)
    die("cannot delete test files");
}

int
main(void)
{
  printf(1, "cs_test: continuous allocation\n");
  continuous_case();
  printf(1, "cs_test: interrupted allocation\n");
  discontinuous_case();
  printf(1, "cs_test: all checks passed\n");
  exit();
}
