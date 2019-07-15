/* libunwind - a platform-independent unwind library
   Copyright (c) 2002-2004 Hewlett-Packard Development Company, L.P.
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

HIDDEN int
tdep_access_reg (struct cursor *c, unw_regnum_t reg, unw_word_t *valp,
                 int write)
{
  dwarf_loc_t loc = DWARF_NULL_LOC;

  switch (reg)
    {
    case UNW_S390X_CFA:
      if (write)
        return -UNW_EREADONLYREG;
      *valp = c->dwarf.cfa;
      return 0;

    case UNW_S390X_R0:
    case UNW_S390X_R1:
    case UNW_S390X_R2:
    case UNW_S390X_R3:
    case UNW_S390X_R4:
    case UNW_S390X_R5:
    case UNW_S390X_R6:
    case UNW_S390X_R7:
    case UNW_S390X_R8:
    case UNW_S390X_R9:
    case UNW_S390X_R10:
    case UNW_S390X_R11:
    case UNW_S390X_R12:
    case UNW_S390X_R13:
    case UNW_S390X_R14:
    case UNW_S390X_IP:
      loc = c->dwarf.loc[reg];
      break;

    case UNW_S390X_R15:
      if (write)
        return -UNW_EREADONLYREG;
      loc = c->dwarf.loc[reg];
      break;

    default:
      Debug (1, "bad register number %u\n", reg);
      return -UNW_EBADREG;
    }

  if (write)
    return dwarf_put (&c->dwarf, loc, *valp);
  else
    return dwarf_get (&c->dwarf, loc, valp);
}

HIDDEN int
tdep_access_fpreg (struct cursor *c, unw_regnum_t reg, unw_fpreg_t *valp,
                   int write)
{
  dwarf_loc_t loc = DWARF_NULL_LOC;

  switch (reg)
    {
    case UNW_S390X_F0:
    case UNW_S390X_F1:
    case UNW_S390X_F2:
    case UNW_S390X_F3:
    case UNW_S390X_F4:
    case UNW_S390X_F5:
    case UNW_S390X_F6:
    case UNW_S390X_F7:
    case UNW_S390X_F8:
    case UNW_S390X_F9:
    case UNW_S390X_F10:
    case UNW_S390X_F11:
    case UNW_S390X_F12:
    case UNW_S390X_F13:
    case UNW_S390X_F14:
    case UNW_S390X_F15:
      loc = c->dwarf.loc[reg];
      break;
    default:
      Debug (1, "bad register number %u\n", reg);
      return -UNW_EBADREG;
    }

  if (write)
    return dwarf_putfp (&c->dwarf, loc, *valp);
  else
    return dwarf_getfp (&c->dwarf, loc, valp);
}
