/* libunwind - a platform-independent unwind library
   Copyright (C) 2008 CodeSourcery
   Copyright (C) 2011-2013 Linaro Limited
   Copyright (C) 2012 Tommi Rantala <tt.rantala@gmail.com>
   Copyright 2024 Stephen M. Webb  <swebb@blackberry.com>

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

#ifndef UNW_REMOTE_ONLY

/* Magic constants generated from gen-offsets.c */
#define SC_R0_OFF   "8"
#define SC_R2_OFF   "24"
#define SC_R18_OFF  "152"
#define SC_R20_OFF  "168"
#define SC_R22_OFF  "184"
#define SC_R24_OFF  "200"
#define SC_R26_OFF  "216"
#define SC_R28_OFF  "232"
#define SC_R30_OFF  "248"

#define FP_R08_OFF "80"
#define FP_R09_OFF "88"
#define FP_R10_OFF "96"
#define FP_R11_OFF "104"
#define FP_R12_OFF "112"
#define FP_R13_OFF "120"
#define FP_R14_OFF "128"
#define FP_R15_OFF "136"

#define SC_SP_OFF   "0x100"

HIDDEN int
aarch64_local_resume (unw_addr_space_t as, unw_cursor_t *cursor, void *arg)
{
  struct cursor *c = (struct cursor *) cursor;
  unw_context_t *uc = c->uc;

  if (c->sigcontext_format == AARCH64_SCF_NONE)
    {

      /*
       * This is effectively the old POSIX setcontext().
       *
       * This inline asm is broken up to use local scratch registers for the
       * uc_mcontext.regs and FPCTX base addresses because newer versions of GCC
       * and clang barf on too many constraints (gh-702) when the C array
       * elements are used directly.
       *
       * Clobbers aren't required for the inline asm because they just convince
       * the compiler to save those registers and they never get restored
       * becauise the asm ends with a plain ol' ret.
       */
      register void* uc_mcontext __asm__ ("x5") = (void*) &uc->uc_mcontext;
      register void* fpctx __asm__ ("x4") = (void*) GET_FPCTX(uc);

      /* Since there are no signals involved here we restore EH and non scratch
         registers only.  */
      __asm__ __volatile__ (
        "ldp x0,  x1,  [x5, " SC_R0_OFF  "]\n\t"
        "ldp x2,  x3,  [x5, " SC_R2_OFF  "]\n\t"
        "ldp x18, x19, [x5, " SC_R18_OFF "]\n\t"
        "ldp x20, x21, [x5, " SC_R20_OFF "]\n\t"
        "ldp x22, x23, [x5, " SC_R22_OFF "]\n\t"
        "ldp x24, x25, [x5, " SC_R24_OFF "]\n\t"
        "ldp x26, x27, [x5, " SC_R26_OFF "]\n\t"
        "ldp x28, x29, [x5, " SC_R28_OFF "]\n\t"
        "ldr x30, [x5, " SC_R30_OFF "]\n\t"
        "ldr d8,  [x4, " FP_R08_OFF "]\n\t"
        "ldr d9,  [x4, " FP_R09_OFF "]\n\t"
        "ldr d10, [x4, " FP_R10_OFF "]\n\t"
        "ldr d11, [x4, " FP_R11_OFF "]\n\t"
        "ldr d12, [x4, " FP_R12_OFF "]\n\t"
        "ldr d13, [x4, " FP_R13_OFF "]\n\t"
        "ldr d14, [x4, " FP_R14_OFF "]\n\t"
        "ldr d15, [x4, " FP_R15_OFF "]\n\t"
        "ldr x5,  [x5, " SC_SP_OFF "]\n\t"
        "mov sp, x5\n\t"
        "ret\n"
        : 
        : [uc_mcontext] "r"(uc_mcontext),
          [fpctx] "r"(fpctx)
      );
    }
  else
    {
      struct sigcontext *sc = (struct sigcontext *) c->sigcontext_addr;

      if (c->dwarf.eh_valid_mask & 0x1) sc->regs[0] = c->dwarf.eh_args[0];
      if (c->dwarf.eh_valid_mask & 0x2) sc->regs[1] = c->dwarf.eh_args[1];
      if (c->dwarf.eh_valid_mask & 0x4) sc->regs[2] = c->dwarf.eh_args[2];
      if (c->dwarf.eh_valid_mask & 0x8) sc->regs[3] = c->dwarf.eh_args[3];

      sc->regs[4] = uc->uc_mcontext.regs[4];
      sc->regs[5] = uc->uc_mcontext.regs[5];
      sc->regs[6] = uc->uc_mcontext.regs[6];
      sc->regs[7] = uc->uc_mcontext.regs[7];
      sc->regs[8] = uc->uc_mcontext.regs[8];
      sc->regs[9] = uc->uc_mcontext.regs[9];
      sc->regs[10] = uc->uc_mcontext.regs[10];
      sc->regs[11] = uc->uc_mcontext.regs[11];
      sc->regs[12] = uc->uc_mcontext.regs[12];
      sc->regs[13] = uc->uc_mcontext.regs[13];
      sc->regs[14] = uc->uc_mcontext.regs[14];
      sc->regs[15] = uc->uc_mcontext.regs[15];
      sc->regs[16] = uc->uc_mcontext.regs[16];
      sc->regs[17] = uc->uc_mcontext.regs[17];
      sc->regs[18] = uc->uc_mcontext.regs[18];
      sc->regs[19] = uc->uc_mcontext.regs[19];
      sc->regs[20] = uc->uc_mcontext.regs[20];
      sc->regs[21] = uc->uc_mcontext.regs[21];
      sc->regs[22] = uc->uc_mcontext.regs[22];
      sc->regs[23] = uc->uc_mcontext.regs[23];
      sc->regs[24] = uc->uc_mcontext.regs[24];
      sc->regs[25] = uc->uc_mcontext.regs[25];
      sc->regs[26] = uc->uc_mcontext.regs[26];
      sc->regs[27] = uc->uc_mcontext.regs[27];
      sc->regs[28] = uc->uc_mcontext.regs[28];
      sc->regs[29] = uc->uc_mcontext.regs[29];
      sc->regs[30] = uc->uc_mcontext.regs[30];
      sc->sp = uc->uc_mcontext.sp;
      sc->pc = uc->uc_mcontext.pc;
      sc->pstate = uc->uc_mcontext.pstate;

      __asm__ __volatile__ (
        "mov sp, %0\n"
        "ret %1\n"
        : : "r" (c->sigcontext_sp), "r" (c->sigcontext_pc)
      );
   }
  unreachable();
  return -UNW_EINVAL;
}

#endif /* !UNW_REMOTE_ONLY */
