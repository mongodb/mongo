/* libunwind - a platform-independent unwind library
   Copyright (C) 2004 Hewlett-Packard Co
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

#include <stdio.h>

#include <libunwind.h>

extern int verbose;

static void *funcs[] =
  {
    (void *) &unw_get_reg,
    (void *) &unw_get_fpreg,
    (void *) &unw_set_reg,
    (void *) &unw_set_fpreg,
    (void *) &unw_resume,
    (void *) &unw_create_addr_space,
    (void *) &unw_destroy_addr_space,
    (void *) &unw_get_accessors,
    (void *) &unw_flush_cache,
    (void *) &unw_set_caching_policy,
    (void *) &unw_set_cache_size,
    (void *) &unw_regname,
    (void *) &unw_get_proc_info,
    (void *) &unw_get_save_loc,
    (void *) &unw_is_signal_frame,
    (void *) &unw_get_proc_name
  };

int
test_generic (void)
{
  if (verbose)
    printf (__FILE__": funcs[0]=%p\n", funcs[0]);

#ifndef UNW_REMOTE_ONLY
  {
    unw_context_t uc;
    unw_cursor_t c;

    unw_getcontext (&uc);
    unw_init_local (&c, &uc);
    unw_init_remote (&c, unw_local_addr_space, &uc);

    return unw_step (&c);
  }
#else
  return 0;
#endif
}
