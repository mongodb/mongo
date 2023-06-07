;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
; Copyright (C) 2022 Intel Corporation
;
; SPDX-License-Identifier: MIT
;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;

%include "reg_sizes.asm"
%include "lz0a_const.asm"
%include "data_struct2.asm"
%include "huffman.asm"


%define USE_HSWNI
%define ARCH 04

%ifidn __OUTPUT_FORMAT__, win64
%define arg1 rcx
%define arg2 rdx
%define arg3 r8
%define hash rsi
%define next_in rdi
%else
%define arg1 rdi
%define arg2 rsi
%define arg3 rdx
%define hash r8
%define next_in rcx
%endif

%define stream arg1
%define level_buf arg1
%define matches_next arg2
%define f_i_end arg3

%define f_i rax
%define file_start rbp
%define tmp r9
%define tmp2 r10
%define prev_len r11
%define prev_dist r12
%define f_i_orig r13

%define hash_table level_buf + _hash_map_hash_table

%define datas ymm0
%define datas_lookup ymm1
%define yhashes ymm2
%define ydists ymm3
%define ydists_lookup ymm4

%define ydownconvert_qd ymm5
%define ydists2 ymm5
%define yscatter ymm5
%define ytmp2 ymm5
%define ynull_syms ymm5

%define ylens1 ymm6
%define ylens2 ymm7
%define ylookup ymm8
%define ylookup2 ymm9
%define yindex ymm10

%define yrot_left ymm11
%define yshift_finish ymm11
%define yqword_shuf ymm11
%define yhash_prod ymm11
%define ycode ymm11
%define ytmp3 ymm11

%define yones ymm12
%define ydatas_perm2 ymm13
%define yincrement ymm14

%define ytmp ymm15
%define ydist_extra ymm15
%define yhash_mask ymm15
%define ydist_mask ymm15

%ifidn __OUTPUT_FORMAT__, win64
%define stack_size  10*16 + 6 * 8 + 3 * 8
%define local_storage_offset (stack_size - 16)
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
	vmovdqu	[rsp + 8*16], xmm14
	vmovdqa	[rsp + 9*16], xmm15
	save_reg	rsi, 10*16 + 0*8
	save_reg	rdi, 10*16 + 1*8
	save_reg	rbp, 10*16 + 2*8
	save_reg	r12, 10*16 + 3*8
	save_reg	r13, 10*16 + 4*8
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
	vmovdqa	xmm14, [rsp + 8*16]
	vmovdqa	xmm15, [rsp + 9*16]

	mov	rsi, [rsp + 10*16 + 0*8]
	mov	rdi, [rsp + 10*16 + 1*8]
	mov	rbp, [rsp + 10*16 + 2*8]
	mov	r12, [rsp + 10*16 + 3*8]
	mov	r13, [rsp + 10*16 + 4*8]
	add	rsp, stack_size
%endm
%else
%define stack_size  16
%define local_storage_offset 0

%define func(x) x: endbranch
%macro FUNC_SAVE 0
	push	rbp
	push	r12
	push	r13
	sub	rsp, stack_size
%endm

%macro FUNC_RESTORE 0
	add	rsp, stack_size
	pop	r13
	pop	r12
	pop	rbp
%endm
%endif

%define dist_mask_offset local_storage_offset
%define hash_mask_offset local_storage_offset + 8

%define VECT_SIZE 8
%define HASH_BYTES 2

[bits 64]
default rel
section .text

global gen_icf_map_lh1_04
func(gen_icf_map_lh1_04)
	endbranch
	FUNC_SAVE

	mov	file_start, [stream + _next_in]
	mov	f_i %+ d, dword [stream + _total_in]
	mov	f_i_orig, f_i

	sub	file_start, f_i
	add	f_i_end, f_i
	cmp	f_i, f_i_end
	jge	end_main

;; Prep for main loop
	mov	tmp %+ d, dword [stream + _internal_state_dist_mask]
	mov	[rsp + dist_mask_offset], tmp
	mov	tmp %+ d, dword [stream + _internal_state_hash_mask]
	mov	[rsp + hash_mask_offset], tmp
	mov	tmp, stream
	mov	level_buf, [stream + _level_buf]
	sub	f_i_end, LA
	vmovdqu yincrement, [increment]
	vpbroadcastd yones, [ones]
	vmovdqu ydatas_perm2, [datas_perm2]

;; Process first byte
	vpbroadcastd	yhash_prod, [hash_prod]
	vpbroadcastd	yhash_mask, [rsp + hash_mask_offset]
	vmovd	yhashes %+ x, dword [f_i + file_start]
	vpmaddwd yhashes, yhashes, yhash_prod
	vpmaddwd yhashes, yhashes, yhash_prod
	vpand	yhashes, yhashes, yhash_mask
	vmovd	hash %+ d, yhashes %+ x
	cmp	byte [tmp + _internal_state_has_hist], IGZIP_NO_HIST
	jne	.has_hist
	;; No history, the byte is a literal
	xor	prev_len, prev_len
	xor	prev_dist, prev_dist
	mov	byte [tmp + _internal_state_has_hist], IGZIP_HIST
	jmp .byte_processed

.has_hist:
	;; History exists, need to set prev_len and prev_dist accordingly
	lea	next_in, [f_i + file_start]

	;; Determine match lookback distance
	xor	tmp, tmp
	mov	tmp %+ w, f_i %+ w
	dec	tmp
	sub	tmp %+ w, word [hash_table + HASH_BYTES * hash]

	and	tmp %+ d, [rsp + dist_mask_offset]
	neg	tmp

	;; Check first 8 bytes of match
	mov	prev_len, [next_in]
	xor	prev_len, [next_in + tmp - 1]
	neg	tmp

	;; Set prev_dist
%ifidn arg1, rcx
	mov	tmp2, rcx
%endif
	;; The third register is unused on Haswell and later,
	;; This line will not work on previous architectures
	get_dist_icf_code tmp, prev_dist, tmp

%ifidn arg1, rcx
	mov	rcx, tmp2
%endif

	;; Set prev_len
	xor	tmp2, tmp2
	tzcnt	prev_len, prev_len
	shr	prev_len, 3
	cmp	prev_len, MIN_DEF_MATCH
	cmovl	prev_len, tmp2

.byte_processed:
	mov	word [hash_table + HASH_BYTES * hash], f_i %+ w

	add	f_i, 1

;;hash
	vmovdqu datas, [f_i + file_start]
	vpermq	yhashes, datas, 0x44
	vpshufb	yhashes, yhashes, [datas_shuf]
	vpmaddwd yhashes, yhashes, yhash_prod
	vpmaddwd yhashes, yhashes, yhash_prod
	vpand	yhashes, yhashes, yhash_mask

	vpermq	ylookup, datas, 0x44
	vmovdqu	yqword_shuf, [qword_shuf]
	vpshufb	ylookup, ylookup, yqword_shuf
	vpermd	ylookup2, ydatas_perm2, datas
	vpshufb	ylookup2, ylookup2, yqword_shuf

;;gather/scatter hashes
	vpcmpeqq ytmp, ytmp, ytmp
	vpgatherdd ydists_lookup, [hash_table + HASH_BYTES * yhashes], ytmp

	vpbroadcastd ytmp2, [upper_word]
	vpbroadcastd ytmp, [low_word]
	vmovd	yindex %+ x, f_i %+ d
	vpbroadcastd yindex, yindex %+ x
	vpaddd	yindex, yindex, yincrement
	vpand	yscatter, ydists_lookup, ytmp2
 	vpand ytmp, yindex, ytmp
	vpor	yscatter, yscatter, ytmp

	vmovd tmp %+ d, yhashes %+ x
	vmovd [hash_table + HASH_BYTES * tmp], yscatter %+ x
	vpextrd tmp %+ d, yhashes %+ x, 1
	vpextrd [hash_table + HASH_BYTES * tmp], yscatter %+ x, 1
	vpextrd tmp %+ d, yhashes %+ x, 2
	vpextrd [hash_table + HASH_BYTES * tmp], yscatter %+ x, 2
	vpextrd tmp %+ d,yhashes %+ x, 3
	vpextrd [hash_table + HASH_BYTES * tmp], yscatter %+ x, 3

	vextracti128 yscatter %+ x, yscatter, 1
	vextracti128 yhashes %+ x, yhashes, 1

	vmovd tmp %+ d, yhashes %+ x
	vmovd [hash_table + HASH_BYTES * tmp], yscatter %+ x
	vpextrd tmp %+ d, yhashes %+ x, 1
	vpextrd [hash_table + HASH_BYTES * tmp], yscatter %+ x, 1
	vpextrd tmp %+ d, yhashes %+ x, 2
	vpextrd [hash_table + HASH_BYTES * tmp], yscatter %+ x, 2
	vpextrd tmp %+ d,yhashes %+ x, 3
	vpextrd [hash_table + HASH_BYTES * tmp], yscatter %+ x, 3

;; Compute hash for next loop
	vpbroadcastd	yhash_prod, [hash_prod]
	vpbroadcastd	yhash_mask, [rsp + hash_mask_offset]
	vmovdqu datas, [f_i + file_start + VECT_SIZE]
	vpermq	yhashes, datas, 0x44
	vpshufb	yhashes, yhashes, [datas_shuf]
	vpmaddwd yhashes, yhashes, yhash_prod
	vpmaddwd yhashes, yhashes, yhash_prod
	vpand	yhashes, yhashes, yhash_mask

	vmovdqu datas_lookup, [f_i + file_start + 2 * VECT_SIZE]

	sub	f_i_end, VECT_SIZE
	cmp	f_i, f_i_end
	jg	.loop1_end

.loop1:
	lea	next_in, [f_i + file_start]

;; Calculate look back dists
	vpbroadcastd ydist_mask, [rsp + dist_mask_offset]
	vpaddd	ydists, ydists_lookup, yones
	vpsubd	ydists, yindex, ydists
	vpand	ydists, ydists, ydist_mask
	vpaddd	ydists, ydists, yones
	vpsubd	ydists, yincrement, ydists

;;gather/scatter hashes
	add	f_i, VECT_SIZE

	vpcmpeqq ytmp, ytmp, ytmp
	vpgatherdd ydists_lookup, [hash_table + HASH_BYTES * yhashes], ytmp

	vpbroadcastd ytmp2, [upper_word]
	vpbroadcastd ytmp, [low_word]
	vmovd	yindex %+ x, f_i %+ d
	vpbroadcastd yindex, yindex %+ x
	vpaddd	yindex, yindex, yincrement
	vpand	yscatter, ydists_lookup, ytmp2
	vpand ytmp, yindex, ytmp
	vpor	yscatter, yscatter, ytmp

	vmovd tmp %+ d, yhashes %+ x
	vmovd [hash_table + HASH_BYTES * tmp], yscatter %+ x
	vpextrd tmp %+ d, yhashes %+ x, 1
	vpextrd [hash_table + HASH_BYTES * tmp], yscatter %+ x, 1
	vpextrd tmp %+ d, yhashes %+ x, 2
	vpextrd [hash_table + HASH_BYTES * tmp], yscatter %+ x, 2
	vpextrd tmp %+ d,yhashes %+ x, 3
	vpextrd [hash_table + HASH_BYTES * tmp], yscatter %+ x, 3

	vextracti128 yscatter %+ x, yscatter, 1
	vextracti128 yhashes %+ x, yhashes, 1

	vmovd tmp %+ d, yhashes %+ x
	vmovd [hash_table + HASH_BYTES * tmp], yscatter %+ x
	vpextrd tmp %+ d, yhashes %+ x, 1
	vpextrd [hash_table + HASH_BYTES * tmp], yscatter %+ x, 1
	vpextrd tmp %+ d, yhashes %+ x, 2
	vpextrd [hash_table + HASH_BYTES * tmp], yscatter %+ x, 2
	vpextrd tmp %+ d,yhashes %+ x, 3
	vpextrd [hash_table + HASH_BYTES * tmp], yscatter %+ x, 3

;; Compute hash for next loop
	vpbroadcastd	yhash_prod, [hash_prod]
	vpbroadcastd	yhash_mask, [rsp + hash_mask_offset]
	vpermq	yhashes, datas_lookup, 0x44
	vpshufb	yhashes, yhashes, [datas_shuf]
	vpmaddwd yhashes, yhashes, yhash_prod
	vpmaddwd yhashes, yhashes, yhash_prod
	vpand	yhashes, yhashes, yhash_mask

;;lookup old codes
	vextracti128 ydists2 %+ x, ydists, 1

	vpcmpeqq ytmp, ytmp, ytmp
	vpgatherdq ylens1, [next_in + ydists %+ x], ytmp
	vpcmpeqq ytmp, ytmp, ytmp
	vpgatherdq ylens2, [next_in + ydists2 %+ x], ytmp

;; Calculate dist_icf_code
	vpaddd	ydists, ydists, yones
	vpsubd	ydists, yincrement, ydists

	vpbroadcastd ytmp2, [low_nibble]
	vbroadcasti128 ytmp3, [nibble_order]
	vpslld	ydist_extra, ydists, 12
	vpor	ydist_extra, ydists, ydist_extra
	vpand	ydist_extra, ydist_extra, ytmp2
	vpshufb ydist_extra, ydist_extra, ytmp3
	vbroadcasti128 ytmp2, [bit_index]
	vpshufb ydist_extra, ytmp2, ydist_extra
	vpxor	ytmp2, ytmp2, ytmp2
	vpcmpgtb ytmp2, ydist_extra, ytmp2
	vpsrld	ytmp3, ytmp2, 8
	vpandn	ytmp2, ytmp3, ytmp2
	vpsrld	ytmp3, ytmp2, 16
	vpandn	ytmp2, ytmp3, ytmp2
	vpsrld	ytmp3, ytmp2, 24
	vpandn	ytmp2, ytmp3, ytmp2
	vpbroadcastd ytmp3, [base_offset]
	vpaddb	ydist_extra, ytmp3
	vpand	ydist_extra, ydist_extra, ytmp2
	vpsrlq	ytmp2, ydist_extra, 32
	vpxor	ytmp3, ytmp3, ytmp3
	vpsadbw	ydist_extra, ydist_extra, ytmp3
	vpsadbw	ytmp2, ytmp2, ytmp3
	vpsubd	ydist_extra, ydist_extra, ytmp2
	vpsllq	ytmp2, ytmp2, 32
	vpor	ydist_extra, ydist_extra, ytmp2
	vpcmpgtb ytmp3, ydist_extra, ytmp3
	vpand ydist_extra, ydist_extra, ytmp3

	vpsllvd	ycode, yones, ydist_extra
	vpsubd	ycode, ycode, yones
	vpcmpgtd ytmp2, ydists, yones
	vpand	ycode, ydists, ycode
	vpand	ycode, ycode, ytmp2
	vpsrlvd	ydists, ydists, ydist_extra
	vpslld	ydist_extra, ydist_extra, 1
	vpaddd	ydists, ydists, ydist_extra
	vpslld	ycode, ycode, EXTRA_BITS_OFFSET - DIST_OFFSET
	vpaddd	ydists, ydists, ycode

;; Setup ydists for combining with ylens
	vpslld	ydists, ydists, DIST_OFFSET

;; xor current data with lookback dist
	vpxor	ylens1, ylens1, ylookup
	vpxor	ylens2, ylens2, ylookup2

;; Setup registers for next loop
	vpermq	ylookup, datas, 0x44
	vmovdqu	yqword_shuf, [qword_shuf]
	vpshufb	ylookup, ylookup, yqword_shuf
	vpermd	ylookup2, ydatas_perm2, datas
	vpshufb	ylookup2, ylookup2, yqword_shuf

;; Compute match length
	vpxor	ytmp, ytmp, ytmp
	vpcmpeqb ylens1, ylens1, ytmp
	vpcmpeqb ylens2, ylens2, ytmp
	vpbroadcastq yshift_finish, [shift_finish]
	vpand	ylens1, ylens1, yshift_finish
	vpand	ylens2, ylens2, yshift_finish
	vpsadbw ylens1, ylens1, ytmp
	vpsadbw ylens2, ylens2, ytmp
	vmovdqu ydownconvert_qd, [downconvert_qd]
	vpshufb	ylens1, ylens1, ydownconvert_qd
	vextracti128 ytmp %+ x, ylens1, 1
	vpor	ylens1, ylens1, ytmp
	vpshufb	ylens2, ylens2, ydownconvert_qd
	vextracti128 ytmp %+ x, ylens2, 1
	vpor	ylens2, ylens2, ytmp
	vinserti128 ylens1, ylens1, ylens2 %+ x, 1
	vpbroadcastd ytmp, [low_nibble]
	vpsrld	ylens2, ylens1, 4
	vpand	ylens1, ylens1, ytmp
	vbroadcasti128	ytmp, [match_cnt_perm]
	vpbroadcastd ytmp2, [match_cnt_low_max]
	vpshufb	ylens1, ytmp, ylens1
	vpshufb	ylens2, ytmp, ylens2
	vpcmpeqb ytmp, ylens1, ytmp2
	vpand	ylens2, ylens2, ytmp
	vpaddd	ylens1, ylens1, ylens2

;; Preload for next loops
	vmovdqu datas, datas_lookup
	vmovdqu datas_lookup, [f_i + file_start + 2 * VECT_SIZE]

;; Zero out matches which should not be taken
	vmovdqu yrot_left, [drot_left]
	vpermd	ylens2, yrot_left, ylens1
	vpermd	ydists, yrot_left, ydists

	vpinsrd ytmp %+ x, ylens2 %+ x, prev_len %+ d, 0
	vmovd	prev_len %+ d, ylens2 %+ x
	vinserti128 ylens2, ylens2, ytmp %+ x, 0

	vpinsrd ytmp %+ x, ydists %+ x, prev_dist %+ d, 0
	vmovd	prev_dist %+ d, ydists %+ x
	vinserti128 ydists, ydists, ytmp %+ x, 0

	vpbroadcastd ytmp, [shortest_matches]
	vpcmpgtd ytmp, ylens2, ytmp
	vpcmpgtd ytmp2, ylens1, ylens2

	vpcmpeqd ytmp3, ytmp3, ytmp3
	vpxor	ytmp, ytmp, ytmp3
	vpor	ytmp, ytmp, ytmp2

	vpandn	ylens1, ytmp, ylens2

;; Update zdists to match ylens1
	vpbroadcastd ytmp2, [twofiftyfour]
	vpaddd	ydists, ydists, ylens1
	vpaddd	ydists, ydists, ytmp2

	vpbroadcastd ynull_syms, [null_dist_syms]
	vpmovzxbd ytmp3, [f_i + file_start - VECT_SIZE - 1]
	vpaddd	ytmp3, ynull_syms
	vpand	ytmp3, ytmp3, ytmp
	vpandn	ydists, ytmp, ydists
	vpor	ydists, ydists, ytmp3

;;Store ydists
	vmovdqu [matches_next], ydists
	add	matches_next, ICF_CODE_BYTES * VECT_SIZE

	cmp	f_i, f_i_end
	jle	.loop1

.loop1_end:
	lea	next_in, [f_i + file_start]

;; Calculate look back dists
	vpbroadcastd ydist_mask, [rsp + dist_mask_offset]
	vpaddd	ydists, ydists_lookup, yones
	vpsubd	ydists, yindex, ydists
	vpand	ydists, ydists, ydist_mask
	vpaddd	ydists, ydists, yones
	vpsubd	ydists, yincrement, ydists

;;lookup old codes
	vextracti128 ydists2 %+ x, ydists, 1
	vpcmpeqq ytmp, ytmp, ytmp
	vpgatherdq ylens1, [next_in + ydists %+ x], ytmp
	vpcmpeqq ytmp, ytmp, ytmp
	vpgatherdq ylens2, [next_in + ydists2 %+ x], ytmp

;; Restore last update hash value
	vpextrd	tmp %+ d, ydists2 %+ x, 3
	add	tmp %+ d, f_i %+ d

	vpbroadcastd	yhash_prod %+ x, [hash_prod]
	vpbroadcastd	yhash_mask %+ x, [rsp + hash_mask_offset]

	vmovd	yhashes %+ x, dword [f_i + file_start + VECT_SIZE - 1]
	vpmaddwd yhashes %+ x, yhashes %+ x, yhash_prod %+ x
	vpmaddwd yhashes %+ x, yhashes %+ x, yhash_prod %+ x
	vpand	yhashes %+ x, yhashes %+ x, yhash_mask %+ x
	vmovd	hash %+ d, yhashes %+ x

	mov	word [hash_table + HASH_BYTES * hash], tmp %+ w

;; Calculate dist_icf_code
	vpaddd	ydists, ydists, yones
	vpsubd	ydists, yincrement, ydists

	vpbroadcastd ytmp2, [low_nibble]
	vbroadcasti128 ytmp3, [nibble_order]
	vpslld	ydist_extra, ydists, 12
	vpor	ydist_extra, ydists, ydist_extra
	vpand	ydist_extra, ydist_extra, ytmp2
	vpshufb ydist_extra, ydist_extra, ytmp3
	vbroadcasti128 ytmp2, [bit_index]
	vpshufb ydist_extra, ytmp2, ydist_extra
	vpxor	ytmp2, ytmp2, ytmp2
	vpcmpgtb ytmp2, ydist_extra, ytmp2
	vpsrld	ytmp3, ytmp2, 8
	vpandn	ytmp2, ytmp3, ytmp2
	vpsrld	ytmp3, ytmp2, 16
	vpandn	ytmp2, ytmp3, ytmp2
	vpsrld	ytmp3, ytmp2, 24
	vpandn	ytmp2, ytmp3, ytmp2
	vpbroadcastd ytmp3, [base_offset]
	vpaddb	ydist_extra, ytmp3
	vpand	ydist_extra, ydist_extra, ytmp2
	vpsrlq	ytmp2, ydist_extra, 32
	vpxor	ytmp3, ytmp3, ytmp3
	vpsadbw	ydist_extra, ydist_extra, ytmp3
	vpsadbw	ytmp2, ytmp2, ytmp3
	vpsubd	ydist_extra, ydist_extra, ytmp2
	vpsllq	ytmp2, ytmp2, 32
	vpor	ydist_extra, ydist_extra, ytmp2
	vpcmpgtb ytmp3, ydist_extra, ytmp3
	vpand ydist_extra, ydist_extra, ytmp3

	vpsllvd	ycode, yones, ydist_extra
	vpsubd	ycode, ycode, yones
	vpcmpgtd ytmp2, ydists, yones
	vpand	ycode, ydists, ycode
	vpand	ycode, ycode, ytmp2
	vpsrlvd	ydists, ydists, ydist_extra
	vpslld	ydist_extra, ydist_extra, 1
	vpaddd	ydists, ydists, ydist_extra
	vpslld	ycode, ycode, EXTRA_BITS_OFFSET - DIST_OFFSET
	vpaddd	ydists, ydists, ycode

;; Setup ydists for combining with ylens
	vpslld	ydists, ydists, DIST_OFFSET

;; xor current data with lookback dist
	vpxor	ylens1, ylens1, ylookup
	vpxor	ylens2, ylens2, ylookup2

;; Compute match length
	vpxor	ytmp, ytmp, ytmp
	vpcmpeqb ylens1, ylens1, ytmp
	vpcmpeqb ylens2, ylens2, ytmp
	vpbroadcastq yshift_finish, [shift_finish]
	vpand	ylens1, ylens1, yshift_finish
	vpand	ylens2, ylens2, yshift_finish
	vpsadbw ylens1, ylens1, ytmp
	vpsadbw ylens2, ylens2, ytmp
	vmovdqu ydownconvert_qd, [downconvert_qd]
	vpshufb	ylens1, ylens1, ydownconvert_qd
	vextracti128 ytmp %+ x, ylens1, 1
	vpor	ylens1, ylens1, ytmp
	vpshufb	ylens2, ylens2, ydownconvert_qd
	vextracti128 ytmp %+ x, ylens2, 1
	vpor	ylens2, ylens2, ytmp
	vinserti128 ylens1, ylens1, ylens2 %+ x, 1
	vpbroadcastd ytmp, [low_nibble]
	vpsrld	ylens2, ylens1, 4
	vpand	ylens1, ylens1, ytmp
	vbroadcasti128	ytmp, [match_cnt_perm]
	vpbroadcastd ytmp2, [match_cnt_low_max]
	vpshufb	ylens1, ytmp, ylens1
	vpshufb	ylens2, ytmp, ylens2
	vpcmpeqb ytmp, ylens1, ytmp2
	vpand	ylens2, ylens2, ytmp
	vpaddd	ylens1, ylens1, ylens2

;; Zero out matches which should not be taken
	vmovdqu yrot_left, [drot_left]
	vpermd	ylens2, yrot_left, ylens1
	vpermd	ydists, yrot_left, ydists

	vpinsrd ytmp %+ x, ylens2 %+ x, prev_len %+ d, 0
	vinserti128 ylens2, ylens2, ytmp %+ x, 0

	vpinsrd ytmp %+ x, ydists %+ x, prev_dist %+ d, 0
	vinserti128 ydists, ydists, ytmp %+ x, 0

	vpbroadcastd ytmp, [shortest_matches]
	vpcmpgtd ytmp, ylens2, ytmp
	vpcmpgtd ytmp2, ylens1, ylens2

	vpcmpeqd ytmp3, ytmp3, ytmp3
	vpxor	ytmp, ytmp, ytmp3
	vpor	ytmp, ytmp, ytmp2

	vpandn	ylens1, ytmp, ylens2

;; Update zdists to match ylens1
	vpbroadcastd ytmp2, [twofiftyfour]
	vpaddd	ydists, ydists, ylens1
	vpaddd	ydists, ydists, ytmp2

	vpbroadcastd ynull_syms, [null_dist_syms]
	vpmovzxbd ytmp3, [f_i + file_start - 1]
	vpaddd	ytmp3, ynull_syms
	vpand	ytmp3, ytmp3, ytmp
	vpandn	ydists, ytmp, ydists
	vpor	ydists, ydists, ytmp3

;;Store ydists
	vmovdqu [matches_next], ydists
	add	f_i, VECT_SIZE

end_main:
	sub	f_i, f_i_orig
	sub	f_i, 1

%ifnidn f_i, rax
	mov	rax, f_i
%endif
	FUNC_RESTORE
	ret

endproc_frame

section .data
align 32
;; 32 byte data
datas_perm2:
	dd 0x1, 0x2, 0x3, 0x4, 0x1, 0x2, 0x3, 0x4
drot_left:
	dd 0x7, 0x0, 0x1, 0x2, 0x3, 0x4, 0x5, 0x6
datas_shuf:
	db 0x0, 0x1, 0x2, 0x3
	db 0x1, 0x2, 0x3, 0x4
	db 0x2, 0x3, 0x4, 0x5
	db 0x3, 0x4, 0x5, 0x6
	db 0x4, 0x5, 0x6, 0x7
	db 0x5, 0x6, 0x7, 0x8
	db 0x6, 0x7, 0x8, 0x9
	db 0x7, 0x8, 0x9, 0xa
qword_shuf:
	db 0x0, 0x1, 0x2, 0x3, 0x4, 0x5, 0x6, 0x7
	db 0x1, 0x2, 0x3, 0x4, 0x5, 0x6, 0x7, 0x8
	db 0x2, 0x3, 0x4, 0x5, 0x6, 0x7, 0x8, 0x9
	db 0x3, 0x4, 0x5, 0x6, 0x7, 0x8, 0x9, 0xa
increment:
	dd 0x0, 0x1, 0x2, 0x3, 0x4, 0x5, 0x6, 0x7
downconvert_qd:
	db 0x00, 0xff, 0xff, 0xff, 0x08, 0xff, 0xff, 0xff
	db 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff
	db 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff
	db 0x00, 0xff, 0xff, 0xff, 0x08, 0xff, 0xff, 0xff

;; 16 byte data
match_cnt_perm:
	db 0x0, 0x1, 0x0, 0x2, 0x0, 0x1, 0x0, 0x3, 0x0, 0x1, 0x0, 0x2, 0x0, 0x1, 0x0, 0x4
bit_index:
	db 0x0, 0x1, 0x2, 0x2, 0x3, 0x3, 0x3, 0x3
	db 0x4, 0x4, 0x4, 0x4, 0x4, 0x4, 0x4, 0x4
nibble_order:
	db 0x0, 0x2, 0x1, 0x3, 0x4, 0x6, 0x5, 0x7
	db 0x8, 0xa, 0x9, 0xb, 0xc, 0xe, 0xd, 0xf

;; 8 byte data
shift_finish:
	db 0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80

;; 4 byte data
ones:
	dd 0x1
%define PROD1 0xE84B
%define PROD2 0x97B1
hash_prod:
	dw PROD1, PROD2
null_dist_syms:
	dd LIT
twofiftyfour:
	dd 0xfe
shortest_matches:
	dd MIN_DEF_MATCH
upper_word:
	dw 0x0000, 0xffff
low_word:
	dw 0xffff, 0x0000
low_nibble:
	db 0x0f, 0x0f, 0x0f, 0x0f
match_cnt_low_max:
	dd 0x4
base_offset:
	db -0x2, 0x2, 0x6, 0xa
