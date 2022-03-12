/* libunwind - a platform-independent unwind library
   Copyright (C) 2004 Hewlett-Packard Co
        Contributed by Zhaofeng Li <hello@zhaofeng.li>

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

#if defined __linux__

/* https://sourceware.org/git/?p=glibc.git;a=blob;f=sysdeps/riscv/setjmp.S;h=0b92016b311b11aa9eeb62b38c670a262f1924c9;hb=HEAD */
#define JB_SP           13
#define JB_RP           0

#if __riscv_xlen == 64

/* GCC's internal structure for this depends on the floating-point ABI:
   https://sourceware.org/git/?p=glibc.git;a=blob;f=sysdeps/unix/sysv/linux/riscv/rv64/jmp_buf-macros.h;h=be9199e514bf4f15f0612327b8b762e29a2b7862;hb=HEAD
*/

#if defined __riscv_float_abi_double
# define JB_MASK_SAVED  (208>>3)
# define JB_MASK        (216>>3)
#else
# error "Unsupported RISC-V floating point ABI"
#endif /* __riscv_float_abi_double */

#else
# error "Add offsets here"
#endif /* __riscv_xlen */

#endif
