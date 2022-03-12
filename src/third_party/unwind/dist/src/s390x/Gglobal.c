/* libunwind - a platform-independent unwind library
   Copyright (c) 2003, 2005 Hewlett-Packard Development Company, L.P.
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

#include "config.h"
#include "unwind_i.h"
#include "dwarf_i.h"

HIDDEN define_lock (s390x_lock);
HIDDEN atomic_bool tdep_init_done = 0;

/* The API register numbers are exactly the same as the .eh_frame
   registers, for now at least.  */
HIDDEN const uint8_t dwarf_to_unw_regnum_map[DWARF_NUM_PRESERVED_REGS] =
  {
    UNW_S390X_R0,
    UNW_S390X_R1,
    UNW_S390X_R2,
    UNW_S390X_R3,
    UNW_S390X_R4,
    UNW_S390X_R5,
    UNW_S390X_R6,
    UNW_S390X_R7,
    UNW_S390X_R8,
    UNW_S390X_R9,
    UNW_S390X_R10,
    UNW_S390X_R11,
    UNW_S390X_R12,
    UNW_S390X_R13,
    UNW_S390X_R14,
    UNW_S390X_R15,

    UNW_S390X_F0,
    UNW_S390X_F2,
    UNW_S390X_F4,
    UNW_S390X_F6,
    UNW_S390X_F1,
    UNW_S390X_F3,
    UNW_S390X_F5,
    UNW_S390X_F7,
    UNW_S390X_F8,
    UNW_S390X_F10,
    UNW_S390X_F12,
    UNW_S390X_F14,
    UNW_S390X_F9,
    UNW_S390X_F11,
    UNW_S390X_F13,
    UNW_S390X_F15,
  };

HIDDEN void
tdep_init (void)
{
  intrmask_t saved_mask;

  sigfillset (&unwi_full_mask);

  lock_acquire (&s390x_lock, saved_mask);
  {
    if (atomic_load(&tdep_init_done))
      /* another thread else beat us to it... */
      goto out;

    mi_init ();

    dwarf_init ();

    tdep_init_mem_validate ();

#ifndef UNW_REMOTE_ONLY
    s390x_local_addr_space_init ();
#endif
    atomic_store(&tdep_init_done, 1); /* signal that we're initialized... */
  }
 out:
  lock_release (&s390x_lock, saved_mask);
}
