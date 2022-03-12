/* libunwind - a platform-independent unwind library
   Copyright (C) 2004-2005 Hewlett-Packard Co
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

/* This file tests corner-cases of NaT-bit handling.  */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libunwind.h>
#include "compiler.h"

#ifdef HAVE_SYS_UC_ACCESS_H
# include <sys/uc_access.h>
#endif

#include "tdep-ia64/rse.h"

#define NUM_RUNS		1024
//#define NUM_RUNS		1
#define MAX_CHECKS		1024
//#define MAX_CHECKS		2
#define MAX_VALUES_PER_FUNC	4

#define panic(args...)							  \
	do { printf (args); ++nerrors; } while (0)

typedef void save_func_t (void *funcs, unsigned long *vals);
typedef unw_word_t *check_func_t (unw_cursor_t *c, unsigned long *vals);

extern void flushrs (void);

extern save_func_t save_static_to_stacked;
static check_func_t check_static_to_stacked;

extern save_func_t save_static_to_fr;
static check_func_t check_static_to_fr;

extern save_func_t save_static_to_br;
static check_func_t check_static_to_br;

extern save_func_t save_static_to_mem;
static check_func_t check_static_to_mem;

extern save_func_t save_static_to_mem2;
static check_func_t check_static_to_mem2;

extern save_func_t save_static_to_mem3;
static check_func_t check_static_to_mem3;

extern save_func_t save_static_to_mem4;
static check_func_t check_static_to_mem4;

extern save_func_t save_static_to_mem5;
static check_func_t check_static_to_mem5;

extern save_func_t save_static_to_scratch;
static check_func_t check_static_to_scratch;

extern save_func_t rotate_regs;
static check_func_t check_rotate_regs;

extern save_func_t save_pr;
static check_func_t check_pr;

static int verbose;
static int nerrors;

static int num_checks;
static save_func_t *funcs[MAX_CHECKS + 1];
static check_func_t *checks[MAX_CHECKS];
static unw_word_t values[MAX_CHECKS*MAX_VALUES_PER_FUNC];

static struct
  {
    save_func_t *func;
    check_func_t *check;
  }
all_funcs[] =
  {
    { save_static_to_stacked,	check_static_to_stacked },
    { save_static_to_fr,	check_static_to_fr },
    { save_static_to_br,	check_static_to_br },
    { save_static_to_mem,	check_static_to_mem },
    { save_static_to_mem2,	check_static_to_mem2 },
    { save_static_to_mem3,	check_static_to_mem3 },
    { save_static_to_mem4,	check_static_to_mem4 },
    { save_static_to_mem5,	check_static_to_mem5 },
    { save_static_to_scratch,	check_static_to_scratch },
    { save_pr,			check_pr },
    { rotate_regs,		check_rotate_regs },
  };

static unw_word_t
random_word (void)
{
  unw_word_t val = random ();

  if (sizeof (unw_word_t) > 4)
    val |= ((unw_word_t) random ()) << 32;

  return val;
}

void
sighandler (int signal, void *siginfo, void *context)
{
  unsigned long *bsp, *arg1;
  save_func_t **arg0;
  ucontext_t *uc = context;

#if defined(__linux__)
  {
    long sof;
    int sp;

    if (verbose)
      printf ("sighandler: signal %d sp=%p nat=%08lx pr=%lx\n",
	      signal, &sp, uc->uc_mcontext.sc_nat, uc->uc_mcontext.sc_pr);
    sof = uc->uc_mcontext.sc_cfm & 0x7f;
    bsp = (unsigned long *) rse_skip_regs (uc->uc_mcontext.sc_ar_bsp, -sof);
  }
#elif defined(__hpux)
  if (__uc_get_ar (uc, UNW_IA64_AR_BSP - UNW_IA64_AR, &bsp) != 0)
    {
      panic ("%s: reading of ar.bsp failed, errno=%d", __FUNCTION__, errno);
      return;
    }
#endif

  flushrs ();
  arg0 = (save_func_t **) *bsp;
  bsp = (unsigned long *) rse_skip_regs ((uint64_t) bsp, 1);
  arg1 = (unsigned long *) *bsp;

  (*arg0[0]) (arg0 + 1, arg1);

  /* skip over the instruction which triggered sighandler() */
#if defined(__linux__)
  ++uc->uc_mcontext.sc_ip;
#elif defined(HAVE_SYS_UC_ACCESS_H)
  {
    unsigned long ip;

    if (__uc_get_ip (uc, &ip) != 0)
      {
	panic ("%s: reading of ip failed, errno=%d", __FUNCTION__, errno);
	return;
      }
    if (__uc_set_ip (uc, ip) != 0)
      {
	panic ("%s: writing of ip failed, errno=%d", __FUNCTION__, errno);
	return;
      }
  }
#endif
}

static void
enable_sighandler (void)
{
  struct sigaction act;

  memset (&act, 0, sizeof (act));
  act.sa_handler = (void (*)(int)) sighandler;
  act.sa_flags = SA_SIGINFO | SA_NODEFER;
  if (sigaction (SIGSEGV, &act, NULL) < 0)
    panic ("sigaction: %s\n", strerror (errno));
}

static void
disable_sighandler (void)
{
  struct sigaction act;

  memset (&act, 0, sizeof (act));
  act.sa_handler = SIG_DFL;
  act.sa_flags = SA_SIGINFO | SA_NODEFER;
  if (sigaction (SIGSEGV, &act, NULL) < 0)
    panic ("sigaction: %s\n", strerror (errno));
}

static unw_word_t *
check_static_to_stacked (unw_cursor_t *c, unw_word_t *vals)
{
  unw_word_t r[4];
  unw_word_t nat[4];
  int i, ret;

  if (verbose)
    printf ("  %s()\n", __FUNCTION__);

  vals -= 4;

  for (i = 0; i < 4; ++i)
    if ((ret = unw_get_reg (c, UNW_IA64_GR + 4 + i, &r[i])) < 0)
      panic ("%s: failed to read register r%d, error=%d\n",
	     __FUNCTION__, 4 + i, ret);

  for (i = 0; i < 4; ++i)
    if ((ret = unw_get_reg (c, UNW_IA64_NAT + 4 + i, &nat[i])) < 0)
      panic ("%s: failed to read register nat%d, error=%d\n",
	     __FUNCTION__, 4 + i, ret);

  for (i = 0; i < 4; ++i)
    {
      if (verbose)
	printf ("    r%d = %c%016lx (expected %c%016lx)\n",
		4 + i, nat[i] ? '*' : ' ', r[i],
		(vals[i] & 1) ? '*' : ' ', vals[i]);

      if (vals[i] & 1)
	{
	  if (!nat[i])
	    panic ("%s: r%d not a NaT!\n", __FUNCTION__, 4 + i);
	}
      else
	{
	  if (nat[i])
	    panic ("%s: r%d a NaT!\n", __FUNCTION__, 4 + i);
	  if (r[i] != vals[i])
	    panic ("%s: r%d=%lx instead of %lx!\n",
		   __FUNCTION__, 4 + i, r[i], vals[i]);
	}
    }
  return vals;
}

static unw_word_t *
check_static_to_fr (unw_cursor_t *c, unw_word_t *vals)
{
  unw_word_t r4;
  unw_word_t nat4;
  int ret;

  if (verbose)
    printf ("  %s()\n", __FUNCTION__);

  vals -= 1;

  if ((ret = unw_get_reg (c, UNW_IA64_GR + 4, &r4)) < 0)
    panic ("%s: failed to read register r4, error=%d\n", __FUNCTION__, ret);

  if ((ret = unw_get_reg (c, UNW_IA64_NAT + 4, &nat4)) < 0)
    panic ("%s: failed to read register nat4, error=%d\n", __FUNCTION__, ret);

  if (verbose)
    printf ("    r4 = %c%016lx (expected %c%016lx)\n",
	    nat4 ? '*' : ' ', r4, (vals[0] & 1) ? '*' : ' ', vals[0]);

  if (vals[0] & 1)
    {
      if (!nat4)
	panic ("%s: r4 not a NaT!\n", __FUNCTION__);
    }
  else
    {
      if (nat4)
	panic ("%s: r4 a NaT!\n", __FUNCTION__);
      if (r4 != vals[0])
	panic ("%s: r4=%lx instead of %lx!\n", __FUNCTION__, r4, vals[0]);
    }
  return vals;
}

static unw_word_t *
check_static_to_br (unw_cursor_t *c, unw_word_t *vals)
{
  unw_word_t r4, nat4;
  int ret;

  if (verbose)
    printf ("  %s()\n", __FUNCTION__);

  vals -= 1;

  if ((ret = unw_get_reg (c, UNW_IA64_GR + 4, &r4)) < 0)
    panic ("%s: failed to read register r4, error=%d\n", __FUNCTION__, ret);

  if ((ret = unw_get_reg (c, UNW_IA64_NAT + 4, &nat4)) < 0)
    panic ("%s: failed to read register nat4, error=%d\n", __FUNCTION__, ret);

  if (verbose)
    printf ("    r4 = %c%016lx (expected %c%016lx)\n",
	    nat4 ? '*' : ' ', r4, (vals[0] & 1) ? '*' : ' ', vals[0]);

  if (vals[0] & 1)
    {
      if (!nat4)
	panic ("%s: r4 not a NaT!\n", __FUNCTION__);
    }
  else
    {
      if (nat4)
	panic ("%s: r4 a NaT!\n", __FUNCTION__);
      if (r4 != vals[0])
	panic ("%s: r4=%lx instead of %lx!\n", __FUNCTION__, r4, vals[0]);
    }
  return vals;
}

static unw_word_t *
check_static_to_mem (unw_cursor_t *c, unw_word_t *vals)
{
  unw_word_t r5, nat5;
  int ret;

  if (verbose)
    printf ("  %s()\n", __FUNCTION__);

  vals -= 1;

  if ((ret = unw_get_reg (c, UNW_IA64_GR + 5, &r5)) < 0)
    panic ("%s: failed to read register r5, error=%d\n", __FUNCTION__, ret);

  if ((ret = unw_get_reg (c, UNW_IA64_NAT + 5, &nat5)) < 0)
    panic ("%s: failed to read register nat5, error=%d\n", __FUNCTION__, ret);

  if (verbose)
    printf ("    r5 = %c%016lx (expected %c%016lx)\n",
	    nat5 ? '*' : ' ', r5, (vals[0] & 1) ? '*' : ' ', vals[0]);

  if (vals[0] & 1)
    {
      if (!nat5)
	panic ("%s: r5 not a NaT!\n", __FUNCTION__);
    }
  else
    {
      if (nat5)
	panic ("%s: r5 a NaT!\n", __FUNCTION__);
      if (r5 != vals[0])
	panic ("%s: r5=%lx instead of %lx!\n", __FUNCTION__, r5, vals[0]);
    }
  return vals;
}

static unw_word_t *
check_static_to_memN (unw_cursor_t *c, unw_word_t *vals, const char *func)
{
  unw_word_t r6, nat6;
  int ret;

  if (verbose)
    printf ("  %s()\n", func);

  vals -= 1;

  if ((ret = unw_get_reg (c, UNW_IA64_GR + 6, &r6)) < 0)
    panic ("%s: failed to read register r6, error=%d\n", __FUNCTION__, ret);

  if ((ret = unw_get_reg (c, UNW_IA64_NAT + 6, &nat6)) < 0)
    panic ("%s: failed to read register nat6, error=%d\n", __FUNCTION__, ret);

  if (verbose)
    printf ("    r6 = %c%016lx (expected %c%016lx)\n",
	    nat6 ? '*' : ' ', r6, (vals[0] & 1) ? '*' : ' ', vals[0]);

  if (vals[0] & 1)
    {
      if (!nat6)
	panic ("%s: r6 not a NaT!\n", __FUNCTION__);
    }
  else
    {
      if (nat6)
	panic ("%s: r6 a NaT!\n", __FUNCTION__);
      if (r6 != vals[0])
	panic ("%s: r6=%lx instead of %lx!\n", __FUNCTION__, r6, vals[0]);
    }
  return vals;
}

static unw_word_t *
check_static_to_mem2 (unw_cursor_t *c, unw_word_t *vals)
{
  return check_static_to_memN (c, vals, __FUNCTION__);
}

static unw_word_t *
check_static_to_mem3 (unw_cursor_t *c, unw_word_t *vals)
{
  return check_static_to_memN (c, vals, __FUNCTION__);
}

static unw_word_t *
check_static_to_mem4 (unw_cursor_t *c, unw_word_t *vals)
{
  return check_static_to_memN (c, vals, __FUNCTION__);
}

static unw_word_t *
check_static_to_mem5 (unw_cursor_t *c, unw_word_t *vals)
{
  return check_static_to_memN (c, vals, __FUNCTION__);
}

static unw_word_t *
check_static_to_scratch (unw_cursor_t *c, unw_word_t *vals)
{
  unw_word_t r[4], nat[4], ec, expected;
  unw_fpreg_t f4;
  int i, ret;

  if (verbose)
    printf ("  %s()\n", __FUNCTION__);

  vals -= 4;

  while (!unw_is_signal_frame (c))
    if ((ret = unw_step (c)) < 0)
      panic ("%s: unw_step (ret=%d): Failed to skip over signal handler\n",
	     __FUNCTION__, ret);
  if ((ret = unw_step (c)) < 0)
    panic ("%s: unw_step (ret=%d): Failed to skip over signal handler\n",
	   __FUNCTION__, ret);

  for (i = 0; i < 4; ++i)
    if ((ret = unw_get_reg (c, UNW_IA64_GR + 4 + i, &r[i])) < 0)
      panic ("%s: failed to read register r%d, error=%d\n",
	     __FUNCTION__, 4 + i, ret);

  for (i = 0; i < 4; ++i)
    if ((ret = unw_get_reg (c, UNW_IA64_NAT + 4 + i, &nat[i])) < 0)
      panic ("%s: failed to read register nat%d, error=%d\n",
	     __FUNCTION__, 4 + i, ret);

  for (i = 0; i < 4; ++i)
    {
      if (verbose)
	printf ("    r%d = %c%016lx (expected %c%016lx)\n",
		4 + i, nat[i] ? '*' : ' ', r[i],
		(vals[i] & 1) ? '*' : ' ', vals[i]);

      if (vals[i] & 1)
	{
	  if (!nat[i])
	    panic ("%s: r%d not a NaT!\n", __FUNCTION__, 4 + i);
	}
      else
	{
	  if (nat[i])
	    panic ("%s: r%d a NaT!\n", __FUNCTION__, 4 + i);
	  if (r[i] != vals[i])
	    panic ("%s: r%d=%lx instead of %lx!\n",
		   __FUNCTION__, 4 + i, r[i], vals[i]);
	}
    }
  if ((ret = unw_get_fpreg (c, UNW_IA64_FR + 4, &f4)) < 0)
    panic ("%s: failed to read f4, error=%d\n", __FUNCTION__, ret);

  /* These tests are little-endian specific: */
  if (nat[0])
    {
      if (f4.raw.bits[0] != 0 || f4.raw.bits[1] != 0x1fffe)
	panic ("%s: f4=%016lx.%016lx instead of NaTVal!\n",
	       __FUNCTION__, f4.raw.bits[1], f4.raw.bits[0]);
    }
  else
    {
      if (f4.raw.bits[0] != r[0] || f4.raw.bits[1] != 0x1003e)
	panic ("%s: f4=%016lx.%016lx instead of %lx!\n",
	       __FUNCTION__, f4.raw.bits[1], f4.raw.bits[0], r[0]);
    }

  if ((unw_get_reg (c, UNW_IA64_AR_EC, &ec)) < 0)
    panic ("%s: failed to read register ar.ec, error=%d\n", __FUNCTION__, ret);

  expected = vals[0] & 0x3f;
  if (ec != expected)
    panic ("%s: ar.ec=%016lx instead of %016lx!\n",
	   __FUNCTION__, ec, expected);

  return vals;
}

static unw_word_t *
check_pr (unw_cursor_t *c, unw_word_t *vals)
{
  unw_word_t pr, expected;
  int ret;
# define BIT(n) ((unw_word_t) 1 << (n))
# define DONTCARE (BIT( 6) | BIT( 7) | BIT( 8) | BIT( 9) | BIT(10) \
		 | BIT(11) | BIT(12) | BIT(13) | BIT(14) | BIT(15))

  if (verbose)
    printf ("  %s()\n", __FUNCTION__);

  vals -= 1;

  if ((ret = unw_get_reg (c, UNW_IA64_PR, &pr)) < 0)
    panic ("%s: failed to read register pr, error=%d\n", __FUNCTION__, ret);

  pr &= ~DONTCARE;
  expected = (vals[0] & ~DONTCARE) | 1;

  if (verbose)
    printf ("    pr = %016lx (expected %016lx)\n", pr, expected);

  if (pr != expected)
    panic ("%s: pr=%lx instead of %lx!\n", __FUNCTION__, pr, expected);

  if ((ret = unw_set_reg (c, UNW_IA64_PR, vals[0])) < 0)
    panic ("%s: failed to write register pr, error=%d\n", __FUNCTION__, ret);

  if ((ret = unw_get_reg (c, UNW_IA64_PR, &pr)) < 0)
    panic ("%s: failed to read register pr, error=%d\n", __FUNCTION__, ret);

  if (pr != vals[0])
    panic ("%s: secondary pr=%lx instead of %lx!\n",
	   __FUNCTION__, pr, vals[0]);
  return vals;
}

static unw_word_t *
check_rotate_regs (unw_cursor_t *c, unw_word_t *vals)
{
  if (verbose)
    printf ("  %s()\n", __FUNCTION__);
  return check_pr (c, vals - 1);
}

static void
start_checks (void *funcs, unsigned long *vals)
{
  unw_context_t uc;
  unw_cursor_t c;
  int i, ret;

  disable_sighandler ();

  unw_getcontext (&uc);

  if ((ret = unw_init_local (&c, &uc)) < 0)
    panic ("%s: unw_init_local (ret=%d)\n", __FUNCTION__, ret);

  if ((ret = unw_step (&c)) < 0)
    panic ("%s: unw_step (ret=%d)\n", __FUNCTION__, ret);

  for (i = 0; i < num_checks; ++i)
    {
      vals = (*checks[num_checks - 1 - i]) (&c, vals);

      if ((ret = unw_step (&c)) < 0)
	panic ("%s: unw_step (ret=%d)\n", __FUNCTION__, ret);
    }
}

static void
run_check (int test)
{
  int index, i;

  if (test == 1)
    /* Make first test always go the full depth... */
    num_checks = MAX_CHECKS;
  else
    num_checks = (random () % MAX_CHECKS) + 1;

  for (i = 0; i < num_checks * MAX_VALUES_PER_FUNC; ++i)
    values[i] = random_word ();

  for (i = 0; i < num_checks; ++i)
    {
      if (test == 1)
	/* Make first test once go through each test... */
	index = i % (int) ARRAY_SIZE (all_funcs);
      else
	index = random () % (int) ARRAY_SIZE (all_funcs);
      funcs[i] = all_funcs[index].func;
      checks[i] = all_funcs[index].check;
    }

  funcs[num_checks] = start_checks;

  enable_sighandler ();
  (*funcs[0]) (funcs + 1, values);
}

int
main (int argc, char **argv)
{
  int i;

  if (argc > 1)
    verbose = 1;

  for (i = 0; i < NUM_RUNS; ++i)
    {
      if (verbose)
	printf ("Run %d\n", i + 1);
      run_check (i + 1);
    }

  if (nerrors > 0)
    {
      fprintf (stderr, "FAILURE: detected %d errors\n", nerrors);
      exit (-1);
    }
  if (verbose)
    printf ("SUCCESS.\n");
  return 0;
}
