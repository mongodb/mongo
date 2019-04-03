#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#ifndef HAVE__BUILTIN___CLEAR_CACHE

#if defined(__ia64__)

	.global flush_cache

	.proc flush_cache
flush_cache:
	.prologue
        alloc r2=ar.pfs,2,0,0,0
	add r8=31,in1			// round up to 32 byte-boundary
        ;;
        shr.u r8=r8,5                   // we flush 32 bytes per iteration
	;;
	add r8=-1,r8
        .save ar.lc, r3
        mov r3=ar.lc                    // save ar.lc
        ;;
        .body

        mov ar.lc=r8
        ;;
.loop:  fc in0                          // issuable on M0 only
        add in0=32,in0
        br.cloop.sptk.few .loop
        ;;
        sync.i
        ;;
        srlz.i
        ;;
        mov ar.lc=r3                    // restore ar.lc
        br.ret.sptk.many rp
	.endp flush_cache

#elif defined(__i386__) || defined (__x86_64__)

	.globl flush_cache
flush_cache:
	ret

#elif defined(__hppa__)

# warning FIX ME!!

        .globl flush_cache
flush_cache:
        .proc
        .callinfo
        bv      %r0(%rp)
        .procend
#elif defined(__powerpc64__)
# warning IMPLEMENT ME FOR PPC64!!
	.globl flush_cache
flush_cache:
	lwz    11,  0(1)  ;
	lwz     0,  4(11) ;
	mtlr    0         ;
	lwz    31, -4(11) ;
	mr      1, 11     ;
	blr
#elif defined(__powerpc__)
# warning IMPLEMENT ME FOR PPC32!!
	.globl flush_cache
flush_cache:
	lwz    11,  0(1)  ;
	lwz     0,  4(11) ;
	mtlr    0         ;
	lwz    31, -4(11) ;
	mr      1, 11     ;
	blr
#elif defined(__arm__)
	.text
	.globl flush_cache
flush_cache:
	bx    lr
#elif defined(__tilegx__)
        .text
        .globl flush_cache
flush_cache:

        andi r0, r0, -64
1:      {
         flush r0 ;
         addi  r0, r0, 64
        }
        {
         bgtz r1, 1b ;
         addi r1, r1, -64
        }
        jrp   lr
#else
# error Need flush_cache code for this architecture.
#endif

#if defined ( __linux__) && !defined (__arm__)
        /* We do not need executable stack.  */
        .section        .note.GNU-stack,"",@progbits
#endif

#endif
