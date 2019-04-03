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

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <libunwind.h>
#include "compiler.h"

#include <sys/resource.h>
#include <sys/time.h>

#define panic(args...)							  \
	do { fprintf (stderr, args); exit (-1); } while (0)

long dummy;

static long iterations = 10000;
static int maxlevel = 100;

#define KB	1024
#define MB	(1024*1024)

static char big[64*MB];	/* should be >> max. cache size */

static inline double
gettime (void)
{
  struct timeval tv;

  gettimeofday (&tv, NULL);
  return tv.tv_sec + 1e-6*tv.tv_usec;
}

static int NOINLINE
measure_unwind (int maxlevel, double *step)
{
  double stop, start;
  int level = 0;
  void *buffer[128];

  start = gettime ();
  level = unw_backtrace(buffer, 128);
  stop = gettime ();

  if (level <= maxlevel)
    panic ("Unwound only %d levels, expected at least %d levels\n",
	   level, maxlevel);

  *step = (stop - start) / (double) level;
  return 0;
}

static int f1 (int, int, double *);

static int NOINLINE
g1 (int level, int maxlevel, double *step)
{
  if (level == maxlevel)
    return measure_unwind (maxlevel, step);
  else
    /* defeat last-call/sibcall optimization */
    return f1 (level + 1, maxlevel, step) + level;
}

static int NOINLINE
f1 (int level, int maxlevel, double *step)
{
  if (level == maxlevel)
    return measure_unwind (maxlevel, step);
  else
    /* defeat last-call/sibcall optimization */
    return g1 (level + 1, maxlevel, step) + level;
}

static void
doit (const char *label)
{
  double step, min_step, first_step, sum_step;
  int i;

  sum_step = first_step = 0.0;
  min_step = 1e99;
  for (i = 0; i < iterations; ++i)
    {
      f1 (0, maxlevel, &step);

      sum_step += step;

      if (step < min_step)
	min_step = step;

      if (i == 0)
	first_step = step;
    }
  printf ("%s: unw_step : 1st=%9.3f min=%9.3f avg=%9.3f nsec\n", label,
	  1e9*first_step, 1e9*min_step, 1e9*sum_step/iterations);
}

static long
sum (void *buf, size_t size)
{
  long s = 0;
  char *cp = buf;
  size_t i;

  for (i = 0; i < size; i += 8)
    s += cp[i];
  return s;
}

static void
measure_init (void)
{
# define N	100
# define M	10	/* must be at least 2 to get steady-state */
  double stop, start, get_cold, get_warm, init_cold, init_warm, delta;
  struct
    {
      unw_cursor_t c;
      char padding[1024];	/* should be > 2 * max. cacheline size */
    }
  cursor[N];
  struct
    {
      unw_context_t uc;
      char padding[1024];	/* should be > 2 * max. cacheline size */
    }
  uc[N];
  int i, j;

  /* Run each test M times and take the minimum to filter out noise
     such dynamic linker resolving overhead, context-switches,
     page-in, cache, and TLB effects.  */

  get_cold = 1e99;
  for (j = 0; j < M; ++j)
    {
      dummy += sum (big, sizeof (big));			/* flush the cache */
      for (i = 0; i < N; ++i)
	uc[i].padding[511] = i;		/* warm up the TLB */
      start = gettime ();
      for (i = 0; i < N; ++i)
	unw_getcontext (&uc[i].uc);
      stop = gettime ();
      delta = (stop - start) / N;
      if (delta < get_cold)
	get_cold = delta;
    }

  init_cold = 1e99;
  for (j = 0; j < M; ++j)
    {
      dummy += sum (big, sizeof (big));	/* flush cache */
      for (i = 0; i < N; ++i)
	uc[i].padding[511] = i;		/* warm up the TLB */
      start = gettime ();
      for (i = 0; i < N; ++i)
	unw_init_local (&cursor[i].c, &uc[i].uc);
      stop = gettime ();
      delta = (stop - start) / N;
      if (delta < init_cold)
	init_cold = delta;
    }

  get_warm = 1e99;
  for (j = 0; j < M; ++j)
    {
      start = gettime ();
      for (i = 0; i < N; ++i)
	unw_getcontext (&uc[0].uc);
      stop = gettime ();
      delta = (stop - start) / N;
      if (delta < get_warm)
	get_warm = delta;
    }

  init_warm = 1e99;
  for (j = 0; j < M; ++j)
    {
      start = gettime ();
      for (i = 0; i < N; ++i)
	unw_init_local (&cursor[0].c, &uc[0].uc);
      stop = gettime ();
      delta = (stop - start) / N;
      if (delta < init_warm)
	init_warm = delta;
    }

  printf ("unw_getcontext : cold avg=%9.3f nsec, warm avg=%9.3f nsec\n",
	  1e9 * get_cold, 1e9 * get_warm);
  printf ("unw_init_local : cold avg=%9.3f nsec, warm avg=%9.3f nsec\n",
	  1e9 * init_cold, 1e9 * init_warm);
}

int
main (int argc, char **argv)
{
  struct rlimit rlim;

  rlim.rlim_cur = RLIM_INFINITY;
  rlim.rlim_max = RLIM_INFINITY;
  setrlimit (RLIMIT_STACK, &rlim);

  memset (big, 0xaa, sizeof (big));

  if (argc > 1)
    {
      maxlevel = atol (argv[1]);
      if (argc > 2)
	iterations = atol (argv[2]);
    }

  measure_init ();

  doit ("default         ");

  unw_set_caching_policy (unw_local_addr_space, UNW_CACHE_NONE);
  doit ("no cache        ");

  unw_set_caching_policy (unw_local_addr_space, UNW_CACHE_GLOBAL);
  doit ("global cache    ");

  unw_set_caching_policy (unw_local_addr_space, UNW_CACHE_PER_THREAD);
  doit ("per-thread cache");

  return 0;
}
