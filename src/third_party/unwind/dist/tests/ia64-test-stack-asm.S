/* libunwind - a platform-independent unwind library
   Copyright (C) 2003 Hewlett-Packard Co
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

#include "ia64-test-stack.h"

	.common stackmem, NSTACKS*STACK_SIZE, 16

	.global do_unwind_tests

	.text

#define SAVED_SP_OFF		 0
#define SAVED_RP_OFF		 8
#define SAVED_PFS_OFF		16
#define SAVED_RNAT_OFF		24
#define SAVED_BSP_OFF		32
#define SAVED_BSPSTORE_OFF	40
#define FRAME_SIZE		48

	.proc stack_it
stack_it:
	.prologue
	alloc r18 = ar.pfs, 0, 0, 0, 0		// read ar.pfs
	// first, calculate address of new stack:
	addl r2 = @ltoff(stackmem), gp
	shl r3 = r8, STACK_SIZE_SHIFT
	;;
	ld8 r2 = [r2]				// r2 = &stackmem
	;;
	add r2 = r2, r3				// r2 = stackmem[iteration]
	;;
	addl r3 = STACK_SIZE-FRAME_SIZE, r2	// r3 = &stackframe
	;;
	st8 [r3] = sp
	.vframesp SAVED_SP_OFF+16
	adds sp = -16, r3			// switch the memory stack
	;;
	adds r3 = (SAVED_RP_OFF - SAVED_SP_OFF), r3
	mov r16 = rp
	;;
	.savesp rp, SAVED_RP_OFF+16
	st8 [r3] = r16, (SAVED_PFS_OFF - SAVED_RP_OFF)
	;;
	.savesp ar.pfs, SAVED_PFS_OFF+16
	st8 [r3] = r18, (SAVED_BSP_OFF - SAVED_PFS_OFF)

	mov r16 = ar.bsp
	mov r17 = ar.bspstore
	mov r18 = ar.rnat
	;;
	.savesp ar.bsp, SAVED_BSP_OFF+16
	st8 [r3] = r16, (SAVED_BSPSTORE_OFF - SAVED_BSP_OFF)
	;;
	.savesp ar.bspstore, SAVED_BSPSTORE_OFF+16
	st8 [r3] = r17, (SAVED_RNAT_OFF - SAVED_BSPSTORE_OFF)
	;;
	.savesp ar.rnat, SAVED_RNAT_OFF+16
	st8 [r3] = r18
	;;
	mov ar.bspstore = r2			// switch the backing store

	.body

	// for even iterations, allocate a local variable:
	tbit.nz p6, p0 = r8, 0
(p6)	br.cond.sptk.few .skip
	;;
	alloc r2 = ar.pfs, 0, 1, 0, 0
	mov loc0 = r8
	;;
.skip:	cmp.ne p6, p7 = 0, r8
	;;
{	.mbb
(p6)	adds r8 = -1, r8
(p6)	br.call.sptk.many rp = stack_it		// next iteration
(p7)	br.call.sptk.many rp = do_unwind_tests	// time for introspection...
}
	// switch back to stack:

	adds r3 = SAVED_SP_OFF+16, sp
	;;
	ld8 r16 = [r3], (SAVED_RP_OFF-SAVED_SP_OFF);;		// saved sp
	ld8 r17 = [r3], (SAVED_PFS_OFF-SAVED_RP_OFF);;		// saved rp
	ld8 r18 = [r3], (SAVED_RNAT_OFF-SAVED_PFS_OFF);;	// saved pfs
	ld8 r19 = [r3], (SAVED_BSP_OFF-SAVED_RNAT_OFF);;	// saved rnat
	ld8 r20 = [r3], (SAVED_BSPSTORE_OFF-SAVED_BSP_OFF);;	// saved bsp
	ld8 r21 = [r3];;				// saved bspstore

	mov rp = r17
	mov ar.pfs = r18
	mov ar.bspstore = r21	// this also restores ar.bsp
	;;
	mov ar.rnat = r19

	.restore sp
	mov sp = r16
	br.ret.sptk.many rp
	.endp stack_it


#define SET_LOC(n)	add loc##n = n, r8
#define SET_NAT(n)	ld8.s loc##n = [r0]

	.global touch_all
	.proc touch_all
touch_all:
	.prologue
	.save ar.pfs, r34
	alloc loc1 = ar.pfs, 1, 94, 1, 0
	.save rp, loc0
	mov loc0 = rp
	.body

	mov ar.rsc = 0		// put RSE into enforced lazy mode
	shl r8 = in0, 32	// store iteration # in top 32 bits
	add out0 = -1, in0
	cmp.eq p6, p7 = 1, in0
	;;
	SET_LOC( 2); SET_LOC( 3)
	SET_LOC( 4); SET_LOC( 5); SET_LOC( 6); SET_LOC( 7)
	SET_LOC( 8); SET_LOC( 9); SET_LOC(10); SET_LOC(11)
	SET_LOC(12); SET_LOC(13); SET_LOC(14); SET_LOC(15)
	SET_LOC(16); SET_LOC(17); SET_LOC(18); SET_LOC(19)
	SET_LOC(20); SET_LOC(21); SET_LOC(22); SET_LOC(23)
	SET_LOC(24); SET_LOC(25); SET_LOC(26); SET_LOC(27)
	SET_LOC(28); SET_LOC(29); SET_LOC(30); SET_LOC(31)
	SET_LOC(32); SET_LOC(33); SET_LOC(34); SET_LOC(35)
	SET_LOC(36); SET_LOC(37); SET_LOC(38); SET_LOC(39)
	SET_LOC(40); SET_LOC(41); SET_LOC(42); SET_LOC(43)
	SET_LOC(44); SET_LOC(45); SET_LOC(46); SET_LOC(47)
	SET_LOC(48); SET_LOC(49); SET_LOC(50); SET_LOC(51)
	SET_LOC(52); SET_LOC(53); SET_LOC(54); SET_LOC(55)
	SET_LOC(56); SET_LOC(57); SET_LOC(58); SET_LOC(59)
	SET_LOC(60); SET_LOC(61); SET_LOC(62); SET_LOC(63)
	SET_LOC(64); SET_LOC(65); SET_LOC(66); SET_LOC(67)
	SET_LOC(68); SET_LOC(69); SET_LOC(70); SET_LOC(71)
	SET_LOC(72); SET_LOC(73); SET_LOC(74); SET_LOC(75)
	SET_LOC(76); SET_LOC(77); SET_LOC(78); SET_LOC(79)
	SET_LOC(80); SET_LOC(81); SET_LOC(82); SET_LOC(83)
	SET_LOC(84); SET_LOC(85); SET_LOC(86); SET_LOC(87)
	SET_LOC(88); SET_LOC(89); SET_LOC(90); SET_LOC(91)
	SET_LOC(92); SET_LOC(93)
	;;
	SET_NAT(2);  SET_NAT(31); SET_NAT(73); SET_NAT(93)
	;;
{	.mbb
	mov r8=NSTACKS-1
(p6)	br.call.sptk.many rp = stack_it
(p7)	br.call.sptk.many rp = touch_all
}
	;;

	mov rp = loc0
	mov ar.pfs = loc1
	br.ret.sptk.many rp
	.endp touch_all

#ifdef __linux__
        /* We do not need executable stack.  */
        .section        .note.GNU-stack,"",@progbits
#endif
