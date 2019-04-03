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

/* This file tests dynamic code-generation via function-cloning.  */

#include "flush-cache.h"

#include "compiler.h"

#include <libunwind.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/mman.h>

#if UNW_TARGET_ARM
#define MAX_FUNC_SIZE   96 	/* FIXME: arch/compiler dependent */
#else
#define MAX_FUNC_SIZE	2048	/* max. size of cloned function */
#endif

#define panic(args...)				\
	{ fprintf (stderr, args); exit (-1); }

typedef void (*template_t) (int, void (*)(),
			    int (*)(const char *, ...), const char *,
			    const char **);

int verbose;

static const char *strarr[] =
  {
    "i", "ii", "iii", "iv", "v", "vi", "vii", "viii", "ix", "x", NULL
  };

#ifdef __ia64__
struct fdesc
  {
    long code;
    long gp;
  };
# define get_fdesc(fdesc,func)	(fdesc = *(struct fdesc *) &(func))
# define get_funcp(fdesc)	((template_t) &(fdesc))
# define get_gp(fdesc)		((fdesc).gp)
#elif __arm__
struct fdesc
  {
    long code;
    long is_thumb;
  };
/* Workaround GCC bug: https://bugs.launchpad.net/gcc-linaro/+bug/721531 */
# define get_fdesc(fdesc,func)  ({long tmp = (long) &(func); \
                                  (fdesc).code = (long) &(func) & ~0x1; \
                                  (fdesc).is_thumb = tmp & 0x1;})
/*# define get_fdesc(fdesc,func)  ({(fdesc).code = (long) &(func) & ~0x1; \
                                  (fdesc).is_thumb = (long) &(func) & 0x1;})*/
# define get_funcp(fdesc)       ((template_t) ((fdesc).code | (fdesc).is_thumb))
# define get_gp(fdesc)          (0)
#else
struct fdesc
  {
    long code;
  };
# define get_fdesc(fdesc,func)	(fdesc.code = (long) &(func))
# define get_funcp(fdesc)	((template_t) (fdesc).code)
# define get_gp(fdesc)		(0)
#endif

void
template (int i, template_t self,
	  int (*printer)(const char *, ...), const char *fmt, const char **arr)
{
  (*printer) (fmt, arr[11 - i][0], arr[11 - i] + 1);
  if (i > 0)
    (*self) (i - 1, self, printer, fmt, arr);
}

static void
sighandler (int signal)
{
  unw_cursor_t cursor;
  char name[128], off[32];
  unw_word_t ip, offset;
  unw_context_t uc;
  int count;

  if (verbose)
    printf ("caught signal %d\n", signal);

  unw_getcontext (&uc);
  unw_init_local (&cursor, &uc);

  count = 0;
  while (!unw_is_signal_frame (&cursor))
    {
      if (unw_step (&cursor) < 0)
	panic ("failed to find signal frame!\n");

      if (count++ > 20)
	{
	  panic ("Too many steps to the signal frame (%d)\n", count);
	  break;
	}
    }
  unw_step (&cursor);

  count = 0;
  do
    {
      unw_get_reg (&cursor, UNW_REG_IP, &ip);
      name[0] = '\0';
      off[0] = '\0';
      if (unw_get_proc_name (&cursor, name, sizeof (name), &offset) == 0
	  && offset > 0)
	snprintf (off, sizeof (off), "+0x%lx", (long) offset);
      if (verbose)
	printf ("ip = %lx <%s%s>\n", (long) ip, name, off);
      ++count;

      if (count > 20)
	{
	  panic ("Too many steps (%d)\n", count);
	  break;
	}

    }
  while (unw_step (&cursor) > 0);

  if (count != 13)
    panic ("FAILURE: expected 13, not %d frames below signal frame\n", count);

  if (verbose)
    printf ("SUCCESS\n");
  exit (0);
}

int
dev_null (const char *format UNUSED, ...)
{
  return 0;
}

int
main (int argc, char *argv[] UNUSED)
{
  unw_dyn_region_info_t *region;
  unw_dyn_info_t di;
  struct fdesc fdesc;
  template_t funcp;
  void *mem;

  if (argc > 1)
    ++verbose;

  mem = malloc (getpagesize ());

  get_fdesc (fdesc, template);

  if (verbose)
    printf ("old code @ %p, new code @ %p\n", (void *) fdesc.code, mem);

  memcpy (mem, (void *) fdesc.code, MAX_FUNC_SIZE);
  mprotect ((void *) ((long) mem & ~(getpagesize () - 1)),
	    2*getpagesize(), PROT_READ | PROT_WRITE | PROT_EXEC);

  flush_cache (mem, MAX_FUNC_SIZE);

  signal (SIGSEGV, sighandler);

  /* register the new function: */
  region = alloca (_U_dyn_region_info_size (2));
  region->next = NULL;
  region->insn_count = 3 * (MAX_FUNC_SIZE / 16);
  region->op_count = 2;
  _U_dyn_op_alias (&region->op[0], 0, -1, fdesc.code);
  _U_dyn_op_stop (&region->op[1]);

  memset (&di, 0, sizeof (di));
  di.start_ip = (long) mem;
  di.end_ip = (long) mem + 16*region->insn_count/3;
  di.gp = get_gp (fdesc);
  di.format = UNW_INFO_FORMAT_DYNAMIC;
  di.u.pi.name_ptr = (unw_word_t) "copy_of_template";
  di.u.pi.regions = region;

  _U_dyn_register (&di);

  /* call new function: */
  fdesc.code = (long) mem;
  funcp = get_funcp (fdesc);

  if (verbose)
    (*funcp) (10, funcp, printf, "iteration %c%s\n", strarr);
  else
    (*funcp) (10, funcp, dev_null, "iteration %c%s\n", strarr);

  _U_dyn_cancel (&di);
  return -1;
}
