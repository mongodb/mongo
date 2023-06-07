;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
; Copyright (C) 2022 Intel Corporation
;
; SPDX-License-Identifier: MIT
;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;

%include "reg_sizes.asm"
%include "lz0a_const.asm"
%include "data_struct2.asm"
%include "stdmac.asm"

%ifdef HAVE_AS_KNOWS_AVX512

%define ARCH 06
%define USE_HSWNI

; tree entry is 4 bytes:
; lit/len tree (513 entries)
; |  3  |  2   |  1 | 0 |
; | len |       code    |
;
; dist tree
; |  3  |  2   |  1 | 0 |
; |eblen:codlen|   code |

; token format:
; DIST_OFFSET:0 : lit/len
; 31:(DIST_OFFSET + 5) : dist Extra Bits
; (DIST_OFFSET + 5):DIST_OFFSET : dist code
; lit/len: 0-256 (literal)
;          257-512 (dist + 254)

; returns final token pointer
; equal to token_end if successful
;    uint32_t* encode_df(uint32_t *token_start, uint32_t *token_end,
;                            BitBuf *out_buf, uint32_t *trees);

%ifidn __OUTPUT_FORMAT__, win64
%define arg1 rcx
%define arg2 rdx
%define arg3 r8
%define arg4 r9
%define sym		rsi
%define dsym		rdi
%define hufftables	r9
%define ptr		r11
%else
; Linux
%define arg1 rdi
%define arg2 rsi
%define arg3 rdx
%define arg4 rcx
%define sym		r9
%define dsym		r8
%define hufftables	r11
%define ptr		rdi
%endif

%define in_buf_end	arg2
%define bitbuf		arg3
%define out_buf		bitbuf
; bit_count is rcx
%define bits		rax
%define data		r12
%define tmp		rbx
%define len 		dsym
%define tmp2 		r10
%define end_ptr		rbp

%define LIT_MASK	((0x1 << LIT_LEN_BIT_COUNT) - 1)
%define DIST_MASK	((0x1 << DIST_LIT_BIT_COUNT) - 1)

%define codes1		zmm1
%define code_lens1	zmm2
%define codes2		zmm3
%define code_lens2	zmm4
%define codes3		zmm5
%define ztmp		zmm5
%define	code_lens3	zmm6
%define codes4		zmm7
%define syms		zmm7

%define code_lens4	zmm8
%define dsyms		zmm8
%define zbits_count_q	zmm8

%define codes_lookup1	zmm9
%define	codes_lookup2	zmm10
%define datas		zmm11
%define zbits		zmm12
%define zbits_count	zmm13
%define zoffset_mask	zmm14
%define znotoffset_mask	zmm23

%define zq_64		zmm15
%define zlit_mask	zmm16
%define zdist_mask	zmm17
%define zlit_icr_mask	zmm18
%define zeb_icr_mask	zmm19
%define zmax_write	zmm20
%define zrot_perm	zmm21
%define zq_8		zmm22

%define VECTOR_SIZE 0x40
%define VECTOR_LOOP_PROCESSED (2 * VECTOR_SIZE)
%define VECTOR_SLOP 0x40 - 8

gpr_save_mem_offset	equ	0
gpr_save_mem_size	equ	8 * 6
xmm_save_mem_offset	equ	gpr_save_mem_offset + gpr_save_mem_size
xmm_save_mem_size	equ	10 * 16
bitbuf_mem_offset	equ	xmm_save_mem_offset + xmm_save_mem_size
bitbuf_mem_size		equ	8
stack_size		equ	gpr_save_mem_size + xmm_save_mem_size + bitbuf_mem_size


%macro FUNC_SAVE 0
	sub	rsp, stack_size
	mov	[rsp + gpr_save_mem_offset + 0*8], rbx
	mov	[rsp + gpr_save_mem_offset + 1*8], rbp
	mov	[rsp + gpr_save_mem_offset + 2*8], r12

%ifidn __OUTPUT_FORMAT__, win64
	mov	[rsp + gpr_save_mem_offset + 3*8], rsi
	mov	[rsp + gpr_save_mem_offset + 4*8], rdi

	MOVDQU	[rsp + xmm_save_mem_offset + 0*8], xmm6
	MOVDQU	[rsp + xmm_save_mem_offset + 1*8], xmm7
	MOVDQU	[rsp + xmm_save_mem_offset + 2*8], xmm8
	MOVDQU	[rsp + xmm_save_mem_offset + 3*8], xmm9
	MOVDQU	[rsp + xmm_save_mem_offset + 4*8], xmm10
	MOVDQU	[rsp + xmm_save_mem_offset + 5*8], xmm11
	MOVDQU	[rsp + xmm_save_mem_offset + 6*8], xmm12
	MOVDQU	[rsp + xmm_save_mem_offset + 7*8], xmm13
	MOVDQU	[rsp + xmm_save_mem_offset + 8*8], xmm14
	MOVDQU	[rsp + xmm_save_mem_offset + 9*8], xmm15
%endif

%endm

%macro FUNC_RESTORE 0
	mov	rbx, [rsp + gpr_save_mem_offset + 0*8]
	mov	rbp, [rsp + gpr_save_mem_offset + 1*8]
	mov	r12, [rsp + gpr_save_mem_offset + 2*8]

%ifidn __OUTPUT_FORMAT__, win64
	mov	rsi, [rsp + gpr_save_mem_offset + 3*8]
	mov	rdi, [rsp + gpr_save_mem_offset + 4*8]

	MOVDQU	xmm6, [rsp + xmm_save_mem_offset + 0*8]
	MOVDQU	xmm7, [rsp + xmm_save_mem_offset + 1*8]
	MOVDQU	xmm8, [rsp + xmm_save_mem_offset + 2*8]
	MOVDQU	xmm9, [rsp + xmm_save_mem_offset + 3*8]
	MOVDQU	xmm10, [rsp + xmm_save_mem_offset + 4*8]
	MOVDQU	xmm11, [rsp + xmm_save_mem_offset + 5*8]
	MOVDQU	xmm12, [rsp + xmm_save_mem_offset + 6*8]
	MOVDQU	xmm13, [rsp + xmm_save_mem_offset + 7*8]
	MOVDQU	xmm14, [rsp + xmm_save_mem_offset + 8*8]
	MOVDQU	xmm15, [rsp + xmm_save_mem_offset + 9*8]
%endif
	add	rsp, stack_size

%endmacro

default rel
section .text

global encode_deflate_icf_ %+ ARCH
encode_deflate_icf_ %+ ARCH:
	endbranch
	FUNC_SAVE

%ifnidn ptr, arg1
	mov	ptr, arg1
%endif
%ifnidn hufftables, arg4
	mov	hufftables, arg4
%endif

	mov	[rsp + bitbuf_mem_offset], bitbuf
	mov	bits, [bitbuf + _m_bits]
	mov	ecx, [bitbuf + _m_bit_count]
	mov	end_ptr, [bitbuf + _m_out_end]
	mov	out_buf, [bitbuf + _m_out_buf]	; clobbers bitbuf

	sub	end_ptr, VECTOR_SLOP
	sub	in_buf_end, VECTOR_LOOP_PROCESSED
	cmp	ptr, in_buf_end
	jge	.finish

	kxorq	k0, k0, k0
	kmovq	k1, [k_mask_1]
	kmovq	k2, [k_mask_2]
	kmovq	k3, [k_mask_3]
	kmovq	k4, [k_mask_4]
	kmovq	k5, [k_mask_5]

	vmovdqa64 zrot_perm, [rot_perm]

	vbroadcasti64x2 zq_64, [q_64]
	vbroadcasti64x2 zq_8, [q_8]

	vpbroadcastq zoffset_mask, [offset_mask]
	vpternlogd znotoffset_mask, znotoffset_mask, zoffset_mask, 0x55

	vpbroadcastd zlit_mask, [lit_mask]
	vpbroadcastd zdist_mask, [dist_mask]
	vpbroadcastd zlit_icr_mask, [lit_icr_mask]
	vpbroadcastd zeb_icr_mask, [eb_icr_mask]
	vpbroadcastd zmax_write, [max_write_d]

	knotq	k6, k0
	vmovdqu64	datas, [ptr]
	vpandd	syms, datas, zlit_mask
	vpgatherdd codes_lookup1 {k6}, [hufftables + _lit_len_table + 4 * syms]

	knotq	k7, k0
	vpsrld	dsyms, datas, DIST_OFFSET
	vpandd	dsyms, dsyms, zdist_mask
	vpgatherdd codes_lookup2 {k7}, [hufftables + _dist_table + 4 * dsyms]

	vmovq	zbits %+ x, bits
	vmovq	zbits_count %+ x, rcx

.main_loop:
	;;  Sets codes1 to contain lit/len codes andcode_lens1 the corresponding lengths
	vpsrld	code_lens1, codes_lookup1, 24
	vpandd	codes1, codes_lookup1, zlit_icr_mask

	;; Sets codes2 to contain dist codes, code_lens2 the corresponding lengths,
	;; and code_lens3 the extra bit counts
	vmovdqu16	codes2 {k1}{z}, codes_lookup2 ;Bits 8 and above of zbits are 0
	vpsrld	code_lens2, codes_lookup2, 24
	vpsrld	code_lens3, codes_lookup2, 16
	vpandd	code_lens3, code_lens3, zeb_icr_mask

	;; Set codes3 to contain the extra bits
	vpsrld	codes3, datas, EXTRA_BITS_OFFSET

	cmp	out_buf, end_ptr
	ja	.main_loop_exit

	;; Start code lookups for next iteration
	knotq	k6, k0
	add	ptr, VECTOR_SIZE
	vmovdqu64	datas, [ptr]
	vpandd	syms, datas, zlit_mask
	vpgatherdd codes_lookup1 {k6}, [hufftables + _lit_len_table + 4 * syms]

	knotq	k7, k0
	vpsrld	dsyms, datas, DIST_OFFSET
	vpandd	dsyms, dsyms, zdist_mask
	vpgatherdd codes_lookup2 {k7}, [hufftables + _dist_table + 4 * dsyms]

	;; Merge dist code with extra bits
	vpsllvd	codes3, codes3, code_lens2
	vpxord	codes2, codes2, codes3
	vpaddd	code_lens2, code_lens2, code_lens3

	;; Check for long codes
	vpaddd	code_lens3, code_lens1, code_lens2
	vpcmpgtd	k6, code_lens3, zmax_write
	ktestd	k6, k6
	jnz	.long_codes

	;; Merge dist and len codes
	vpsllvd	codes2, codes2, code_lens1
	vpxord	codes1, codes1, codes2

	vmovdqa32 codes3 {k1}{z}, codes1
	vpsrlq	codes1, codes1, 32
	vpsrlq	code_lens1, code_lens3, 32
	vmovdqa32	code_lens3 {k1}{z}, code_lens3

	;; Merge bitbuf bits
	vpsllvq codes3, codes3, zbits_count
	vpxord	codes3, codes3, zbits
	vpaddq	code_lens3, code_lens3, zbits_count

	;; Merge two symbols into qwords
	vpsllvq	codes1, codes1, code_lens3
	vpxord codes1, codes1, codes3
	vpaddq code_lens1, code_lens1, code_lens3

	;; Determine total bits at end of each qword
	vpermq	zbits_count {k5}{z}, zrot_perm, code_lens1
	vpaddq	code_lens2, zbits_count, code_lens1
	vshufi64x2 zbits_count {k3}{z}, code_lens2, code_lens2, 0x90
	vpaddq	code_lens2, code_lens2, zbits_count
	vshufi64x2 zbits_count {k2}{z}, code_lens2, code_lens2, 0x40
	vpaddq	code_lens2, code_lens2, zbits_count

	;; Bit align quadwords
	vpandd	zbits_count, code_lens2, zoffset_mask
	vpermq	zbits_count_q {k5}{z}, zrot_perm, zbits_count
	vpsllvq	codes1, codes1, zbits_count_q

	;; Check whether any of the last bytes overlap
	vpcmpq k6 {k5}, code_lens1, zbits_count, 1

	;; Get last byte in each qword
	vpsrlq	code_lens2, code_lens2, 3
	vpaddq	code_lens1, code_lens1, zbits_count_q
	vpandq	code_lens1, code_lens1, znotoffset_mask
	vpsrlvq	codes3, codes1, code_lens1

	;; Branch to handle overlapping last bytes
	ktestd k6, k6
	jnz .small_codes

.small_codes_next:
	;; Save off zbits and zbits_count for next loop
	knotq	k7, k5
	vpermq	zbits {k7}{z}, zrot_perm, codes3
	vpermq	zbits_count {k7}{z}, zrot_perm, zbits_count

	;; Merge last byte in each qword with the next qword
	vpermq	codes3 {k5}{z}, zrot_perm, codes3
	vpxord codes1, codes1, codes3

	;; Determine total bytes written
	vextracti64x2 code_lens1 %+ x, code_lens2, 3
	vpextrq tmp2, code_lens1 %+ x, 1

	;; Write out qwords
	knotq	k6, k0
	vpermq code_lens2 {k5}{z}, zrot_perm, code_lens2
	vpscatterqq [out_buf + code_lens2] {k6}, codes1

	add	out_buf, tmp2

	cmp	ptr, in_buf_end
	jbe	.main_loop

.main_loop_exit:
	vmovq	rcx, zbits_count %+ x
	vmovq	bits, zbits %+ x
	jmp	.finish

.small_codes:
	;; Merge overlapping last bytes
	vpermq	codes4 {k6}{z}, zrot_perm, codes3
	vporq codes3, codes3, codes4
	kshiftlq k7, k6, 1
	ktestd k6, k7
	jz .small_codes_next

	kandq k6, k6, k7
	jmp .small_codes

.long_codes:
	add	end_ptr, VECTOR_SLOP
	sub	ptr, VECTOR_SIZE

	vmovdqa32 codes3 {k1}{z}, codes1
	vmovdqa32 code_lens3 {k1}{z}, code_lens1
	vmovdqa32 codes4 {k1}{z}, codes2

	vpsllvq	codes4, codes4, code_lens3
	vpxord	codes3, codes3, codes4
	vpaddd	code_lens3, code_lens1, code_lens2

	vpsrlq	codes1, codes1, 32
	vpsrlq	code_lens1, code_lens1, 32
	vpsrlq	codes2, codes2, 32

	vpsllvq	codes2, codes2, code_lens1
	vpxord codes1, codes1, codes2

	vpsrlq code_lens1, code_lens3, 32
	vmovdqa32	code_lens3 {k1}{z}, code_lens3

	;; Merge bitbuf bits
	vpsllvq codes3, codes3, zbits_count
	vpxord	codes3, codes3, zbits
	vpaddq	code_lens3, code_lens3, zbits_count
	vpaddq code_lens1, code_lens1, code_lens3

	xor	bits, bits
	xor	rcx, rcx
	vpsubq code_lens1, code_lens1, code_lens3

	vmovdqu64 codes2, codes1
	vmovdqu64 code_lens2, code_lens1
	vmovdqu64 codes4, codes3
	vmovdqu64 code_lens4, code_lens3
%assign i 0
%rep 4
%assign i (i + 1)
;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
	cmp	out_buf, end_ptr
	ja	.overflow
	;; insert LL code
	vmovq	sym, codes3 %+ x
	vmovq	tmp2, code_lens3 %+ x
	SHLX	sym, sym, rcx
	or	bits, sym
	add	rcx, tmp2

	; empty bits
	mov	[out_buf], bits
	mov	tmp, rcx
	shr	tmp, 3		; byte count
	add	out_buf, tmp
	mov	tmp, rcx
	and	rcx, ~7
	SHRX	bits, bits, rcx
	mov	rcx, tmp
	and	rcx, 7
	add	ptr, 4

;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
	cmp	out_buf, end_ptr
	ja	.overflow
	;; insert LL code
	vmovq	sym, codes1 %+ x
	vmovq	tmp2, code_lens1 %+ x
	SHLX	sym, sym, rcx
	or	bits, sym
	add	rcx, tmp2

	; empty bits
	mov	[out_buf], bits
	mov	tmp, rcx
	shr	tmp, 3		; byte count
	add	out_buf, tmp
	mov	tmp, rcx
	and	rcx, ~7
	SHRX	bits, bits, rcx
	mov	rcx, tmp
	and	rcx, 7
	add	ptr, 4

;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
	cmp	out_buf, end_ptr
	ja	.overflow
	;; insert LL code
	vpextrq	sym, codes3 %+ x, 1
	vpextrq	tmp2, code_lens3 %+ x, 1
	SHLX	sym, sym, rcx
	or	bits, sym
	add	rcx, tmp2

	; empty bits
	mov	[out_buf], bits
	mov	tmp, rcx
	shr	tmp, 3		; byte count
	add	out_buf, tmp
	mov	tmp, rcx
	and	rcx, ~7
	SHRX	bits, bits, rcx
	mov	rcx, tmp
	and	rcx, 7
	add	ptr, 4

;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
	cmp	out_buf, end_ptr
	ja	.overflow
	;; insert LL code
	vpextrq	sym, codes1 %+ x, 1
	vpextrq	tmp2, code_lens1 %+ x, 1
	SHLX	sym, sym, rcx
	or	bits, sym
	add	rcx, tmp2

	; empty bits
	mov	[out_buf], bits
	mov	tmp, rcx
	shr	tmp, 3		; byte count
	add	out_buf, tmp
	mov	tmp, rcx
	and	rcx, ~7
	SHRX	bits, bits, rcx
	mov	rcx, tmp
	and	rcx, 7
	add	ptr, 4

	vextracti32x4 codes3 %+ x, codes4, i
	vextracti32x4 code_lens3 %+ x, code_lens4, i
	vextracti32x4 codes1 %+ x, codes2, i
	vextracti32x4 code_lens1 %+ x, code_lens2, i
%endrep
	sub	end_ptr, VECTOR_SLOP

	vmovq	zbits %+ x, bits
	vmovq	zbits_count %+ x, rcx
	cmp	ptr, in_buf_end
	jbe	.main_loop

.finish:
	add	in_buf_end, VECTOR_LOOP_PROCESSED
	add	end_ptr, VECTOR_SLOP

	cmp	ptr, in_buf_end
	jge	.overflow

.finish_loop:
	mov	DWORD(data), [ptr]

	cmp	out_buf, end_ptr
	ja	.overflow

	mov	sym, data
	and	sym, LIT_MASK	; sym has ll_code
	mov	DWORD(sym), [hufftables + _lit_len_table + sym * 4]

	; look up dist sym
	mov	dsym, data
	shr	dsym, DIST_OFFSET
	and	dsym, DIST_MASK
	mov	DWORD(dsym), [hufftables + _dist_table + dsym * 4]

	; insert LL code
	; sym: 31:24 length; 23:0 code
	mov	tmp2, sym
	and	sym, 0xFFFFFF
	SHLX	sym, sym, rcx
	shr	tmp2, 24
	or	bits, sym
	add	rcx, tmp2

	; insert dist code
	movzx	tmp, WORD(dsym)
	SHLX	tmp, tmp, rcx
	or	bits, tmp
	mov	tmp, dsym
	shr	tmp, 24
	add	rcx, tmp

	; insert dist extra bits
	shr	data, EXTRA_BITS_OFFSET
	add	ptr, 4
	SHLX	data, data, rcx
	or	bits, data
	shr	dsym, 16
	and	dsym, 0xFF
	add	rcx, dsym

	; empty bits
	mov	[out_buf], bits
	mov	tmp, rcx
	shr	tmp, 3		; byte count
	add	out_buf, tmp
	mov	tmp, rcx
	and	rcx, ~7
	SHRX	bits, bits, rcx
	mov	rcx, tmp
	and	rcx, 7

	cmp	ptr, in_buf_end
	jb	.finish_loop

.overflow:
	mov	tmp, [rsp + bitbuf_mem_offset]
	mov	[tmp + _m_bits], bits
	mov	[tmp + _m_bit_count], ecx
	mov	[tmp + _m_out_buf], out_buf

	mov	rax, ptr

	FUNC_RESTORE

	ret

section .data
	align 64
;; 64 byte data
rot_perm:
	dq 0x00000007, 0x00000000, 0x00000001, 0x00000002
	dq 0x00000003, 0x00000004, 0x00000005, 0x00000006

;; 16 byte data
q_64:
	dq 0x0000000000000040, 0x0000000000000000
q_8 :
	dq 0x0000000000000000, 0x0000000000000008

;; 8 byte data
offset_mask:
	dq 0x0000000000000007

;; 4 byte data
max_write_d:
	dd 0x1c
lit_mask:
	dd LIT_MASK
dist_mask:
	dd DIST_MASK
lit_icr_mask:
	dd 0x00ffffff
eb_icr_mask:
	dd 0x000000ff

;; k mask constants
k_mask_1: dq 0x55555555
k_mask_2: dq 0xfffffff0
k_mask_3: dq 0xfffffffc
k_mask_4: dw 0x0101, 0x0101, 0x0101, 0x0101
k_mask_5: dq 0xfffffffe

%endif
