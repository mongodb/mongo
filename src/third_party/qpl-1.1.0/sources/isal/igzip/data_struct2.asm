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

;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;

START_FIELDS	;; BitBuf2

;;      name		size    align
FIELD	_m_bits,	8,	8
FIELD	_m_bit_count,	4,	4
FIELD	_m_out_buf,	8,	8
FIELD	_m_out_end,	8,	8
FIELD	_m_out_start,	8,	8

%assign _BitBuf2_size	_FIELD_OFFSET
%assign _BitBuf2_align	_STRUCT_ALIGN

;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
%define HIST_ELEM_SIZE 4

START_FIELDS	;; isal_mod_hist

;;      name		size    align
FIELD	_d_hist,	30*HIST_ELEM_SIZE,	HIST_ELEM_SIZE
FIELD	_ll_hist,	513*HIST_ELEM_SIZE,	HIST_ELEM_SIZE

%assign _isal_mod_hist_size	_FIELD_OFFSET
%assign _isal_mod_hist_align	_STRUCT_ALIGN

;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;

%define HUFF_CODE_SIZE 4

START_FIELDS	;; hufftables_icf

;;      name		size    align
FIELD	_dist_table,	31 * HUFF_CODE_SIZE,	HUFF_CODE_SIZE
FIELD	_lit_len_table,	513 * HUFF_CODE_SIZE,	HUFF_CODE_SIZE

%assign _hufftables_icf_size	_FIELD_OFFSET
%assign _hufftables_icf_align	_STRUCT_ALIGN

;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;

START_FIELDS	;; hash8k_buf

;;      name		size    align
FIELD	_hash8k_table,	2 * IGZIP_HASH8K_HASH_SIZE,	2

%assign _hash_buf1_size	_FIELD_OFFSET
%assign _hash_buf1_align	_STRUCT_ALIGN

;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;

START_FIELDS	;; hash_map_buf

;;      name		size    align
FIELD	_hash_table,	2 * IGZIP_HASH_MAP_HASH_SIZE,	2
FIELD	_matches_next,	8,	8
FIELD	_matches_end,	8,	8
FIELD	_matches,	4*4*1024,	4
FIELD	_overflow,	4*LA,	4

%assign _hash_map_buf_size	_FIELD_OFFSET
%assign _hash_map_buf_align	_STRUCT_ALIGN

;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;

%define DEF_MAX_HDR_SIZE 328
START_FIELDS	;; level_buf

;;      name		size    align
FIELD	_encode_tables,		_hufftables_icf_size,	_hufftables_icf_align
FIELD	_hist,		_isal_mod_hist_size, _isal_mod_hist_align
FIELD	_deflate_hdr_count,	4,	4
FIELD	_deflate_hdr_extra_bits,4,	4
FIELD	_deflate_hdr,		DEF_MAX_HDR_SIZE,	1
FIELD	_icf_buf_next,		8,	8
FIELD	_icf_buf_avail_out,	8,	8
FIELD	_icf_buf_start,		8,	8
FIELD	_lvl_extra,		_hash_map_buf_size,	_hash_map_buf_align

%assign _level_buf_base_size	_FIELD_OFFSET
%assign _level_buf_base_align	_STRUCT_ALIGN

_hash8k_hash_table	equ	_lvl_extra + _hash8k_table
_hash_map_hash_table	equ	_lvl_extra + _hash_table
_hash_map_matches_next	equ	_lvl_extra + _matches_next
_hash_map_matches_end	equ	_lvl_extra + _matches_end
_hash_map_matches	equ	_lvl_extra + _matches
_hist_lit_len		equ	_hist+_ll_hist
_hist_dist		equ	_hist+_d_hist

;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;

START_FIELDS	;; isal_zstate

;;      name		size    align
FIELD	_total_in_start,4,	4
FIELD	_block_next,	4,	4
FIELD	_block_end,	4,	4
FIELD	_dist_mask,	4,	4
%ifdef QPL_LIB
; Additional struct fields to limit offset/length for indexing operation
FIELD   _max_dist,      4,      4
FIELD   _mb_mask,       4,      4
%endif
FIELD	_hash_mask,	4,	4
FIELD	_state,		4,	4
FIELD	_bitbuf,	_BitBuf2_size,	_BitBuf2_align
FIELD	_crc,		4,	4
FIELD	_has_wrap_hdr,	1,	1
FIELD	_has_eob_hdr,	1,	1
FIELD	_has_eob,	1,	1
FIELD	_has_hist,	1,	1
FIELD	_has_level_buf_init,	2,	2
FIELD	_count,		4,	4
FIELD   _tmp_out_buff,	16,	1
FIELD   _tmp_out_start,	4,	4
FIELD	_tmp_out_end,	4,	4
FIELD	_b_bytes_valid,	4,	4
FIELD	_b_bytes_processed,	4,	4
FIELD	_buffer,	BSIZE,	1
FIELD	_head,		IGZIP_LVL0_HASH_SIZE*2,	2
%assign _isal_zstate_size	_FIELD_OFFSET
%assign _isal_zstate_align	_STRUCT_ALIGN

_bitbuf_m_bits		equ	_bitbuf+_m_bits
_bitbuf_m_bit_count	equ	_bitbuf+_m_bit_count
_bitbuf_m_out_buf	equ	_bitbuf+_m_out_buf
_bitbuf_m_out_end	equ	_bitbuf+_m_out_end
_bitbuf_m_out_start	equ	_bitbuf+_m_out_start

;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;

START_FIELDS	;; isal_zstream

;;      name		size    align
FIELD	_next_in,	8,	8
FIELD	_avail_in,	4,	4
FIELD	_total_in,	4,	4
FIELD	_next_out,	8,	8
FIELD	_avail_out,	4,	4
FIELD	_total_out,	4,	4
FIELD	_hufftables,	8,	8
FIELD	_level,		4,	4
FIELD	_level_buf_size,	4,	4
FIELD	_level_buf,	8,	8
FIELD	_end_of_stream,	2,	2
FIELD   _flush,		2,	2
FIELD	_gzip_flag,	2,	2
FIELD	_hist_bits,	2,	2
%ifdef QPL_LIB
FIELD	_huffman_only_flag,	2,	2
FIELD	_canned_mode_flag,	2,	2
%endif
FIELD	_internal_state,	_isal_zstate_size,	_isal_zstate_align

%assign _isal_zstream_size	_FIELD_OFFSET
%assign _isal_zstream_align	_STRUCT_ALIGN

_internal_state_total_in_start		equ	_internal_state+_total_in_start
_internal_state_block_next		equ	_internal_state+_block_next
_internal_state_block_end		equ	_internal_state+_block_end
_internal_state_b_bytes_valid		  equ   _internal_state+_b_bytes_valid
_internal_state_b_bytes_processed	 equ   _internal_state+_b_bytes_processed
_internal_state_crc			  equ   _internal_state+_crc
_internal_state_dist_mask		  equ   _internal_state+_dist_mask
_internal_state_hash_mask		  equ   _internal_state+_hash_mask
%ifdef QPL_LIB
; Additional struct fields to limit offset/length for indexing operation
_internal_state_max_dist		  equ   _internal_state+_max_dist
_internal_state_mb_mask 		  equ   _internal_state+_mb_mask
%endif
_internal_state_bitbuf			  equ   _internal_state+_bitbuf
_internal_state_state			  equ   _internal_state+_state
_internal_state_count			  equ   _internal_state+_count
_internal_state_tmp_out_buff		  equ   _internal_state+_tmp_out_buff
_internal_state_tmp_out_start		  equ   _internal_state+_tmp_out_start
_internal_state_tmp_out_end		  equ   _internal_state+_tmp_out_end
_internal_state_has_wrap_hdr		  equ   _internal_state+_has_wrap_hdr
_internal_state_has_eob		  equ   _internal_state+_has_eob
_internal_state_has_eob_hdr		  equ   _internal_state+_has_eob_hdr
_internal_state_has_hist		  equ   _internal_state+_has_hist
_internal_state_has_level_buf_init	  equ   _internal_state+_has_level_buf_init
_internal_state_buffer			  equ   _internal_state+_buffer
_internal_state_head			  equ   _internal_state+_head
_internal_state_bitbuf_m_bits		  equ   _internal_state+_bitbuf_m_bits
_internal_state_bitbuf_m_bit_count	equ   _internal_state+_bitbuf_m_bit_count
_internal_state_bitbuf_m_out_buf	  equ   _internal_state+_bitbuf_m_out_buf
_internal_state_bitbuf_m_out_end	  equ   _internal_state+_bitbuf_m_out_end
_internal_state_bitbuf_m_out_start	equ   _internal_state+_bitbuf_m_out_start

;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
;; Internal States
ZSTATE_NEW_HDR			equ	0
ZSTATE_HDR			equ	(ZSTATE_NEW_HDR + 1)
ZSTATE_CREATE_HDR		equ	(ZSTATE_HDR + 1)
ZSTATE_BODY			equ	(ZSTATE_CREATE_HDR + 1)
ZSTATE_FLUSH_READ_BUFFER	equ	(ZSTATE_BODY + 1)
ZSTATE_FLUSH_ICF_BUFFER		equ	(ZSTATE_FLUSH_READ_BUFFER + 1)
ZSTATE_TYPE0_HDR		equ	(ZSTATE_FLUSH_ICF_BUFFER + 1)
ZSTATE_TYPE0_BODY		equ	(ZSTATE_TYPE0_HDR + 1)
ZSTATE_SYNC_FLUSH		equ	(ZSTATE_TYPE0_BODY + 1)
ZSTATE_FLUSH_WRITE_BUFFER	equ	(ZSTATE_SYNC_FLUSH + 1)
ZSTATE_TRL			equ	(ZSTATE_FLUSH_WRITE_BUFFER + 1)

;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
_NO_FLUSH		equ 0
_SYNC_FLUSH		equ 1
_FULL_FLUSH		equ 2
%ifdef QPL_LIB
_QPL_PARTIAL_FLUSH      equ 3
%endif
_STORED_BLK		equ 0
%assign _STORED_BLK_END 65535
;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
IGZIP_NO_HIST		equ 0
IGZIP_HIST		equ 1
IGZIP_DICT_HIST		equ 2

;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
