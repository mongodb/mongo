/*  Contributed by Dmitry Chagin <dchagin@FreeBSD.org>.

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
#ifndef libunwind_src_aarch64_ucontext_i_h
#define libunwind_src_aarch64_ucontext_i_h

#if defined __FreeBSD__ || defined __APPLE__

#define	UC_MCONTEXT_OFF			0x10
#define	SC_GPR_OFF			0x00

#define	SC_X29_OFF			0x0e8
#define	SC_X30_OFF			0x0f0
#define	SC_SP_OFF			0x0f8
#define	SC_PC_OFF			0x100
#define	SC_PSTATE_OFF			0x108
#define	SC_FPSIMD_OFF			0x110

#define	SCF_FORMAT			AARCH64_SCF_FREEBSD_RT_SIGFRAME

#elif defined(__linux__)

#define	UC_MCONTEXT_OFF			0xb0
#define	SC_GPR_OFF			0x08

#define	SC_X29_OFF			0x0f0
#define	SC_X30_OFF			0x0f8
#define	SC_SP_OFF			0x100
#define	SC_PC_OFF			0x108
#define	SC_PSTATE_OFF			0x110

#define	SCF_FORMAT			AARCH64_SCF_LINUX_RT_SIGFRAME

#define	LINUX_SC_RESERVED_OFF		0x120

#define	LINUX_SC_RESERVED_MAGIC_OFF	0x0
#define	LINUX_SC_RESERVED_SIZE_OFF	0x4
#define	LINUX_SC_RESERVED_SVE_VL_OFF	0x8

#elif defined(__QNX__)

#define UC_MCONTEXT_OFF   48
#define SC_GPR_OFF         0
#define SC_X29_OFF       232
#define SC_X30_OFF       240
#define SC_SP_OFF        248
#define SC_PC_OFF        256
#define SC_PSTATE_OFF    264
#define SCF_FORMAT       AARCH64_SCF_QNX_RT_SIGFRAME

#else
# error Port me
#endif

#endif /* libunwind_src_aarch64_ucontext_i_h */
