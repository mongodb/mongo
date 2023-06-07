/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

#include <assert.h>
#include "hw_descriptors_api.h"
#include "own_hw_definitions.h"

#define PLATFORM 2
#include "qplc_memop.h"

HW_PATH_BYTE_PACKED_STRUCTURE_BEGIN {
    uint32_t trusted_fields;          /**< @todo */
    uint32_t op_code_op_flags;        /**< Opcode 31:24, operation flags 23:0 */
    uint8_t  *completion_record_ptr;  /**< Completion record address */
    uint8_t  *source_ptr;             /**< Source address */
    uint8_t  *destination_ptr;        /**< Destination address */
    uint32_t source_size;             /**< Source transfer size */
    uint16_t interrupt_handle;        /**< Not used (completion interrupt handle) */
    uint16_t compression_flags;       /**< Compression flags */
    uint8_t *aecs_ptr;                /**< AECS address (32-bit aligned) */
    uint32_t max_destination_size;    /**< Maximum destination size */
    uint32_t aecs_size;               /**< AECS size (multiple of 32-bytes, LE 288 bytes) */
    uint8_t  reserved[8];             /**< Reserved bytes */
} own_hw_compress_descriptor;
HW_PATH_BYTE_PACKED_STRUCTURE_END

/*
 * Check that descriptor has a correct size
 */
static_assert(sizeof(own_hw_compress_descriptor) == HW_PATH_DESCRIPTOR_SIZE, "Descriptor size is not correct");

HW_PATH_IAA_API(void, descriptor_init_statistic_collector, (hw_descriptor *const descriptor_ptr,
                                                           const uint8_t *const source_ptr,
                                                           const uint32_t source_size,
                                                           hw_iaa_histogram *const histogram_ptr)) {
    own_hw_compress_descriptor *const this_ptr = (own_hw_compress_descriptor *) descriptor_ptr;

    this_ptr->source_ptr           = (uint8_t *) source_ptr;
    this_ptr->source_size          = source_size;
    this_ptr->destination_ptr      = (uint8_t *) histogram_ptr;
    this_ptr->max_destination_size = sizeof (hw_iaa_histogram);
    this_ptr->aecs_ptr             = NULL;
    this_ptr->aecs_size            = 0u;

    avx512_qplc_zero_8u(this_ptr->reserved, sizeof(this_ptr->reserved));

    this_ptr->op_code_op_flags = ADOF_OPCODE(QPL_OPCODE_COMPRESS);

    this_ptr->compression_flags = ADCF_STATS_MODE;

    // reserved in case of SWQ
    // , so need to make sure that there is no garbage is stored
    this_ptr->trusted_fields = 0u;

    // reserved when bit #4 in op_code_op_flags is 0
    // (Request Completion Interrupt flag is 0, no interrupt is generated)
    // , so need to make sure that there is no garbage is stored
    if (!(this_ptr->op_code_op_flags & (1 << 3))) this_ptr->interrupt_handle = 0u;
}

HW_PATH_IAA_API(void, descriptor_init_compress_body, (hw_descriptor *const descriptor_ptr)) {
    own_hw_compress_descriptor *const this_ptr = (own_hw_compress_descriptor *) descriptor_ptr;

    this_ptr->trusted_fields    = 0u;
    this_ptr->op_code_op_flags  = ADOF_OPCODE(QPL_OPCODE_COMPRESS);
    this_ptr->compression_flags = 0u;

    avx512_qplc_zero_8u(this_ptr->reserved, sizeof(this_ptr->reserved));
}

HW_PATH_IAA_API(void, descriptor_init_deflate_body, (hw_descriptor *const descriptor_ptr,
                                                     uint8_t *const source_ptr,
                                                     const uint32_t source_size,
                                                     uint8_t *const destination_ptr,
                                                     const uint32_t destination_size)) {
    own_hw_compress_descriptor *const this_ptr = (own_hw_compress_descriptor *) descriptor_ptr;

    this_ptr->trusted_fields    = 0u;
    this_ptr->op_code_op_flags  = ADOF_OPCODE(QPL_OPCODE_COMPRESS);
    this_ptr->compression_flags = 0u;

    this_ptr->source_ptr           = source_ptr;
    this_ptr->source_size          = source_size;
    this_ptr->destination_ptr      = destination_ptr;
    this_ptr->max_destination_size = destination_size;

    avx512_qplc_zero_8u(this_ptr->reserved, sizeof(this_ptr->reserved));
}

HW_PATH_IAA_API(void, descriptor_compress_set_aecs, (hw_descriptor *const descriptor_ptr,
                                                     hw_iaa_aecs *const aecs_ptr,
                                                     const hw_iaa_aecs_access_policy access_policy)) {
    own_hw_compress_descriptor *const this_ptr = (own_hw_compress_descriptor *) descriptor_ptr;

    uint32_t read_flag  = (access_policy & hw_aecs_access_read) ? ADOF_READ_SRC2(AD_RDSRC2_AECS) : 0;
    uint32_t write_flag = (access_policy & hw_aecs_access_write) ? ADOF_WRITE_SRC2(AD_WRSRC2_ALWAYS) : 0u;

    uint32_t toggle_aecs_flag = (access_policy & hw_aecs_toggle_rw) ? ADOF_AECS_SEL : 0u;

    this_ptr->op_code_op_flags |= read_flag | write_flag | toggle_aecs_flag;

    bool is_final = !(access_policy & hw_aecs_access_write);

    this_ptr->aecs_ptr  = (uint8_t *) aecs_ptr;
    this_ptr->aecs_size = HW_AECS_COMPRESSION_SIZE;

    if (is_final) {
        this_ptr->compression_flags |= ADCF_FLUSH_OUTPUT;

        // Fix for QPL_FLAG_HUFFMAN_BE in ver 1.0, shall be ommited for future HW versions
        // The workaround: When writing to the AECS compress Huffman table, if V1 and the job is a LAST job,
        // and the job specifies Big-Endian-16 mode: set the Huffman code for LL[256] to be 8 bits of 00.
        // Also, set the compression flag for append EOB at end.
        if (this_ptr->compression_flags & ADCF_COMP_BE) {
            hw_iaa_aecs_compress *const compress_state = (hw_iaa_aecs_compress *const) aecs_ptr;
            // V1 work-around
            compress_state[toggle_aecs_flag].histogram.ll_sym[256] = 8u << 15u;
            // Append EOB at end + Need to apply
            this_ptr->compression_flags |= (this_ptr->compression_flags & ~0xCu) | 0x4u;
        }
    }
}
