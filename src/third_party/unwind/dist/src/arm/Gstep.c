/* libunwind - a platform-independent unwind library
   Copyright (C) 2008 CodeSourcery
   Copyright 2011 Linaro Limited
   Copyright (C) 2012 Tommi Rantala <tt.rantala@gmail.com>

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
#include "ex_tables.h"

#include <signal.h>

#define arm_exidx_step  UNW_OBJ(arm_exidx_step)

static inline int
arm_exidx_step (struct cursor *c)
{
  unw_word_t old_ip, old_cfa;
  uint8_t buf[32];
  int ret;

  old_ip = c->dwarf.ip;
  old_cfa = c->dwarf.cfa;

  /* mark PC unsaved */
  c->dwarf.loc[UNW_ARM_R15] = DWARF_NULL_LOC;
  unw_word_t ip = c->dwarf.ip;
  if (c->dwarf.use_prev_instr)
    /* The least bit denotes thumb/arm mode, clear it. */
    ip = (ip & ~(unw_word_t)0x1) - 1;

  /* check dynamic info first --- it overrides everything else */
  ret = unwi_find_dynamic_proc_info (c->dwarf.as, ip, &c->dwarf.pi, 1,
                                     c->dwarf.as_arg);
  if (ret == -UNW_ENOINFO)
    {
#ifdef UNW_LOCAL_ONLY
      if ((ret = arm_find_proc_info2 (c->dwarf.as, ip, &c->dwarf.pi,
                                      1, c->dwarf.as_arg,
                                      UNW_ARM_METHOD_EXIDX)) < 0)
        return ret;
#else
      if ((ret = tdep_find_proc_info (&c->dwarf, ip, 1)) < 0)
        return ret;
#endif
    }

  if (c->dwarf.pi.format != UNW_INFO_FORMAT_ARM_EXIDX)
    return -UNW_ENOINFO;

  ret = arm_exidx_extract (&c->dwarf, buf);
  if (ret < 0)
    return ret;

  ret = arm_exidx_decode (buf, ret, &c->dwarf);
  if (ret < 0)
    return ret;

  if (c->dwarf.ip == old_ip && c->dwarf.cfa == old_cfa)
    {
      Dprintf ("%s: ip and cfa unchanged; stopping here (ip=0x%lx)\n",
               __FUNCTION__, (long) c->dwarf.ip);
      return -UNW_EBADFRAME;
    }

  c->dwarf.pi_valid = 0;

  return (c->dwarf.ip == 0) ? 0 : 1;
}

int
unw_step (unw_cursor_t *cursor)
{
  struct cursor *c = (struct cursor *) cursor;
  int ret = -UNW_EUNSPEC;
  int has_stopunwind = 0;
  int validate = c->validate;
  c->validate = 1;

  Debug (1, "(cursor=%p)\n", c);

  /* Check if this is a signal frame. */
  if (unw_is_signal_frame (cursor) > 0)
     return arm_handle_signal_frame (cursor);

  /* First, try extbl-based unwinding. */
  if (UNW_TRY_METHOD (UNW_ARM_METHOD_EXIDX))
    {
      ret = arm_exidx_step (c);
      Debug(1, "arm_exidx_step()=%d\n", ret);
      if (ret > 0)
        {
          c->validate = validate;
          return 1;
        }
      if (ret == 0)
        {
          c->validate = validate;
          return ret;
        }
      if (ret == -UNW_ESTOPUNWIND)
        has_stopunwind = 1;
    }

#ifdef CONFIG_DEBUG_FRAME
  /* Second, try DWARF-based unwinding. */
  if (UNW_TRY_METHOD(UNW_ARM_METHOD_DWARF))
    {
      Debug (13, "%s(ret=%d), trying extbl\n",
             UNW_TRY_METHOD(UNW_ARM_METHOD_EXIDX) ? "arm_exidx_step() failed " : "",
             ret);
      ret = dwarf_step (&c->dwarf);
      Debug(1, "dwarf_step()=%d\n", ret);

      if (likely (ret > 0))
        {
          c->validate = validate;
          return 1;
        }

      if (ret < 0 && ret != -UNW_ENOINFO)
        {
          Debug (2, "returning %d\n", ret);
          c->validate = validate;
          return ret;
        }
    }
#endif /* CONFIG_DEBUG_FRAME */

  c->validate = validate;
  // Before trying the fallback, if any unwind info tell us to stop, do that.
  if (has_stopunwind)
    return -UNW_ESTOPUNWIND;

  /* Fall back on APCS frame parsing.
     Note: This won't work in case the ARM EABI is used. */
#ifdef __FreeBSD__
  if (0)
#else
  if (unlikely (ret < 0))
#endif
    {
      if (UNW_TRY_METHOD(UNW_ARM_METHOD_FRAME))
        {
          Debug (13, "%s%s%s%s(ret=%d), trying frame-chain\n",
                 UNW_TRY_METHOD(UNW_ARM_METHOD_EXIDX) ? "arm_exidx_step() " : "",
                 (UNW_TRY_METHOD(UNW_ARM_METHOD_EXIDX) && UNW_TRY_METHOD(UNW_ARM_METHOD_DWARF)) ? "and " : "",
                 UNW_TRY_METHOD(UNW_ARM_METHOD_DWARF) ? "dwarf_step() " : "",
                 (UNW_TRY_METHOD(UNW_ARM_METHOD_EXIDX) || UNW_TRY_METHOD(UNW_ARM_METHOD_DWARF)) ? "failed " : "",
                 ret);
          ret = UNW_ESUCCESS;
          /* EXIDX and/or DWARF unwinding failed, try to follow APCS/optimized APCS frame chain */
          unw_word_t instr, i;
          dwarf_loc_t ip_loc, fp_loc;
          unw_word_t frame;
          /* Mark all registers unsaved, since we don't know where
             they are saved (if at all), except for the EBP and
             EIP.  */
          if (dwarf_get(&c->dwarf, c->dwarf.loc[UNW_ARM_R11], &frame) < 0)
            {
              return 0;
            }
          for (i = 0; i < DWARF_NUM_PRESERVED_REGS; ++i) {
            c->dwarf.loc[i] = DWARF_NULL_LOC;
          }
          if (frame)
            {
              if (dwarf_get(&c->dwarf, DWARF_LOC(frame, 0), &instr) < 0)
                {
                  return 0;
                }
              instr -= 8;
              if (dwarf_get(&c->dwarf, DWARF_LOC(instr, 0), &instr) < 0)
                {
                  return 0;
                }
              if ((instr & 0xFFFFD800) == 0xE92DD800)
                {
                  /* Standard APCS frame. */
                  ip_loc = DWARF_LOC(frame - 4, 0);
                  fp_loc = DWARF_LOC(frame - 12, 0);
                }
              else
                {
                  /* Codesourcery optimized normal frame. */
                  ip_loc = DWARF_LOC(frame, 0);
                  fp_loc = DWARF_LOC(frame - 4, 0);
                }
              if (dwarf_get(&c->dwarf, ip_loc, &c->dwarf.ip) < 0)
                {
                  return 0;
                }
              c->dwarf.loc[UNW_ARM_R12] = ip_loc;
              c->dwarf.loc[UNW_ARM_R11] = fp_loc;
              c->dwarf.pi_valid = 0;
              Debug(15, "ip=%x\n", c->dwarf.ip);
            }
          else
            {
              ret = -UNW_ENOINFO;
            }
        }
    }
  return ret == -UNW_ENOINFO ? 0 : ret;
}
