/* libunwind - a platform-independent unwind library
   Copyright (C) 2008 CodeSourcery

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
#include "offsets.h"
#include <ucontext.h>

#ifndef UNW_REMOTE_ONLY

HIDDEN inline int
riscv_local_resume (unw_addr_space_t as, unw_cursor_t *cursor, void *arg)
{
#ifdef __linux__
  struct cursor *c = (struct cursor *) cursor;
  ucontext_t *uc = c->uc;

  unw_word_t *mcontext = (unw_word_t*) &uc->uc_mcontext;
  mcontext[0] = c->dwarf.ip;

  if (c->sigcontext_format == RISCV_SCF_NONE)
    {
      /* Restore PC in RA */
      mcontext[1] = c->dwarf.ip;

      Debug (8, "resuming at ip=0x%lx via setcontext()\n", c->dwarf.ip);

      setcontext(uc);
    }
  else
    {
      struct sigcontext *sc = (struct sigcontext *) c->sigcontext_addr;
      unw_word_t *regs = (unw_word_t*)sc;

      regs[0] = c->dwarf.ip;
      for (int i = UNW_RISCV_X1; i <= UNW_RISCV_F31; ++i) {
        regs[i] = mcontext[i];
      }

      Debug (8, "resuming at ip=0x%lx via sigreturn() (trampoline @ 0x%lx, sp @ 0x%lx)\n", c->dwarf.ip, c->sigcontext_pc, c->sigcontext_sp);

      // Jump back to the trampoline
      __asm__ __volatile__ (
        "mv sp, %0\n"
        "jr %1 \n"
        : : "r" (c->sigcontext_sp), "r" (c->sigcontext_pc)
      );
    }

  unreachable();
#else
# warning Implement me
#endif
  return -UNW_EINVAL;
}

#endif /* !UNW_REMOTE_ONLY */

static inline int
establish_machine_state (struct cursor *c)
{
  unw_addr_space_t as = c->dwarf.as;
  void *arg = c->dwarf.as_arg;
  unw_fpreg_t fpval;
  unw_word_t val;
  int reg;

  Debug (8, "copying out cursor state\n");

  for (reg = UNW_RISCV_X1; reg <= UNW_REG_LAST; ++reg)
    {
      Debug (16, "copying %s %d\n", unw_regname (reg), reg);
      if (unw_is_fpreg (reg))
        {
          if (tdep_access_fpreg (c, reg, &fpval, 0) >= 0)
            as->acc.access_fpreg (as, reg, &fpval, 1, arg);
        }
      else
        {
          if (tdep_access_reg (c, reg, &val, 0) >= 0)
            as->acc.access_reg (as, reg, &val, 1, arg);
        }
    }

  return 0;
}

int
unw_resume (unw_cursor_t *cursor)
{
  struct cursor *c = (struct cursor *) cursor;
  int ret;

  Debug (1, "(cursor=%p)\n", c);

  if ((ret = establish_machine_state (c)) < 0)
    return ret;

  return (*c->dwarf.as->acc.resume) (c->dwarf.as, (unw_cursor_t *)c,
                                     c->dwarf.as_arg);
}
