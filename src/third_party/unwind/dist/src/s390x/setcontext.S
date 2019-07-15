/* libunwind - a platform-independent unwind library
   Copyright (C) 2008 Google, Inc
	Contributed by Paul Pluzhnikov <ppluzhnikov@google.com>
   Copyright (C) 2010 Konstantin Belousov <kib@freebsd.org>
   Copyright (C) 2017 IBM

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

// int _Us390x_setcontext (const ucontext_t *ucp)

	.global _Us390x_setcontext
	.type _Us390x_setcontext, @function
_Us390x_setcontext:
	.cfi_startproc

	// Must only restore registers saved by getcontext, other fields
	// in the ucontext_t might be uninitialised.

	// Stop this function being unwound. We are clobbering callee-save
	// registers in this function so unwinding it is unsafe.
	// Ideally we'd save callee-save registers, update the CFI for them
	// and then switch to the new CFI once the context switch is
	// complete.
	.cfi_undefined %r14

	// floating point parameters
	ld    %f0,0x100(%r2)
	ld    %f2,0x110(%r2)
	ld    %f4,0x120(%r2)
	ld    %f6,0x130(%r2)

	// floating point preserved registers
	lfpc  0xf8(%r2)
	ld    %f8,0x140(%r2)
	ld    %f9,0x148(%r2)
	ld    %f10,0x150(%r2)
	ld    %f11,0x158(%r2)
	ld    %f12,0x160(%r2)
	ld    %f13,0x168(%r2)
	ld    %f14,0x170(%r2)
	ld    %f15,0x178(%r2)

	// preserved registers and parameters
	lgr   %r1,%r2
	lmg   %r2,%r15,0x48(%r1)

	// restore PSW address
	lg    %r1,0x30(%r1)
	br    %r1

	.cfi_endproc
	.size _Us390x_setcontext, . - _Us390x_setcontext

	// We do not need executable stack.
	.section        .note.GNU-stack,"",@progbits
