/* libunwind - a platform-independent unwind library
   Copyright (C) 2003-2004 Hewlett-Packard Co
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

/* This file tests corner-cases of unwinding across multiple stacks.
   In particular, it verifies that the extreme case with a frame of 96
   stacked registers that are all backed up by separate stacks works
   as expected.  */

#include <stdio.h>
#include <stdlib.h>

#include <libunwind.h>
#include "compiler.h"

#include "ia64-test-rbs.h"

#define panic(args...)							  \
	do { fprintf (stderr, args); ++nerrors; return -9999; } while (0)

/* The loadrs field in ar.rsc is 14 bits wide, which limits all ia64
   implementations to at most 2048 physical stacked registers
   (actually, slightly less than that, because loadrs also counts RNaT
   slots).  Since we can dirty 93 stacked registers per recursion, we
   need to recurse RECURSION_DEPTH times to ensure all physical
   stacked registers are in use. */
#define MAX_PHYS_STACKED	2048
#define RECURSION_DEPTH		((MAX_PHYS_STACKED + 92) / 93)

typedef int spill_func_t (long iteration, int (*next_func[])());

extern int loadup (long iteration, int *values, int (*next_func[])());
extern char resumption_point_label;

#define DCL(n) \
 extern int rbs_spill_##n (long iteration, int (*next_func[])())
	           DCL(2);  DCL(3);  DCL(4);  DCL(5);  DCL(6);  DCL(7);
  DCL(8);  DCL(9); DCL(10); DCL(11); DCL(12); DCL(13); DCL(14); DCL(15);
 DCL(16); DCL(17); DCL(18); DCL(19); DCL(20); DCL(21); DCL(22); DCL(23);
 DCL(24); DCL(25); DCL(26); DCL(27); DCL(28); DCL(29); DCL(30); DCL(31);
 DCL(32); DCL(33); DCL(34); DCL(35); DCL(36); DCL(37); DCL(38); DCL(39);
 DCL(40); DCL(41); DCL(42); DCL(43); DCL(44); DCL(45); DCL(46); DCL(47);
 DCL(48); DCL(49); DCL(50); DCL(51); DCL(52); DCL(53); DCL(54); DCL(55);
 DCL(56); DCL(57); DCL(58); DCL(59); DCL(60); DCL(61); DCL(62); DCL(63);
 DCL(64); DCL(65); DCL(66); DCL(67); DCL(68); DCL(69); DCL(70); DCL(71);
 DCL(72); DCL(73); DCL(74); DCL(75); DCL(76); DCL(77); DCL(78); DCL(79);
 DCL(80); DCL(81); DCL(82); DCL(83); DCL(84); DCL(85); DCL(86); DCL(87);
 DCL(88); DCL(89); DCL(90); DCL(91); DCL(92); DCL(93); DCL(94);

#define SPL(n)  rbs_spill_##n
spill_func_t *spill_funcs[] =
  {
	            SPL(2),   SPL(3),  SPL(4),  SPL(5),  SPL(6),  SPL(7),
     SPL(8),  SPL(9), SPL(10), SPL(11), SPL(12), SPL(13), SPL(14), SPL(15),
    SPL(16), SPL(17), SPL(18), SPL(19), SPL(20), SPL(21), SPL(22), SPL(23),
    SPL(24), SPL(25), SPL(26), SPL(27), SPL(28), SPL(29), SPL(30), SPL(31),
    SPL(32), SPL(33), SPL(34), SPL(35), SPL(36), SPL(37), SPL(38), SPL(39),
    SPL(40), SPL(41), SPL(42), SPL(43), SPL(44), SPL(45), SPL(46), SPL(47),
    SPL(48), SPL(49), SPL(50), SPL(51), SPL(52), SPL(53), SPL(54), SPL(55),
    SPL(56), SPL(57), SPL(58), SPL(59), SPL(60), SPL(61), SPL(62), SPL(63),
    SPL(64), SPL(65), SPL(66), SPL(67), SPL(68), SPL(69), SPL(70), SPL(71),
    SPL(72), SPL(73), SPL(74), SPL(75), SPL(76), SPL(77), SPL(78), SPL(79),
    SPL(80), SPL(81), SPL(82), SPL(83), SPL(84), SPL(85), SPL(86), SPL(87),
    SPL(88), SPL(89), SPL(90), SPL(91), SPL(92), SPL(93), SPL(94)
  };

static int verbose;
static int nerrors;
static int unwind_count;

static int
unwind_and_resume (long iteration, int (*next_func[])())
{
  unw_context_t uc;
  unw_cursor_t c;
  unw_word_t ip;
  int i, ret;

  if (verbose)
    printf (" %s(iteration=%ld, next_func=%p)\n",
	    __FUNCTION__, iteration, next_func);

  unw_getcontext (&uc);
  if ((ret = unw_init_local (&c, &uc)) < 0)
    panic ("unw_init_local (ret=%d)", ret);

  for (i = 0; i < unwind_count; ++i)
    if ((ret = unw_step (&c)) < 0)
      panic ("unw_step (ret=%d)", ret);

  if (unw_get_reg (&c, UNW_REG_IP, &ip) < 0
      || unw_set_reg (&c, UNW_REG_IP, (unw_word_t) &resumption_point_label) < 0
      || unw_set_reg (&c, UNW_REG_EH + 0, 0)	/* ret val */
      || unw_set_reg (&c, UNW_REG_EH + 1, ip))
    panic ("failed to redirect to resumption_point\n");

  if (verbose)
    {
      unw_word_t bsp;
      if (unw_get_reg (&c, UNW_IA64_BSP, &bsp) < 0)
	panic ("unw_get_reg() failed\n");
      printf ("  bsp=%lx, old ip=%lx, new ip=%p\n", bsp,
	      ip, &resumption_point_label);
    }

  ret = unw_resume (&c);
  panic ("unw_resume() returned (ret=%d)!!\n", ret);
  return 0;
}

static int
run_check (int test)
{
  int nfuncs, nspills, n, ret, i, reg_values[88];
  spill_func_t *func[NSTACKS + 1];

  /* First, generate a set of 88 random values which loadup() will load
     into loc2-loc89 (r37-r124).  */
  for (i = 0; i < (int) ARRAY_SIZE (reg_values); ++i)
    {
      reg_values[i] = random ();
      /* Generate NaTs with a reasonably probability (1/16th): */
      if (reg_values[i] < 0x10000000)
	reg_values[i] = 0;
    }

  nspills = 0;
  nfuncs = 0;
  do
    {
      n = random () % (int) ARRAY_SIZE (spill_funcs);
      func[nfuncs++] = spill_funcs[n];
      nspills += 2 + n;
    }
  while (nspills < 128);
  func[nfuncs++] = unwind_and_resume;

  unwind_count = 1 + (random () % (nfuncs + RECURSION_DEPTH - 1));

  if (verbose)
    printf ("test%d: nfuncs=%d, unwind_count=%d\n",
	    test, nfuncs, unwind_count);

  ret = loadup (RECURSION_DEPTH, reg_values, func);
  if (ret < 0)
    panic ("test%d: load() returned %d\n", test, ret);
  else if (ret != RECURSION_DEPTH + nfuncs - unwind_count)
    panic ("test%d: resumed wrong frame: expected %d, got %d\n",
	   test, RECURSION_DEPTH + nfuncs - unwind_count, ret);
  return 0;
}

int
main (int argc, char **argv)
{
  int i;

  if (argc > 1)
    verbose = 1;

  for (i = 0; i < 100000; ++i)
    run_check (i + 1);

  if (nerrors > 0)
    {
      fprintf (stderr, "FAILURE: detected %d errors\n", nerrors);
      exit (-1);
    }
  if (verbose)
    printf ("SUCCESS.\n");
  return 0;
}
