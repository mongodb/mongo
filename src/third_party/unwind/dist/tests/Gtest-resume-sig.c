/* libunwind - a platform-independent unwind library
   Copyright (C) 2003-2004 Hewlett-Packard Co
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

/*  Verify that unw_resume() restores the signal mask at proper time.  */

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include "compiler.h"

#include <libunwind.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

#ifdef HAVE_IA64INTRIN_H
# include <ia64intrin.h>
#endif

#define panic(args...)						\
	do { fprintf (stderr, args); ++nerrors; } while (0)

int verbose;
int nerrors;
int got_usr1, got_usr2;
char *sigusr1_sp;

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

#ifdef TEST_WITH_SIGINFO
void
handler (int sig,
         siginfo_t *si UNUSED,
         void *ucontext UNUSED)
#else
void
handler (int sig)
#endif
{
  unw_word_t ip;
  sigset_t mask;
  unw_context_t uc;
  unw_cursor_t c;
  char foo;
  int ret;
  // The test rely on SIGUSR2 mask to be cleared when the handler returns.
  // For local context from the signal handler, there doesn't seem to be a way
  // currently to set it so just clear the whole struct to make sure the signal mask is cleared.
  // This should probably be fixed to avoid signal mask being set to random values
  // by `unw_resume` if the context was not pre-zeroed.,
  // Using the signal ucontext direction should also work automatically but currently doesn't
  // on ARM/AArch64 (or any other archs that doesn't have a proper sigreturn implementation)
  memset(&uc, 0x0, sizeof(uc));

#if UNW_TARGET_IA64
  if (verbose)
    printf ("bsp = %llx\n", (unsigned long long) get_bsp ());
#endif

  if (verbose)
    printf ("got signal %d\n", sig);

  if (sig == SIGUSR1)
    {
      ++got_usr1;
      sigusr1_sp = &foo;

      sigemptyset (&mask);
      sigaddset (&mask, SIGUSR2);
      sigprocmask (SIG_BLOCK, &mask, NULL);
      kill (getpid (), SIGUSR2);	/* pend SIGUSR2 */

      signal (SIGUSR1, SIG_IGN);

      if ((ret = unw_getcontext (&uc)) < 0)
	panic ("unw_getcontext() failed: ret=%d\n", ret);
      if ((ret = unw_init_local (&c, &uc)) < 0)
	panic ("unw_init_local() failed: ret=%d\n", ret);

      if ((ret = unw_step (&c)) < 0)		/* step to signal trampoline */
	panic ("unw_step(1) failed: ret=%d\n", ret);

      if ((ret = unw_step (&c)) < 0)		/* step to kill() */
	panic ("unw_step(2) failed: ret=%d\n", ret);

#if defined(UNW_TARGET_TILEGX)
      if ((ret = unw_step (&c)) < 0)		/* step to signal trampoline */
	panic ("unw_step(2) failed: ret=%d\n", ret);
#endif

      if ((ret = unw_get_reg (&c, UNW_REG_IP, &ip)) < 0)
	panic ("unw_get_reg(IP) failed: ret=%d\n", ret);
      if (verbose)
	printf ("resuming at 0x%lx, with SIGUSR2 pending\n",
		(unsigned long) ip);
      unw_resume (&c);
    }
  else if (sig == SIGUSR2)
    {
      ++got_usr2;
      if (got_usr1)
	{
	  if (verbose)
	    printf ("OK: stack still at %p\n", &foo);
	}
      signal (SIGUSR2, SIG_IGN);
    }
  else
    panic ("Got unexpected signal %d\n", sig);
}

int
main (int argc, char **argv UNUSED)
{
  struct sigaction sa;
  float d = 1.0;
  int n = 0;

  if (argc > 1)
    verbose = 1;

  memset (&sa, 0, sizeof(sa));
#ifdef TEST_WITH_SIGINFO
  sa.sa_sigaction = handler;
  sa.sa_flags = SA_SIGINFO;
#else
  sa.sa_handler = handler;
#endif

  if (sigaction (SIGUSR1, &sa, NULL) != 0 ||
      sigaction (SIGUSR2, &sa, NULL) != 0)
    {
      fprintf (stderr, "sigaction() failed: %s\n", strerror (errno));
      return -1;
    }

  /* Use the FPU a bit; otherwise we get spurious errors should the
     signal handler need to use the FPU for any reason.  This seems to
     happen on x86-64.  */
  while (d > 0.0)
    {
      d /= 2.0;
      ++n;
    }
  if (n > 9999)
    return -1;	/* can't happen, but don't tell the compiler... */

  if (verbose)
    printf ("sending SIGUSR1\n");
  kill (getpid (), SIGUSR1);

  if (!got_usr2)
    panic ("failed to get SIGUSR2\n");

  if (nerrors)
    {
      fprintf (stderr, "FAILURE: detected %d errors\n", nerrors);
      exit (-1);
    }

  if (verbose)
    printf ("SUCCESS\n");
  return 0;
}
