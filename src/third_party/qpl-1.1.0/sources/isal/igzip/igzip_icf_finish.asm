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

%include "stdmac.asm"
%include "reg_sizes.asm"

;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;

%define curr_data	rax
%define tmp1		rax

%define f_index		rbx
%define code		rbx
%define tmp4		rbx
%define tmp5		rbx
%define tmp6		rbx

%define tmp2		rcx
%define hash		rcx

%define tmp3		rdx

%define stream		rsi

%define f_i		rdi

%define code_len2	rbp
%define hmask1		rbp

%define m_out_buf	r8

%define level_buf	r9

%define dist 		r10
%define hmask2		r10

%define code2		r12
%define f_end_i		r12

%define file_start	r13

%define len		r14

%define hufftables	r15

%define hash_table level_buf + _hash8k_hash_table
%define lit_len_hist level_buf + _hist_lit_len
%define dist_hist level_buf + _hist_dist

;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
f_end_i_mem_offset	equ 0    ; local variable (8 bytes)
m_out_end		equ 8
m_out_start		equ 16
dist_mask_offset	equ 24
hash_mask_offset	equ 32
stack_size		equ 5*8

%xdefine METHOD hash_hist

[bits 64]
default rel
section .text

; void isal_deflate_icf_finish ( isal_zstream *stream )
; arg 1: rcx: addr of stream
global isal_deflate_icf_finish_ %+ METHOD %+ _01
isal_deflate_icf_finish_ %+ METHOD %+ _01:
	endbranch
	PUSH_ALL	rbx, rsi, rdi, rbp, r12, r13, r14, r15
	sub	rsp, stack_size

%ifidn __OUTPUT_FORMAT__, win64
	mov	stream, rcx
%else
	mov	stream, rdi
%endif

	; state->bitbuf.set_buf(stream->next_out, stream->avail_out);
	mov	tmp2 %+ d, dword [stream + _internal_state_dist_mask]
	mov	tmp3 %+ d, dword [stream + _internal_state_hash_mask]
	mov	level_buf, [stream + _level_buf]
	mov	m_out_buf, [level_buf + _icf_buf_next]
	mov	[rsp + m_out_start], m_out_buf
	mov	tmp1, [level_buf + _icf_buf_avail_out]
	add	tmp1, m_out_buf
	sub	tmp1, 4

	mov     [rsp + dist_mask_offset], tmp2
	mov	[rsp + hash_mask_offset], tmp3
	mov	[rsp + m_out_end], tmp1

	mov	hufftables, [stream + _hufftables]

	mov	file_start, [stream + _next_in]

	mov	f_i %+ d, dword [stream + _total_in]
	sub	file_start, f_i

	mov	f_end_i %+ d, dword [stream + _avail_in]
	add	f_end_i, f_i

	sub	f_end_i, LAST_BYTES_COUNT
	mov	[rsp + f_end_i_mem_offset], f_end_i
	; for (f_i = f_start_i; f_i < f_end_i; f_i++) {
	cmp	f_i, f_end_i
	jge	.end_loop_2

	mov	curr_data %+ d, [file_start + f_i]

	cmp	byte [stream + _internal_state_has_hist], IGZIP_NO_HIST
	jne	.skip_write_first_byte

	cmp	m_out_buf, [rsp + m_out_end]
	ja	.end_loop_2

	mov	hmask1 %+ d, [rsp + hash_mask_offset]
	compute_hash	hash, curr_data
	and	hash %+ d, hmask1 %+ d
	mov	[hash_table + 2 * hash], f_i %+ w
	mov	byte [stream + _internal_state_has_hist], IGZIP_HIST
	jmp	.encode_literal

.skip_write_first_byte:

.loop2:
	mov	tmp3 %+ d, [rsp + dist_mask_offset]
	mov	hmask1 %+ d, [rsp + hash_mask_offset]
	; if (state->bitbuf.is_full()) {
	cmp	m_out_buf, [rsp + m_out_end]
	ja	.end_loop_2

	; hash = compute_hash(state->file_start + f_i) & hash_mask;
	mov	curr_data %+ d, [file_start + f_i]
	compute_hash	hash, curr_data
	and	hash %+ d, hmask1 %+ d

	; f_index = state->head[hash];
	movzx	f_index %+ d, word [hash_table + 2 * hash]

	; state->head[hash] = (uint16_t) f_i;
	mov	[hash_table + 2 * hash], f_i %+ w

	; dist = f_i - f_index; // mod 64k
	mov	dist %+ d, f_i %+ d
	sub	dist %+ d, f_index %+ d
	and	dist %+ d, 0xFFFF

	; if ((dist-1) <= (D-1)) {
	mov	tmp1 %+ d, dist %+ d
	sub	tmp1 %+ d, 1
	cmp	tmp1 %+ d, tmp3 %+ d
	jae	.encode_literal

	; len = f_end_i - f_i;
	mov	tmp4, [rsp + f_end_i_mem_offset]
	sub	tmp4, f_i
	add	tmp4, LAST_BYTES_COUNT

	; if (len > 258) len = 258;
	cmp	tmp4, 258
	cmovg	tmp4, [c258]

	; len = compare(state->file_start + f_i,
	;               state->file_start + f_i - dist, len);
	lea	tmp1, [file_start + f_i]
	mov	tmp2, tmp1
	sub	tmp2, dist
	compare	tmp4, tmp1, tmp2, len, tmp3

	; if (len >= SHORTEST_MATCH) {
	cmp	len, SHORTEST_MATCH
	jb	.encode_literal

	;; encode as dist/len

	; get_dist_code(dist, &code2, &code_len2);
	dec	dist
	get_dist_icf_code	dist, code2, tmp3 ;; clobbers dist, rcx

	;; get_len_code
	lea	code, [len + 254]

	mov	hmask2 %+ d, [rsp + hash_mask_offset]

	or	code2, code
	inc	dword [lit_len_hist + HIST_ELEM_SIZE*code]

	; for (k = f_i+1, f_i += len-1; k <= f_i; k++) {
	lea	tmp3, [f_i + 1]	; tmp3 <= k
	add	f_i, len
	cmp	f_i, [rsp + f_end_i_mem_offset]
	jae	.skip_hash_update

	; only update hash twice

	; hash = compute_hash(state->file_start + k) & hash_mask;
	mov	tmp6 %+ d, dword [file_start + tmp3]
	compute_hash	hash, tmp6
	and	hash %+ d, hmask2 %+ d
	; state->head[hash] = k;
	mov	[hash_table + 2 * hash], tmp3 %+ w

	add	tmp3, 1

	; hash = compute_hash(state->file_start + k) & hash_mask;
	mov	tmp6 %+ d, dword [file_start + tmp3]
	compute_hash	hash, tmp6
	and	hash %+ d, hmask2 %+ d
	; state->head[hash] = k;
	mov	[hash_table + 2 * hash], tmp3 %+ w

.skip_hash_update:
	write_dword	code2, m_out_buf
	shr	code2, DIST_OFFSET
	and	code2, 0x1F
	inc	dword [dist_hist + HIST_ELEM_SIZE*code2]
	; continue
	cmp	f_i, [rsp + f_end_i_mem_offset]
	jl	.loop2
	jmp	.end_loop_2

.encode_literal:
	; get_lit_code(state->file_start[f_i], &code2, &code_len2);
	movzx	tmp5, byte [file_start + f_i]
	inc	dword [lit_len_hist + HIST_ELEM_SIZE*tmp5]
	or	tmp5, LIT
	write_dword	tmp5, m_out_buf
	; continue
	add	f_i, 1
	cmp	f_i, [rsp + f_end_i_mem_offset]
	jl	.loop2

.end_loop_2:
	mov	f_end_i, [rsp + f_end_i_mem_offset]
	add	f_end_i, LAST_BYTES_COUNT
	mov	[rsp + f_end_i_mem_offset], f_end_i
	; if ((f_i >= f_end_i) && ! state->bitbuf.is_full()) {
	cmp	f_i, f_end_i
	jge	.input_end

	xor	tmp5, tmp5
.final_bytes:
	cmp	m_out_buf, [rsp + m_out_end]
	ja	.out_end

	movzx	tmp5, byte [file_start + f_i]
	inc	dword [lit_len_hist + HIST_ELEM_SIZE*tmp5]
	or	tmp5, LIT
	write_dword	tmp5, m_out_buf

	inc	f_i
	cmp	f_i, [rsp + f_end_i_mem_offset]
	jl	.final_bytes

.input_end:
	cmp	word [stream + _end_of_stream], 0
	jne	.out_end
	cmp	word [stream + _flush], _NO_FLUSH
	jne	.out_end
	jmp .end

.out_end:
	mov	dword [stream + _internal_state_state], ZSTATE_CREATE_HDR
.end:
	;; Update input buffer
	mov	f_end_i, [rsp + f_end_i_mem_offset]
	mov	[stream + _total_in], f_i %+ d
	mov	[stream + _internal_state_block_end], f_i %+ d

	add	file_start, f_i
	mov	[stream + _next_in], file_start
	sub	f_end_i, f_i
	mov	[stream + _avail_in], f_end_i %+ d

	;; Update output buffer
	mov	[level_buf + _icf_buf_next], m_out_buf

	;    len = state->bitbuf.buffer_used();
	sub	m_out_buf, [rsp + m_out_start]

	;    stream->avail_out -= len;
	sub	[level_buf + _icf_buf_avail_out], m_out_buf

	add	rsp, stack_size
	POP_ALL
	ret

section .data
	align 4
c258:	dq	258
