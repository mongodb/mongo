/* libunwind - a platform-independent unwind library
   Copyright (C) 2023 Dmitry Chagin <dchagin@FreeBSD.org>

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

#include <sys/types.h>

#include <signal.h>
#include <stddef.h>
#include <ucontext.h>

#include <machine/sigframe.h>

#include "unwind_i.h"
#include "ucontext_i.h"

#ifndef UNW_REMOTE_ONLY

#define setcontext		UNW_ARCH_OBJ(setcontext)
extern NORETURN void setcontext(ucontext_t *);

HIDDEN int
aarch64_local_resume (unw_addr_space_t as, unw_cursor_t *cursor, void *arg)
{
  struct cursor *c = (struct cursor *) cursor;

  /*
   * XXX. Due to incorrectly handled cfi_signal_frame directive
   * (it should mark current function, not a frame above)
   * temporarily use unw_is_signal_frame to detect signal trampoline.
   */
  if (unw_is_signal_frame (cursor))
    {
      ucontext_t *uc = (ucontext_t *)(c->sigcontext_sp + offsetof(struct sigframe, sf_uc));

      if (c->dwarf.eh_valid_mask & 0x1)
        uc->uc_mcontext.mc_gpregs.gp_x[0] = c->dwarf.eh_args[0];
      if (c->dwarf.eh_valid_mask & 0x2)
        uc->uc_mcontext.mc_gpregs.gp_x[1] = c->dwarf.eh_args[1];
      if (c->dwarf.eh_valid_mask & 0x4)
        uc->uc_mcontext.mc_gpregs.gp_x[2] = c->dwarf.eh_args[2];

      Debug (8, "resuming at ip=%llx via sigreturn(%p)\n",
               (unsigned long long) c->sigcontext_pc, uc);
      sigreturn(uc);
      abort();
    }
  else
    {
	setcontext(c->uc);
    }

  unreachable();
  return -UNW_EINVAL;
}

#endif /* !UNW_REMOTE_ONLY */
