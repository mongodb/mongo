	.globl func_add1, func_add1_end
	.proc func_add1
func_add1:
{.mib;	add r8 = 1, r32
	nop.i 0
	br.ret.sptk.many rp
}
func_add1_end:
	.endp func_add1

	.globl func_add3, func_add3_end
	.proc func_add3
func_add3:
{.mmi;	alloc loc0 = ar.pfs, 2, 1, 2, 0
	mov r2 = sp
	add sp = -16, sp
}	;;
{.mii;	ld8 r8 = [in1], 8		// load the function pointer
	mov r3 = rp
	mov rp = loc0			// trash rp
}	;;
{.mmi;	ld8 r9 = [r8], 8		// load the entry-point
	st8 [r2] = r3
	mov out0 = in0
}	;;
{.mii;	ld8 gp = [r8]			// load the gp
	mov b6 = r9
	mov out1 = in1
}
{.mib;	nop 0
	nop 0
	br.call.sptk rp = b6
}
{.mmi;	add r2 = 16, sp
	;;
	ld8 r3 = [r2]			// r3 = saved rp
	mov ar.pfs = loc0
}	;;
{.mii;	nop 0
	mov rp = r3
	adds sp = 16, sp
}	;;
{.mib;	st8 [sp] = in0			// trash rp save location
	add r8 = 2, r8
	br.ret.sptk.many rp
}
func_add3_end:
	.endp func_add3

	.globl func_vframe, func_vframe_end
	.proc func_vframe
func_vframe:
{.mii;	alloc r16 = ar.pfs, 1, 2, 0, 0	// 0
	mov loc0 = rp
	mov loc1 = sp
}	;;
{.mmi;	sub sp = sp, in0
	st8 [loc1] = r16
	mov r2 = -99			// 0
}	;;
{.mii;	nop 0
	mov rp = r2
	mov ar.pfs = r0
}
{.mib;	mov r16 = r2
	tbit.nz p6, p0 = in0, 4
(p6)	br.cond.sptk.many .exit
}	;;
{.mmi;	ld8 r16 = [loc1]
	;;
	mov r3 = loc0			// 8 move saved rp to r3
	mov ar.pfs = r16
}	;;
{.mmi;	mov sp = loc1			// 10
	st8 [loc1] = r0			// trash saved pfs
	mov loc0 = r2
}	;;
{.mib;	mov r8 = 10
	mov rp = r3
	br.ret.sptk.many rp
}
.exit:
{.mmi;	ld8 r16 = [loc1]
	;;
	sub sp = 32, sp
	mov ar.pfs = r16
}	;;
{.mmi;	mov sp = loc1
	st8 [loc1] = r0			// trash saved pfs
	mov rp = loc0
}
{.mib;	nop 0
	mov r8 = 4
	br.ret.sptk.many rp
}
func_vframe_end:
	.endp func_vframe

#ifdef __linux__
        /* We do not need executable stack.  */
        .section        .note.GNU-stack,"",@progbits
#endif
