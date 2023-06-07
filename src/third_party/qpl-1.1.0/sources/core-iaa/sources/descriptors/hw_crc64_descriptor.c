/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

#include <assert.h>

#include "own_hw_definitions.h"
#include "hw_descriptors_api.h"

#define PLATFORM 2
#include "qplc_memop.h"

/**
 * @brief Defines a type of the Intel® In-Memory Analytics Accelerator (Intel® IAA)
 * crc64 descriptor
 */
HW_PATH_BYTE_PACKED_STRUCTURE_BEGIN {
    uint32_t trusted_fields;         /**< 19:0 PASID - process address space ID; 30:20 - reserved; 31 - User/Supervisor */
    uint32_t op_code_op_flags;       /**< Opcode 31:24, opflags 23:0 */
    uint8_t  *completion_record_ptr; /**< Completion record address */
    uint8_t  *source_ptr;            /**< Source address */
    uint8_t  reserved_1[8];          /**< Reserved */
    uint32_t calculated_bytes;       /**< Bytes to calculation */
    uint16_t interruption_handle;    /**< Completion interrupt handle */
    uint16_t crc_flags;              /**< Crc operation flags */
    uint8_t  reserved_2[16];         /**< Reserved */
    uint64_t polynomial;             /**< Polynomial */
} own_hw_crc64_descriptor;
HW_PATH_BYTE_PACKED_STRUCTURE_END

/*
 * Check that descriptor has a correct size
 */
static_assert(sizeof(own_hw_crc64_descriptor) == HW_PATH_DESCRIPTOR_SIZE, "Descriptor size is not correct");

HW_PATH_IAA_API(void, descriptor_init_crc64, (hw_descriptor *const descriptor_ptr,
                                              const uint8_t *const source_ptr,
                                              const uint32_t size,
                                              const uint64_t polynomial,
                                              const bool is_be_bit_order,
                                              const bool is_inverse)) {
    avx512_qplc_zero_8u((uint8_t *) descriptor_ptr, sizeof(hw_descriptor));

    own_hw_crc64_descriptor *const this_ptr = (own_hw_crc64_descriptor *) descriptor_ptr;

    this_ptr->op_code_op_flags      = ADOF_OPCODE(QPL_OPCODE_CRC64);
    this_ptr->source_ptr            = (uint8_t *) source_ptr;
    this_ptr->calculated_bytes      = size;

    if (is_be_bit_order) {
        this_ptr->crc_flags = ADC64F_BE;
    }

    if (is_inverse) {
        this_ptr->crc_flags |= ADC64F_INVCRC;
    }

    this_ptr->polynomial = polynomial;
}
