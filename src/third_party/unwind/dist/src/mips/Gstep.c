/* libunwind - a platform-independent unwind library
   Copyright (C) 2015 Imagination Technologies Limited
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

static int
mips_handle_signal_frame (unw_cursor_t *cursor)
{
  struct cursor *c = (struct cursor *) cursor;
  unw_word_t sc_addr, sp_addr = c->dwarf.cfa;
  unw_word_t ra, fp;
  int ret;

  switch (unw_is_signal_frame (cursor)) {
  case 1:
    sc_addr = sp_addr + LINUX_SF_TRAMP_SIZE + sizeof (siginfo_t) +
              LINUX_UC_MCONTEXT_OFF;
    break;
  case 2:
    sc_addr = sp_addr + LINUX_UC_MCONTEXT_OFF;
    break;
  default:
    return -UNW_EUNSPEC;
  }

  if (tdep_big_endian(c->dwarf.as))
    sc_addr += 4;

  c->sigcontext_addr = sc_addr;

  /* Update the dwarf cursor. */
  c->dwarf.loc[UNW_MIPS_R0]  = DWARF_LOC (sc_addr + LINUX_SC_R0_OFF, 0);
  c->dwarf.loc[UNW_MIPS_R1]  = DWARF_LOC (sc_addr + LINUX_SC_R1_OFF, 0);
  c->dwarf.loc[UNW_MIPS_R2]  = DWARF_LOC (sc_addr + LINUX_SC_R2_OFF, 0);
  c->dwarf.loc[UNW_MIPS_R3]  = DWARF_LOC (sc_addr + LINUX_SC_R3_OFF, 0);
  c->dwarf.loc[UNW_MIPS_R4]  = DWARF_LOC (sc_addr + LINUX_SC_R4_OFF, 0);
  c->dwarf.loc[UNW_MIPS_R5]  = DWARF_LOC (sc_addr + LINUX_SC_R5_OFF, 0);
  c->dwarf.loc[UNW_MIPS_R6]  = DWARF_LOC (sc_addr + LINUX_SC_R6_OFF, 0);
  c->dwarf.loc[UNW_MIPS_R7]  = DWARF_LOC (sc_addr + LINUX_SC_R7_OFF, 0);
  c->dwarf.loc[UNW_MIPS_R8]  = DWARF_LOC (sc_addr + LINUX_SC_R8_OFF, 0);
  c->dwarf.loc[UNW_MIPS_R9]  = DWARF_LOC (sc_addr + LINUX_SC_R9_OFF, 0);
  c->dwarf.loc[UNW_MIPS_R10] = DWARF_LOC (sc_addr + LINUX_SC_R10_OFF, 0);
  c->dwarf.loc[UNW_MIPS_R11] = DWARF_LOC (sc_addr + LINUX_SC_R11_OFF, 0);
  c->dwarf.loc[UNW_MIPS_R12] = DWARF_LOC (sc_addr + LINUX_SC_R12_OFF, 0);
  c->dwarf.loc[UNW_MIPS_R13] = DWARF_LOC (sc_addr + LINUX_SC_R13_OFF, 0);
  c->dwarf.loc[UNW_MIPS_R14] = DWARF_LOC (sc_addr + LINUX_SC_R14_OFF, 0);
  c->dwarf.loc[UNW_MIPS_R15] = DWARF_LOC (sc_addr + LINUX_SC_R15_OFF, 0);
  c->dwarf.loc[UNW_MIPS_R16] = DWARF_LOC (sc_addr + LINUX_SC_R16_OFF, 0);
  c->dwarf.loc[UNW_MIPS_R17] = DWARF_LOC (sc_addr + LINUX_SC_R17_OFF, 0);
  c->dwarf.loc[UNW_MIPS_R18] = DWARF_LOC (sc_addr + LINUX_SC_R18_OFF, 0);
  c->dwarf.loc[UNW_MIPS_R19] = DWARF_LOC (sc_addr + LINUX_SC_R19_OFF, 0);
  c->dwarf.loc[UNW_MIPS_R20] = DWARF_LOC (sc_addr + LINUX_SC_R20_OFF, 0);
  c->dwarf.loc[UNW_MIPS_R21] = DWARF_LOC (sc_addr + LINUX_SC_R21_OFF, 0);
  c->dwarf.loc[UNW_MIPS_R22] = DWARF_LOC (sc_addr + LINUX_SC_R22_OFF, 0);
  c->dwarf.loc[UNW_MIPS_R23] = DWARF_LOC (sc_addr + LINUX_SC_R23_OFF, 0);
  c->dwarf.loc[UNW_MIPS_R24] = DWARF_LOC (sc_addr + LINUX_SC_R24_OFF, 0);
  c->dwarf.loc[UNW_MIPS_R25] = DWARF_LOC (sc_addr + LINUX_SC_R25_OFF, 0);
  c->dwarf.loc[UNW_MIPS_R26] = DWARF_LOC (sc_addr + LINUX_SC_R26_OFF, 0);
  c->dwarf.loc[UNW_MIPS_R27] = DWARF_LOC (sc_addr + LINUX_SC_R27_OFF, 0);
  c->dwarf.loc[UNW_MIPS_R28] = DWARF_LOC (sc_addr + LINUX_SC_R28_OFF, 0);
  c->dwarf.loc[UNW_MIPS_R29] = DWARF_LOC (sc_addr + LINUX_SC_R29_OFF, 0);
  c->dwarf.loc[UNW_MIPS_R30] = DWARF_LOC (sc_addr + LINUX_SC_R30_OFF, 0);
  c->dwarf.loc[UNW_MIPS_R31] = DWARF_LOC (sc_addr + LINUX_SC_R31_OFF, 0);
  c->dwarf.loc[UNW_MIPS_PC] = DWARF_LOC (sc_addr + LINUX_SC_PC_OFF, 0);

  /* Set SP/CFA and PC/IP. */
  dwarf_get (&c->dwarf, c->dwarf.loc[UNW_MIPS_R29], &c->dwarf.cfa);

  if ((ret = dwarf_get(&c->dwarf, DWARF_LOC(sc_addr + LINUX_SC_PC_OFF, 0),
                       &c->dwarf.ip)) < 0)
    return ret;

  if ((ret = dwarf_get(&c->dwarf, DWARF_LOC(sc_addr + LINUX_SC_R31_OFF, 0),
                       &ra)) < 0)
    return ret;
  if ((ret = dwarf_get(&c->dwarf, DWARF_LOC(sc_addr + LINUX_SC_R30_OFF, 0),
                       &fp)) < 0)
    return ret;

  Debug (2, "SH (ip=0x%016llx, ra=0x%016llx, sp=0x%016llx, fp=0x%016llx)\n",
         (unsigned long long)c->dwarf.ip, (unsigned long long)ra,
         (unsigned long long)c->dwarf.cfa, (unsigned long long)fp);

  c->dwarf.pi_valid = 0;
  c->dwarf.use_prev_instr = 0;

  return 1;
}


int _step_n64(struct cursor *c)
{
  //TODO:handle plt entry
  struct dwarf_loc fp_loc, pc_loc;
  int ret;
  unw_word_t fp = 0;
  unw_word_t ra = 0;

  ret = dwarf_get (&c->dwarf, c->dwarf.loc[UNW_MIPS_R30], &fp);
  if (ret < 0)
    {
      Debug (2, "returning %d [FP=0x%lx]\n", ret,
             DWARF_GET_LOC (c->dwarf.loc[UNW_MIPS_R30]));
      return ret;
    }
  ret = dwarf_get (&c->dwarf, c->dwarf.loc[UNW_MIPS_R31], &ra);
  if (ret < 0)
    {
      Debug (2, "returning %d [RA=0x%lx]\n", ret,
             DWARF_GET_LOC (c->dwarf.loc[UNW_MIPS_R31]));
      return ret;
    }

  if (!fp) {
    fp_loc = DWARF_NULL_LOC;
    pc_loc = DWARF_NULL_LOC;
  } else {
    fp_loc = DWARF_LOC(fp+16, 0);
    pc_loc = DWARF_LOC (fp+24, 0);
#if UNW_DEBUG
    unw_word_t fp1 = 0;
    ret = dwarf_get (&c->dwarf, fp_loc, &fp1);
    Debug (1, "RET:%d [FP=0x%lx] = 0x%lx (cfa = 0x%lx) -> 0x%lx. SAVED PC:0x%lx\n",
           ret,
           (unsigned long) DWARF_GET_LOC (c->dwarf.loc[UNW_MIPS_R30]),
           fp, c->dwarf.cfa, fp1, ra);
#endif
    /* Heuristic to determine incorrect guess.  For FP to be a
       valid frame it needs to be above current CFA, but don't
       let it go more than a little.  Note that we can't deduce
       anything about new FP (fp1) since it may not be a frame
       pointer in the frame above.  Just check we get the value. */
    if (ret < 0
        || fp < c->dwarf.cfa
        || (fp - c->dwarf.cfa) > 0x4000)
      {
        pc_loc = DWARF_NULL_LOC;
        fp_loc = DWARF_NULL_LOC;
      }
  }

  c->dwarf.loc[UNW_MIPS_R30] = fp_loc;

  c->dwarf.loc[UNW_MIPS_PC] = c->dwarf.loc[UNW_MIPS_R31];
  c->dwarf.loc[UNW_MIPS_R31] = pc_loc;
  c->dwarf.use_prev_instr = 1;

  if (DWARF_IS_NULL_LOC (c->dwarf.loc[UNW_MIPS_R30]))
    {
      ret = 0;
      Debug (2, "NULL %%fp loc, returning %d\n", ret);
      return ret;
    }

  if (!DWARF_IS_NULL_LOC (c->dwarf.loc[UNW_MIPS_PC]))
    {
      ret = dwarf_get (&c->dwarf, c->dwarf.loc[UNW_MIPS_PC], &c->dwarf.ip);
      Debug (1, "Frame Chain [IP=0x%Lx] = 0x%Lx\n",
             (unsigned long long) DWARF_GET_LOC (c->dwarf.loc[UNW_MIPS_PC]),
             (unsigned long long) c->dwarf.ip);
      if (ret < 0)
        {
          Debug (2, "returning %d\n", ret);
          return ret;
        }
      ret = 1;
    }
  else {
    c->dwarf.ip = 0;
  }
  return (c->dwarf.ip == 0) ? 0 : 1;
}

int
unw_step (unw_cursor_t *cursor)
{
  struct cursor *c = (struct cursor *) cursor;
  int ret;

  ret = mips_handle_signal_frame (cursor);
  if (ret < 0)
    /* Not a signal frame, try DWARF-based unwinding. */
    ret = dwarf_step (&c->dwarf);

  if (unlikely (ret == -UNW_ESTOPUNWIND))
    return ret;

#if _MIPS_SIM == _ABI64
  if (unlikely (ret < 0))
    {
      return _step_n64(c);
    }
#endif
  return (c->dwarf.ip == 0) ? 0 : 1;
}
