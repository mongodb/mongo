;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
; Copyright (C) 2022 Intel Corporation
;
; SPDX-License-Identifier: MIT
;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;

%include "options.asm"
%include "lz0a_const.asm"
%include "stdmac.asm"

; Macros for doing Huffman Encoding

%ifdef LONGER_HUFFTABLE
	%if (D > 8192)
		%error History D is larger than 8K, cannot use %LONGER_HUFFTABLE
		 % error
	%else
		%define DIST_TABLE_SIZE 8192
		%define DECODE_OFFSET     26
	%endif
%else
	%define DIST_TABLE_SIZE 2
	%define DECODE_OFFSET     0
%endif

%define LEN_TABLE_SIZE 256
%define LIT_TABLE_SIZE 257

%define DIST_TABLE_START (ISAL_DEF_MAX_HDR_SIZE + 8)
%define DIST_TABLE_OFFSET (DIST_TABLE_START + - 4 * 1)
%define LEN_TABLE_OFFSET (DIST_TABLE_START + DIST_TABLE_SIZE * 4 - 4*3)
%define LIT_TABLE_OFFSET (DIST_TABLE_START + 4 * DIST_TABLE_SIZE + 4 * LEN_TABLE_SIZE)
%define LIT_TABLE_SIZES_OFFSET (LIT_TABLE_OFFSET + 2 * LIT_TABLE_SIZE)
%define DCODE_TABLE_OFFSET (LIT_TABLE_SIZES_OFFSET + LIT_TABLE_SIZE + 1 - DECODE_OFFSET * 2)
%define DCODE_TABLE_SIZE_OFFSET (DCODE_TABLE_OFFSET + 2 * 30 - DECODE_OFFSET)
;; /** @brief Holds the huffman tree used to huffman encode the input stream **/
;; struct isal_hufftables {
;;	// deflate huffman tree header
;; 	uint8_t deflate_huff_hdr[ISAL_DEF_MAX_HDR_SIZE];
;;
;;	//!< Number of whole bytes in deflate_huff_hdr
;;	uint32_t deflate_huff_hdr_count;
;;
;;	//!< Number of bits in the partial byte in header
;; 	uint32_t deflate_huff_hdr_extra_bits;
;;
;;	//!< bits 7:0 are the code length, bits 31:8 are the code
;; 	uint32_t dist_table[DIST_TABLE_SIZE];
;;
;;	//!< bits 7:0 are the code length, bits 31:8 are the code
;; 	uint32_t len_table[LEN_TABLE_SIZE];
;;
;;	//!< bits 3:0 are the code length, bits 15:4 are the code
;; 	uint16_t lit_table[LIT_TABLE_SIZE];
;;
;;	//!< bits 3:0 are the code length, bits 15:4 are the code
;; 	uint16_t dcodes[30 - DECODE_OFFSET];

;; };


%ifdef LONGER_HUFFTABLE
; Uses RCX, clobbers dist
; get_dist_code	dist, code, len
%macro get_dist_code 4
%define %%dist %1	; 64-bit IN
%define %%code %2d	; 32-bit OUT
%define %%len  %3d	; 32-bit OUT
%define %%hufftables %4	; address of the hufftable

	mov	%%len, [%%hufftables + DIST_TABLE_OFFSET + 4*(%%dist + 1) ]
	mov	%%code, %%len
	and	%%len, 0x1F;
	shr	%%code, 5
%endm

%macro get_packed_dist_code 3
%define %%dist %1	; 64-bit IN
%define %%code_len  %2d	; 32-bit OUT
%define %%hufftables %3	; address of the hufftable
	mov	%%code_len, [%%hufftables + DIST_TABLE_OFFSET + 4*%%dist ]
%endm

%macro unpack_dist_code 2
%define %%code %1d	; 32-bit OUT
%define %%len  %2d	; 32-bit OUT

	mov	%%len, %%code
	and	%%len, 0x1F;
	shr	%%code, 5
%endm

%else
; Assumes (dist != 0)
; Uses RCX, clobbers dist
; void compute_dist_code	dist, code, len
%macro compute_dist_code 4
%define %%dist %1	; IN, clobbered
%define %%distq %1
%define %%code %2	; OUT
%define %%len  %3	; OUT
%define %%hufftables %4

	bsr	rcx, %%dist	; ecx = msb = bsr(dist)
	dec	rcx		; ecx = num_extra_bits = msb - N
	BZHI	%%code, %%dist, rcx, %%len
	SHRX	%%dist, %%dist, rcx	; dist >>= num_extra_bits
	lea	%%dist, [%%dist + 2*rcx] ; dist = sym = dist + num_extra_bits*2
	mov	%%len, rcx	; len = num_extra_bits
	movzx	rcx, byte [hufftables + DCODE_TABLE_SIZE_OFFSET + %%distq WRT_OPT]
	movzx	%%dist, word [hufftables + DCODE_TABLE_OFFSET + 2 * %%distq WRT_OPT]
	SHLX	%%code, %%code, rcx	; code = extra_bits << (sym & 0xF)
	or	%%code, %%dist	; code = (sym >> 4) | (extra_bits << (sym & 0xF))
	add	%%len, rcx	; len = num_extra_bits + (sym & 0xF)
%endm

; Uses RCX, clobbers dist
; get_dist_code	dist, code, len
%macro get_dist_code 4
%define %%dist %1	; 32-bit IN, clobbered
%define %%distq %1	; 64-bit IN, clobbered
%define %%code %2	; 32-bit OUT
%define %%len  %3	; 32-bit OUT
%define %%hufftables %4

	cmp	%%dist, DIST_TABLE_SIZE - 1
	jg	%%do_compute
%ifndef IACA
	mov	%%len %+ d, dword [hufftables + DIST_TABLE_OFFSET + 4*(%%distq + 1) WRT_OPT]
	mov	%%code, %%len
	and	%%len, 0x1F;
	shr	%%code, 5
	jmp	%%done
%endif
%%do_compute:
	compute_dist_code	%%distq, %%code, %%len, %%hufftables
%%done:
%endm

%macro get_packed_dist_code 3
%define %%dist %1	; 64-bit IN
%define %%code_len  %2d	; 32-bit OUT
%define %%hufftables %3	; address of the hufftable
%endm

%endif


; Macros for doing Huffman Encoding

; Assumes (dist != 0)
; Uses RCX, clobbers dist
; void compute_dist_code	dist, code, len
%macro compute_dist_icf_code 3
%define %%dist %1	; IN, clobbered
%define %%distq %1
%define %%code %2	; OUT
%define	%%tmp1 %3

	bsr	rcx, %%dist	; ecx = msb = bsr(dist)
	dec	rcx		; ecx = num_extra_bits = msb - N
	BZHI	%%code, %%dist, rcx, %%tmp1
	SHRX	%%dist, %%dist, rcx	; dist >>= num_extra_bits
	lea	%%dist, [%%dist + 2*rcx] ; code = sym = dist + num_extra_bits*2
	shl	%%code, EXTRA_BITS_OFFSET - DIST_OFFSET
	add	%%code, %%dist	; code = extra_bits | sym

%endm

; Uses RCX, clobbers dist
; get_dist_code	dist, code, len
%macro get_dist_icf_code 3
%define %%dist %1	; 32-bit IN, clobbered
%define %%distq %1	; 64-bit IN, clobbered
%define %%code %2	; 32-bit OUT
%define %%tmp1 %3

	cmp	%%dist, 1
	jg	%%do_compute

%ifnidn	%%code, %%dist
	mov	%%code, %%dist
%endif
	jmp 	%%done
%%do_compute:
	compute_dist_icf_code	%%distq, %%code, %%tmp1
%%done:
	shl	%%code, DIST_OFFSET
%endm


; "len" can be same register as "length"
; get_len_code	length, code, len
%macro get_len_code 4
%define %%length %1	; 64-bit IN
%define %%code %2d	; 32-bit OUT
%define %%len  %3d	; 32-bit OUT
%define %%hufftables %4

	mov	%%len, [%%hufftables + LEN_TABLE_OFFSET + 4 * %%length]
	mov	%%code, %%len
	and	%%len, 0x1F
	shr	%%code, 5
%endm


%macro get_lit_code 4
%define %%lit %1	; 64-bit IN or CONST
%define %%code %2d	; 32-bit OUT
%define %%len  %3d	; 32-bit OUT
%define %%hufftables %4

	movzx	%%len, byte [%%hufftables + LIT_TABLE_SIZES_OFFSET + %%lit]
	movzx	%%code, word [%%hufftables + LIT_TABLE_OFFSET + 2 * %%lit]

%endm


;; Compute hash of first 3 bytes of data
%macro compute_hash 2
%define %%result	%1d	; 32-bit reg
%define %%data		%2d	; 32-bit reg (low byte not clobbered)

	xor	%%result, %%result
	crc32	%%result, %%data
%endm
