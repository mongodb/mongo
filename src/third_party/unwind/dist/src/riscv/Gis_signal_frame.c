/* libunwind - a platform-independent unwind library
   Copyright (C) 2008 CodeSourcery
   Copyright (C) 2021 Zhaofeng Li

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

#ifdef __linux__

/*
  The stub looks like:

  addi x17, zero, 139    0x08b00893
  ecall                  0x00000073

  See <https://github.com/torvalds/linux/blob/44db63d1ad8d71c6932cbe007eb41f31c434d140/arch/riscv/kernel/vdso/rt_sigreturn.S>.
*/
#define SIGRETURN_I0 0x08b00893
#define SIGRETURN_I1 0x00000073

#endif /* __linux__ */

int
unw_is_signal_frame (unw_cursor_t *cursor)
{
#ifdef __linux__
  struct cursor *c = (struct cursor*) cursor;
  unw_word_t i0, i1, ip;
  unw_addr_space_t as;
  unw_accessors_t *a;
  void *arg;
  int ret;

  as = c->dwarf.as;
  a = unw_get_accessors_int (as);
  arg = c->dwarf.as_arg;

  ip = c->dwarf.ip;

  if (!ip || !a->access_mem || (ip & (sizeof(unw_word_t) - 1)))
    return 0;

  if ((ret = (*a->access_mem) (as, ip, &i0, 0, arg)) < 0)
    return ret;

  if ((ret = (*a->access_mem) (as, ip + 4, &i1, 0, arg)) < 0)
    return ret;

  if ((i0 & 0xffffffff) == SIGRETURN_I0 && (i1 & 0xffffffff) == SIGRETURN_I1)
    {
      Debug (8, "cursor at signal frame\n");
      return 1;
    }

  return 0;
#else
  return -UNW_ENOINFO;
#endif
}
