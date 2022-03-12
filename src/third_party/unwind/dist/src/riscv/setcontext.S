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
	.global _Uriscv_setcontext
	.type   _Uriscv_setcontext, @function
_Uriscv_setcontext:
	.cfi_startproc

#ifdef LOAD_FP
	/* The FCSR is always 32-bits and comes after all registers */
	lw a1, FREG(32)
	fscsr a1

	LOAD_FP fs0, FREG(8)
	LOAD_FP fs1, FREG(9)
	LOAD_FP fs2, FREG(18)
	LOAD_FP fs3, FREG(19)
	LOAD_FP fs4, FREG(20)
	LOAD_FP fs5, FREG(21)
	LOAD_FP fs6, FREG(22)
	LOAD_FP fs7, FREG(23)
	LOAD_FP fs8, FREG(24)
	LOAD_FP fs9, FREG(25)
	LOAD_FP fs10, FREG(26)
	LOAD_FP fs11, FREG(27)
#endif

	LOAD t1, REG(0)
	LOAD ra, REG(1)
	LOAD sp, REG(2)
	LOAD s0, REG(8)
	LOAD s1, REG(9)
	LOAD a1, REG(11)
	LOAD a2, REG(12)
	LOAD a3, REG(13)
	LOAD a4, REG(14)
	LOAD a5, REG(15)
	LOAD a6, REG(16)
	LOAD a7, REG(17)
	LOAD s2, REG(18)
	LOAD s3, REG(19)
	LOAD s4, REG(20)
	LOAD s5, REG(21)
	LOAD s6, REG(22)
	LOAD s7, REG(23)
	LOAD s8, REG(24)
	LOAD s9, REG(25)
	LOAD s10, REG(26)
	LOAD s11, REG(27)

	LOAD a0, REG(10)

	jr t1

	.cfi_endproc
	.size _Uriscv_setcontext, . - _Uriscv_setcontext

	/* We do not need executable stack.  */
	.section        .note.GNU-stack,"",@progbits
