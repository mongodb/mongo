/* libunwind - a platform-independent unwind library
   Copyright (C) 2003-2004 Hewlett-Packard Co
	Contributed by David Mosberger-Tang <davidm@hpl.hp.com>

This file is part of libunwind.

Copyright (c) 2003 Hewlett-Packard Co.

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

#include <libunwind.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/resource.h>

#define panic(args...)				\
	{ fprintf (stderr, args); exit (-1); }

int verbose;

static void
do_backtrace (void)
{
  unw_cursor_t cursor;
  unw_word_t ip, sp;
  unw_context_t uc;
  int ret;

  unw_getcontext (&uc);
  if (unw_init_local (&cursor, &uc) < 0)
    panic ("unw_init_local failed!\n");

  do
    {
      unw_get_reg (&cursor, UNW_REG_IP, &ip);
      unw_get_reg (&cursor, UNW_REG_SP, &sp);

      if (verbose)
	printf ("%016lx (sp=%016lx)\n", (long) ip, (long) sp);

      ret = unw_step (&cursor);
      if (ret < 0)
	{
	  unw_get_reg (&cursor, UNW_REG_IP, &ip);
	  panic ("FAILURE: unw_step() returned %d for ip=%lx\n",
		 ret, (long) ip);
	}
    }
  while (ret > 0);
}

int
consume_some_stack_space (void)
{
  unw_cursor_t cursor;
  unw_context_t uc;
  char string[1024];

  memset (&cursor, 0, sizeof (cursor));
  memset (&uc, 0, sizeof (uc));
  return sprintf (string, "hello %p %p\n", &cursor, &uc);
}

int
main (int argc, char **argv UNUSED)
{
  struct rlimit rlim;

  verbose = argc > 1;

  if (consume_some_stack_space () > 9999)
    exit (-1);	/* can't happen, but don't let the compiler know... */

  rlim.rlim_cur = 0;
  rlim.rlim_max = RLIM_INFINITY;
  setrlimit (RLIMIT_DATA, &rlim);
  setrlimit (RLIMIT_AS, &rlim);

  do_backtrace ();
  return 0;
}
