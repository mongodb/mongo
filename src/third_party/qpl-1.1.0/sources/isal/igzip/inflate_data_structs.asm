;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
; Copyright (C) 2022 Intel Corporation
;
; SPDX-License-Identifier: MIT
;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;

;; START_FIELDS
%macro START_FIELDS 0
%assign _FIELD_OFFSET 0
%assign _STRUCT_ALIGN 0
%endm

;; FIELD name size align
%macro FIELD 3
%define %%name  %1
%define %%size  %2
%define %%align %3

%assign _FIELD_OFFSET (_FIELD_OFFSET + (%%align) - 1) & (~ ((%%align)-1))
%%name	equ	_FIELD_OFFSET
%assign _FIELD_OFFSET _FIELD_OFFSET + (%%size)
%if (%%align > _STRUCT_ALIGN)
%assign _STRUCT_ALIGN %%align
%endif
%endm

;; See inflate_huff_code structure declaration in igzip_lib.h calculation explanation
%define L_REM (21 - ISAL_DECODE_LONG_BITS)
%define S_REM (15 - ISAL_DECODE_SHORT_BITS)

%define L_DUP ((1 << L_REM) - (L_REM + 1))
%define S_DUP ((1 << S_REM) - (S_REM + 1))

%define L_UNUSED ((1 << L_REM) - (1 << ((L_REM)/2)) - (1 << ((L_REM + 1)/2)) + 1)
%define S_UNUSED ((1 << S_REM) - (1 << ((S_REM)/2)) - (1 << ((S_REM + 1)/2)) + 1)

%define L_SIZE (286 + L_DUP + L_UNUSED)
%define S_SIZE (30 + S_DUP + S_UNUSED)

%define HUFF_CODE_LARGE_LONG_ALIGNED (L_SIZE + (-L_SIZE & 0xf))
%define HUFF_CODE_SMALL_LONG_ALIGNED (S_SIZE + (-S_SIZE & 0xf))

%define MAX_LONG_CODE_LARGE (L_SIZE + (-L_SIZE & 0xf))
%define MAX_LONG_CODE_SMALL (S_SIZE + (-S_SIZE & 0xf))

%define LARGE_SHORT_CODE_SIZE 4
%define LARGE_LONG_CODE_SIZE 2

%define SMALL_SHORT_CODE_SIZE 2
%define SMALL_LONG_CODE_SIZE 2

;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;

START_FIELDS	;; inflate huff code

;;      name				size    align
FIELD	_short_code_lookup_large,	LARGE_SHORT_CODE_SIZE * (1 << (ISAL_DECODE_LONG_BITS)),	LARGE_LONG_CODE_SIZE
FIELD	_long_code_lookup_large,	LARGE_LONG_CODE_SIZE * MAX_LONG_CODE_LARGE,		LARGE_SHORT_CODE_SIZE

%assign _inflate_huff_code_large_size	_FIELD_OFFSET
%assign _inflate_huff_code_large_align	_STRUCT_ALIGN

;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;

START_FIELDS	;; inflate huff code

;;      name				size    align
FIELD	_short_code_lookup_small,	SMALL_SHORT_CODE_SIZE * (1 << (ISAL_DECODE_SHORT_BITS)), SMALL_LONG_CODE_SIZE
FIELD	_long_code_lookup_small,	SMALL_LONG_CODE_SIZE * MAX_LONG_CODE_SMALL,		 SMALL_SHORT_CODE_SIZE

%assign _inflate_huff_code_small_size	_FIELD_OFFSET
%assign _inflate_huff_code_small_align	_STRUCT_ALIGN
;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;

START_FIELDS	;; inflate state

;;      name		size    align
FIELD	_next_out,	8,	8
FIELD	_avail_out,	4,	4
FIELD	_total_out,	4,	4
FIELD	_next_in,	8,	8
FIELD	_read_in,	8,	8
FIELD	_avail_in,	4,	4
FIELD	_read_in_length,4,	4
FIELD	_lit_huff_code,	_inflate_huff_code_large_size,	_inflate_huff_code_large_align
FIELD	_dist_huff_code,_inflate_huff_code_small_size,	_inflate_huff_code_small_align
FIELD	_block_state,	4,	4
FIELD	_dict_length,	4,	4
FIELD	_bfinal,	4,	4
FIELD	_crc_flag,	4,	4
FIELD	_crc,		4,	4
FIELD	_hist_bits,	4,	4
FIELD	_type0_block_len,	4, 	4
FIELD	_write_overflow_lits,	4,	4
FIELD	_write_overflow_len,	4,	4
FIELD	_copy_overflow_len,  	4,	4
FIELD	_copy_overflow_dist,	4,	4

%assign _inflate_state_size		_FIELD_OFFSET
%assign _inflate_state_align	_STRUCT_ALIGN

_lit_huff_code_short_code_lookup	equ	_lit_huff_code+_short_code_lookup_large
_lit_huff_code_long_code_lookup		equ	_lit_huff_code+_long_code_lookup_large

_dist_huff_code_short_code_lookup	equ	_dist_huff_code+_short_code_lookup_small
_dist_huff_code_long_code_lookup	equ	_dist_huff_code+_long_code_lookup_small

ISAL_BLOCK_NEW_HDR	equ	0
ISAL_BLOCK_HDR		equ	1
ISAL_BLOCK_TYPE0	equ	2
ISAL_BLOCK_CODED	equ	3
ISAL_BLOCK_INPUT_DONE	equ	4
ISAL_BLOCK_FINISH	equ	5
;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;

