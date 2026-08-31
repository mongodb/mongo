/* libunwind - a platform-independent unwind library
   Copyright (C) 2006-2007 IBM
   Contributed by
     Corey Ashford <cjashfor@us.ibm.com>
     Jose Flavio Aguilar Paulino <jflavio@br.ibm.com> <joseflavio@gmail.com>

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

#ifndef ucontext_i_h
#define ucontext_i_h

#include "compiler.h"
#include <ucontext.h>

#if defined(__linux__)

/* These values were derived by reading
   /usr/src/linux-2.6.18-1.8/arch/um/include/sysdep-ppc/ptrace.h and
   /usr/src/linux-2.6.18-1.8/arch/powerpc/kernel/ppc32.h
*/

//#define NIP_IDX               32
#define CTR_IDX         32
#define XER_IDX         33
#define CCR_IDX         34
#define MSR_IDX         35
//#define MQ_IDX                36
#define LINK_IDX        36

#define _UC_MCONTEXT_GPR(x) ( ((void *)&dmy_ctxt.uc_mcontext.uc_regs->gregs[x] - (void *)&dmy_ctxt) )
#define _UC_MCONTEXT_FPR(x) ( ((void *)&dmy_ctxt.uc_mcontext.uc_regs->fpregs.fpregs[x] - (void *)&dmy_ctxt) )

/* These are dummy structures used only for obtaining the offsets of the
   various structure members. */
static ucontext_t dmy_ctxt UNUSED;

#elif defined(__FreeBSD__)
 /* See /usr/src/sys/powerpc/include/ucontext.h.
   FreeBSD uses a different structure than Linux.
*/

#define NIP_IDX         36
#define MSR_IDX         37
//#define ORIG_GPR3_IDX
#define CTR_IDX         35
#define LINK_IDX        32
#define XER_IDX         34
#define CCR_IDX         33
//#define SOFTE_IDX
//#define TRAP_IDX
#define DAR_IDX         39
#define DSISR_IDX       40
//#define RESULT_IDX

#define _UC_MCONTEXT_GPR(_x) ( ((void *)&dmy_ctxt.mc_gpr[_x] - (void *)&dmy_ctxt) )
#define _UC_MCONTEXT_FPR(_x) ( ((void *)&dmy_ctxt.mc_fpreg[_x] - (void *)&dmy_ctxt) )

/* These are dummy structures used only for obtaining the offsets of the
   various structure members. */
static struct __mcontext dmy_ctxt;

#else
#error "Not implemented!"
#endif

#define UC_MCONTEXT_GREGS_R0 _UC_MCONTEXT_GPR(0)
#define UC_MCONTEXT_GREGS_R1 _UC_MCONTEXT_GPR(1)
#define UC_MCONTEXT_GREGS_R2 _UC_MCONTEXT_GPR(2)
#define UC_MCONTEXT_GREGS_R3 _UC_MCONTEXT_GPR(3)
#define UC_MCONTEXT_GREGS_R4 _UC_MCONTEXT_GPR(4)
#define UC_MCONTEXT_GREGS_R5 _UC_MCONTEXT_GPR(5)
#define UC_MCONTEXT_GREGS_R6 _UC_MCONTEXT_GPR(6)
#define UC_MCONTEXT_GREGS_R7 _UC_MCONTEXT_GPR(7)
#define UC_MCONTEXT_GREGS_R8 _UC_MCONTEXT_GPR(8)
#define UC_MCONTEXT_GREGS_R9 _UC_MCONTEXT_GPR(9)
#define UC_MCONTEXT_GREGS_R10 _UC_MCONTEXT_GPR(10)
#define UC_MCONTEXT_GREGS_R11 _UC_MCONTEXT_GPR(11)
#define UC_MCONTEXT_GREGS_R12 _UC_MCONTEXT_GPR(12)
#define UC_MCONTEXT_GREGS_R13 _UC_MCONTEXT_GPR(13)
#define UC_MCONTEXT_GREGS_R14 _UC_MCONTEXT_GPR(14)
#define UC_MCONTEXT_GREGS_R15 _UC_MCONTEXT_GPR(15)
#define UC_MCONTEXT_GREGS_R16 _UC_MCONTEXT_GPR(16)
#define UC_MCONTEXT_GREGS_R17 _UC_MCONTEXT_GPR(17)
#define UC_MCONTEXT_GREGS_R18 _UC_MCONTEXT_GPR(18)
#define UC_MCONTEXT_GREGS_R19 _UC_MCONTEXT_GPR(19)
#define UC_MCONTEXT_GREGS_R20 _UC_MCONTEXT_GPR(20)
#define UC_MCONTEXT_GREGS_R21 _UC_MCONTEXT_GPR(21)
#define UC_MCONTEXT_GREGS_R22 _UC_MCONTEXT_GPR(22)
#define UC_MCONTEXT_GREGS_R23 _UC_MCONTEXT_GPR(23)
#define UC_MCONTEXT_GREGS_R24 _UC_MCONTEXT_GPR(24)
#define UC_MCONTEXT_GREGS_R25 _UC_MCONTEXT_GPR(25)
#define UC_MCONTEXT_GREGS_R26 _UC_MCONTEXT_GPR(26)
#define UC_MCONTEXT_GREGS_R27 _UC_MCONTEXT_GPR(27)
#define UC_MCONTEXT_GREGS_R28 _UC_MCONTEXT_GPR(28)
#define UC_MCONTEXT_GREGS_R29 _UC_MCONTEXT_GPR(29)
#define UC_MCONTEXT_GREGS_R30 _UC_MCONTEXT_GPR(30)
#define UC_MCONTEXT_GREGS_R31 _UC_MCONTEXT_GPR(31)
#define UC_MCONTEXT_GREGS_NIP _UC_MCONTEXT_GPR(NIP_IDX)
#define UC_MCONTEXT_GREGS_MSR _UC_MCONTEXT_GPR(MSR_IDX)
#ifdef ORIG_GPR3_IDX
#define UC_MCONTEXT_GREGS_ORIG_GPR3 _UC_MCONTEXT_GPR(ORIG_GPR3_IDX)
#endif
#define UC_MCONTEXT_GREGS_CTR _UC_MCONTEXT_GPR(CTR_IDX)
#define UC_MCONTEXT_GREGS_LINK _UC_MCONTEXT_GPR(LINK_IDX)
#define UC_MCONTEXT_GREGS_XER _UC_MCONTEXT_GPR(XER_IDX)
#define UC_MCONTEXT_GREGS_CCR _UC_MCONTEXT_GPR(CCR_IDX)
#ifdef SOFTE_IDX
#define UC_MCONTEXT_GREGS_SOFTE _UC_MCONTEXT_GPR(SOFTE_IDX)
#endif
#ifdef TRAP_IDX
#define UC_MCONTEXT_GREGS_TRAP _UC_MCONTEXT_GPR(TRAP_IDX)
#endif
#define UC_MCONTEXT_GREGS_DAR _UC_MCONTEXT_GPR(DAR_IDX)
#define UC_MCONTEXT_GREGS_DSISR _UC_MCONTEXT_GPR(DSISR_IDX)
#ifdef RESULT_IDX
#define UC_MCONTEXT_GREGS_RESULT _UC_MCONTEXT_GPR(RESULT_IDX)
#endif

#define UC_MCONTEXT_FREGS_R0 _UC_MCONTEXT_FPR(0)
#define UC_MCONTEXT_FREGS_R1 _UC_MCONTEXT_FPR(1)
#define UC_MCONTEXT_FREGS_R2 _UC_MCONTEXT_FPR(2)
#define UC_MCONTEXT_FREGS_R3 _UC_MCONTEXT_FPR(3)
#define UC_MCONTEXT_FREGS_R4 _UC_MCONTEXT_FPR(4)
#define UC_MCONTEXT_FREGS_R5 _UC_MCONTEXT_FPR(5)
#define UC_MCONTEXT_FREGS_R6 _UC_MCONTEXT_FPR(6)
#define UC_MCONTEXT_FREGS_R7 _UC_MCONTEXT_FPR(7)
#define UC_MCONTEXT_FREGS_R8 _UC_MCONTEXT_FPR(8)
#define UC_MCONTEXT_FREGS_R9 _UC_MCONTEXT_FPR(9)
#define UC_MCONTEXT_FREGS_R10 _UC_MCONTEXT_FPR(10)
#define UC_MCONTEXT_FREGS_R11 _UC_MCONTEXT_FPR(11)
#define UC_MCONTEXT_FREGS_R12 _UC_MCONTEXT_FPR(12)
#define UC_MCONTEXT_FREGS_R13 _UC_MCONTEXT_FPR(13)
#define UC_MCONTEXT_FREGS_R14 _UC_MCONTEXT_FPR(14)
#define UC_MCONTEXT_FREGS_R15 _UC_MCONTEXT_FPR(15)
#define UC_MCONTEXT_FREGS_R16 _UC_MCONTEXT_FPR(16)
#define UC_MCONTEXT_FREGS_R17 _UC_MCONTEXT_FPR(17)
#define UC_MCONTEXT_FREGS_R18 _UC_MCONTEXT_FPR(18)
#define UC_MCONTEXT_FREGS_R19 _UC_MCONTEXT_FPR(19)
#define UC_MCONTEXT_FREGS_R20 _UC_MCONTEXT_FPR(20)
#define UC_MCONTEXT_FREGS_R21 _UC_MCONTEXT_FPR(21)
#define UC_MCONTEXT_FREGS_R22 _UC_MCONTEXT_FPR(22)
#define UC_MCONTEXT_FREGS_R23 _UC_MCONTEXT_FPR(23)
#define UC_MCONTEXT_FREGS_R24 _UC_MCONTEXT_FPR(24)
#define UC_MCONTEXT_FREGS_R25 _UC_MCONTEXT_FPR(25)
#define UC_MCONTEXT_FREGS_R26 _UC_MCONTEXT_FPR(26)
#define UC_MCONTEXT_FREGS_R27 _UC_MCONTEXT_FPR(27)
#define UC_MCONTEXT_FREGS_R28 _UC_MCONTEXT_FPR(28)
#define UC_MCONTEXT_FREGS_R29 _UC_MCONTEXT_FPR(29)
#define UC_MCONTEXT_FREGS_R30 _UC_MCONTEXT_FPR(30)
#define UC_MCONTEXT_FREGS_R31 _UC_MCONTEXT_FPR(31)
#define UC_MCONTEXT_FREGS_FPSCR _UC_MCONTEXT_FPR(32)

#endif
