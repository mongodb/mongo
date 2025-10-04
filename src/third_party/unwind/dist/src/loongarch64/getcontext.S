/* libunwind - a platform-independent unwind library
   Copyright (C) 2008 CodeSourcery
   Copyright (C) 2012 Tommi Rantala <tt.rantala@gmail.com>
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

#include "offsets.h"
#include <endian.h>
	.text

#define SREG(X) st.d $r##X, $r4, (LINUX_UC_MCONTEXT_GREGS + 8 * X)
#define SPC st.d $r1, $r4, (LINUX_UC_MCONTEXT_PC)

	.global _Uloongarch64_getcontext
	.type   _Uloongarch64_getcontext, %function

_Uloongarch64_getcontext:
        .cfi_startproc
        SREG(0)
        SREG(1)
        SREG(3)
        SREG(21)
        SREG(22)
        SREG(23)
        SREG(24)
        SREG(25)
        SREG(26)
        SREG(27)
        SREG(28)
        SREG(29)
        SREG(30)
        SREG(31)

        SPC

        ori $r4, $r0, 0
        jirl $r0, $r1, 0

        .cfi_endproc

	.size   _Uloongarch64_getcontext, .-_Uloongarch64_getcontext
