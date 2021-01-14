/* libunwind - a platform-independent unwind library
   Copyright (C) 2002-2003 Hewlett-Packard Co
	Contributed by David Mosberger-Tang <davidm@hpl.hp.com>

   Modified for x86_64 by Max Asbock <masbock@us.ibm.com>

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
#include "ucontext_i.h"

#include <sys/syscall.h>

struct sigframe {
	uint64_t	signo;
	uint64_t	sip;
};

int
unw_is_signal_frame (unw_cursor_t *cursor)
{
  struct cursor *c = (struct cursor *) cursor;

  c->sigcontext_format = (c->dwarf.ip == (unw_word_t)-1) ?
    X86_64_SCF_SOLARIS_SIGFRAME : X86_64_SCF_NONE;

  return (c->sigcontext_format);
}

HIDDEN int
x86_64_handle_signal_frame (unw_cursor_t *cursor)
{
  struct cursor *c = (struct cursor *) cursor;
  unw_word_t ucontext = c->dwarf.cfa + sizeof (struct sigframe);

  if (c->sigcontext_format != X86_64_SCF_SOLARIS_SIGFRAME)
    return -UNW_EBADFRAME;

  c->sigcontext_addr = c->dwarf.cfa;

  Debug(1, "signal frame cfa = %lx ucontext = %lx\n",
    (uint64_t)c->dwarf.cfa, (uint64_t)ucontext);

  struct dwarf_loc rsp_loc = DWARF_LOC (ucontext + UC_MCONTEXT_GREGS_RSP, 0);
  int ret = dwarf_get (&c->dwarf, rsp_loc, &c->dwarf.cfa);

  if (ret < 0)
    {
      Debug (2, "return %d\n", ret);
      return ret;
    }

    c->dwarf.loc[RAX] = DWARF_LOC (ucontext + UC_MCONTEXT_GREGS_RAX, 0);
    c->dwarf.loc[RDX] = DWARF_LOC (ucontext + UC_MCONTEXT_GREGS_RDX, 0);
    c->dwarf.loc[RCX] = DWARF_LOC (ucontext + UC_MCONTEXT_GREGS_RCX, 0);
    c->dwarf.loc[RBX] = DWARF_LOC (ucontext + UC_MCONTEXT_GREGS_RBX, 0);
    c->dwarf.loc[RSI] = DWARF_LOC (ucontext + UC_MCONTEXT_GREGS_RSI, 0);
    c->dwarf.loc[RDI] = DWARF_LOC (ucontext + UC_MCONTEXT_GREGS_RDI, 0);
    c->dwarf.loc[RBP] = DWARF_LOC (ucontext + UC_MCONTEXT_GREGS_RBP, 0);
    c->dwarf.loc[RSP] = DWARF_LOC (ucontext + UC_MCONTEXT_GREGS_RSP, 0);
    c->dwarf.loc[ R8] = DWARF_LOC (ucontext + UC_MCONTEXT_GREGS_R8, 0);
    c->dwarf.loc[ R9] = DWARF_LOC (ucontext + UC_MCONTEXT_GREGS_R9, 0);
    c->dwarf.loc[R10] = DWARF_LOC (ucontext + UC_MCONTEXT_GREGS_R10, 0);
    c->dwarf.loc[R11] = DWARF_LOC (ucontext + UC_MCONTEXT_GREGS_R11, 0);
    c->dwarf.loc[R12] = DWARF_LOC (ucontext + UC_MCONTEXT_GREGS_R12, 0);
    c->dwarf.loc[R13] = DWARF_LOC (ucontext + UC_MCONTEXT_GREGS_R13, 0);
    c->dwarf.loc[R14] = DWARF_LOC (ucontext + UC_MCONTEXT_GREGS_R14, 0);
    c->dwarf.loc[R15] = DWARF_LOC (ucontext + UC_MCONTEXT_GREGS_R15, 0);
    c->dwarf.loc[RIP] = DWARF_LOC (ucontext + UC_MCONTEXT_GREGS_RIP, 0);

    c->dwarf.use_prev_instr = 1;
    return 0;
}

#ifndef UNW_REMOTE_ONLY
HIDDEN void *
x86_64_r_uc_addr (ucontext_t *uc, int reg)
{
  /* NOTE: common_init() in init.h inlines these for fast path access. */
  void *addr;

  switch (reg)
    {
    case UNW_X86_64_R8: addr = &uc->uc_mcontext.gregs[REG_R8]; break;
    case UNW_X86_64_R9: addr = &uc->uc_mcontext.gregs[REG_R9]; break;
    case UNW_X86_64_R10: addr = &uc->uc_mcontext.gregs[REG_R10]; break;
    case UNW_X86_64_R11: addr = &uc->uc_mcontext.gregs[REG_R11]; break;
    case UNW_X86_64_R12: addr = &uc->uc_mcontext.gregs[REG_R12]; break;
    case UNW_X86_64_R13: addr = &uc->uc_mcontext.gregs[REG_R13]; break;
    case UNW_X86_64_R14: addr = &uc->uc_mcontext.gregs[REG_R14]; break;
    case UNW_X86_64_R15: addr = &uc->uc_mcontext.gregs[REG_R15]; break;
    case UNW_X86_64_RDI: addr = &uc->uc_mcontext.gregs[REG_RDI]; break;
    case UNW_X86_64_RSI: addr = &uc->uc_mcontext.gregs[REG_RSI]; break;
    case UNW_X86_64_RBP: addr = &uc->uc_mcontext.gregs[REG_RBP]; break;
    case UNW_X86_64_RBX: addr = &uc->uc_mcontext.gregs[REG_RBX]; break;
    case UNW_X86_64_RDX: addr = &uc->uc_mcontext.gregs[REG_RDX]; break;
    case UNW_X86_64_RAX: addr = &uc->uc_mcontext.gregs[REG_RAX]; break;
    case UNW_X86_64_RCX: addr = &uc->uc_mcontext.gregs[REG_RCX]; break;
    case UNW_X86_64_RSP: addr = &uc->uc_mcontext.gregs[REG_RSP]; break;
    case UNW_X86_64_RIP: addr = &uc->uc_mcontext.gregs[REG_RIP]; break;

    default:
      addr = NULL;
    }
  return addr;
}

HIDDEN NORETURN void
x86_64_sigreturn (unw_cursor_t *cursor)
{
  abort();
}

#endif
