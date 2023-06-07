;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
; Copyright (C) 2022 Intel Corporation
;
; SPDX-License-Identifier: MIT
;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;

default rel

%include "reg_sizes.asm"

%define DECOMP_OK 0
%define END_INPUT 1
%define OUT_OVERFLOW 2
%define INVALID_BLOCK -1
%define INVALID_SYMBOL -2
%define INVALID_LOOKBACK -3

%ifdef QPL_LIB
%define QPL_HW_BASE_CODE        		    -200 ; Emulated HW error base
%define QPL_AD_ERROR_CODE_BAD_LL_CODE   	QPL_HW_BASE_CODE - 11
%define QPL_AD_ERROR_CODE_BAD_D_CODE   	    QPL_HW_BASE_CODE - 12
%define QPL_AD_ERROR_CODE_BAD_DIST          QPL_HW_BASE_CODE - 17
%define QPL_AD_ERROR_CODE_REF_BEFORE_START  QPL_HW_BASE_CODE - 18
%endif

%define ISAL_DECODE_LONG_BITS 12
%define ISAL_DECODE_SHORT_BITS 10

%define COPY_SIZE 16
%define	COPY_LEN_MAX 258

%define IN_BUFFER_SLOP 8
%define OUT_BUFFER_SLOP	COPY_SIZE + COPY_LEN_MAX

%include "inflate_data_structs.asm"
%include "stdmac.asm"

extern rfc1951_lookup_table



%define LARGE_SHORT_SYM_LEN 25
%define LARGE_SHORT_SYM_MASK ((1 << LARGE_SHORT_SYM_LEN) - 1)
%define LARGE_LONG_SYM_LEN 10
%define LARGE_LONG_SYM_MASK ((1 << LARGE_LONG_SYM_LEN) - 1)
%define LARGE_SHORT_CODE_LEN_OFFSET 28
%define LARGE_LONG_CODE_LEN_OFFSET 10
%define LARGE_FLAG_BIT_OFFSET 25
%define LARGE_FLAG_BIT (1 << LARGE_FLAG_BIT_OFFSET)
%define LARGE_SYM_COUNT_OFFSET 26
%define LARGE_SYM_COUNT_LEN 2
%define LARGE_SYM_COUNT_MASK ((1 << LARGE_SYM_COUNT_LEN) - 1)
%define LARGE_SHORT_MAX_LEN_OFFSET 26

%define SMALL_SHORT_SYM_LEN 9
%define SMALL_SHORT_SYM_MASK ((1 << SMALL_SHORT_SYM_LEN) - 1)
%define SMALL_LONG_SYM_LEN 9
%define SMALL_LONG_SYM_MASK ((1 << SMALL_LONG_SYM_LEN) - 1)
%define SMALL_SHORT_CODE_LEN_OFFSET 11
%define SMALL_LONG_CODE_LEN_OFFSET 10
%define SMALL_FLAG_BIT_OFFSET 10
%define SMALL_FLAG_BIT (1 << SMALL_FLAG_BIT_OFFSET)

%define DIST_SYM_OFFSET 0
%define DIST_SYM_LEN 5
%define DIST_SYM_MASK ((1 << DIST_SYM_LEN) - 1)
%define DIST_SYM_EXTRA_OFFSET 5
%define DIST_SYM_EXTRA_LEN 4
%define DIST_SYM_EXTRA_MASK ((1 << DIST_SYM_EXTRA_LEN) - 1)

;; rax
%define	tmp3		rax
%define	read_in_2	rax
%define	look_back_dist	rax

;; rcx
;; rdx	arg3
%define	next_sym2	rdx
%define copy_start	rdx
%define	tmp4		rdx

;; rdi	arg1
%define	tmp1		rdi
%define look_back_dist2 rdi
%define next_bits2	rdi
%define next_sym3	rdi

;; rsi	arg2
%define	tmp2		rsi
%define next_sym_num	rsi
%define	next_bits	rsi

;; rbx ; Saved
%define	next_in		rbx

;; rbp ; Saved
%define	end_in		rbp

;; r8
%define	repeat_length	r8

;; r9
%define	read_in		r9

;; r10
%define read_in_length	r10

;; r11
%define	state		r11

;; r12 ; Saved
%define next_out	r12

;; r13 ; Saved
%define	end_out		r13

;; r14 ; Saved
%define	next_sym	r14

;; r15 ; Saved
%define rfc_lookup	r15

start_out_mem_offset	equ	0
read_in_mem_offset	equ	8
read_in_length_mem_offset	equ	16
next_out_mem_offset	equ	24
gpr_save_mem_offset	equ	32
stack_size		equ	4 * 8 + 8 * 8

%define	_dist_extra_bit_count	264
%define	_dist_start		_dist_extra_bit_count + 1*32
%define	_len_extra_bit_count	_dist_start + 4*32
%define	_len_start		_len_extra_bit_count + 1*32

%ifidn __OUTPUT_FORMAT__, elf64
%define arg0	rdi
%define arg1	rsi

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
%define arg1	rdx

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

;; Load read_in and updated in_buffer accordingly
;; when there are at least 8 bytes in the in buffer
;; Clobbers rcx, unless rcx is %%read_in_length
%macro inflate_in_load 6
%define	%%next_in		%1
%define %%end_in		%2
%define %%read_in		%3
%define %%read_in_length	%4
%define %%tmp1			%5 ; Tmp registers
%define %%tmp2			%6

	SHLX	%%tmp1, [%%next_in], %%read_in_length
	or	%%read_in, %%tmp1

	mov	%%tmp1, 64
	sub	%%tmp1, %%read_in_length
	shr	%%tmp1, 3

	add	%%next_in, %%tmp1
	lea	%%read_in_length, [%%read_in_length + 8 * %%tmp1]
%%end:
%endm

;; Load read_in and updated in_buffer accordingly
;; Clobbers rcx, unless rcx is %%read_in_length
%macro inflate_in_small_load 6
%define	%%next_in		%1
%define %%end_in		%2
%define %%read_in		%3
%define %%read_in_length	%4
%define %%avail_in		%5 ; Tmp registers
%define %%tmp1			%5
%define %%loop_count		%6

	mov	%%avail_in, %%end_in
	sub	%%avail_in, %%next_in

%ifnidn %%read_in_length, rcx
	mov	rcx, %%read_in_length
%endif

	mov	%%loop_count, 64
	sub	%%loop_count, %%read_in_length
	shr	%%loop_count, 3

	cmp	%%loop_count, %%avail_in
	cmovg	%%loop_count, %%avail_in
	cmp	%%loop_count, 0
	je	%%end

%%load_byte:
	xor	%%tmp1, %%tmp1
	mov	%%tmp1 %+ b, byte [%%next_in]
	SHLX	%%tmp1, %%tmp1, rcx
	or	%%read_in, %%tmp1
	add	rcx, 8
	add	%%next_in, 1
	sub	%%loop_count, 1
	jg	%%load_byte
%ifnidn %%read_in_length, rcx
	mov	%%read_in_length, rcx
%endif
%%end:
%endm

;; Clears all bits at index %%bit_count and above in %%next_bits
;; May clobber rcx and %%bit_count
%macro CLEAR_HIGH_BITS		3
%define %%next_bits		%1
%define %%bit_count		%2
%define %%lookup_size		%3

	sub	%%bit_count, 0x40 + %%lookup_size
;; Extract the 15-DECODE_LOOKUP_SIZE bits beyond the first DECODE_LOOKUP_SIZE bits.
%ifdef USE_HSWNI
	and	%%bit_count, 0x1F
	bzhi	%%next_bits, %%next_bits, %%bit_count
%else
%ifnidn %%bit_count, rcx
	mov rcx, %%bit_count
%endif
	neg	rcx
	shl	%%next_bits, cl
	shr	%%next_bits, cl
%endif

%endm

;; Decode next symbol
;; Clobber rcx
%macro decode_next_lit_len	8
%define	%%state			%1 ; State structure associated with compressed stream
%define %%lookup_size		%2 ; Number of bits used for small lookup
%define	%%state_offset		%3 ; Type of huff code, should be either LIT or DIST
%define %%read_in		%4 ; Bits read in from compressed stream
%define %%read_in_length	%5 ; Number of valid bits in read_in
%define %%next_sym		%6 ; Returned symbols
%define %%next_sym_num		%7 ; Returned symbols count
%define	%%next_bits		%8

	mov	%%next_sym_num, %%next_sym
	mov	rcx, %%next_sym
	shr	rcx, LARGE_SHORT_CODE_LEN_OFFSET
	jz	invalid_symbol

	and	%%next_sym_num, LARGE_SYM_COUNT_MASK << LARGE_SYM_COUNT_OFFSET
	shr	%%next_sym_num, LARGE_SYM_COUNT_OFFSET

	;; Check if symbol or hint was looked up
	and	%%next_sym, LARGE_FLAG_BIT | LARGE_SHORT_SYM_MASK
	test	%%next_sym, LARGE_FLAG_BIT
	jz	%%end

	shl	rcx, LARGE_SYM_COUNT_LEN
	or	rcx, %%next_sym_num

	;; Save length associated with symbol
	mov	%%next_bits, %%read_in
	shr	%%next_bits, %%lookup_size

	;; Extract the bits beyond the first %%lookup_size bits.
	CLEAR_HIGH_BITS %%next_bits, rcx, %%lookup_size

	and	%%next_sym, LARGE_SHORT_SYM_MASK
	add	%%next_sym, %%next_bits

	;; Lookup actual next symbol
	movzx	%%next_sym, word [%%state + LARGE_LONG_CODE_SIZE * %%next_sym + %%state_offset + LARGE_SHORT_CODE_SIZE * (1 << %%lookup_size)]
	mov	%%next_sym_num, 1

	;; Save length associated with symbol
	mov	rcx, %%next_sym
	shr	rcx, LARGE_LONG_CODE_LEN_OFFSET
	jz	invalid_symbol
	and	%%next_sym, LARGE_LONG_SYM_MASK

%%end:
;; Updated read_in to reflect the bits which were decoded
	SHRX	%%read_in, %%read_in, rcx
	sub	%%read_in_length, rcx
%endm

;; Decode next symbol
;; Clobber rcx
%macro decode_next_lit_len_with_load	8
%define	%%state			%1 ; State structure associated with compressed stream
%define %%lookup_size		%2 ; Number of bits used for small lookup
%define	%%state_offset		%3
%define %%read_in		%4 ; Bits read in from compressed stream
%define %%read_in_length	%5 ; Number of valid bits in read_in
%define %%next_sym		%6 ; Returned symbols
%define %%next_sym_num		%7 ; Returned symbols count
%define %%next_bits		%8

	;; Lookup possible next symbol
	mov	%%next_bits, %%read_in
	and	%%next_bits, (1 << %%lookup_size) - 1
	mov	%%next_sym %+ d, dword [%%state + %%state_offset + LARGE_SHORT_CODE_SIZE * %%next_bits]

	decode_next_lit_len %%state, %%lookup_size, %%state_offset, %%read_in, %%read_in_length, %%next_sym, %%next_sym_num, %%next_bits
%endm

;; Decode next symbol
;; Clobber rcx
%macro decode_next_dist	8
%define	%%state			%1 ; State structure associated with compressed stream
%define %%lookup_size		%2 ; Number of bits used for small lookup
%define	%%state_offset		%3 ; Type of huff code, should be either LIT or DIST
%define %%read_in		%4 ; Bits read in from compressed stream
%define %%read_in_length	%5 ; Number of valid bits in read_in
%define %%next_sym		%6 ; Returned symobl
%define %%next_extra_bits	%7
%define	%%next_bits		%8

	mov	rcx, %%next_sym
	shr	rcx, SMALL_SHORT_CODE_LEN_OFFSET
	jz	invalid_dist_symbol_ %+ %%next_sym

	;; Check if symbol or hint was looked up
	and	%%next_sym, SMALL_FLAG_BIT | SMALL_SHORT_SYM_MASK
	test	%%next_sym, SMALL_FLAG_BIT
	jz	%%end

	;; Save length associated with symbol
	mov	%%next_bits, %%read_in
	shr	%%next_bits, %%lookup_size

	;; Extract the 15-DECODE_LOOKUP_SIZE bits beyond the first %%lookup_size bits.
	lea	%%next_sym, [%%state + SMALL_LONG_CODE_SIZE * %%next_sym]

	CLEAR_HIGH_BITS %%next_bits, rcx, %%lookup_size

	;; Lookup actual next symbol
	movzx	%%next_sym, word [%%next_sym + %%state_offset + SMALL_LONG_CODE_SIZE * %%next_bits + SMALL_SHORT_CODE_SIZE * (1 << %%lookup_size) - SMALL_LONG_CODE_SIZE * SMALL_FLAG_BIT]

	;; Save length associated with symbol
	mov	rcx, %%next_sym
	shr	rcx, SMALL_LONG_CODE_LEN_OFFSET
	jz	invalid_dist_symbol_ %+ %%next_sym
	and	%%next_sym, SMALL_SHORT_SYM_MASK

%%end:
	;; Updated read_in to reflect the bits which were decoded
	SHRX	%%read_in, %%read_in, rcx
	sub	%%read_in_length, rcx
	mov	rcx, %%next_sym
	shr	rcx, DIST_SYM_EXTRA_OFFSET
	and	%%next_sym, DIST_SYM_MASK
%endm

;; Decode next symbol
;; Clobber rcx
%macro decode_next_dist_with_load	8
%define	%%state			%1 ; State structure associated with compressed stream
%define %%lookup_size		%2 ; Number of bits used for small lookup
%define	%%state_offset		%3
%define %%read_in		%4 ; Bits read in from compressed stream
%define %%read_in_length	%5 ; Number of valid bits in read_in
%define %%next_sym		%6 ; Returned symobl
%define %%next_extra_bits	%7
%define %%next_bits		%8

	;; Lookup possible next symbol
	mov	%%next_bits, %%read_in
	and	%%next_bits, (1 << %%lookup_size) - 1
	movzx	%%next_sym, word [%%state + %%state_offset + SMALL_SHORT_CODE_SIZE * %%next_bits]

	decode_next_dist %%state, %%lookup_size, %%state_offset, %%read_in, %%read_in_length, %%next_sym, %%next_extra_bits, %%next_bits
%endm

[bits 64]
default rel
section .text

global decode_huffman_code_block_stateless_ %+ ARCH
decode_huffman_code_block_stateless_ %+ ARCH %+ :
	endbranch

	FUNC_SAVE

	mov	state, arg0
	mov	[rsp + start_out_mem_offset], arg1
	lea	rfc_lookup, [rfc1951_lookup_table]

	mov	read_in,[state + _read_in]
	mov	read_in_length %+ d, dword [state + _read_in_length]
	mov	next_out, [state + _next_out]
	mov	end_out %+ d, dword [state + _avail_out]
	add	end_out, next_out
	mov	next_in, [state + _next_in]
	mov	end_in %+ d, dword [state + _avail_in]
	add	end_in, next_in

	mov	dword [state + _copy_overflow_len], 0
	mov	dword [state + _copy_overflow_dist], 0

	sub	end_out, OUT_BUFFER_SLOP
	sub	end_in, IN_BUFFER_SLOP

	cmp	next_in, end_in
	jg	end_loop_block_pre

	cmp	read_in_length, 64
	je	skip_load

	inflate_in_load	next_in, end_in, read_in, read_in_length, tmp1, tmp2

skip_load:
	mov	tmp3, read_in
	and	tmp3, (1 << ISAL_DECODE_LONG_BITS) - 1
	mov	next_sym %+ d, dword [state + _lit_huff_code + LARGE_SHORT_CODE_SIZE * tmp3]

;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
; Main Loop
;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
loop_block:
	;; Check if near end of in buffer or out buffer
	cmp	next_in, end_in
	jg	end_loop_block_pre
	cmp	next_out, end_out
	jg	end_loop_block_pre

	;; Decode next symbol and reload the read_in buffer
	decode_next_lit_len	state, ISAL_DECODE_LONG_BITS, _lit_huff_code, read_in, read_in_length, next_sym, next_sym_num, tmp1

	;; Specutively write next_sym if it is a literal
	mov	[next_out], next_sym
	add	next_out, next_sym_num
	lea	next_sym2, [8 * next_sym_num - 8]
	SHRX	next_sym2, next_sym, next_sym2

	;; Find index to specutively preload next_sym from
	mov	tmp3, (1 << ISAL_DECODE_LONG_BITS) - 1
	and	tmp3, read_in

	;; Start reloading read_in
	mov	tmp1, [next_in]
	SHLX	tmp1, tmp1, read_in_length
	or	read_in, tmp1

	;; Specutively load data associated with length symbol
	lea	repeat_length, [next_sym2 - 254]

	;; Test for end of block symbol
	cmp	next_sym2, 256
	je	end_symbol_pre

	;; Specutively load next_sym for next loop if a literal was decoded
	mov	next_sym %+ d, dword [state + _lit_huff_code + LARGE_SHORT_CODE_SIZE * tmp3]

	;; Finish updating read_in_length for read_in
	mov	tmp1, 64
	sub	tmp1, read_in_length
	shr	tmp1, 3
	add	next_in, tmp1
	lea	read_in_length, [read_in_length + 8 * tmp1]

	;; Specultively load next dist code
	mov	next_bits2, (1 << ISAL_DECODE_SHORT_BITS) - 1
	and	next_bits2, read_in
	movzx	next_sym3, word [state + _dist_huff_code + SMALL_SHORT_CODE_SIZE * next_bits2]

	;; Check if next_sym2 is a literal, length, or end of block symbol
	cmp	next_sym2, 256
	jl	loop_block

decode_len_dist:
	;; Determine next_out after the copy is finished
	lea	next_out, [next_out + repeat_length - 1]

	;; Decode distance code
	decode_next_dist state, ISAL_DECODE_SHORT_BITS, _dist_huff_code, read_in, read_in_length, next_sym3, rcx, tmp2

	mov	look_back_dist2 %+ d, [rfc_lookup + _dist_start + 4 * next_sym3]

	; ;; Load distance code extra bits
	mov	next_bits, read_in

	;; Calculate the look back distance
	BZHI	next_bits, next_bits, rcx, tmp4
	SHRX	read_in, read_in, rcx

	;; Setup next_sym, read_in, and read_in_length for next loop
	mov	read_in_2, (1 << ISAL_DECODE_LONG_BITS) - 1
	and	read_in_2, read_in
	mov	next_sym %+ d, dword [state + _lit_huff_code + LARGE_SHORT_CODE_SIZE * read_in_2]
	sub	read_in_length, rcx

	;; Copy distance in len/dist pair
	add	look_back_dist2, next_bits

	;; Find beginning of copy
	mov	copy_start, next_out
	sub	copy_start, repeat_length
	sub	copy_start, look_back_dist2

	;; Check if a valid look back distances was decoded
	cmp	copy_start, [rsp + start_out_mem_offset]
	jl	invalid_look_back_distance

    %ifdef QPL_LIB
        ;; Check if a loop back is reference to out of history
    	cmp look_back_dist2, QPL_HISTORY_SIZE
        jg invalid_look_back_history_underflow
    %endif

	MOVDQU	xmm1, [copy_start]

	;; Set tmp2 to be the minimum of COPY_SIZE and repeat_length
	;; This is to decrease use of small_byte_copy branch
	mov	tmp2, COPY_SIZE
	cmp	tmp2, repeat_length
	cmovg	tmp2, repeat_length

	;; Check for overlapping memory in the copy
	cmp	look_back_dist2, tmp2
	jl	small_byte_copy_pre

large_byte_copy:
	;; Copy length distance pair when memory overlap is not an issue
	MOVDQU [copy_start + look_back_dist2], xmm1

	sub	repeat_length, COPY_SIZE
	jle	loop_block

	add	copy_start, COPY_SIZE
	MOVDQU	xmm1, [copy_start]
	jmp	large_byte_copy

small_byte_copy_pre:
	;; Copy length distance pair when source and destination overlap
	add	repeat_length, look_back_dist2
small_byte_copy:
	MOVDQU [copy_start + look_back_dist2], xmm1

	shl	look_back_dist2, 1
	MOVDQU	xmm1, [copy_start]
	cmp	look_back_dist2, COPY_SIZE
	jl	small_byte_copy

	sub	repeat_length, look_back_dist2
	jge	large_byte_copy
	jmp	loop_block

;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
; Finish Main Loop
;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
end_loop_block_pre:
	;; Fix up in buffer and out buffer to reflect the actual buffer end
	add	end_out, OUT_BUFFER_SLOP
	add	end_in, IN_BUFFER_SLOP

end_loop_block:
	;; Load read in buffer and decode next lit/len symbol
	inflate_in_small_load	next_in, end_in, read_in, read_in_length, tmp1, tmp2
	mov	[rsp + read_in_mem_offset], read_in
	mov	[rsp + read_in_length_mem_offset], read_in_length
	mov	[rsp + next_out_mem_offset], next_out

	decode_next_lit_len_with_load state, ISAL_DECODE_LONG_BITS, _lit_huff_code, read_in, read_in_length, next_sym, next_sym_num, tmp1

	;; Check that enough input was available to decode symbol
	cmp	read_in_length, 0
	jl	end_of_input

multi_symbol_start:
	cmp	next_sym_num, 1
	jg	decode_literal

	cmp	next_sym, 256
	jl	decode_literal
	je	end_symbol

decode_len_dist_2:
	lea	repeat_length, [next_sym - 254]
	;; Decode distance code
	decode_next_dist_with_load state, ISAL_DECODE_SHORT_BITS, _dist_huff_code, read_in, read_in_length, next_sym, rcx, tmp1

	;; Load distance code extra bits
	mov	next_bits, read_in
	mov	look_back_dist %+ d, [rfc_lookup + _dist_start + 4 * next_sym]

	;; Calculate the look back distance and check for enough input
	BZHI	next_bits, next_bits, rcx, tmp1
	SHRX	read_in, read_in, rcx
	add	look_back_dist, next_bits
	sub	read_in_length, rcx
	jl	end_of_input

	;; Setup code for byte copy using rep  movsb
	mov	rsi, next_out
	mov	rdi, rsi
	mov	rcx, repeat_length
	sub	rsi, look_back_dist

	;; Check if a valid look back distance was decoded
	cmp	rsi, [rsp + start_out_mem_offset]
	jl	invalid_look_back_distance

%ifdef QPL_LIB
    ;; Check if a loop back is reference to out of history
	cmp look_back_dist, QPL_HISTORY_SIZE
    jg invalid_look_back_history_underflow
%endif

	;; Check for out buffer overflow
	add	repeat_length, next_out
	cmp	repeat_length, end_out
	jg	out_buffer_overflow_repeat

	mov	next_out, repeat_length

	rep	movsb
	jmp	end_loop_block

decode_literal:
	;; Store literal decoded from the input stream
	cmp	next_out, end_out
	jge	out_buffer_overflow_lit
	add	next_out, 1
	mov	byte [next_out - 1], next_sym %+ b
	sub	next_sym_num, 1
	jz	end_loop_block
	shr	next_sym, 8
	jmp	multi_symbol_start

;; Set exit codes
end_of_input:
	mov	read_in, [rsp + read_in_mem_offset]
	mov	read_in_length, [rsp + read_in_length_mem_offset]
	mov	next_out, [rsp + next_out_mem_offset]
	xor	tmp1, tmp1
	mov	dword [state + _write_overflow_lits], tmp1 %+ d
	mov	dword [state + _write_overflow_len], tmp1 %+ d
	mov	rax, END_INPUT
	jmp	end

out_buffer_overflow_repeat:
	mov	rcx, end_out
	sub	rcx, next_out
	sub	repeat_length, rcx
	sub	repeat_length, next_out
	rep	movsb

	mov	[state + _copy_overflow_len], repeat_length %+ d
	mov	[state + _copy_overflow_dist], look_back_dist %+ d

	mov	next_out, end_out

	mov	rax, OUT_OVERFLOW
	jmp	end

out_buffer_overflow_lit:
	mov	dword [state + _write_overflow_lits], next_sym %+ d
	mov	dword [state + _write_overflow_len], next_sym_num %+ d
	sub	next_sym_num, 1
	shl	next_sym_num, 3
	SHRX	next_sym, next_sym, next_sym_num
	mov	rax, OUT_OVERFLOW
	shr	next_sym_num, 3
	cmp	next_sym, 256
	jl	end
	mov	dword [state + _write_overflow_len], next_sym_num %+ d
	jg	decode_len_dist_2
	jmp	end_state

invalid_look_back_distance:
%ifdef QPL_LIB
	mov	rax, QPL_AD_ERROR_CODE_REF_BEFORE_START
%else
	mov	rax, INVALID_LOOKBACK
%endif
	jmp	end

%ifdef QPL_LIB
invalid_look_back_history_underflow:
    mov rax, QPL_AD_ERROR_CODE_BAD_DIST
    jmp end
%endif

invalid_dist_symbol_ %+ next_sym:
	cmp	read_in_length, next_sym
	jl	end_of_input
	jmp	invalid_symbol
invalid_dist_symbol_ %+ next_sym3:
	cmp	read_in_length, next_sym3
	jl	end_of_input
%ifdef QPL_LIB
    mov	rax, QPL_AD_ERROR_CODE_BAD_D_CODE
    jmp end
%endif
invalid_symbol:
%ifdef QPL_LIB
	mov	rax, QPL_AD_ERROR_CODE_BAD_LL_CODE
%else
	mov	rax, INVALID_SYMBOL
%endif
	jmp	end

end_symbol_pre:
	;; Fix up in buffer and out buffer to reflect the actual buffer
	sub	next_out, 1
	add	end_out, OUT_BUFFER_SLOP
	add	end_in, IN_BUFFER_SLOP
end_symbol:
	xor	rax, rax
end_state:
	;;  Set flag identifying a new block is required
	mov	byte [state + _block_state], ISAL_BLOCK_NEW_HDR
	cmp	dword [state + _bfinal], 0
	je	end
	mov	byte [state + _block_state], ISAL_BLOCK_INPUT_DONE

end:
	;; Save current buffer states
	mov	[state + _read_in], read_in
	mov	[state + _read_in_length], read_in_length %+ d

	;; Set avail_out
	sub	end_out, next_out
	mov	dword [state + _avail_out], end_out %+ d

	;; Set total_out
	mov	tmp1, next_out
	sub	tmp1, [state + _next_out]
	add	[state + _total_out], tmp1 %+ d

	;; Set next_out
	mov	[state + _next_out], next_out

	;; Set next_in
	mov	[state + _next_in], next_in

	;; Set avail_in
	sub	end_in, next_in
	mov	[state + _avail_in], end_in %+ d

	FUNC_RESTORE

	ret
