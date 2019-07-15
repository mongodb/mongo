/* libunwind - a platform-independent unwind library
   Copyright (C) 2004 Hewlett-Packard Co
	Contributed by David Mosberger-Tang <davidm@hpl.hp.com>

This file is part of libunwind.

Permission is hereby granted, free of charge, to any person obtaining
a copy of this software and associated documentation files (the
"Software"), to deal in the Software without restriction, including
without limitation the rights to use, copy, modify, merge, publish,
distribute, sublicense, and/or sell copies of the Software, and to
permit persons to whom the Software is furnished to do so, subject to
the following conditions:

The above copyright notice and this permission notice shall be
included in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE
LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.  */

/* This program creates lots of mappings such that on Linux the
   reading of /proc/PID/maps gets very slow.  With proper caching,
   test-ptrace should still run at acceptable speed once
   /proc/PID/maps has been scanned.  If the program dies with a
   SIGALRM, it means it was running unexpectedly slow.  */

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <sys/mman.h>

#if !defined(MAP_ANONYMOUS) && defined(MAP_ANON)
# define MAP_ANONYMOUS MAP_ANON
#endif
#if !defined(MAP_NORESERVE)
# define MAP_NORESERVE 0
#endif

int
main (void)
{
  long n = 0;

  signal (SIGUSR1, SIG_IGN);
  signal (SIGUSR2, SIG_IGN);

  printf ("Starting mmap test...\n");
  for (n = 0; n < 30000; ++n)
    {
      if (mmap (NULL, 1, (n & 1) ? PROT_READ : PROT_WRITE,
		MAP_PRIVATE | MAP_ANONYMOUS
#ifdef MAP_NORESERVE
		| MAP_NORESERVE
#endif
		,
		-1, 0) == MAP_FAILED)
	{
	  printf ("Failed after %ld successful maps\n", n - 1);
	  exit (0);
	}
    }

  alarm (80);	/* die if we don't finish in 80 seconds */

  printf ("Turning on single-stepping...\n");
  kill (getpid (), SIGUSR1);	/* tell test-ptrace to start single-stepping */
  printf ("Va bene?\n");
  kill (getpid (), SIGUSR2);	/* tell test-ptrace to stop single-stepping */
  printf ("Turned single-stepping off...\n");
  return 0;
}
