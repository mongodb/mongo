#include "compiler.h"
#include <libunwind.h>
#include <stdio.h>

int
main (int argc, char **argv UNUSED)
{
  int i, verbose = argc > 1;
  const char *msg;

  for (i = 0; i < 16; ++i)
    {
      msg = unw_strerror (-i);
      if (verbose)
	printf ("%6d -> %s\n", -i, msg);
    }
  return 0;
}
