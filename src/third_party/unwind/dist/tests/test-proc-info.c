/* libunwind - a platform-independent unwind library
   Copyright (C) 2003 Hewlett-Packard Co
	Contributed by David Mosberger-Tang <davidm@hpl.hp.com>

This file is part of libunwind.

Copyright (c) 2003 Hewlett-Packard Co.

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

/* This test program checks whether proc_info lookup failures are
   cached.  They must NOT be cached because it could otherwise turn
   temporary failures into permanent ones.  Furthermore, we allow apps
   to return -UNW_ESTOPUNWIND to terminate unwinding (though this
   feature is deprecated and dynamic unwind info should be used
   instead).  */

#include <stdio.h>
#include <string.h>

#include <libunwind.h>
#include "compiler.h"

int errors;

#define panic(args...)					\
	{ ++errors; fprintf (stderr, args); return -1; }

static int
find_proc_info (unw_addr_space_t as UNUSED,
                unw_word_t ip UNUSED,
                unw_proc_info_t *pip UNUSED,
                int need_unwind_info UNUSED,
                void *arg UNUSED)
{
  return -UNW_ESTOPUNWIND;
}

static int
access_mem (unw_addr_space_t as UNUSED,
            unw_word_t addr UNUSED,
            unw_word_t *valp, int write,
            void *arg UNUSED)
{
  if (!write)
    *valp = 0;
  return 0;
}

static int
access_reg (unw_addr_space_t as UNUSED,
            unw_regnum_t regnum UNUSED,
            unw_word_t *valp, int write,
            void *arg UNUSED)
{
  if (!write)
    *valp = 32;
  return 0;
}

static int
access_fpreg (unw_addr_space_t as UNUSED,
              unw_regnum_t regnum UNUSED,
              unw_fpreg_t *valp, int write,
              void *arg UNUSED)
{
  if (!write)
    memset (valp, 0, sizeof (*valp));
  return 0;
}

static int
get_dyn_info_list_addr (unw_addr_space_t as UNUSED,
                        unw_word_t *dilap UNUSED,
                        void *arg UNUSED)
{
  return -UNW_ENOINFO;
}

static void
put_unwind_info (unw_addr_space_t as UNUSED,
                 unw_proc_info_t *pi UNUSED,
                 void *arg UNUSED)
{
  ++errors;
  fprintf (stderr, "%s() got called!\n", __FUNCTION__);
}

static int
resume (unw_addr_space_t as UNUSED,
        unw_cursor_t *reg UNUSED,
        void *arg UNUSED)
{
  panic ("%s() got called!\n", __FUNCTION__);
}

static int
get_proc_name (unw_addr_space_t as UNUSED,
               unw_word_t ip UNUSED,
               char *buf UNUSED,
               size_t buf_len UNUSED,
               unw_word_t *offp UNUSED,
               void *arg UNUSED)
{
  panic ("%s() got called!\n", __FUNCTION__);
}

int
main (int argc, char **argv)
{
  unw_accessors_t acc;
  unw_addr_space_t as;
  int ret, verbose = 0;
  unw_cursor_t c;

  if (argc > 1 && strcmp (argv[1], "-v") == 0)
    verbose = 1;

  memset (&acc, 0, sizeof (acc));
  acc.find_proc_info = find_proc_info;
  acc.put_unwind_info = put_unwind_info;
  acc.get_dyn_info_list_addr = get_dyn_info_list_addr;
  acc.access_mem = access_mem;
  acc.access_reg = access_reg;
  acc.access_fpreg = access_fpreg;
  acc.resume = resume;
  acc.get_proc_name = get_proc_name;

  as = unw_create_addr_space (&acc, 0);
  if (!as)
    panic ("unw_create_addr_space() failed\n");

  unw_set_caching_policy (as, UNW_CACHE_GLOBAL);

  ret = unw_init_remote (&c, as, NULL);
  if (ret < 0)
    panic ("unw_init_remote() returned %d instead of 0\n", ret);

  ret = unw_step (&c);
  if (ret != -UNW_ESTOPUNWIND)
    panic ("First call to unw_step() returned %d instead of %d\n",
	   ret, -UNW_ESTOPUNWIND);

  ret = unw_step (&c);
  if (ret != -UNW_ESTOPUNWIND)
    panic ("Second call to unw_step() returned %d instead of %d\n",
	   ret, -UNW_ESTOPUNWIND);

  unw_destroy_addr_space (as);

  if (verbose)
    printf ("SUCCESS\n");
  return 0;
}
