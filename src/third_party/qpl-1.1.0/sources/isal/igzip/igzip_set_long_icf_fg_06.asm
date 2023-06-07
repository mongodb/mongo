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

%ifdef HAVE_AS_KNOWS_AVX512
%ifidn __OUTPUT_FORMAT__, win64
%define arg1 rcx
%define arg2 rdx
%define arg3 r8
%define arg4 r9
%define len rdi
%define dist rsi
%else
%define arg1 rdi
%define arg2 rsi
%define arg3 rdx
%define arg4 rcx
%define len r8
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
%define tmp2 r13

%define zmatch_lookup zmm0
%define zmatch_lookup2 zmm1
%define zlens zmm2
%define zdist_codes zmm3
%define zdist_extras zmm4
%define zdists zmm5
%define zdists2 zmm6
%define zlens1 zmm7
%define zlens2 zmm8
%define zlookup zmm9
%define zlookup2 zmm10
%define datas zmm11
%define ztmp1 zmm12
%define ztmp2 zmm13
%define zvect_size zmm16
%define zmax_len zmm17
%define ztwofiftyfour zmm18
%define ztwofiftysix zmm19
%define ztwosixtytwo zmm20
%define znlen_mask zmm21
%define zbswap zmm22
%define zqword_shuf zmm23
%define zdatas_perm3 zmm24
%define zdatas_perm2 zmm25
%define zincrement zmm26
%define zdists_mask zmm27
%define zdists_start zmm28
%define zlong_lens2 zmm29
%define zlong_lens zmm30
%define zlens_mask zmm31

%ifidn __OUTPUT_FORMAT__, win64
%define stack_size  8*16 + 4 * 8 + 8
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
	save_reg	rsi, 8*16 + 0*8
	save_reg	rdi, 8*16 + 1*8
	save_reg	r12, 8*16 + 2*8
	save_reg	r13, 8*16 + 3*8
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

	mov	rsi, [rsp + 8*16 + 0*8]
	mov	rdi, [rsp + 8*16 + 1*8]
	mov	r12, [rsp + 8*16 + 2*8]
	mov	r13, [rsp + 8*16 + 3*8]
	add	rsp, stack_size
%endm
%else
%define func(x) x: endbranch
%macro FUNC_SAVE 0
	push	r12
	push	r13
%endm

%macro FUNC_RESTORE 0
	pop	r13
	pop	r12
%endm
%endif
%define VECT_SIZE 16

[bits 64]
default rel
section .text

global set_long_icf_fg_06
func(set_long_icf_fg_06)
	endbranch
	FUNC_SAVE

	lea	end_in, [next_in + arg3]
	add	end_processed, next_in
	mov	end_processed_orig, end_processed
	lea	tmp1, [end_processed + LA_STATELESS]
	cmp	end_in, tmp1
	cmovg	end_in, tmp1
	sub	end_processed, 15
	vpbroadcastd zlong_lens, [long_len]
	vpbroadcastd zlong_lens2, [long_len2]
	vpbroadcastd zlens_mask, [len_mask]
	vmovdqu16 zdists_start, [dist_start]
	vpbroadcastd zdists_mask, [dists_mask]
	vmovdqu32 zincrement, [increment]
	vbroadcasti64x2 zdatas_perm2, [datas_perm2]
	vbroadcasti64x2 zdatas_perm3, [datas_perm3]
	vmovdqu64 zqword_shuf, [qword_shuf]
	vbroadcasti64x2 zbswap, [bswap_shuf]
	vpbroadcastd znlen_mask, [nlen_mask]
	vpbroadcastd zvect_size, [vect_size]
	vpbroadcastd zmax_len, [max_len]
	vpbroadcastd ztwofiftyfour, [twofiftyfour]
	vpbroadcastd ztwofiftysix, [twofiftysix]
	vpbroadcastd ztwosixtytwo, [twosixtytwo]
	vmovdqu32 zmatch_lookup, [match_lookup]

.fill_loop: ; Tahiti is a magical place
	vmovdqu32 zmatch_lookup2, zmatch_lookup
	vmovdqu32 zmatch_lookup, [match_lookup + ICF_CODE_BYTES * VECT_SIZE]

	cmp	next_in, end_processed
	jae	.end_fill

.finish_entry:
	vpandd	zlens, zmatch_lookup2, zlens_mask
	vpcmpgtd k3, zlens, zlong_lens

;; Speculatively increment
	add	next_in, VECT_SIZE
	add	match_lookup, ICF_CODE_BYTES * VECT_SIZE

	ktestw	k3, k3
	jz	.fill_loop

	vpsrld	zdist_codes, zmatch_lookup2, DIST_OFFSET
	vpmovdw	zdists %+ y, zdist_codes ; Relies on perm working mod 32
	vpermw	zdists, zdists, zdists_start
	vpmovzxwd zdists, zdists %+ y

	vpsrld	zdist_extras, zmatch_lookup2, EXTRA_BITS_OFFSET
	vpsubd	zdist_extras, zincrement, zdist_extras

	vpsubd	zdists, zdist_extras, zdists
	vextracti32x8 zdists2 %+ y, zdists, 1
	kmovb	k6, k3
	kshiftrw k7, k3, 8
	vpgatherdq zlens1 {k6}, [next_in + zdists %+ y - 8]
	vpgatherdq zlens2 {k7}, [next_in + zdists2 %+ y - 8]

	vmovdqu8 datas %+ y, [next_in - 8]
	vpermq	zlookup, zdatas_perm2, datas
	vpshufb	zlookup, zlookup, zqword_shuf
	vpermq	zlookup2, zdatas_perm3, datas
	vpshufb	zlookup2, zlookup2, zqword_shuf

	vpxorq	zlens1, zlens1, zlookup
	vpxorq	zlens2, zlens2, zlookup2

	vpshufb	zlens1, zlens1, zbswap
	vpshufb	zlens2, zlens2, zbswap
	vplzcntq zlens1, zlens1
	vplzcntq zlens2, zlens2
	vpmovqd	zlens1 %+ y, zlens1
	vpmovqd	zlens2 %+ y, zlens2
	vinserti32x8 zlens1, zlens2 %+ y, 1
	vpsrld	zlens1 {k3}{z}, zlens1, 3

	vpandd	zmatch_lookup2 {k3}{z}, zmatch_lookup2, znlen_mask
	vpaddd	zmatch_lookup2 {k3}{z}, zmatch_lookup2, ztwosixtytwo
	vpaddd	zmatch_lookup2 {k3}{z}, zmatch_lookup2, zlens1

	vmovdqu32 [match_lookup - ICF_CODE_BYTES * VECT_SIZE] {k3}, zmatch_lookup2

	vpcmpgtd k3, zlens1, zlong_lens2
	ktestw	k3, k3
	jz	.fill_loop

	vpsubd	zdists, zincrement, zdists

	vpcompressd zdists2 {k3}, zdists
	vpcompressd zmatch_lookup2 {k3}, zmatch_lookup2
	kmovq	match_offset, k3
	tzcnt	match_offset, match_offset

	vmovd	dist %+ d, zdists2 %+ x
	lea	next_in, [next_in + match_offset - VECT_SIZE]
	lea	match_lookup, [match_lookup + ICF_CODE_BYTES * (match_offset - VECT_SIZE)]
	mov	match_in, next_in
	sub	match_in, dist

	mov	len, 16
	mov	tmp2, end_in
	sub	tmp2, next_in

	compare_z next_in, match_in, len, tmp2, tmp1, k3, ztmp1, ztmp2

	vpbroadcastd zlens1, len %+ d
	vpsubd	zlens1, zlens1, zincrement
	vpaddd	zlens1, zlens1, ztwofiftyfour

	mov	tmp2, end_processed
	sub	tmp2, next_in
	cmp	len, tmp2
	cmovg	len, tmp2

	add	next_in, len
	lea	match_lookup, [match_lookup + ICF_CODE_BYTES * len]
	vmovdqu32 zmatch_lookup, [match_lookup]

	vpbroadcastd zmatch_lookup2, zmatch_lookup2 %+ x
	vpandd	zmatch_lookup2, zmatch_lookup2, znlen_mask

	neg	len

.update_match_lookup:
	vpandd	zlens2, zlens_mask, [match_lookup + ICF_CODE_BYTES * len]
	vpcmpgtd k3, zlens1, zlens2
	vpcmpgtd k4, zlens1, ztwofiftysix
	kandw	k3, k3, k4

	vpcmpgtd k4, zlens1, zmax_len
	vmovdqu32 zlens, zlens1
	vmovdqu32 zlens {k4}, zmax_len

	vpaddd	zlens2 {k3}{z}, zlens, zmatch_lookup2

	vmovdqu32 [match_lookup + ICF_CODE_BYTES * len] {k3}, zlens2

	knotw	k3, k3
	ktestw	k3, k3
	jnz	.fill_loop

	add	len, VECT_SIZE
	vpsubd	zlens1, zlens1, zvect_size

	jmp	.update_match_lookup

.end_fill:
	mov	end_processed, end_processed_orig
	cmp	next_in, end_processed
	jge	.finish

	mov	tmp1, end_processed
	sub	tmp1, next_in
	vpbroadcastd ztmp1, tmp1 %+ d
	vpcmpd k3, ztmp1, zincrement, 6
	vmovdqu32 zmatch_lookup2 {k3}{z}, zmatch_lookup2
	jmp	.finish_entry

.finish:

	FUNC_RESTORE
	ret

endproc_frame

section .data
align 64
;; 64 byte data
dist_start:
	dw 0x0001, 0x0002, 0x0003, 0x0004, 0x0005, 0x0007, 0x0009, 0x000d
	dw 0x0011, 0x0019, 0x0021, 0x0031, 0x0041, 0x0061, 0x0081, 0x00c1
	dw 0x0101, 0x0181, 0x0201, 0x0301, 0x0401, 0x0601, 0x0801, 0x0c01
	dw 0x1001, 0x1801, 0x2001, 0x3001, 0x4001, 0x6001, 0x0000, 0x0000
qword_shuf:
	db 0x0, 0x1, 0x2, 0x3, 0x4, 0x5, 0x6, 0x7
	db 0x1, 0x2, 0x3, 0x4, 0x5, 0x6, 0x7, 0x8
	db 0x2, 0x3, 0x4, 0x5, 0x6, 0x7, 0x8, 0x9
	db 0x3, 0x4, 0x5, 0x6, 0x7, 0x8, 0x9, 0xa
	db 0x4, 0x5, 0x6, 0x7, 0x8, 0x9, 0xa, 0xb
	db 0x5, 0x6, 0x7, 0x8, 0x9, 0xa, 0xb, 0xc
	db 0x6, 0x7, 0x8, 0x9, 0xa, 0xb, 0xc, 0xd
	db 0x7, 0x8, 0x9, 0xa, 0xb, 0xc, 0xd, 0xe
	db 0x8, 0x9, 0xa, 0xb, 0xc, 0xd, 0xe, 0xf

;; 16 byte data
increment:
	dd 0x0, 0x1, 0x2, 0x3, 0x4, 0x5, 0x6, 0x7
	dd 0x8, 0x9, 0xa, 0xb, 0xc, 0xd, 0xe, 0xf

datas_perm2:
	dq 0x0, 0x1
datas_perm3:
	dq 0x1, 0x2
bswap_shuf:
	db 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01, 0x00
	db 0x0f, 0x0e, 0x0d, 0x0c, 0x0b, 0x0a, 0x09, 0x08

;; 4 byte data
len_mask:
	dd LIT_LEN_MASK
dists_mask:
	dd LIT_DIST_MASK
long_len:
	dd 0x105
long_len2:
	dd 0x7
max_len:
	dd 0xfe + 0x102
vect_size:
	dd VECT_SIZE
twofiftyfour:
	dd 0xfe
twofiftysix:
	dd 0x100
twosixtytwo:
	dd 0x106
nlen_mask:
	dd 0xfffffc00
%endif
