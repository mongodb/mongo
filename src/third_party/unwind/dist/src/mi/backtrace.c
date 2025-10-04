/* libunwind - a platform-independent unwind library
   Copyright (C) 2001-2002 Hewlett-Packard Co
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

#if !defined(UNW_REMOTE_ONLY) && !defined(UNW_LOCAL_ONLY)
#define UNW_LOCAL_ONLY

#include <libunwind.h>
#include <libunwind_i.h>
#include <string.h>

/* See glibc manual for a description of this function.  */

static ALWAYS_INLINE int
slow_backtrace (void **buffer, int size, unw_context_t *uc, int flag)
{
  unw_cursor_t cursor;
  unw_word_t ip;
  int n = 0;

  if (unlikely (unw_init_local2 (&cursor, uc, flag) < 0))
    return 0;


  while (unw_step (&cursor) > 0)
    {
      if (n >= size)
        return n;

      if (unw_get_reg (&cursor, UNW_REG_IP, &ip) < 0)
        return n;
      buffer[n++] = (void *) (uintptr_t) ip;
    }
  return n;
}

int
unw_backtrace (void **buffer, int size)
{
  unw_cursor_t cursor;
  unw_context_t uc;
  int n = size;

  tdep_getcontext_trace (&uc);

  if (unlikely (unw_init_local (&cursor, &uc) < 0))
    return 0;

  if (unlikely (tdep_trace (&cursor, buffer, &n) < 0))
    {
      unw_getcontext (&uc);
      return slow_backtrace (buffer, size, &uc, 0);
    }

  return n;
}

int
unw_backtrace2 (void **buffer, int size, unw_context_t* uc2, int flag)
{
  if (size == 0)
    return 0;
    
  if (uc2 == NULL)
    return unw_backtrace(buffer, size);

  unw_cursor_t cursor;
  // need to copy, because the context will be modified by tdep_trace
  unw_context_t uc = *(unw_context_t*)uc2;

  if (unlikely (unw_init_local2 (&cursor, &uc, flag) < 0))
    return 0;

  // get the first ip from the context
  unw_word_t ip;

  if (unw_get_reg (&cursor, UNW_REG_IP, &ip) < 0)
    return 0;

  buffer[0] =  (void *) (uintptr_t)ip;

  // update buffer info to collect the rest of the IPs
  buffer = buffer+1;
  int remaining_size = size-1;

  int n = remaining_size;

  // returns the number of frames collected by tdep_trace or slow_backtrace
  // and add 1 to it (the one we retrieved above)
  if (unlikely (tdep_trace (&cursor, buffer, &n) < 0))
    {
      return slow_backtrace (buffer, remaining_size, &uc, flag) + 1;
    }

  return n + 1;
}

#ifdef CONFIG_WEAK_BACKTRACE
extern int backtrace (void **buffer, int size)
  WEAK ALIAS(unw_backtrace);
#endif

#endif /* !UNW_REMOTE_ONLY */
