/* libunwind - a platform-independent unwind library
   Copyright (C) 2002 Hewlett-Packard Co
        Contributed by David Mosberger-Tang <davidm@hpl.hp.com>

   Modified for x86_64 by Max Asbock <masbock@us.ibm.com>
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

static inline int
common_init (struct cursor *c, unsigned use_prev_instr)
{
  int ret;
  int i;

  for (i = UNW_S390X_R0; i <= UNW_S390X_R15; ++i) {
    c->dwarf.loc[i] = DWARF_REG_LOC(&c->dwarf, i);
  }
  for (i = UNW_S390X_F0; i <= UNW_S390X_F15; ++i) {
    c->dwarf.loc[i] = DWARF_FPREG_LOC(&c->dwarf, i);
  }
  /* IP isn't a real register, it is encoded in the PSW */
  c->dwarf.loc[UNW_S390X_IP] = DWARF_REG_LOC(&c->dwarf, UNW_S390X_IP);

  ret = dwarf_get (&c->dwarf, c->dwarf.loc[UNW_S390X_IP], &c->dwarf.ip);
  if (ret < 0)
    return ret;

  /* Normally the CFA offset on s390x is biased, however this is taken
     into account by the CFA offset in dwarf_step, so here we just mark
     make it equal to the stack pointer.  */
  ret = dwarf_get (&c->dwarf, DWARF_REG_LOC (&c->dwarf, UNW_S390X_R15),
                   &c->dwarf.cfa);
  if (ret < 0)
    return ret;

  c->sigcontext_format = S390X_SCF_NONE;
  c->sigcontext_addr = 0;

  c->dwarf.args_size = 0;
  c->dwarf.stash_frames = 0;
  c->dwarf.use_prev_instr = use_prev_instr;
  c->dwarf.pi_valid = 0;
  c->dwarf.pi_is_dynamic = 0;
  c->dwarf.hint = 0;
  c->dwarf.prev_rs = 0;
  c->dwarf.eh_valid_mask = 0;

  return 0;
}
