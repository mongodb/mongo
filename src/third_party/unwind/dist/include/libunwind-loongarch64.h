/* libunwind - a platform-independent unwind library
   Copyright (C) 2001-2004 Hewlett-Packard Co
        Contributed by David Mosberger-Tang <davidm@hpl.hp.com>
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

#ifndef LIBUNWIND_H
#define LIBUNWIND_H

#if defined(__cplusplus) || defined(c_plusplus)
extern "C" {
#endif

#include <inttypes.h>
#include <stdint.h>
#include <ucontext.h>

#ifndef UNW_EMPTY_STRUCT
#  define UNW_EMPTY_STRUCT uint8_t unused;
#endif

#define UNW_TARGET      loongarch64
#define UNW_TARGET_LOONGARCH64 1

#define _U_TDEP_QP_TRUE 0       /* see libunwind-dynamic.h  */

/* This needs to be big enough to accommodate "struct cursor", while
   leaving some slack for future expansion.  Changing this value will
   require recompiling all users of this library.  Stack allocation is
   relatively cheap and unwind-state copying is relatively rare, so we
   want to err on making it rather too big than too small.  */

/* FIXME for LoongArch64. Too big?  What do other things use for similar tasks?  */
#define UNW_TDEP_CURSOR_LEN     4096

typedef uint64_t unw_word_t;
typedef int64_t unw_sword_t;

typedef long double unw_tdep_fpreg_t;

#define UNW_WORD_MAX UINT64_MAX

typedef enum
  {
    UNW_LOONGARCH64_R0,
    UNW_LOONGARCH64_R1,
    UNW_LOONGARCH64_R2,
    UNW_LOONGARCH64_R3,
    UNW_LOONGARCH64_R4,
    UNW_LOONGARCH64_R5,
    UNW_LOONGARCH64_R6,
    UNW_LOONGARCH64_R7,
    UNW_LOONGARCH64_R8,
    UNW_LOONGARCH64_R9,
    UNW_LOONGARCH64_R10,
    UNW_LOONGARCH64_R11,
    UNW_LOONGARCH64_R12,
    UNW_LOONGARCH64_R13,
    UNW_LOONGARCH64_R14,
    UNW_LOONGARCH64_R15,
    UNW_LOONGARCH64_R16,
    UNW_LOONGARCH64_R17,
    UNW_LOONGARCH64_R18,
    UNW_LOONGARCH64_R19,
    UNW_LOONGARCH64_R20,
    UNW_LOONGARCH64_R21,
    UNW_LOONGARCH64_R22,
    UNW_LOONGARCH64_R23,
    UNW_LOONGARCH64_R24,
    UNW_LOONGARCH64_R25,
    UNW_LOONGARCH64_R26,
    UNW_LOONGARCH64_R27,
    UNW_LOONGARCH64_R28,
    UNW_LOONGARCH64_R29,
    UNW_LOONGARCH64_R30,
    UNW_LOONGARCH64_R31,

    UNW_LOONGARCH64_PC = 33,

    /* FIXME: Other registers!  */

    /* For LoongArch64, the CFA is the value of SP (r3) at the call site in the
       previous frame.  */
    UNW_LOONGARCH64_CFA,

    UNW_TDEP_LAST_REG = UNW_LOONGARCH64_PC,

    UNW_TDEP_IP = UNW_LOONGARCH64_R1,
    UNW_TDEP_SP = UNW_LOONGARCH64_R3,
    UNW_TDEP_EH = UNW_LOONGARCH64_R0   /* FIXME.  */
  }
loongarch64_regnum_t;

#define UNW_TDEP_NUM_EH_REGS	4

typedef struct unw_tdep_save_loc
  {
    /* Additional target-dependent info on a save location.  */
    UNW_EMPTY_STRUCT
  }
unw_tdep_save_loc_t;

/* On LoongArch64, we can directly use ucontext_t as the unwind context. */
typedef ucontext_t unw_tdep_context_t;

typedef struct
  {
    /* no loongarch64-specific auxiliary proc-info */
    UNW_EMPTY_STRUCT
  }
unw_tdep_proc_info_t;

#include "libunwind-dynamic.h"
#include "libunwind-common.h"

#define unw_tdep_getcontext		UNW_ARCH_OBJ(getcontext)
extern int unw_tdep_getcontext (ucontext_t *uc);

#define unw_tdep_is_fpreg		UNW_ARCH_OBJ(is_fpreg)
extern int unw_tdep_is_fpreg (int);

#if defined(__cplusplus) || defined(c_plusplus)
}
#endif

#endif /* LIBUNWIND_H */
