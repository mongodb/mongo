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

%define LARGE_MATCH_HASH_REP 1  ; Hash 4 * LARGE_MATCH_HASH_REP elements
%define LARGE_MATCH_MIN 264 	; Minimum match size to enter large match emit loop
%define MIN_INBUF_PADDING 16
%define MAX_EMIT_SIZE 258 * 16
;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;

%define	tmp2		rcx
%define	hash2		rcx

%define	curr_data	rax
%define	code		rax
%define	tmp5		rax

%define	tmp4		rbx
%define	dist		rbx
%define	code2		rbx
%define hmask1		rbx

%define	hash		rdx
%define	len		rdx
%define	code_len3	rdx
%define	tmp8		rdx

%define	tmp1		rsi
%define	code_len2	rsi

%define	file_start	rdi

%define	m_bit_count	rbp

%define	curr_data2	r8
%define	len2		r8
%define	tmp6		r8
%define	f_end_i		r8

%define	m_bits		r9

%define	f_i		r10

%define	m_out_buf	r11

%define	dist2		r12
%define	tmp7		r12
%define	code4		r12

%define	tmp3		r13
%define	code3		r13

%define	stream		r14

%define	hufftables	r15

;; GPR r8 & r15 can be used

%define xtmp0		xmm0	; tmp
%define xtmp1		xmm1	; tmp
%define	xhash		xmm2
%define	xmask		xmm3
%define	xdata		xmm4

%define ytmp0		ymm0	; tmp
%define ytmp1		ymm1	; tmp


;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;


blen_mem_offset     equ  0	 ; local variable (8 bytes)
f_end_i_mem_offset  equ  8
inbuf_slop_offset    equ 16
gpr_save_mem_offset equ 32       ; gpr save area (8*8 bytes)
xmm_save_mem_offset equ 32 + 8*8 ; xmm save area (4*16 bytes) (16 byte aligned)
stack_size          equ 4*8 + 8*8 + 4*16 + 8
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

%rep 3
%if ARCH == 04
%define USE_HSWNI
%endif

[bits 64]
default rel
section .text

; void isal_deflate_body ( isal_zstream *stream )
; arg 1: rcx: addr of stream
global isal_deflate_body_ %+ ARCH
isal_deflate_body_ %+ ARCH %+ :
	endbranch
%ifidn __OUTPUT_FORMAT__, elf64
	mov	rcx, rdi
%endif

	;; do nothing if (avail_in == 0)
	cmp	dword [rcx + _avail_in], 0
	jne	.skip1

	;; Set stream's next state
	mov	rdx, ZSTATE_FLUSH_READ_BUFFER
	mov	rax, ZSTATE_BODY
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
	mov	byte [stream + _internal_state_has_eob], 0

	MOVD	xmask, [stream + _internal_state_hash_mask]
	PSHUFD	xmask, xmask, 0

	; state->bitbuf.set_buf(stream->next_out, stream->avail_out);
	mov	m_out_buf, [stream + _next_out]
	mov	[stream + _internal_state_bitbuf_m_out_start], m_out_buf
	mov	tmp1 %+ d, [stream + _avail_out]
	add	tmp1, m_out_buf
	sub	tmp1, SLOP

	mov	[stream + _internal_state_bitbuf_m_out_end], tmp1

	mov	m_bits,           [stream + _internal_state_bitbuf_m_bits]
	mov	m_bit_count %+ d, [stream + _internal_state_bitbuf_m_bit_count]
	mov	hufftables, [stream + _hufftables]

	mov	file_start, [stream + _next_in]

	mov	f_i %+ d, dword [stream + _total_in]
	sub	file_start, f_i

	mov	f_end_i %+ d, [stream + _avail_in]
	add	f_end_i, f_i

	mov	qword [rsp + inbuf_slop_offset], MIN_INBUF_PADDING
	cmp	byte [stream + _end_of_stream], 0
	jnz	.default_inbuf_padding
	cmp	byte [stream + _flush], 0
	jnz	.default_inbuf_padding
	mov	qword [rsp + inbuf_slop_offset], LA
.default_inbuf_padding:

	; f_end_i -= INBUF_PADDING;
	sub	f_end_i, [rsp + inbuf_slop_offset]
	mov	[rsp + f_end_i_mem_offset], f_end_i
	; if (f_end_i <= 0) continue;

	cmp	f_end_i, f_i
	jle	.input_end

	MOVD	hmask1 %+ d, xmask
	; for (f_i = f_start_i; f_i < f_end_i; f_i++) {
	MOVDQU	xdata, [file_start + f_i]
	mov	curr_data, [file_start + f_i]
	mov	tmp3, curr_data
	mov	tmp6, curr_data

	compute_hash	hash, curr_data

	shr	tmp3, 8
	compute_hash	hash2, tmp3

	and	hash %+ d, hmask1 %+ d
	and	hash2 %+ d, hmask1 %+ d

	cmp	byte [stream + _internal_state_has_hist], IGZIP_NO_HIST
	je	.write_first_byte

	jmp	.loop2
	align	16

.loop2:
	mov	tmp3 %+ d, dword [stream + _internal_state_dist_mask]

	; if (state->bitbuf.is_full()) {
	cmp	m_out_buf, [stream + _internal_state_bitbuf_m_out_end]
	ja	.output_end

	xor	dist, dist
	xor	dist2, dist2

	lea	tmp1, [file_start + f_i]

	mov	dist %+ w, f_i %+ w
	dec	dist
	sub	dist %+ w, word [stream + _internal_state_head + 2 * hash]
	mov	[stream + _internal_state_head + 2 * hash], f_i %+ w

%ifdef QPL_LIB
	mov	tmp6, f_i
	and	tmp6 %+ d, [stream + _internal_state_mb_mask]
	; Do we need to set _internal_state_max_dist ?
    ; mov	[stream + _internal_state_max_dist], tmp6 %+ d
.inc_f_i:
%endif
	inc	f_i
%ifdef QPL_LIB
	mov	tmp6, f_i
	and	tmp6 %+ d, [stream + _internal_state_mb_mask]
	dec	tmp6
	cmp	tmp6 %+ d, [stream + _internal_state_max_dist]
	jz	.inc_max_dist1
;	db 0cch
; increment max_dist
.inc_max_dist1:
;	mov	tmp6 %+ d, [stream + _internal_state_max_dist]
;        add	tmp6, 1
;	and	tmp6 %+ d, [stream + _internal_state_mb_mask]
;	mov	[stream + _internal_state_max_dist], tmp6 %+ d
%endif

	MOVQ	tmp6, xdata
	shr	tmp5, 16
	mov	tmp8, tmp5
	compute_hash	tmp6, tmp5

	mov	dist2 %+ w, f_i %+ w
	dec	dist2
	sub	dist2 %+ w, word [stream + _internal_state_head + 2 * hash2]
	mov	[stream + _internal_state_head + 2 * hash2], f_i %+ w

	; if ((dist-1) < (D-1)) {
	and	dist, tmp3
	neg	dist

	shr	tmp8, 8
	compute_hash	tmp2, tmp8

	and	dist2, tmp3
	neg	dist2

	;; Check for long len/dist match (>7) with first literal
	MOVQ	len, xdata
	mov	curr_data, len
	PSRLDQ	xdata, 1
%ifdef QPL_LIB
	;
	; if dist is inappropriate, spoil results of test and goto check lit2
	;
	; Check dist validity (tmp2 is temp)
.check_dist:
	mov	tmp2 %+ d, [stream + _internal_state_max_dist]
	sub	tmp2, 3
	add	tmp2, dist	; dist is negative!
	jns	.check_1	; dist is ok goto compare
	mov	len, 1		; spoil len to go to literal coding
	jmp	.lit1_not_match
.check_1:
%endif
	xor	len, [tmp1 + dist - 1]
%ifdef QPL_LIB
	jnz	.my_check_1
	; check if 8 bytes more fit into miniblock
	mov	tmp2 %+ d, [stream + _internal_state_max_dist]
	add	tmp2, 8
	cmp	tmp2 %+ d, [stream + _internal_state_mb_mask]
	jl      .compare_loop
	mov	len, 1
	jmp     .lit1_not_match
%else
	jz	.compare_loop
%endif

%ifdef QPL_LIB
.my_check_1:
	; check if smaller match
	test	len %+ d, 0xFFFFFFFF
	jnz	.lit1_not_match		; ok no small match
	; check if small match ok for miniblock
	bsf	tmp2, len
	shr	tmp2, 3
	inc tmp2        ; add 1 byte for sure
	add	tmp2 %+ d, [stream + _internal_state_max_dist]
	cmp	tmp2 %+ d, [stream + _internal_state_mb_mask]
	jl   	.lit1_not_match	; small match ok for miniblock
	mov	len, 1		; not ok
%endif

.lit1_not_match:

	MOVD	xhash, tmp6 %+ d
	PINSRD	xhash, tmp2 %+ d, 1
	PAND	xhash, xhash, xmask
%ifdef QPL_LIB
.check_dist2:
	mov	tmp2 %+ d, [stream + _internal_state_max_dist]
	sub	tmp2, 2
	add	tmp2, dist2	; dist2 is negative!
	jns	.check_2	; dist2 is ok goto compare
	mov	len2, 1		; spoil len2 to go to literal coding
	jmp 	.lit2_not_match
%endif

.check_2:

	;; Check for len/dist match (>7) with second literal
	MOVQ	len2, xdata
	xor	len2, [tmp1 + dist2]

%ifdef QPL_LIB
.my_check_2:
	; check if smaller match
	test	len2 %+ d, 0xFFFFFFFF
	jnz	.lit2_not_match		; ok no small match
	; check if small match ok for miniblock
	bsf	tmp2, len2
	shr	tmp2, 3
	inc tmp2        ; add 1 byte for sure
	add	tmp2 %+ d, [stream + _internal_state_max_dist]
	cmp	tmp2 %+ d, [stream + _internal_state_mb_mask]
	jl   	.lit2_not_match	; small match ok for miniblock
	mov	len2, 1		; not ok
%else
	jz	.compare_loop2
%endif

.lit2_not_match:

	;; Specutively load the code for the first literal
	movzx   tmp1, curr_data %+ b
	get_lit_code    tmp1, code3, rcx, hufftables

	;; Check for len/dist match for first literal
	test    len %+ d, 0xFFFFFFFF
	jz      .len_dist_huffman_pre

	;; Specutively load the code for the second literal
	shr     curr_data, 8
	and     curr_data, 0xff
	get_lit_code    curr_data, code2, code_len2, hufftables

	SHLX    code2, code2, rcx
	or      code2, code3
	add     code_len2, rcx

	;; Check for len/dist match for second literal
	test    len2 %+ d, 0xFFFFFFFF
	jnz     .write_lit_bits
%ifdef QPL_LIB
	;
	; Check if we are at mini-block boundary-4 bytes
	; tmp2 is used
	mov	tmp2 %+ d, [stream + _internal_state_mb_mask]
	add	tmp2, 1	;; f_i (and max_dist is 1 byte forward)
	sub	tmp2 %+ d, [stream + _internal_state_max_dist]
	cmp	tmp2, 4
	jns 	.write_lit_bits
%endif
.len_dist_lit_huffman_pre:
	mov     code_len3, rcx
	bsf	len2, len2
	shr	len2, 3

.len_dist_lit_huffman:
	neg	dist2

%ifndef LONGER_HUFFTABLE
	mov	tmp4, dist2
	get_dist_code	tmp4, code4, code_len2, hufftables ;; clobbers dist, rcx
%else
	get_dist_code	dist2, code4, code_len2, hufftables
%endif
	get_len_code	len2, code, rcx, hufftables ;; rcx is code_len

	MOVD	hmask1 %+ d, xmask

	SHLX	code4, code4, rcx
	or	code4, code
	add	code_len2, rcx

	add	f_i, len2
%ifdef QPL_LIB
;-------------------------------
	; update max_dist
.inc_max_dist2:
	mov	tmp5 %+ d, [stream + _internal_state_max_dist]
        add	tmp5, len2
	and	tmp5 %+ d, [stream + _internal_state_mb_mask]
	mov	[stream + _internal_state_max_dist], tmp5 %+ d
;-------------------------------
%endif
	neg	len2

	SHLX	code4, code4, code_len3

	MOVQ	tmp5, xdata
	shr	tmp5, 24
	compute_hash	hash2, tmp5
	and	hash2 %+ d, hmask1 %+ d

	or	code4, code3
	add	code_len2, code_len3

	;; Setup for updating hash
	lea	tmp3, [f_i + len2 + 1]	; tmp3 <= k

	mov	tmp6, [rsp + f_end_i_mem_offset]
	cmp	f_i, tmp6
	jge	.len_dist_lit_huffman_finish

	MOVDQU	xdata, [file_start + f_i]
	mov	curr_data, [file_start + f_i]

	MOVD	hash %+ d, xhash
	PEXTRD	tmp6 %+ d, xhash, 1
	mov	[stream + _internal_state_head + 2 * hash], tmp3 %+ w

	compute_hash	hash, curr_data

	add	tmp3,1
	mov	[stream + _internal_state_head + 2 * tmp6], tmp3 %+ w

	add	tmp3, 1
	mov	[stream + _internal_state_head + 2 * hash2], tmp3 %+ w

	write_bits	m_bits, m_bit_count, code4, code_len2, m_out_buf

	mov	curr_data2, curr_data
	shr	curr_data2, 8
	compute_hash	hash2, curr_data2

%ifdef	NO_LIMIT_HASH_UPDATE
.loop3:
	add     tmp3,1
	cmp	tmp3, f_i
	jae	.loop3_done
	mov     tmp6, [file_start + tmp3]
	compute_hash    tmp1, tmp6
	and     tmp1 %+ d, hmask1 %+ d
	; state->head[hash] = k;
	mov     [stream + _internal_state_head + 2 * tmp1], tmp3 %+ w
	jmp      .loop3
.loop3_done:
%endif
	; hash = compute_hash(state->file_start + f_i) & hash_mask;
	and	hash %+ d, hmask1 %+ d
	and	hash2 %+ d, hmask1 %+ d

	; continue
	jmp	.loop2
	;; encode as dist/len
.len_dist_lit_huffman_finish:
	MOVD	hash %+ d, xhash
	PEXTRD	tmp6 %+ d, xhash, 1
	mov	[stream + _internal_state_head + 2 * hash], tmp3 %+ w
	add	tmp3,1
	mov	[stream + _internal_state_head + 2 * tmp6], tmp3 %+ w
	add	tmp3, 1
	mov	[stream + _internal_state_head + 2 * hash2], tmp3 %+ w

	write_bits	m_bits, m_bit_count, code4, code_len2, m_out_buf
	jmp	.input_end

align	16
.len_dist_huffman_pre:
	bsf	len, len
	shr	len, 3

.len_dist_huffman:
	dec	f_i
	neg	dist
%ifdef QPL_LIB
	; decrement max_dist (tmp3 used)
.inc_max_dist3:
	mov	tmp3 %+ d, [stream + _internal_state_max_dist]
        dec	tmp3
	and	tmp3 %+ d, [stream + _internal_state_mb_mask]
	mov	[stream + _internal_state_max_dist], tmp3 %+ d
%endif
	; get_dist_code(dist, &code2, &code_len2);
%ifndef LONGER_HUFFTABLE
	mov tmp3, dist	; since code2 and dist are rbx
	get_dist_code	tmp3, code2, code_len2, hufftables ;; clobbers dist, rcx
%else
	get_dist_code	dist, code2, code_len2, hufftables
%endif
	; get_len_code(len, &code, &code_len);
	get_len_code	len, code, rcx, hufftables ;; rcx is code_len

	; code2 <<= code_len
	; code2 |= code
	; code_len2 += code_len
	SHLX	code4, code2, rcx
	or	code4, code
	add	code_len2, rcx

	;; Setup for updateing hash
	lea	tmp3, [f_i + 2]	; tmp3 <= k
	add	f_i, len

%ifdef QPL_LIB
	; update max_dist
.inc_max_dist4:
	mov	rbx %+ d, [stream + _internal_state_max_dist]
        add	rbx, len
	and	rbx %+ d, [stream + _internal_state_mb_mask]
	mov	[stream + _internal_state_max_dist], rbx %+ d
%endif
	MOVD	hash %+ d, xhash
	PEXTRD	hash2 %+ d, xhash, 1
	mov	[stream + _internal_state_head + 2 * hash], tmp3 %+ w
	add	tmp3,1
	mov	[stream + _internal_state_head + 2 * hash2], tmp3 %+ w

	MOVD	hmask1 %+ d, xmask

	cmp	f_i, [rsp + f_end_i_mem_offset]
	jge	.len_dist_huffman_finish

	MOVDQU	xdata, [file_start + f_i]
	mov	curr_data, [file_start + f_i]
	compute_hash	hash, curr_data

	write_bits	m_bits, m_bit_count, code4, code_len2, m_out_buf

	mov	curr_data2, curr_data
	shr	curr_data2, 8
	compute_hash	hash2, curr_data2

%ifdef	NO_LIMIT_HASH_UPDATE
.loop4:
	add     tmp3,1
	cmp	tmp3, f_i
	jae	.loop4_done
	mov     tmp6, [file_start + tmp3]
	compute_hash    tmp1, tmp6
	and     tmp1 %+ d, hmask1 %+ d
	mov     [stream + _internal_state_head + 2 * tmp1], tmp3 %+ w
	jmp     .loop4
.loop4_done:
%endif

	; hash = compute_hash(state->file_start + f_i) & hash_mask;
	and	hash %+ d, hmask1 %+ d
	and	hash2 %+ d, hmask1 %+ d

	; continue
	jmp	.loop2

.len_dist_huffman_finish:
	write_bits	m_bits, m_bit_count, code4, code_len2, m_out_buf
	jmp	.input_end

align	16
.write_lit_bits:
	PSRLDQ	xdata, 1

	add	f_i, 1
%ifdef QPL_LIB
	; update max_dist
.inc_max_dist5:
	mov	rcx %+ d, [stream + _internal_state_max_dist]
	add	rcx, 1
	and	rcx %+ d, [stream + _internal_state_mb_mask]
	mov	[stream + _internal_state_max_dist], rcx %+ d
%endif
	cmp     f_i, [rsp + f_end_i_mem_offset]
	jge     .write_lit_bits_finish

	MOVQ	curr_data, xdata
	MOVDQU	xdata, [file_start + f_i]

	MOVD	hash %+ d, xhash

	write_bits	m_bits, m_bit_count, code2, code_len2, m_out_buf

	PEXTRD	hash2 %+ d, xhash, 1
	jmp      .loop2

.write_lit_bits_finish:
	write_bits	m_bits, m_bit_count, code2, code_len2, m_out_buf

.input_end:
	mov	tmp1, ZSTATE_FLUSH_READ_BUFFER
	mov	tmp5, ZSTATE_BODY
	cmp	word [stream + _end_of_stream], 0
	cmovne	tmp5, tmp1
	cmp	word [stream + _flush], _NO_FLUSH
	cmovne	tmp5, tmp1
	mov	dword [stream + _internal_state_state], tmp5 %+ d

.output_end:
	;; update input buffer
	mov	f_end_i, [rsp + f_end_i_mem_offset]
	add	f_end_i, [rsp + inbuf_slop_offset]
	mov	[stream + _total_in], f_i %+ d
	add	file_start, f_i
	mov     [stream + _next_in], file_start
	sub	f_end_i, f_i
	mov     [stream + _avail_in], f_end_i %+ d

	;; update output buffer
	mov	[stream + _next_out], m_out_buf
	sub	m_out_buf, [stream + _internal_state_bitbuf_m_out_start]
	sub	[stream + _avail_out], m_out_buf %+ d
	add	[stream + _total_out], m_out_buf %+ d

	mov	[stream + _internal_state_bitbuf_m_bits], m_bits
	mov	[stream + _internal_state_bitbuf_m_bit_count], m_bit_count %+ d

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

align	16
.compare_loop:
	MOVD	xhash, tmp6 %+ d
	PINSRD	xhash, tmp2 %+ d, 1
	PAND	xhash, xhash, xmask
	lea	tmp2, [tmp1 + dist - 1]

	mov	len2, [rsp + f_end_i_mem_offset]
	sub	len2, f_i
	add	len2, [rsp + inbuf_slop_offset]
	add	len2, 1
	mov	tmp3,  MAX_EMIT_SIZE
	cmp	len2, tmp3
	cmovg	len2, tmp3

%ifdef QPL_LIB
.set_limit_mb_boundary_1:
	; tmp3 is used
	mov	tmp3 %+ d, [stream + _internal_state_mb_mask]
	add	tmp3, 1
	sub	tmp3 %+ d, [stream + _internal_state_max_dist]
	cmp	len2, tmp3
	cmovge	len2, tmp3
%endif

	mov	len, 8
	compare_large	tmp1, tmp2, len, len2, tmp3, ytmp0, ytmp1

	cmp	len, 258
	jle	.len_dist_huffman
	cmp	len, LARGE_MATCH_MIN
	jge	.do_emit
	mov	len, 258
	jmp	.len_dist_huffman

align	16
.compare_loop2:
	lea	tmp2, [tmp1 + dist2]
	add	tmp1, 1

	mov	len, [rsp + f_end_i_mem_offset]
	sub	len, f_i
	add	len, [rsp + inbuf_slop_offset]
	mov	tmp3,  MAX_EMIT_SIZE
	cmp	len, tmp3
	cmovg	len, tmp3

%ifdef QPL_LIB
.set_limit_mb_boundary_2:
	; tmp3 is used
	mov	tmp3 %+ d, [stream + _internal_state_mb_mask]
	add	tmp3, 1
	sub	tmp3 %+ d, [stream + _internal_state_max_dist]
	cmp	len, tmp3
	cmovge	len, tmp3
%endif

	mov	len2, 8
	compare_large	tmp1, tmp2, len2, len, tmp3, ytmp0, ytmp1

	and	curr_data, 0xff
	get_lit_code	curr_data, code3, code_len3, hufftables
	cmp	len2, 258
	jle	.len_dist_lit_huffman
	cmp	len2, LARGE_MATCH_MIN
	jge	.do_emit2
	mov	len2, 258
	jmp	.len_dist_lit_huffman

align	16
.do_emit2:
	neg	dist2

	; get_dist_code(dist2, &code2, &code_len2);
	get_dist_code	dist2, code2, code_len2, hufftables

	; get_len_code(len, &code, &code_len);
	get_len_code	258, code, rcx, hufftables ;; rcx is code_len

	; code2 <<= code_len
	; code2 |= code
	; code_len2 += code_len
	SHLX	code4, code2, rcx
	or	code4, code
	add	code_len2, rcx
	mov	tmp5, rcx

	mov	rcx, code_len3
	SHLX	tmp8, code4, rcx
	or	code3, tmp8
	add	rcx, code_len2
	mov	code_len3, rcx

	write_bits	m_bits, m_bit_count, code3, code_len3, m_out_buf

	lea	tmp3, [f_i + 2]	; tmp3 <= k
	MOVD	tmp2 %+ d, xhash
	mov	[stream + _internal_state_head + 2 * tmp2], tmp3 %+ w
	add	tmp3,1
	PEXTRD	tmp2 %+ d, xhash, 1
	mov	[stream + _internal_state_head + 2 * tmp2], tmp3 %+ w

	add	f_i, 258
	lea	len, [len2 - 258]

	jmp	.emit_loop

.do_emit:
	dec	f_i
	neg	dist

	; get_dist_code(dist, &code2, &code_len2);
%ifndef LONGER_HUFFTABLE
	mov tmp3, dist	; since code2 and dist are rbx
	get_dist_code	tmp3, code2, code_len2, hufftables ;; clobbers dist, rcx
%else
	get_dist_code	dist, code2, code_len2, hufftables
%endif
	; get_len_code(len, &code, &code_len);
	get_len_code	258, code, rcx, hufftables ;; rcx is code_len

	; code2 <<= code_len
	; code2 |= code
	; code_len2 += code_len
	SHLX	code4, code2, rcx
	or	code4, code
	add	code_len2, rcx

	lea	tmp3, [f_i + 2]	; tmp3 <= k
	MOVD	tmp6 %+ d, xhash
	PEXTRD	tmp5 %+ d, xhash, 1
	mov	[stream + _internal_state_head + 2 * tmp6], tmp3 %+ w
	add	tmp3,1
	mov	[stream + _internal_state_head + 2 * tmp5], tmp3 %+ w
	mov	tmp5, rcx

.emit:
	add	f_i, 258
	sub	len, 258
	mov	code3, code4

	write_bits	m_bits, m_bit_count, code3, code_len2, m_out_buf

.emit_loop:
	cmp	m_out_buf, [stream + _internal_state_bitbuf_m_out_end]
	ja	.output_end
	cmp	len, LARGE_MATCH_MIN
	jge	.emit

	mov	len2, 258
	cmp	len, len2
	cmovg	len, len2

	add	f_i, len

%ifdef QPL_LIB
	; update max_dist (rcx as temporary)
.inc_max_dist6:
	mov	rcx %+ d, [stream + _internal_state_max_dist]
        add	rcx , len
	and	rcx %+ d, [stream + _internal_state_mb_mask]
	mov	[stream + _internal_state_max_dist], rcx %+ d
%endif

	sub	code_len2, tmp5
	get_len_code	len, code, rcx, hufftables
	SHLX	code4, code2, rcx
	or	code4, code
	add	code_len2, rcx

	write_bits	m_bits, m_bit_count, code4, code_len2, m_out_buf

	cmp	f_i, [rsp + f_end_i_mem_offset]
	jge	.input_end

	lea	tmp7, [f_i - 4 * LARGE_MATCH_HASH_REP]
	MOVD	hmask1 %+ d, xmask
%rep LARGE_MATCH_HASH_REP
	mov	curr_data %+ d, dword [file_start + tmp7]
	mov	curr_data2 %+ d, dword [file_start + tmp7 + 1]

	compute_hash	hash, curr_data
	compute_hash	hash2, curr_data2

	and	hash %+ d, hmask1 %+ d
	and	hash2 %+ d, hmask1 %+ d

	mov	[stream + _internal_state_head + 2 * hash], tmp7 %+ w
	add	tmp7, 1
	mov	[stream + _internal_state_head + 2 * hash2], tmp7 %+ w
	add	tmp7, 1

	mov	curr_data %+ d, dword [file_start + tmp7]
	mov	curr_data2 %+ d, dword [file_start + tmp7 + 1]

	compute_hash	hash, curr_data
	compute_hash	hash2, curr_data2

	and	hash %+ d, hmask1 %+ d
	and	hash2 %+ d, hmask1 %+ d

	mov	[stream + _internal_state_head + 2 * hash], tmp7 %+ w
	add	tmp7, 1
	mov	[stream + _internal_state_head + 2 * hash2], tmp7 %+ w
%if (LARGE_MATCH_HASH_REP > 1)
	add	tmp7, 1
%endif
%endrep

	MOVDQU	xdata, [file_start + f_i]
	mov	curr_data, [file_start + f_i]
	compute_hash	hash, curr_data


	mov	curr_data2, curr_data
	shr	curr_data2, 8
	compute_hash	hash2, curr_data2

	; hash = compute_hash(state->file_start + f_i) & hash_mask;
	and	hash %+ d, hmask1 %+ d
	and	hash2 %+ d, hmask1 %+ d

	; continue
	jmp	.loop2

.write_first_byte:
	cmp	m_out_buf, [stream + _internal_state_bitbuf_m_out_end]
	ja	.output_end

	mov	byte [stream + _internal_state_has_hist], IGZIP_HIST

	mov	[stream + _internal_state_head + 2 * hash], f_i %+ w

	mov	hash, hash2
	shr	tmp6, 16
	compute_hash	hash2, tmp6

	MOVD	xhash, hash %+ d
	PINSRD	xhash, hash2 %+ d, 1
	PAND	xhash, xhash, xmask

	and	curr_data, 0xff
	get_lit_code	curr_data, code2, code_len2, hufftables
	jmp	.write_lit_bits

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
