;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
; Copyright (C) 2022 Intel Corporation
;
; SPDX-License-Identifier: MIT
;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
%include "options.asm"

%include "lz0a_const.asm"
%include "data_struct2.asm"
%include "bitbuf2.asm"
%include "huffman.asm"
%include "igzip_compare_types.asm"
%include "reg_sizes.asm"

%include "stdmac.asm"
%ifdef DEBUG
%macro MARK 1
global %1
%1:
%endm
%else
%macro MARK 1
%endm
%endif

%define LARGE_MATCH_HASH_REP 1 	; Hash 4 * LARGE_MATCH_HASH_REP elements
%define LARGE_MATCH_MIN 264 	; Minimum match size to enter large match emit loop
%define MIN_INBUF_PADDING 16
%define MAX_EMIT_SIZE 258 * 16

%define SKIP_SIZE_BASE (2 << 10)      ; No match length before starting skipping
%define SKIP_BASE 32		      ; Initial skip size
%define SKIP_START 512                ; Start increasing skip size once level is beyond SKIP_START
%define SKIP_RATE 2		      ; Rate skip size increases after SKIP_START
%define MAX_SKIP_SIZE 128             ; Maximum skip size
%define MAX_SKIP_LEVEL (((MAX_SKIP_SIZE - SKIP_BASE) / SKIP_RATE) + SKIP_START) ; Maximum skip level
;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
%define	file_start	rdi
%define file_length	r15
%define level_buf	r14
%define	f_i		r10
%define	m_out_buf	r11

%define	curr_data	rax

%define	tmp2		rcx
%define skip_count	rcx

%define	dist		rbx
%define dist_code2	rbx
%define	lit_code2	rbx
%define hmask2		rbx

%define	dist2		r12
%define dist_code	r12
%define hmask3		r12

%define	tmp1		rsi
%define	lit_code	rsi

%define	curr_data2	r8
%define	len2		r8
%define	tmp4		r8
%define hmask1		r8
%define len_code2	r8

%define	len		rdx
%define len_code	rdx
%define hash3		rdx

%define	stream		r13
%define	tmp3		r13

%define hash		rbp
%define	hash2		r9

;; GPR r8 & r15 can be used

%define xtmp0		xmm0	; tmp
%define xtmp1		xmm1	; tmp
%define xlow_lit_shuf	xmm2
%define xup_lit_shuf	xmm3
%define	xdata		xmm4
%define xlit		xmm5

%define ytmp0		ymm0	; tmp
%define ytmp1		ymm1	; tmp

%define hash_table level_buf + _hash8k_hash_table
%define lit_len_hist level_buf + _hist_lit_len
%define dist_hist level_buf + _hist_dist

;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;

m_out_end           equ  0	 ; local variable (8 bytes)
m_out_start         equ	 8
dist_mask_offset    equ 16
hash_mask_offset    equ 24
f_end_i_mem_offset  equ 32
stream_offset       equ 40
inbuf_slop_offset   equ 48
skip_match_offset   equ 56
skip_level_offset    equ 64
gpr_save_mem_offset equ 80       ; gpr save area (8*8 bytes)
xmm_save_mem_offset equ gpr_save_mem_offset + 8*8 ; xmm save area (4*16 bytes) (16 byte aligned)
stack_size          equ 11*8 + 8*8 + 4*16

;;; 8 because stack address is odd multiple of 8 after a function call and
;;; we want it aligned to 16 bytes

;; Defines to generate functions for different architecture
%xdefine ARCH 01
%xdefine ARCH1 02
%xdefine ARCH2 04

%ifndef COMPARE_TYPE
%xdefine COMPARE_TYPE_NOT_DEF
%xdefine COMPARE_TYPE 1
%xdefine COMPARE_TYPE1 2
%xdefine COMPARE_TYPE2 3
%endif

;; Defines to generate functions for different levels
%xdefine METHOD hash_hist

%rep 3
%if ARCH == 04
%define USE_HSWNI
%endif

[bits 64]
default rel
section .text

; void isal_deflate_icf_body <hashsize> <arch> ( isal_zstream *stream )
; we make 6 different versions of this function
; arg 1: rcx: addr of stream
global isal_deflate_icf_body_ %+ METHOD %+ _ %+ ARCH
isal_deflate_icf_body_ %+ METHOD %+ _ %+ ARCH %+ :
	endbranch
%ifidn __OUTPUT_FORMAT__, elf64
	mov	rcx, rdi
%endif

	;; do nothing if (avail_in == 0)
	cmp	dword [rcx + _avail_in], 0
	jne	.skip1

	;; Set stream's next state
	mov	rdx, ZSTATE_CREATE_HDR
	mov	eax, [rcx + _internal_state_state]
	cmp	word [rcx + _end_of_stream], 0
	cmovne	rax, rdx
	cmp	word [rcx + _flush], _NO_FLUSH
	cmovne	rax, rdx
	mov	dword [rcx + _internal_state_state], eax
	ret
.skip1:

%ifdef ALIGN_STACK
	push	rbp
	mov	rbp, rsp
	sub	rsp, stack_size
	and	rsp, ~15
%else
	sub	rsp, stack_size
%endif

	mov [rsp + gpr_save_mem_offset + 0*8], rbx
	mov [rsp + gpr_save_mem_offset + 1*8], rsi
	mov [rsp + gpr_save_mem_offset + 2*8], rdi
	mov [rsp + gpr_save_mem_offset + 3*8], rbp
	mov [rsp + gpr_save_mem_offset + 4*8], r12
	mov [rsp + gpr_save_mem_offset + 5*8], r13
	mov [rsp + gpr_save_mem_offset + 6*8], r14
	mov [rsp + gpr_save_mem_offset + 7*8], r15

	mov	stream, rcx
	mov	[rsp + stream_offset], stream

	mov	byte [stream + _internal_state_has_eob], 0

	mov	tmp1 %+ d, dword[stream + _internal_state_dist_mask]
	mov	[rsp + dist_mask_offset], tmp1
	mov	tmp1 %+ d, dword[stream + _internal_state_hash_mask]
	mov	[rsp + hash_mask_offset], tmp1

	; state->bitbuf.set_buf(stream->next_out, stream->avail_out);
	mov	level_buf, [stream + _level_buf]
	mov	m_out_buf, [level_buf + _icf_buf_next]

	mov     [rsp + m_out_start], m_out_buf
	mov	tmp1, [level_buf + _icf_buf_avail_out]
	add	tmp1, m_out_buf
	sub	tmp1, SLOP

	mov	[rsp + m_out_end], tmp1

	mov	file_start, [stream + _next_in]

	mov	f_i %+ d, dword [stream + _total_in]
	sub	file_start, f_i

	mov	file_length %+ d, [stream + _avail_in]
	add	file_length, f_i

	mov	[rsp + skip_match_offset], f_i
	add	qword [rsp + skip_match_offset], SKIP_SIZE_BASE
	mov	qword [rsp + skip_level_offset], 0

	PBROADCASTD xlit, dword [min_lit_dist_syms]
	MOVDQU	xlow_lit_shuf, [low_lit_shuf]
	MOVDQU	xup_lit_shuf, [up_lit_shuf]

	mov	qword [rsp + inbuf_slop_offset], MIN_INBUF_PADDING
	cmp	byte [stream + _end_of_stream], 0
	jnz	.default_inbuf_padding
	cmp	byte [stream + _flush], 0
	jnz	.default_inbuf_padding
	mov	qword [rsp + inbuf_slop_offset], LA
.default_inbuf_padding:

	; file_length -= INBUF_PADDING;
	sub	file_length, [rsp + inbuf_slop_offset]
	; if (file_length <= 0) continue;
	mov	hmask1 %+ d, [rsp + hash_mask_offset]

	cmp	file_length, f_i
	jle	.input_end

	; for (f_i = f_start_i; f_i < file_length; f_i++) {
	MOVDQU	xdata, [file_start + f_i]
	mov	curr_data, [file_start + f_i]
	mov	tmp1, curr_data

	compute_hash	hash, curr_data

	shr	tmp1, 8
	compute_hash	hash2, tmp1

	and	hash, hmask1
	and	hash2, hmask1

	cmp	byte [stream + _internal_state_has_hist], IGZIP_NO_HIST
	je	.write_first_byte

	jmp	.loop2
	align	16

.loop2:
	mov	tmp3 %+ d, [rsp + dist_mask_offset]
	mov	hmask1 %+ d, [rsp + hash_mask_offset]
	; if (state->bitbuf.is_full()) {
	cmp	m_out_buf, [rsp + m_out_end]
	ja	.output_end

	xor	dist, dist
	xor	dist2, dist2

	lea	tmp1, [file_start + f_i]

	mov	dist %+ w, f_i %+ w
	dec	dist
	sub	dist %+ w, word [hash_table + 2 * hash]
	mov	[hash_table + 2 * hash], f_i %+ w

	inc	f_i

	mov	tmp2, curr_data
	shr	curr_data, 16
	compute_hash	hash, curr_data
	and	hash %+ d, hmask1 %+ d

	mov	dist2 %+ w, f_i %+ w
	dec	dist2
	sub	dist2 %+ w, word [hash_table + 2 * hash2]
	mov	[hash_table + 2 * hash2], f_i %+ w

	; if ((dist-1) < (D-1)) {
	and	dist %+ d, tmp3 %+ d
	neg	dist

	shr	tmp2, 24
	compute_hash	hash2, tmp2
	and	hash2 %+ d, hmask1 %+ d

	and	dist2 %+ d, tmp3 %+ d
	neg	dist2

	;; Check for long len/dist match (>7) with first literal
	MOVQ	len, xdata
	mov	curr_data, len
	PSRLDQ	xdata, 1
	xor	len, [tmp1 + dist - 1]
	jz	.compare_loop

	;; Check for len/dist match (>7) with second literal
	MOVQ	len2, xdata
	xor	len2, [tmp1 + dist2]
	jz	.compare_loop2

	movzx	lit_code, curr_data %+ b
	shr	curr_data, 8

	;; Check for len/dist match for first literal
	test    len %+ d, 0xFFFFFFFF
	jz      .len_dist_huffman_pre

	PSRLDQ	xdata, 1
	inc	dword [lit_len_hist + HIST_ELEM_SIZE*lit_code]
	movzx	lit_code2, curr_data %+ b
	;; Check for len/dist match for second literal
	test    len2 %+ d, 0xFFFFFFFF
	jnz     .write_lit_bits

.len_dist_lit_huffman_pre:
	bsf	len2, len2
	shr	len2, 3

.len_dist_lit_huffman:
	or	lit_code, LIT
	mov	dword [m_out_buf], lit_code %+ d

	neg	dist2

	get_dist_icf_code	dist2, dist_code2, tmp1

	mov hmask3 %+ d, dword [rsp + hash_mask_offset]

	;; Setup for updating hash
	lea	tmp3, [f_i + 1]	; tmp3 <= k

	mov	tmp2, f_i
	add	file_start, f_i
	add	f_i, len2
	cmp	f_i, file_length
	jg	.len_dist_lit_huffman_finish

	lea	tmp1, [f_i + SKIP_SIZE_BASE]
	mov	qword [rsp + skip_match_offset], tmp1
	sub	qword [rsp + skip_level_offset], len2

	MOVDQU	xdata, [file_start + len2]
	mov	tmp1, [file_start + len2]
	sub	file_start, tmp2

	shr	curr_data, 24
	compute_hash	hash3, curr_data
	and	hash3 %+ d, hmask3 %+ d

	mov	curr_data, tmp1
	shr	tmp1, 8

	mov	[hash_table + 2 * hash], tmp3 %+ w

	compute_hash	hash, curr_data

	add	tmp3,1
	mov	[hash_table + 2 * hash2], tmp3 %+ w

	compute_hash	hash2, tmp1

	add	tmp3, 1
	mov	[hash_table + 2 * hash3], tmp3 %+ w

	add	dist_code2, 254
	add	dist_code2, len2

	inc	dword [lit_len_hist + HIST_ELEM_SIZE*(len2 + 254)]

	mov	dword [m_out_buf + 4], dist_code2 %+ d
	add	m_out_buf, 8

	shr	dist_code2, DIST_OFFSET
	and	dist_code2, 0x1F
	inc	dword [dist_hist + HIST_ELEM_SIZE*dist_code2]

	; hash = compute_hash(state->file_start + f_i) & hash_mask;
	and	hash %+ d, hmask3 %+ d
	and	hash2 %+ d, hmask3 %+ d

	; continue
	jmp	.loop2

.len_dist_lit_huffman_finish:
	sub	file_start, tmp2

	mov	[hash_table + 2 * hash], tmp3 %+ w
	add	tmp3,1
	mov	[hash_table + 2 * hash2], tmp3 %+ w

	add	dist_code2, 254
	add	dist_code2, len2

	inc	dword [lit_len_hist + HIST_ELEM_SIZE*(len2 + 254)]

	mov	dword [m_out_buf + 4], dist_code2 %+ d
	add	m_out_buf, 8

	shr	dist_code2, DIST_OFFSET
	and	dist_code2, 0x1F
	inc	dword [dist_hist + HIST_ELEM_SIZE*dist_code2]

	jmp	.input_end

.len_dist_huffman_pre:
	bsf	len, len
	shr	len, 3

.len_dist_huffman:
	dec	f_i
	;; Setup for updateing hash
	lea	tmp3, [f_i + 2]	; tmp3 <= k

	neg	dist

	; get_dist_code(dist, &code2, &code_len2);
	get_dist_icf_code   dist, dist_code, tmp1

.len_dist_huffman_skip:

	mov	hmask2 %+ d, [rsp + hash_mask_offset]

	mov	tmp1, f_i
	add	file_start, f_i

	add	f_i, len
	cmp	f_i, file_length
	jg	.len_dist_huffman_finish

	lea	tmp2, [f_i + SKIP_SIZE_BASE]
	mov	qword [rsp + skip_match_offset], tmp2
	sub	qword [rsp + skip_level_offset], len

	MOVDQU	xdata, [file_start + len]
	mov	curr_data2, [file_start + len]
	mov	curr_data, curr_data2
	sub	file_start, tmp1
	; get_len_code(len, &code, &code_len);
	lea	len_code, [len + 254]
	or	dist_code, len_code

	mov	[hash_table + 2 * hash], tmp3 %+ w
	add	tmp3,1
	mov	[hash_table + 2 * hash2], tmp3 %+ w

	compute_hash	hash, curr_data

	shr	curr_data2, 8
	compute_hash	hash2, curr_data2

	inc	dword [lit_len_hist + HIST_ELEM_SIZE*len_code]

	mov	dword [m_out_buf], dist_code %+ d
	add	m_out_buf, 4

	shr     dist_code, DIST_OFFSET
	and     dist_code, 0x1F
	inc     dword [dist_hist + HIST_ELEM_SIZE*dist_code]

	; hash = compute_hash(state->file_start + f_i) & hash_mask;
	and	hash %+ d, hmask2 %+ d
	and	hash2 %+ d, hmask2 %+ d

	; continue
	jmp	.loop2

.len_dist_huffman_finish:
	sub	file_start, tmp1

	; get_len_code(len, &code, &code_len);
	lea	len_code, [len + 254]
	or	dist_code, len_code

	mov	[hash_table + 2 * hash], tmp3 %+ w
	add	tmp3,1
	mov	[hash_table + 2 * hash2], tmp3 %+ w

	inc	dword [lit_len_hist + HIST_ELEM_SIZE*len_code]

	mov	dword [m_out_buf], dist_code %+ d
	add	m_out_buf, 4

	shr     dist_code, DIST_OFFSET
	and     dist_code, 0x1F
	inc     dword [dist_hist + HIST_ELEM_SIZE*dist_code]

	jmp	.input_end

.write_lit_bits:
	MOVQ	curr_data, xdata

	add	f_i, 1
	cmp	f_i, file_length
	jg	.write_lit_bits_finish

	MOVDQU	xdata, [file_start + f_i]

	inc	dword [lit_len_hist + HIST_ELEM_SIZE*lit_code2]

	shl	lit_code2, DIST_OFFSET
	lea	lit_code, [lit_code + lit_code2 + (31 << DIST_OFFSET)]

	mov	dword [m_out_buf], lit_code %+ d
	add	m_out_buf, 4

	cmp	f_i, [rsp + skip_match_offset]
	jle	.loop2

	xor	tmp3, tmp3
	mov	rcx, [rsp + skip_level_offset]
	add	rcx, 1
	cmovl	rcx, tmp3
	mov	tmp1, MAX_SKIP_LEVEL
	cmp	rcx, MAX_SKIP_LEVEL
	cmovg	rcx, tmp1

	mov	tmp1, SKIP_SIZE_BASE
	shr	tmp1, cl

%if MAX_SKIP_LEVEL > 63
	cmp	rcx, 63
	cmovg	tmp1, tmp3
%endif
	mov	[rsp + skip_match_offset], tmp1
	mov	[rsp + skip_level_offset], rcx

	sub	rcx, SKIP_START
	cmovl	rcx, tmp3

	lea	skip_count, [SKIP_RATE * rcx + SKIP_BASE]
	and	skip_count, -SKIP_BASE

	mov	tmp1, [rsp + m_out_end]
	lea	tmp1, [tmp1 + 4]
	sub	tmp1, m_out_buf
	shr	tmp1, 1
	cmp	tmp1, skip_count
	jl	.skip_forward_short

	mov	tmp1, [rsp + inbuf_slop_offset]
	add	tmp1, file_length
	sub	tmp1, f_i
	cmp	tmp1, skip_count
	jl	.skip_forward_short

.skip_forward_long:
	MOVQ	xdata, [file_start + f_i]

	movzx	lit_code, byte [file_start + f_i]
	movzx	lit_code2, byte [file_start + f_i + 1]

	add	dword [lit_len_hist + HIST_ELEM_SIZE*lit_code], 1
	add	dword [lit_len_hist + HIST_ELEM_SIZE*lit_code2], 1

	movzx	lit_code, byte [file_start + f_i + 2]
	movzx	lit_code2, byte [file_start + f_i + 3]

	add	dword [lit_len_hist + HIST_ELEM_SIZE*lit_code], 1
	add	dword [lit_len_hist + HIST_ELEM_SIZE*lit_code2], 1

	movzx	lit_code, byte [file_start + f_i + 4]
	movzx	lit_code2, byte [file_start + f_i + 5]

	add	dword [lit_len_hist + HIST_ELEM_SIZE*lit_code], 1
	add	dword [lit_len_hist + HIST_ELEM_SIZE*lit_code2], 1

	movzx	lit_code, byte [file_start + f_i + 6]
	movzx	lit_code2, byte [file_start + f_i + 7]

	add	dword [lit_len_hist + HIST_ELEM_SIZE*lit_code], 1
	add	dword [lit_len_hist + HIST_ELEM_SIZE*lit_code2], 1

	PSHUFB	xtmp0, xdata, xlow_lit_shuf
	PSHUFB	xtmp1, xdata, xup_lit_shuf
	PSLLD	xtmp1, xtmp1, DIST_OFFSET
	POR	xtmp0, xtmp0, xtmp1
	PADDD	xtmp0, xtmp0, xlit
	MOVDQU	[m_out_buf], xtmp0

	add	m_out_buf, 16
	add	f_i, 8

	sub	skip_count, 8
	jg	.skip_forward_long

	cmp	file_length, f_i
	jle	.input_end

	mov	curr_data, [file_start + f_i]
	MOVDQU	xdata, [file_start + f_i]
	add	[rsp + skip_match_offset], f_i

	jmp	.loop2

.skip_forward_short:
	movzx	lit_code, byte [file_start + f_i]
	movzx	lit_code2, byte [file_start + f_i + 1]

	inc	dword [lit_len_hist + HIST_ELEM_SIZE*lit_code]
	inc	dword [lit_len_hist + HIST_ELEM_SIZE*lit_code2]

	shl	lit_code2, DIST_OFFSET
	lea	lit_code, [lit_code + lit_code2 + (31 << DIST_OFFSET)]

	mov	dword [m_out_buf], lit_code %+ d
	add	m_out_buf, 4
	add	f_i, 2

	cmp	m_out_buf, [rsp + m_out_end]
	ja	.output_end

	cmp	file_length, f_i
	jle	.input_end

	jmp	.skip_forward_short

.write_lit_bits_finish:
	inc	dword [lit_len_hist + HIST_ELEM_SIZE*lit_code2]

	shl	lit_code2, DIST_OFFSET
	lea	lit_code, [lit_code + lit_code2 + (31 << DIST_OFFSET)]

	mov	dword [m_out_buf], lit_code %+ d
	add	m_out_buf, 4

.input_end:
	mov	stream, [rsp + stream_offset]
	mov	tmp1, ZSTATE_FLUSH_READ_BUFFER
	mov	tmp2, ZSTATE_BODY
	cmp	word [stream + _end_of_stream], 0
	cmovne	tmp2, tmp1
	cmp	word [stream + _flush], _NO_FLUSH

	cmovne	tmp2, tmp1
	mov	dword [stream + _internal_state_state], tmp2 %+ d
	jmp	.end

.output_end:
	mov	stream, [rsp + stream_offset]
	mov	dword [stream + _internal_state_state], ZSTATE_CREATE_HDR

.end:
	;; update input buffer
	add	file_length, [rsp + inbuf_slop_offset]
	mov	[stream + _total_in], f_i %+ d
	mov	[stream + _internal_state_block_end], f_i %+ d
	add	file_start, f_i
	mov     [stream + _next_in], file_start
	sub	file_length, f_i
	mov     [stream + _avail_in], file_length %+ d

	;; update output buffer
	mov	[level_buf + _icf_buf_next], m_out_buf
	sub	m_out_buf, [rsp + m_out_start]
	sub	[level_buf + _icf_buf_avail_out], m_out_buf %+ d

	mov rbx, [rsp + gpr_save_mem_offset + 0*8]
	mov rsi, [rsp + gpr_save_mem_offset + 1*8]
	mov rdi, [rsp + gpr_save_mem_offset + 2*8]
	mov rbp, [rsp + gpr_save_mem_offset + 3*8]
	mov r12, [rsp + gpr_save_mem_offset + 4*8]
	mov r13, [rsp + gpr_save_mem_offset + 5*8]
	mov r14, [rsp + gpr_save_mem_offset + 6*8]
	mov r15, [rsp + gpr_save_mem_offset + 7*8]

%ifndef ALIGN_STACK
	add	rsp, stack_size
%else
	mov	rsp, rbp
	pop	rbp
%endif
	ret

align 16
.compare_loop:
	lea	tmp2, [tmp1 + dist - 1]

	mov	len2, file_length
	sub	len2, f_i
	add	len2, [rsp + inbuf_slop_offset]
	add	len2, 1
	mov	tmp3,  MAX_EMIT_SIZE
	cmp	len2, tmp3
	cmovg	len2, tmp3

	mov	len, 8
	compare_large	tmp1, tmp2, len, len2, tmp3, ytmp0, ytmp1

	cmp	len, 258
	jle	.len_dist_huffman
	cmp	len, LARGE_MATCH_MIN
	jge	.do_emit
	mov	len, 258
	jmp	.len_dist_huffman

align 16
.compare_loop2:
	lea	tmp2, [tmp1 + dist2]
	add	tmp1, 1

	mov	len, file_length
	sub	len, f_i
	add	len, [rsp + inbuf_slop_offset]
	mov	tmp3, MAX_EMIT_SIZE
	cmp	len, tmp3
	cmovg	len, tmp3

	mov	len2, 8
	compare_large	tmp1, tmp2, len2, len, tmp3, ytmp0, ytmp1

	movzx	lit_code, curr_data %+ b
	shr	curr_data, 8
	inc	dword [lit_len_hist + HIST_ELEM_SIZE*lit_code]
	cmp	len2, 258
	jle	.len_dist_lit_huffman
	cmp	len2, LARGE_MATCH_MIN
	jge	.do_emit2
	mov	len2, 258
	jmp	.len_dist_lit_huffman

.do_emit2:
	or	lit_code, LIT
	mov	dword [m_out_buf], lit_code %+ d
	add	m_out_buf, 4

	inc	f_i
	mov	dist, dist2
	mov	len, len2

.do_emit:
	neg	dist
	get_dist_icf_code   dist, dist_code, tmp1

	mov	len_code2, 258 + 254
	or	len_code2, dist_code
	mov	tmp1, dist_code
	shr     tmp1, DIST_OFFSET
	and     tmp1, 0x1F
	lea	tmp3, [f_i + 1]
	dec	f_i

	mov	[hash_table + 2 * hash], tmp3 %+ w
	add	tmp3,1
	mov	[hash_table + 2 * hash2], tmp3 %+ w
.emit:
	sub	len, 258
	add	f_i, 258

	inc	dword [lit_len_hist + HIST_ELEM_SIZE*(258 + 254)]
	inc     dword [dist_hist + HIST_ELEM_SIZE*tmp1]
	mov	dword [m_out_buf], len_code2 %+ d
	add	m_out_buf, 4

	cmp	m_out_buf, [rsp + m_out_end]
	ja	.output_end

	cmp	len, LARGE_MATCH_MIN
	jge	.emit

	mov	len2, 258
	cmp	len, len2
	cmovg	len, len2

		; get_len_code(len, &code, &code_len);
	add	f_i, len
	lea	len_code, [len + 254]
	or	dist_code, len_code

	inc	dword [lit_len_hist + HIST_ELEM_SIZE*len_code]
	inc     dword [dist_hist + HIST_ELEM_SIZE*tmp1]

	mov	dword [m_out_buf], dist_code %+ d
	add	m_out_buf, 4

	cmp	file_length, f_i
	jle	.input_end

	lea	tmp2, [f_i - 4 * LARGE_MATCH_HASH_REP]
	mov	hmask2 %+ d, [rsp + hash_mask_offset]

%rep LARGE_MATCH_HASH_REP
	mov	curr_data %+ d, dword [file_start + tmp2]
	mov	curr_data2 %+ d, dword [file_start + tmp2 + 1]
	mov	tmp3 %+ d, dword [file_start + tmp2 + 2]
	mov	tmp1 %+ d, dword [file_start + tmp2 + 3]

	compute_hash	hash, curr_data
	compute_hash	hash2, curr_data2
	compute_hash	hash3, tmp3
	compute_hash	hmask3, tmp1

	and	hash %+ d, hmask2 %+ d
	and	hash2 %+ d, hmask2 %+ d
	and	hash3 %+ d, hmask2 %+ d
	and	hmask3 %+ d, hmask2 %+ d

	mov	[hash_table + 2 * hash], tmp2 %+ w
	add	tmp2, 1
	mov	[hash_table + 2 * hash2], tmp2 %+ w
	add	tmp2, 1
	mov	[hash_table + 2 * hash3], tmp2 %+ w
	add	tmp2, 1
	mov	[hash_table + 2 * hmask3], tmp2 %+ w
%if (LARGE_MATCH_HASH_REP > 1)
	add	tmp2, 1
%endif
%endrep
	; for (f_i = f_start_i; f_i < file_length; f_i++) {
	MOVDQU	xdata, [file_start + f_i]
	mov	curr_data, [file_start + f_i]
	mov	tmp1, curr_data

	compute_hash	hash, curr_data

	shr	tmp1, 8
	compute_hash	hash2, tmp1

	and	hash, hmask2
	and	hash2, hmask2

	jmp	.loop2

.write_first_byte:
	mov	hmask1 %+ d, [rsp + hash_mask_offset]
	cmp	m_out_buf, [rsp + m_out_end]
	ja	.output_end

	mov	byte [stream + _internal_state_has_hist], IGZIP_HIST

	mov	[hash_table + 2 * hash], f_i %+ w

	mov	hash, hash2
	shr	tmp2, 16
	compute_hash	hash2, tmp2

	and	curr_data, 0xff
	inc	dword [lit_len_hist + HIST_ELEM_SIZE*curr_data]
	or	curr_data, LIT

	mov	dword [m_out_buf], curr_data %+ d
	add	m_out_buf, 4

	MOVDQU	xdata, [file_start + f_i + 1]
	add	f_i, 1
	mov	curr_data, [file_start + f_i]
	and	hash %+ d, hmask1 %+ d
	and	hash2 %+ d, hmask1 %+ d

	cmp	f_i, file_length
	jl	.loop2
	jmp	.input_end

%ifdef USE_HSWNI
%undef USE_HSWNI
%endif

;; Shift defines over in order to iterate over all versions
%undef ARCH
%xdefine ARCH ARCH1
%undef ARCH1
%xdefine ARCH1 ARCH2

%ifdef COMPARE_TYPE_NOT_DEF
%undef COMPARE_TYPE
%xdefine COMPARE_TYPE COMPARE_TYPE1
%undef COMPARE_TYPE1
%xdefine COMPARE_TYPE1 COMPARE_TYPE2
%endif
%endrep
min_lit_dist_syms:
	dd LIT + (1 << DIST_OFFSET)

low_lit_shuf:
	db 0x00, 0xff, 0xff, 0xff, 0x02, 0xff, 0xff, 0xff
	db 0x04, 0xff, 0xff, 0xff, 0x06, 0xff, 0xff, 0xff
up_lit_shuf:
	db 0x01, 0xff, 0xff, 0xff, 0x03, 0xff, 0xff, 0xff
	db 0x05, 0xff, 0xff, 0xff, 0x07, 0xff, 0xff, 0xff
