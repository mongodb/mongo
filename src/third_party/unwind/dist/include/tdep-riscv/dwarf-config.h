/* libunwind - a platform-independent unwind library
   Copyright (c) 2003, 2005 Hewlett-Packard Development Company, L.P.
        Contributed by David Mosberger-Tang <davidm@hpl.hp.com>

   Modified for riscv by Zhaofeng Li <hello@zhaofeng.li>

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

#ifndef dwarf_config_h
#define dwarf_config_h

/* 32 integer registers + 32 floating-point registers + 2 pseudo-registers */
#define DWARF_NUM_PRESERVED_REGS        66

#define DWARF_REGNUM_MAP_LENGTH         DWARF_NUM_PRESERVED_REGS

/* Not big-endian. */
#define dwarf_is_big_endian(addr_space) 0

/* Convert a pointer to a dwarf_cursor structure to a pointer to
   unw_cursor_t.  */
#define dwarf_to_cursor(c)      ((unw_cursor_t *) (c))

typedef struct dwarf_loc
  {
    unw_word_t val;
    unw_word_t type;            /* see RISCV_LOC_TYPE_* macros.  */
  }
dwarf_loc_t;

#endif /* dwarf_config_h */
