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

#include "compiler.h"

#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <sys/types.h>

pid_t self;
int global[64];

int
func (int arg)
{
  int sum = 0, i, max, arr[1024];

  if (arg == 0)
    {
      sum = global[2];
      sum += sum + sum * getppid ();
      return sum;
    }
  else
    {
      max = arg;
      if (max >= 64)
	max = 64;

      for (i = 0; i < max; ++i)
	arr[i] = func (arg - 1);

      for (i = 0; i < max; ++i)
	if (arr[i] > 16)
	  sum += arr[i];
	else
	  sum -= arr[i];
    }
  return sum;
}

int
bar (int v)
{
  extern long f (long);
  int arr[1] = { v };
  uintptr_t r;

  /* This is a vain attempt to use up lots of registers to force
     the frame-chain info to be saved on the memory stack on ia64.
     It happens to work with gcc v3.3.4 and gcc v3.4.1 but perhaps
     not with any other compiler.  */
  r = (uintptr_t) malloc(f (arr[0])
       + (f (v) + (f (v) + (f (v) + (f (v) + (f (v) + (f (v)
       + (f (v) + (f (v) + (f (v) + (f (v) + (f (v) + (f (v) + (f (v)
       + (f (v) + (f (v) + (f (v) + (f (v) + (f (v) + (f (v) + (f (v)
       + (f (v) + (f (v) + (f (v) + (f (v) + (f (v) + (f (v) + (f (v)
       + (f (v) + (f (v) + (f (v) + (f (v) + (f (v) + (f (v) + (f (v)
       + (f (v) + (f (v) + (f (v) + (f (v) + (f (v) + (f (v) + (f (v)
       + (f (v) + (f (v) + (f (v) + (f (v) + (f (v) + (f (v) + (f (v)
       + (f (v) + (f (v) + (f (v) + (f (v) + (f (v) + (f (v) + (f (v)
       + (f (v) + (f (v) + (f (v) + (f (v) + (f (v) + (f (v) + (f (v)
       + (f (v) + (f (v) + (f (v) + (f (v) + (f (v) + (f (v) + (f (v)
       + (f (v) + (f (v) + (f (v) + (f (v) + (f (v) + (f (v) + (f (v)
       + (f (v) + (f (v) + (f (v) + (f (v) + (f (v) + (f (v) + (f (v)
       + (f (v) + (f (v) + (f (v) + (f (v) + (f (v) + (f (v) + (f (v)
       + (f (v) + (f (v) + (f (v) + (f (v) + (f (v) + (f (v) + (f (v)
       + (f (v) + (f (v) + (f (v) + (f (v) + (f (v) + (f (v) + (f (v)
       + (f (v) + (f (v) + (f (v) + (f (v) + (f (v) + (f (v) + (f (v)
       + (f (v) + (f (v) + (f (v) + (f (v) + (f (v) + (f (v) + f (v))
       ))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))
       )))))))))))))))))))))))))))))))))))))))))))))))))))))));
  if (r < 2)
    v = r;

  kill (self, SIGUSR1);	/* tell test-ptrace to start single-stepping */
  v = func (v);
  kill (self, SIGUSR2);	/* tell test-ptrace to stop single-stepping */
  return v;
}

int
main (int argc, char **argv UNUSED)
{
  int val = argc;

  signal (SIGUSR1, SIG_IGN);
  signal (SIGUSR2, SIG_IGN);

  self = getpid ();

  printf ("sum = %d\n", bar (val));
  return 0;
}
