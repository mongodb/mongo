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

/* FIXME for LoongArch64.  */

#include <stdlib.h>

#include "unwind_i.h"

#ifndef UNW_REMOTE_ONLY

HIDDEN inline int
loongarch64_local_resume (unw_addr_space_t as, unw_cursor_t *cursor, void *arg)
{
  struct cursor *c = (struct cursor *) cursor;
  unw_tdep_context_t *uc = c->uc;

  if (c->sigcontext_format == LOONGARCH64_SCF_NONE)
    {
      /* Since there are no signals involved here we restore EH and non scratch
         registers only.  */
      register void *gregs asm("$t0") = uc->uc_mcontext.__gregs;
      asm volatile (
        "ld.d $ra, %0, 8\n"
        "ld.d $sp, %0, 3*8\n"
        "ld.d $fp, %0, 22*8\n"
        "ld.d $s0, %0, 23*8\n"
        "ld.d $s1, %0, 24*8\n"
        "ld.d $s2, %0, 25*8\n"
        "ld.d $s3, %0, 26*8\n"
        "ld.d $s4, %0, 27*8\n"
        "ld.d $s5, %0, 28*8\n"
        "ld.d $s6, %0, 29*8\n"
        "ld.d $s7, %0, 30*8\n"
        "ld.d $s8, %0, 31*8\n"
        "jr $ra\n"
        :
        : "r" (gregs)
      );
      unreachable();
    }
  else /* c->sigcontext_format == LOONGARCH64_SCF_LINUX_RT_SIGFRAME */
    {
      int i;
      struct sigcontext *sc = (struct sigcontext *) c->sigcontext_addr;

      sc->sc_pc = c->dwarf.ip;
      for (i = UNW_LOONGARCH64_R0; i <= UNW_LOONGARCH64_R31; i++)
            sc->sc_regs[i] = uc->uc_mcontext.__gregs[i];

      Debug (8, "resuming at ip=0x%lx via sigreturn() (trampoline @ 0x%lx, sp @ 0x%lx)\n",
        c->dwarf.ip, c->sigcontext_pc, c->sigcontext_sp);

      asm volatile (
        "move $sp, %0\n"
        "jr %1\n"
        : : "r" (c->sigcontext_sp), "r" (c->sigcontext_pc)
      );
   }
  unreachable();

  return -UNW_EINVAL;
}

#endif /* !UNW_REMOTE_ONLY */

static inline void
establish_machine_state (struct cursor *c)
{
  unw_addr_space_t as = c->dwarf.as;
  void *arg = c->dwarf.as_arg;
  unw_word_t val;
  int reg;

  Debug (8, "copying out cursor state\n");

  for (reg = UNW_LOONGARCH64_R0; reg <= UNW_LOONGARCH64_R31; reg++)
    {
      Debug (16, "copying %s %d\n", unw_regname (reg), reg);
      if (tdep_access_reg (c, reg, &val, 0) >= 0)
        as->acc.access_reg (as, reg, &val, 1, arg);
    }
}

int
unw_resume (unw_cursor_t *cursor)
{
  struct cursor *c = (struct cursor *) cursor;

  Debug (1, "(cursor=%p)\n", c);

  if (!c->dwarf.ip)
    {
      /* This can happen easily when the frame-chain gets truncated
         due to bad or missing unwind-info.  */
      Debug (1, "refusing to resume execution at address 0\n");
      return -UNW_EINVAL;
    }

  establish_machine_state (c);

  return (*c->dwarf.as->acc.resume) (c->dwarf.as, (unw_cursor_t *) c,
                                     c->dwarf.as_arg);
}
