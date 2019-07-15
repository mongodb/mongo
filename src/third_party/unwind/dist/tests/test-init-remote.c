/* libunwind - a platform-independent unwind library
   Copyright (C) 2004 Hewlett-Packard Co
	Contributed by David Mosberger-Tang <davidm@hpl.hp.com>

This file is part of libunwind.

Copyright (c) 2002 Hewlett-Packard Co.

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

/* This test simply verifies that unw_init_remote() can be used in
   lieu of unw_init_local().  This was broken for a while on ia64.  */

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include "compiler.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libunwind.h>

#define panic(args...)				\
	{ fprintf (stderr, args); exit (-1); }

int verbose;

static int
do_backtrace (void)
{
  char buf[512], name[256];
  unw_word_t ip, sp, off;
  unw_cursor_t cursor;
  unw_context_t uc;
  int ret;

  unw_getcontext (&uc);
  if (unw_init_remote (&cursor, unw_local_addr_space, &uc) < 0)
    panic ("unw_init_remote failed!\n");

  do
    {
      unw_get_reg (&cursor, UNW_REG_IP, &ip);
      unw_get_reg (&cursor, UNW_REG_SP, &sp);
      buf[0] = '\0';
      if (unw_get_proc_name (&cursor, name, sizeof (name), &off) == 0)
	{
	  if (off)
	    snprintf (buf, sizeof (buf), "<%s+0x%lx>", name, (long) off);
	  else
	    snprintf (buf, sizeof (buf), "<%s>", name);
	}
      if (verbose)
	printf ("%016lx %-32s (sp=%016lx)\n", (long) ip, buf, (long) sp);

      ret = unw_step (&cursor);
      if (ret < 0)
	{
	  unw_get_reg (&cursor, UNW_REG_IP, &ip);
	  printf ("FAILURE: unw_step() returned %d for ip=%lx\n",
		  ret, (long) ip);
	  return -1;
	}
    }
  while (ret > 0);

  return 0;
}

static int
foo (void)
{
  return do_backtrace ();
}

int
main (int argc, char **argv UNUSED)
{
  verbose = (argc > 1);

  if (verbose)
    printf ("Normal backtrace:\n");
  return foo ();
}
