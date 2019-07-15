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

#include "ia64-test-rbs.h"

	.common stackmem, NSTACKS*STACK_SIZE, 16

	.text

#define SAVED_SP_OFF		 0
#define SAVED_RP_OFF		 8
#define SAVED_PFS_OFF		16
#define SAVED_RNAT_OFF		24
#define SAVED_BSP_OFF		32
#define SAVED_BSPSTORE_OFF	40
#define FRAME_SIZE		48

#define SPILL(n)							     \
	/* int rbs_spill_#n(long iteration, int (*next_func[])()) */	     \
	.globl rbs_spill_##n;						     \
	.proc rbs_spill_##n;						     \
rbs_spill_##n:								     \
	.prologue;							     \
	alloc r18 = ar.pfs, 2, (n)-2, 2, 0;/* read ar.pfs */		     \
	/* first, calculate address of new stack: */			     \
	addl r2 = @ltoff(stackmem), gp;					     \
	add r8 = 1, in0;						     \
	;;								     \
	ld8 r2 = [r2];			/* r2 = &stackmem */		     \
	shl r3 = in0, STACK_SIZE_SHIFT;					     \
	shladd r8 = r8, 3, in1;		/* r8 = &next_func[iteration+1] */   \
	;;								     \
	ld8 r8 = [r8];			/* r8 = next_func[iteration+1] */    \
	add r2 = r2, r3;		/* r2 = stackmem[iteration] */	     \
	;;								     \
	ld8 r9 = [r8], 8;;		/* r9 = target's entry-point */	     \
	ld8 gp = [r8];			/* r22 = target's gp */		     \
	addl r3 = STACK_SIZE-FRAME_SIZE, r2; /* r3 = &stackframe */	     \
	;;								     \
	mov b6 = r9;							     \
	st8 [r3] = sp;							     \
	.vframesp SAVED_SP_OFF+16;					     \
	adds sp = -16, r3;		/* switch the memory stack */	     \
	;;								     \
	adds r3 = (SAVED_RP_OFF - SAVED_SP_OFF), r3;			     \
	mov r16 = rp;							     \
	;;								     \
	.savesp rp, SAVED_RP_OFF+16;					     \
	st8 [r3] = r16, (SAVED_PFS_OFF - SAVED_RP_OFF);			     \
	;;								     \
	.savesp ar.pfs, SAVED_PFS_OFF+16;				     \
	st8 [r3] = r18, (SAVED_BSP_OFF - SAVED_PFS_OFF);		     \
	mov r16 = ar.bsp;						     \
	mov r17 = ar.bspstore;						     \
	mov r18 = ar.rnat;						     \
	;;								     \
	.savesp ar.bsp, SAVED_BSP_OFF+16;				     \
	st8 [r3] = r16, (SAVED_BSPSTORE_OFF - SAVED_BSP_OFF);		     \
	;;								     \
	.savesp ar.bspstore, SAVED_BSPSTORE_OFF+16;			     \
	st8 [r3] = r17, (SAVED_RNAT_OFF - SAVED_BSPSTORE_OFF);		     \
	mov out1 = in1;							     \
	;;								     \
	.savesp ar.rnat, SAVED_RNAT_OFF+16;				     \
	st8 [r3] = r18;							     \
	.body;								     \
	mov ar.bspstore = r2;		/* switch the backing store */	     \
	adds out0 = 1, in0;						     \
	;;								     \
	br.call.sptk.many rp = b6;					     \
1:	/* switch back to stack: */					     \
	adds r3 = SAVED_SP_OFF+16, sp;					     \
	cmp.ge p8, p0 = r8, r0;						     \
	;;								     \
(p8)	add r8 = 1, r8;							     \
	ld8 r16 = [r3], (SAVED_RP_OFF-SAVED_SP_OFF);;	/* saved sp */	     \
	ld8 r17 = [r3], (SAVED_PFS_OFF-SAVED_RP_OFF);;	/* saved rp */	     \
	ld8 r18 = [r3], (SAVED_RNAT_OFF-SAVED_PFS_OFF);;/* saved pfs */	     \
	ld8 r19 = [r3], (SAVED_BSP_OFF-SAVED_RNAT_OFF);;/* saved rnat */     \
	ld8 r20 = [r3], (SAVED_BSPSTORE_OFF-SAVED_BSP_OFF);;/* saved bsp */  \
	ld8 r21 = [r3];;				/* saved bspstore */ \
	mov rp = r17;							     \
	mov ar.pfs = r18;						     \
	shl r3 = in0, STACK_SIZE_SHIFT;					     \
	addl r2 = @ltoff(stackmem), gp;;				     \
	ld8 r2 = [r2];;			/* r2 = &stackmem */		     \
	add r2 = r2, r3;		/* r2 = stackmem[iteration] */	     \
	mov r3 = ar.bsp;;						     \
	sub r2 = r3, r2;;		/* r2 = dirty_size */		     \
	shl r2 = r2, 16;;						     \
	mov ar.rsc = r2;;						     \
	alloc r3 = ar.pfs, 0, 0, 0, 0;;					     \
	loadrs;;							     \
	mov ar.bspstore = r21;;	/* this also restores ar.bsp */		     \
	mov ar.rnat = r19;						     \
	.restore sp;							     \
	mov sp = r16;							     \
	br.ret.sptk.many rp;						     \
	.endp rbs_spill_##n

		        SPILL(2);  SPILL(3)
  SPILL(4);  SPILL(5);  SPILL(6);  SPILL(7)
  SPILL(8);  SPILL(9); SPILL(10); SPILL(11)
 SPILL(12); SPILL(13); SPILL(14); SPILL(15)
 SPILL(16); SPILL(17); SPILL(18); SPILL(19)
 SPILL(20); SPILL(21); SPILL(22); SPILL(23)
 SPILL(24); SPILL(25); SPILL(26); SPILL(27)
 SPILL(28); SPILL(29); SPILL(30); SPILL(31)
 SPILL(32); SPILL(33); SPILL(34); SPILL(35)
 SPILL(36); SPILL(37); SPILL(38); SPILL(39)
 SPILL(40); SPILL(41); SPILL(42); SPILL(43)
 SPILL(44); SPILL(45); SPILL(46); SPILL(47)
 SPILL(48); SPILL(49); SPILL(50); SPILL(51)
 SPILL(52); SPILL(53); SPILL(54); SPILL(55)
 SPILL(56); SPILL(57); SPILL(58); SPILL(59)
 SPILL(60); SPILL(61); SPILL(62); SPILL(63)
 SPILL(64); SPILL(65); SPILL(66); SPILL(67)
 SPILL(68); SPILL(69); SPILL(70); SPILL(71)
 SPILL(72); SPILL(73); SPILL(74); SPILL(75)
 SPILL(76); SPILL(77); SPILL(78); SPILL(79)
 SPILL(80); SPILL(81); SPILL(82); SPILL(83)
 SPILL(84); SPILL(85); SPILL(86); SPILL(87)
 SPILL(88); SPILL(89); SPILL(90); SPILL(91)
 SPILL(92); SPILL(93); SPILL(94)

#define LD_LOC(n)				\
	ld4 loc##n = [in1], 4;;			\
	cmp.eq p8, p9 = r0, loc##n;;		\
(p9)	or loc##n = loc##n, r8;			\
(p8)	ld4.s loc##n = [r0]

#define CK_LOC(n)				\
	ld4 r16 = [in1], 4;;			\
	cmp.eq p8, p9 = r0, r16;		\
	or r16 = r16, r9;;			\
(p8)	tnat.z p10, p0 = loc##n;		\
(p9)	cmp.ne p10, p0 = r16, loc##n;		\
	;;					\
(p10)	mov r8 = -n;				\
(p10)	br.cond.spnt.many .fail

	/* int loadup(long iteration, int *values, next_func[]) */

	.global loadup
	.proc loadup
loadup:
	.prologue
	.save ar.pfs, r36
	alloc loc1 = ar.pfs, 3, 90, 3, 0
	.save rp, loc0
	mov loc0 = rp
	.body
	cmp.eq p6, p7 = 1, in0
	;;
	mov ar.rsc = 0		// put RSE into enforced lazy mode
(p6)	mov out1 = in2
(p7)	mov out2 = in2

(p6)	ld8 r17 = [in2]		// get address of function descriptor
(p7)	add out0 = -1, in0
(p7)	mov out1 = in1

	;;
(p6)	ld8 r16 = [r17], 8	// load entry point
	shl r8 = in0, 32	// store iteration # in top 32 bits
	mov r18 = in1
	;;
(p6)	ld8 r1 = [r17]		// load gp
(p6)	mov b6 = r16

(p6)	mov out0 = 0
	;;
	LD_LOC( 2); LD_LOC( 3)
	LD_LOC( 4); LD_LOC( 5); LD_LOC( 6); LD_LOC( 7)
	LD_LOC( 8); LD_LOC( 9); LD_LOC(10); LD_LOC(11)
	LD_LOC(12); LD_LOC(13); LD_LOC(14); LD_LOC(15)
	LD_LOC(16); LD_LOC(17); LD_LOC(18); LD_LOC(19)
	LD_LOC(20); LD_LOC(21); LD_LOC(22); LD_LOC(23)
	LD_LOC(24); LD_LOC(25); LD_LOC(26); LD_LOC(27)
	LD_LOC(28); LD_LOC(29); LD_LOC(30); LD_LOC(31)
	LD_LOC(32); LD_LOC(33); LD_LOC(34); LD_LOC(35)
	LD_LOC(36); LD_LOC(37); LD_LOC(38); LD_LOC(39)
	LD_LOC(40); LD_LOC(41); LD_LOC(42); LD_LOC(43)
	LD_LOC(44); LD_LOC(45); LD_LOC(46); LD_LOC(47)
	LD_LOC(48); LD_LOC(49); LD_LOC(50); LD_LOC(51)
	LD_LOC(52); LD_LOC(53); LD_LOC(54); LD_LOC(55)
	LD_LOC(56); LD_LOC(57); LD_LOC(58); LD_LOC(59)
	LD_LOC(60); LD_LOC(61); LD_LOC(62); LD_LOC(63)
	LD_LOC(64); LD_LOC(65); LD_LOC(66); LD_LOC(67)
	LD_LOC(68); LD_LOC(69); LD_LOC(70); LD_LOC(71)
	LD_LOC(72); LD_LOC(73); LD_LOC(74); LD_LOC(75)
	LD_LOC(76); LD_LOC(77); LD_LOC(78); LD_LOC(79)
	LD_LOC(80); LD_LOC(81); LD_LOC(82); LD_LOC(83)
	LD_LOC(84); LD_LOC(85); LD_LOC(86); LD_LOC(87)
	LD_LOC(88); LD_LOC(89)
	;;
{	.mbb
	mov in1 = r18
(p6)	br.call.sptk.many rp = b6
(p7)	br.call.sptk.many rp = loadup
}
	cmp.lt p8, p9 = r8, r0
	shl r9 = in0, 32	// store iteration # in top 32 bits
(p8)	br.cond.spnt.few .fail
	;;
	add r8 = 1, r8
	CK_LOC( 2); CK_LOC( 3)
	CK_LOC( 4); CK_LOC( 5); CK_LOC( 6); CK_LOC( 7)
	CK_LOC( 8); CK_LOC( 9); CK_LOC(10); CK_LOC(11)
	CK_LOC(12); CK_LOC(13); CK_LOC(14); CK_LOC(15)
	CK_LOC(16); CK_LOC(17); CK_LOC(18); CK_LOC(19)
	CK_LOC(20); CK_LOC(21); CK_LOC(22); CK_LOC(23)
	CK_LOC(24); CK_LOC(25); CK_LOC(26); CK_LOC(27)
	CK_LOC(28); CK_LOC(29); CK_LOC(30); CK_LOC(31)
	CK_LOC(32); CK_LOC(33); CK_LOC(34); CK_LOC(35)
	CK_LOC(36); CK_LOC(37); CK_LOC(38); CK_LOC(39)
	CK_LOC(40); CK_LOC(41); CK_LOC(42); CK_LOC(43)
	CK_LOC(44); CK_LOC(45); CK_LOC(46); CK_LOC(47)
	CK_LOC(48); CK_LOC(49); CK_LOC(50); CK_LOC(51)
	CK_LOC(52); CK_LOC(53); CK_LOC(54); CK_LOC(55)
	CK_LOC(56); CK_LOC(57); CK_LOC(58); CK_LOC(59)
	CK_LOC(60); CK_LOC(61); CK_LOC(62); CK_LOC(63)
	CK_LOC(64); CK_LOC(65); CK_LOC(66); CK_LOC(67)
	CK_LOC(68); CK_LOC(69); CK_LOC(70); CK_LOC(71)
	CK_LOC(72); CK_LOC(73); CK_LOC(74); CK_LOC(75)
	CK_LOC(76); CK_LOC(77); CK_LOC(78); CK_LOC(79)
	CK_LOC(80); CK_LOC(81); CK_LOC(82); CK_LOC(83)
	CK_LOC(84); CK_LOC(85); CK_LOC(86); CK_LOC(87)
	CK_LOC(88); CK_LOC(89)
.fail:
	mov rp = loc0
	mov ar.pfs = loc1
	br.ret.sptk.many rp
	.endp loadup

	.global resumption_point_label
	.proc resumption_point
resumption_point:
resumption_point_label:
	.prologue
	.save rp, r16
	.save ar.pfs, r0
	.body
	mov r8 = r15
	mov b6 = r16
	;;
	br.cond.sptk.many b6
	.endp resumption_point

#ifdef __linux__
        /* We do not need executable stack.  */
        .section        .note.GNU-stack,"",@progbits
#endif
