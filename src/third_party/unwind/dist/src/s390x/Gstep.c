/* libunwind - a platform-independent unwind library
   Copyright (C) 2002-2004 Hewlett-Packard Co
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
#include <signal.h>

static int
s390x_handle_signal_frame (unw_cursor_t *cursor)
{
  struct cursor *c = (struct cursor *) cursor;
  int ret, i;
  unw_word_t sc_addr, sp, *gprs, *fprs, *psw;

  ret = dwarf_get (&c->dwarf, c->dwarf.loc[UNW_S390X_R15], &sp);
  if (ret < 0)
    return ret;

  /* Save the SP and PC to be able to return execution at this point
     later in time (unw_resume).  */
  c->sigcontext_sp = sp;
  c->sigcontext_pc = c->dwarf.ip;
  switch (c->sigcontext_format)
    {
    case S390X_SCF_LINUX_SIGFRAME: /* sigreturn */
      sc_addr = sp + 160;
      gprs = ((struct sigcontext*)sc_addr)->sregs->regs.gprs;
      fprs = (unw_word_t*)((struct sigcontext*)sc_addr)->sregs->fpregs.fprs;
      psw  = &((struct sigcontext*)sc_addr)->sregs->regs.psw.addr;
      break;
    case S390X_SCF_LINUX_RT_SIGFRAME: /* rt_sigreturn */
      sc_addr = sp + sizeof(siginfo_t) + 8 + 160;
      gprs = ((ucontext_t*)sc_addr)->uc_mcontext.gregs;
      fprs = (unw_word_t*)((ucontext_t*)sc_addr)->uc_mcontext.fpregs.fprs;
      psw  = &((ucontext_t*)sc_addr)->uc_mcontext.psw.addr;
      break;
    default:
      return -UNW_EUNSPEC;
    }

  c->sigcontext_addr = sc_addr;

  /* Update the dwarf cursor.
     Set the location of the registers to the corresponding addresses of the
     uc_mcontext / sigcontext structure contents.  */
  for (i = UNW_S390X_R0; i <= UNW_S390X_R15; ++i)
    c->dwarf.loc[i] = DWARF_MEM_LOC (c, (unw_word_t) &gprs[i-UNW_S390X_R0]);
  for (i = UNW_S390X_F0; i <= UNW_S390X_F15; ++i)
    c->dwarf.loc[i] = DWARF_MEM_LOC (c, (unw_word_t) &fprs[i-UNW_S390X_F0]);

  c->dwarf.loc[UNW_S390X_IP] = DWARF_MEM_LOC (c, (unw_word_t) psw);

  /* Set SP/CFA and PC/IP.
     Normally the default CFA on s390x is r15+160. We do not add that offset
     here because dwarf_step will add the offset.  */
  dwarf_get (&c->dwarf, c->dwarf.loc[UNW_S390X_R15], &c->dwarf.cfa);
  dwarf_get (&c->dwarf, c->dwarf.loc[UNW_S390X_IP], &c->dwarf.ip);

  c->dwarf.pi_valid = 0;
  c->dwarf.use_prev_instr = 0;

  return 1;
}

int
unw_step (unw_cursor_t *cursor)
{
  struct cursor *c = (struct cursor *) cursor;
  int ret = 0, val = c->validate, sig;

#if CONSERVATIVE_CHECKS
  c->validate = 1;
#endif

  Debug (1, "(cursor=%p, ip=0x%016lx, cfa=0x%016lx)\n",
         c, c->dwarf.ip, c->dwarf.cfa);

  /* Try DWARF-based unwinding... */
  c->sigcontext_format = S390X_SCF_NONE;
  ret = dwarf_step (&c->dwarf);

#if CONSERVATIVE_CHECKS
  c->validate = val;
#endif

  if (unlikely (ret == -UNW_ENOINFO))
    {
      /* GCC doesn't currently emit debug information for signal
         trampolines on s390x so we check for them explicitly.

         If there isn't debug information available we could also
         try using the backchain (if available).

         Other platforms also detect PLT entries here. That's
         tricky to do reliably on s390x so I've left it out for
         now.  */

      /* Memory accesses here are quite likely to be unsafe. */
      c->validate = 1;

      /* Check if this is a signal frame. */
      sig = unw_is_signal_frame (cursor);
      if (sig > 0)
        {
          c->sigcontext_format = sig;
          ret = s390x_handle_signal_frame (cursor);
        }
      else
        {
          c->dwarf.ip = 0;
          ret = 0;
        }

      c->validate = val;
      return ret;
    }

  if (unlikely (ret > 0 && c->dwarf.ip == 0))
    return 0;

  return ret;
}
