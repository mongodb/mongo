;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
; Copyright (C) 2022 Intel Corporation
;
; SPDX-License-Identifier: MIT
;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;

%ifndef STDMAC_ASM
%define STDMAC_ASM
;; internal macro used by push_all
;; push args L to R
%macro push_all_ 1-*
%xdefine _PUSH_ALL_REGS_COUNT_ %0
%rep %0
	push %1
	%rotate 1
%endrep
%endmacro

;; internal macro used by pop_all
;; pop args R to L
%macro pop_all_ 1-*
%rep %0
	%rotate -1
	pop %1
%endrep
%endmacro

%xdefine _PUSH_ALL_REGS_COUNT_ 0
%xdefine _ALLOC_STACK_VAL_     0
;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
;; STACK_OFFSET
;; Number of bytes subtracted from stack due to PUSH_ALL and ALLOC_STACK
;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
%define STACK_OFFSET (_PUSH_ALL_REGS_COUNT_ * 8 + _ALLOC_STACK_VAL_)

;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
;; PUSH_ALL reg1, reg2, ...
;; push args L to R, remember regs for pop_all
;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
%macro PUSH_ALL 1+
%xdefine _PUSH_ALL_REGS_ %1
	push_all_ %1
%endmacro

;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
;; POP_ALL
;; push args from prev "push_all" R to L
;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
%macro POP_ALL 0
	pop_all_ _PUSH_ALL_REGS_
%xdefine _PUSH_ALL_REGS_COUNT_ 0
%endmacro

;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
;; ALLOC_STACK n
;; subtract n from the stack pointer and remember the value for restore_stack
;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
%macro ALLOC_STACK 1
%xdefine _ALLOC_STACK_VAL_ %1
	sub	rsp, %1
%endmacro

;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
;; RESTORE_STACK
;; add n to the stack pointer, where n is the arg to the previous alloc_stack
;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
%macro RESTORE_STACK 0
	add	rsp, _ALLOC_STACK_VAL_
%xdefine _ALLOC_STACK_VAL_     0
%endmacro


;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
;; NOPN n
;; Create n bytes of NOP, using nops of up to 8 bytes each
;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
%macro NOPN 1

 %assign %%i %1
 %rep 200
  %if (%%i < 9)
	nopn %%i
	%exitrep
  %else
	nopn 8
	%assign %%i (%%i - 8)
  %endif
 %endrep
%endmacro

;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
;; nopn n
;; Create n bytes of NOP, where n is between 1 and 9
;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
%macro nopn 1
%if (%1 == 1)
	nop
%elif (%1 == 2)
	db	0x66
	nop
%elif (%1 == 3)
	db	0x0F
	db	0x1F
	db	0x00
%elif (%1 == 4)
	db	0x0F
	db	0x1F
	db	0x40
	db	0x00
%elif (%1 == 5)
	db	0x0F
	db	0x1F
	db	0x44
	db	0x00
	db	0x00
%elif (%1 == 6)
	db	0x66
	db	0x0F
	db	0x1F
	db	0x44
	db	0x00
	db	0x00
%elif (%1 == 7)
	db	0x0F
	db	0x1F
	db	0x80
	db	0x00
	db	0x00
	db	0x00
	db	0x00
%elif (%1 == 8)
	db	0x0F
	db	0x1F
	db	0x84
	db	0x00
	db	0x00
	db	0x00
	db	0x00
	db	0x00
%elif (%1 == 9)
	db	0x66
	db	0x0F
	db	0x1F
	db	0x84
	db	0x00
	db	0x00
	db	0x00
	db	0x00
	db	0x00
%else
%error Invalid value to nopn
%endif
%endmacro

;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
;; rolx64 dst, src, amount
;; Emulate a rolx instruction using rorx, assuming data 64 bits wide
;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
%macro rolx64 3
	rorx %1, %2, (64-%3)
%endm

;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
;; rolx32 dst, src, amount
;; Emulate a rolx instruction using rorx, assuming data 32 bits wide
;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
%macro rolx32 3
	rorx %1, %2, (32-%3)
%endm


;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
;; Define a function void ssc(uint64_t x)
;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
%macro DEF_SSC 0
global ssc
ssc:
	mov	rax, rbx
	mov	rbx, rcx
	db	0x64
	db	0x67
	nop
	mov	rbx, rax
	ret
%endm

%macro	MOVDQU	2
%define	%%dest	%1
%define	%%src	%2
%if ((ARCH == 02) || (ARCH == 03) || (ARCH == 04))
	vmovdqu	%%dest, %%src
%else
	movdqu	%%dest, %%src
%endif
%endm

%macro	MOVDQA	2
%define	%%dest	%1
%define	%%src	%2
%if ((ARCH == 02) || (ARCH == 03) || (ARCH == 04))
	vmovdqa	%%dest, %%src
%else
	movdqa	%%dest, %%src
%endif
%endm

%macro	MOVD	2
%define	%%dest	%1
%define	%%src	%2
%if (ARCH == 02 || ARCH == 03 || ARCH == 04)
	vmovd	%%dest, %%src
%else
	movd	%%dest, %%src
%endif
%endm

%macro	MOVQ	2
%define	%%dest	%1
%define	%%src	%2
%if (ARCH == 02 || ARCH == 03 || ARCH == 04)
	vmovq	%%dest, %%src
%else
	movq	%%dest, %%src
%endif
%endm

;; Move register if the src and dest are not equal
%macro MOVNIDN 2
%define dest %1
%define src %2
%ifnidn dest, src
	mov	dest, src
%endif
%endm

%macro MOVDQANIDN 2
%define dest %1
%define src %2
%ifnidn dest, src
	MOVDQA	dest, src
%endif
%endm

%macro PSHUFD	3
%define	%%dest	%1
%define %%src1	%2
%define	%%imm8	%3
%if ((ARCH == 02) || (ARCH == 03) || (ARCH == 04))
	vpshufd	%%dest, %%src1, %%imm8
%else
	pshufd	%%dest, %%src1, %%imm8
%endif
%endm

%macro PSHUFB	3
%define	%%dest	%1
%define %%src1	%2
%define	%%shuf	%3
%if ((ARCH == 02) || (ARCH == 03) || (ARCH == 04))
	vpshufb	%%dest, %%src1, %%shuf
%else
	MOVDQANIDN %%dest, %%src1
	pshufb	%%dest, %%shuf
%endif
%endm

%macro PBROADCASTD 2
%define %%dest %1
%define %%src %2
%if (ARCH == 04)
	vpbroadcastd %%dest, %%src
%else
	MOVD %%dest, %%src
	PSHUFD %%dest, %%dest, 0
%endif
%endm

;; Implement BZHI instruction on older architectures
;; Clobbers rcx, unless rcx is %%index
%macro	BZHI	4
%define	%%dest		%1
%define	%%src		%2
%define	%%index		%3
%define	%%tmp1		%4

%ifdef USE_HSWNI
	bzhi	%%dest, %%src, %%index
%else
	MOVNIDN	rcx, %%index
	mov	%%tmp1, 1
	shl	%%tmp1, cl
	sub	%%tmp1, 1

	MOVNIDN	%%dest, %%src

	and	%%dest, %%tmp1
%endif
%endm

;; Implement shrx instruction on older architectures
;; Clobbers rcx, unless rcx is %%index
%macro	SHRX	3
%define	%%dest		%1
%define	%%src		%2
%define	%%index		%3

%ifdef USE_HSWNI
	shrx	%%dest, %%src, %%index
%else
	MOVNIDN	rcx, %%index
	MOVNIDN	%%dest, %%src
	shr	%%dest, cl
%endif
%endm

;; Implement shlx instruction on older architectures
;; Clobbers rcx, unless rcx is %%index
%macro	SHLX	3
%define	%%dest		%1
%define	%%src		%2
%define	%%index		%3

%ifdef USE_HSWNI
	shlx	%%dest, %%src, %%index
%else
	MOVNIDN	%%dest, %%src
	MOVNIDN	rcx, %%index
	shl	%%dest, cl
%endif
%endm

%macro	PINSRD	3
%define	%%dest	%1
%define	%%src	%2
%define	%%offset	%3
%if ((ARCH == 02) || (ARCH == 03) || (ARCH == 04))
	vpinsrd	%%dest, %%src, %%offset
%else
	pinsrd	%%dest, %%src, %%offset
%endif
%endm

%macro	PEXTRD	3
%define	%%dest	%1
%define	%%src	%2
%define	%%offset	%3
%if ((ARCH == 02) || (ARCH == 03) || (ARCH == 04))
	vpextrd	%%dest, %%src, %%offset
%else
	pextrd	%%dest, %%src, %%offset
%endif
%endm

%macro	PSRLDQ	2
%define	%%dest	%1
%define	%%offset	%2
%if ((ARCH == 02) || (ARCH == 03) || (ARCH == 04))
	vpsrldq	%%dest, %%offset
%else
	psrldq	%%dest, %%offset
%endif
%endm

%macro	PSLLD	3
%define	%%dest	%1
%define %%src	%2
%define	%%offset	%3
%if ((ARCH == 02) || (ARCH == 03) || (ARCH == 04))
	vpslld	%%dest, %%src, %%offset
%else
	MOVDQANIDN %%dest, %%src
	pslld	%%dest, %%offset
%endif
%endm

%macro	PAND	3
%define	%%dest	%1
%define	%%src1	%2
%define	%%src2	%3
%if (ARCH == 02 || ARCH == 03 || ARCH == 04)
	vpand	%%dest, %%src1, %%src2
%else
	MOVDQANIDN %%dest, %%src1
	pand	%%dest, %%src2
%endif
%endm

%macro	POR	3
%define	%%dest	%1
%define	%%src1	%2
%define	%%src2	%3
%if (ARCH == 02 || ARCH == 03 || ARCH == 04)
	vpor	%%dest, %%src1, %%src2
%else
	MOVDQANIDN %%dest, %%src1
	por	%%dest, %%src2
%endif
%endm

%macro PXOR	3
%define	%%dest	%1
%define %%src1	%2
%define	%%src2	%3
%if ((ARCH == 02) || (ARCH == 03) || (ARCH == 04))
	vpxor	%%dest, %%src1, %%src2
%else
	MOVDQANIDN %%dest, %%src1
	pxor	%%dest, %%src2
%endif
%endm

%macro PADDD 3
%define %%dest %1
%define %%src1 %2
%define %%src2 %3
%if ((ARCH == 02) || (ARCH == 03) || (ARCH == 04))
	vpaddd	%%dest, %%src1, %%src2
%else
	MOVDQANIDN %%dest, %%src1
	paddd	%%dest, %%src2
%endif
%endm

%macro	PCMPEQB	3
%define	%%dest	%1
%define	%%src1	%2
%define	%%src2	%3
%if ((ARCH == 02) || (ARCH == 03) || (ARCH == 04))
	vpcmpeqb	%%dest, %%src1, %%src2
%else
	MOVDQANIDN %%dest, %%src1
	pcmpeqb	%%dest, %%src2
%endif
%endm

%macro	PMOVMSKB	2
%define	%%dest	%1
%define	%%src	%2
%if ((ARCH == 02) || (ARCH == 03) || (ARCH == 04))
	vpmovmskb	%%dest, %%src
%else
	pmovmskb	%%dest, %%src
%endif
%endm

%endif 	;; ifndef STDMAC_ASM
