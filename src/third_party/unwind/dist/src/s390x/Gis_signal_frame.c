/* libunwind - a platform-independent unwind library
   Copyright (C) 2012 Tommi Rantala <tt.rantala@gmail.com>
   Copyright (C) 2013 Linaro Limited
   Copyright (C) 2017 IBM

   Modified for s390x by Michael Munday <mike.munday@ibm.com>

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

/* The restorer stub will be a system call:
   - rt_sigreturn: svc 173 (0x0aad)
   - sigreturn:    svc 119 (0x0a77)
*/

int
unw_is_signal_frame (unw_cursor_t *cursor)
{
#ifdef __linux__
  struct cursor *c = (struct cursor *) cursor;
  unw_word_t w0, ip;
  unw_addr_space_t as;
  unw_accessors_t *a;
  void *arg;
  int ret, shift = 48;

  as = c->dwarf.as;
  a = unw_get_accessors (as);
  arg = c->dwarf.as_arg;

  /* Align the instruction pointer to 8 bytes so that we guarantee
     an 8 byte read from it won't cross a page boundary.
     Instructions on s390x are 2 byte aligned.  */
  ip = c->dwarf.ip & ~7;
  shift -= (c->dwarf.ip - ip) * 8;

  ret = (*a->access_mem) (as, ip, &w0, 0, arg);
  if (ret < 0)
    return ret;

  /* extract first 2 bytes of the next instruction */
  w0 = (w0 >> shift) & 0xffff;

  /* sigreturn */
  if (w0 == 0x0a77)
    return 1;

  /* rt_sigreturn */
  if (w0 == 0x0aad)
    return 2;

  return 0;

#else
  return -UNW_ENOINFO;
#endif
}
