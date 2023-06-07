;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
; Copyright (C) 2022 Intel Corporation
;
; SPDX-License-Identifier: MIT
;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;

%include "options.asm"
%include "stdmac.asm"

%ifndef UTILS_ASM
%define UTILS_ASM
; compare macro

;; sttni2 is faster, but it can't be debugged
;; so following code is based on "mine5"

;; compares 8 bytes at a time, using xor
;; assumes the input buffer has size at least 8
;; compare_r src1, src2, result, result_max, tmp
%macro compare_r 5
%define %%src1		%1
%define %%src2		%2
%define %%result	%3
%define %%result_max	%4
%define %%tmp		%5
%define %%tmp16		%5w	; tmp as a 16-bit register

	sub	%%result_max, 16
	cmp	%%result, %%result_max
	jg	%%_by_8

%%loop1:
	mov	%%tmp, [%%src1 + %%result]
	xor	%%tmp, [%%src2 + %%result]
	jnz	%%miscompare_reg
	add	%%result, 8

	mov	%%tmp, [%%src1 + %%result]
	xor	%%tmp, [%%src2 + %%result]
	jnz	%%miscompare_reg
	add	%%result, 8
	cmp	%%result, %%result_max
	jle	%%loop1

%%_by_8:
	add	%%result_max, 8
	cmp	%%result, %%result_max
	jg	%%_cmp_last

	; compare last two bytes
	mov	%%tmp, [%%src1 + %%result]
	xor	%%tmp, [%%src2 + %%result]
	jnz	%%miscompare_reg
	add	%%result, 8

%%_cmp_last:
	add	%%result_max, 8
	cmp	%%result, %%result_max
	je	%%end

	lea	%%result, [%%result_max - 8]

	mov	%%tmp, [%%src1 + %%result]
	xor	%%tmp, [%%src2 + %%result]
	jnz	%%miscompare_reg
	add	%%result, 8
	jmp	%%end

%%miscompare_reg:
	bsf	%%tmp, %%tmp
	shr	%%tmp, 3
	add	%%result, %%tmp
%%end:
%endm

;; compares 16 bytes at a time, using pcmpeqb/pmovmskb
;; assumes the input buffer has size at least 8
;; compare_x src1, src2, result, result_max, tmp, xtmp1, xtmp2
%macro compare_x 7
%define %%src1		%1
%define %%src2		%2
%define %%result	%3	; Accumulator for match_length
%define %%result_max	%4
%define %%tmp		%5
%define %%tmp16		%5w	; tmp as a 16-bit register
%define %%tmp32		%5d	; tmp as a 32-bit register
%define %%xtmp		%6
%define %%xtmp2		%7

	sub	%%result_max, 32
	cmp	%%result, %%result_max
	jg	%%_by_16

%%loop1:
	MOVDQU		%%xtmp, [%%src1 + %%result]
	MOVDQU		%%xtmp2, [%%src2 + %%result]
	PCMPEQB		%%xtmp, %%xtmp, %%xtmp2
	PMOVMSKB	%%tmp32, %%xtmp
	xor		%%tmp, 0xFFFF
	jnz		%%miscompare_vect
	add		%%result, 16

	MOVDQU		%%xtmp, [%%src1 + %%result]
	MOVDQU		%%xtmp2, [%%src2 + %%result]
	PCMPEQB		%%xtmp, %%xtmp, %%xtmp2
	PMOVMSKB	%%tmp32, %%xtmp
	xor		%%tmp, 0xFFFF
	jnz		%%miscompare_vect
	add		%%result, 16

	cmp	%%result, %%result_max
	jle	%%loop1

%%_by_16:
	add	%%result_max, 16
	cmp	%%result, %%result_max
	jg	%%_by_8

	MOVDQU		%%xtmp, [%%src1 + %%result]
	MOVDQU		%%xtmp2, [%%src2 + %%result]
	PCMPEQB		%%xtmp, %%xtmp, %%xtmp2
	PMOVMSKB	%%tmp32, %%xtmp
	xor		%%tmp, 0xFFFF
	jnz		%%miscompare_vect
	add		%%result, 16

%%_by_8:
	add	%%result_max, 8
	cmp	%%result, %%result_max
	jg	%%_cmp_last

	; compare last two bytes
	mov	%%tmp, [%%src1 + %%result]
	xor	%%tmp, [%%src2 + %%result]
	jnz	%%miscompare_reg
	add	%%result, 8

%%_cmp_last:
	add	%%result_max, 8
	cmp	%%result, %%result_max
	je	%%end

	lea	%%result, [%%result_max - 8]

	mov	%%tmp, [%%src1 + %%result]
	xor	%%tmp, [%%src2 + %%result]
	jnz	%%miscompare_reg
	add	%%result, 8
	jmp	%%end

%%miscompare_reg:
	bsf	%%tmp, %%tmp
	shr	%%tmp, 3
	add	%%result, %%tmp
	jmp	%%end

%%miscompare_vect:
	bsf	%%tmp, %%tmp
	add	%%result, %%tmp
%%end:
%endm

;; compares 32 bytes at a time, using pcmpeqb/pmovmskb
;; assumes the input buffer has size at least 8
;; compare_y src1, src2, result, result_max, tmp, xtmp1, xtmp2
%macro compare_y 7
%define %%src1		%1
%define %%src2		%2
%define %%result	%3	; Accumulator for match_length
%define %%result_max	%4
%define %%tmp		%5
%define %%tmp16		%5w	; tmp as a 16-bit register
%define %%tmp32		%5d	; tmp as a 32-bit register
%define %%ytmp		%6
%define %%ytmp2		%7

	sub	%%result_max, 64
	cmp	%%result, %%result_max
	jg	%%_by_32

%%loop1:
	vmovdqu		%%ytmp, [%%src1 + %%result]
	vmovdqu		%%ytmp2, [%%src2 + %%result]
	vpcmpeqb	%%ytmp, %%ytmp, %%ytmp2
	vpmovmskb	%%tmp, %%ytmp
	xor		%%tmp32, 0xFFFFFFFF
	jnz		%%miscompare_vect
	add		%%result, 32

	vmovdqu		%%ytmp, [%%src1 + %%result]
	vmovdqu		%%ytmp2, [%%src2 + %%result]
	vpcmpeqb	%%ytmp, %%ytmp, %%ytmp2
	vpmovmskb	%%tmp, %%ytmp
	xor		%%tmp32, 0xFFFFFFFF
	jnz		%%miscompare_vect
	add		%%result, 32

	cmp	%%result, %%result_max
	jle	%%loop1

%%_by_32:
	add	%%result_max, 32
	cmp	%%result, %%result_max
	jg	%%_by_16

	vmovdqu		%%ytmp, [%%src1 + %%result]
	vmovdqu		%%ytmp2, [%%src2 + %%result]
	vpcmpeqb	%%ytmp, %%ytmp, %%ytmp2
	vpmovmskb	%%tmp, %%ytmp
	xor		%%tmp32, 0xFFFFFFFF
	jnz		%%miscompare_vect
	add		%%result, 32

%%_by_16:
	add	%%result_max, 16
	cmp	%%result, %%result_max
	jg	%%_by_8

	vmovdqu		%%ytmp %+ x, [%%src1 + %%result]
	vmovdqu		%%ytmp2 %+ x, [%%src2 + %%result]
	vpcmpeqb	%%ytmp %+ x, %%ytmp %+ x, %%ytmp2 %+ x
	vpmovmskb	%%tmp, %%ytmp %+ x
	xor		%%tmp32, 0xFFFF
	jnz		%%miscompare_vect
	add		%%result, 16

%%_by_8:
	add	%%result_max, 8
	cmp	%%result, %%result_max
	jg	%%_cmp_last

	mov	%%tmp, [%%src1 + %%result]
	xor	%%tmp, [%%src2 + %%result]
	jnz	%%miscompare_reg
	add	%%result, 8

%%_cmp_last:
	add	%%result_max, 8
	cmp	%%result, %%result_max
	je	%%end

	lea	%%result, [%%result_max - 8]

	; compare last two bytes
	mov	%%tmp, [%%src1 + %%result]
	xor	%%tmp, [%%src2 + %%result]
	jnz	%%miscompare_reg
	add	%%result, 8
	jmp	%%end

%%miscompare_reg:
	bsf	%%tmp, %%tmp
	shr	%%tmp, 3
	add	%%result, %%tmp
	jmp	%%end

%%miscompare_vect:
	tzcnt	%%tmp, %%tmp
	add	%%result, %%tmp
%%end:
%endm

;; compares 64 bytes at a time
;; compare_z src1, src2, result, result_max, tmp, ktmp, ztmp1, ztmp2
;; Clobbers result_max
%macro compare_z 8
%define %%src1		%1
%define %%src2		%2
%define %%result	%3	; Accumulator for match_length
%define %%result_max	%4
%define %%tmp		%5	; tmp as a 16-bit register
%define %%ktmp		%6
%define %%ztmp		%7
%define %%ztmp2		%8

	sub	%%result_max, 128
	cmp	%%result, %%result_max
	jg	%%_by_64

%%loop1:
	vmovdqu8	%%ztmp, [%%src1 + %%result]
	vmovdqu8	%%ztmp2, [%%src2 + %%result]
	vpcmpb		%%ktmp, %%ztmp, %%ztmp2, NEQ
	ktestq		%%ktmp, %%ktmp
	jnz		%%miscompare
	add		%%result, 64

	vmovdqu8	%%ztmp, [%%src1 + %%result]
	vmovdqu8	%%ztmp2, [%%src2 + %%result]
	vpcmpb		%%ktmp, %%ztmp, %%ztmp2, NEQ
	ktestq		%%ktmp, %%ktmp
	jnz		%%miscompare
	add		%%result, 64

	cmp	%%result, %%result_max
	jle	%%loop1

%%_by_64:
	add	%%result_max, 64
	cmp	%%result, %%result_max
	jg	%%_less_than_64

	vmovdqu8	%%ztmp, [%%src1 + %%result]
	vmovdqu8	%%ztmp2, [%%src2 + %%result]
	vpcmpb		%%ktmp, %%ztmp, %%ztmp2, NEQ
	ktestq		%%ktmp, %%ktmp
	jnz		%%miscompare
	add		%%result, 64

%%_less_than_64:
	add	%%result_max, 64
	sub	%%result_max, %%result
	jle	%%end

	mov	%%tmp, -1
	bzhi	%%tmp, %%tmp, %%result_max
	kmovq	%%ktmp, %%tmp

	vmovdqu8	%%ztmp {%%ktmp}{z}, [%%src1 + %%result]
	vmovdqu8	%%ztmp2 {%%ktmp}{z}, [%%src2 + %%result]
	vpcmpb		%%ktmp, %%ztmp, %%ztmp2, NEQ
	ktestq		%%ktmp, %%ktmp
	jnz		%%miscompare
	add		%%result, %%result_max

	jmp	%%end
%%miscompare:
	kmovq	%%tmp, %%ktmp
	tzcnt	%%tmp, %%tmp
	add	%%result, %%tmp
%%end:
%endm

%macro compare250 7
%define %%src1		%1
%define %%src2		%2
%define %%result	%3
%define %%result_max	%4
%define %%tmp		%5
%define %%xtmp0		%6x
%define %%xtmp1		%7x
%define %%ytmp0		%6
%define %%ytmp1		%7

	mov	%%tmp, 250
	cmp	%%result_max, 250
	cmovg	%%result_max, %%tmp

%if (COMPARE_TYPE == 1)
	compare_r	%%src1, %%src2, %%result, %%result_max, %%tmp
%elif (COMPARE_TYPE == 2)
	compare_x	%%src1, %%src2, %%result, %%result_max, %%tmp, %%xtmp0, %%xtmp1
%elif (COMPARE_TYPE == 3)
	compare_y	%%src1, %%src2, %%result, %%result_max, %%tmp, %%ytmp0, %%ytmp1
%else
%error Unknown Compare type COMPARE_TYPE
 % error
%endif
%endmacro

; Assumes the buffer has at least 8 bytes
; Accumulates match length onto result
%macro compare_large 7
%define %%src1		%1
%define %%src2		%2
%define %%result	%3
%define %%result_max	%4
%define %%tmp		%5
%define %%xtmp0		%6x
%define %%xtmp1		%7x
%define %%ytmp0		%6
%define %%ytmp1		%7

%if (COMPARE_TYPE == 1)
	compare_r	%%src1, %%src2, %%result, %%result_max, %%tmp
%elif (COMPARE_TYPE == 2)
	compare_x	%%src1, %%src2, %%result, %%result_max, %%tmp, %%xtmp0, %%xtmp1
%elif (COMPARE_TYPE == 3)
	compare_y	%%src1, %%src2, %%result, %%result_max, %%tmp, %%ytmp0, %%ytmp1
%else
%error Unknown Compare type COMPARE_TYPE
 % error
%endif
%endmacro

;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;

;; compare size, src1, src2, result, tmp
%macro compare 5
%define %%size		%1
%define %%src1		%2
%define %%src2		%3
%define %%result	%4
%define %%tmp		%5
%define %%tmp8		%5b	; tmp as a 8-bit register

	xor	%%result, %%result
	sub	%%size, 7
	jle	%%lab2
%%loop1:
	mov	%%tmp, [%%src1 + %%result]
	xor	%%tmp, [%%src2 + %%result]
	jnz	%%miscompare
	add	%%result, 8
	sub	%%size, 8
	jg	%%loop1
%%lab2:
	;; if we fall through from above, we have found no mismatches,
	;; %%size+7 is the number of bytes left to look at, and %%result is the
	;; number of bytes that have matched
	add	%%size, 7
	jle	%%end
%%loop3:
	mov	%%tmp8, [%%src1 + %%result]
	cmp	%%tmp8, [%%src2 + %%result]
	jne	%%end
	inc	%%result
	dec	%%size
	jg	%%loop3
	jmp	%%end
%%miscompare:
	bsf	%%tmp, %%tmp
	shr	%%tmp, 3
	add	%%result, %%tmp
%%end:
%endm

%endif	;UTILS_ASM
