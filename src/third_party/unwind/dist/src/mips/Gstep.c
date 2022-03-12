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



static inline
int is_valid_fp_val(unw_word_t cfa_val, unw_word_t fp_val)
{
  return fp_val > 0 && cfa_val > 0 && fp_val >cfa_val && (fp_val - cfa_val < 0x4000);
}

static int _step_n64(struct cursor *c)
{
  #define FP_REG UNW_MIPS_R30
  #define SP_REG UNW_MIPS_R29
  #define RA_REG UNW_MIPS_R31

  //TODO:handle plt entry
  int ret;
  unw_word_t current_fp_val = 0;
  unw_word_t current_ra_val = 0;
  unw_word_t current_sp_val = 0;
  struct dwarf_loc up_fp_loc = DWARF_NULL_LOC;
  struct dwarf_loc up_ra_loc = DWARF_NULL_LOC;

  ret = dwarf_get (&c->dwarf, c->dwarf.loc[SP_REG], &current_sp_val);
  if (ret < 0)
    {
      Debug (2, "returning %d [SP=0x%lx]\n", ret,
             DWARF_GET_LOC (c->dwarf.loc[FP_REG]));
      return ret;
    }
  ret = dwarf_get (&c->dwarf, c->dwarf.loc[FP_REG], &current_fp_val);
  if (ret < 0)
    {
      Debug (2, "returning %d [FP=0x%lx]\n", ret,
             DWARF_GET_LOC (c->dwarf.loc[FP_REG]));
      return ret;
    }
  ret = dwarf_get (&c->dwarf, c->dwarf.loc[RA_REG], &current_ra_val);
  if (ret < 0)
    {
      Debug (2, "returning %d [RA=0x%lx]\n", ret,
             DWARF_GET_LOC (c->dwarf.loc[RA_REG]));
      return ret;
    }

  Debug(2, "BEGIN GUESSING WITH SP:%p FP:%p CFA:%p at %p, RA:%p\n",
         current_sp_val, current_fp_val, c->dwarf.cfa,
         c->dwarf.ip, current_ra_val
         );

  if (current_fp_val == current_sp_val) {
    // Don't adjust FP
    up_fp_loc = c->dwarf.loc[FP_REG];
    up_ra_loc = c->dwarf.loc[RA_REG];
  } else if (is_valid_fp_val(c->dwarf.cfa, current_fp_val)) {
    /* Heuristic to determine incorrect guess.  For FP to be a
       valid frame it needs to be above current CFA, but don't
       let it go more than a little.  Note that we can't deduce
       anything about new FP (fp1) since it may not be a frame
       pointer in the frame above.  Just check we get the value. */
    up_fp_loc = DWARF_MEM_LOC (c, current_fp_val+16);
    up_ra_loc = DWARF_MEM_LOC (c, current_fp_val+24);
    unw_word_t up_fp_val = 0;
    ret = dwarf_get (&c->dwarf, up_fp_loc, &up_fp_val);
    if (ret > 0 && is_valid_fp_val(current_fp_val, up_fp_val)) {
      c->dwarf.loc[FP_REG] = up_fp_loc;
    }
  }

  if (DWARF_IS_NULL_LOC (up_fp_loc))
    {
      ret = 0;
      Debug (2, "NULL %%fp loc, returning %d\n", ret);
      return ret;
    }

  c->dwarf.loc[UNW_MIPS_PC] = c->dwarf.loc[RA_REG];
  c->dwarf.loc[RA_REG] = up_ra_loc;
  c->dwarf.loc[SP_REG] = up_fp_loc;
  c->dwarf.loc[FP_REG] = up_fp_loc;
  c->dwarf.use_prev_instr = 1;

  if (c->dwarf.ip == current_ra_val && current_fp_val == current_sp_val) {
    // Backtrace stopped: frame did not save the PC
    c->dwarf.ip = 0;
  } else {
    c->dwarf.ip = current_ra_val;
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

  if (unlikely (ret < 0))
    {
#if _MIPS_SIM == _ABI64
      return _step_n64(c);
#else
      return ret;
#endif
    }

  return (c->dwarf.ip == 0) ? 0 : 1;
}
