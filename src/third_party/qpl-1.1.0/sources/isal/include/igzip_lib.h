/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

#ifndef _IGZIP_H
#define _IGZIP_H

/**
 * @file igzip_lib.h
 *
 * @brief This file defines the igzip compression and decompression interface, a
 * high performance deflate compression interface for storage applications.
 *
 * Deflate is a widely used compression standard that can be used standalone, it
 * also forms the basis of gzip and zlib compression formats. Igzip supports the
 * following flush features:
 *
 * - No Flush: The default method where no special flush is performed.
 *
 * - Sync flush: whereby isal_deflate() finishes the current deflate block at
 *   the end of each input buffer. The deflate block is byte aligned by
 *   appending an empty stored block.
 *
 * - Full flush: whereby isal_deflate() finishes and aligns the deflate block as
 *   in sync flush but also ensures that subsequent block's history does not
 *   look back beyond this point and new blocks are fully independent.
 *
 * Igzip also supports compression levels from ISAL_DEF_MIN_LEVEL to
 * ISAL_DEF_MAX_LEVEL.
 *
 * Igzip contains some behavior configurable at compile time. These
 * configurable options are:
 *
 * - IGZIP_HIST_SIZE - Defines the window size. The default value is 32K (note K
 *   represents 1024), but 8K is also supported. Powers of 2 which are at most
 *   32K may also work.
 *
 * - LONGER_HUFFTABLES - Defines whether to use a larger hufftables structure
 *   which may increase performance with smaller IGZIP_HIST_SIZE values. By
 *   default this option is not defined. This define sets IGZIP_HIST_SIZE to be
 *   8 if IGZIP_HIST_SIZE > 8K.
 *
 *   As an example, to compile gzip with an 8K window size, in a terminal run
 *   @verbatim gmake D="-D IGZIP_HIST_SIZE=8*1024" @endverbatim on Linux and
 *   FreeBSD, or with @verbatim nmake -f Makefile.nmake D="-D
 *   IGZIP_HIST_SIZE=8*1024" @endverbatim on Windows.
 *
 */
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/******************************************************************************/
/* Deflate Compression Standard Defines */
/******************************************************************************/
#define IGZIP_K  1024
#define ISAL_DEF_MAX_HDR_SIZE 328
#define ISAL_DEF_MAX_CODE_LEN 15
#define ISAL_DEF_HIST_SIZE (32*IGZIP_K)
#define ISAL_DEF_MAX_HIST_BITS 15
#define ISAL_DEF_MAX_MATCH 258
#define ISAL_DEF_MIN_MATCH 3

#define ISAL_DEF_LIT_SYMBOLS 257
#define ISAL_DEF_LEN_SYMBOLS 29
#define ISAL_DEF_DIST_SYMBOLS 30
#define ISAL_DEF_LIT_LEN_SYMBOLS (ISAL_DEF_LIT_SYMBOLS + ISAL_DEF_LEN_SYMBOLS)

/* Max repeat length, rounded up to 32 byte boundary */
#define ISAL_LOOK_AHEAD ((ISAL_DEF_MAX_MATCH + 31) & ~31)

/******************************************************************************/
/* Deflate Implementation Specific Defines */
/******************************************************************************/
/* Note IGZIP_HIST_SIZE must be a power of two */
#ifndef IGZIP_HIST_SIZE
#define IGZIP_HIST_SIZE ISAL_DEF_HIST_SIZE
#endif

#if (IGZIP_HIST_SIZE > ISAL_DEF_HIST_SIZE)
#undef IGZIP_HIST_SIZE
#define IGZIP_HIST_SIZE ISAL_DEF_HIST_SIZE
#endif

#ifdef LONGER_HUFFTABLE
#if (IGZIP_HIST_SIZE > 8 * IGZIP_K)
#undef IGZIP_HIST_SIZE
#define IGZIP_HIST_SIZE (8 * IGZIP_K)
#endif
#endif

#define ISAL_LIMIT_HASH_UPDATE

#define IGZIP_HASH8K_HASH_SIZE (8 * IGZIP_K)
#define IGZIP_HASH_HIST_SIZE IGZIP_HIST_SIZE
#define IGZIP_HASH_MAP_HASH_SIZE IGZIP_HIST_SIZE

#define IGZIP_LVL0_HASH_SIZE  (8 * IGZIP_K)
#define IGZIP_LVL1_HASH_SIZE  IGZIP_HASH8K_HASH_SIZE
#define IGZIP_LVL2_HASH_SIZE  IGZIP_HASH_HIST_SIZE
#define IGZIP_LVL3_HASH_SIZE  IGZIP_HASH_MAP_HASH_SIZE

#ifdef LONGER_HUFFTABLE
enum {IGZIP_DIST_TABLE_SIZE = 8*1024};

/* DECODE_OFFSET is dist code index corresponding to DIST_TABLE_SIZE + 1 */
enum { IGZIP_DECODE_OFFSET = 26 };
#else
enum {IGZIP_DIST_TABLE_SIZE = 2};
/* DECODE_OFFSET is dist code index corresponding to DIST_TABLE_SIZE + 1 */
enum { IGZIP_DECODE_OFFSET = 0 };
#endif
enum {IGZIP_LEN_TABLE_SIZE = 256};
enum {IGZIP_LIT_TABLE_SIZE = ISAL_DEF_LIT_SYMBOLS};

#define IGZIP_HUFFTABLE_CUSTOM 0
#define IGZIP_HUFFTABLE_DEFAULT 1
#define IGZIP_HUFFTABLE_STATIC 2

/* Flush Flags */
#define NO_FLUSH	0	/* Default */
#define SYNC_FLUSH	1
#define FULL_FLUSH	2
#define FINISH_FLUSH	0	/* Deprecated */

#if defined(QPL_LIB)
#define QPL_PARTIAL_FLUSH 3
#endif

/* Gzip Flags */
#define IGZIP_DEFLATE	0	/* Default */
#define IGZIP_GZIP	1
#define IGZIP_GZIP_NO_HDR	2
#define IGZIP_ZLIB	3
#define IGZIP_ZLIB_NO_HDR	4

/* Compression Return values */
#define COMP_OK 0
#define INVALID_FLUSH -7
#define INVALID_PARAM -8
#define STATELESS_OVERFLOW -1
#define ISAL_INVALID_OPERATION -9
#define ISAL_INVALID_STATE -3
#define ISAL_INVALID_LEVEL -4	/* Invalid Compression level set */
#define ISAL_INVALID_LEVEL_BUF -5 /* Invalid buffer specified for the compression level */

/**
 *  @enum isal_zstate_state
 *  @brief Compression State please note ZSTATE_TRL only applies for GZIP compression
 */


/* When the state is set to ZSTATE_NEW_HDR or TMP_ZSTATE_NEW_HEADER, the
 * hufftable being used for compression may be swapped
 */
enum isal_zstate_state {
	ZSTATE_NEW_HDR,  //!< Header to be written
	ZSTATE_HDR,	//!< Header state
	ZSTATE_CREATE_HDR, //!< Header to be created
	ZSTATE_BODY,	//!< Body state
	ZSTATE_FLUSH_READ_BUFFER, //!< Flush buffer
	ZSTATE_FLUSH_ICF_BUFFER,
	ZSTATE_TYPE0_HDR, //! Type0 block header to be written
	ZSTATE_TYPE0_BODY, //!< Type0 block body to be written
	ZSTATE_SYNC_FLUSH, //!< Write sync flush block
	ZSTATE_FLUSH_WRITE_BUFFER, //!< Flush bitbuf
	ZSTATE_TRL,	//!< Trailer state
	ZSTATE_END,	//!< End state
	ZSTATE_TMP_NEW_HDR, //!< Temporary Header to be written
	ZSTATE_TMP_HDR,	//!< Temporary Header state
	ZSTATE_TMP_CREATE_HDR, //!< Temporary Header to be created state
	ZSTATE_TMP_BODY,	//!< Temporary Body state
	ZSTATE_TMP_FLUSH_READ_BUFFER, //!< Flush buffer
	ZSTATE_TMP_FLUSH_ICF_BUFFER,
	ZSTATE_TMP_TYPE0_HDR, //! Temporary Type0 block header to be written
	ZSTATE_TMP_TYPE0_BODY, //!< Temporary Type0 block body to be written
	ZSTATE_TMP_SYNC_FLUSH, //!< Write sync flush block
	ZSTATE_TMP_FLUSH_WRITE_BUFFER, //!< Flush bitbuf
	ZSTATE_TMP_TRL,	//!< Temporary Trailer state
	ZSTATE_TMP_END	//!< Temporary End state
};

/* Offset used to switch between TMP states and non-tmp states */
#define ZSTATE_TMP_OFFSET ZSTATE_TMP_HDR - ZSTATE_HDR

/******************************************************************************/
/* Inflate Implementation Specific Defines */
/******************************************************************************/
#define ISAL_DECODE_LONG_BITS 12
#define ISAL_DECODE_SHORT_BITS 10

/* Current state of decompression */
enum isal_block_state {
	ISAL_BLOCK_NEW_HDR,	/* Just starting a new block */
	ISAL_BLOCK_HDR,		/* In the middle of reading in a block header */
	ISAL_BLOCK_TYPE0,	/* Decoding a type 0 block */
	ISAL_BLOCK_CODED,	/* Decoding a huffman coded block */
	ISAL_BLOCK_INPUT_DONE,	/* Decompression of input is completed */
	ISAL_BLOCK_FINISH,	/* Decompression of input is completed and all data has been flushed to output */
	ISAL_GZIP_EXTRA_LEN,
	ISAL_GZIP_EXTRA,
	ISAL_GZIP_NAME,
	ISAL_GZIP_COMMENT,
	ISAL_GZIP_HCRC,
	ISAL_ZLIB_DICT,
	ISAL_CHECKSUM_CHECK,
};

#if defined(QPL_LIB)
/**
 * @briеf Defines type (lit/len or distance) of input code in dynamic header
 * 
 */
typedef enum
{
    llTable,
    dTable
} OwnTableType; 

enum inflate_end_proc
{
    DECOMP_STOP_AND_CHECK_FOR_BFINAL_EOB = 0,
    DECOMP_DONT_STOP_OR_CHECK = 1,
    DECOMP_STOP_AND_CHECK_FOR_ANY_EOB = 2,
    DECOMP_STOP_ON_ANY_EOB = 3,
    DECOMP_STOP_ON_BFINAL_EOB = 4,
    DECOMP_CHECK_FOR_ANY_EOB = 5,
    DECOMP_CHECK_FOR_BFINAL_EOB = 6,
    DECOMP_CHECK_ON_NONLAST_BLOCK = 8
};
#endif


/* Inflate Flags */
#define ISAL_DEFLATE	0	/* Default */
#define ISAL_GZIP	1
#define ISAL_GZIP_NO_HDR	2
#define ISAL_ZLIB	3
#define ISAL_ZLIB_NO_HDR	4
#define ISAL_ZLIB_NO_HDR_VER	5
#define ISAL_GZIP_NO_HDR_VER	6

/* Inflate Return values */
#define ISAL_DECOMP_OK 0	/* No errors encountered while decompressing */
#define ISAL_END_INPUT 1	/* End of input reached */
#define ISAL_OUT_OVERFLOW 2	/* End of output reached */
#define ISAL_NAME_OVERFLOW 3	/* End of gzip name buffer reached */
#define ISAL_COMMENT_OVERFLOW 4	/* End of gzip name buffer reached */
#define ISAL_EXTRA_OVERFLOW 5	/* End of extra buffer reached */
#define ISAL_NEED_DICT 6 /* Stream needs a dictionary to continue */
#define ISAL_INVALID_BLOCK -1	/* Invalid deflate block found */
#define ISAL_INVALID_SYMBOL -2	/* Invalid deflate symbol found */
#define ISAL_INVALID_LOOKBACK -3	/* Invalid lookback distance found */
#define ISAL_INVALID_WRAPPER -4 /* Invalid gzip/zlib wrapper found */
#define ISAL_UNSUPPORTED_METHOD -5	/* Gzip/zlib wrapper specifies unsupported compress method */
#define ISAL_INCORRECT_CHECKSUM -6 /* Incorrect checksum found */
#if defined(QPL_LIB)
#define QPL_MAX_CL_HUFFMAN_CODE_LEN             7u
#define QPL_MAX_LL_D_HUFFMAN_CODE_LEN           15u
#define QPL_32U_SIGN_BIT_MASK                   0x80000000u
#define QPL_HUFFMAN_ONLY_TOKENS_COUNT           256u /* All literals except for EOB symbol */
#define QPL_HW_BASE_CODE                        -200 /* Emulated HW error base */
#define QPL_AD_ERROR_CODE_BIGHDR                (QPL_HW_BASE_CODE - 1)
#define QPL_AD_ERROR_CODE_UNDEF_CL_CODE         (QPL_HW_BASE_CODE - 2)
#define QPL_AD_ERROR_CODE_FIRST_LL_CODE_16      (QPL_HW_BASE_CODE - 3)
#define QPL_AD_ERROR_CODE_FIRST_D_CODE_16       (QPL_HW_BASE_CODE - 4)
#define QPL_AD_ERROR_CODE_NO_LL_CODE            (QPL_HW_BASE_CODE - 5)
#define QPL_AD_ERROR_CODE_WRONG_NUM_LL_CODES    (QPL_HW_BASE_CODE - 6)
#define QPL_AD_ERROR_CODE_WRONG_NUM_DIST_CODES  (QPL_HW_BASE_CODE - 7)
#define QPL_AD_ERROR_CODE_BAD_CL_CODE_LEN       (QPL_HW_BASE_CODE - 8)
#define QPL_AD_ERROR_CODE_BAD_LL_CODE_LEN       (QPL_HW_BASE_CODE - 9)
#define QPL_AD_ERROR_CODE_BAD_DIST_CODE_LEN     (QPL_HW_BASE_CODE - 10)
#define QPL_AD_ERROR_CODE_BAD_LL_CODE           (QPL_HW_BASE_CODE - 11)
#define QPL_AD_ERROR_CODE_BAD_D_CODE            (QPL_HW_BASE_CODE - 12)
#define QPL_AD_ERROR_CODE_INVALID_BLOCK_TYPE    (QPL_HW_BASE_CODE - 13)
#define QPL_AD_ERROR_CODE_INVALID_STORED_LEN    (QPL_HW_BASE_CODE - 14)
#define QPL_AD_ERROR_CODE_BAD_EOF               (QPL_HW_BASE_CODE - 15)
#define QPL_AD_ERROR_CODE_BAD_DIST              (QPL_HW_BASE_CODE - 17)
#define QPL_AD_ERROR_CODE_REF_BEFORE_START      (QPL_HW_BASE_CODE - 18)
#define QPL_AD_ERROR_MINI_BLOCK_OVERWRITE       (QPL_HW_BASE_CODE - 26)
#define QPL_AD_ERROR_MINI_BLOCK_OVERREAD        (QPL_HW_BASE_CODE - 27)
#define QPL_AD_ERROR_MINI_BLOCK_BAD_SIZE        (QPL_HW_BASE_CODE - 28)
#endif

/******************************************************************************/
/* Compression structures */
/******************************************************************************/
/** @brief Holds histogram of deflate symbols*/
struct isal_huff_histogram {
	uint64_t lit_len_histogram[ISAL_DEF_LIT_LEN_SYMBOLS]; //!< Histogram of Literal/Len symbols seen
	uint64_t dist_histogram[ISAL_DEF_DIST_SYMBOLS]; //!< Histogram of Distance Symbols seen
	uint16_t hash_table[IGZIP_LVL0_HASH_SIZE]; //!< Tmp space used as a hash table
};

struct isal_mod_hist {
    uint32_t d_hist[30];
    uint32_t ll_hist[513];
};

#define ISAL_DEF_MIN_LEVEL 0
#define ISAL_DEF_MAX_LEVEL 3

/* Defines used set level data sizes */
/* has to be at least sizeof(struct level_buf) + sizeof(struct lvlX_buf */
#define ISAL_DEF_LVL0_REQ 0
#define ISAL_DEF_LVL1_REQ (4 * IGZIP_K + 2 * IGZIP_LVL1_HASH_SIZE)
#define ISAL_DEF_LVL1_TOKEN_SIZE 4
#define ISAL_DEF_LVL2_REQ (4 * IGZIP_K + 2 * IGZIP_LVL2_HASH_SIZE)
#define ISAL_DEF_LVL2_TOKEN_SIZE 4
#define ISAL_DEF_LVL3_REQ 4 * IGZIP_K + 4 * 4 * IGZIP_K + 2 * IGZIP_LVL3_HASH_SIZE
#define ISAL_DEF_LVL3_TOKEN_SIZE 4

/* Data sizes for level specific data options */
#define ISAL_DEF_LVL0_MIN ISAL_DEF_LVL0_REQ
#define ISAL_DEF_LVL0_SMALL ISAL_DEF_LVL0_REQ
#define ISAL_DEF_LVL0_MEDIUM ISAL_DEF_LVL0_REQ
#define ISAL_DEF_LVL0_LARGE ISAL_DEF_LVL0_REQ
#define ISAL_DEF_LVL0_EXTRA_LARGE ISAL_DEF_LVL0_REQ
#define ISAL_DEF_LVL0_DEFAULT ISAL_DEF_LVL0_REQ

#define ISAL_DEF_LVL1_MIN (ISAL_DEF_LVL1_REQ + ISAL_DEF_LVL1_TOKEN_SIZE * 1 * IGZIP_K)
#define ISAL_DEF_LVL1_SMALL (ISAL_DEF_LVL1_REQ + ISAL_DEF_LVL1_TOKEN_SIZE * 16 * IGZIP_K)
#define ISAL_DEF_LVL1_MEDIUM (ISAL_DEF_LVL1_REQ + ISAL_DEF_LVL1_TOKEN_SIZE * 32 * IGZIP_K)
#define ISAL_DEF_LVL1_LARGE (ISAL_DEF_LVL1_REQ + ISAL_DEF_LVL1_TOKEN_SIZE * 64 * IGZIP_K)
#define ISAL_DEF_LVL1_EXTRA_LARGE (ISAL_DEF_LVL1_REQ + ISAL_DEF_LVL1_TOKEN_SIZE * 128 * IGZIP_K)
#define ISAL_DEF_LVL1_DEFAULT ISAL_DEF_LVL1_LARGE

#define ISAL_DEF_LVL2_MIN (ISAL_DEF_LVL2_REQ + ISAL_DEF_LVL2_TOKEN_SIZE * 1 * IGZIP_K)
#define ISAL_DEF_LVL2_SMALL (ISAL_DEF_LVL2_REQ + ISAL_DEF_LVL2_TOKEN_SIZE * 16 * IGZIP_K)
#define ISAL_DEF_LVL2_MEDIUM (ISAL_DEF_LVL2_REQ + ISAL_DEF_LVL2_TOKEN_SIZE * 32 * IGZIP_K)
#define ISAL_DEF_LVL2_LARGE (ISAL_DEF_LVL2_REQ + ISAL_DEF_LVL2_TOKEN_SIZE * 64 * IGZIP_K)
#define ISAL_DEF_LVL2_EXTRA_LARGE (ISAL_DEF_LVL2_REQ + ISAL_DEF_LVL2_TOKEN_SIZE * 128 * IGZIP_K)
#define ISAL_DEF_LVL2_DEFAULT ISAL_DEF_LVL2_LARGE

#define ISAL_DEF_LVL3_MIN (ISAL_DEF_LVL3_REQ + ISAL_DEF_LVL3_TOKEN_SIZE * 1 * IGZIP_K)
#define ISAL_DEF_LVL3_SMALL (ISAL_DEF_LVL3_REQ + ISAL_DEF_LVL3_TOKEN_SIZE * 16 * IGZIP_K)
#define ISAL_DEF_LVL3_MEDIUM (ISAL_DEF_LVL3_REQ + ISAL_DEF_LVL3_TOKEN_SIZE * 32 * IGZIP_K)
#define ISAL_DEF_LVL3_LARGE (ISAL_DEF_LVL3_REQ + ISAL_DEF_LVL3_TOKEN_SIZE * 64 * IGZIP_K)
#define ISAL_DEF_LVL3_EXTRA_LARGE (ISAL_DEF_LVL3_REQ + ISAL_DEF_LVL3_TOKEN_SIZE * 128 * IGZIP_K)
#define ISAL_DEF_LVL3_DEFAULT ISAL_DEF_LVL3_LARGE

#define IGZIP_NO_HIST 0
#define IGZIP_HIST 1
#define IGZIP_DICT_HIST 2
#define IGZIP_DICT_HASH_SET 3

/** @brief Holds Bit Buffer information*/
struct BitBuf2 {
	uint64_t m_bits;	//!< bits in the bit buffer
	uint32_t m_bit_count;	//!< number of valid bits in the bit buffer
	uint8_t *m_out_buf;	//!< current index of buffer to write to
	uint8_t *m_out_end;	//!< end of buffer to write to
	uint8_t *m_out_start;	//!< start of buffer to write to
};

struct isal_zlib_header {
	uint32_t info;		//!< base-2 logarithm of the LZ77 window size minus 8
	uint32_t level;		//!< Compression level (fastest, fast, default, maximum)
	uint32_t dict_id;	//!< Dictionary id
	uint32_t dict_flag;	//!< Whether to use a dictionary
};

struct isal_gzip_header {
	uint32_t text;		//!< Optional Text hint
	uint32_t time;		//!< Unix modification time in gzip header
	uint32_t xflags;		//!< xflags in gzip header
	uint32_t os;		//!< OS in gzip header
	uint8_t *extra;		//!< Extra field in gzip header
	uint32_t extra_buf_len;	//!< Length of extra buffer
	uint32_t extra_len;	//!< Actual length of gzip header extra field
	char *name;		//!< Name in gzip header
	uint32_t name_buf_len;	//!< Length of name buffer
	char *comment;		//!< Comments in gzip header
	uint32_t comment_buf_len;	//!< Length of comment buffer
	uint32_t hcrc;		//!< Header crc or header crc flag
	uint32_t flags;		//!< Internal data
};

/* Variable prefixes:
 * b_ : Measured wrt the start of the buffer
 * f_ : Measured wrt the start of the file (aka file_start)
 */

/** @brief Holds the internal state information for input and output compression streams*/
struct isal_zstate {
	uint32_t total_in_start; //!< Not used, may be replaced with something else
	uint32_t block_next;	//!< Start of current deflate block in the input
	uint32_t block_end;	//!< End of current deflate block in the input
	uint32_t dist_mask;	//!< Distance mask used.
#if defined(QPL_LIB)
    uint32_t max_dist;  //!< For indexing feature: maximum distance (distance from last mini-block boundary)
    uint32_t mb_mask;   //!< For indexing feature: size of mini-block
#endif
	uint32_t hash_mask;
	enum isal_zstate_state state;	//!< Current state in processing the data stream
	struct BitBuf2 bitbuf;	//!< Bit Buffer
	uint32_t crc;		//!< Current checksum without finalize step if any (adler)
	uint8_t has_wrap_hdr;	//!< keeps track of wrapper header
	uint8_t has_eob_hdr;	//!< keeps track of eob hdr (with BFINAL set)
	uint8_t has_eob;	//!< keeps track of eob on the last deflate block
	uint8_t has_hist;	//!< flag to track if there is match history
	uint16_t has_level_buf_init; //!< flag to track if user supplied memory has been initialized.
	uint32_t count;	//!< used for partial header/trailer writes
	uint8_t tmp_out_buff[16];	//!< temporary array
	uint32_t tmp_out_start;	//!< temporary variable
	uint32_t tmp_out_end;	//!< temporary variable
	uint32_t b_bytes_valid;	//!< number of valid bytes in buffer
	uint32_t b_bytes_processed;	//!< number of bytes processed in buffer
	uint8_t buffer[2 * IGZIP_HIST_SIZE + ISAL_LOOK_AHEAD];	//!< Internal buffer

	/* Stream should be setup such that the head is cache aligned*/
	uint16_t head[IGZIP_LVL0_HASH_SIZE];	//!< Hash array
};

/** @brief Holds the huffman tree used to huffman encode the input stream **/
struct isal_hufftables {

	uint8_t deflate_hdr[ISAL_DEF_MAX_HDR_SIZE]; //!< deflate huffman tree header
	uint32_t deflate_hdr_count; //!< Number of whole bytes in deflate_huff_hdr
	uint32_t deflate_hdr_extra_bits; //!< Number of bits in the partial byte in header
	uint32_t dist_table[IGZIP_DIST_TABLE_SIZE]; //!< bits 4:0 are the code length, bits 31:5 are the code
	uint32_t len_table[IGZIP_LEN_TABLE_SIZE]; //!< bits 4:0 are the code length, bits 31:5 are the code
	uint16_t lit_table[IGZIP_LIT_TABLE_SIZE]; //!< literal code
	uint8_t lit_table_sizes[IGZIP_LIT_TABLE_SIZE]; //!< literal code length
	uint16_t dcodes[30 - IGZIP_DECODE_OFFSET]; //!< distance code
	uint8_t dcodes_sizes[30 - IGZIP_DECODE_OFFSET]; //!< distance code length

};

/** @brief Holds stream information*/
struct isal_zstream {
	uint8_t *next_in;	//!< Next input byte
	uint32_t avail_in;	//!< number of bytes available at next_in
	uint32_t total_in;	//!< total number of bytes read so far

	uint8_t *next_out;	//!< Next output byte
	uint32_t avail_out;	//!< number of bytes available at next_out
	uint32_t total_out;	//!< total number of bytes written so far

	struct isal_hufftables *hufftables; //!< Huffman encoding used when compressing
	uint32_t level; //!< Compression level to use
	uint32_t level_buf_size; //!< Size of level_buf
	uint8_t * level_buf; //!< User allocated buffer required for different compression levels
	uint16_t end_of_stream;	//!< non-zero if this is the last input buffer
	uint16_t flush;	//!< Flush type can be NO_FLUSH, SYNC_FLUSH or FULL_FLUSH
	uint16_t gzip_flag; //!< Indicate if gzip compression is to be performed
	uint16_t hist_bits; //!< Log base 2 of maximum lookback distance, 0 is use default
#ifdef QPL_LIB
    uint16_t huffman_only_flag; //!< Activate huffman only mode
    uint16_t canned_mode_flag; //!< Activate huffman only mode
#endif
	struct isal_zstate internal_state;	//!< Internal state for this stream
};

/******************************************************************************/
/* Inflate structures */
/******************************************************************************/
/*
 * Inflate_huff_code data structures are used to store a Huffman code for fast
 * lookup. It works by performing a lookup in small_code_lookup that hopefully
 * yields the correct symbol. Otherwise a lookup into long_code_lookup is
 * performed to find the correct symbol. The details of how this works follows:
 *
 * Let i be some index into small_code_lookup and let e be the associated
 * element.  Bit 15 in e is a flag. If bit 15 is not set, then index i contains
 * a Huffman code for a symbol which has length at most DECODE_LOOKUP_SIZE. Bits
 * 0 through 8 are the symbol associated with that code and bits 9 through 12 of
 * e represent the number of bits in the code. If bit 15 is set, the i
 * corresponds to the first DECODE_LOOKUP_SIZE bits of a Huffman code which has
 * length longer than DECODE_LOOKUP_SIZE. In this case, bits 0 through 8
 * represent an offset into long_code_lookup table and bits 9 through 12
 * represent the maximum length of a Huffman code starting with the bits in the
 * index i. The offset into long_code_lookup is for an array associated with all
 * codes which start with the bits in i.
 *
 * The elements of long_code_lookup are in the same format as small_code_lookup,
 * except bit 15 is never set. Let i be a number made up of DECODE_LOOKUP_SIZE
 * bits.  Then all Huffman codes which start with DECODE_LOOKUP_SIZE bits are
 * stored in an array starting at index h in long_code_lookup. This index h is
 * stored in bits 0 through 9 at index i in small_code_lookup. The index j is an
 * index of this array if the number of bits contained in j and i is the number
 * of bits in the longest huff_code starting with the bits of i. The symbol
 * stored at index j is the symbol whose huffcode can be found in (j <<
 * DECODE_LOOKUP_SIZE) | i. Note these arrays will be stored sorted in order of
 * maximum Huffman code length.
 *
 * The following are explanations for sizes of the tables:
 *
 * Since small_code_lookup is a lookup on DECODE_LOOKUP_SIZE bits, it must have
 * size 2^DECODE_LOOKUP_SIZE.
 *
 * To determine the amount of memory required for long_code_lookup, note that
 * any element of long_code_lookup corresponds to a code, a duplicate of an
 * existing code, or a invalid code. Since deflate Huffman are stored such that
 * the code size and the code value form an increasing function, the number of
 * duplicates is maximized when all the duplicates are contained in a single
 * array, thus there are at most 2^(15 - DECODE_LOOKUP_SIZE) -
 * (DECODE_LOOKUP_SIZE + 1) duplicate elements. Similarly the number of invalid
 * elements is maximized at 2^(15 - DECODE_LOOKUP_SIZE) - 2^(floor((15 -
 * DECODE_LOOKUP_SIZE)/2) - 2^(ceil((15 - DECODE_LOOKUP_SIZE)/2) + 1. Thus the
 * amount of memory required is: NUM_CODES + 2^(16 - DECODE_LOOKUP_SIZE) -
 * (DECODE_LOOKUP_SIZE + 1) - 2^(floor((15 - DECODE_LOOKUP_SIZE)/2) -
 * 2^(ceil((15 - DECODE_LOOKUP_SIZE)/2) + 1. The values used below are those
 * values rounded up to the nearest 16 byte boundary
 *
 * Note that DECODE_LOOKUP_SIZE can be any length even though the offset in
 * small_lookup_code is 9 bits long because the increasing relationship between
 * code length and code value forces the maximum offset to be less than 288.
 */

/* In the following defines, L stands for LARGE and S for SMALL */
#define ISAL_L_REM (21 - ISAL_DECODE_LONG_BITS)
#define ISAL_S_REM (15 - ISAL_DECODE_SHORT_BITS)

#define ISAL_L_DUP ((1 << ISAL_L_REM) - (ISAL_L_REM + 1))
#define ISAL_S_DUP ((1 << ISAL_S_REM) - (ISAL_S_REM + 1))

#define ISAL_L_UNUSED ((1 << ISAL_L_REM) - (1 << ((ISAL_L_REM)/2)) - (1 << ((ISAL_L_REM + 1)/2)) + 1)
#define ISAL_S_UNUSED ((1 << ISAL_S_REM) - (1 << ((ISAL_S_REM)/2)) - (1 << ((ISAL_S_REM + 1)/2)) + 1)

#define ISAL_L_SIZE (ISAL_DEF_LIT_LEN_SYMBOLS + ISAL_L_DUP + ISAL_L_UNUSED)
#define ISAL_S_SIZE (ISAL_DEF_DIST_SYMBOLS + ISAL_S_DUP + ISAL_S_UNUSED)

#define ISAL_HUFF_CODE_LARGE_LONG_ALIGNED (ISAL_L_SIZE + (-ISAL_L_SIZE & 0xf))
#define ISAL_HUFF_CODE_SMALL_LONG_ALIGNED (ISAL_S_SIZE + (-ISAL_S_SIZE & 0xf))

/* Large lookup table for decoding huffman codes */
struct inflate_huff_code_large {
	uint32_t short_code_lookup[1 << (ISAL_DECODE_LONG_BITS)];
	uint16_t long_code_lookup[ISAL_HUFF_CODE_LARGE_LONG_ALIGNED];
};

/* Small lookup table for decoding huffman codes */
struct inflate_huff_code_small {
	uint16_t short_code_lookup[1 << (ISAL_DECODE_SHORT_BITS)];
	uint16_t long_code_lookup[ISAL_HUFF_CODE_SMALL_LONG_ALIGNED];
};

/** @brief Holds decompression state information*/
struct inflate_state {
	uint8_t *next_out;	//!< Next output Byte
	uint32_t avail_out;	//!< Number of bytes available at next_out
	uint32_t total_out;	//!< Total bytes written out so far
	uint8_t *next_in;	//!< Next input byte
	uint64_t read_in;	//!< Bits buffered to handle unaligned streams
	uint32_t avail_in;	//!< Number of bytes available at next_in
	int32_t read_in_length;	//!< Bits in read_in
	struct inflate_huff_code_large lit_huff_code;	//!< Structure for decoding lit/len symbols
	struct inflate_huff_code_small dist_huff_code;	//!< Structure for decoding dist symbols
	enum isal_block_state block_state;	//!< Current decompression state
	uint32_t dict_length;	//!< Length of dictionary used
	uint32_t bfinal;	//!< Flag identifying final block
	uint32_t crc_flag;	//!< Flag identifying whether to track of crc
	uint32_t crc;		//!< Contains crc or adler32 of output if crc_flag is set
	uint32_t hist_bits; //!< Log base 2 of maximum lookback distance
	union {
		int32_t type0_block_len;	//!< Length left to read of type 0 block when outbuffer overflow occurred
		int32_t count; //!< Count of bytes remaining to be parsed
		uint32_t dict_id;
	};
	int32_t write_overflow_lits;
	int32_t write_overflow_len;
	int32_t copy_overflow_length; 	//!< Length left to copy when outbuffer overflow occurred
	int32_t copy_overflow_distance;	//!< Lookback distance when outbuffer overflow occurred
#if defined(QPL_LIB)
    uint32_t mini_block_size;       //!< Size of miniblock to decompress.. // TODO add detailes
#endif
	int16_t wrapper_flag;
	int16_t tmp_in_size;	//!< Number of bytes in tmp_in_buffer
	int32_t tmp_out_valid;	//!< Number of bytes in tmp_out_buffer
	int32_t tmp_out_processed;	//!< Number of bytes processed in tmp_out_buffer
	uint8_t tmp_in_buffer[ISAL_DEF_MAX_HDR_SIZE];	//!< Temporary buffer containing data from the input stream
	uint8_t tmp_out_buffer[2 * ISAL_DEF_HIST_SIZE + ISAL_LOOK_AHEAD]; 	//!< Temporary buffer containing data from the output stream
#if defined(QPL_LIB)
    uint32_t eob_code_and_len;
    uint8_t  decomp_end_proc;

    /**
    * The following flag prohibits creating multisymbol fields in lookup table during
    * preparation of the dynamic header. Therefore, at the decompression stage 
    * all deflate tokens will be read one-by-one.
    *
    * This is temporary WORKAROUND for indexing (which is currently used during verification stage),
    * TODO: Delete this flag once indexing is moved to compression stage.
    */
    uint8_t  disable_multisymbol_lookup_table;
#endif
};

/******************************************************************************/
/* Compression functions */
/******************************************************************************/
/**
 * @brief Updates histograms to include the symbols found in the input
 * stream. Since this function only updates the histograms, it can be called on
 * multiple streams to get a histogram better representing the desired data
 * set. When first using histogram it must be initialized by zeroing the
 * structure.
 *
 * @param in_stream: Input stream of data.
 * @param length: The length of start_stream.
 * @param histogram: The returned histogram of lit/len/dist symbols.
 */
void isal_update_histogram(uint8_t * in_stream, int length, struct isal_huff_histogram * histogram);


/**
 * @brief Creates a custom huffman code for the given histograms in which
 *  every literal and repeat length is assigned a code and all possible lookback
 *  distances are assigned a code.
 *
 * @param hufftables: the output structure containing the huffman code
 * @param histogram: histogram containing frequency of literal symbols,
 *        repeat lengths and lookback distances
 * @returns Returns a non zero value if an invalid huffman code was created.
 */
int isal_create_hufftables(struct isal_hufftables * hufftables,
			struct isal_huff_histogram * histogram);

/**
 * @brief Creates a custom huffman code for the given histograms like
 * isal_create_hufftables() except literals with 0 frequency in the histogram
 * are not assigned a code
 *
 * @param hufftables: the output structure containing the huffman code
 * @param histogram: histogram containing frequency of literal symbols,
 *        repeat lengths and lookback distances
 * @returns Returns a non zero value if an invalid huffman code was created.
 */
int isal_create_hufftables_subset(struct isal_hufftables * hufftables,
				struct isal_huff_histogram * histogram);

#if defined(QPL_LIB)
/**
 * @brief Creates a custom huffman code for the given literal histogram (code is
 * assigned for every literal symbol except for EOB symbol),
 * this function doesn't create deflate header.
 *
 * @todo: remove this function, create custom huffman tree creating functions
 *
 * @param hufftables: the output structure containing the huffman code
 * @param histogram: histogram containing frequency of literal symbols
 * @returns Ret
 */
int isal_create_hufftables_literals_only(struct isal_hufftables *hufftables,
                                         struct isal_huff_histogram *histogram);
#endif

/**
 * @brief Initialize compression stream data structure
 *
 * @param stream Structure holding state information on the compression streams.
 * @returns none
 */
void isal_deflate_init(struct isal_zstream *stream);

/**
 * @brief Reinitialize compression stream data structure. Performs the same
 * action as isal_deflate_init, but does not change user supplied input such as
 * the level, flush type, compression wrapper (like gzip), hufftables, and
 * end_of_stream_flag.
 *
 * @param stream Structure holding state information on the compression streams.
 * @returns none
 */
void isal_deflate_reset(struct isal_zstream *stream);


/**
 * @brief Set gzip header default values
 *
 * @param gz_hdr: Gzip header to initialize.
 */
void isal_gzip_header_init(struct isal_gzip_header *gz_hdr);

/**
 * @brief Write gzip header to output stream
 *
 * Writes the gzip header to the output stream. On entry this function assumes
 * that the output buffer has been initialized, so stream->next_out,
 * stream->avail_out and stream->total_out have been set. If the output buffer
 * contains insufficient space, stream is not modified.
 *
 * @param stream: Structure holding state information on the compression stream.
 * @param gz_hdr: Structure holding the gzip header information to encode.
 *
 * @returns Returns 0 if the header is successfully written, otherwise returns
 * the minimum size required to successfully write the gzip header to the output
 * buffer.
 */
uint32_t isal_write_gzip_header(struct isal_zstream * stream, struct isal_gzip_header *gz_hdr);

/**
 * @brief Write zlib header to output stream
 *
 * Writes the zlib header to the output stream. On entry this function assumes
 * that the output buffer has been initialized, so stream->next_out,
 * stream->avail_out and stream->total_out have been set. If the output buffer
 * contains insufficient space, stream is not modified.
 *
 * @param stream: Structure holding state information on the compression stream.
 * @param z_hdr: Structure holding the zlib header information to encode.
 *
 * @returns Returns 0 if the header is successfully written, otherwise returns
 * the minimum size required to successfully write the zlib header to the output
 * buffer.
 */
uint32_t isal_write_zlib_header(struct isal_zstream * stream, struct isal_zlib_header *z_hdr);

/**
 * @brief Set stream to use a new Huffman code
 *
 * Sets the Huffman code to be used in compression before compression start or
 * after the successful completion of a SYNC_FLUSH or FULL_FLUSH. If type has
 * value IGZIP_HUFFTABLE_DEFAULT, the stream is set to use the default Huffman
 * code. If type has value IGZIP_HUFFTABLE_STATIC, the stream is set to use the
 * deflate standard static Huffman code, or if type has value
 * IGZIP_HUFFTABLE_CUSTOM, the stream is set to sue the isal_hufftables
 * structure input to isal_deflate_set_hufftables.
 *
 * @param stream: Structure holding state information on the compression stream.
 * @param hufftables: new huffman code to use if type is set to
 * IGZIP_HUFFTABLE_CUSTOM.
 * @param type: Flag specifying what hufftable to use.
 *
 * @returns Returns INVALID_OPERATION if the stream was unmodified. This may be
 * due to the stream being in a state where changing the huffman code is not
 * allowed or an invalid input is provided.
 */
int isal_deflate_set_hufftables(struct isal_zstream *stream,
				struct isal_hufftables *hufftables, int type);

/**
 * @brief Initialize compression stream data structure
 *
 * @param stream Structure holding state information on the compression streams.
 * @returns none
 */
void isal_deflate_stateless_init(struct isal_zstream *stream);


/**
 * @brief Set compression dictionary to use
 *
 * This function is to be called after isal_deflate_init, or after completing a
 * SYNC_FLUSH or FULL_FLUSH and before the next call do isal_deflate. If the
 * dictionary is longer than IGZIP_HIST_SIZE, only the last IGZIP_HIST_SIZE
 * bytes will be used.
 *
 * @param stream Structure holding state information on the compression streams.
 * @param dict: Array containing dictionary to use.
 * @param dict_len: Length of dict.
 * @returns COMP_OK,
 *          ISAL_INVALID_STATE (dictionary could not be set)
 */
int isal_deflate_set_dict(struct isal_zstream *stream, uint8_t *dict, uint32_t dict_len);

/** @brief Structure for holding processed dictionary information */

struct isal_dict {
	uint32_t params;
	uint32_t level;
	uint32_t hist_size;
	uint32_t hash_size;
	uint8_t history[ISAL_DEF_HIST_SIZE];
	uint16_t hashtable[IGZIP_LVL3_HASH_SIZE];
};

/**
 * @brief Process dictionary to reuse later
 *
 * Processes a dictionary so that the generated output can be reused to reset a
 * new deflate stream more quickly than isal_deflate_set_dict() alone. This
 * function is paired with isal_deflate_reset_dict() when using the same
 * dictionary on multiple deflate objects. The stream.level must be set prior to
 * calling this function to process the dictionary correctly. If the dictionary
 * is longer than IGZIP_HIST_SIZE, only the last IGZIP_HIST_SIZE bytes will be
 * used.
 *
 * @param stream Structure holding state information on the compression streams.
 * @param dict_str: Structure to hold processed dictionary info to reuse later.
 * @param dict: Array containing dictionary to use.
 * @param dict_len: Length of dict.
 * @returns COMP_OK,
 *          ISAL_INVALID_STATE (dictionary could not be processed)
 */
int isal_deflate_process_dict(struct isal_zstream *stream, struct isal_dict *dict_str,
			uint8_t *dict, uint32_t dict_len);

/**
 * @brief Reset compression dictionary to use
 *
 * Similar to isal_deflate_set_dict() but on pre-processed dictionary
 * data. Pairing with isal_deflate_process_dict() can reduce the processing time
 * on subsequent compression with dictionary especially on small files.
 *
 * Like isal_deflate_set_dict(), this function is to be called after
 * isal_deflate_init, or after completing a SYNC_FLUSH or FULL_FLUSH and before
 * the next call do isal_deflate. Changing compression level between dictionary
 * process and reset will cause return of ISAL_INVALID_STATE.
 *
 * @param stream Structure holding state information on the compression streams.
 * @param dict_str: Structure with pre-processed dictionary info.
 * @returns COMP_OK,
 *          ISAL_INVALID_STATE or other (dictionary could not be reset)
 */
int isal_deflate_reset_dict(struct isal_zstream *stream, struct isal_dict *dict_str);


/**
 * @brief Fast data (deflate) compression for storage applications.
 *
 * The call to isal_deflate() will take data from the input buffer (updating
 * next_in, avail_in and write a compressed stream to the output buffer
 * (updating next_out and avail_out). The function returns when either the input
 * buffer is empty or the output buffer is full.
 *
 * On entry to isal_deflate(), next_in points to an input buffer and avail_in
 * indicates the length of that buffer. Similarly next_out points to an empty
 * output buffer and avail_out indicates the size of that buffer.
 *
 * The fields total_in and total_out start at 0 and are updated by
 * isal_deflate(). These reflect the total number of bytes read or written so far.
 *
 * When the last input buffer is passed in, signaled by setting the
 * end_of_stream, the routine will complete compression at the end of the input
 * buffer, as long as the output buffer is big enough.
 *
 * The compression level can be set by setting level to any value between
 * ISAL_DEF_MIN_LEVEL and ISAL_DEF_MAX_LEVEL. When the compression level is
 * ISAL_DEF_MIN_LEVEL, hufftables can be set to a table trained for the the
 * specific data type being compressed to achieve better compression. When a
 * higher compression level is desired, a larger generic memory buffer needs to
 * be supplied by setting level_buf and level_buf_size to represent the chunk of
 * memory. For level x, the suggest size for this buffer this buffer is
 * ISAL_DEFL_LVLx_DEFAULT. The defines ISAL_DEFL_LVLx_MIN, ISAL_DEFL_LVLx_SMALL,
 * ISAL_DEFL_LVLx_MEDIUM, ISAL_DEFL_LVLx_LARGE, and ISAL_DEFL_LVLx_EXTRA_LARGE
 * are also provided as other suggested sizes.
 *
 * The equivalent of the zlib FLUSH_SYNC operation is currently supported.
 * Flush types can be NO_FLUSH, SYNC_FLUSH or FULL_FLUSH. Default flush type is
 * NO_FLUSH. A SYNC_ OR FULL_ flush will byte align the deflate block by
 * appending an empty stored block once all input has been compressed, including
 * the buffered input. Checking that the out_buffer is not empty or that
 * internal_state.state = ZSTATE_NEW_HDR is sufficient to guarantee all input
 * has been flushed. Additionally FULL_FLUSH will ensure look back history does
 * not include previous blocks so new blocks are fully independent. Switching
 * between flush types is supported.
 *
 * If a compression dictionary is required, the dictionary can be set calling
 * isal_deflate_set_dictionary before calling isal_deflate.
 *
 * If the gzip_flag is set to IGZIP_GZIP, a generic gzip header and the gzip
 * trailer are written around the deflate compressed data. If gzip_flag is set
 * to IGZIP_GZIP_NO_HDR, then only the gzip trailer is written. A full-featured
 * header is supported by the isal_write_{gzip,zlib}_header() functions.
 *
 * @param  stream Structure holding state information on the compression streams.
 * @return COMP_OK (if everything is ok),
 *         INVALID_FLUSH (if an invalid FLUSH is selected),
 *         ISAL_INVALID_LEVEL (if an invalid compression level is selected),
 *         ISAL_INVALID_LEVEL_BUF (if the level buffer is not large enough).
 */
int isal_deflate(struct isal_zstream *stream);


/**
 * @brief Fast data (deflate) stateless compression for storage applications.
 *
 * Stateless (one shot) compression routine with a similar interface to
 * isal_deflate() but operates on entire input buffer at one time. Parameter
 * avail_out must be large enough to fit the entire compressed output. Max
 * expansion is limited to the input size plus the header size of a stored/raw
 * block.
 *
 * When the compression level is set to 1, unlike in isal_deflate(), level_buf
 * may be optionally set depending on what what performance is desired.
 *
 * For stateless the flush types NO_FLUSH and FULL_FLUSH are supported.
 * FULL_FLUSH will byte align the output deflate block so additional blocks can
 * be easily appended.
 *
 * If the gzip_flag is set to IGZIP_GZIP, a generic gzip header and the gzip
 * trailer are written around the deflate compressed data. If gzip_flag is set
 * to IGZIP_GZIP_NO_HDR, then only the gzip trailer is written.
 *
 * @param  stream Structure holding state information on the compression streams.
 * @return COMP_OK (if everything is ok),
 *         INVALID_FLUSH (if an invalid FLUSH is selected),
 *         ISAL_INVALID_LEVEL (if an invalid compression level is selected),
 *         ISAL_INVALID_LEVEL_BUF (if the level buffer is not large enough),
 *         STATELESS_OVERFLOW (if output buffer will not fit output).
 */
int isal_deflate_stateless(struct isal_zstream *stream);


/******************************************************************************/
/* Inflate functions */
/******************************************************************************/
/**
 * @brief Initialize decompression state data structure
 *
 * @param state Structure holding state information on the compression streams.
 * @returns none
 */
void isal_inflate_init(struct inflate_state *state);

/**
 * @brief Reinitialize decompression state data structure
 *
 * @param state Structure holding state information on the compression streams.
 * @returns none
 */
void isal_inflate_reset(struct inflate_state *state);

/**
 * @brief Set decompression dictionary to use
 *
 * This function is to be called after isal_inflate_init. If the dictionary is
 * longer than IGZIP_HIST_SIZE, only the last IGZIP_HIST_SIZE bytes will be
 * used.
 *
 * @param state: Structure holding state information on the decompression stream.
 * @param dict: Array containing dictionary to use.
 * @param dict_len: Length of dict.
 * @returns COMP_OK,
 *          ISAL_INVALID_STATE (dictionary could not be set)
 */
int isal_inflate_set_dict(struct inflate_state *state, uint8_t *dict, uint32_t dict_len);

/**
 * @brief Read and return gzip header information
 *
 * On entry state must be initialized and next_in pointing to a gzip compressed
 * buffer. The buffers gz_hdr->extra, gz_hdr->name, gz_hdr->comments and the
 * buffer lengths must be set to record the corresponding field, or set to NULL
 * to disregard that gzip header information. If one of these buffers overflows,
 * the user can reallocate a larger buffer and call this function again to
 * continue reading the header information.
 *
 * @param state: Structure holding state information on the decompression stream.
 * @param gz_hdr: Structure to return data encoded in the gzip header
 * @returns ISAL_DECOMP_OK (header was successfully parsed)
 *          ISAL_END_INPUT (all input was parsed),
 *          ISAL_NAME_OVERFLOW (gz_hdr->name overflowed while parsing),
 *          ISAL_COMMENT_OVERFLOW (gz_hdr->comment overflowed while parsing),
 *          ISAL_EXTRA_OVERFLOW (gz_hdr->extra overflowed while parsing),
 *          ISAL_INVALID_WRAPPER (invalid gzip header found),
 *          ISAL_UNSUPPORTED_METHOD (deflate is not the compression method),
 *          ISAL_INCORRECT_CHECKSUM (gzip header checksum was incorrect)
 */
int isal_read_gzip_header (struct inflate_state *state, struct isal_gzip_header *gz_hdr);

/**
 * @brief Read and return zlib header information
 *
 * On entry state must be initialized and next_in pointing to a zlib compressed
 * buffer.
 *
 * @param state: Structure holding state information on the decompression stream.
 * @param zlib_hdr: Structure to return data encoded in the zlib header
 * @returns ISAL_DECOMP_OK (header was successfully parsed),
 *          ISAL_END_INPUT (all input was parsed),
 *          ISAL_UNSUPPORTED_METHOD (deflate is not the compression method),
 *          ISAL_INCORRECT_CHECKSUM (zlib header checksum was incorrect)
 */
int isal_read_zlib_header (struct inflate_state *state, struct isal_zlib_header *zlib_hdr);

/**
 * @brief Fast data (deflate) decompression for storage applications.
 *
 * On entry to isal_inflate(), next_in points to an input buffer and avail_in
 * indicates the length of that buffer. Similarly next_out points to an empty
 * output buffer and avail_out indicates the size of that buffer.
 *
 * The field total_out starts at 0 and is updated by isal_inflate(). This
 * reflects the total number of bytes written so far.
 *
 * The call to isal_inflate() will take data from the input buffer (updating
 * next_in, avail_in and write a decompressed stream to the output buffer
 * (updating next_out and avail_out). The function returns when the input buffer
 * is empty, the output buffer is full, invalid data is found, or in the case of
 * zlib formatted data if a dictionary is specified. The current state of the
 * decompression on exit can be read from state->block-state.
 *
 * If the crc_flag is set to ISAL_GZIP_NO_HDR the gzip crc of the output is
 * stored in state->crc. Alternatively, if the crc_flag is set to
 * ISAL_ZLIB_NO_HDR the adler32 of the output is stored in state->crc (checksum
 * may not be updated until decompression is complete). When the crc_flag is set
 * to ISAL_GZIP_NO_HDR_VER or ISAL_ZLIB_NO_HDR_VER, the behavior is the same,
 * except the checksum is verified with the checksum after immediately following
 * the deflate data. If the crc_flag is set to ISAL_GZIP or ISAL_ZLIB, the
 * gzip/zlib header is parsed, state->crc is set to the appropriate checksum,
 * and the checksum is verified. If the crc_flag is set to ISAL_DEFLATE
 * (default), then the data is treated as a raw deflate block.
 *
 * The element state->hist_bits has values from 0 to 15, where values of 1 to 15
 * are the log base 2 size of the matching window and 0 is the default with
 * maximum history size.
 *
 * If a dictionary is required, a call to isal_inflate_set_dict will set the
 * dictionary.
 *
 * @param  state Structure holding state information on the compression streams.
 * @return ISAL_DECOMP_OK (if everything is ok),
 *         ISAL_INVALID_BLOCK,
 *         ISAL_NEED_DICT,
 *         ISAL_INVALID_SYMBOL,
 *         ISAL_INVALID_LOOKBACK,
 *         ISAL_INVALID_WRAPPER,
 *         ISAL_UNSUPPORTED_METHOD,
 *         ISAL_INCORRECT_CHECKSUM.
 */

int isal_inflate(struct inflate_state *state);

/**
 * @brief Fast data (deflate) stateless decompression for storage applications.
 *
 * Stateless (one shot) decompression routine with a similar interface to
 * isal_inflate() but operates on entire input buffer at one time. Parameter
 * avail_out must be large enough to fit the entire decompressed
 * output. Dictionaries are not supported.
 *
 * @param  state Structure holding state information on the compression streams.
 * @return ISAL_DECOMP_OK (if everything is ok),
 *         ISAL_END_INPUT (if all input was decompressed),
 *         ISAL_NEED_DICT,
 *         ISAL_OUT_OVERFLOW (if output buffer ran out of space),
 *         ISAL_INVALID_BLOCK,
 *         ISAL_INVALID_SYMBOL,
 *         ISAL_INVALID_LOOKBACK,
 *         ISAL_INVALID_WRAPPER,
 *         ISAL_UNSUPPORTED_METHOD,
 *         ISAL_INCORRECT_CHECKSUM.
 */
int isal_inflate_stateless(struct inflate_state *state);

/******************************************************************************/
/* Other functions */
/******************************************************************************/
/**
 * @brief Calculate Adler-32 checksum, runs appropriate version.
 *
 * This function determines what instruction sets are enabled and selects the
 * appropriate version at runtime.
 *
 * @param init: initial Adler-32 value
 * @param buf: buffer to calculate checksum on
 * @param len: buffer length in bytes
 *
 * @returns 32-bit Adler-32 checksum
 */
uint32_t isal_adler32(uint32_t init, const unsigned char *buf, uint64_t len);

#ifdef QPL_LIB
int read_header(struct inflate_state *state);
int decode_huffman_code_block_stateless(struct inflate_state *s, uint8_t *out);
int decode_huffman_code_block_stateless_base(struct inflate_state* s, uint8_t* out);
int check_gzip_checksum(struct inflate_state *state);
#endif

#ifdef __cplusplus
}
#endif
#endif	/* ifndef _IGZIP_H */
