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

#include "unwind_i.h"

/* FIXME for LoongArch64.  */

int
unw_get_save_loc (unw_cursor_t *cursor, int reg, unw_save_loc_t *sloc)
{
  struct cursor *c = (struct cursor *) cursor;
  dwarf_loc_t loc;

  loc = DWARF_NULL_LOC;         /* default to "not saved" */

  switch (reg)
    {
    case UNW_LOONGARCH64_R0:
    case UNW_LOONGARCH64_R1:
    case UNW_LOONGARCH64_R2:
    case UNW_LOONGARCH64_R3:
    case UNW_LOONGARCH64_R4:
    case UNW_LOONGARCH64_R5:
    case UNW_LOONGARCH64_R6:
    case UNW_LOONGARCH64_R7:
    case UNW_LOONGARCH64_R8:
    case UNW_LOONGARCH64_R9:
    case UNW_LOONGARCH64_R10:
    case UNW_LOONGARCH64_R11:
    case UNW_LOONGARCH64_R12:
    case UNW_LOONGARCH64_R13:
    case UNW_LOONGARCH64_R14:
    case UNW_LOONGARCH64_R15:
    case UNW_LOONGARCH64_R16:
    case UNW_LOONGARCH64_R17:
    case UNW_LOONGARCH64_R18:
    case UNW_LOONGARCH64_R19:
    case UNW_LOONGARCH64_R20:
    case UNW_LOONGARCH64_R21:
    case UNW_LOONGARCH64_R22:
    case UNW_LOONGARCH64_R23:
    case UNW_LOONGARCH64_R24:
    case UNW_LOONGARCH64_R25:
    case UNW_LOONGARCH64_R26:
    case UNW_LOONGARCH64_R27:
    case UNW_LOONGARCH64_R28:
    case UNW_LOONGARCH64_R29:
    case UNW_LOONGARCH64_R30:
    case UNW_LOONGARCH64_R31:
    case UNW_LOONGARCH64_PC:
      loc = c->dwarf.loc[reg - UNW_LOONGARCH64_R0];
      break;

    default:
      break;
    }

  memset (sloc, 0, sizeof (*sloc));

  if (DWARF_IS_NULL_LOC (loc))
    {
      sloc->type = UNW_SLT_NONE;
      return 0;
    }

#if !defined(UNW_LOCAL_ONLY)
  if (DWARF_IS_REG_LOC (loc))
    {
      sloc->type = UNW_SLT_REG;
      sloc->u.regnum = DWARF_GET_LOC (loc);
    }
  else
#endif
    {
      sloc->type = UNW_SLT_MEMORY;
      sloc->u.addr = DWARF_GET_LOC (loc);
    }
  return 0;
}
