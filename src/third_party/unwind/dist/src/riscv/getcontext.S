/* libunwind - a platform-independent unwind library
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

#include "offsets.h"
#include "asm.h"

#define REG(X) (UC_MCONTEXT_REGS_OFF + SZREG * X)(a0)
#define FREG(X) (UC_MCONTEXT_REGS_OFF + SZREG * 32 + SZFREG * X)(a0)

	.text
	.global _Uriscv_getcontext
	.type   _Uriscv_getcontext, @function
_Uriscv_getcontext:
	.cfi_startproc

	STORE ra, REG(0)
	STORE ra, REG(1)
	STORE sp, REG(2)
	STORE s0, REG(8)
	STORE s1, REG(9)
	STORE x0, REG(10) /* Write 0 to a0 */
	STORE a1, REG(11)
	STORE a2, REG(12)
	STORE a3, REG(13)
	STORE a4, REG(14)
	STORE a5, REG(15)
	STORE a6, REG(16)
	STORE a7, REG(17)
	STORE s2, REG(18)
	STORE s3, REG(19)
	STORE s4, REG(20)
	STORE s5, REG(21)
	STORE s6, REG(22)
	STORE s7, REG(23)
	STORE s8, REG(24)
	STORE s9, REG(25)
	STORE s10, REG(26)
	STORE s11, REG(27)

#ifdef STORE_FP
	/* The FCSR is always 32-bits and comes after all registers */
	frcsr a1
	sw a1, FREG(32)

	STORE_FP fs0, FREG(8)
	STORE_FP fs1, FREG(9)
	STORE_FP fs2, FREG(18)
	STORE_FP fs3, FREG(19)
	STORE_FP fs4, FREG(20)
	STORE_FP fs5, FREG(21)
	STORE_FP fs6, FREG(22)
	STORE_FP fs7, FREG(23)
	STORE_FP fs8, FREG(24)
	STORE_FP fs9, FREG(25)
	STORE_FP fs10, FREG(26)
	STORE_FP fs11, FREG(27)
#endif

	li a0, 0
	ret

	.cfi_endproc
	.size _Uriscv_getcontext, . - _Uriscv_getcontext

	/* We do not need executable stack.  */
	.section        .note.GNU-stack,"",@progbits
