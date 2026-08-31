/* libunwind - a platform-independent unwind library
   Copyright (C) 2023 Dmitry Chagin <dchagin@FreeBSD.org>

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

#include "ucontext_i.h"


	.global _Uaarch64_setcontext
	.type _Uaarch64_setcontext, @function
_Uaarch64_setcontext:
	.cfi_startproc

	/*
	 * Since there are no signals involved here we restore EH and
	 * non scratch registers only.
	 */

	mov  x9,  x0
	ldp  x0,  x1, [x9, #(UC_MCONTEXT_OFF + SC_GPR_OFF)]
	ldp  x2,  x3, [x9, #(UC_MCONTEXT_OFF + SC_GPR_OFF +  2 * 8)]
	ldp x19, x20, [x9, #(UC_MCONTEXT_OFF + SC_GPR_OFF + 19 * 8)]
	ldp x21, x22, [x9, #(UC_MCONTEXT_OFF + SC_GPR_OFF + 21 * 8)]
	ldp x23, x24, [x9, #(UC_MCONTEXT_OFF + SC_GPR_OFF + 23 * 8)]
	ldp x25, x26, [x9, #(UC_MCONTEXT_OFF + SC_GPR_OFF + 25 * 8)]
	ldp x27, x28, [x9, #(UC_MCONTEXT_OFF + SC_GPR_OFF + 27 * 8)]
	ldp x29, x30, [x9, #(UC_MCONTEXT_OFF + SC_GPR_OFF + 29 * 8)]

	ldp  q8,  q9, [x9, #(UC_MCONTEXT_OFF + SC_FPSIMD_OFF +  8 * 16)]
	ldp q10, q11, [x9, #(UC_MCONTEXT_OFF + SC_FPSIMD_OFF + 10 * 16)]
	ldp q12, q13, [x9, #(UC_MCONTEXT_OFF + SC_FPSIMD_OFF + 12 * 16)]
	ldp q14, q15, [x9, #(UC_MCONTEXT_OFF + SC_FPSIMD_OFF + 14 * 16)]

	ldr  x9, [x9, #(UC_MCONTEXT_OFF + SC_SP_OFF)]
	mov  sp, x9

	ret
	.cfi_endproc
	.size _Uaarch64_setcontext, . - _Uaarch64_setcontext

	.section        .note.GNU-stack,"",@progbits
