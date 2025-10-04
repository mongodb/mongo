/* libunwind - a platform-independent unwind library
   Copyright (C) 2008 CodeSourcery
   Copyright (C) 2011-2013 Linaro Limited
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

static inline void
establish_machine_state (struct cursor *c)
{
  unw_addr_space_t as = c->dwarf.as;
  void *arg = c->dwarf.as_arg;
  unw_fpreg_t fpval;
  unw_word_t val;
  int reg;

  Debug (8, "copying out cursor state\n");

  for (reg = 0; reg <= UNW_AARCH64_RA_SIGN_STATE; ++reg)
    {
      Debug (16, "copying %s %d\n", unw_regname (reg), reg);
      if (tdep_access_reg (c, reg, &val, 0) >= 0)
        as->acc.access_reg (as, reg, &val, 1, arg);
    }
  for (reg = UNW_AARCH64_V0; reg <= UNW_AARCH64_V31; ++reg)
    {
      Debug (16, "copying %s %d\n", unw_regname (reg), reg);
      if (tdep_access_fpreg (c, reg, &fpval, 0) >= 0)
        as->acc.access_fpreg (as, reg, &fpval, 1, arg);
    }
}

int
unw_resume (unw_cursor_t *cursor)
{
  struct cursor *c = (struct cursor *) cursor;

  Debug (1, "(cursor=%p)\n", c);

  if (!c->dwarf.ip)
    {
      /* This can happen easily when the frame-chain gets truncated
         due to bad or missing unwind-info.  */
      Debug (1, "refusing to resume execution at address 0\n");
      return -UNW_EINVAL;
    }

Debug(1, "==smw> before establish_machine_state\n");
  establish_machine_state (c);

Debug(1, "==smw> before acc.resume\n");
  return (*c->dwarf.as->acc.resume) (c->dwarf.as, (unw_cursor_t *) c,
                                     c->dwarf.as_arg);
}
