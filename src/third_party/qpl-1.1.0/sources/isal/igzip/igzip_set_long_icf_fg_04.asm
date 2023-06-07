;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
; Copyright (C) 2022 Intel Corporation
;
; SPDX-License-Identifier: MIT
;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;

%include "reg_sizes.asm"
%include "lz0a_const.asm"
%include "data_struct2.asm"
%include "igzip_compare_types.asm"
%define NEQ 4

default rel

%ifidn __OUTPUT_FORMAT__, win64
%define arg1 rcx
%define arg2 rdx
%define arg3 r8
%define arg4 r9
%define len rdi
%define tmp2 rdi
%define dist rsi
%else
%define arg1 rdi
%define arg2 rsi
%define arg3 rdx
%define arg4 rcx
%define len r8
%define tmp2 r8
%define dist r9
%endif

%define next_in arg1
%define end_processed arg2
%define end_in arg3
%define match_lookup arg4
%define match_in rax
%define match_offset r10
%define tmp1 r11
%define end_processed_orig r12
%define dist_code r13
%define tmp3 r13

%define ymatch_lookup ymm0
%define ymatch_lookup2 ymm1
%define ylens ymm2
%define ycmp2 ymm3
%define ylens1 ymm4
%define ylens2 ymm5
%define ycmp ymm6
%define ytmp1 ymm7
%define ytmp2 ymm8
%define yvect_size ymm9
%define ymax_len ymm10
%define ytwofiftysix ymm11
%define ynlen_mask ymm12
%define ydists_mask ymm13
%define ylong_lens ymm14
%define ylens_mask ymm15

%ifidn __OUTPUT_FORMAT__, win64
%define stack_size  10*16 + 4 * 8 + 8
%define func(x) proc_frame x
%macro FUNC_SAVE 0
	alloc_stack	stack_size
	vmovdqa	[rsp + 0*16], xmm6
	vmovdqa	[rsp + 1*16], xmm7
	vmovdqa	[rsp + 2*16], xmm8
	vmovdqa	[rsp + 3*16], xmm9
	vmovdqa	[rsp + 4*16], xmm10
	vmovdqa	[rsp + 5*16], xmm11
	vmovdqa	[rsp + 6*16], xmm12
	vmovdqa	[rsp + 7*16], xmm13
	vmovdqa [rsp + 8*16], xmm14
	vmovdqa [rsp + 9*16], xmm15
	save_reg	rsi, 10*16 + 0*8
	save_reg	rdi, 10*16 + 1*8
	save_reg	r12, 10*16 + 2*8
	save_reg	r13, 10*16 + 3*8
	end_prolog
%endm

%macro FUNC_RESTORE 0
	vmovdqa	xmm6, [rsp + 0*16]
	vmovdqa	xmm7, [rsp + 1*16]
	vmovdqa	xmm8, [rsp + 2*16]
	vmovdqa	xmm9, [rsp + 3*16]
	vmovdqa	xmm10, [rsp + 4*16]
	vmovdqa	xmm11, [rsp + 5*16]
	vmovdqa	xmm12, [rsp + 6*16]
	vmovdqa	xmm13, [rsp + 7*16]
	vmovdqa xmm14, [rsp + 8*16]
	vmovdqa xmm15, [rsp + 9*16]

	mov	rsi, [rsp + 10*16 + 0*8]
	mov	rdi, [rsp + 10*16 + 1*8]
	mov	r12, [rsp + 10*16 + 2*8]
	mov	r13, [rsp + 10*16 + 3*8]
	add	rsp, stack_size
%endm
%else
%define func(x) x: endbranch
%macro FUNC_SAVE 0
	push r12
	push r13
%endm

%macro FUNC_RESTORE 0
	pop r13
	pop r12
%endm
%endif
%define VECT_SIZE 8

[bits 64]
default rel
section .text

global set_long_icf_fg_04
func(set_long_icf_fg_04)
	endbranch
	FUNC_SAVE

	lea	end_in, [next_in + arg3]
	add	end_processed, next_in
	mov	end_processed_orig, end_processed
	lea	tmp1, [end_processed + LA_STATELESS]
	cmp	end_in, tmp1
	cmovg	end_in, tmp1
	sub	end_processed, VECT_SIZE - 1
	vmovdqu ylong_lens, [long_len]
	vmovdqu ylens_mask, [len_mask]
	vmovdqu ydists_mask, [dists_mask]
	vmovdqu ynlen_mask, [nlen_mask]
	vmovdqu yvect_size, [vect_size]
	vmovdqu ymax_len, [max_len]
	vmovdqu ytwofiftysix, [twofiftysix]
	vmovdqu ymatch_lookup, [match_lookup]

.fill_loop: ; Tahiti is a magical place
	vmovdqu ymatch_lookup2, ymatch_lookup
	vmovdqu ymatch_lookup, [match_lookup + ICF_CODE_BYTES * VECT_SIZE]

	cmp	next_in, end_processed
	jae	.end_fill

.finish_entry:
	vpand	ylens, ymatch_lookup2, ylens_mask
	vpcmpgtd ycmp, ylens, ylong_lens
	vpmovmskb tmp1, ycmp

;; Speculatively increment
	add	next_in, VECT_SIZE
	add	match_lookup, ICF_CODE_BYTES * VECT_SIZE

	test	tmp1, tmp1
	jz	.fill_loop

	tzcnt	match_offset, tmp1
	shr	match_offset, 2

	lea	next_in, [next_in + match_offset - VECT_SIZE]
	lea	match_lookup, [match_lookup + ICF_CODE_BYTES * (match_offset - VECT_SIZE)]
	mov	dist %+ d, [match_lookup]
	vmovd	ymatch_lookup2 %+ x, dist %+ d

	mov	tmp1, dist
	shr	dist, DIST_OFFSET
	and	dist, LIT_DIST_MASK
	shr	tmp1, EXTRA_BITS_OFFSET
	lea	tmp2, [dist_start]
	mov	dist %+ w, [tmp2 +  2 * dist]
	add	dist, tmp1

	mov	match_in, next_in
	sub	match_in, dist

	mov	len, 8
	mov	tmp3, end_in
	sub	tmp3, next_in

	compare_y next_in, match_in, len, tmp3, tmp1, ytmp1, ytmp2

	vmovd	ylens1 %+ x, len %+ d
	vpbroadcastd ylens1, ylens1 %+ x
	vpsubd	ylens1, ylens1, [increment]
	vpaddd	ylens1, ylens1, [twofiftyfour]

	mov	tmp3, end_processed
	sub	tmp3, next_in
	cmp	len, tmp3
	cmovg	len, tmp3

	add	next_in, len
	lea	match_lookup, [match_lookup + ICF_CODE_BYTES * len]
	vmovdqu ymatch_lookup, [match_lookup]

	vpbroadcastd ymatch_lookup2, ymatch_lookup2 %+ x
	vpand	ymatch_lookup2, ymatch_lookup2, ynlen_mask

	neg	len

.update_match_lookup:
	vpand	ylens2, ylens_mask, [match_lookup + ICF_CODE_BYTES * len]

	vpcmpgtd ycmp, ylens1, ylens2
	vpcmpgtd ytmp1, ylens1, ytwofiftysix
	vpand	ycmp, ycmp, ytmp1
	vpmovmskb tmp1, ycmp

	vpcmpgtd ycmp2, ylens1, ymax_len
	vpandn ylens, ycmp2, ylens1
	vpand ycmp2, ymax_len, ycmp2
	vpor ylens, ycmp2

	vpaddd	ylens2, ylens, ymatch_lookup2
	vpand	ylens2, ylens2, ycmp

	vpmaskmovd [match_lookup + ICF_CODE_BYTES * len], ycmp, ylens2

	test	tmp1 %+ d, tmp1 %+ d
	jz	.fill_loop

	add	len, VECT_SIZE
	vpsubd	ylens1, ylens1, yvect_size

	jmp	.update_match_lookup

.end_fill:
	mov	end_processed, end_processed_orig
	cmp	next_in, end_processed
	jge	.finish

	mov	tmp1, end_processed
	sub	tmp1, next_in
	vmovd	ytmp1 %+ x, tmp1 %+ d
	vpbroadcastd ytmp1, ytmp1 %+ x
	vpcmpgtd ytmp1, ytmp1, [increment]
	vpand	ymatch_lookup2, ymatch_lookup2, ytmp1
	jmp	.finish_entry

.finish:
	FUNC_RESTORE
	ret

endproc_frame

section .data
align 64
dist_start:
	dw 0x0001, 0x0002, 0x0003, 0x0004, 0x0005, 0x0007, 0x0009, 0x000d
	dw 0x0011, 0x0019, 0x0021, 0x0031, 0x0041, 0x0061, 0x0081, 0x00c1
	dw 0x0101, 0x0181, 0x0201, 0x0301, 0x0401, 0x0601, 0x0801, 0x0c01
	dw 0x1001, 0x1801, 0x2001, 0x3001, 0x4001, 0x6001, 0x0000, 0x0000
len_mask:
	dd LIT_LEN_MASK, LIT_LEN_MASK, LIT_LEN_MASK, LIT_LEN_MASK
	dd LIT_LEN_MASK, LIT_LEN_MASK, LIT_LEN_MASK, LIT_LEN_MASK
dists_mask:
	dd LIT_DIST_MASK, LIT_DIST_MASK, LIT_DIST_MASK, LIT_DIST_MASK
	dd LIT_DIST_MASK, LIT_DIST_MASK, LIT_DIST_MASK, LIT_DIST_MASK
long_len:
	dd 0x105, 0x105, 0x105, 0x105, 0x105, 0x105, 0x105, 0x105
increment:
	dd 0x0, 0x1, 0x2, 0x3, 0x4, 0x5, 0x6, 0x7
vect_size:
	dd VECT_SIZE, VECT_SIZE, VECT_SIZE, VECT_SIZE
	dd VECT_SIZE, VECT_SIZE, VECT_SIZE, VECT_SIZE
twofiftyfour:
	dd 0xfe, 0xfe, 0xfe, 0xfe, 0xfe, 0xfe, 0xfe, 0xfe
twofiftysix:
	dd 0x100, 0x100, 0x100, 0x100, 0x100, 0x100, 0x100, 0x100
nlen_mask:
	dd 0xfffffc00, 0xfffffc00, 0xfffffc00, 0xfffffc00
	dd 0xfffffc00, 0xfffffc00, 0xfffffc00, 0xfffffc00
max_len:
	dd 0xfe + 0x102, 0xfe + 0x102, 0xfe + 0x102, 0xfe + 0x102
	dd 0xfe + 0x102, 0xfe + 0x102, 0xfe + 0x102, 0xfe + 0x102
