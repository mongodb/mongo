/* libunwind - a platform-independent unwind library
   Copyright (C) 2002-2003 Hewlett-Packard Co
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

/* This file tests unwinding from a constructor from within an
   atexit() handler.  */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <libunwind.h>
#include "compiler.h"

int verbose, errors;

#define panic(args...)					\
	{ ++errors; fprintf (stderr, args); return; }

class Test_Class {
  public:
  Test_Class (void);
};

static Test_Class t;

static void
do_backtrace (void)
{
  char name[128], off[32];
  unw_word_t ip, offset;
  unw_cursor_t cursor;
  unw_context_t uc;
  int ret, count = 0;

  unw_getcontext (&uc);
  unw_init_local (&cursor, &uc);

  do
    {
      unw_get_reg (&cursor, UNW_REG_IP, &ip);
      name[0] = '\0';
      off[0] = '\0';
      if (unw_get_proc_name (&cursor, name, sizeof (name), &offset) == 0
	  && offset > 0)
	snprintf (off, sizeof (off), "+0x%lx", (long) offset);
      if (verbose)
	printf ("  [%lx] <%s%s>\n", (long) ip, name, off);
      if (++count > 32)
	panic ("FAILURE: didn't reach beginning of unwind-chain\n");
    }
  while ((ret = unw_step (&cursor)) > 0);

  if (ret < 0)
    panic ("FAILURE: unw_step() returned %d\n", ret);
}

static void
b (void)
{
  do_backtrace();
}

static void
a (void)
{
  if (verbose)
    printf ("do_backtrace() from atexit()-handler:\n");
  b();
  if (errors)
    abort ();	/* cannot portably call exit() from an atexit() handler */
}

Test_Class::Test_Class (void)
{
  if (verbose)
    printf ("do_backtrace() from constructor:\n");
  b();
}

int
main (int argc, char **argv UNUSED)
{
  verbose = argc > 1;
  return atexit (a);
}
