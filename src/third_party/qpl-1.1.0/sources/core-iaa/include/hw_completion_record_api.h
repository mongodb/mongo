/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

/**
 * @brief Contains API used to work with Intel® In-Memory Analytics Accelerator (Intel® IAA) Completion Record structure
 *
 * @defgroup HW_COMPLETION_RECORD_API Completion Record API
 * @ingroup HW_PUBLIC_API
 * @{
 */

#ifndef HW_PATH_HW_COMPLETION_RECORD_API_H_
#define HW_PATH_HW_COMPLETION_RECORD_API_H_

#include <stdint.h>

#include "hw_definitions.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TRIVIAL_COMPLETE  0xFFu  /**< Status to emulate successful operation execution */

/**
 * @brief Describes the structure (in memory) that the device writes when an operation is completed or error encountered.
 *
 * @warning The structure address must be 64-byte aligned.
 *
 * @note There are no fields for CRC completion record and you should calculate them on your own using byte offsets listed in
 * Intel IAA specification.
 */
HW_PATH_BYTE_PACKED_STRUCTURE_BEGIN {
    hw_operation_status status;      /**< Descriptor execution result */
    hw_operation_error error_code;   /**< Operation execution status */
    uint16_t reserved0;              /**< Reserved bytes */
    uint32_t bytes_completed;        /**< Total processed input bytes */
    uint64_t fault_address;          /**< Page Fault address */
    uint64_t invalid_flags;          /**< Contain bit-mask for invalid flags */
    uint32_t output_size;            /**< Number of bytes written into output buffer */
    uint8_t  output_bits;            /**< Number of actual bits in the last output byte */
    uint8_t  reserved1;              /**< Reserved bytes */
    uint16_t xor_checksum;           /**< Contains XOR checksum computed on uncompressed data */
    uint32_t crc;                    /**< Contains CRC checksum computed on uncompressed data */
    uint32_t min_first_agg;          /**< Minimum value in output */
    uint32_t max_last_agg;           /**< Maximum value in output */
    uint32_t sum_agg;                /**< Sum of all values */
    uint64_t reserved2;              /**< Reserved bytes */
    uint64_t reserved3;              /**< Reserved bytes */
} hw_iaa_completion_record;
HW_PATH_BYTE_PACKED_STRUCTURE_END

/**
 * @brief Set Completion record as fictional completed.
 * @details Can be used to emulate success task execution while input data is collected into accumulation buffer.
 * @param completion_record_ptr pointer to @ref hw_iaa_completion_record
 * @param bytes_processed       fictional bytes completed
 */
static inline void hw_iaa_completion_record_init_trivial_completion(hw_iaa_completion_record *const completion_record_ptr,
                                                                    const uint32_t bytes_processed) {
    completion_record_ptr->status          = TRIVIAL_COMPLETE;
    completion_record_ptr->error_code      = 0u;
    completion_record_ptr->bytes_completed = bytes_processed;
    completion_record_ptr->output_size     = 0u;
}

#ifdef __cplusplus
}
#endif

#endif //HW_PATH_INCLUDE_HW_COMPLETION_RECORD_API_H_

/** @} */
