#include "types.h"
#include "stat.h"
#include "user.h"
#include "mmu.h"

static void
show(char *label)
{
  printf(1, "%s: virtual pages: %d, physical pages: %d\n",
         label, getvp(), getpp());
}

static void
fail(char *message)
{
  printf(1, "ssualloc_test failed: %s\n", message);
  exit();
}

int
main(void)
{
  volatile char *one, *three;
  int vp, pp;

  show("Start: memory usages");

  if(ssualloc(0) != (void*)-1)
    fail("zero-sized allocation accepted");
  printf(1, "ssualloc() usage: argument wrong...\n");
  if(ssualloc(PGSIZE - 1) != (void*)-1)
    fail("non-page-aligned allocation accepted");
  printf(1, "ssualloc() usage: argument wrong...\n");

  vp = getvp();
  pp = getpp();
  one = (volatile char*)ssualloc(PGSIZE);
  if(one == (void*)-1 || getvp() != vp + 1 || getpp() != pp)
    fail("one-page lazy allocation accounting");
  show("After allocate one virtual page");

  one[0] = 1;
  if(getvp() != vp + 1 || getpp() != pp + 1)
    fail("one-page fault allocation accounting");
  show("After access one virtual page");

  vp = getvp();
  pp = getpp();
  three = (volatile char*)ssualloc(3 * PGSIZE);
  if(three == (void*)-1 || getvp() != vp + 3 || getpp() != pp)
    fail("three-page lazy allocation accounting");
  show("After allocate three virtual pages");

  three[0] = 1;
  if(getpp() != pp + 1)
    fail("first page was not allocated alone");
  show("After access of first virtual page");

  three[2 * PGSIZE] = 3;
  if(getpp() != pp + 2)
    fail("third page was not allocated alone");
  show("After access of third virtual page");

  three[PGSIZE] = 2;
  if(getpp() != pp + 3)
    fail("second page was not allocated alone");
  show("After access of second virtual page");

  printf(1, "ssualloc_test passed\n");
  exit();
}
