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

extern rfc1951_lookup_table
_len_to_code_offset	equ	0

%define LAST_BYTES_COUNT	3 ; Bytes to prevent reading out of array bounds
%define LA_STATELESS	280	  ; Max number of bytes read in loop2 rounded up to 8 byte boundary
%define LIT_LEN 286
%define DIST_LEN 30
%define HIST_ELEM_SIZE	8

%ifdef DEBUG
%macro MARK 1
global %1
%1:
%endm
%else
%macro MARK 1
%endm
%endif

;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
%define	file_start	rdi
%define file_length	rsi
%define	histogram	rdx
%define rfc_lookup	r9
%define	f_i		r10

%define	curr_data	rax

%define	tmp2		rcx

%define	dist		rbx
%define	dist_code2	rbx

%define	dist2		r12
%define	dist_code	r12

%define	len		rbp
%define	len_code	rbp
%define	hash3		rbp

%define	curr_data2	r8
%define	len2		r8
%define	tmp4		r8

%define	tmp1		r11

%define	tmp3		r13

%define	hash		r14

%define	hash2		r15

%define	xtmp0		xmm0
%define	xtmp1		xmm1
%define	xdata		xmm2

%define	ytmp0		ymm0
%define	ytmp1		ymm1

%if(ARCH == 01)
%define	vtmp0	xtmp0
%define	vtmp1	xtmp1
%define	V_LENGTH	16
%else
%define	vtmp0	ytmp0
%define	vtmp1	ytmp1
%define	V_LENGTH	32
%endif
;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
_eob_count_offset   equ  0	 ; local variable (8 bytes)
f_end_i_mem_offset  equ  8
gpr_save_mem_offset equ 16       ; gpr save area (8*8 bytes)
xmm_save_mem_offset equ 16 + 8*8 ; xmm save area (4*16 bytes) (16 byte aligned)
stack_size          equ 2*8 + 8*8 + 4*16 + 8
;;; 8 because stack address is odd multiple of 8 after a function call and
;;; we want it aligned to 16 bytes

%ifidn __OUTPUT_FORMAT__, elf64
%define arg0	rdi
%define	arg1	rsi
%define arg2	rdx

%macro FUNC_SAVE 0
%ifdef ALIGN_STACK
	push	rbp
	mov	rbp, rsp
	sub	rsp, stack_size
	and	rsp, ~15
%else
	sub	rsp, stack_size
%endif

	mov [rsp + gpr_save_mem_offset + 0*8], rbx
	mov [rsp + gpr_save_mem_offset + 1*8], rbp
	mov [rsp + gpr_save_mem_offset + 2*8], r12
	mov [rsp + gpr_save_mem_offset + 3*8], r13
	mov [rsp + gpr_save_mem_offset + 4*8], r14
	mov [rsp + gpr_save_mem_offset + 5*8], r15
%endm

%macro FUNC_RESTORE 0
	mov	rbx, [rsp + gpr_save_mem_offset + 0*8]
	mov	rbp, [rsp + gpr_save_mem_offset + 1*8]
	mov	r12, [rsp + gpr_save_mem_offset + 2*8]
	mov	r13, [rsp + gpr_save_mem_offset + 3*8]
	mov	r14, [rsp + gpr_save_mem_offset + 4*8]
	mov	r15, [rsp + gpr_save_mem_offset + 5*8]

%ifndef ALIGN_STACK
	add	rsp, stack_size
%else
	mov	rsp, rbp
	pop	rbp
%endif
%endm
%endif

%ifidn __OUTPUT_FORMAT__, win64
%define arg0	rcx
%define	arg1	rdx
%define	arg2	r8

%macro FUNC_SAVE 0
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
%endm

%macro FUNC_RESTORE 0
	mov	rbx, [rsp + gpr_save_mem_offset + 0*8]
	mov	rsi, [rsp + gpr_save_mem_offset + 1*8]
	mov	rdi, [rsp + gpr_save_mem_offset + 2*8]
	mov	rbp, [rsp + gpr_save_mem_offset + 3*8]
	mov	r12, [rsp + gpr_save_mem_offset + 4*8]
	mov	r13, [rsp + gpr_save_mem_offset + 5*8]
	mov	r14, [rsp + gpr_save_mem_offset + 6*8]
	mov	r15, [rsp + gpr_save_mem_offset + 7*8]

%ifndef ALIGN_STACK
	add	rsp, stack_size
%else
	mov	rsp, rbp
	pop	rbp
%endif
%endm
%endif


_lit_len_offset	equ	0
_dist_offset	equ	(8 * LIT_LEN)
_hash_offset	equ	(_dist_offset + 8 * DIST_LEN)


%macro len_to_len_code 3
%define %%len_code	%1 	; Output
%define	%%len		%2	; Input
%define	%%rfc_lookup	%3
	movzx	%%len_code, byte [%%rfc_lookup + _len_to_code_offset + %%len]
	or	%%len_code, 0x100
%endm

;;; Clobbers rcx and dist
%macro	dist_to_dist_code 2
%define %%dist_code	%1	; Output code associated with dist
%define	%%dist_coded	%1d
%define	%%dist		%2d	; Input dist
	dec	%%dist
	mov	%%dist_coded, %%dist
	bsr	ecx, %%dist_coded
	dec	ecx
	SHRX	%%dist_code, %%dist_code, rcx
	lea	%%dist_coded, [%%dist_coded + 2*ecx]

	cmp	%%dist, 1
	cmovle	%%dist_coded, %%dist
%endm

;;; Clobbers rcx and dist
%macro	dist_to_dist_code2 2
%define	%%dist_code	%1	; Output code associated with dist
%define %%dist_coded	%1d
%define	%%dist		%2d	; Input -(dist - 1)
	neg	%%dist
	mov	%%dist_coded, %%dist
	bsr	ecx, %%dist_coded
	dec	ecx
	SHRX	%%dist_code, %%dist_code, rcx
	lea	%%dist_coded, [%%dist_coded + 2*ecx]

	cmp	%%dist, 1
	cmovle	%%dist_coded, %%dist
%endm

[bits 64]
default rel
section .text

; void isal_update_histogram
global isal_update_histogram_ %+ ARCH
isal_update_histogram_ %+ ARCH %+ :
	endbranch
	FUNC_SAVE

%ifnidn	file_start, arg0
	mov	file_start, arg0
%endif
%ifnidn	file_length, arg1
	mov	file_length, arg1
%endif
%ifnidn	histogram, arg2
	mov	histogram, arg2
%endif
	mov	f_i, 0
	cmp	file_length, 0
	je	exit_ret	; If nothing to do then exit

	mov	tmp1, qword [histogram + _lit_len_offset + 8*256]
	inc	tmp1
	mov	[rsp + _eob_count_offset], tmp1

	lea	rfc_lookup, [rfc1951_lookup_table]

	;; Init hash_table
	PXOR	vtmp0, vtmp0, vtmp0
	mov	rcx, (IGZIP_LVL0_HASH_SIZE - V_LENGTH)
init_hash_table:
	MOVDQU	[histogram + _hash_offset + 2 * rcx], vtmp0
	MOVDQU	[histogram + _hash_offset + 2 * (rcx + V_LENGTH / 2)], vtmp0
	sub	rcx, V_LENGTH
	jge	init_hash_table

	sub	file_length, LA_STATELESS
	cmp	file_length, 0
	jle	end_loop_2


	;; Load first literal into histogram
	mov	curr_data, [file_start + f_i]
	compute_hash	hash, curr_data
	and	hash %+ d, LVL0_HASH_MASK
	mov	[histogram + _hash_offset + 2 * hash], f_i %+ w
	and	curr_data, 0xff
	inc	qword [histogram + _lit_len_offset + HIST_ELEM_SIZE * curr_data]
	inc	f_i

	;; Setup to begin loop 2
	MOVDQU	xdata, [file_start + f_i]
	mov	curr_data, [file_start + f_i]
	mov	curr_data2, curr_data
	compute_hash	hash, curr_data
	shr	curr_data2, 8
	compute_hash	hash2, curr_data2

	and	hash2 %+ d, LVL0_HASH_MASK
	and	hash, LVL0_HASH_MASK
loop2:
	xor	dist, dist
	xor	dist2, dist2
	xor	tmp3, tmp3

	lea	tmp1, [file_start + f_i]

	MOVQ	curr_data, xdata
	PSRLDQ	xdata, 1

	;; Load possible look back distances and update hash data
	mov	dist %+ w, f_i %+ w
	sub	dist, 1
	sub	dist %+ w, word [histogram + _hash_offset + 2 * hash]
	mov	[histogram + _hash_offset + 2 * hash], f_i %+ w

	add	f_i, 1

	mov	dist2 %+ w, f_i %+ w
	sub	dist2, 1
	sub	dist2 %+ w, word [histogram + _hash_offset + 2 * hash2]
	mov	[histogram + _hash_offset + 2 * hash2], f_i %+ w

	;; Start computing hashes to be used in either the next loop or
	;; for updating the hash if a match is found
	MOVQ	curr_data2, xdata
	MOVQ	tmp2, xdata
	shr	curr_data2, 8
	compute_hash	hash, curr_data2

	;; Check if look back distances are valid. Load a junk distance of 1
	;; if the look back distance is too long for speculative lookups.
	and	dist %+ d, (D-1)
	neg	dist

	and	dist2 %+ d, (D-1)
	neg	dist2

	shr	tmp2, 16
	compute_hash	hash2, tmp2

	;; Check for long len/dist matches (>7)
	mov	len, curr_data
	xor	len, [tmp1 + dist - 1]
	jz	compare_loop

	and	hash %+ d, LVL0_HASH_MASK
	and	hash2 %+ d, LVL0_HASH_MASK

	MOVQ	len2, xdata
	xor	len2, [tmp1 + dist2]
	jz	compare_loop2

	;; Specutively load the code for the first literal
	movzx   tmp1, curr_data %+ b
	shr	curr_data, 8

	lea	tmp3, [f_i + 1]

	;; Check for len/dist match for first literal
	test    len %+ d, 0xFFFFFFFF
	jz      len_dist_huffman_pre

	;; Store first literal
	inc	qword [histogram + _lit_len_offset + HIST_ELEM_SIZE * tmp1]

	;; Check for len/dist match for second literal
	test    len2 %+ d, 0xFFFFFFFF
	jnz     lit_lit_huffman
len_dist_lit_huffman_pre:
	;; Calculate repeat length
	tzcnt	len2, len2
	shr	len2, 3

len_dist_lit_huffman:
	MOVQ	curr_data, xdata
	shr	curr_data, 24
	compute_hash hash3, curr_data

	;; Store updated hashes
	mov	[histogram + _hash_offset + 2 * hash], tmp3 %+ w
	add	tmp3,1
	mov	[histogram + _hash_offset + 2 * hash2], tmp3 %+ w
	add	tmp3, 1

	add	f_i, len2

	MOVDQU	xdata, [file_start + f_i]
	mov	curr_data, [file_start + f_i]
	mov	tmp1, curr_data
	compute_hash	hash, curr_data

	and	hash3, LVL0_HASH_MASK
	mov	[histogram + _hash_offset + 2 * hash3], tmp3 %+ w

	dist_to_dist_code2 dist_code2, dist2

	len_to_len_code len_code, len2, rfc_lookup

	shr	tmp1, 8
	compute_hash	hash2, tmp1

	inc	qword [histogram + _lit_len_offset + HIST_ELEM_SIZE * len_code]
	inc	qword [histogram + _dist_offset + HIST_ELEM_SIZE * dist_code2]

	and	hash2 %+ d, LVL0_HASH_MASK
	and	hash, LVL0_HASH_MASK

	cmp	f_i, file_length
	jl	loop2
	jmp	end_loop_2
	;; encode as dist/len

len_dist_huffman_pre:
	tzcnt	len, len
	shr	len, 3

len_dist_huffman:
	mov	[histogram + _hash_offset + 2 * hash], tmp3 %+ w
	add	tmp3,1
	mov	[histogram + _hash_offset + 2 * hash2], tmp3 %+ w

	dec	f_i
	add	f_i, len

	MOVDQU	xdata, [file_start + f_i]
	mov	curr_data, [file_start + f_i]
	mov	tmp1, curr_data
	compute_hash	hash, curr_data

	dist_to_dist_code2 dist_code, dist

	len_to_len_code len_code, len, rfc_lookup

	shr	tmp1, 8
	compute_hash	hash2, tmp1

	inc	qword [histogram + _lit_len_offset + HIST_ELEM_SIZE * len_code]
	inc	qword [histogram + _dist_offset + HIST_ELEM_SIZE * dist_code]

	and	hash2 %+ d, LVL0_HASH_MASK
	and	hash, LVL0_HASH_MASK

	cmp	f_i, file_length
	jl	loop2
	jmp	end_loop_2

lit_lit_huffman:
	MOVDQU	xdata, [file_start + f_i + 1]
	and     curr_data, 0xff
	add	f_i, 1
	inc	qword [histogram + _lit_len_offset + HIST_ELEM_SIZE * curr_data]

	cmp	f_i, file_length
	jl	loop2

end_loop_2:
	add	file_length, LA_STATELESS - LAST_BYTES_COUNT
	cmp	f_i, file_length
	jge	final_bytes

loop2_finish:
	mov	curr_data %+ d, dword [file_start + f_i]
	compute_hash	hash, curr_data
	and	hash %+ d, LVL0_HASH_MASK

	;; Calculate possible distance for length/dist pair.
	xor	dist, dist
	mov	dist %+ w, f_i %+ w
	sub	dist %+ w, word [histogram + _hash_offset + 2 * hash]
	mov	[histogram + _hash_offset + 2 * hash], f_i %+ w

	;; Check if look back distance is valid (the dec is to handle when dist = 0)
	dec	dist
	cmp	dist %+ d, (D-1)
	jae	encode_literal_finish
	inc	dist

	;; Check if look back distance is a match
	lea	tmp4, [file_length + LAST_BYTES_COUNT]
	sub	tmp4, f_i
	lea	tmp1, [file_start + f_i]
	mov	tmp2, tmp1
	sub	tmp2, dist
	compare	tmp4, tmp1, tmp2, len, tmp3

	;; Limit len to maximum value of 258
	mov	tmp2, 258
	cmp	len, 258
	cmova	len, tmp2
	cmp	len, SHORTEST_MATCH
	jb	encode_literal_finish

	add	f_i, len

	len_to_len_code	len_code, len, rfc_lookup
	dist_to_dist_code dist_code, dist

	inc	qword [histogram + _lit_len_offset + HIST_ELEM_SIZE * len_code]
	inc	qword [histogram + _dist_offset + HIST_ELEM_SIZE * dist_code]

	cmp	f_i, file_length
	jl	loop2_finish
	jmp	final_bytes

encode_literal_finish:
	;; Encode literal
	and	curr_data %+ d, 0xFF
	inc	qword [histogram + _lit_len_offset + HIST_ELEM_SIZE * curr_data]

	;; Setup for next loop
	add	f_i, 1
	cmp	f_i, file_length
	jl	loop2_finish

final_bytes:
	add	file_length, LAST_BYTES_COUNT
final_bytes_loop:
	cmp	f_i, file_length
	jge	end
	movzx	curr_data, byte [file_start + f_i]
	inc	qword [histogram + _lit_len_offset + HIST_ELEM_SIZE * curr_data]
	inc	f_i
	jmp	final_bytes_loop

end:
	;; Handle eob at end of stream
	mov	tmp1, [rsp + _eob_count_offset]
	mov	qword [histogram + _lit_len_offset + HIST_ELEM_SIZE * 256], tmp1

exit_ret:
	FUNC_RESTORE
	ret

compare_loop:
	and	hash %+ d, LVL0_HASH_MASK
	and	hash2 %+ d, LVL0_HASH_MASK
	lea	tmp2, [tmp1 + dist - 1]

	mov	len2, 250
	mov	len, 8
	compare250	tmp1, tmp2, len, len2, tmp3, ytmp0, ytmp1

	lea	tmp3, [f_i + 1]
	jmp	len_dist_huffman

compare_loop2:
	add	tmp1, 1
	lea	tmp2, [tmp1 + dist2 - 1]

	mov	len, 250
	mov	len2, 8
	compare250	tmp1, tmp2, len2, len, tmp3, ytmp0, ytmp1

	and	curr_data, 0xff
	inc	qword [histogram + _lit_len_offset + 8 * curr_data]
	lea	tmp3, [f_i + 1]
	jmp	len_dist_lit_huffman

section .data
	align 32
D_vector:
	dw	-(D + 1) & 0xFFFF, -(D + 1) & 0xFFFF, -(D + 1) & 0xFFFF, -(D + 1) & 0xFFFF
	dw	-(D + 1) & 0xFFFF, -(D + 1) & 0xFFFF, -(D + 1) & 0xFFFF, -(D + 1) & 0xFFFF
	dw	-(D + 1) & 0xFFFF, -(D + 1) & 0xFFFF, -(D + 1) & 0xFFFF, -(D + 1) & 0xFFFF
	dw	-(D + 1) & 0xFFFF, -(D + 1) & 0xFFFF, -(D + 1) & 0xFFFF, -(D + 1) & 0xFFFF
