;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
; Copyright (C) 2022 Intel Corporation
;
; SPDX-License-Identifier: MIT
;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;

; uint32_t adler32_avx2(uint32_t init, const unsigned char *buf, uint64_t len)

%define LIMIT 5552
%define BASE  0xFFF1 ; 65521

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

 %define stack_size  5*8		; must be an odd multiple of 8
 %define func(x) proc_frame x
 %macro FUNC_SAVE 0
	alloc_stack	stack_size
	save_reg	rdi,  0*8
	save_reg	rsi,  1*8
	save_reg	r12,  2*8
	save_reg	r13,  3*8
	end_prolog
	mov	init_d, ecx	; initalize init_d from arg1 to keep ecx free
 %endmacro

 %macro FUNC_RESTORE 0
	mov	rdi,  [rsp + 0*8]
	mov	rsi,  [rsp + 1*8]
	mov	r12,  [rsp + 2*8]
	mov	r13,  [rsp + 3*8]
	add	rsp, stack_size
 %endmacro
%endif

%define xa	xmm0
%define xb	xmm1
%define xdata0	xmm2
%define xdata1	xmm3
%define xsa	xmm4

[bits 64]
default rel
section .text

mk_global adler32_sse, function
func(adler32_sse)
	FUNC_SAVE

	mov	data, arg2
	mov	size, arg3

	mov	b_d, init_d
	shr	b_d, 16
	and	init_d, 0xFFFF
	cmp	size, 32
	jb	.lt64
	movd	xa, init_d
	pxor	xb, xb
.sloop1:
	mov	s, LIMIT
	cmp	s, size
	cmova	s, size		; s = min(size, LIMIT)
	lea	end, [data + s - 7]
	cmp	data, end
	jae	.skip_loop_1a
align 32
.sloop1a:
	; do 8 adds
	pmovzxbd xdata0, [data]
	pmovzxbd xdata1, [data + 4]
	add	data, 8
	paddd	xa, xdata0
	paddd	xb, xa
	paddd	xa, xdata1
	paddd	xb, xa
	cmp	data, end
	jb	.sloop1a

.skip_loop_1a:
	add	end, 7

	test	s, 7
	jnz	.do_final

	; either we're done, or we just did LIMIT
	sub	size, s

	; reduce
	pslld	xb, 2   ; b is scaled by 4
	movdqa	xsa, xa ; scaled a
	pmulld	xsa, [A_SCALE]

	phaddd	xa, xa
	phaddd	xb, xb
	phaddd	xsa, xsa
	phaddd	xa, xa
	phaddd	xb, xb
	phaddd	xsa, xsa

	movd	eax, xa
	xor	edx, edx
	mov	ecx, BASE
	div	ecx		; divide edx:eax by ecx, quot->eax, rem->edx
	mov	a_d, edx

	psubd	xb, xsa
	movd	eax, xb
	add	eax, b_d
	xor	edx, edx
	mov	ecx, BASE
	div	ecx		; divide edx:eax by ecx, quot->eax, rem->edx
	mov	b_d, edx

	test	size, size
	jz	.finish

	; continue loop
	movd	xa, a_d
	pxor	xb, xb
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
	pslld	xb, 2   ; b is scaled by 4
	movdqa	xsa, xa ; scaled a
	pmulld	xsa, [A_SCALE]

	phaddd	xa, xa
	phaddd	xb, xb
	phaddd	xsa, xsa
	phaddd	xa, xa
	phaddd	xb, xb
	phaddd	xsa, xsa
	psubd	xb, xsa

	movd	a_d, xa
	movd	eax, xb
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
