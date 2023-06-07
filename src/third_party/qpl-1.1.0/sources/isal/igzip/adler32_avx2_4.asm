;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
; Copyright (C) 2022 Intel Corporation
;
; SPDX-License-Identifier: MIT
;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;

; uint32_t adler32_avx2(uint32_t init, const unsigned char *buf, uint64_t len)

%define LIMIT 5552
%define BASE  0xFFF1 ; 65521

%define CHUNKSIZE 16
%define CHUNKSIZE_M1 (CHUNKSIZE-1)

%include "reg_sizes.asm"

default rel
[bits 64]

; need to keep free: eax, ecx, edx

%ifidn __OUTPUT_FORMAT__, elf64
 %define arg1   rdi
 %define arg2   rsi
 %define arg3   rdx

 %define init_d edi
 %define data   r9
 %define size   r10
 %define s      r11
 %define a_d    r12d
 %define b_d    r8d
 %define end    r13

 %define func(x) x: endbranch
 %macro FUNC_SAVE 0
	push	r12
	push	r13
 %endmacro
 %macro FUNC_RESTORE 0
	pop	r13
	pop	r12
 %endmacro
%endif

%ifidn __OUTPUT_FORMAT__, win64
 %define arg1   rcx
 %define arg2   rdx
 %define arg3   r8

 %define init_d r12d
 %define data   r9
 %define size	r10
 %define s	r11
 %define a_d	esi
 %define b_d	edi
 %define end	r13

 %define stack_size  2*16 + 5*8		; must be an odd multiple of 8
 %define arg(x)      [rsp + stack_size + PS + PS*x]
 %define func(x) proc_frame x
 %macro FUNC_SAVE 0
	alloc_stack	stack_size
	vmovdqa	[rsp + 0*16], xmm6
	vmovdqa	[rsp + 1*16], xmm7
	save_reg	rdi,  2*16 + 0*8
	save_reg	rsi,  2*16 + 1*8
	save_reg	r12,  2*16 + 2*8
	save_reg	r13,  2*16 + 3*8
	end_prolog
	mov	init_d, ecx	; initalize init_d from arg1 to keep ecx free
 %endmacro

 %macro FUNC_RESTORE 0
	vmovdqa	xmm6, [rsp + 0*16]
	vmovdqa	xmm7, [rsp + 1*16]
	mov	rdi,  [rsp + 2*16 + 0*8]
	mov	rsi,  [rsp + 2*16 + 1*8]
	mov	r12,  [rsp + 2*16 + 2*8]
	mov	r13,  [rsp + 2*16 + 3*8]
	add	rsp, stack_size
 %endmacro
%endif

%define ya	ymm0
%define yb	ymm1
%define ydata0	ymm2
%define ydata1	ymm3
%define ysa	ymm4
%define ydata   ysa
%define ytmp0   ydata0
%define ytmp1   ydata1
%define ytmp2   ymm5
%define xa	xmm0
%define xb      xmm1
%define xtmp0   xmm2
%define xtmp1   xmm3
%define xsa     xmm4
%define xtmp2   xmm5
%define yshuf0	ymm6
%define yshuf1	ymm7

[bits 64]
default rel
section .text

mk_global adler32_avx2_4, function
func(adler32_avx2_4)
	FUNC_SAVE

	vmovdqa	yshuf0, [SHUF0]
	vmovdqa	yshuf1, [SHUF1]

	mov	data, arg2
	mov	size, arg3

	mov	b_d, init_d
	shr	b_d, 16
	and	init_d, 0xFFFF
	cmp	size, 32
	jb	.lt64
	vmovd	xa, init_d
	vpxor	yb, yb, yb
.sloop1:
	mov	s, LIMIT
	cmp	s, size
	cmova	s, size		; s = min(size, LIMIT)
	lea	end, [data + s - CHUNKSIZE_M1]
	cmp	data, end
	jae	.skip_loop_1a
align 32
.sloop1a:
	; do CHUNKSIZE adds
	vbroadcastf128	ydata, [data]
	add	data, CHUNKSIZE
	vpshufb	ydata0, ydata, yshuf0
	vpaddd	ya, ya, ydata0
	vpaddd	yb, yb, ya
	vpshufb	ydata1, ydata, yshuf1
	vpaddd	ya, ya, ydata1
	vpaddd	yb, yb, ya
	cmp	data, end
	jb	.sloop1a

.skip_loop_1a:
	add	end, CHUNKSIZE_M1

	test	s, CHUNKSIZE_M1
	jnz	.do_final

	; either we're done, or we just did LIMIT
	sub	size, s

	; reduce
	vpslld	yb, 3   ; b is scaled by 8
	vpmulld	ysa, ya, [A_SCALE] ; scaled a

	; compute horizontal sums of ya, yb, ysa
	vextracti128 xtmp0, ya, 1
	vextracti128 xtmp1, yb, 1
	vextracti128 xtmp2, ysa, 1
	vpaddd	xa, xa, xtmp0
	vpaddd	xb, xb, xtmp1
	vpaddd	xsa, xsa, xtmp2
	vphaddd	xa, xa, xa
	vphaddd	xb, xb, xb
	vphaddd	xsa, xsa, xsa
	vphaddd	xa, xa, xa
	vphaddd	xb, xb, xb
	vphaddd	xsa, xsa, xsa

	vmovd	eax, xa
	xor	edx, edx
	mov	ecx, BASE
	div	ecx		; divide edx:eax by ecx, quot->eax, rem->edx
	mov	a_d, edx

	vpsubd	xb, xb, xsa
	vmovd	eax, xb
	add	eax, b_d
	xor	edx, edx
	mov	ecx, BASE
	div	ecx		; divide edx:eax by ecx, quot->eax, rem->edx
	mov	b_d, edx

	test	size, size
	jz	.finish

	; continue loop
	vmovd	xa, a_d
	vpxor	yb, yb
	jmp	.sloop1

.finish:
	mov	eax, b_d
	shl	eax, 16
	or	eax, a_d
	jmp	.end

.lt64:
	mov	a_d, init_d
	lea	end, [data + size]
	test	size, size
	jnz	.final_loop
	jmp	.zero_size

	; handle remaining 1...15 bytes
.do_final:
	; reduce
	vpslld	yb, 3   ; b is scaled by 8
	vpmulld	ysa, ya, [A_SCALE] ; scaled a

	vextracti128 xtmp0, ya, 1
	vextracti128 xtmp1, yb, 1
	vextracti128 xtmp2, ysa, 1
	vpaddd	xa, xa, xtmp0
	vpaddd	xb, xb, xtmp1
	vpaddd	xsa, xsa, xtmp2
	vphaddd	xa, xa, xa
	vphaddd	xb, xb, xb
	vphaddd	xsa, xsa, xsa
	vphaddd	xa, xa, xa
	vphaddd	xb, xb, xb
	vphaddd	xsa, xsa, xsa
	vpsubd	xb, xb, xsa

	vmovd	a_d, xa
	vmovd	eax, xb
	add	b_d, eax

align 32
.final_loop:
	movzx	eax, byte[data]
	add	a_d, eax
	inc	data
	add	b_d, a_d
	cmp	data, end
	jb	.final_loop

.zero_size:
	mov	eax, a_d
	xor	edx, edx
	mov	ecx, BASE
	div	ecx		; divide edx:eax by ecx, quot->eax, rem->edx
	mov	a_d, edx

	mov	eax, b_d
	xor	edx, edx
	mov	ecx, BASE
	div	ecx		; divide edx:eax by ecx, quot->eax, rem->edx
	shl	edx, 16
	or	edx, a_d
	mov	eax, edx

.end:
	FUNC_RESTORE
	ret

endproc_frame

section .data
align 32
A_SCALE:
	dq	0x0000000100000000, 0x0000000300000002
	dq	0x0000000500000004, 0x0000000700000006
SHUF0:
	dq	0xFFFFFF01FFFFFF00, 0xFFFFFF03FFFFFF02
	dq	0xFFFFFF05FFFFFF04, 0xFFFFFF07FFFFFF06
SHUF1:
	dq	0xFFFFFF09FFFFFF08, 0xFFFFFF0BFFFFFF0A
	dq	0xFFFFFF0DFFFFFF0C, 0xFFFFFF0FFFFFFF0E

