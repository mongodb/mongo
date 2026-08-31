/* libunwind - a platform-independent unwind library
   Copyright (C) 2002-2004 Hewlett-Packard Co
        Contributed by David Mosberger-Tang <davidm@hpl.hp.com>

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

#ifndef LIBUNWIND_H
#define LIBUNWIND_H

#if defined(__cplusplus) || defined(c_plusplus)
extern "C" {
#endif

#include <sys/types.h>
#include <inttypes.h>
#include <ucontext.h>

#ifndef UNW_EMPTY_STRUCT
#  define UNW_EMPTY_STRUCT uint8_t unused;
#endif

#define UNW_TARGET              s390x
#define UNW_TARGET_S390X        1

#define _U_TDEP_QP_TRUE 0       /* see libunwind-dynamic.h  */

/* This needs to be big enough to accommodate "struct cursor", while
   leaving some slack for future expansion.  Changing this value will
   require recompiling all users of this library.  Stack allocation is
   relatively cheap and unwind-state copying is relatively rare, so we
   want to err on making it rather too big than too small.  */
#define UNW_TDEP_CURSOR_LEN     384

typedef uint64_t unw_word_t;
typedef int64_t unw_sword_t;

typedef double unw_tdep_fpreg_t;

#define UNW_WORD_MAX UINT64_MAX

typedef enum
  {
    /* general purpose registers */
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

    /* floating point registers */
    UNW_S390X_F0,
    UNW_S390X_F1,
    UNW_S390X_F2,
    UNW_S390X_F3,
    UNW_S390X_F4,
    UNW_S390X_F5,
    UNW_S390X_F6,
    UNW_S390X_F7,
    UNW_S390X_F8,
    UNW_S390X_F9,
    UNW_S390X_F10,
    UNW_S390X_F11,
    UNW_S390X_F12,
    UNW_S390X_F13,
    UNW_S390X_F14,
    UNW_S390X_F15,

    /* PSW */
    UNW_S390X_IP,

    UNW_TDEP_LAST_REG = UNW_S390X_IP,

    /* TODO: access, vector registers */

    /* frame info (read-only) */
    UNW_S390X_CFA,

    UNW_TDEP_IP = UNW_S390X_IP,
    UNW_TDEP_SP = UNW_S390X_R15,

    /* TODO: placeholders */
    UNW_TDEP_EH = UNW_S390X_R0,
  }
s390x_regnum_t;

#define UNW_TDEP_NUM_EH_REGS    2       /* XXX Not sure what this means */

typedef struct unw_tdep_save_loc
  {
    /* Additional target-dependent info on a save location.  */
    UNW_EMPTY_STRUCT
  }
unw_tdep_save_loc_t;

/* On s390x, we can directly use ucontext_t as the unwind context.  */
typedef ucontext_t unw_tdep_context_t;

typedef struct
  {
    /* no s390x-specific auxiliary proc-info */
    UNW_EMPTY_STRUCT
  }
unw_tdep_proc_info_t;

#include "libunwind-dynamic.h"
#include "libunwind-common.h"

#define unw_tdep_getcontext             UNW_ARCH_OBJ(getcontext)
#define unw_tdep_is_fpreg               UNW_ARCH_OBJ(is_fpreg)

extern int unw_tdep_getcontext (unw_tdep_context_t *);
extern int unw_tdep_is_fpreg (int);

#if defined(__cplusplus) || defined(c_plusplus)
}
#endif

#endif /* LIBUNWIND_H */
