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

#if defined(__FreeBSD__)
#include <sys/syscall.h>
#define	SIG_SETMASK	3
#endif

	.global _UI_siglongjmp_cont
	.type _UI_siglongjmp_cont, @function
_UI_siglongjmp_cont:
#if defined(__linux__) || defined(__QNX__)
	ret
#elif defined(__FreeBSD__)
	.cfi_startproc
	/*
	 * x0 - return address
	 * x1 - return value
	 * x2 - mask
	 */
	stp	x0, x1, [sp, #-16]!
	.cfi_def_cfa_offset 16
        /* Restore the signal mask */
	mov	x1, x2				/* set */
	mov	x2, #0				/* oset */
	mov	x0, #SIG_SETMASK
	mov	x8, #SYS_sigprocmask
	svc	0
	ldp	x30, x0, [sp], #16
	.cfi_def_cfa_offset 0
	ret
	.cfi_endproc
#else
#error Port me
#endif
	.size _UI_siglongjmp_cont, . - _UI_siglongjmp_cont
	/* We do not need executable stack.  */
	.section  .note.GNU-stack,"",%progbits
