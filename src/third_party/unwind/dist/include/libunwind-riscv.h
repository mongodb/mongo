/* libunwind - a platform-independent unwind library
   Copyright (C) 2002-2004 Hewlett-Packard Co
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

#ifndef LIBUNWIND_H
#define LIBUNWIND_H

#if defined(__cplusplus) || defined(c_plusplus)
extern "C" {
#endif

#include <sys/types.h>
#include <inttypes.h>
#include <stdint.h>
#include <ucontext.h>

#ifndef UNW_EMPTY_STRUCT
#  define UNW_EMPTY_STRUCT uint8_t unused;
#endif

#define UNW_TARGET              riscv
#define UNW_TARGET_RISCV      1

#define _U_TDEP_QP_TRUE 0       /* ignored - see libunwind-dynamic.h */

/* This needs to be big enough to accommodate "struct cursor", while
   leaving some slack for future expansion.  Changing this value will
   require recompiling all users of this library.  Stack allocation is
   relatively cheap and unwind-state copying is relatively rare, so we
   want to err on making it rather too big than too small.  */
/* FIXME for riscv: Figure out a more reasonable size */
#define UNW_TDEP_CURSOR_LEN     4096

#if __riscv_xlen == 32
typedef uint32_t unw_word_t;
typedef int32_t unw_sword_t;
# define UNW_WORD_MAX UINT32_MAX
#elif __riscv_xlen == 64
typedef uint64_t unw_word_t;
typedef int64_t unw_sword_t;
# define UNW_WORD_MAX UINT64_MAX
#endif

#if __riscv_flen == 64
typedef double unw_tdep_fpreg_t;
#elif __riscv_flen == 32
typedef float unw_tdep_fpreg_t;
#else
# error "Unsupported RISC-V floating-point size"
#endif

/* Also see src/riscv/Gglobal.c. This ordering is consistent with
   https://github.com/riscv/riscv-elf-psabi-doc/blob/74ecf07bcebd0cb4bf3c39f3f9d96946cd6aba61/riscv-elf.md#dwarf-register-numbers- */

typedef enum
  {
    /* integer registers */
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

    /* floating point registers */
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

    UNW_RISCV_PC,

    UNW_TDEP_LAST_REG = UNW_RISCV_PC,

    /* The CFA is the value of SP in previous frame */
    UNW_RISCV_CFA = UNW_RISCV_X2,

    UNW_TDEP_IP = UNW_RISCV_PC,
    UNW_TDEP_SP = UNW_RISCV_X2,
    UNW_TDEP_EH = UNW_RISCV_X10,
  }
riscv_regnum_t;

/* https://github.com/gcc-mirror/gcc/blob/16e2427f50c208dfe07d07f18009969502c25dc8/gcc/config/riscv/riscv.h#L104-L106 */
#define UNW_TDEP_NUM_EH_REGS    4

typedef struct unw_tdep_save_loc
  {
    /* Additional target-dependent info on a save location.  */
    UNW_EMPTY_STRUCT
  }
unw_tdep_save_loc_t;

/* On riscv, we can directly use ucontext_t as the unwind context.  */
typedef ucontext_t unw_tdep_context_t;

typedef struct
  {
    /* no riscv-specific auxiliary proc-info */
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
