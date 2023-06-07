;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
; Copyright (C) 2022 Intel Corporation
;
; SPDX-License-Identifier: MIT
;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;

%include "reg_sizes.asm"
%include "lz0a_const.asm"
%include "data_struct2.asm"
%include "stdmac.asm"

%define ARCH 04
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

%define codes1		ymm1
%define code_lens1	ymm2
%define codes2		ymm3
%define code_lens2	ymm4
%define codes3		ymm5
%define	code_lens3	ymm6
%define codes4		ymm7
%define syms		ymm7

%define code_lens4	ymm8
%define dsyms		ymm8

%define ytmp		ymm9
%define codes_lookup1	ymm10
%define	codes_lookup2	ymm11
%define datas		ymm12
%define ybits		ymm13
%define ybits_count	ymm14
%define yoffset_mask	ymm15

%define VECTOR_SIZE 0x20
%define VECTOR_LOOP_PROCESSED (2 * VECTOR_SIZE)
%define VECTOR_SLOP 0x20 - 8

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

	vpcmpeqq	ytmp, ytmp, ytmp
	vmovdqu	datas, [ptr]
	vpand	syms, datas, [lit_mask]
	vpgatherdd codes_lookup1, [hufftables + _lit_len_table + 4 * syms], ytmp

	vpcmpeqq	ytmp, ytmp, ytmp
	vpsrld	dsyms, datas, DIST_OFFSET
	vpand	dsyms, dsyms, [dist_mask]
	vpgatherdd codes_lookup2, [hufftables + _dist_table + 4 * dsyms], ytmp

	vmovq	ybits %+ x, bits
	vmovq	ybits_count %+ x, rcx
	vmovdqa	yoffset_mask, [offset_mask]

.main_loop:
	;;  Sets codes1 to contain lit/len codes andcode_lens1 the corresponding lengths
	vpsrld	code_lens1, codes_lookup1, 24
	vpand	codes1, codes_lookup1, [lit_icr_mask]

	;; Sets codes2 to contain dist codes, code_lens2 the corresponding lengths,
	;; and code_lens3 the extra bit counts
	vpblendw	codes2, ybits, codes_lookup2, 0x55 ;Bits 8 and above of ybits are 0
	vpsrld	code_lens2, codes_lookup2, 24
	vpsrld	code_lens3, codes_lookup2, 16
	vpand	code_lens3, [eb_icr_mask]

	;; Set codes3 to contain the extra bits
	vpsrld	codes3, datas, EXTRA_BITS_OFFSET

	cmp	out_buf, end_ptr
	ja	.main_loop_exit

	;; Start code lookups for next iteration
	add	ptr, VECTOR_SIZE
	vpcmpeqq	ytmp, ytmp, ytmp
	vmovdqu	datas, [ptr]
	vpand	syms, datas, [lit_mask]
	vpgatherdd codes_lookup1, [hufftables + _lit_len_table + 4 * syms], ytmp

	vpcmpeqq	ytmp, ytmp, ytmp
	vpsrld	dsyms, datas, DIST_OFFSET
	vpand	dsyms, dsyms, [dist_mask]
	vpgatherdd codes_lookup2, [hufftables + _dist_table + 4 * dsyms], ytmp

	;; Merge dist code with extra bits
	vpsllvd	codes3, codes3, code_lens2
	vpxor	codes2, codes2, codes3
	vpaddd	code_lens2, code_lens2, code_lens3

	;; Check for long codes
	vpaddd	code_lens3, code_lens1, code_lens2
	vpcmpgtd	ytmp, code_lens3, [max_write_d]
	vptest	ytmp, ytmp
	jnz	.long_codes

	;; Merge dist and len codes
	vpsllvd	codes2, codes2, code_lens1
	vpxor	codes1, codes1, codes2

	;; Split buffer data into qwords, ytmp is 0 after last branch
	vpblendd codes3, ytmp, codes1, 0x55
	vpsrlq	codes1, codes1, 32
	vpsrlq	code_lens1, code_lens3, 32
	vpblendd	code_lens3, ytmp, code_lens3, 0x55

	;; Merge bitbuf bits
	vpsllvq codes3, codes3, ybits_count
	vpxor	codes3, codes3, ybits
	vpaddq	code_lens3, code_lens3, ybits_count

	;; Merge two symbols into qwords
	vpsllvq	codes1, codes1, code_lens3
	vpxor codes1, codes1, codes3
	vpaddq code_lens1, code_lens1, code_lens3

	;; Split buffer data into dqwords, ytmp is 0 after last branch
	vpblendd	codes2, ytmp, codes1, 0x33
	vpblendd	code_lens2, ytmp, code_lens1, 0x33
	vpsrldq	codes1, 8
	vpsrldq	code_lens1, 8

	;; Bit align dqwords
	vpaddq	code_lens1, code_lens1, code_lens2
	vpand	ybits_count, code_lens1, yoffset_mask ;Extra bits
	vpermq	ybits_count, ybits_count, 0xcf
	vpaddq	code_lens2, ybits_count
	vpsllvq	codes2, codes2, ybits_count

	;; Merge two qwords into dqwords
	vmovdqa	ytmp, [q_64]
	vpsubq	code_lens3, ytmp, code_lens2
	vpsrlvq	codes3, codes1, code_lens3
	vpslldq	codes3, codes3, 8

	vpsllvq	codes1, codes1, code_lens2

	vpxor	codes1, codes1, codes3
	vpxor	codes1, codes1, codes2

	vmovq	tmp, code_lens1 %+ x 	;Number of bytes
	shr	tmp, 3

	;; Extract last bytes
	vpaddq	code_lens2, code_lens1, ybits_count
	vpsrlq	code_lens2, code_lens2, 3
	vpshufb	codes2, codes1, code_lens2
	vpand	codes2, codes2, [bytes_mask]
	vextracti128	ybits %+ x, codes2, 1

	;; Check for short codes
	vptest code_lens2, [min_write_mask]
	jz	.short_codes
.short_codes_next:

	vpermq	codes2, codes2, 0x45
	vpor	codes1, codes1, codes2

	;; bit shift upper dqword combined bits to line up with lower dqword
	vextracti128	code_lens2 %+ x, code_lens1, 1

	; Write out lower dqword of combined bits
	vmovdqu	[out_buf], codes1
	vpaddq	code_lens1, code_lens1, code_lens2

	vmovq	tmp2, code_lens1 %+ x	;Number of bytes
	shr	tmp2, 3
	vpand	ybits_count, code_lens1, yoffset_mask ;Extra bits

	; Write out upper dqword of combined bits
	vextracti128	[out_buf + tmp], codes1, 1
	add	out_buf, tmp2

	cmp	ptr, in_buf_end
	jbe	.main_loop

.main_loop_exit:
	vmovq	rcx, ybits_count %+ x
	vmovq	bits, ybits %+ x
	jmp	.finish

.short_codes:
	;; Merge last bytes when the second dqword contains less than a byte
	vpor ybits %+ x, codes2 %+ x
	jmp .short_codes_next

.long_codes:
	add	end_ptr, VECTOR_SLOP
	sub	ptr, VECTOR_SIZE

	vpxor ytmp, ytmp, ytmp
	vpblendd codes3, ytmp, codes1, 0x55
	vpblendd code_lens3, ytmp, code_lens1, 0x55
	vpblendd codes4, ytmp, codes2, 0x55

	vpsllvq	codes4, codes4, code_lens3
	vpxor	codes3, codes3, codes4
	vpaddd	code_lens3, code_lens1, code_lens2

	vpsrlq	codes1, codes1, 32
	vpsrlq	code_lens1, code_lens1, 32
	vpsrlq	codes2, codes2, 32

	vpsllvq	codes2, codes2, code_lens1
	vpxor codes1, codes1, codes2

	vpsrlq code_lens1, code_lens3, 32
	vpblendd	code_lens3, ytmp, code_lens3, 0x55

	;; Merge bitbuf bits
	vpsllvq codes3, codes3, ybits_count
	vpxor	codes3, codes3, ybits
	vpaddq	code_lens3, code_lens3, ybits_count
	vpaddq code_lens1, code_lens1, code_lens3

	xor	bits, bits
	xor	rcx, rcx
	vpsubq code_lens1, code_lens1, code_lens3
%rep 2
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

	vextracti128 codes3 %+ x, codes3, 1
	vextracti128 code_lens3 %+ x, code_lens3, 1
	vextracti128 codes1 %+ x, codes1, 1
	vextracti128 code_lens1 %+ x, code_lens1, 1
%endrep
	sub	end_ptr, VECTOR_SLOP

	vmovq	ybits %+ x, bits
	vmovq	ybits_count %+ x, rcx
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
	align 32
max_write_d:
	dd	0x1c, 0x1d, 0x1f, 0x20, 0x1c, 0x1d, 0x1f, 0x20
min_write_mask:
	dq	0x00, 0x00, 0xff, 0x00
offset_mask:
	dq	0x0000000000000007, 0x0000000000000000
	dq	0x0000000000000000, 0x0000000000000000
q_64:
	dq	0x0000000000000040, 0x0000000000000000
	dq	0x0000000000000040, 0x0000000000000000
lit_mask:
	dd LIT_MASK, LIT_MASK, LIT_MASK, LIT_MASK
	dd LIT_MASK, LIT_MASK, LIT_MASK, LIT_MASK
dist_mask:
	dd DIST_MASK, DIST_MASK, DIST_MASK, DIST_MASK
	dd DIST_MASK, DIST_MASK, DIST_MASK, DIST_MASK
lit_icr_mask:
	dd 0x00FFFFFF, 0x00FFFFFF, 0x00FFFFFF, 0x00FFFFFF
	dd 0x00FFFFFF, 0x00FFFFFF, 0x00FFFFFF, 0x00FFFFFF
eb_icr_mask:
	dd 0x000000FF, 0x000000FF, 0x000000FF, 0x000000FF
	dd 0x000000FF, 0x000000FF, 0x000000FF, 0x000000FF
bytes_mask:
	dq	0x00000000000000ff, 0x0000000000000000
	dq	0x00000000000000ff, 0x0000000000000000
