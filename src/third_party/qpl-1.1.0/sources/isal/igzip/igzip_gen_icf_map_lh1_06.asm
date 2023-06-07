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
%define ARCH 06

%ifdef HAVE_AS_KNOWS_AVX512
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

%define datas zmm0
%define datas_lookup zmm1
%define zhashes zmm2
%define zdists zmm3
%define zdists_lookup zmm4
%define zscatter zmm5
%define zdists2 zmm6
%define zlens1 zmm7
%define zlens2 zmm8
%define zlookup zmm9
%define zlookup2 zmm10
%define match_lookups zmm11
%define zindex zmm12
%define zdist_extra zmm13
%define zdists_tmp zmm14
%define znull_dist_syms zmm15
%define zcode zmm16
%define zthirty zmm17
%define zdist_mask zmm18
%define zshortest_matches zmm19
%define zrot_left zmm20
%define zdatas_perm zmm21
%define zdatas_perm2 zmm22
%define zdatas_perm3 zmm23
%define zdatas_shuf zmm24
%define zhash_prod zmm25
%define zhash_mask zmm26
%define zincrement zmm27
%define zqword_shuf zmm28
%define zones zmm29
%define ztwofiftyfour zmm30
%define zbswap zmm31

%ifidn __OUTPUT_FORMAT__, win64
%define stack_size  10*16 + 6 * 8 + 8
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
%define func(x) x: endbranch
%macro FUNC_SAVE 0
	push	rbp
	push	r12
	push	r13
%endm

%macro FUNC_RESTORE 0
	pop	r13
	pop	r12
	pop	rbp
%endm
%endif

%define VECT_SIZE 16
%define HASH_BYTES 2

[bits 64]
default rel
section .text

global gen_icf_map_lh1_06
func(gen_icf_map_lh1_06)
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
	vpbroadcastd zdist_mask, dword [stream + _internal_state_dist_mask]
	vpbroadcastd zhash_mask, dword [stream + _internal_state_hash_mask]
	mov	tmp, stream
	mov	level_buf, [stream + _level_buf]
	sub	f_i_end, LA
	vmovdqu64 zdatas_perm, [datas_perm]
	vbroadcasti32x8 zdatas_shuf, [datas_shuf]
	vpbroadcastd zhash_prod, [hash_prod]
	vmovdqu64 zincrement, [increment]
	vmovdqu64 zqword_shuf, [qword_shuf]
	vbroadcasti64x2 zdatas_perm2, [datas_perm2]
	vbroadcasti64x2 zdatas_perm3, [datas_perm3]
	vpbroadcastd zones, [ones]
	vbroadcasti32x4 zbswap, [bswap_shuf]
	vpbroadcastd zthirty, [thirty]
	vmovdqu64 zrot_left, [drot_left]
	vpbroadcastd zshortest_matches, [shortest_matches]
	vpbroadcastd ztwofiftyfour, [twofiftyfour]
	vpbroadcastd znull_dist_syms, [null_dist_syms]
	kxorq	k0, k0, k0
	kmovq	k1, [k_mask_1]
	kmovq	k2, [k_mask_2]

;; Process first byte
	vmovd	zhashes %+ x, dword [f_i + file_start]
	vpmaddwd zhashes, zhashes, zhash_prod
	vpmaddwd zhashes, zhashes, zhash_prod
	vpandd	zhashes, zhashes, zhash_mask
	vmovd	hash %+ d, zhashes %+ x

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

	vmovd	tmp2 %+ d, zdist_mask %+ x
	and	tmp %+ d, tmp2 %+ d
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
	cmp	f_i, f_i_end
	jg	end_main

;;hash
	vmovdqu64 datas %+ y, [f_i + file_start]
	vpermq	zhashes, zdatas_perm, datas
	vpshufb	zhashes, zhashes, zdatas_shuf
	vpmaddwd zhashes, zhashes, zhash_prod
	vpmaddwd zhashes, zhashes, zhash_prod
	vpandd	zhashes, zhashes, zhash_mask

	vpermq	zlookup, zdatas_perm2, datas
	vpshufb	zlookup, zlookup, zqword_shuf
	vpermq	zlookup2, zdatas_perm3, datas
	vpshufb	zlookup2, zlookup2, zqword_shuf

;;gather/scatter hashes
	knotq	k6, k0
	vpgatherdd zdists_lookup {k6}, [hash_table + HASH_BYTES * zhashes]

	vpbroadcastd zindex, f_i %+ d
	vpaddd	zindex, zindex, zincrement
	vpblendmw zscatter {k1}, zindex, zdists_lookup

	knotq	k6, k0
	vpscatterdd [hash_table + HASH_BYTES * zhashes] {k6}, zscatter

;; Compute hash for next loop
	vmovdqu64 datas %+ y, [f_i + file_start + VECT_SIZE]
	vpermq	zhashes, zdatas_perm, datas
	vpshufb	zhashes, zhashes, zdatas_shuf
	vpmaddwd zhashes, zhashes, zhash_prod
	vpmaddwd zhashes, zhashes, zhash_prod
	vpandd	zhashes, zhashes, zhash_mask

	vmovdqu64 datas_lookup %+ y, [f_i + file_start + 2 * VECT_SIZE]

	sub	f_i_end, VECT_SIZE
	cmp	f_i, f_i_end
	jg	.loop1_end

.loop1:
	lea	next_in, [f_i + file_start]

;; Calculate look back dists
	vpaddd	zdists, zdists_lookup, zones
	vpsubd	zdists, zindex, zdists
	vpandd	zdists, zdists, zdist_mask
	vpaddd	zdists, zdists, zones
	vpsubd	zdists, zincrement, zdists

;;gather/scatter hashes
	add	f_i, VECT_SIZE

	kxnorq	k6, k6, k6
	kxnorq	k7, k7, k7
	vpgatherdd zdists_lookup {k6}, [hash_table + HASH_BYTES * zhashes]

	vpbroadcastd zindex, f_i %+ d
	vpaddd	zindex, zindex, zincrement
	vpblendmw zscatter {k1}, zindex, zdists_lookup

	vpscatterdd [hash_table + HASH_BYTES * zhashes] {k7}, zscatter

;; Compute hash for next loop
	vpermq	zhashes, zdatas_perm, datas_lookup
	vpshufb	zhashes, zhashes, zdatas_shuf
	vpmaddwd zhashes, zhashes, zhash_prod
	vpmaddwd zhashes, zhashes, zhash_prod
	vpandd	zhashes, zhashes, zhash_mask

;;lookup old codes
	vextracti32x8 zdists2 %+ y, zdists, 1
	kxnorq	k6, k6, k6
	kxnorq	k7, k7, k7
	vpgatherdq zlens1 {k6}, [next_in + zdists %+ y]
	vpgatherdq zlens2 {k7}, [next_in + zdists2 %+ y]

;; Calculate dist_icf_code
	vpaddd	zdists, zdists, zones
	vpsubd	zdists, zincrement, zdists
	vpcmpgtd k5, zdists, zones
	vplzcntd zdist_extra, zdists
	vpsubd	zdist_extra {k5}{z}, zthirty, zdist_extra
	vpsllvd	zcode, zones, zdist_extra
	vpsubd	zcode, zcode, zones
	vpandd	zcode {k5}{z}, zdists, zcode
	vpsrlvd	zdists, zdists, zdist_extra
	vpslld	zdist_extra, zdist_extra, 1
	vpaddd	zdists, zdists, zdist_extra
	vpslld	zcode, zcode, EXTRA_BITS_OFFSET - DIST_OFFSET
	vpaddd	zdists, zdists, zcode

;; Setup zdists for combining with zlens
	vpslld	zdists, zdists, DIST_OFFSET

;; xor current data with lookback dist
	vpxorq	zlens1, zlens1, zlookup
	vpxorq	zlens2, zlens2, zlookup2

;; Setup registers for next loop
	vpermq	zlookup, zdatas_perm2, datas
	vpshufb	zlookup, zlookup, zqword_shuf
	vpermq	zlookup2, zdatas_perm3, datas
	vpshufb	zlookup2, zlookup2, zqword_shuf

;; Compute match length
	vpshufb	zlens1, zlens1, zbswap
	vpshufb	zlens2, zlens2, zbswap
	vplzcntq zlens1, zlens1
	vplzcntq zlens2, zlens2
	vpmovqd	zlens1 %+ y, zlens1
	vpmovqd	zlens2 %+ y, zlens2
	vinserti32x8 zlens1, zlens2 %+ y, 1
	vpsrld	zlens1, zlens1, 3

;; Preload for next loops
	vmovdqu64 datas, datas_lookup
	vmovdqu64 datas_lookup %+ y, [f_i + file_start + 2 * VECT_SIZE]

;; Zero out matches which should not be taken
	kshiftrw k3, k1, 15
	vpermd	zlens2, zrot_left, zlens1
	vpermd	zdists, zrot_left, zdists

	vmovd	zdists_tmp %+ x, prev_len %+ d
	vmovd	prev_len %+ d, zlens2 %+ x
	vmovdqu32 zlens2 {k3}, zdists_tmp

	vmovd	zdists_tmp %+ x, prev_dist %+ d
	vmovd	prev_dist %+ d, zdists %+ x
	vmovdqu32 zdists {k3}, zdists_tmp

	vpcmpgtd k3, zlens2, zshortest_matches
	vpcmpgtd k4, zlens1, zlens2

	knotq	k3, k3
	korq	k3, k3, k4
	knotq	k4, k3
	vmovdqu32 zlens1 {k4}{z}, zlens2

;; Update zdists to match zlens1
	vpaddd	zdists, zdists, zlens1
	vpaddd	zdists, zdists, ztwofiftyfour
	vpmovzxbd zdists {k3}, [f_i + file_start - VECT_SIZE - 1]
	vpaddd	zdists {k3}, zdists, znull_dist_syms

;;Store zdists
	vmovdqu64 [matches_next], zdists
	add	matches_next, ICF_CODE_BYTES * VECT_SIZE

	cmp	f_i, f_i_end
	jle	.loop1

.loop1_end:
	lea	next_in, [f_i + file_start]

;; Calculate look back dists
	vpaddd	zdists, zdists_lookup, zones
	vpsubd	zdists, zindex, zdists
	vpandd	zdists, zdists, zdist_mask
	vpaddd	zdists, zdists, zones
	vpsubd	zdists, zincrement, zdists

;;lookup old codes
	vextracti32x8 zdists2 %+ y, zdists, 1
	kxnorq	k6, k6, k6
	kxnorq	k7, k7, k7
	vpgatherdq zlens1 {k6}, [next_in + zdists %+ y]
	vpgatherdq zlens2 {k7}, [next_in + zdists2 %+ y]

;; Restore last update hash value
	vextracti32x4 zdists2 %+ x, zdists, 3
	vpextrd tmp %+ d, zdists2 %+ x, 3
	add	tmp %+ d, f_i %+ d

	vmovd	zhashes %+ x, dword [f_i + file_start + VECT_SIZE - 1]
	vpmaddwd zhashes %+ x, zhashes %+ x, zhash_prod %+ x
	vpmaddwd zhashes %+ x, zhashes %+ x, zhash_prod %+ x
	vpandd	zhashes %+ x, zhashes %+ x, zhash_mask %+ x
	vmovd	hash %+ d, zhashes %+ x

	mov	word [hash_table + HASH_BYTES * hash], tmp %+ w

;; Calculate dist_icf_code
	vpaddd	zdists, zdists, zones
	vpsubd	zdists, zincrement, zdists
	vpcmpgtd k5, zdists, zones
	vplzcntd zdist_extra, zdists
	vpsubd	zdist_extra {k5}{z}, zthirty, zdist_extra
	vpsllvd	zcode, zones, zdist_extra
	vpsubd	zcode, zcode, zones
	vpandd	zcode {k5}{z}, zdists, zcode
	vpsrlvd	zdists, zdists, zdist_extra
	vpslld	zdist_extra, zdist_extra, 1
	vpaddd	zdists, zdists, zdist_extra
	vpslld	zcode, zcode, EXTRA_BITS_OFFSET - DIST_OFFSET
	vpaddd	zdists, zdists, zcode

;; Setup zdists for combining with zlens
	vpslld	zdists, zdists, DIST_OFFSET

;; xor current data with lookback dist
	vpxorq	zlens1, zlens1, zlookup
	vpxorq	zlens2, zlens2, zlookup2

;; Compute match length
	vpshufb	zlens1, zlens1, zbswap
	vpshufb	zlens2, zlens2, zbswap
	vplzcntq zlens1, zlens1
	vplzcntq zlens2, zlens2
	vpmovqd	zlens1 %+ y, zlens1
	vpmovqd	zlens2 %+ y, zlens2
	vinserti32x8 zlens1, zlens2 %+ y, 1
	vpsrld	zlens1, zlens1, 3

;; Zero out matches which should not be taken
	kshiftrw k3, k1, 15
	vpermd	zlens2, zrot_left, zlens1
	vpermd	zdists, zrot_left, zdists

	vmovd	zdists_tmp %+ x, prev_len %+ d
	vmovd	prev_len %+ d, zlens2 %+ x
	vmovdqu32 zlens2 {k3}, zdists_tmp

	vmovd	zdists_tmp %+ x, prev_dist %+ d
	vmovd	prev_dist %+ d, zdists %+ x
	vmovdqu32 zdists {k3}, zdists_tmp

	vpcmpgtd k3, zlens2, zshortest_matches
	vpcmpgtd k4, zlens1, zlens2

	knotq	k3, k3
	korq	k3, k3, k4
	knotq	k4, k3
	vmovdqu32 zlens1 {k4}{z}, zlens2

;; Update zdists to match zlens1
	vpaddd	zdists, zdists, zlens1
	vpaddd	zdists, zdists, ztwofiftyfour
	vpmovzxbd zdists {k3}, [f_i + file_start - 1]
	vpaddd	zdists {k3}, zdists, znull_dist_syms

;;Store zdists
	vmovdqu64 [matches_next], zdists
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
align 64
;; 64 byte data
datas_perm:
	dq 0x0, 0x1, 0x0, 0x1, 0x1, 0x2, 0x1, 0x2
drot_left:
	dd 0xf, 0x0, 0x1, 0x2, 0x3, 0x4, 0x5, 0x6
	dd 0x7, 0x8, 0x9, 0xa, 0xb, 0xc, 0xd, 0xe
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
datas_shuf:
	db 0x0, 0x1, 0x2, 0x3
	db 0x1, 0x2, 0x3, 0x4
	db 0x2, 0x3, 0x4, 0x5
	db 0x3, 0x4, 0x5, 0x6
	db 0x4, 0x5, 0x6, 0x7
	db 0x5, 0x6, 0x7, 0x8
	db 0x6, 0x7, 0x8, 0x9
	db 0x7, 0x8, 0x9, 0xa
increment:
	dd 0x0, 0x1, 0x2, 0x3, 0x4, 0x5, 0x6, 0x7
	dd 0x8, 0x9, 0xa, 0xb, 0xc, 0xd, 0xe, 0xf

;; 16 byte data
datas_perm2:
	dq 0x0, 0x1
datas_perm3:
	dq 0x1, 0x2
bswap_shuf:
	db 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01, 0x00
	db 0x0f, 0x0e, 0x0d, 0x0c, 0x0b, 0x0a, 0x09, 0x08
;; 8 byte data
k_mask_1:
	dq 0xaaaaaaaaaaaaaaaa
k_mask_2:
	dq 0x7fff
;; 4 byte data
null_dist_syms:
	dd LIT
%define PROD1 0xE84B
%define PROD2 0x97B1
hash_prod:
	dw PROD1, PROD2
ones:
	dd 0x1
thirty:
	dd 0x1e
twofiftyfour:
	dd 0xfe
lit_len_mask:
	dd LIT_LEN_MASK
shortest_matches:
	dd MIN_DEF_MATCH
%endif
