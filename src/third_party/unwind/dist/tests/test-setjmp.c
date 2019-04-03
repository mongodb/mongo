/* libunwind - a platform-independent unwind library
   Copyright (C) 2003 Hewlett-Packard Co
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

/* The setjmp()/longjmp(), sigsetjmp()/siglongjmp().  */

#include "compiler.h"

#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int nerrors;
int verbose;

static jmp_buf jbuf;
static sigjmp_buf sigjbuf;
static sigset_t sigset4;

void
raise_longjmp (jmp_buf jbuf, int i, int n)
{
  while (i < n)
    raise_longjmp (jbuf, i + 1, n);

  longjmp (jbuf, n);
}

void
test_setjmp (void)
{
  volatile int i;
  jmp_buf jbuf;
  int ret;

  for (i = 0; i < 10; ++i)
    {
      if ((ret = setjmp (jbuf)))
	{
	  if (verbose)
	    printf ("%s: secondary setjmp () return, ret=%d\n",
		    __FUNCTION__, ret);
	  if (ret != i + 1)
	    {
	      fprintf (stderr, "%s: setjmp() returned %d, expected %d\n",
		       __FUNCTION__, ret, i + 1);
	      ++nerrors;
	    }
	  continue;
	}
      if (verbose)
	printf ("%s.%d: done with setjmp(); calling children\n",
		__FUNCTION__, i + 1);

      raise_longjmp (jbuf, 0, i + 1);

      fprintf (stderr, "%s: raise_longjmp() returned unexpectedly\n",
	       __FUNCTION__);
      ++nerrors;
    }
}


void
raise_siglongjmp (sigjmp_buf jbuf, int i, int n)
{
  while (i < n)
    raise_siglongjmp (jbuf, i + 1, n);

  siglongjmp (jbuf, n);
}

void
test_sigsetjmp (void)
{
  sigjmp_buf jbuf;
  volatile int i;
  int ret;

  for (i = 0; i < 10; ++i)
    {
      if ((ret = sigsetjmp (jbuf, 1)))
	{
	  if (verbose)
	    printf ("%s: secondary sigsetjmp () return, ret=%d\n",
		    __FUNCTION__, ret);
	  if (ret != i + 1)
	    {
	      fprintf (stderr, "%s: sigsetjmp() returned %d, expected %d\n",
		       __FUNCTION__, ret, i + 1);
	      ++nerrors;
	    }
	  continue;
	}
      if (verbose)
	printf ("%s.%d: done with sigsetjmp(); calling children\n",
		__FUNCTION__, i + 1);

      raise_siglongjmp (jbuf, 0, i + 1);

      fprintf (stderr, "%s: raise_siglongjmp() returned unexpectedly\n",
	       __FUNCTION__);
      ++nerrors;
    }
}

void
sighandler (int signal)
{
  if (verbose)
    printf ("%s: got signal %d\n", __FUNCTION__, signal);

  sigprocmask (SIG_BLOCK, NULL, (sigset_t *) &sigset4);
  if (verbose)
    printf ("%s: back from sigprocmask\n", __FUNCTION__);

  siglongjmp (sigjbuf, 1);
  printf ("%s: siglongjmp() returned unexpectedly!\n", __FUNCTION__);
}

int
main (int argc, char **argv UNUSED)
{
  volatile sigset_t sigset1, sigset2, sigset3;
  volatile struct sigaction act;

  if (argc > 1)
    verbose = 1;

  sigemptyset ((sigset_t *) &sigset1);
  sigaddset ((sigset_t *) &sigset1, SIGUSR1);
  sigemptyset ((sigset_t *) &sigset2);
  sigaddset ((sigset_t *) &sigset2, SIGUSR2);

  memset ((void *) &act, 0, sizeof (act));
  act.sa_handler = sighandler;
  sigaction (SIGTERM, (struct sigaction *) &act, NULL);

  test_setjmp ();
  test_sigsetjmp ();

  /* _setjmp() MUST NOT change signal mask: */
  sigprocmask (SIG_SETMASK, (sigset_t *) &sigset1, NULL);
  if (_setjmp (jbuf))
    {
      sigemptyset ((sigset_t *) &sigset3);
      sigprocmask (SIG_BLOCK, NULL, (sigset_t *) &sigset3);
      if (memcmp ((sigset_t *) &sigset3, (sigset_t *) &sigset2,
		  sizeof (sigset_t)) != 0)
	{
	  fprintf (stderr, "FAILURE: _longjmp() manipulated signal mask!\n");
	  ++nerrors;
	}
      else if (verbose)
	printf ("OK: _longjmp() seems not to change signal mask\n");
    }
  else
    {
      sigprocmask (SIG_SETMASK, (sigset_t *) &sigset2, NULL);
      _longjmp (jbuf, 1);
    }

  /* sigsetjmp(jbuf, 1) MUST preserve signal mask: */
  sigprocmask (SIG_SETMASK, (sigset_t *) &sigset1, NULL);
  if (sigsetjmp (sigjbuf, 1))
    {
      sigemptyset ((sigset_t *) &sigset3);
      sigprocmask (SIG_BLOCK, NULL, (sigset_t *) &sigset3);
      if (memcmp ((sigset_t *) &sigset3, (sigset_t *) &sigset1,
		  sizeof (sigset_t)) != 0)
	{
	  fprintf (stderr,
		   "FAILURE: siglongjmp() didn't restore signal mask!\n");
	  ++nerrors;
	}
      else if (verbose)
	printf ("OK: siglongjmp() restores signal mask when asked to\n");
    }
  else
    {
      sigprocmask (SIG_SETMASK, (sigset_t *) &sigset2, NULL);
      siglongjmp (sigjbuf, 1);
    }

  /* sigsetjmp(jbuf, 0) MUST NOT preserve signal mask: */
  sigprocmask (SIG_SETMASK, (sigset_t *) &sigset1, NULL);
  if (sigsetjmp (sigjbuf, 0))
    {
      sigemptyset ((sigset_t *) &sigset3);
      sigprocmask (SIG_BLOCK, NULL, (sigset_t *) &sigset3);
      if (memcmp ((sigset_t *) &sigset3, (sigset_t *) &sigset2,
		  sizeof (sigset_t)) != 0)
	{
	  fprintf (stderr,
		   "FAILURE: siglongjmp() changed signal mask!\n");
	  ++nerrors;
	}
      else if (verbose)
	printf ("OK: siglongjmp() leaves signal mask alone when asked to\n");
    }
  else
    {
      sigprocmask (SIG_SETMASK, (sigset_t *) &sigset2, NULL);
      siglongjmp (sigjbuf, 1);
    }

  /* sigsetjmp(jbuf, 1) MUST preserve signal mask: */
  sigprocmask (SIG_SETMASK, (sigset_t *) &sigset1, NULL);
  if (sigsetjmp (sigjbuf, 1))
    {
      sigemptyset ((sigset_t *) &sigset3);
      sigprocmask (SIG_BLOCK, NULL, (sigset_t *) &sigset3);
      if (memcmp ((sigset_t *) &sigset3, (sigset_t *) &sigset1,
		  sizeof (sigset_t)) != 0)
	{
	  fprintf (stderr,
		   "FAILURE: siglongjmp() didn't restore signal mask!\n");
	  ++nerrors;
	}
      else if (verbose)
	printf ("OK: siglongjmp() restores signal mask when asked to\n");
    }
  else
    {
      sigprocmask (SIG_SETMASK, (sigset_t *) &sigset2, NULL);
      kill (getpid (), SIGTERM);
      fprintf (stderr, "FAILURE: unexpected return from kill()\n");
      ++nerrors;
    }

  /* sigsetjmp(jbuf, 0) MUST NOT preserve signal mask: */
  sigprocmask (SIG_SETMASK, (sigset_t *) &sigset1, NULL);
  if (sigsetjmp (sigjbuf, 0))
    {
      sigemptyset ((sigset_t *) &sigset3);
      sigprocmask (SIG_BLOCK, NULL, (sigset_t *) &sigset3);
      if (memcmp ((sigset_t *) &sigset3, (sigset_t *) &sigset4,
		  sizeof (sigset_t)) != 0)
	{
	  fprintf (stderr,
		   "FAILURE: siglongjmp() changed signal mask!\n");
	  ++nerrors;
	}
      else if (verbose)
	printf ("OK: siglongjmp() leaves signal mask alone when asked to\n");
    }
  else
    {
      sigprocmask (SIG_SETMASK, (sigset_t *) &sigset2, NULL);
      kill (getpid (), SIGTERM);
      fprintf (stderr, "FAILURE: unexpected return from kill()\n");
      ++nerrors;
    }

  if (nerrors > 0)
    {
      fprintf (stderr, "FAILURE: detected %d failures\n", nerrors);
      exit (-1);
    }
  if (verbose)
    printf ("SUCCESS\n");
  return 0;
}
