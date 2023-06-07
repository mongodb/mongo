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
 * @brief Contains API to work with Intel® In-Memory Analytics Accelerator (Intel® IAA) Descriptors.
 *
 * @defgroup HW_DESCRIPTORS_API Descriptors API
 * @ingroup HW_PUBLIC_API
 * @{
 */

#ifndef HW_PATH_HW_DESCRIPTORS_API_H_
#define HW_PATH_HW_DESCRIPTORS_API_H_

#include "own_hw_definitions.h"
#include "hw_definitions.h"
#include "hw_aecs_api.h"
#include "hw_iaa_flags.h"
#include "stdbool.h"

#if !defined( HW_PATH_IAA_API )
#define HW_PATH_IAA_API(type, name, arg) type HW_STDCALL hw_iaa_##name arg
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* ################# SIMPLE OPERATIONS ################# */

/**
 * @name Simple Descriptors API
 *
 * @brief Contains setters to initialize @ref hw_descriptor to perform filtering operation.
 *
 * @{
 */

/**
 * @brief Inits @ref hw_descriptor for CRC_64 operation
 * @param[in,out] descriptor_ptr        pointer to allocated descriptor to init
 * @param[in] source_ptr                pointer to the source
 * @param[in] size                      number of bytes to crc calculation
 * @param[in] polynomial                polynomial for CRC64
 * @param[in] is_be_bit_order           flag is used for the data is viewed as Big Endian
 * @param[in] is_inverse                is crc inversion forward or inverse
 *
 * @note Memory pointed with `completion_record_ptr` will be changed after descriptor executed
 *
 */
HW_PATH_IAA_API(void, descriptor_init_crc64, (hw_descriptor * descriptor_ptr,
                                              const uint8_t *source_ptr,
                                              uint32_t      size,
                                              uint64_t      polynomial,
                                              bool          is_be_bit_order,
                                              bool          is_inverse));
/** @} */


/* ################# ANALYTIC OPERATIONS ################# */

/**
 * @name Analytic(Filtering) API
 *
 * @brief Contains setters to initialize @ref hw_descriptor to perform filtering operation.
 *
 * @{
 */


/**
 * @brief Setup filtering descriptor input stream (`source-1`) and one's properties.
 *
 * @param[out] descriptor_ptr    @ref hw_descriptor
 * @param[in] source_ptr         pointer to stream beginning
 * @param[in] source_size        stream size
 * @param[in] elements_count     element number to process
 * @param[in] input_format       stream input format
 * @param[in] input_bit_width    element bit-width
 *
 */
HW_PATH_IAA_API(void, descriptor_analytic_set_filter_input, (hw_descriptor *const descriptor_ptr,
                                                             uint8_t *const source_ptr,
                                                             const uint32_t source_size,
                                                             const uint32_t elements_count,
                                                             const hw_iaa_input_format input_format,
                                                             const uint32_t input_bit_width));


/**
 * @brief Setup filtering descriptor output stream and one's properties
 *
 * @param[out] descriptor_ptr @ref hw_descriptor
 * @param[in] output_ptr      pointer to stream beginning
 * @param[in] output_size     stream size
 * @param[in] output_format   stream output format
 *
 */
HW_PATH_IAA_API(void, descriptor_analytic_set_filter_output, (hw_descriptor *const descriptor_ptr,
                                                              uint8_t *const output_ptr,
                                                              const uint32_t output_size,
                                                              const hw_iaa_output_format output_format));


/**
 * @brief Setup filtering descriptor to perform `scan` operation.
 *
 * @details `Scan` operation scans `source-1` for values that meet the requirement that is in range [`low_border`;`high_border`].
 *
 * @param[out] descriptor_ptr   @ref hw_descriptor
 * @param[in] low_border        beginning of range
 * @param[in] high_border       end of range
 * @param[in] filter_config_ptr relative @ref hw_iaa_aecs_analytic
 *
 * @note Operation's streams:
 *  - Output: `bit-vector` that is set with @ref hw_iaa_descriptor_analytic_set_filter_output.
 *  - Input: `bit-vector` of elements that is set with @ref hw_iaa_descriptor_analytic_set_filter_input.
 *
 */
HW_PATH_IAA_API(void, descriptor_analytic_set_scan_operation, (hw_descriptor *const descriptor_ptr,
                                                               const uint32_t low_border,
                                                               const uint32_t high_border,
                                                               hw_iaa_aecs_analytic *const filter_config_ptr));


/**
 * @brief Setup filtering descriptor to perform `extract` operation
 *
 * @details `Extract` operation extracts a sub-vector from the `source-1` starting from index `first_element_index` and
 * finishing at index `last_element_index`.
 *
 * @param[out] descriptor_ptr       @ref hw_descriptor
 * @param[in] first_element_index   first element index to extract
 * @param[in] last_element_index    last element index to extract
 * @param[in] filter_config_ptr     relative @ref hw_iaa_aecs_analytic
 *
 * @note Operation's streams:
 *  - Output: `array-vector` that is set with @ref hw_iaa_descriptor_analytic_set_filter_output.
 *  - Input: `bit-vector` of elements that is set with @ref hw_iaa_descriptor_analytic_set_filter_input.
 */
HW_PATH_IAA_API(void, descriptor_analytic_set_extract_operation, (hw_descriptor *const descriptor_ptr,
                                                                  const uint32_t first_element_index,
                                                                  const uint32_t last_element_index,
                                                                  hw_iaa_aecs_analytic *const filter_config_ptr));

/**
 * @brief Setup filtering descriptor to perform `select` operation
 *
 * @details `Select` operation selects a elements from the `source-1` in accordance with indexes of non-zero elements
 * in the `mask` stream (bit-stream).
 *
 * @param[out] descriptor_ptr       @ref hw_descriptor
 * @param[in] mask_ptr              pointer to mask
 * @param[in] mask_size             size of mask
 * @param[in] is_mask_big_endian    mask is in big-endian encoding format
 *
 * @note Operation's streams:
 *  - Output: `array-vector` that set with @ref hw_iaa_descriptor_analytic_set_filter_output.
 *  - Input: bit-vector of elements that set with @ref hw_iaa_descriptor_analytic_set_filter_input.
 *
 */
HW_PATH_IAA_API(void, descriptor_analytic_set_select_operation, (hw_descriptor *const descriptor_ptr,
                                                                 uint8_t *const mask_ptr,
                                                                 const uint32_t mask_size,
                                                                 const bool is_mask_big_endian));


/**
 * @brief Setup filtering descriptor to perform `expand` operation
 *
 * @details `Expand` operation Expands `source-1` using `mask` stream (bit-stream). `mask` modifies an output
 * by the following way:
 *  - Each 0-bit element from `mask` writes a zero into the output stream
 *  - Each 1-bit element from `mask` writes the next entry from input stream
 *
 * @param[out] descriptor_ptr       @ref hw_descriptor
 * @param[in] mask_ptr              pointer to mask
 * @param[in] mask_size             size of mask
 * @param[in] is_mask_big_endian    mask is in big-endian encoding format
 *
 * @note Operation's streams:
 *  - Output: `array-vector` that set with @ref hw_iaa_descriptor_analytic_set_filter_output.
 *  - Input: `bit-vector` of elements that set with @ref hw_iaa_descriptor_analytic_set_filter_input.
 *
 */
HW_PATH_IAA_API(void, descriptor_analytic_set_expand_operation, (hw_descriptor *const descriptor_ptr,
                                                                 uint8_t *const mask_ptr,
                                                                 const uint32_t mask_size,
                                                                 const bool is_mask_big_endian));

/**
 * @brief Setup decompress pass for filtering operation
 *
 * @param[out] descriptor_ptr                   @ref hw_descriptor
 * @param[in] is_big_endian_compressed_stream   compressed stream (source-1) is in big-endian encoding format
 * @param[in] ignore_last_bits                  don't decompress last n bits
 *
 */
HW_PATH_IAA_API(void, descriptor_analytic_enable_decompress, (hw_descriptor *const descriptor_ptr,
                                                              bool is_big_endian_compressed_stream,
                                                              uint32_t ignore_last_bits));

/** @} */

/* ################# COMPRESS OPERATIONS ################# */

/**
 * @name Compress API
 *
 * @brief Contains setters to initialize @ref hw_descriptor to perform compress operation.
 *
 * @{
 */

/**
 * @brief Setup descriptor to collect statistic for `source_ptr` and save one into `histogram_ptr` (creates `compress descriptor`).
 * Statistics can be used to build huffman tree.
 *
 * @param[out] descriptor_ptr  @ref hw_descriptor
 * @param[in] source_ptr       source stream
 * @param[in] source_size      source size
 * @param[in] histogram_ptr    pointer to @ref hw_iaa_histogram to fill
 *
 */
HW_PATH_IAA_API(void, descriptor_init_statistic_collector, (hw_descriptor *const descriptor_ptr,
                                                            const uint8_t *const source_ptr,
                                                            const uint32_t source_size,
                                                            hw_iaa_histogram *const histogram_ptr));

/**
 * @brief Setup descriptor to perform `Compress` operation (creates `compress descriptor`)
 *
 * @param[out] descriptor_ptr @ref hw_descriptor
 *
 */
HW_PATH_IAA_API(void, descriptor_init_compress_body, (hw_descriptor *const descriptor_ptr));


/**
 * @brief Setup descriptor to perform stream compression
 *
 * @param[out] descriptor_ptr    @ref hw_descriptor previously inited with @ref hw_iaa_descriptor_init_compress_body
 * @param[in] source_ptr        input data
 * @param[in] source_size       input size
 * @param[in] destination_ptr   destination buffer
 * @param[in] destination_size  destination buffer size
 */
HW_PATH_IAA_API(void, descriptor_init_deflate_body, (hw_descriptor *const descriptor_ptr,
                                                     uint8_t *const source_ptr,
                                                     const uint32_t source_size,
                                                     uint8_t *const destination_ptr,
                                                     const uint32_t destination_size));

/**
 * @todo API will be described after refactoring completed
 */
HW_PATH_IAA_API(void, descriptor_init_compress_verification, (hw_descriptor * descriptor_ptr));

/**
 * @brief Setup `compress descriptor` to to compress stream with huffman codes only, e.g without LZ coding.
 *
 * @param[out] descriptor_ptr @ref hw_descriptor previously inited with @ref hw_iaa_descriptor_init_compress_body or @ref hw_iaa_descriptor_init_statistic_collector
 *
 */
static inline
HW_PATH_IAA_API(void, descriptor_compress_set_huffman_only_mode, (hw_descriptor *const descriptor_ptr)) {
    const uint16_t COMPRESSION_FLAG_BIT_MASK = 1u << 4u;
    const uint8_t  COMPRESSION_FLAG_OFFSET   = 38u;

    *(uint16_t *) (&descriptor_ptr->data[COMPRESSION_FLAG_OFFSET]) |= COMPRESSION_FLAG_BIT_MASK;
}

/**
 * @brief Setup `compress descriptor`to compress stream in the `big-endian` format
 *
 * @param[out] descriptor_ptr @ref hw_descriptor previously inited with @ref hw_iaa_descriptor_init_compress_body or @ref hw_iaa_descriptor_init_statistic_collector
 *
 */
static inline
HW_PATH_IAA_API(void, descriptor_compress_set_be_output_mode, (hw_descriptor *const descriptor_ptr)) {
    const uint16_t COMPRESSION_FLAG_BIT_MASK = 1u << 5u;
    const uint8_t  COMPRESSION_FLAG_OFFSET   = 38u;

    *(uint16_t *) (&descriptor_ptr->data[COMPRESSION_FLAG_OFFSET]) |= COMPRESSION_FLAG_BIT_MASK;
}

/**
 * @brief Setup `compress descriptor` to compress stream by `mini-blocks`
 *
 * @param[out] descriptor_ptr @ref hw_descriptor previously inited with @ref hw_iaa_descriptor_init_compress_body or @ref hw_iaa_descriptor_init_statistic_collector
 * @param[in]  mini_block_size size of `mini-blocks` in the stream
 *
 */
static inline
HW_PATH_IAA_API(void, descriptor_compress_set_mini_block_size, (hw_descriptor *const descriptor_ptr,
                                                                const hw_iaa_mini_block_size_t mini_block_size)) {
    const uint16_t COMPRESSION_FLAG_BIT_MASK = (((uint32_t) (mini_block_size) & 7u) << 6u);
    const uint8_t  COMPRESSION_FLAG_OFFSET   = 38u;

    *(uint16_t *) (&descriptor_ptr->data[COMPRESSION_FLAG_OFFSET]) |= COMPRESSION_FLAG_BIT_MASK;
}

/**
 * @brief Setup `compress descriptor`to terminate compressed stream by concrete way.
 *
 * @param[out] descriptor_ptr @ref hw_descriptor previously inited with @ref hw_iaa_descriptor_init_compress_body or @ref hw_iaa_descriptor_init_statistic_collector
 * @param[in] terminator      used stream termination
 *
 */
static inline
HW_PATH_IAA_API(void, descriptor_compress_set_termination_rule, (hw_descriptor *const descriptor_ptr,
                                                                 const hw_iaa_terminator_t terminator)) {
    const uint16_t COMPRESSION_FLAG_BIT_MASK = (((terminator) & 3u) << 2u);
    const uint8_t  COMPRESSION_FLAG_OFFSET   = 38u;

    *(uint16_t *) (&descriptor_ptr->data[COMPRESSION_FLAG_OFFSET]) |= COMPRESSION_FLAG_BIT_MASK;
}

/**

 * @brief Setup AECS to `compress descriptor`
 *
 * @param descriptor_ptr @ref hw_descriptor previously inited with @ref hw_iaa_descriptor_init_compress_body or @ref hw_iaa_descriptor_init_statistic_collector
 * @param aecs_ptr       @ref hw_iaa_aecs_compress
 * @param access_policy  @ref hw_iaa_aecs_access_policy
 *
 */
HW_PATH_IAA_API(void, descriptor_compress_set_aecs, (hw_descriptor *const descriptor_ptr,
                                                     hw_iaa_aecs *const aecs_ptr,
                                                     const hw_iaa_aecs_access_policy access_policy));

/**
 * @todo API will be described after refactoring completed
 */
HW_PATH_IAA_API(void, descriptor_compress_verification_write_initial_index, (hw_descriptor *const descriptor_ptr,
                                                                             hw_iaa_aecs_analytic *const aecs_analytic_ptr,
                                                                             uint32_t crc,
                                                                             uint32_t bit_offset));

/** @} */

/* ################# DECOMPRESS OPERATIONS ################# */

/**
 * @name Decompress API
 *
 * @brief Contains setters to initialize @ref hw_descriptor to perform decompress operation.
 *
 * @{
 */

/**
 * @todo API will be described after refactoring completed
 */
HW_PATH_IAA_API(void, descriptor_init_huffman_only_decompress, (hw_descriptor *const descriptor_ptr,
                                                                hw_iaa_aecs *const aecs_ptr,
                                                                const bool huffman_be,
                                                                const uint8_t ignore_end_bits));


/**
 * @todo API will be described after refactoring completed
 */
HW_PATH_IAA_API(void, descriptor_init_inflate, (hw_descriptor *const descriptor_ptr,
                                                hw_iaa_aecs *const aecs_ptr,
                                                const uint32_t aecs_size,
                                                const hw_iaa_aecs_access_policy access_policy));

/**
 * @todo API will be described after refactoring completed
 */
HW_PATH_IAA_API(void, descriptor_init_inflate_header, (hw_descriptor *const descriptor_ptr,
                                                       hw_iaa_aecs *const aecs_ptr,
                                                       const uint8_t ignore_end_bits,
                                                       const hw_iaa_aecs_access_policy access_policy));

/**
 * @todo API will be described after refactoring completed
 */
HW_PATH_IAA_API(void, descriptor_init_inflate_body, (hw_descriptor *const descriptor_ptr,
                                                     hw_iaa_aecs *const aecs_ptr,
                                                     const uint8_t ignore_end_bit));

/**
 * @todo API will be described after refactoring completed
 */
HW_PATH_IAA_API(void, descriptor_inflate_set_aecs, (hw_descriptor *const descriptor_ptr,
                                                    hw_iaa_aecs *const aecs_ptr,
                                                    const uint32_t aecs_size,
                                                    const hw_iaa_aecs_access_policy access_policy));

/**
 * @todo API will be described after refactoring completed
 */
HW_PATH_IAA_API(void, descriptor_set_inflate_stop_check_rule, (hw_descriptor *const descriptor_ptr,
                                                               hw_iaa_decompress_start_stop_rule_t rules,
                                                               bool check_for_eob));
/**
 * @todo API will be described after refactoring completed
 */
static inline
HW_PATH_IAA_API(void, descriptor_decompress_set_mini_block_size, (hw_descriptor *const descriptor_ptr,
                                                                  const hw_iaa_mini_block_size_t mini_block_size)) {
    const uint16_t DECOMPRESSION_FLAG_BIT_MASK = (((uint32_t) (mini_block_size) & 7u) << 10u);
    const uint8_t  DECOMPRESSION_FLAG_OFFSET   = 38u;

    *(uint16_t *) (&descriptor_ptr->data[DECOMPRESSION_FLAG_OFFSET]) |= DECOMPRESSION_FLAG_BIT_MASK;
}

/**
* @todo API will be described after refactoring completed
*/
static inline
HW_PATH_IAA_API(void, descriptor_inflate_set_flush, (hw_descriptor *const descriptor_ptr)) {
    const uint16_t DECOMPRESSION_FLAG_BIT_MASK = 0x02;
    const uint8_t  DECOMPRESSION_FLAG_OFFSET   = 38u;

    *(uint16_t *) (&descriptor_ptr->data[DECOMPRESSION_FLAG_OFFSET]) |= DECOMPRESSION_FLAG_BIT_MASK;
}

/** @} */

/* ################# DESCRIPTOR SETTERS ################# */

/**
 * @name Service API
 *
 * @brief Contains common setters to initialize @ref hw_descriptor
 *
 * @{
 */

/**
 * @todo API will be described after refactoring completed
 */
static inline
HW_PATH_IAA_API(void, descriptor_set_input_buffer, (hw_descriptor *const descriptor_ptr,
                                                    uint8_t *const buffer_ptr,
                                                    const uint32_t size)) {
    hw_iaa_analytics_descriptor *const this_ptr = (hw_iaa_analytics_descriptor *) descriptor_ptr;

    this_ptr->src1_ptr  = buffer_ptr;
    this_ptr->src1_size = size;
}

/**
 * @todo API will be described after refactoring completed
 */
static inline
HW_PATH_IAA_API(void, descriptor_shift_input_buffer, (hw_descriptor *const descriptor_ptr, uint32_t shift)) {
    hw_iaa_analytics_descriptor *const this_ptr = (hw_iaa_analytics_descriptor *) descriptor_ptr;

    this_ptr->src1_ptr += shift;
    this_ptr->src1_size -= shift;
}

/**
 * @todo API will be described after refactoring completed
 */
static inline
HW_PATH_IAA_API(void, descriptor_set_number_of_elements, (hw_descriptor *const descriptor_ptr,
                                                        uint32_t number_of_elements)) {
    hw_iaa_analytics_descriptor *const this_ptr = (hw_iaa_analytics_descriptor *) descriptor_ptr;

    this_ptr->num_input_elements = number_of_elements;
}

/**
 * @todo API will be described after refactoring completed
 */
static inline
HW_PATH_IAA_API(void, descriptor_set_output_buffer, (hw_descriptor *const descriptor_ptr,
                                                     uint8_t *const buffer_ptr,
                                                     const uint32_t size)) {
    hw_iaa_analytics_descriptor *const this_ptr = (hw_iaa_analytics_descriptor *) descriptor_ptr;

    this_ptr->dst_ptr      = buffer_ptr;
    this_ptr->max_dst_size = size;
}

/**
* @todo API will be described after refactoring completed
*/
static inline
HW_PATH_IAA_API(void, descriptor_shift_output_buffer, (hw_descriptor *const descriptor_ptr, uint32_t shift)) {
    hw_iaa_analytics_descriptor *const this_ptr = (hw_iaa_analytics_descriptor *) descriptor_ptr;

    this_ptr->dst_ptr += shift;
    this_ptr->max_dst_size -= shift;
}


/**
 * @todo API will be described after refactoring completed
 */
static inline
HW_PATH_IAA_API(void, descriptor_set_crc_rfc3720, (hw_descriptor *const descriptor_ptr)) {
    const uint8_t  CRC32_C_FLAG_BIT_MASK    = 0x20u;
    const uint32_t CRC32_C_FLAG_BYTE_OFFSET = 6u;

    descriptor_ptr->data[CRC32_C_FLAG_BYTE_OFFSET] |= CRC32_C_FLAG_BIT_MASK;
}

/**
 * @todo API will be described after refactoring completed
 */
HW_PATH_IAA_API(void, descriptor_set_completion_record, (hw_descriptor *const descriptor_ptr,
                                                         HW_PATH_VOLATILE hw_completion_record *const completion_record));

/** @} */

/* ################# DESCRIPTOR GETTERS ################# */

/**
 * @name Service API
 *
 * @brief Contains common getters to retrieve information from @ref hw_descriptor
 *
 * @{
 */

/**
 * @todo API will be described after refactoring completed
 */
HW_PATH_IAA_API(uint32_t, descriptor_get_source1_bit_width, (const hw_descriptor *const descriptor_ptr));

/**
* @todo API will be described after refactoring completed
*/
static inline
HW_PATH_IAA_API(void, descriptor_get_output_buffer, (hw_descriptor *const descriptor_ptr,
                                                     uint8_t **const buffer_ptr,
                                                     uint32_t *const size)) {
    hw_iaa_analytics_descriptor *const this_ptr = (hw_iaa_analytics_descriptor *) descriptor_ptr;

    *buffer_ptr = this_ptr->dst_ptr;
    *size       = this_ptr->max_dst_size;
}

/**
 * @todo API will be described after refactoring completed
 */
static inline
HW_PATH_IAA_API(uint32_t, descriptor_get_number_of_elements, (hw_descriptor *const descriptor_ptr)) {
    hw_iaa_analytics_descriptor *const this_ptr = (hw_iaa_analytics_descriptor *) descriptor_ptr;

    return this_ptr->num_input_elements;
}

/**
 * @todo API will be described after refactoring completed
 */
static inline
HW_PATH_IAA_API(void, descriptor_get_input_buffer, (hw_descriptor *const descriptor_ptr,
        uint8_t **const buffer_ptr,
        uint32_t *const size)) {
    hw_iaa_analytics_descriptor *const this_ptr = (hw_iaa_analytics_descriptor *) descriptor_ptr;

    *buffer_ptr = this_ptr->src1_ptr;
    *size       = this_ptr->src1_size;
}

/**
 * @todo API will be described after refactoring completed
 */
HW_PATH_IAA_API(void, descriptor_set_completion_record, (hw_descriptor *const descriptor_ptr,
                                                         HW_PATH_VOLATILE hw_completion_record *const completion_record));

/**
 * @todo API will be described after refactoring completed
 */
static inline
HW_PATH_IAA_API(void, descriptor_compress_verification_set_index_table, (hw_descriptor *const descriptor_ptr,
                                                                         uint64_t *const index_table_ptr,
                                                                         const uint32_t size,
                                                                         const uint32_t capacity)) {
    hw_iaa_descriptor_set_output_buffer(descriptor_ptr,
                                        (uint8_t *)(&index_table_ptr[size]),
                                        (capacity - size) * sizeof(uint64_t));
}


/**
 * @todo API will be described after refactoring completed
 */
static inline
HW_PATH_IAA_API(void, descriptor_hint_cpu_cache_as_destination, (hw_descriptor *const descriptor_ptr, bool flag)) {
    const uint8_t  CACHE_CONTROL_FLAG_BIT_MASK    = 0x01u;
    const uint32_t CACHE_CONTROL_FLAG_BYTE_OFFSET = 5u;

    // Cache control is a reserved field for CRC64, so set the flag to false to clear this field
    if (QPL_OPCODE_CRC64 == ADOF_GET_OPCODE(((hw_iaa_analytics_descriptor *)descriptor_ptr)->op_code_op_flags)) {
        flag = false;
    }

    if(flag)
        descriptor_ptr->data[CACHE_CONTROL_FLAG_BYTE_OFFSET] |= CACHE_CONTROL_FLAG_BIT_MASK;
    else
        descriptor_ptr->data[CACHE_CONTROL_FLAG_BYTE_OFFSET] &= ~CACHE_CONTROL_FLAG_BIT_MASK;
}

/**
 * @todo API will be described after refactoring completed
 */
static inline
HW_PATH_IAA_API(void, descriptor_set_block_on_fault, (hw_descriptor *const descriptor_ptr, bool flag)) {
    const uint8_t  BLOCK_ON_FAULT_FLAG_BIT_MASK    = 0x02u;
    const uint32_t BLOCK_ON_FAULT_FLAG_BYTE_OFFSET = 4u;

    if(flag)
        descriptor_ptr->data[BLOCK_ON_FAULT_FLAG_BYTE_OFFSET] |= BLOCK_ON_FAULT_FLAG_BIT_MASK;
    else
        descriptor_ptr->data[BLOCK_ON_FAULT_FLAG_BYTE_OFFSET] &= ~BLOCK_ON_FAULT_FLAG_BIT_MASK;
}

/**
 * @todo API will be described after refactoring completed
 */
HW_PATH_IAA_API(void, descriptor_reset, (hw_descriptor *const descriptor_ptr));

/** @} */

#ifdef __cplusplus
}
#endif

#endif //HW_PATH_HW_DESCRIPTORS_API_H_

/** @} */
