/* libunwind - a platform-independent unwind library
   Copyright (C) 2004-2005 Hewlett-Packard Co
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

#define CALL_NEXT_PTR(gp_save_reg, arg0, arg1)				\
	ld8 r2 = [arg0], 8;;	/* read the next function pointer */	\
	ld8 r3 = [r2], 8;;	/* read the function's entry-point */	\
	ld8 r2 = [r2];;		/* read the function's gp */		\
	mov b6 = r3;							\
	mov gp_save_reg = gp;						\
	mov out0 = arg0;						\
	mov out1 = arg1;						\
	mov gp = r2;							\
	br.call.sptk.many rp = b6;;					\
	mov gp = gp_save_reg

#define CALL_NEXT(gp_save_reg)	CALL_NEXT_PTR(gp_save_reg, in0, in1)

#define LOAD_VAL(reg)				\
	ld8 reg = [in1], 8;;			\
	tbit.nz p15, p0 = reg, 0;;		\
(p15)	ld8.s reg = [r0]


	.global flushrs
	.proc flushrs
flushrs:
	flushrs;;
	br.ret.sptk.many rp
	.endp flushrs

	/* Save r4-r7 into stacked registers, load them up with the
	   values passed via the pointer in in1 and then call the
	   function passed via the pointer in in0.  */

	.global save_static_to_stacked
	.proc save_static_to_stacked
save_static_to_stacked:
	.prologue
	.regstk 2, 7, 2, 0
	.save ar.pfs, loc0
	alloc loc0 = ar.pfs, 2, 7, 2, 0
	.save rp, loc1
	mov loc1 = rp
	.spillreg r4, loc2
	mov loc2 = r4
	.spillreg r5, loc3
	mov loc3 = r5
	.spillreg r6, loc4
	mov loc4 = r6
	.spillreg r7, loc5
	mov loc5 = r7
	.body
	LOAD_VAL(r4)
	LOAD_VAL(r5)
	LOAD_VAL(r6)
	LOAD_VAL(r7)
	CALL_NEXT(loc6)

	mov r4 = loc2
	mov r5 = loc3
	mov r6 = loc4
	mov r7 = loc5

	mov ar.pfs = loc0
	mov rp = loc1
	br.ret.sptk.many rp
	.endp save_static_to_stacked

	/* Save f2 to the memory stack, save r4 to f2, then load
	   r4 with the value passed via in1 and call the function
	   passed via in0.  */

	.global save_static_to_fr
	.proc save_static_to_fr
save_static_to_fr:
	.prologue
	.regstk 2, 3, 2, 0
	.save ar.pfs, loc0
	alloc loc0 = ar.pfs, 2, 3, 2, 0
	.save rp, loc1
	mov loc1 = rp
	.fframe 16
	.spillpsp f2, 0
	stf.spill [sp] = f2, -16
	.spillreg r4, f2
	setf.sig f2 = r4

	.body

	ld8 r4 = [in1], 8;;
	tbit.nz p6, p0 = r4, 0;;
(p6)	ld8.s r4 = [r0]

	CALL_NEXT(loc2)

	getf.sig r4 = f2		// restore r4
	.restore sp
	add sp = 16, sp;;
	ldf.fill f2 = [sp]		// restore r2

	mov ar.pfs = loc0
	mov rp = loc1
	br.ret.sptk.many rp
	.endp save_static_to_fr

	/* If r4 is not a NaT, save b3 to a stacked register and
	   then save r4 in b3.  The non-NaTness of r4 is saved in
	   p1.  */

	.global save_static_to_br
	.proc save_static_to_br
save_static_to_br:
	.prologue
	.regstk 2, 6, 2, 0
	.save ar.pfs, loc0
	alloc loc0 = ar.pfs, 2, 6, 2, 0
	.save rp, loc1
	mov loc1 = rp

	.save pr, loc2
	mov loc2 = pr			// save predicates

	.spillreg b3, loc3
	mov loc3 = b3

	tnat.z p1, p2 = r4;;
	.spillreg.p p1, r4, b3
(p1)	mov b3 = r4
	.spillreg.p p2, r4, loc4
(p2)	mov loc4 = r4

	.body

	LOAD_VAL(r4)
	CALL_NEXT(loc5)

	.pred.rel.mutex p1, p2
(p1)	mov r4 = b3			// restore r4
(p2)	mov r4 = loc4

	mov ar.pfs = loc0
	mov rp = loc1
	mov pr = loc2, -1
	mov b3 = loc3			// restore b3
	br.ret.sptk.many rp
	.endp save_static_to_br

	/* Spill r4 into memory and then save r5 in r4.  */

	.global save_static_to_mem
	.proc save_static_to_mem
save_static_to_mem:
	.prologue
	.regstk 2, 4, 2, 0
	.save ar.pfs, loc0
	alloc loc0 = ar.pfs, 2, 4, 2, 0
	.save rp, loc1
	mov loc1 = rp
	.save ar.unat, loc2
	mov loc2 = ar.unat

	.fframe 16
	.spillpsp r4, 0
	st8.spill [sp] = r4, -16

	.spillreg r5, r4
	mov r4 = r5

	.body

	LOAD_VAL(r5)
	CALL_NEXT(loc3)

	mov r5 = r4			// restore r5
	.restore sp
	add sp = 16, sp;;
	ld8.fill r4 = [sp]		// restore r4

	mov ar.pfs = loc0
	mov rp = loc1
	mov ar.unat = loc2		// restore ar.unat
	br.ret.sptk.many rp
	.endp save_static_to_mem

	/* Spill r6 into memory and save primary ar.unat in a register.  */

	.global save_static_to_mem2
	.proc save_static_to_mem2
save_static_to_mem2:
	.prologue
	.regstk 2, 5, 2, 0
	.save ar.pfs, loc0
	alloc loc0 = ar.pfs, 2, 5, 2, 0
	.save rp, loc1
	mov loc1 = rp
	.save ar.unat, loc2
	mov loc2 = ar.unat

	.fframe 16
	.spillpsp r6, 0
	st8.spill [sp] = r6, -16;;
	.save @priunat, loc3
	mov loc3 = ar.unat
	mov ar.unat = 0			// trash ar.unat

	.body

	LOAD_VAL(r6)
	CALL_NEXT(loc4)

	mov ar.unat = loc3		// restore primary UNaT
	.restore sp
	add sp = 16, sp;;
	ld8.fill r6 = [sp]		// restore r6

	mov ar.pfs = loc0
	mov rp = loc1
	mov ar.unat = loc2		// restore ar.unat
	br.ret.sptk.many rp
	.endp save_static_to_mem2

	/* Spill r6 into memory and save primary ar.unat in memory.  */

	.global save_static_to_mem3
	.proc save_static_to_mem3
save_static_to_mem3:
	.prologue
	.regstk 2, 5, 2, 0
	.save ar.pfs, loc0
	alloc loc0 = ar.pfs, 2, 5, 2, 0
	.save rp, loc1
	mov loc1 = rp
	.save ar.unat, loc2
	mov loc2 = ar.unat

	add r2 = 8, sp
	.fframe 16
	.spillpsp r6, 0
	st8.spill [sp] = r6, -16;;
	mov r3 = ar.unat;;
	.savepsp @priunat, -8
	st8 [r2] = r3
	mov ar.unat = 0			// trash ar.unat

	.body

	LOAD_VAL(r6)
	CALL_NEXT(loc4)

	add r2 = 24, sp;;
	ld8 r3 = [r2];;
	mov ar.unat = r3		// restore primary UNaT
	.restore sp
	add sp = 16, sp;;
	ld8.fill r6 = [sp]		// restore r6

	mov ar.pfs = loc0
	mov rp = loc1
	mov ar.unat = loc2		// restore ar.unat
	br.ret.sptk.many rp
	.endp save_static_to_mem3

	/* Spill r6 into memory and save primary ar.unat in register,
	   then in memory.  */

	.global save_static_to_mem4
	.proc save_static_to_mem4
save_static_to_mem4:
	.prologue
	.regstk 2, 5, 2, 0
	.save ar.pfs, loc0
	alloc loc0 = ar.pfs, 2, 5, 2, 0
	.save rp, loc1
	mov loc1 = rp
	.save ar.unat, loc2
	mov loc2 = ar.unat

	add r2 = 8, sp
	.fframe 16
	.spillpsp r6, 0
	st8.spill [sp] = r6, -16;;
	.save @priunat, r3
	mov r3 = ar.unat;;
	mov ar.unat = 0			// trash ar.unat
	.savepsp @priunat, -8
	st8 [r2] = r3
	mov r3 = r0			// trash register pri UNaT location
	.body

	LOAD_VAL(r6)
	CALL_NEXT(loc4)

	add r2 = 24, sp;;
	ld8 r3 = [r2];;
	mov ar.unat = r3		// restore primary UNaT
	.restore sp
	add sp = 16, sp;;
	ld8.fill r6 = [sp]		// restore r6

	mov ar.pfs = loc0
	mov rp = loc1
	mov ar.unat = loc2		// restore ar.unat
	br.ret.sptk.many rp
	.endp save_static_to_mem4

	/* Spill r6 into memory and save primary ar.unat in register,
	   then in memory.  */

	.global save_static_to_mem5
	.proc save_static_to_mem5
save_static_to_mem5:
	.prologue
	.regstk 2, 5, 2, 0
	.save ar.pfs, loc0
	alloc loc0 = ar.pfs, 2, 5, 2, 0
	.save rp, loc1
	mov loc1 = rp
	.save ar.unat, loc2
	mov loc2 = ar.unat

	add r2 = 8, sp
	.fframe 16
	.spillpsp r6, 0
	st8.spill [sp] = r6, -16;;
	mov r3 = ar.unat;;
	mov ar.unat = 0			// trash ar.unat
	.savepsp @priunat, -8
	st8 [r2] = r3
	.save @priunat, loc3
	mov loc3 = r3
	st8 [r2] = r0			// trash memory pri UNaT location
	.body

	LOAD_VAL(r6)
	CALL_NEXT(loc4)

	add r2 = 24, sp;;
	ld8 r3 = [r2];;
	mov ar.unat = loc3		// restore primary UNaT
	.restore sp
	add sp = 16, sp;;
	ld8.fill r6 = [sp]		// restore r6

	mov ar.pfs = loc0
	mov rp = loc1
	mov ar.unat = loc2		// restore ar.unat
	br.ret.sptk.many rp
	.endp save_static_to_mem5

	/* Save r4-r7 to various scratch registers, then trigger
	   a segfault.  */

	.global save_static_to_scratch
	.proc save_static_to_scratch
save_static_to_scratch:
	.prologue

	.spillreg r4, r16
	mov r16 = r4			// save r4 in r16
	tnat.nz p6, p7 = r5;;
	.spillreg.p p6, r5, f31
(p6)	setf.sig f31 = r5		// save r5 in f31 if it's a NaT
	.spillreg.p p7, r5, b6
(p7)	mov b6 = r5			//         in  b6 if it not
	.spillreg r6, f32
	setf.sig f32 = r6		// save r6 in f32 (fph partition)
	.spillsp r7, 0
	st8.spill [sp] = r7		// save r7 in the scratch stack space
	.spillreg f4, f6
	mov f6 = f4;;
	.body

	ld8 r2 = [in1]
	;;
	mov ar.ec = r2

	LOAD_VAL(r4)
	LOAD_VAL(r5)
	LOAD_VAL(r6)
	LOAD_VAL(r7)
	setf.sig f4 = r4

	/* Now force a SIGSEGV.  Make sure the ld8 is at the beginning of a
	   bundle, so the signal-handler can skip over it simply by
	   incrementing the IP.  */
	{
		.mmi
		ld8 r2 = [r0]
		nop.m 0
		nop.i 0 ;;
	}

	mov f4 = f6
	mov r4 = r16
	.pred.rel.mutex p6, p7
(p6)	getf.sig r5 = f31
(p7)	mov r5 = b6
	getf.sig r6 = f32
	ld8.fill r7 = [sp]

	br.ret.sptk.many rp
	.endp save_static_to_scratch

	/* Rotate registers a bit in a vain attempt to sow some confusion.
	   Care must be taken not to write any rotating general register
	   after rotation, because we keep the preserved state
	   there... */

	.global rotate_regs
	.proc rotate_regs
rotate_regs:
	.prologue
	.regstk 2, 14, 2, 16
	.save ar.pfs, loc0
	alloc loc0 = ar.pfs, 2, 14, 2, 16
	.save rp, loc1
	mov loc1 = rp
	.save pr, loc2
	mov loc2 = pr
	.save ar.lc, loc3
	mov loc3 = ar.lc
	.spillreg r4, loc4
	mov loc4 = r4

	ld8 r2 = [in1], 8;;
	mov pr = r2, -1

	ld8 r2 = [in1], 8;;
	mov r8 = in0
	mov r9 = in1
	and r2 = 127, r2;;
	mov ar.ec = 0
	mov ar.lc = r2;;

	// use p6 to preserve p63 as it gets rotated into p16:
(p16)	cmp.eq.unc p6,p0 = r0,r0;;
1:
(p6)	cmp.eq.unc p16,p0 = r0,r0
(p63)	cmp.eq.unc p6,p0 = r0,r0
	br.ctop.dptk.few 1b;;

(p6)	cmp.eq.unc p63,p0 = r0,r0

	CALL_NEXT_PTR(r4, r8, r9)

	clrrrb

	mov ar.pfs = loc0
	mov rp = loc1
	mov pr = loc2, -1
	mov ar.lc = loc3
	mov r4 = loc4
	br.ret.sptk.many rp

	.endp rotate_regs

	.global save_pr
	.proc save_pr
save_pr:
	.prologue
	.regstk 2, 4, 2, 0
	.save ar.pfs, loc0
	alloc loc0 = ar.pfs, 2, 4, 2, 0
	.save rp, loc1
	mov loc1 = rp
	.save pr, loc2
	mov loc2 = pr

	ld8 r2 = [in1], 8;;
	mov pr = r2, -1

	CALL_NEXT(loc3)

	mov ar.pfs = loc0
	mov rp = loc1
	mov pr = loc2, -1
	br.ret.sptk.many rp

	.endp save_pr

#ifdef __linux__
        /* We do not need executable stack.  */
        .section        .note.GNU-stack,"",@progbits
#endif
