#define UNW_LOCAL_ONLY
#include <libunwind.h>
#include "compiler.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

int ok;
int verbose;

#if __GNUC__ > 4 || (__GNUC__ == 4 && __GNUC_MINOR__ > 3)
void a (int, ...) __attribute__((optimize(0)));
void b (void) __attribute__((optimize(0)));
void c (void) __attribute__((optimize(0)));
#endif

void NOINLINE
b (void)
{
  void *v[20];
  int i, n;

  n = unw_backtrace(v, 20);

  /* Check that the number of addresses given by unw_backtrace() looks
   * reasonable. If the compiler inlined everything, then this check will also
   * break. */
  if (n >= 7)
    ok = 1;

  if (verbose)
    for (i = 0; i < n; ++i)
      printf ("[%d] %p\n", i, v[i]);
}

void NOINLINE
c (void)
{
    b ();
}

void NOINLINE
a (int d, ...)
{
  switch (d)
    {
    case 5:
      a (4, 2,4);
      break;
    case 4:
      a (3, 1,3,5);
      break;
    case 3:
      a (2, 11, 13, 17, 23);
      break;
    case 2:
      a (1);
      break;
    case 1:
      c ();
    }
}

int
main (int argc, char **argv UNUSED)
{
  if (argc > 1)
    verbose = 1;

  a (5, 3, 4, 5, 6);

  if (!ok)
    {
      fprintf (stderr, "FAILURE: expected deeper backtrace.\n");
      return 1;
    }

  if (verbose)
    printf ("SUCCESS.\n");

  return 0;
}
