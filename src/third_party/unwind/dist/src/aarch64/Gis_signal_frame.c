/* libunwind - a platform-independent unwind library
   Copyright (C) 2012 Tommi Rantala <tt.rantala@gmail.com>
   Copyright (C) 2013 Linaro Limited
   Copyright 2022-2023 Blackberry Limited.

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

#include "unwind_i.h"

#if defined(__linux__)
/*
 * The restorer stub will always have the form:
 *
 *  d2801168        movz    x8, #0x8b
 *  d4000001        svc     #0x0
 */
# define SIGNAL_RETURN 0xd4000001d2801168
#elif defined(__FreeBSD__)
/*
 * The restorer stub will always have the form:
 *
 *  910003e0	mov     x0, sp
 *  91014000	add     x0, x0, #SF_UC
 *	d2803428	mov     x8, #SYS_sigreturn
 *	d4000001	svc     0
 */
# define SIGNAL_RETURN 0x91014000910003e0
#elif defined(__QNX__)
/*
 * The restorer stub will always have the form:
 *
 *  f9400260        ldr  x0, [x19]
 *  d63f0060        blr  x3
 *  aa1303e0        mov  x0, x19
 *  14xxxxxx        b    SignalReturn@plt
 */
# define SIGNAL_RETURN 0x14000000aa1303e0
# define HANDLER_CALL  0xd63f0060f9400260
#endif

int
unw_is_signal_frame (unw_cursor_t *cursor)
{
#if defined(__linux__) || defined(__FreeBSD__) || defined(__QNX__)
  struct cursor *c = (struct cursor *) cursor;
  unw_word_t w0, ip;
  unw_addr_space_t as;
  unw_accessors_t *a;
  void *arg;
  int ret;

  as = c->dwarf.as;
  a = unw_get_accessors_int (as);
  arg = c->dwarf.as_arg;

  ip = c->dwarf.ip;

  ret = (*a->access_mem) (as, ip, &w0, 0, arg);
  if (ret < 0)
    return ret;

  if ((w0 & SIGNAL_RETURN) != SIGNAL_RETURN)
  	return 0;
#if defined(__FreeBSD__)
  ip += 8;
  /*
   */
  ret = (*a->access_mem) (as, ip, &w0, 0, arg);
  if (ret < 0)
    return ret;
  if (w0 != 0xd4000001d2803428)
    return 0;
#elif defined(__QNX__)
  unw_word_t w1 = 0;
  ret = (*a->access_mem) (as, ip-sizeof(w1), &w1, 0, arg);
  if (ret < 0)
    return ret;

  if ((w1 & HANDLER_CALL) != HANDLER_CALL)
  	{
  	  return 0;
	}

#endif

  return 1;

#else
  return -UNW_ENOINFO;
#endif
}
