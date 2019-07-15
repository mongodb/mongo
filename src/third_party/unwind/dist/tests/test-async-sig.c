/* libunwind - a platform-independent unwind library
   Copyright (C) 2004 Hewlett-Packard Co
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

/* Check whether basic unwinding truly is async-signal safe.  */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "compiler.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/time.h>

#define UNW_LOCAL_ONLY
#include <libunwind.h>

static const int nerrors_max = 100;

struct itimerval interval =
  {
    .it_interval = { .tv_sec = 0, .tv_usec = 0 },
    .it_value    = { .tv_sec = 0, .tv_usec = 1000 }
  };

int verbose;
int nerrors;
int sigcount;

#ifndef CONFIG_BLOCK_SIGNALS
/* When libunwind is configured with --enable-block-signals=no, the caller
   is responsible for preventing recursion via signal handlers.
   We use a simple global here.  In a multithreaded program, one would use
   a thread-local variable.  */
int recurcount;
#endif

#define panic(args...)					\
	{ ++nerrors; fprintf (stderr, args); return; }

static void
do_backtrace (int may_print, int get_proc_name)
{
  char buf[512], name[256];
  unw_cursor_t cursor;
  unw_word_t ip, sp, off;
  unw_context_t uc;
  int ret;
  int depth = 0;

#ifndef CONFIG_BLOCK_SIGNALS
  if (recurcount > 0)
    return;
  recurcount += 1;
#endif

  unw_getcontext (&uc);
  if (unw_init_local (&cursor, &uc) < 0)
    panic ("unw_init_local failed!\n");

  do
    {
      unw_get_reg (&cursor, UNW_REG_IP, &ip);
      unw_get_reg (&cursor, UNW_REG_SP, &sp);

      buf[0] = '\0';
      if (get_proc_name || (may_print && verbose))
	{
	  ret = unw_get_proc_name (&cursor, name, sizeof (name), &off);
	  if (ret == 0 && (may_print && verbose))
	    {
	      if (off)
		snprintf (buf, sizeof (buf), "<%s+0x%lx>", name, (long) off);
	      else
		{
		  size_t len = strlen (name);
		  buf[0] = '<';
		  memcpy (buf + 1, name, len);
		  buf[len + 1] = '>';
		  buf[len + 2] = '\0';
		}
	    }
	}

      if (may_print && verbose)
	printf ("%016lx %-32s (sp=%016lx)\n", (long) ip, buf, (long) sp);

      ret = unw_step (&cursor);
      if (ret < 0)
	{
	  unw_get_reg (&cursor, UNW_REG_IP, &ip);
	  panic ("FAILURE: unw_step() returned %d for ip=%lx\n",
		 ret, (long) ip);
	}
      if (depth++ > 100)
        {
	  panic ("FAILURE: unw_step() looping over %d iterations\n", depth);
	  break;
        }
    }
  while (ret > 0);

#ifndef CONFIG_BLOCK_SIGNALS
  recurcount -= 1;
#endif
}

void
sighandler (int signal)
{
  if (verbose)
    printf ("sighandler(signal=%d, count=%d)\n", signal, sigcount);

  do_backtrace (1, 1);

  ++sigcount;

  if (sigcount == 100)
    unw_set_caching_policy (unw_local_addr_space, UNW_CACHE_GLOBAL);
  else if (sigcount == 200)
    unw_set_caching_policy (unw_local_addr_space, UNW_CACHE_PER_THREAD);
  else if (sigcount == 300 || nerrors > nerrors_max)
    {
      if (nerrors > nerrors_max)
        panic ("Too many errors (%d)\n", nerrors);
      if (nerrors)
	{
	  fprintf (stderr, "FAILURE: detected %d errors\n", nerrors);
	  exit (-1);
	}
      if (verbose)
	printf ("SUCCESS.\n");
      exit (0);
    }
  setitimer (ITIMER_VIRTUAL, &interval, NULL);
}

int
main (int argc, char **argv UNUSED)
{
  struct sigaction act;
  long i = 0;

  if (argc > 1)
    verbose = 1;

  unw_set_caching_policy (unw_local_addr_space, UNW_CACHE_NONE);

  memset (&act, 0, sizeof (act));
  act.sa_handler = sighandler;
  act.sa_flags = SA_SIGINFO;
  sigaction (SIGVTALRM, &act, NULL);

  setitimer (ITIMER_VIRTUAL, &interval, NULL);

  while (1)
    {
      if (0 && verbose)
	printf ("%s: starting backtrace\n", __FUNCTION__);
      do_backtrace (0, (i++ % 100) == 0);
      if (nerrors > nerrors_max)
        {
	  fprintf (stderr, "Too many errors (%d)\n", nerrors);
	  exit (-1);
        }
    }
  return (0);
}
