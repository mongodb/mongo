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

/* This test uses the unwind interface to modify the IP in an ancestor
   frame while still returning to the parent frame.  */

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>

#include <libunwind-ia64.h>

#define panic(args...)				\
	{ fprintf (stderr, args); exit (-1); }

int verbose;

static void
sighandler (int signal)
{
  unw_cursor_t cursor, cursor2;
  unw_word_t ip;
  unw_context_t uc;

  if (verbose)
    printf ("caught signal %d\n", signal);

  unw_getcontext (&uc);
  if (unw_init_local (&cursor, &uc) < 0)
    panic ("unw_init() failed!\n");

  /* get cursor for caller of sighandler: */
  if (unw_step (&cursor) < 0)
    panic ("unw_step() failed!\n");

  cursor2 = cursor;
  while (!unw_is_signal_frame (&cursor2))
    if (unw_step (&cursor2) < 0)
      panic ("failed to find signal frame!\n");

  if (unw_step (&cursor2) < 0)
    panic ("unw_step() failed!\n");

  if (unw_get_reg (&cursor2, UNW_REG_IP, &ip) < 0)
    panic ("failed to get IP!\n");

  /* skip faulting instruction (doesn't handle MLX template) */
  ++ip;
  if ((ip & 0x3) == 0x3)
    ip += 13;

  if (unw_set_reg (&cursor2, UNW_REG_IP, ip) < 0)
    panic ("failed to set IP!\n");

  unw_resume (&cursor);	/* update context & return to caller of sighandler() */

  panic ("unexpected return from unw_resume()!\n");
}

static void
doit (volatile char *p)
{
  int ch;

  ch = *p;	/* trigger SIGSEGV */

  if (verbose)
    printf ("doit: finishing execution!\n");
}

int
main (int argc, char **argv)
{
  if (argc > 1)
    verbose = 1;

  signal (SIGSEGV, sighandler);
  doit (0);
  if (verbose)
    printf ("SUCCESS\n");
  return 0;
}
