/* libunwind - a platform-independent unwind library
   Copyright (C) 2004 Hewlett-Packard Co
	Contributed by David Mosberger-Tang <davidm@hpl.hp.com>

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

	.text

	.global test_func
	.proc test_func
test_func:
	.prologue
	.regstk 1, 3, 0, 0
	.save ar.pfs, loc0
	alloc loc0 = ar.pfs, 1, 3, 0, 0
	mov loc1 = rp
	.save rp, r0
	.save ar.lc, r0
	.body
	mov loc2 = gp
	ld8 r2 = [in0], 8;;
	ld8 r1 = [in0];;
	mov b6 = r2
	br.call.sptk.many rp = b6

	mov gp = loc2
	mov rp = loc1
	mov ar.pfs = loc0
	br.ret.sptk.many rp

	.endp test_func

#ifdef __linux__
        /* We do not need executable stack.  */
        .section        .note.GNU-stack,"",@progbits
#endif
