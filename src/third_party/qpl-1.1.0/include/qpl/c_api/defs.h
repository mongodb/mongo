/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

/*
 *  Intel® Query Processing Library (Intel® QPL)
 *  Job API (public C API)
 */

/**
 * @file
 * @brief Common Types and Macro Definitions for Intel QPL
 *
 * @defgroup JOB_API_DEFINITIONS Definitions
 * @ingroup JOB_API
 * @{
 * @brief General definitions that can be used for task configuration
 */
#ifndef QPL_DEFS_H__
#define QPL_DEFS_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdint.h>

/**
 * @brief Macro to manipulate public API prototypes
 */
#if !defined( QPL_API )
#define QPL_API(type, name, arg) type name arg;
#endif

#if !defined( UNREFERENCED_PARAMETER )
#if defined( __GNUC__ )
#define UNREFERENCED_PARAMETER(p) p __attribute__((unused)) /**< Unreferenced parameter - warning removal */
#else
#define UNREFERENCED_PARAMETER(p) p                         /**< Unreferenced parameter - warning removal */
#endif
#endif

/**
 * Unused variables which might be used later - warning removal
 */
#define MAYBE_UNUSED(x) ((void)x)

/**
 * Marker for the last enumerator of qpl_decomp_end_proc
 */
#define OWN_LAST_INFLATE_MANIPULATOR 8u

/**
 * Reserved enumerator of qpl_decomp_end_proc - hardware does not support this code
 */
#define OWN_RESERVED_INFLATE_MANIPULATOR 7u

/**
 * Reserved enumerator for the compression level of the dictionary
 */
#define DICT_NONE -1 /**< Value for software  */


/**
 * @addtogroup QPL_FLAGS
 * @brief Use this enumeration to indicate various options when submitting a job.
 * @{
 *
 */

/* Common flags */
/**
 * The start of a new overall task (Filter @ref qpl_operation implies flags FIRST and LAST)
 */
#define QPL_FLAG_FIRST 0x0001u

/**
 * The end of an overall task (Filter @ref qpl_operation implies flags FIRST and LAST)
 */
#define QPL_FLAG_LAST 0x0002u

/**
 * Polynomial 0x11edc6f41 is used, which is the one used by iSCSI
 */
#define QPL_FLAG_CRC32C 0x0004u

/**
 * Check and skip GZIP header/trailer
 */
#define QPL_FLAG_GZIP_MODE 0x0008u

/**
 * Huffman tokens are in Big Endian format (both compress and decompress)
 */
#define QPL_FLAG_HUFFMAN_BE 0x0010u

/**
 * Decompress: Parse only Huffman Tokens; Compress: Write no headers or EOBs
 */
#define QPL_FLAG_NO_HDRS 0x0020u

/* Inflate (decompress) flags */
/**
 * The data from the input buffer is decompressed and then sent to the filter unit
 */
#define QPL_FLAG_DECOMPRESS_ENABLE 0x0040u

/**
 * Source-2 is viewed as a Big-Endian packed array
 */
#define QPL_FLAG_SRC2_BE 0x0080u

/**
 * Destination is stored as a Big-Endian packed array
 */
#define QPL_FLAG_OUT_BE 0x0100u

/**
 * One or several mini-blocks are decompressed only
 */
#define QPL_FLAG_RND_ACCESS 0x0200u

/**
 * All processed data should be written to output, otherwise, only complete qwords
 */
#define QPL_FLAG_DECOMP_FLUSH_ALWAYS 0x0800u

/* Deflate (compress) flags */
/**
 * Implied by QPL_FLAG_FIRST, should be used in the middle of the stream to specify a different Huffman table
 */
#define QPL_FLAG_START_NEW_BLOCK 0x1000u

/**
 * The data will be compressed as a single dynamic DEFLATE block; requires 2 passes
 */
#define QPL_FLAG_DYNAMIC_HUFFMAN 0x2000u

/**
 * Turn off verification to improve performance
 */
#define QPL_FLAG_OMIT_VERIFY 0x4000u

/**
 * Compression only: generate only literals
 */
#define QPL_FLAG_GEN_LITERALS 0x8000u

/**
 * Flag to enable canned compression and decompression
 */
#define QPL_FLAG_CANNED_MODE 0x00400000u

/* CRC64 flags */
/**
 * The data should be viewed as Big-Endian
 */
#define QPL_FLAG_CRC64_BE 0x00010000u

/**
 * Bitwise inversion of the initial and the final CRC
 */
#define QPL_FLAG_CRC64_INV 0x00020000u

/**
 * Bitwise inversion of the output bit-vector for a filtering operation
 */
#define QPL_FLAG_INV_OUT 0x00040000u

/**
 * Check and skip ZLIB header/trailer
 */
#define QPL_FLAG_ZLIB_MODE 0x00080000u

/* Data Integrity & Aggregates flags */
/**
 * Filtering only: don't calculate CRC and XOR checksums
 */
#define QPL_FLAG_OMIT_CHECKSUMS 0x00100000u

/**
 * Filtering only: don't calculate aggregates values
 */
#define QPL_FLAG_OMIT_AGGREGATES 0x00200000u

/** @} */

/**
 * @enum qpl_path_t
 * @brief Enum of the executing paths
 */
typedef enum {
    qpl_path_auto = 0x00000000u,       /**< Enable auto-detection of the equipment for executing */
    qpl_path_hardware = 0x00000001u,   /**< Enable execution by hardware path */
    qpl_path_software = 0x00000002u    /**< Enable execution by software path (Run on CPU) */
} qpl_path_t;

/**
 * @brief Enum of all output formats
 */
typedef enum {
    qpl_ow_nom = 0u,    /**< Output stream in its nominal format without modification*/
    qpl_ow_8   = 1u,    /**< Output 8-bit stream  */
    qpl_ow_16  = 2u,    /**< Output 16-bit stream */
    qpl_ow_32  = 3u     /**< Output 32-bit stream */
} qpl_out_format;

/**
 * @brief Enum of all supported parser types
 */
typedef enum {
    qpl_p_le_packed_array = 0u,    /**< Input vector is written in the Little-Endian format */
    qpl_p_be_packed_array = 1u,    /**< Input vector is written in the Big-Endian format    */
    qpl_p_parquet_rle     = 2u     /**< input vector is written in the Parquet RLE format   */
} qpl_parser;

/**
 * @brief Enum of all supported operations
 */
typedef enum {
    qpl_op_decompress = 0x00u,    /**< Performs Inflate operation (@ref DEFLATE_OPERATIONS group) */
    qpl_op_compress   = 0x01u,    /**< Performs Deflate operation (@ref DEFLATE_OPERATIONS group) */

    qpl_op_crc64      = 0x05u,    /**< Performs @ref CRC_OPERATION */

    // start filter operations
    /**
     * Affiliation to boolean histogram filter operation (@ref ANALYTIC_OPERATIONS group)
     */
    qpl_op_extract        = 0x11u,  /**< Extract sub-vector filter operation (@ref ANALYTIC_OPERATIONS group) */
    qpl_op_select         = 0x12u,  /**< Down-sampling filter operation (@ref ANALYTIC_OPERATIONS group) */
    qpl_op_expand         = 0x15u,  /**< Up-sampling filter operation (@ref ANALYTIC_OPERATIONS group) */

    // start filter scan operations
    /**
     * Compare "equal" filter operation (@ref ANALYTIC_OPERATIONS group)
     */
    qpl_op_scan_eq = 0x20u,

    /**
     * Compare "not-equal" filter operation (@ref ANALYTIC_OPERATIONS group)
     */
    qpl_op_scan_ne = 0x21u,

    /**
     * Compare "less" filter operation (@ref ANALYTIC_OPERATIONS group)
     */
    qpl_op_scan_lt = 0x22u,

    /**
     * Compare "less-or-equal" filter operation (@ref ANALYTIC_OPERATIONS group)
     */
    qpl_op_scan_le = 0x23u,

    /**
     * Compare "greater" filter operation (@ref ANALYTIC_OPERATIONS group)
     */
    qpl_op_scan_gt = 0x24u,

    /**
     * Compare "greater-or-equal" filter operation (@ref ANALYTIC_OPERATIONS group)
     */
    qpl_op_scan_ge = 0x25u,

    /**
     * Compare "in-range" filter operation (@ref ANALYTIC_OPERATIONS group)
     */
    qpl_op_scan_range = 0x26u,

    /**
     * Compare "not-in-range" filter operation (@ref ANALYTIC_OPERATIONS group)
     */
    qpl_op_scan_not_range = 0x27u
} qpl_operation;

/**
 * @brief Enumerates mini-blocks sizes for the @ref qpl_op_compress and @ref qpl_op_decompress operations.
 */
typedef enum {
    qpl_mblk_size_none = 0u,    /**< Disable indexing for the deflate stream */
    qpl_mblk_size_512  = 1u,    /**< Put index into the deflate stream each 512   bytes */
    qpl_mblk_size_1k   = 2u,    /**< Put index into the deflate stream each 1024  bytes */
    qpl_mblk_size_2k   = 3u,    /**< Put index into the deflate stream each 2048  bytes */
    qpl_mblk_size_4k   = 4u,    /**< Put index into the deflate stream each 4096  bytes */
    qpl_mblk_size_8k   = 5u,    /**< Put index into the deflate stream each 8192  bytes */
    qpl_mblk_size_16k  = 6u,    /**< Put index into the deflate stream each 16384 bytes */
    qpl_mblk_size_32k  = 7u     /**< Put index into the deflate stream each 32768 bytes */
} qpl_mini_block_size;

/**
 * @brief Enumerates the compression level on software path, used for building @ref qpl_dictionary
 */
typedef enum {
    SW_NONE = DICT_NONE, /**< No compression expected to be performed on software */
    LEVEL_0 = 0,         /**< The lowest compression level */
    LEVEL_1 = 1,         /**< The 1st compression level */
    LEVEL_2 = 2,         /**< The 2nd compression level */
    LEVEL_3 = 3,         /**< The 3rd compression level */
    LEVEL_4 = 4,         /**< The 4th compression level */
    LEVEL_9 = 9          /**< The highest compression level */
} sw_compression_level;

/**
 * @brief Enumerates the compression level on hardware path, used for building @ref qpl_dictionary
 */
typedef enum {
    HW_NONE = DICT_NONE, /**< No compression expected to be performed on hardware */
    SMALL   = 0,         /**< The lowest compression level */
    LARGE   = 1          /**< The highest compression level */
} hw_compression_level;


/**
 * @brief Determines under what conditions the @ref qpl_op_decompress operation stops, and what expectations
 *        there are for how the stream should end.
 * @note  Specified stop condition is ignored if @ref QPL_FLAG_LAST flag is not set.
 * @note  Specified check condition is performed if @ref QPL_FLAG_LAST flag is set.
 *
 */
typedef enum {
    qpl_stop_and_check_for_bfinal_eob = 0u,    /**< Stop condition: b_final EOB; Check condition: b_final EOB */
    qpl_dont_stop_or_check,                    /**< Stop condition: none;       Check condition: none */
    qpl_stop_and_check_for_any_eob,            /**< Stop condition: EOB;        Check condition: EOB */
    qpl_stop_on_any_eob,                       /**< Stop condition: EOB;        Check condition: none */
    qpl_stop_on_bfinal_eob,                    /**< Stop condition: b_final EOB; Check condition: none */
    qpl_check_for_any_eob,                     /**< Stop condition: none;       Check condition: EOB */

    /**
     * Stop condition: none; Check condition: b_final EOB
     */
    qpl_check_for_bfinal_eob = OWN_RESERVED_INFLATE_MANIPULATOR - 1u,

    /**
     * Stop condition: disabled; Check condition: not last block
     */
    qpl_check_on_nonlast_block = OWN_LAST_INFLATE_MANIPULATOR,
} qpl_decomp_end_proc;

/**
 * @brief Enumerates different compressions levels
 */
typedef enum {
    qpl_level_1 = 1,                 /**< The fastest compression with low compression ratio*/
    qpl_level_2 = 2,                 /**< Not supported */
    qpl_level_3 = 3,                 /**< Medium compression speed, medium compression ratio*/
    qpl_level_4 = 4,                 /**< Not supported */
    qpl_level_5 = 5,                 /**< Not supported */
    qpl_level_6 = 6,                 /**< Not supported */
    qpl_level_7 = 7,                 /**< Not supported */
    qpl_level_8 = 8,                 /**< Not supported */
    qpl_level_9 = 9,                 /**< Not supported */
    qpl_default_level = qpl_level_1, /**< Default compression level defined by the highest compression level supported by Accelerator */
    qpl_high_level = qpl_level_3     /**< The level with highest compression level from supported by Intel QPL */
} qpl_compression_levels;

#ifdef __cplusplus
}
#endif

#endif /* QPL_DEFS_H__ */

/** @} */
