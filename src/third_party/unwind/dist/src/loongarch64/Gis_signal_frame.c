/* libunwind - a platform-independent unwind library
   Copyright (C) 2008 CodeSourcery
   Copyright (C) 2021 Loongson Technology Corporation Limited

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
#include <stdio.h>

/*
  arch/loongarch/vdso/sigreturn.S:

  0000000000000000 <__vdso_rt_sigreturn>:
     0:	03822c0b 	ori	$r11,$r0,0x8b
     4:	002b0000 	syscall	0x0
*/

#define SIGRETURN_W0    0x03822c0b
#define SIGRETURN_W1    0x002b0000

int
unw_is_signal_frame (unw_cursor_t *cursor)
{
  struct cursor *c = (struct cursor *) cursor;
  unw_word_t w0, w1, ip;
  unw_addr_space_t as;
  unw_accessors_t *a;
  void *arg;
  int ret;

  as = c->dwarf.as;
  a = unw_get_accessors (as);
  arg = c->dwarf.as_arg;

  ip = c->dwarf.ip;

  /* syscall 0x0 */
  if ((ret = (*a->access_mem) (as, ip + 4, &w1, 0, arg)) < 0)
    return ret;

  /* ori a7,zero,0x8b */
  if ((ret = (*a->access_mem) (as, ip, &w0, 0, arg)) < 0)
    return ret;

  if ((w1 & 0xffffffff) == SIGRETURN_W1 && (w0 & 0xffffffff) == SIGRETURN_W0)
    {
      Debug (8, "cursor at signal frame\n");
      return 1;
    }

    return 0;
}
