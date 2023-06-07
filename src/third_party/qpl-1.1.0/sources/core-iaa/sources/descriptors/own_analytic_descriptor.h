/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

#ifndef HW_PATH_OWN_ANALYTIC_DESCRIPTOR_H_
#define HW_PATH_OWN_ANALYTIC_DESCRIPTOR_H_

#include <assert.h>

#include "hw_definitions.h"
#include "own_hw_definitions.h"

#ifdef __cplusplus
extern "C" {
#endif

HW_PATH_BYTE_PACKED_STRUCTURE_BEGIN
{
    uint32_t trusted_fields;            /**< @todo */
    uint32_t op_code_op_flags;          /**< Opcode 31:24, opflags 23:0 */
    uint8_t  *completion_record_ptr;    /**< Completion record address */
    uint8_t  *first_source_ptr;         /**< Source 1 address */
    uint8_t  *destination_ptr;          /**< Destination address */
    uint32_t first_source_size;         /**< Source 1 transfer size */
    uint16_t interrupt_handle;          /**< Not used (completion interrupt handle) */
    uint16_t decompression_flags;       /**< (De)compression flags */
    uint8_t  *second_source_ptr;        /**< Source 2 address | AECS address (32-bit aligned) */
    uint32_t max_destination_size;      /**< Maximum destination size */
    uint32_t second_source_size;        /**< Source 2 transfer size | AECS size (multiple of 32-bytes, LE 288 bytes) */
    uint32_t filter_flags;              /**< Filter flags */
    uint32_t input_elements;            /**< Number of input elements */
} own_hw_analytic_descriptor;
HW_PATH_BYTE_PACKED_STRUCTURE_END


/*
 * Check that descriptor has a correct size
 */
static_assert(sizeof(own_hw_analytic_descriptor) == HW_PATH_DESCRIPTOR_SIZE, "Descriptor size is not correct");

#ifdef __cplusplus
}
#endif

#endif //HW_PATH_OWN_ANALYTIC_DESCRIPTOR_H_
