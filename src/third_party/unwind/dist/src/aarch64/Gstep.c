/* libunwind - a platform-independent unwind library
   Copyright (C) 2008 CodeSourcery
   Copyright (C) 2011-2013 Linaro Limited
   Copyright (C) 2012 Tommi Rantala <tt.rantala@gmail.com>
   Copyright 2022 Blackberry Limited.

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

#include "dwarf_i.h"
#include "ucontext_i.h"
#include "unwind_i.h"

static const int WSIZE = sizeof (unw_word_t);

/* Recognise PLT entries such as:
  40ddf0:       b0000570        adrp    x16, 4ba000 <_GLOBAL_OFFSET_TABLE_+0x2a8>
  40ddf4:       f9433611        ldr     x17, [x16,#1640]
  40ddf8:       9119a210        add     x16, x16, #0x668
  40ddfc:       d61f0220        br      x17

  \note The current implementation only supports little endian modes.
*/
static int
is_plt_entry (struct dwarf_cursor *c)
{
  unw_word_t w0 = 0, w1 = 0;
  unw_accessors_t *a;

  if (c->as->big_endian)
    {
      return 0;
    }

  /*
    A PLT (Procedure Linkage Table) is used by the dynamic linker to map the
    relative address of a position independent function call onto the real
    address of the function. If we attempt to unwind from any instruction
    inside the PLT, and the PLT is missing DWARF unwind information, then
    conventional unwinding will fail because although the function has been
    "called" we have not yet entered the prologue and set-up the stack frame.

    This code looks to see if the instruction is anywhere within a "recognised"
    PLT entry (note that the IP could be anywhere within the PLT, so we have to
    examine nearby instructions).
  */

  struct instruction_entry
    {
      uint32_t pattern;
      uint32_t mask;
    } instructions[4] =
    {
      // aarch64
      {0x90000010,0x9f00001f}, // adrp
      {0xf9400211,0xffc003ff}, // ldr
      {0x91000210,0xff8003ff}, // add
      {0xd61f0220,0xffffffff}, // br
    };

  a = unw_get_accessors (c->as);
  if ((*a->access_mem) (c->as, c->ip, &w0, 0, c->as_arg) < 0)
    {
      return 0;
    }

  /*
    NB: the following code is endian sensitive!

    The current implementation is for little-endian modes, big-endian modes
    will see the first instruction in the high bits of w0, and the second
    instruction in the low bits of w0. Some tweaks will be needed to read from
    the correct part of the word to support big endian modes.
  */
  if ((w0      & instructions[0].mask) == instructions[0].pattern &&
     ((w0>>32) & instructions[1].mask) == instructions[1].pattern)
  {
    if ((*a->access_mem) (c->as, c->ip+8, &w1, 0, c->as_arg) >= 0 &&
       (w1      & instructions[2].mask) == instructions[2].pattern &&
      ((w1>>32) & instructions[3].mask) == instructions[3].pattern)
      {
        return 1;
      }
    else
      {
        return 0;
      }
  }
  else if ((w0 & instructions[2].mask) == instructions[2].pattern &&
     ((w0>>32) & instructions[3].mask) == instructions[3].pattern)
  {
    w1 = w0;
    if ((*a->access_mem) (c->as, c->ip-8, &w0, 0, c->as_arg) >= 0 &&
       (w0      & instructions[0].mask) == instructions[0].pattern &&
      ((w0>>32) & instructions[1].mask) == instructions[1].pattern)
      {
        return 1;
      }
    else
      {
        return 0;
      }
  }
  else if ((w0 & instructions[1].mask) == instructions[1].pattern &&
     ((w0>>32) & instructions[2].mask) == instructions[2].pattern)
  {
    if ((*a->access_mem) (c->as, c->ip-4, &w0, 0, c->as_arg) < 0 ||
        (*a->access_mem) (c->as, c->ip+4, &w1, 0, c->as_arg) < 0)
    {
      return 0;
    }
  }
  else if ((w0 & instructions[3].mask) == instructions[3].pattern)
  {
    if ((*a->access_mem) (c->as, c->ip-12, &w0, 0, c->as_arg) < 0 ||
        (*a->access_mem) (c->as, c->ip-4, &w1, 0, c->as_arg) < 0)
    {
      return 0;
    }
  }

  if ((w0      & instructions[0].mask) == instructions[0].pattern &&
     ((w0>>32) & instructions[1].mask) == instructions[1].pattern &&
      (w1      & instructions[2].mask) == instructions[2].pattern &&
     ((w1>>32) & instructions[3].mask) == instructions[3].pattern)
    {
      return 1;
    }
  else
    {
      return 0;
    }
}

typedef enum frame_record_location
  {
    NONE,           /* frame record creation has not been detected, use LR */
    AT_SP_OFFSET,   /* frame record creation has been detected, but FP
                       update not detected */
    AT_FP,          /* frame record creation and FP update detected */
  } frame_record_location_t;

typedef struct frame_state
  {
    frame_record_location_t loc;
    int32_t offset;
  } frame_state_t;

/* Recognise when a frame record storing FP+LR has been created and whether FP
   has been updated to point to the frame record. For example:
  4183d4:       a9bd7bfd        stp     x29, x30, [sp,#-48]!    <= FP+LR stored
  4183d8:       d2800005        mov     x5, #0x0
  4183dc:       d2800004        mov     x4, #0x0
  4183e0:       910003fd        mov     x29, sp                 <= FP updated
  4183e4:       a90153f3        stp     x19, x20, [sp,#16]
  ...
  418444:       a94153f3        ldp     x19, x20, [sp,#16]
  418448:       f94013f5        ldr     x21, [sp,#32]
  41844c:       a8c37bfd        ldp     x29, x30, [sp],#48      <= FP+LR retrieved
  418450:       d65f03c0        ret
*/
static frame_state_t
get_frame_state (unw_cursor_t *cursor)
{
  struct cursor *c = (struct cursor *) cursor;
  unw_accessors_t *a = unw_get_accessors (c->dwarf.as);
  unw_word_t w, start_ip, ip, offp;
  frame_state_t fs;
  fs.loc = NONE;
  fs.offset = 0;

  /* PLT entries do not create frame records */
  if (is_plt_entry (&c->dwarf))
    return fs;

  /* Use get_proc_name to find start_ip of procedure */
  char name[128];
  if (((*a->get_proc_name) (c->dwarf.as, c->dwarf.ip, name, sizeof(name), &offp, c->dwarf.as_arg)) != 0)
    return fs;

  start_ip = c->dwarf.ip - offp;

  /* Check for frame record instructions since the start of the procedure (start_ip).
   * access_mem reads WSIZE bytes, so two instructions are checked in each iteration
   */
  for (ip = start_ip; ip < c->dwarf.ip; ip += WSIZE)
    {
      if ((*a->access_mem) (c->dwarf.as, ip, &w, 0, c->dwarf.as_arg) < 0)
        return fs;

      if (fs.loc == NONE)
        {
          /* Check that a 64-bit store pair (STP) instruction storing FP and LR
             has been executed, which would indicate that a frame record has been
             created and that the LR value is saved therein.

             From the ARM Architecture Reference Manual (ARMv8), the format of the
             64-bit STP instruction is

             | 10 | 101 | 0 | 0XX | 0 |   imm7  |  Rt2  |   Rn  |   Rt  |

             where X can be 0/1 above to indicate the instruction variant (post-
             index, pre-index or signed offset). imm7 is a 7-bit signed immediate
             offset. The following should be asserted for this check

             Rt2 == LR (x30) => 0b11110
             Rn  == SP (x31) => 0b11111
             Rt  == FP (x29) => 0b11101.

             Hence the bitmask should be constructed to assert that the instruction
             is

             | 10 | 101 | 0 | 0XX | 0 | XXXXXXX | 11110 | 11111 | 11101 |

             So using a bitmask of 0xfe407fff, the masked instruction would be
             0xa8007bfd */
          if ((((w & 0xfe407fff00000000) == 0xa8007bfd00000000) && (c->dwarf.ip > ip + 4))
           || (((w & 0x00000000fe407fff) == 0x00000000a8007bfd) && (c->dwarf.ip > ip)))
            {
              fs.loc = AT_SP_OFFSET;

              /* If the signed offset variant of the STP instruction is detected,
                 the frame record may not be currently pointed to by SP. Extract
                 the offset from the STP instruction.

                 From the ARM Architecture Reference Manual (ARMv8), the format of
                 the signed offset variant of the 64-bit STP instruction is

                 | 10 | 101 | 0 | 010 | 0 | imm7 | Rt2 | Rn | Rt |

                 Hence the bitmask should be constructed to assert that the
                 instruction is

                 | 10 | 101 | 0 | 010 | 0 | XXXXXXX | 11110 | 11111 | 11101 |

                 So using a bitmask of 0xffc07fff, the masked instruction would
                 be 0xa9007bdf. The offset value is (imm7 * 8) */
              if (((w & 0xffc07fff00000000) == 0xa9007bfd00000000) && (c->dwarf.ip > ip + 4))
                {
                  int32_t abs_offset = (w & 0x001f800000000000) >> 47;
                  fs.offset = ((w & 0x0020000000000000)? -abs_offset : abs_offset) * 8;
                }
              else if (((w & 0x00000000ffc07fff) == 0x00000000a9007bfd) && (c->dwarf.ip > ip))
                {
                  int32_t abs_offset = (w & 0x00000000001f8000) >> 15;
                  fs.offset = ((w & 0x0000000000200000)? -abs_offset : abs_offset) * 8;
                }
              else
                fs.offset = 0;

              Debug (4, "ip=0x%lx => frame record stored at SP+0x%x\n", ip, fs.offset);
            }
        }

      if (fs.loc == AT_SP_OFFSET)
        {
          /* If the STP instruction has been executed, but not the instruction
             to update FP, then the SP (with offset) should be used to find the
             frame record, otherwise the FP should be used as there are no
             guarantees that the SP is pointing to the frame record after the FP
             has been updated.

             Two methods for updating the FP have been seen in practice. The
             first is a MOV instruction. From the ARM Architecture Reference
             Manual (ARMv8), the 64-bit MOV (to/from SP) instruction to update
             FP (x29) is 0x910003fd.

             The second is an ADD instruction taking SP and FP as the source and
             destination registers, respectively. From the ARM Architecture
             Reference Manual (ARMv8), the 64-bit ADD (immediate) instruction is

             | 1 | 0 | 0 | 10001 | shift |     imm12    |   Rn  |   Rd  |

             where the values of shift and imm12 are irrelevant for this check.
             The following should be asserted

             Rn == SP (x31) => 0b11111
             Rd == FP (x29) => 0b11101.

             Hence the bitmask should be constructed to assert that the instruction
             is

             | 1 | 0 | 0 | 10001 |   XX  | XXXXXXXXXXXX | 11111 | 11101 |

             So using a bitmask of 0xff0003ff, the masked instruction would be
             0x910003fd.

             Since the MOV instruction is an alias for ADD, it is sufficient to
             only check for the latter case. */
          if ((((w & 0xff0003ff00000000) == 0x910003fd00000000) && (c->dwarf.ip > ip + 4))
           || (((w & 0x00000000ff0003ff) == 0x00000000910003fd) && (c->dwarf.ip > ip)))
            {
              fs.loc = AT_FP;
              fs.offset = 0;

              Debug (4, "ip=0x%lx => frame record stored at FP\n", ip);
            }
        }
      else if (fs.loc == AT_FP)
        {
          /* Check that a 64-bit load pair (LDP) instruction to restore FP and LR has been
             executed, which would indicate that a branch or return instruction is about to
             be executed and that FP is pointing to the caller's frame record instead. The
             LR should be examined for the return address. In this case, the RA must have
             already been authenticated.

             From the ARM Architecture Reference Manual (ARMv8), the format of the 64-bit
             LDP instruction is

             | 10 | 101 | 0 | 0XX | 1 |   imm7  |  Rt2  |   Rn  |   Rt  |

             where X can be 0/1 above to indicate the instruction variant (post-index, pre-
             index or signed offset). imm7 is a 7-bit signed immediate offset, which is
             irrelevant for this check. However, the following should be asserted

             Rt2 == LR (x30) => 0b11110
             Rn  == SP (x31) => 0b11111
             Rt  == FP (x29) => 0b11101.

             Hence the bitmask should be constructed to assert that the instruction is

             | 10 | 101 | 0 | 0XX | 1 | XXXXXXX | 11110 | 11111 | 11101 |

             So using a bitmask of 0xfe407fff, the masked instruction would be 0xa8407bfd */
          if ((((w & 0xfe407fff00000000) == 0xa8407bfd00000000) && (c->dwarf.ip > ip + 4))
           || (((w & 0x00000000fe407fff) == 0x00000000a8407bfd) && (c->dwarf.ip > ip)))
            {
              fs.loc = NONE;
              fs.offset = 0;

              Debug (4, "ip=0x%lx => frame record has been loaded, use LR\n", ip);
            }
        }
    }

  Debug (3, "[start_ip = 0x%lx, ip=0x%lx) => loc = %d, offset = %d\n",
            start_ip, c->dwarf.ip, fs.loc, fs.offset);

  return fs;
}

#if defined __QNX__
/*
 * QNX kernel call have neither CFI nor save frame pointer,
 * 00000000000549e8 <MsgSendsv_r>:
 *  549e8:       cb0203e2        neg     x2, x2
 *  549ec:       52800168        mov     w8, #0xb  // #11
 *  549f0:       d4000a21        svc     #0x51
 *  549f4:       d65f03c0        ret
 *  549f8:       cb0003e0        neg     x0, x0
 *  549fc:       d65f03c0        ret
 *
 * From disassemble, QNX kernel call have mov followed by svc, and may with some
 * neg instructions at beginning.
 * 1. find procedure's ip start,end start
 * 2. search mov,svc from begin, skip any neg instructions
 */
static bool is_neg_instr(uint32_t instr)
{
  /* 64bit register Xd */
  return ((instr & 0xffe003e0) == 0xcb0003e0);
}

/* QNX use w8 to pass kernel call number */
static bool is_mov_w8_instr(uint32_t instr)
{
  /* movz 32bit register Wd */
  return ((instr & 0xffe00008) == 0x52800008);
}

static bool is_svc_instr(uint32_t instr)
{
  return instr == 0xd4000a21;
}

static bool
is_qnx_kercall(struct dwarf_cursor *c)
{
  unw_word_t w0;
  unw_accessors_t *a;
  int ret;
  unw_word_t proc_start_ip;
  unw_word_t proc_end_ip;

  a = unw_get_accessors_int (c->as);
  if (c->as->big_endian || !a->get_proc_ip_range)
    {
      return false;
    }

  ret = (*a->get_proc_ip_range) (c->as, c->ip, &proc_start_ip, &proc_end_ip, c->as_arg);
  if (ret < 0)
    {
      Debug (2, "ip=0x%lx get proc ip range fail, ret = %d\n", c->ip, ret);
      return false;
    }

  unw_word_t ip = proc_start_ip;
  while ((ip < proc_end_ip) && (ip + 8 < proc_end_ip))
    {
      if ((*a->access_mem) (c->as, ip, &w0, 0, c->as_arg) < 0)
        {
          Debug (14, "access_mem ip=0x%lx fail\n", ip);
          return false;
        }

      uint32_t low32 = w0 & 0xffffffff;
      uint32_t high32 = w0 >> 32;

      if (is_mov_w8_instr(low32) && is_svc_instr(high32))
        {
          return true;
        }
      if (is_neg_instr(low32) && is_neg_instr(high32))
        {
          ip += 8;
        }
      else if (is_neg_instr(low32) && is_mov_w8_instr(high32))
        {
          ip += 4;
        }
      else
        {
          return false;
        }
    }

  return false;
}
#endif

/*
 * Save the location of VL (vector length) from the signal frame to the VG (vector
 * granule) register if it exists, otherwise do nothing. If there is an error,
 * the location is also not modified.
 */
#if defined __linux__
static int
get_sve_vl_signal_loc (struct dwarf_cursor* dwarf, unw_word_t sc_addr)
{
  uint32_t size;
  unw_addr_space_t as = dwarf->as;
  unw_accessors_t *a = unw_get_accessors_int (as);
  void *arg = dwarf->as_arg;
  unw_word_t res_addr = sc_addr + LINUX_SC_RESERVED_OFF;

  /*
   * Max possible size of reserved section is 4096. Jump by size of each record
   * until the SVE signal context section is found.
   */
  for(unw_word_t section_off = 0; section_off < 4096; section_off += size)
    {
      uint32_t magic;
      int ret;
      unw_word_t item_addr = res_addr + section_off + LINUX_SC_RESERVED_MAGIC_OFF;
      if ((ret = dwarf_readu32(as, a, &item_addr, &magic, arg)) < 0)
        return ret;
      item_addr = res_addr + section_off + LINUX_SC_RESERVED_SIZE_OFF;
      if ((ret = dwarf_readu32(as, a, &item_addr, &size, arg)) < 0)
        return ret;

      switch(magic)
        {
          case 0:
            /* End marker, size must be 0 */
            return size ? -UNW_EUNSPEC : 1;

          /* Magic number marking the SVE context section */
          case 0x53564501:
            /* Must be big enough so that the 16 bit VL value can be accessed */
            if (size < LINUX_SC_RESERVED_SVE_VL_OFF + 2)
              return -UNW_EUNSPEC;
            dwarf->loc[UNW_AARCH64_VG] = DWARF_MEM_LOC(c->dwarf, res_addr + section_off +
                                                       LINUX_SC_RESERVED_SVE_VL_OFF);
            return 1;

          default:
            /* Don't care about any of the other sections */
            break;
        }
   }
   return 1;
}
#else
static int
get_sve_vl_signal_loc (struct dwarf_cursor* dwarf, unw_word_t sc_addr)
{
   return 1;
}
#endif

static int
aarch64_handle_signal_frame (unw_cursor_t *cursor)
{
  struct cursor *c = (struct cursor *) cursor;
  int i, ret;
  unw_word_t sc_addr, sp, sp_addr = c->dwarf.cfa;
  struct dwarf_loc sp_loc = DWARF_LOC (sp_addr, 0);

  if ((ret = dwarf_get (&c->dwarf, sp_loc, &sp)) < 0)
    return -UNW_EUNSPEC;

  ret = unw_is_signal_frame (cursor);
  Debug(1, "unw_is_signal_frame()=%d\n", ret);
  if (ret > 0)
    {
      c->sigcontext_format = SCF_FORMAT;
      sc_addr = sp_addr + sizeof (siginfo_t) + UC_MCONTEXT_OFF;
    }
  else
    return -UNW_EUNSPEC;

  /* Save the SP and PC to be able to return execution at this point
     later in time (unw_resume).  */
  c->sigcontext_sp = c->dwarf.cfa;
  c->sigcontext_pc = c->dwarf.ip;

  c->sigcontext_addr = sc_addr;
  c->frame_info.frame_type = UNW_AARCH64_FRAME_SIGRETURN;
  c->frame_info.cfa_reg_offset = sc_addr - sp_addr;

  for (i = 0; i < DWARF_NUM_PRESERVED_REGS; ++i)
    c->dwarf.loc[i] = DWARF_NULL_LOC;

  /* Update the dwarf cursor.
     Set the location of the registers to the corresponding addresses of the
     uc_mcontext / sigcontext structure contents.  */
  c->dwarf.loc[UNW_AARCH64_X0]  = DWARF_MEM_LOC (c->dwarf, sc_addr + SC_GPR_OFF);
  c->dwarf.loc[UNW_AARCH64_X1]  = DWARF_MEM_LOC (c->dwarf, sc_addr + SC_GPR_OFF + 8 * 1);
  c->dwarf.loc[UNW_AARCH64_X2]  = DWARF_MEM_LOC (c->dwarf, sc_addr + SC_GPR_OFF + 8 * 2);
  c->dwarf.loc[UNW_AARCH64_X3]  = DWARF_MEM_LOC (c->dwarf, sc_addr + SC_GPR_OFF + 8 * 3);
  c->dwarf.loc[UNW_AARCH64_X4]  = DWARF_MEM_LOC (c->dwarf, sc_addr + SC_GPR_OFF + 8 * 4);
  c->dwarf.loc[UNW_AARCH64_X5]  = DWARF_MEM_LOC (c->dwarf, sc_addr + SC_GPR_OFF + 8 * 5);
  c->dwarf.loc[UNW_AARCH64_X6]  = DWARF_MEM_LOC (c->dwarf, sc_addr + SC_GPR_OFF + 8 * 6);
  c->dwarf.loc[UNW_AARCH64_X7]  = DWARF_MEM_LOC (c->dwarf, sc_addr + SC_GPR_OFF + 8 * 7);
  c->dwarf.loc[UNW_AARCH64_X8]  = DWARF_MEM_LOC (c->dwarf, sc_addr + SC_GPR_OFF + 8 * 8);
  c->dwarf.loc[UNW_AARCH64_X9]  = DWARF_MEM_LOC (c->dwarf, sc_addr + SC_GPR_OFF + 8 * 9);
  c->dwarf.loc[UNW_AARCH64_X10] = DWARF_MEM_LOC (c->dwarf, sc_addr + SC_GPR_OFF + 8 * 10);
  c->dwarf.loc[UNW_AARCH64_X11] = DWARF_MEM_LOC (c->dwarf, sc_addr + SC_GPR_OFF + 8 * 11);
  c->dwarf.loc[UNW_AARCH64_X12] = DWARF_MEM_LOC (c->dwarf, sc_addr + SC_GPR_OFF + 8 * 12);
  c->dwarf.loc[UNW_AARCH64_X13] = DWARF_MEM_LOC (c->dwarf, sc_addr + SC_GPR_OFF + 8 * 13);
  c->dwarf.loc[UNW_AARCH64_X14] = DWARF_MEM_LOC (c->dwarf, sc_addr + SC_GPR_OFF + 8 * 14);
  c->dwarf.loc[UNW_AARCH64_X15] = DWARF_MEM_LOC (c->dwarf, sc_addr + SC_GPR_OFF + 8 * 15);
  c->dwarf.loc[UNW_AARCH64_X16] = DWARF_MEM_LOC (c->dwarf, sc_addr + SC_GPR_OFF + 8 * 16);
  c->dwarf.loc[UNW_AARCH64_X17] = DWARF_MEM_LOC (c->dwarf, sc_addr + SC_GPR_OFF + 8 * 17);
  c->dwarf.loc[UNW_AARCH64_X18] = DWARF_MEM_LOC (c->dwarf, sc_addr + SC_GPR_OFF + 8 * 18);
  c->dwarf.loc[UNW_AARCH64_X19] = DWARF_MEM_LOC (c->dwarf, sc_addr + SC_GPR_OFF + 8 * 19);
  c->dwarf.loc[UNW_AARCH64_X20] = DWARF_MEM_LOC (c->dwarf, sc_addr + SC_GPR_OFF + 8 * 20);
  c->dwarf.loc[UNW_AARCH64_X21] = DWARF_MEM_LOC (c->dwarf, sc_addr + SC_GPR_OFF + 8 * 21);
  c->dwarf.loc[UNW_AARCH64_X22] = DWARF_MEM_LOC (c->dwarf, sc_addr + SC_GPR_OFF + 8 * 22);
  c->dwarf.loc[UNW_AARCH64_X23] = DWARF_MEM_LOC (c->dwarf, sc_addr + SC_GPR_OFF + 8 * 23);
  c->dwarf.loc[UNW_AARCH64_X24] = DWARF_MEM_LOC (c->dwarf, sc_addr + SC_GPR_OFF + 8 * 24);
  c->dwarf.loc[UNW_AARCH64_X25] = DWARF_MEM_LOC (c->dwarf, sc_addr + SC_GPR_OFF + 8 * 25);
  c->dwarf.loc[UNW_AARCH64_X26] = DWARF_MEM_LOC (c->dwarf, sc_addr + SC_GPR_OFF + 8 * 26);
  c->dwarf.loc[UNW_AARCH64_X27] = DWARF_MEM_LOC (c->dwarf, sc_addr + SC_GPR_OFF + 8 * 27);
  c->dwarf.loc[UNW_AARCH64_X28] = DWARF_MEM_LOC (c->dwarf, sc_addr + SC_GPR_OFF + 8 * 28);
  c->dwarf.loc[UNW_AARCH64_X29] = DWARF_MEM_LOC (c->dwarf, sc_addr + SC_X29_OFF);
  c->dwarf.loc[UNW_AARCH64_X30] = DWARF_MEM_LOC (c->dwarf, sc_addr + SC_X30_OFF);
  c->dwarf.loc[UNW_AARCH64_SP]  = DWARF_MEM_LOC (c->dwarf, sc_addr + SC_SP_OFF);
  c->dwarf.loc[UNW_AARCH64_PC]  = DWARF_MEM_LOC (c->dwarf, sc_addr + SC_PC_OFF);
  c->dwarf.loc[UNW_AARCH64_PSTATE]  = DWARF_MEM_LOC (c->dwarf, sc_addr + SC_PSTATE_OFF);
  c->dwarf.loc[UNW_AARCH64_VG]  = DWARF_NULL_LOC;

  /* Set SP/CFA and PC/IP.  */
  dwarf_get (&c->dwarf, c->dwarf.loc[UNW_AARCH64_SP], &c->dwarf.cfa);
  dwarf_get (&c->dwarf, c->dwarf.loc[UNW_AARCH64_PC], &c->dwarf.ip);

  c->dwarf.pi_valid = 0;
  c->dwarf.use_prev_instr = 0;

  return get_sve_vl_signal_loc (&c->dwarf, sc_addr);
}

int
unw_step (unw_cursor_t *cursor)
{
  struct cursor *c = (struct cursor *) cursor;
  int validate = c->validate;
  unw_word_t fp = 0;
  int ret;

  Debug (1, "(cursor=%p, ip=0x%016lx, cfa=0x%016lx))\n",
         c, c->dwarf.ip, c->dwarf.cfa);

  /* Validate all addresses before dereferencing. */
  c->validate = 1;

  /* Check if this is a signal frame. */
  ret = unw_is_signal_frame (cursor);
  if (ret > 0)
    return aarch64_handle_signal_frame (cursor);
  else if (unlikely (ret < 0))
    {
      /* IP points to non-mapped memory. */
      /* This is probably SIGBUS. */
      /* Try to load LR in IP to recover. */
      Debug(1, "Invalid address found in the call stack: 0x%lx\n", c->dwarf.ip);
      dwarf_get (&c->dwarf, c->dwarf.loc[UNW_AARCH64_X30], &c->dwarf.ip);
    }

  /* Try DWARF-based unwinding... */
  c->sigcontext_format = AARCH64_SCF_NONE;
  ret = dwarf_step (&c->dwarf);
  Debug(1, "dwarf_step()=%d\n", ret);

  /* Restore default memory validation state */
  c->validate = validate;

  if (unlikely (ret == -UNW_ESTOPUNWIND))
    return ret;

  if (likely (ret > 0))
    {
      ret = dwarf_get (&c->dwarf, c->dwarf.loc[UNW_AARCH64_X29], &fp);
      if (ret == 0 && fp == 0)
        {
	  /* Procedure Call Standard for the ARM 64-bit Architecture (AArch64)
	   * specifies that the end of the frame record chain is indicated by
	   * the address zero in the address for the previous frame.
	   */
	  c->dwarf.ip = 0;
	  Debug (2, "NULL frame pointer X29 loc, returning 0\n");
	  return 0;
        }
    }

  if (unlikely (ret < 0))
    {
      /* DWARF failed. */

      /*
       * We could get here because of missing/bad unwind information.
       * Validate all addresses before dereferencing.
       */
      if (c->dwarf.as == unw_local_addr_space)
        {
          c->validate = 1;
        }

      if (is_plt_entry (&c->dwarf))
        {
          Debug (2, "found plt entry\n");
          c->frame_info.frame_type = UNW_AARCH64_FRAME_STANDARD;
        }
#if defined __QNX__
      else if (is_qnx_kercall(&c->dwarf))
        {
          Debug (2, "found qnx kernel call, fallback to use link register\n");
          c->frame_info.frame_type = UNW_AARCH64_FRAME_GUESSED;
        }
#endif
      else
        {
	  /* Try use frame record. */
          c->frame_info.frame_type = UNW_AARCH64_FRAME_GUESSED;
	}

      frame_state_t fs = get_frame_state(cursor);

      /* Prefer using frame record. The LR value is stored at an offset of
         8 into the frame record.  */
      if (fs.loc != NONE)
        {
          if (fs.loc == AT_FP)
            {
              /* X29 points to frame record.  */
              ret = dwarf_get (&c->dwarf, c->dwarf.loc[UNW_AARCH64_X29], &fp);
              if (unlikely (ret == 0))
                {
                  if (fp == 0)
                    {
                      /* Procedure Call Standard for the ARM 64-bit Architecture (AArch64)
                       * specifies that the end of the frame record chain is indicated by
                       * the address zero in the address for the previous frame.
                       */
                      c->dwarf.ip = 0;
                      Debug (2, "NULL frame pointer X29 loc, returning 0\n");
                      return 0;
                    }
                }

              for (int i = 0; i < DWARF_NUM_PRESERVED_REGS; ++i)
                c->dwarf.loc[i] = DWARF_NULL_LOC;

              /* Frame record holds X29 and X30 values.  */
              c->dwarf.loc[UNW_AARCH64_X29] = DWARF_MEM_LOC (c->dwarf, fp);
              c->dwarf.loc[UNW_AARCH64_X30] = DWARF_MEM_LOC (c->dwarf, fp + 8);
            }
          else
            {
              /* Frame record stored but not pointed to by X29, use SP.  */
              unw_word_t sp;
              ret = dwarf_get (&c->dwarf, c->dwarf.loc[UNW_AARCH64_SP], &sp);
              if (ret < 0)
                return ret;

              for (int i = 0; i < DWARF_NUM_PRESERVED_REGS; ++i)
                c->dwarf.loc[i] = DWARF_NULL_LOC;

              c->frame_info.cfa_reg_offset = fs.offset;
              c->frame_info.cfa_reg_sp = 1;

              c->dwarf.loc[UNW_AARCH64_X29] = DWARF_MEM_LOC (c->dwarf, sp + fs.offset);
              c->dwarf.loc[UNW_AARCH64_X30] = DWARF_MEM_LOC (c->dwarf, sp + fs.offset + 8);
            }

          c->dwarf.loc[UNW_AARCH64_PC] = c->dwarf.loc[UNW_AARCH64_X30];

          /* Set SP/CFA and PC/IP.  */
          ret = dwarf_get (&c->dwarf, c->dwarf.loc[UNW_AARCH64_X29], &c->dwarf.cfa);
          if (ret < 0)
            return ret;
          ret = dwarf_get (&c->dwarf, c->dwarf.loc[UNW_AARCH64_PC], &c->dwarf.ip);
          if (ret == 0)
            {
              ret = 1;
            }
          Debug (2, "fallback, CFA = 0x%016lx, IP = 0x%016lx returning %d\n",
            c->dwarf.cfa, c->dwarf.ip, ret);
          return ret;
        }

      /* No frame record, fallback to link register (X30).  */
      c->frame_info.cfa_reg_offset = 0;
      c->frame_info.cfa_reg_sp = 0;
      c->frame_info.fp_cfa_offset = -1;
      c->frame_info.lr_cfa_offset = -1;
      c->frame_info.sp_cfa_offset = -1;
      c->dwarf.loc[UNW_AARCH64_PC] = c->dwarf.loc[UNW_AARCH64_X30];
      c->dwarf.loc[UNW_AARCH64_X30] = DWARF_NULL_LOC;
      if (!DWARF_IS_NULL_LOC (c->dwarf.loc[UNW_AARCH64_PC]))
        {
          ret = dwarf_get (&c->dwarf, c->dwarf.loc[UNW_AARCH64_PC], &c->dwarf.ip);
          if (ret < 0)
            {
              Debug (2, "failed to get pc from link register: %d\n", ret);
              return ret;
            }
          Debug (2, "link register (x30) = 0x%016lx\n", c->dwarf.ip);
          ret = 1;
        }
      else
        c->dwarf.ip = 0;
    }

  return (c->dwarf.ip == 0) ? 0 : 1;
}
