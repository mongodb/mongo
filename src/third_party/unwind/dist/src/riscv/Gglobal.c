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
#include "dwarf_i.h"

HIDDEN define_lock (riscv_lock);
HIDDEN atomic_bool tdep_init_done = 0;

/* Our ordering is already consistent with
   https://github.com/riscv/riscv-elf-psabi-doc/blob/74ecf07bcebd0cb4bf3c39f3f9d96946cd6aba61/riscv-elf.md#dwarf-register-numbers- */
HIDDEN const uint8_t dwarf_to_unw_regnum_map[] =
  {
    UNW_RISCV_X0,
    UNW_RISCV_X1,
    UNW_RISCV_X2,
    UNW_RISCV_X3,
    UNW_RISCV_X4,
    UNW_RISCV_X5,
    UNW_RISCV_X6,
    UNW_RISCV_X7,
    UNW_RISCV_X8,
    UNW_RISCV_X9,
    UNW_RISCV_X10,
    UNW_RISCV_X11,
    UNW_RISCV_X12,
    UNW_RISCV_X13,
    UNW_RISCV_X14,
    UNW_RISCV_X15,
    UNW_RISCV_X16,
    UNW_RISCV_X17,
    UNW_RISCV_X18,
    UNW_RISCV_X19,
    UNW_RISCV_X20,
    UNW_RISCV_X21,
    UNW_RISCV_X22,
    UNW_RISCV_X23,
    UNW_RISCV_X24,
    UNW_RISCV_X25,
    UNW_RISCV_X26,
    UNW_RISCV_X27,
    UNW_RISCV_X28,
    UNW_RISCV_X29,
    UNW_RISCV_X30,
    UNW_RISCV_X31,

    UNW_RISCV_F0,
    UNW_RISCV_F1,
    UNW_RISCV_F2,
    UNW_RISCV_F3,
    UNW_RISCV_F4,
    UNW_RISCV_F5,
    UNW_RISCV_F6,
    UNW_RISCV_F7,
    UNW_RISCV_F8,
    UNW_RISCV_F9,
    UNW_RISCV_F10,
    UNW_RISCV_F11,
    UNW_RISCV_F12,
    UNW_RISCV_F13,
    UNW_RISCV_F14,
    UNW_RISCV_F15,
    UNW_RISCV_F16,
    UNW_RISCV_F17,
    UNW_RISCV_F18,
    UNW_RISCV_F19,
    UNW_RISCV_F20,
    UNW_RISCV_F21,
    UNW_RISCV_F22,
    UNW_RISCV_F23,
    UNW_RISCV_F24,
    UNW_RISCV_F25,
    UNW_RISCV_F26,
    UNW_RISCV_F27,
    UNW_RISCV_F28,
    UNW_RISCV_F29,
    UNW_RISCV_F30,
    UNW_RISCV_F31,
  };

HIDDEN void
tdep_init (void)
{
  intrmask_t saved_mask;

  sigfillset (&unwi_full_mask);

  lock_acquire (&riscv_lock, saved_mask);

  if (atomic_load(&tdep_init_done))
    /* another thread else beat us to it... */
    goto out;

  mi_init ();
  dwarf_init ();
  tdep_init_mem_validate ();

#ifndef UNW_REMOTE_ONLY
  riscv_local_addr_space_init ();
#endif
  atomic_store(&tdep_init_done, 1);  /* signal that we're initialized... */

 out:
  lock_release (&riscv_lock, saved_mask);
}
