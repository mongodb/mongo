/* libunwind - a platform-independent unwind library
   Copyright (C) 2001-2004 Hewlett-Packard Co
	Contributed by David Mosberger-Tang <davidm@hpl.hp.com>

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

/* This illustrates the basics of using the unwind interface for
   exception handling.  */

#include "compiler.h"

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <libunwind.h>

#ifdef HAVE_IA64INTRIN_H
# include <ia64intrin.h>
#endif

#define panic(args...)				\
	{ ++nerrors; fprintf (stderr, args); }

int nerrors = 0;
int verbose = 0;
int depth = 13;
volatile int got_here = 0;

extern void b (int);

void
raise_exception (void)
{
  unw_cursor_t cursor;
  unw_context_t uc;
  int i;
  unw_word_t ip;
  unw_word_t sp;

  unw_getcontext (&uc);
  if (unw_init_local (&cursor, &uc) < 0)
    {
      panic ("unw_init_local() failed!\n");
      return;
    }

  if (verbose)
    {
      printf ("raise_exception(): sp=%p\n", (void*) &sp);
      unw_get_reg (&cursor, UNW_REG_IP, &ip);
      unw_get_reg (&cursor, UNW_REG_SP, &sp);
      printf ("\t #%-3d ip=%p sp=%p\n", 0, (void*) ip, (void*) sp);
    }

  /* unwind to top-most frame a(), skipping over b() and raise_exception(): */
  for (i = 0; i < depth + 2; ++i)
    if (unw_step (&cursor) < 0)
      {
	panic ("unw_step() failed!\n");
	return;
      }
    else if (verbose)
      {
	unw_get_reg (&cursor, UNW_REG_IP, &ip);
	unw_get_reg (&cursor, UNW_REG_SP, &sp);
	printf ("\t #%-3d ip=%p sp=%p\n", i + 1, (void*) ip, (void*) sp);
      }
  unw_resume (&cursor);	/* transfer control to exception handler */
}

uintptr_t
get_bsp (void)
{
#if UNW_TARGET_IA64
# ifdef __INTEL_COMPILER
  return __getReg (_IA64_REG_AR_BSP);
# else
  return (uintptr_t) __builtin_ia64_bsp ();
# endif
#else
  return 0;
#endif
}

int NOINLINE
a (int n)
{
  long stack;
  int result = 99;

  if (verbose)
    printf ("a(n=%d): sp=%p bsp=0x%lx\n",
	    n, &stack, (unsigned long) get_bsp ());

  if (n > 0)
    a (n - 1);
  else
    b (16);

  if (verbose)
    {
      printf ("exception handler: here we go (sp=%p, bsp=0x%lx)...\n",
	      &stack, (unsigned long) get_bsp ());
      /* This call works around a bug in gcc (up-to pre3.4) which
	 causes invalid assembly code to be generated when
	 __builtin_ia64_bsp() gets predicated.  */
      getpid ();
    }
  if (n == depth)
    {
      result = 0;
      got_here = 1;
    }
  return result;
}

void NOINLINE
b (int n)
{
  if ((n & 1) == 0)
    {
      if (verbose)
	printf ("b(n=%d) calling raise_exception()\n", n);
      raise_exception ();
    }
  panic ("FAILURE: b() returned from raise_exception()!!\n");
}

int
main (int argc, char **argv)
{
  int result;

  if (argc > 1)
    {
      ++verbose;
      depth = atol (argv[1]);
      if (depth < 1)
	{
	  fprintf (stderr, "Usage: %s depth\n"
		   "  depth must be >= 1\n", argv[0]);
	  exit (-1);
	}
    }

  result = a (depth);
  if (result != 0 || !got_here || nerrors > 0)
    {
      fprintf (stderr,
	       "FAILURE: test failed: result=%d got_here=%d nerrors=%d\n",
	       result, got_here, nerrors);
      exit (-1);
    }

  if (verbose)
    printf ("SUCCESS!\n");
  return 0;
}
