/* libunwind - a platform-independent unwind library
   Copyright (C) 2009 Google, Inc
	Contributed by Arun Sharma <arun.sharma@google.com>

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

#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <dlfcn.h>
#include <libunwind.h>

#define panic(args...)				\
	{ fprintf (stderr, args); exit (-1); }

int verbose;
int num_errors;
int in_unwind;

void *
malloc(size_t s)
{
  static void * (*func)();

  if(!func)
    func = (void *(*)()) dlsym(RTLD_NEXT, "malloc");

  if (in_unwind) {
    num_errors++;
    return NULL;
  } else {
    return func(s);
  }
}

static void
do_backtrace (void)
{
  unw_word_t ip, sp;
  unw_cursor_t cursor;
  unw_context_t uc;
  int ret;

  in_unwind = 1;
  unw_getcontext (&uc);
  if (unw_init_local (&cursor, &uc) < 0)
    panic ("unw_init_local failed!\n");

  do
    {
      unw_get_reg (&cursor, UNW_REG_IP, &ip);
      unw_get_reg (&cursor, UNW_REG_SP, &sp);

      ret = unw_step (&cursor);
      if (ret < 0)
	{
	  ++num_errors;
	}
    }
  while (ret > 0);
  in_unwind = 0;
}

void
foo3 (void)
{
  do_backtrace ();
}

void
foo2 (void)
{
  foo3 ();
}

void
foo1 (void)
{
  foo2 ();
}

int
main (void)
{
  foo1();

  if (num_errors > 0)
    {
      fprintf (stderr, "FAILURE: detected %d errors\n", num_errors);
      exit (-1);
    }
  return 0;
}
