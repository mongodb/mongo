/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

/*
 *  Intel® Query Processing Library (Intel® QPL)
 *  Job API (public C API)
 */

#ifndef QPL_STATUS_H_
#define QPL_STATUS_H_

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @addtogroup JOB_API_DEFINITIONS
 * @{
 */

/**
 * @addtogroup QPL_STATUS_BASE
 * @{
 */

/* --- Status BASE --- */
#define QPL_PROCESSING_ERROR_BASE    0u
        /**< Processing step errors */
#define QPL_PARAMETER_ERROR_BASE     50u
        /**< Parameter check step errors */
#define QPL_SERVICE_LOGIC_ERROR_BASE 100u
        /**< Operation preprocessing or postprocessing errors */
#define QPL_OPERATION_ERROR_BASE     200u
        /**< Execution step errors */
#define QPL_OPERATION_STATUS_BASE    300u 
        /**< Execution step statuses */
#define QPL_INIT_ERROR_BASE          500u
        /**< Initialization step errors */

/** @} */

/**
 * @addtogroup QPL_STATUS_CALCULATOR
 * @{
 */

/* --- Status Calculators */
#define QPL_PROCESSING_ERROR(x)     QPL_PROCESSING_ERROR_BASE + x
        /**< Calculates status for processing step */
#define QPL_PARAMETER_ERROR(x)      QPL_PARAMETER_ERROR_BASE + x
        /**< Calculates status for parameter check step */
#define QPL_SERVICE_LOGIC_ERROR(x)  QPL_SERVICE_LOGIC_ERROR_BASE + x
        /**< Calculates status for operation preprocessing or postprocessing step */
#define QPL_OPERATION_ERROR(x)      QPL_OPERATION_ERROR_BASE + x
        /**< Calculates status for operation execution step */
#define QPL_OPERATION_STATUS(x)     QPL_OPERATION_STATUS_BASE + x     
        /**< Calculates status for operation execution step */
#define QPL_INIT_ERROR(x)           QPL_INIT_ERROR_BASE + x
        /**< Calculates status for initialization step */

/** @} */

/**
 * @enum qpl_status
 * @brief Intel QPL return status list (Errors marked `Internal` indicate an issue within the library) 
 */
typedef enum {
/* ====== Processing Statuses ====== */
    QPL_STS_OK                      = QPL_PROCESSING_ERROR(0),  /**< Operation completed successfully */
    QPL_STS_BEING_PROCESSED         = QPL_PROCESSING_ERROR(1u), /**< The job is still being processed */
    QPL_STS_MORE_OUTPUT_NEEDED      = QPL_PROCESSING_ERROR(2u), /**< Compression/Decompression operation filled output buffer before finishing input */
    QPL_STS_MORE_INPUT_NEEDED       = QPL_PROCESSING_ERROR(3u), /**< Compress/Decompress operation need more input */
    QPL_STS_JOB_NOT_CONTINUABLE_ERR = QPL_PROCESSING_ERROR(4u), /**< A job after a LAST job was not marked as FIRST */
    QPL_STS_QUEUES_ARE_BUSY_ERR     = QPL_PROCESSING_ERROR(5u), /**< Descriptor can't be submitted into filled work queue */
    QPL_STS_LIBRARY_INTERNAL_ERR    = QPL_PROCESSING_ERROR(6u), /**< Unexpected internal error condition */
    QPL_STS_JOB_NOT_SUBMITTED       = QPL_PROCESSING_ERROR(7u), /**< The job being checked/waited has not been submitted */

/* ====== Operations Statuses ====== */
/* --- Incorrect Parameter Value --- */
// <--- Common
    QPL_STS_NULL_PTR_ERR           = QPL_PARAMETER_ERROR(0u), /**< Null pointer error */
    QPL_STS_OPERATION_ERR          = QPL_PARAMETER_ERROR(1u), /**< Non-supported value in the qpl_job operation field */
    QPL_STS_NOT_SUPPORTED_MODE_ERR = QPL_PARAMETER_ERROR(2u), /**< Indicates an error if the requested mode is not supported */
    QPL_STS_BAD_JOB_STRUCT_ERR     = QPL_PARAMETER_ERROR(3u), /**< Indicates that the qpl_job structure does not match the operation */
    QPL_STS_PATH_ERR               = QPL_PARAMETER_ERROR(4u), /**< Incorrect value for the qpl_path input parameter */
    QPL_STS_INVALID_PARAM_ERR      = QPL_PARAMETER_ERROR(5u), /**< Invalid combination of fields in the qpl_job structure */
    QPL_STS_FLAG_CONFLICT_ERR      = QPL_PARAMETER_ERROR(6u), /**< qpl_job flags field contains conflicted values */
    QPL_STS_SIZE_ERR               = QPL_PARAMETER_ERROR(7u), /**< Incorrect size error */
    QPL_STS_BUFFER_TOO_LARGE_ERR   = QPL_PARAMETER_ERROR(8u), /**< Buffer exceeds max size supported by library */
    QPL_STS_BUFFER_OVERLAP_ERR     = QPL_PARAMETER_ERROR(9u), /**< Buffers overlap */

// <-- Simple Operations
    QPL_STS_CRC64_BAD_POLYNOM      = QPL_PARAMETER_ERROR(10u), /**< Incorrect polynomial value for CRC64 */

// <-- Filtering
    QPL_STS_SET_TOO_LARGE_ERR           = QPL_PARAMETER_ERROR(20u), /**< Set is too large for operation */
    QPL_STS_PARSER_ERR                  = QPL_PARAMETER_ERROR(21u), /**< Non-supported value in the qpl_job parser field */
    QPL_STS_OUT_FORMAT_ERR              = QPL_PARAMETER_ERROR(22u), /**< qpl_job out_bit_width field contains invalid value */
    QPL_STS_DROP_BITS_OVERFLOW_ERR      = QPL_PARAMETER_ERROR(23u), /**< Incorrect dropBits value (param_low + param_high must be beyond 0..32) */
    QPL_STS_BIT_WIDTH_OUT_EXTENDED_ERR  = QPL_PARAMETER_ERROR(24u), /**< qpl_job bit-width field contains an invalid value for current output format */
    QPL_STS_DROP_BYTES_ERR              = QPL_PARAMETER_ERROR(25u), /**< qpl_job drop_initial_bytes field contains an invalid value */

// <-- Compression/Decompression
    QPL_STS_MISSING_HUFFMAN_TABLE_ERR     = QPL_PARAMETER_ERROR(30u), /**< Flags specify NO_HDRS and DYNAMIC_HUFFMAN, but no Huffman table provided */
    QPL_STS_INVALID_HUFFMAN_TABLE_ERR     = QPL_PARAMETER_ERROR(31u), /**< Invalid Huffman table data */
    QPL_STS_MISSING_INDEX_TABLE_ERR       = QPL_PARAMETER_ERROR(32u), /**< Indexing enabled but Indexing table is not set */
    QPL_STS_INVALID_COMPRESS_STYLE_ERR    = QPL_PARAMETER_ERROR(33u), /**< The style of a compression job does not match the style of the previous related job */
    QPL_STS_INFLATE_NEED_DICT_ERR         = QPL_PARAMETER_ERROR(34u), /**< Inflate needs dictionary to perform decompression */
    QPL_STS_INVALID_DECOMP_END_PROC_ERR   = QPL_PARAMETER_ERROR(35u), /**< The qpl_job field for decompression manipulation is incorrect */
    QPL_STS_INVALID_BLOCK_SIZE_ERR        = QPL_PARAMETER_ERROR(36u), /**< Invalid block size used during indexing */
    QPL_STS_UNSUPPORTED_COMPRESSION_LEVEL = QPL_PARAMETER_ERROR(37u), /**< Compression level is not supported */
    QPL_STS_HUFFMAN_TABLE_TYPE_ERROR      = QPL_PARAMETER_ERROR(38u), /**< Incorrect table type used */

// <-- Serialization
    QPL_STS_SERIALIZATION_FORMAT_ERROR    = QPL_PARAMETER_ERROR(40u), /**< Unsupported Serialization format */
    QPL_STS_SERIALIZATION_CORRUPTED_DUMP  = QPL_PARAMETER_ERROR(41u), /**< Deserialization can't re-create the table from the input stream */

/* --- Processing Errors --- */
    QPL_STS_INVALID_DEFLATE_DATA_ERR  = QPL_SERVICE_LOGIC_ERROR(0u), /**< Currently unused */
    QPL_STS_NO_MEM_ERR                = QPL_SERVICE_LOGIC_ERROR(1u), /**< Not enough memory for the operation */
    QPL_STS_INDEX_ARRAY_TOO_SMALL     = QPL_SERVICE_LOGIC_ERROR(2u), /**< Indexing buffer is too small */
    QPL_STS_INDEX_GENERATION_ERR      = QPL_SERVICE_LOGIC_ERROR(3u), /**< Mini-block creation error */
    QPL_STS_ARCHIVE_HEADER_ERR        = QPL_SERVICE_LOGIC_ERROR(4u), /**< Invalid GZIP/Zlib header */
    QPL_STS_ARCHIVE_UNSUP_METHOD_ERR  = QPL_SERVICE_LOGIC_ERROR(5u), /**< Gzip/Zlib header specifies unsupported compress method */
    QPL_STS_OBJECT_ALLOCATION_ERR     = QPL_SERVICE_LOGIC_ERROR(6u), /**< Not able to allocate Huffman table object */

/* --- Corresponds to the error codes in Completion Record from Intel® In-Memory Analytics Accelerator --- */
    QPL_STS_BIG_HEADER_ERR                  = QPL_OPERATION_ERROR(1u),  /**< Reached the end of the input stream before decoding header and header is too big to fit in input buffer */
    QPL_STS_UNDEF_CL_CODE_ERR               = QPL_OPERATION_ERROR(2u),  /**< Bad CL code */
    QPL_STS_FIRST_LL_CODE_16_ERR            = QPL_OPERATION_ERROR(3u),  /**< First code in LL tree is 16 */
    QPL_STS_FIRST_D_CODE_16_ERR             = QPL_OPERATION_ERROR(4u),  /**< First code in D tree is 16 */
    QPL_STS_NO_LL_CODE_ERR                  = QPL_OPERATION_ERROR(5u),  /**< All LL codes are specified with 0 length */
    QPL_STS_WRONG_NUM_LL_CODES_ERR          = QPL_OPERATION_ERROR(6u),  /**< After parsing LL code lengths, total codes != expected value */
    QPL_STS_WRONG_NUM_DIST_CODES_ERR        = QPL_OPERATION_ERROR(7u),  /**< After parsing D code lengths, total codes != expected value */
    QPL_STS_BAD_CL_CODE_LEN_ERR             = QPL_OPERATION_ERROR(8u),  /**< First CL code of length N is greater than 2^N-1 */
    QPL_STS_BAD_LL_CODE_LEN_ERR             = QPL_OPERATION_ERROR(9u),  /**< First LL code of length N is greater than 2^N-1 */
    QPL_STS_BAD_DIST_CODE_LEN_ERR           = QPL_OPERATION_ERROR(10u), /**< First D code of length N is greater than 2^N-1 */
    QPL_STS_BAD_LL_CODE_ERR                 = QPL_OPERATION_ERROR(11u), /**< Incorrect LL code */
    QPL_STS_BAD_D_CODE_ERR                  = QPL_OPERATION_ERROR(12u), /**< Incorrect D code */
    QPL_STS_INVALID_BLOCK_TYPE              = QPL_OPERATION_ERROR(13u), /**< Invalid type of deflate block */
    QPL_STS_INVALID_STORED_LEN_ERR          = QPL_OPERATION_ERROR(14u), /**< Length of stored block doesn't match inverse length */
    QPL_STS_BAD_EOF_ERR                     = QPL_OPERATION_ERROR(15u), /**< EOB flag was set but last token was not EOB */
    QPL_STS_BAD_LEN_ERR                     = QPL_OPERATION_ERROR(16u), /**< Decoded Length code is 0 or greater 258 */
    QPL_STS_BAD_DIST_ERR                    = QPL_OPERATION_ERROR(17u), /**< Decoded Distance is 0 or greater than History Buffer */
    QPL_STS_REF_BEFORE_START_ERR            = QPL_OPERATION_ERROR(18u), /**< Distance of reference is before start of file */
    QPL_STS_TIMEOUT_ERR                     = QPL_OPERATION_ERROR(19u), /**< Library has input data, but is not making forward progress */
    QPL_STS_PRLE_FORMAT_ERR                 = QPL_OPERATION_ERROR(20u), /**< PRLE format is incorrect or is truncated */
    QPL_STS_OUTPUT_OVERFLOW_ERR             = QPL_OPERATION_ERROR(21u), /**< Output index value is greater than max available for current output data type */
    QPL_STS_INTL_AECS_ERR                   = QPL_OPERATION_ERROR(22u), /**< Internal Error Code */
    QPL_STS_SRC1_TOO_SMALL_ERR              = QPL_OPERATION_ERROR(23u), /**< Source 1 contained fewer than expected elements/bytes */
    QPL_STS_SRC2_IS_SHORT_ERR               = QPL_OPERATION_ERROR(24u), /**< Source 2 contained fewer than expected elements/bytes */
    QPL_STS_DST_IS_SHORT_ERR                = QPL_OPERATION_ERROR(25u), /**< qpl_job destination buffer has less bytes than required to process num_input_elements/bytes */
    QPL_STS_INTL_DIST_SPANS_MINI_BLOCKS     = QPL_OPERATION_ERROR(26u), /**< Internal Error Code */
    QPL_STS_INTL_LEN_SPANS_MINI_BLOCKS      = QPL_OPERATION_ERROR(27u), /**< Internal Error Code */
    QPL_STS_INTL_VERIF_INVALID_BLOCK_SIZE   = QPL_OPERATION_ERROR(28u), /**< Internal Error Code */
    QPL_STS_INTL_VERIFY_ERR                 = QPL_OPERATION_ERROR(29u), /**< Internal Error Code */
    QPL_STS_INVALID_HUFFCODE_ERR            = QPL_OPERATION_ERROR(30u), /**< Compressor tried to use an invalid huffman code */
    QPL_STS_BIT_WIDTH_ERR                   = QPL_OPERATION_ERROR(31u), /**< Bit width is out of range [1..32] */
    QPL_STS_SRC_IS_SHORT_ERR                = QPL_OPERATION_ERROR(32u), /**< The input stream ended before specified Number of input Element was seen  */
    QPL_STS_INTL_INVALID_COUNT              = QPL_OPERATION_ERROR(33u), /**< Internal Error Code */
    QPL_STS_TOO_MANY_LL_CODES_ERR           = QPL_OPERATION_ERROR(35u), /**< The number of LL codes specified in the DEFLATE header exceed 286 */
    QPL_STS_TOO_MANY_D_CODES_ERR            = QPL_OPERATION_ERROR(36u), /**< The number of D codes specified in the DEFLATE header exceed 30 */

/* --- Corresponds to the status codes in the Completion Record from Intel® In-Memory Analytics Accelerator --- */
    QPL_STS_INTL_INPROG                 = QPL_OPERATION_STATUS(0u),  /**< Internal Status Code */
    QPL_STS_INTL_SUCCESS                = QPL_OPERATION_STATUS(1u),  /**< Internal Status Code */
    QPL_STS_INTL_ANALYTIC_ERROR         = QPL_OPERATION_STATUS(10u), /**< Internal Status Code */
    QPL_STS_INTL_OUTPUT_OVERFLOW        = QPL_OPERATION_STATUS(11u), /**< Internal Status Code */
    QPL_STS_INTL_UNSUPPORTED_OPCODE     = QPL_OPERATION_STATUS(16u), /**< Internal Status Code */
    QPL_STS_INTL_INVALID_OP_FLAG        = QPL_OPERATION_STATUS(17u), /**< Internal Status Code */
    QPL_STS_INTL_NONZERO_RESERVED_FIELD = QPL_OPERATION_STATUS(18u), /**< Internal Status Code */
    QPL_STS_TRANSFER_SIZE_INVALID       = QPL_OPERATION_STATUS(19u), /**< Invalid value for transfer size or maximum destination size */
    QPL_STS_INTL_OVERLAPPING_BUFFERS    = QPL_OPERATION_STATUS(22u), /**< Internal Status Code */
    QPL_STS_INTL_COMPL_RECORD_UNALIGN   = QPL_OPERATION_STATUS(27u), /**< Internal Status Code */
    QPL_STS_INTL_MISALIGNED_ADDRESS     = QPL_OPERATION_STATUS(28u), /**< Internal Status Code */
    QPL_STS_INTL_INVALID_DECOMP_FLAG    = QPL_OPERATION_STATUS(48u), /**< Internal Status Code */
    QPL_STS_INTL_INVALID_FILTER_FLAG    = QPL_OPERATION_STATUS(49u), /**< Internal Status Code */
    QPL_STS_INTL_INVALID_INPUT_SIZE     = QPL_OPERATION_STATUS(50u), /**< Internal Status Code */
    QPL_STS_INVALID_NUM_ELEM            = QPL_OPERATION_STATUS(51u), /**< Number Elements for Filter operation is 0 */
    QPL_STS_INVALID_SRC1_WIDTH          = QPL_OPERATION_STATUS(52u), /**< Invalid source-1 bit-width */
    QPL_STS_INV_OUTPUT                  = QPL_OPERATION_STATUS(53u), /**< Invert Output flag was used when the output was not a bit-vector */

/* ====== Initialization Statuses ====== */
    QPL_STS_INIT_HW_NOT_SUPPORTED             = QPL_INIT_ERROR(0u), /**< Hardware path is not supported */
    QPL_STS_INIT_LIBACCEL_NOT_FOUND           = QPL_INIT_ERROR(1u), /**< libaccel is not found or not compatible */
    QPL_STS_INIT_LIBACCEL_ERROR               = QPL_INIT_ERROR(2u), /**< libaccel internal error */
    QPL_STS_INIT_WORK_QUEUES_NOT_AVAILABLE    = QPL_INIT_ERROR(3u), /**< Supported and enabled work queues are not found (May be due to lack of privileges e.g. lack of sudo on linux)*/
} qpl_status;

/** @} */

#ifdef __cplusplus
}
#endif

#endif //QPL_STATUS_H_
