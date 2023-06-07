/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

/**
 * @addtogroup HW_PUBLIC_API
 * @{
 */

#ifndef HW_PATH_HW_STATUS_H_
#define HW_PATH_HW_STATUS_H_

#include <stddef.h>
#include <stdint.h>

/**
 * @todo
 */
typedef enum {
    HW_ACCELERATOR_STATUS_OK                    = 0u, /**< Accelerator returned success */
    HW_ACCELERATOR_SUPPORT_ERR                  = 1u, /** System doesn't support accelerator */
    HW_ACCELERATOR_LIBACCEL_NOT_FOUND           = 2u, /**< libaccel is not found */
    HW_ACCELERATOR_LIBACCEL_ERROR               = 3u, /**< Accelerator instance can not be found */
    HW_ACCELERATOR_WORK_QUEUES_NOT_AVAILABLE    = 4u, /**< Enabled work queues are not found or no enabled devices */
    /* Not exposed to external API */
    HW_ACCELERATOR_NULL_PTR_ERR                 = 5u, /**< Null pointer error */
    HW_ACCELERATOR_WQ_IS_BUSY                   = 6u, /**< Work queue is busy with task processing */
} hw_accelerator_status;

/**
 * @name Hardware Operation statuses
 * @anchor HW_STATUS_CODES
 * @{
 * @brief List of all statuses that can be written into @ref hw_completion_record.status field.
 * @todo
 */
#define FAULT_TYPE_IS_WRITE   0x80u /**< Mask for fault type read */
#define STATUS_MASK           0x3Fu /**< Mask for actual completion status read */

typedef uint8_t hw_operation_status; /**< Accelerator status type */

#define AD_STATUS_INPROG                   0x00    /**< Operation is in progress */
#define AD_STATUS_SUCCESS                  0x01    /**< Success */
#define AD_STATUS_ANALYTICS_ERROR          0x0A    /**< Operation execution error. See at @ref HW_ERROR_CODES */
#define AD_STATUS_OUTPUT_OVERFLOW          0x0B    /**< Output buffer overflow. */
#define AD_STATUS_UNSUPPORTED_OPCODE       0x10    /**< Unsupported operation code */
#define AD_STATUS_INVALID_OP_FLAG          0x11    /**< Invalid `Operation Flag` used */
#define AD_STATUS_NONZERO_RESERVED_FIELD   0x12    /**< Non-zero reserved field */
#define AD_STATUS_TRANSFER_SIZE_INVALID    0x13    /**< Invalid size value (`source-1`, `source-2`, `destination`) */
#define AD_STATUS_OVERLAPPING_BUFFERS      0x16    /**< Buffers overlap detected */
#define AD_STATUS_COMPL_RECORD_UNALIGN     0x1B    /**< `Completion Record` address is not 64-byte aligned */
#define AD_STATUS_MISALIGNED_ADDRESS       0x1C    /**< The `AECS` address or size was not a multiple of 32 bytes */
#define AD_STATUS_INVALID_DECOMP_FLAG      0x30    /**< Invalid `Decompression/Compression/CRC Flags` */
#define AD_STATUS_INVALID_FILTER_FLAG      0x31    /**< Invalid `Filter Flags` */
#define AD_STATUS_INVALID_NUM_ELEM         0x33    /**< `Number Elements` for `Filter` operation is 0 */
#define AD_STATUS_INVALID_SRC1_WIDTH       0x34    /**< Invalid `source-1` bit-width */
#define AD_STATUS_INVALID_INV_OUTPUT       0x35    /**< `Invert Output Flag` was used when the output was not a bit-vector */
/** @} */


/**
 * @name Accelerator error codes
 * @anchor HW_ERROR_CODES
 * @{
 * @brief Extended errors status codes list that used in case if @ref hw_operation_status of completion record is @ref AD_STATUS_ANALYTICS_ERROR
 * @todo
 */
typedef uint8_t hw_operation_error; /**< Error code type */

#define AD_ERROR_CODE_OK                            0u      /**< No errors */
#define AD_ERROR_CODE_BIGHDR                        1u      /**< Reached the end of the input stream before decoding header and header is too big to fit in input buffer */
#define AD_ERROR_CODE_UNDEF_CL_CODE                 2u      /**< Bad CL code */
#define AD_ERROR_CODE_FIRST_LL_CODE_16              3u      /**< First code in LL tree is 16 */
#define AD_ERROR_CODE_FIRST_D_CODE_16               4u      /**< First code in D tree is 16 */
#define AD_ERROR_CODE_NO_LL_CODE                    5u      /**< All LL codes are specified with 0 length */
#define AD_ERROR_CODE_WRONG_NUM_LL_CODES            6u      /**< After parsing LL code lengths, total codes != expected value */
#define AD_ERROR_CODE_WRONG_NUM_DIST_CODES          7u      /**< After parsing D code lengths, total codes != expected value */
#define AD_ERROR_CODE_BAD_CL_CODE_LEN               8u      /**< First CL code of length N is greater than 2^N-1 */
#define AD_ERROR_CODE_BAD_LL_CODE_LEN               9u      /**< First LL code of length N is greater than 2^N-1 */
#define AD_ERROR_CODE_BAD_DIST_CODE_LEN             10u     /**< First D code of length N is greater than 2^N-1 */
#define AD_ERROR_CODE_BAD_LL_CODE                   11u     /**< Incorrect LL code */
#define AD_ERROR_CODE_BAD_D_CODE                    12u     /**< Incorrect D code */
#define AD_ERROR_CODE_INVALID_BLOCK_TYPE            13u     /**< Block type 0x3 detected */
#define AD_ERROR_CODE_INVALID_STORED_LEN            14u     /**< Length of stored block doesn't match inverse length */
#define AD_ERROR_CODE_BAD_EOF                       15u     /**< EOB flag was set but last token was not EOB*/
#define AD_ERROR_CODE_BAD_LEN                       16u     /**< Decoded Length code is 0 or greater 258 */
#define AD_ERROR_CODE_BAD_DIST                      17u     /**< Decoded Distance is 0 or greater than History Buffer */
#define AD_ERROR_CODE_REF_BEFORE_START              18u     /**< Distance of reference is before start of file */
#define AD_ERROR_CODE_TIMEOUT                       19u     /**< Engine has input data, but is not making forward progress */
#define AD_ERROR_CODE_PRLE_FORMAT_INCORRECT         20u     /**< `PRLE` record contains an error or is truncated. */
#define AD_ERROR_CODE_OUTPUT_WIDTH_TOO_SMALL        21u     /**< Bit-width overflow on output */
#define AD_ERROR_CODE_AECS_ERROR                    22u     /**< AECS contains an invalid value */
#define AD_ERROR_CODE_SRC1_TOO_SMALL                23u     /**< Source 1 contained fewer than expected elements */
#define AD_ERROR_CODE_SRC2_TOO_SMALL                24u     /**< Source 2 contained fewer than expected elements */
#define AD_ERROR_CODE_UNRECOVERABLE_OUTPUT_OVERFLOW 25u     /**< Output buffer too small */
#define AD_ERROR_CODE_DIST_SPANS_MBLOCKS            26u     /**< Indexing only: A match referenced data in a different mini-block */
#define AD_ERROR_CODE_LEN_SPANS_MBLOCKS             27u     /**< Indexing only: A match had a length expending into the next mini-block */
#define AD_ERROR_CODE_INVALID_BLOCK_SIZE            28u     /**< Mini-block size is incorrect */
#define AD_ERROR_CODE_ZVERIFY_FAILURE               29u     /**< Verify logic for decompress detected incorrect output */
#define AD_ERROR_CODE_INVALID_HUFFCODE              30u     /**< Compressor tried to use an invalid huffman code */
#define AD_ERROR_CODE_PRLE_BITWIDTH_TOO_LARGE       31u     /**< `PLRE` elements `bit-width` was greater than 32 */
#define AD_ERROR_CODE_TOO_FEW_ELEMENTS_PROCESSED    32u     /**< The input stream ended before specified Number of input Element was seen */
#define AD_ERROR_CODE_INVALID_ZDECOMP_HDR           34u     /**< During a `Z-Decompress` operation, the input data ended in the middle of a record header */
#define AD_ERROR_CODE_TOO_MANY_LL_CODES             35u     /**< The number of LL codes specified in the DEFLATE header exceed 286 */
#define AD_ERROR_CODE_TOO_MANY_D_CODES              36u     /**< The number of D codes specified in the DEFLATE header exceed 30 */
/** @} */

#endif //HW_PATH_HW_STATUS_H_

/** @} */

