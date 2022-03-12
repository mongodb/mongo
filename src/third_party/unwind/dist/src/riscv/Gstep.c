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
#include "offsets.h"

static int
riscv_handle_signal_frame (unw_cursor_t *cursor)
{
  int ret, i;
  struct cursor *c = (struct cursor *) cursor;
  unw_word_t sp, sp_addr = c->dwarf.cfa;
  struct dwarf_loc sp_loc = DWARF_LOC (sp_addr, 0);

  if ((ret = dwarf_get (&c->dwarf, sp_loc, &sp)) < 0)
    return -UNW_EUNSPEC;

  if (!unw_is_signal_frame (cursor))
    return -UNW_EUNSPEC;

#ifdef __linux__
  /* rt_sigframe contains the siginfo structure, the ucontext, and then
     the trampoline. We store the mcontext inside ucontext as sigcontext_addr.
  */
  c->sigcontext_format = RISCV_SCF_LINUX_RT_SIGFRAME;
  c->sigcontext_addr = sp_addr + sizeof (siginfo_t) + UC_MCONTEXT_REGS_OFF;
  c->sigcontext_sp = sp_addr;
  c->sigcontext_pc = c->dwarf.ip;
#else
  /* Not making any assumption at all - You need to implement this */
  return -UNW_EUNSPEC;
#endif

  /* Update the dwarf cursor.
     Set the location of the registers to the corresponding addresses of the
     uc_mcontext / sigcontext structure contents.  */

#define  SC_REG_OFFSET(X)   (8 * X)

  /* The PC is stored in place of X0 in sigcontext */
  c->dwarf.loc[UNW_TDEP_IP] = DWARF_LOC (c->sigcontext_addr + SC_REG_OFFSET(UNW_RISCV_X0), 0);

  for (i = UNW_RISCV_X1; i <= UNW_RISCV_F31; i++)
    {
      c->dwarf.loc[i] = DWARF_LOC (c->sigcontext_addr + SC_REG_OFFSET(i), 0);
    }

  /* Set SP/CFA and PC/IP.  */
  dwarf_get (&c->dwarf, c->dwarf.loc[UNW_TDEP_SP], &c->dwarf.cfa);
  dwarf_get (&c->dwarf, c->dwarf.loc[UNW_TDEP_IP], &c->dwarf.ip);

  return 1;
}

int
unw_step (unw_cursor_t *cursor)
{
  struct cursor *c = (struct cursor *) cursor;
  int validate = c->validate;
  int ret;

  Debug (1, "(cursor=%p, ip=0x%016lx, sp=0x%016lx)\n",
         c, c->dwarf.ip, c->dwarf.cfa);

  /* Validate all addresses before dereferencing. */
  c->validate = 1;

  /* Special handling the signal frame. */
  if (unw_is_signal_frame (cursor) > 0)
    return riscv_handle_signal_frame (cursor);

  /* Restore default memory validation state */
  c->validate = validate;

  /* Try DWARF-based unwinding... */
  ret = dwarf_step (&c->dwarf);

  if (unlikely (ret == -UNW_ESTOPUNWIND))
    return ret;

  /* DWARF unwinding didn't work, let's tread carefully here */
  if (unlikely (ret < 0))
    {
      Debug (1, "DWARF unwinding failed (cursor=%p, ip=0x%016lx, sp=0x%016lx)\n", c, c->dwarf.ip, c->dwarf.cfa);

      /* Try RA/X1? */
      c->dwarf.loc[UNW_RISCV_PC] = c->dwarf.loc[UNW_RISCV_X1];
      c->dwarf.loc[UNW_RISCV_X1] = DWARF_NULL_LOC;
      if (!DWARF_IS_NULL_LOC (c->dwarf.loc[UNW_RISCV_PC]))
        {
          ret = dwarf_get (&c->dwarf, c->dwarf.loc[UNW_RISCV_PC], &c->dwarf.ip);
          if (ret < 0)
            {
              Debug (2, "Failed to get PC from return address: %d\n", ret);
              return ret;
            }

          Debug (2, "ra= 0x%016lx\n", c->dwarf.ip);
          ret = 1;
        }
      else
        {
          c->dwarf.ip = 0;
        }
    }

  return (c->dwarf.ip == 0) ? 0 : 1;
}
