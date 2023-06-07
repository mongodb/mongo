/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

/*
 *  Intel® Query Processing Library (Intel® QPL)
 *  Hardware Interconnect API (private C API)
 */

/**
 * @brief Contains API and Definitions to work with Intel® In-Memory Analytics Accelerator (Intel® IAA)
 * AECS structures (Analytic Engine Configuration and State)
 *
 * @defgroup HW_AECS_API AECS API
 * @ingroup HW_PUBLIC_API
 * @{
 */

#include "stdbool.h"
#include "hw_definitions.h"
#include "qplc_huffman_table.h"

#ifndef HW_PATH_HW_AECS_API_H_
#define HW_PATH_HW_AECS_API_H_

#if !defined( HW_PATH_IAA_AECS_API )
#define HW_PATH_IAA_AECS_API(type, name, arg) type HW_STDCALL hw_iaa_aecs_##name arg
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define HW_AECS_COMPRESSION_SIZE             0x620u   /**< AECS Compression size */
#define HW_AECS_ANALYTICS_SIZE               0x1500u  /**< AECS Analytic size */
#define HW_AECS_ANALYTIC_RANDOM_ACCESS_SIZE  0x440u   /**< AECS Analytic size for Random Assess */
#define HW_AECS_ANALYTIC_FILTER_ONLY_SIZE    0x20u    /**< AECS Analytic size for Filter only operations */

typedef void                              hw_iaa_aecs;                 /**< Common AECS type */
typedef void                              hw_iaa_huffman_codes;        /**< Forward declaration */
typedef qplc_huffman_table_default_format hw_iaa_c_huffman_only_table; /**< Redefinition */
typedef qplc_huffman_table_flat_format    hw_iaa_d_huffman_only_table; /**< Redefinition */

/**
 * @brief Describe huffman code according to accelerator format
 */
typedef struct {
    uint16_t code;               /**< Huffman code */
    uint8_t  extra_bit_count;    /**< Number of extra bits */
    uint8_t  length;             /**< Huffman code length */
} hw_huffman_code;

/**
 * @brief Enumerates all possible access modes for AECS.
 */
typedef enum {
    /**
     * @brief Toggle AECS R/W policy. Default one read from the first AECS and write to the second AECS.
     * If no write then this policy mustn't be used.
     */
    hw_aecs_toggle_rw          = 0x001u,
    hw_aecs_access_read        = 0x010u,  /**< Enable reading of the AECS by operation */
    hw_aecs_access_write       = 0x100u,  /**< Enable writing to the AECS by operation */
    hw_aecs_access_maybe_write = 0x200u,  /**< Enable writing to the AECS only in case of output buffer overflow */
} hw_iaa_aecs_access_policy;

/**
* @brief @todo add description
*/
typedef enum {
    hw_aecs_at_ll_token_non_final_block     = 0x0u,  /**< @todo add description */
    hw_aecs_at_ll_token_final_block         = 0x4u,  /**< @todo add description */
    hw_aecs_at_stored_block_non_final_block = 0x2u,  /**< @todo add description */
    hw_aecs_at_stored_block_final_block     = 0x6u,  /**< @todo add description */
    hw_aecs_at_start_block_header           = 0x1u,  /**< @todo add description */
    hw_aecs_processing_terminated_with_eob  = 0x8u   /**< @todo add description */
} hw_iaa_aecs_decompress_state;


/**
 * @brief Describes @ref hw_iaa_aecs_analytic substructure that contains additional input/output data of filter operations
 */
typedef struct {
    uint32_t crc;                                  /**< Initial (on input) or final (on output) CRC Checksum value */
    uint32_t xor_checksum;                         /**< Initial (on input) or final (on output) Xor Checksum value */
    uint32_t filter_low;                           /**< Low parameter value (low limit) of filter operations */
    uint32_t filter_high;                          /**< High parameter value (high limit) of filter operations */
    uint32_t output_mod_idx;                       /**< Number bytes that should be dropped before Analytic start */
    uint32_t drop_initial_decompress_out_bytes;    /**< Number bytes that should be dropped before Filtering start */
} hw_iaa_aecs_filter;


/**
 * @brief Describes @ref hw_iaa_aecs_analytic substructure that contains additional input/output data of decompress operation
 */
typedef struct {
    // Decompression buffers
    uint32_t output_accumulator[3];                         /**< Output accumulator */
    uint32_t idx_bit_offset;                                /**< Initial indexing index */
    uint64_t input_accum[32];                               /**< Input accumulators */
    uint8_t  input_accum_size[32];                          /**< Input accumulators valid bits */
    // Decompression State
    uint32_t reserved0[2];                                  /**< Reserved bytes */
    uint32_t lit_len_first_tbl_idx[5];                      /**< @todo */
    uint32_t lit_len_num_codes[5];                          /**< @todo */
    uint32_t lit_len_first_code[5];                         /**< @todo */
    uint32_t lit_len_first_len_code[5];                     /**< @todo */
    uint32_t reserved1[62];                                 /**< Reserved bytes */
    uint8_t  lit_len_sym[268];                              /**< LL huffman codes mapping table */
    uint16_t decompress_state;                              /**< Indicates the state of decompress parser */
    uint16_t reserved2;                                     /**< Reserved bytes */
    uint32_t reserved3[43];                                 /**< Reserved bytes */
    struct {
        uint16_t history_buffer_write_offset  : 15;         /**< Offset to the first unwritten byte in history_buffer */
        uint16_t is_history_buffer_overflowed : 1;          /**< True or false value, which indicates whenever history buffer capacity was exceed */
    } history_buffer_params;                                /**< History size description */
    uint16_t reserved4;                                     /**< Reserved bytes */
    uint8_t  history_buffer[4096];                          /**< History buffer */
    uint32_t reserved5[6];                                  /**< Reserved bytes */
} hw_iaa_aecs_decompress;


/**
 * @brief Describes an AECS state shared with Decompress and Filter operations. Decompress and Filter operations
 * constitute a specific operations group called Analytics.
 */
typedef struct {
    hw_iaa_aecs_filter     filtering_options; /**< Contains filter specific data */
    uint32_t               reserved[36];      /**< Reserved bytes */
    hw_iaa_aecs_decompress inflate_options;   /**< Contains decompressor specific data */
} hw_iaa_aecs_analytic;

/**
 * @brief Contains the number of uses for each `length`, `match` and `offset` symbol/code
 */
typedef struct {
    uint32_t ll_sym[286];              /**< LL huffman table */
    uint32_t reserved1[2];             /**< Reserved bytes */
    uint32_t d_sym[30];                /**< D huffman table */
    uint32_t reserved2[2];             /**< Reserved bytes */
} hw_iaa_histogram;

/**
 * @brief Describes an AECS state for Compress operation.
 */
typedef struct {
    uint32_t         crc;                      /**< Initial (on input) or final (on output) CRC Checksum value */
    uint32_t         xor_checksum;             /**< Initial (on input) or final (on output) XOR Checksum value */
    uint32_t         reserved0[5];             /**< Reserved bytes */
    uint32_t         num_output_accum_bits;    /**< Number of bits that are valid in Output Accumulator */
    uint8_t          output_accum[256];        /**< Output Accumulator */
    hw_iaa_histogram histogram;                /**< Huffman codes used for compression */
} hw_iaa_aecs_compress;

/* ====== AECS Compress ====== */

/**
 * @name AECS Deflate Compress Mode API
 *
 * @brief Contains AECS service functions to compress stream in the `Deflate` format.
 *
 * @{
 */

/**
 * @brief Specifies deflate fixed header that will be used to declare next compressed block.
 *
 * @param [in,out] aecs_ptr     pointer to @ref hw_iaa_aecs_compress
 * @param [in] b_final           final block marker
 *
 * @note`b_final` must contain `1` or `0` value
 *
 * @return Error in case of aecs corruption.
 */
HW_PATH_IAA_AECS_API(uint32_t, compress_write_deflate_fixed_header, (hw_iaa_aecs_compress *const aecs_ptr,
                                                                     const uint32_t b_final));


/**
 * @brief Specifies deflate dynamic header that will be used to declare next compressed block.
 *
 * @param [in,out] aecs_ptr      pointer to @ref hw_iaa_aecs_compress
 * @param [in] header_ptr        pointer to already prepared deflate header
 * @param [in] header_bit_size   size of header in bits
 * @param [in] b_final           final block marker
 *
 * @note `b_final` must contain `1` or `0` value
 *
 * @return Error in case of aecs corruption.
 *
 */
HW_PATH_IAA_AECS_API(uint32_t , compress_write_deflate_dynamic_header, (hw_iaa_aecs_compress *const aecs_ptr,
                                                                        const uint8_t *const header_ptr,
                                                                        const uint32_t header_bit_size,
                                                                        const uint32_t b_final));


/**
 * @brief Setup @ref hw_iaa_aecs_compress to compress data with previously calculated huffman codes.
 *
 * @param [in,out] aecs_ptr              pointer to @ref hw_iaa_aecs_compress
 * @param [in] literal_length_codes_ptr  pointer to literals and matches huffman codes table
 * @param [in] distance_codes_ptr        pointer to offset huffman codes table
 *
 */
HW_PATH_IAA_AECS_API(void, compress_set_deflate_huffman_table, (hw_iaa_aecs_compress *const aecs_ptr,
                                                                const hw_iaa_huffman_codes *const literal_length_codes_ptr,
                                                                const hw_iaa_huffman_codes *const distance_codes_ptr));


/**
 * @brief Specifies deflate dynamic header that will be used to declare next compressed block.
 *
 * @param [in,out] aecs_ptr   pointer to @ref hw_iaa_aecs_compress
 * @param [in] histogram_ptr  histogram collected with using @ref hw_iaa_descriptor_init_statistic_collector
 * @param [in] b_final        final block marker
 *
 */
HW_PATH_IAA_AECS_API(void, compress_write_deflate_dynamic_header_from_histogram, (hw_iaa_aecs_compress *const aecs_ptr,
                                                                                  hw_iaa_histogram *const histogram_ptr,
                                                                                  const uint32_t b_final));
/** @} */


/**
 * @name AECS Huffman Only Compress Mode API
 *
 * @brief Contains AECS service functions to compress stream in the `Huffman Only` format
 *
 * @{
 */

/**
 * @brief Setup @ref hw_iaa_aecs_compress to compress data with previously calculated huffman codes.
 *
 * @param [in,out] aecs_ptr              pointer to @ref hw_iaa_aecs_compress
 * @param [in] literal_length_codes_ptr  pointer to literals and matches huffman codes table
 *
 */
HW_PATH_IAA_AECS_API(void, compress_set_huffman_only_huffman_table, (hw_iaa_aecs_compress *const aecs_ptr,
                                                                     hw_iaa_huffman_codes *const literal_length_codes_ptr));

/**
 * @brief Setup @ref hw_iaa_aecs_compress to compress data with huffman codes calculated with provided histogram.
 *
 * @param [in,out] aecs_ptr   pointer to @ref hw_iaa_aecs_compress
 * @param [in] histogram_ptr  histogram collected using @ref hw_iaa_descriptor_init_statistic_collector
 *
 */
HW_PATH_IAA_AECS_API(void, compress_set_huffman_only_huffman_table_from_histogram, (hw_iaa_aecs_compress *const aecs_ptr,
                                                                                    hw_iaa_histogram *const histogram_ptr));


/**
 * @brief Extract huffman codes table from @ref hw_iaa_aecs_compress and store this into @ref hw_iaa_c_huffman_only_table
 *
 * @param [in] aecs_ptr            pointer to valid @ref hw_iaa_aecs_compress
 * @param [out] huffman_table_ptr  pointer to target huffman table
 *
 */
HW_PATH_IAA_AECS_API(void, compress_store_huffman_only_huffman_table, (const hw_iaa_aecs_compress *const aecs_ptr,
                                                                       hw_iaa_c_huffman_only_table *const huffman_table_ptr));
/** @} */

/**
 * @name AECS Compress Service API
 *
 * @brief Contains AECS service functions to compress stream in the `Deflate` format
 *
 * @{
 */

/**
 * @brief Clean compress output accumulator.
 *
 * @param [in, out] aecs_ptr pointer to valid @ref hw_iaa_aecs_compress
 *
 */
static inline
HW_PATH_IAA_AECS_API(void, compress_clean_accumulator, (hw_iaa_aecs_compress *const aecs_ptr)) {
    aecs_ptr->num_output_accum_bits = 0u;
}

/**
 * @brief Flush N output accumulator bits into specified destination.
 * @param [in, out] aecs_ptr        pointer to valid @ref hw_iaa_aecs_compress
 * @param [out]     next_out_pptr   destination
 * @param [in]      bits            bits to flush
 *
 * @note Output accumulator will be cleaned
 *
 */
HW_PATH_IAA_AECS_API(void, compress_accumulator_flush, (hw_iaa_aecs_compress *const aecs_ptr,
                                                        uint8_t **const next_out_pptr,
                                                        const uint32_t bits));

/**
 * @brief Get actual bits count in the output accumulator.
 * @param [in] aecs_ptr pointer to valid @ref hw_iaa_aecs_compress
 *
 * @return actual bits count
 *
 */
static inline
HW_PATH_IAA_AECS_API(uint32_t, compress_accumulator_get_actual_bits, (const hw_iaa_aecs_compress *const aecs_ptr)) {
    return aecs_ptr->num_output_accum_bits;
}


/**
 * @brief Get crc32 and xor checksums values from @ref hw_iaa_aecs_compress.
 *
 * @param [in] aecs_ptr       pointer to valid @ref hw_iaa_aecs_compress
 * @param [out] crc           pointer to save crc32 value
 * @param [out] xor_checksum  pointer to save xor checksum value
 *
 */
static inline
HW_PATH_IAA_AECS_API(void, compress_get_checksums, (const hw_iaa_aecs_compress *const aecs_ptr,
                                                    uint32_t *const crc,
                                                    uint32_t *const xor_checksum)) {
    *xor_checksum = aecs_ptr->xor_checksum;
    *crc          = aecs_ptr->crc;
}

/**
 * @brief Set crc32 and xor checksums seeds into @ref hw_iaa_aecs_compress.
 *
 * @param [out] aecs_ptr pointer to @ref hw_iaa_aecs_compress
 * @param [in] crc           crc32 seed
 * @param [in] xor_checksum  xor checksum seed
 *
 */
static inline
HW_PATH_IAA_AECS_API(void, compress_set_checksums, (hw_iaa_aecs_compress *const aecs_ptr,
                                                    const uint32_t crc,
                                                    const uint32_t xor_checksum)) {
    aecs_ptr->xor_checksum = xor_checksum;
    aecs_ptr->crc          = crc;
}

/**
 * @brief Insert `end-of-block` (eob) symbol into output accumulator.
 * @details Can be useful to terminate `Deflate` block.
 *
 * @param [out] eacs_deflate_ptr pointer to valid @ref hw_iaa_aecs_compress
 * @param [in] eob_symbol        `end-of-block` symbol
 */
HW_PATH_IAA_AECS_API(void, compress_accumulator_insert_eob, (hw_iaa_aecs_compress *const eacs_deflate_ptr,
                                                             const hw_huffman_code eob_symbol));
/** @} */

/* ====== AECS Decompress ====== */

/**
 * @name AECS Huffman Only Decompression API
 *
 * @brief Contains AECS service functions to decompress stream in the `Huffman Only` format
 *
 * @{
 */

/**
 * @brief Setup @ref hw_iaa_aecs_decompress to decompress data with previously calculated huffman codes.
 *
 * @param [out]  aecs_ptr           pointer to @ref hw_iaa_aecs_decompress
 * @param [in]   huffman_table_ptr  pointer to decompression Huffman table
 */
HW_PATH_IAA_AECS_API(void, decompress_set_huffman_only_huffman_table, (hw_iaa_aecs_decompress *const aecs_ptr,
                                                                       hw_iaa_d_huffman_only_table *const huffman_table_ptr));


/**
 * @brief Setup @ref hw_iaa_aecs_decompress to decompress data with huffman codes calculated with provided histogram.
 *
 * @param [in,out] aecs_ptr      pointer to @ref hw_iaa_aecs_decompress
 * @param [in]     histogram_ptr histogram collected with using @ref hw_iaa_descriptor_init_statistic_collector
 *
 * @return 0 if success, 1 otherwise
 *
 */
HW_PATH_IAA_AECS_API(uint32_t, decompress_set_huffman_only_huffman_table_from_histogram, (hw_iaa_aecs_decompress *const aecs_ptr,
                                                                                          const hw_iaa_histogram *const histogram_ptr));

/** @} */

/**
 * @name AECS Decompress Service API
 *
 * @brief Contains AECS service functions to compress stream in the `Deflate` format
 *
 * @{
 */

/**
 * @brief Fill decompress input accumulator with initial data
 *
 * @details Can be used to achieve random access to compressed data.
 *
 * @param [out] aecs_ptr           pointer to @ref hw_iaa_aecs_decompress
 * @param [in] source_ptr         data to decompress
 * @param [in] source_size        data size to decompress
 * @param [in] ignore_start_bits  none-actual bit count in the beginning of the source stream
 * @param [in] ignore_end_bits    none-actual bit count in the end of the source stream
 *
 * @note `source_ptr` must be incremented after success function execution.
 * @ref hw_descriptor must be initiated with updated `source_ptr` value
 *
 * @return Error in case of aecs corruption.
 */
HW_PATH_IAA_AECS_API(uint32_t, decompress_set_input_accumulator, (hw_iaa_aecs_decompress *const aecs_ptr,
                                                                  const uint8_t *const source_ptr,
                                                                  const uint32_t source_size,
                                                                  const uint8_t ignore_start_bits,
                                                                  const uint8_t ignore_end_bits));


/**
 * @brief Clean decompress input accumulator.
 *
 * @param [in, out] aecs_ptr pointer to valid @ref hw_iaa_aecs_decompress
 *
 */
static inline
HW_PATH_IAA_AECS_API(void, decompress_clean_input_accumulator, (hw_iaa_aecs_decompress *const aecs_ptr)) {
    aecs_ptr->input_accum[0]      = 0u;
    aecs_ptr->input_accum_size[0] = 0u;
}


/**
 * @brief Check if decompress input accumulator is empty.
 *
 * @param [in] aecs_ptr pointer to valid @ref hw_iaa_aecs_decompress
 *
 * @return `true` if empty and `false` in the other case
 */
static inline
HW_PATH_IAA_AECS_API(bool, decompress_is_empty_input_accumulator, (hw_iaa_aecs_decompress *const aecs_ptr)) {
    return 0u == aecs_ptr->input_accum_size[0];
}

/**
 * @brief @todo add description.
 *
 * @param [in, out] aecs_ptr pointer to valid @ref hw_iaa_aecs_decompress
 * @param [in]      decompression_state decompression stage to set
 *
 * @return @todo add description
 */
static inline
HW_PATH_IAA_AECS_API(void, decompress_set_decompression_state, (hw_iaa_aecs_decompress *const aecs_ptr,
                                                                hw_iaa_aecs_decompress_state decompression_state)) {
    aecs_ptr->decompress_state = (uint16_t) decompression_state;
}

/**
 * @brief @todo add description
 *
 * @param [in] aecs_ptr pointer to valid @ref hw_iaa_aecs_decompress
 * @param [in] raw_dictionary_ptr pointer to dictionary
 * @param [in] dictionary_size    dictionary size in bytes
 *
 * @return @todo add description
 */
HW_PATH_IAA_AECS_API(void, decompress_set_dictionary, (hw_iaa_aecs_decompress *const aecs_ptr,
                                                       const uint8_t *const raw_dictionary_ptr,
                                                       const size_t dictionary_size));

/** @} */

/* ====== AECS Filter ====== */

/**
 * @name AECS Filter Service API
 *
 * @brief Contains AECS service functions to specify special filtering options
 *
 * @{
 */

/**
 * @brief Set initial output index for analytic operations.
 * @details Makes sense for filtering only.
 *
 * @param [out] aecs_ptr     pointer to @ref hw_iaa_aecs_analytic
 * @param [in] output_index  output index
 *
 */
static inline
HW_PATH_IAA_AECS_API(void, filter_set_initial_output_index, (hw_iaa_aecs_analytic *const aecs_ptr,
                                                             const uint32_t output_index)) {
    aecs_ptr->filtering_options.output_mod_idx = output_index;
}

/**
 * @brief Set count of bytes to drop for analytic.
 * @details Bytes will be dropped after decompression pass before filtering one.
 *
 * @param [out] aecs_ptr     pointer to @ref hw_iaa_aecs_analytic
 * @param [in] bytes_count   count dropped bytes
 *
 */
static inline
HW_PATH_IAA_AECS_API(void, filter_set_drop_initial_decompressed_bytes, (hw_iaa_aecs_analytic *const aecs_ptr,
        const uint16_t bytes_count)) {
    aecs_ptr->filtering_options.drop_initial_decompress_out_bytes = bytes_count;
}

/**
 * @brief Set crc32 seed into @ref hw_iaa_aecs_analytic.
 * @details Can be used to specify crc32 seed for analytic (filtering or decompress).
 *
 * @param [out] aecs_ptr pointer to @ref hw_iaa_aecs_analytic
 * @param [in] seed      crc32 seed
 *
 */
static inline
HW_PATH_IAA_AECS_API(void, decompress_set_crc_seed, (hw_iaa_aecs_analytic *const aecs_ptr, const uint32_t seed)) {
    aecs_ptr->filtering_options.crc = seed;
}

/** @} */

#ifdef __cplusplus
}
#endif

#endif //HW_PATH_HW_AECS_API_H_

/** @} */
