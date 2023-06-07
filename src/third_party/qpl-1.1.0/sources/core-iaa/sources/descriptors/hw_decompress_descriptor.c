/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

#include <stdbool.h>
#include "hw_descriptors_api.h"
#include "own_analytic_descriptor.h"
#include "own_compress.h"

#define MAX_BIT_IDX           7u    /**< Maximal index of a bit in the byte */
#define STOP_CHECK_RULE_COUNT 7u    /**< Count of actual stop-check rules */
#define STOP_CHECK_RULE_MASK  0x07  /**< Indexing mask */

static const uint32_t hw_iaa_stop_check_rules[STOP_CHECK_RULE_COUNT + 1u] = {
        ADDF_STOP_ON_EOB | ADDF_CHECK_FOR_EOB | ADDF_SEL_BFINAL_EOB,    //qpl_stop_and_check_for_bfinal_eob = 0,
        0u,                                                             //qpl_dont_stop_or_check = 1,
        ADDF_STOP_ON_EOB | ADDF_CHECK_FOR_EOB,                          //qpl_stop_and_check_for_any_eob
        ADDF_STOP_ON_EOB,                                               //qpl_stop_on_any_eob = 3,
        ADDF_STOP_ON_EOB | ADDF_SEL_BFINAL_EOB,                         //qpl_stop_on_bfinal_eob = 4,
        ADDF_CHECK_FOR_EOB,                                             //qpl_check_for_any_eob = 5,
        ADDF_CHECK_FOR_EOB | ADDF_SEL_BFINAL_EOB,                       //qpl_check_for_bfinal_eob = 6
        ADDF_STOP_ON_EOB | ADDF_CHECK_FOR_EOB | ADDF_SEL_BFINAL_EOB,    //qpl_stop_and_check_for_bfinal_eob = 7
};

HW_PATH_IAA_API(void, descriptor_inflate_set_aecs, (hw_descriptor *const descriptor_ptr,
                                                    hw_iaa_aecs *const aecs_ptr,
                                                    const uint32_t aecs_size,
                                                    const hw_iaa_aecs_access_policy access_policy)) {
    hw_iaa_analytics_descriptor *const this_ptr = (hw_iaa_analytics_descriptor *) descriptor_ptr;

    uint32_t read_flag  = (access_policy & hw_aecs_access_read) ? ADOF_READ_SRC2(AD_RDSRC2_AECS) : 0;
    uint32_t write_flag = (access_policy & hw_aecs_access_write) ?
                          ADOF_WRITE_SRC2(AD_WRSRC2_ALWAYS) :
                          (access_policy & hw_aecs_access_maybe_write) ?
                          ADOF_WRITE_SRC2(AD_WRSRC2_MAYBE) : 0u;

    uint32_t toggle_aecs_flag = (access_policy & hw_aecs_toggle_rw) ? ADOF_AECS_SEL : 0u;

    this_ptr->op_code_op_flags |= read_flag | write_flag | toggle_aecs_flag;

    bool is_final = access_policy & hw_aecs_access_maybe_write;

    this_ptr->src2_ptr  = (uint8_t *) aecs_ptr;
    this_ptr->src2_size = aecs_size;

    if (is_final) {
        this_ptr->decomp_flags |= ADCF_FLUSH_OUTPUT;
    }
}

HW_PATH_IAA_API(void, descriptor_init_inflate, (hw_descriptor *const descriptor_ptr,
                                                hw_iaa_aecs *const aecs_ptr,
                                                const uint32_t aecs_size,
                                                const hw_iaa_aecs_access_policy access_policy)) {
    hw_iaa_analytics_descriptor *const this_ptr = (hw_iaa_analytics_descriptor *) descriptor_ptr;

    this_ptr->trusted_fields   = 0u;
    this_ptr->op_code_op_flags = ADOF_OPCODE(QPL_OPCODE_DECOMPRESS);
    this_ptr->decomp_flags     = ADDF_ENABLE_DECOMP;
    this_ptr->filter_flags     = 0u;

    hw_iaa_descriptor_inflate_set_aecs(descriptor_ptr, aecs_ptr, aecs_size, access_policy);
}

HW_PATH_IAA_API(void, descriptor_init_inflate_header, (hw_descriptor *const descriptor_ptr,
                                                       hw_iaa_aecs *const aecs_ptr,
                                                       const uint8_t ignore_end_bits,
                                                       const hw_iaa_aecs_access_policy access_policy)) {
    hw_iaa_analytics_descriptor *const this_ptr = (hw_iaa_analytics_descriptor *) descriptor_ptr;

    this_ptr->trusted_fields = 0u;

    uint32_t read_flag  = (access_policy & hw_aecs_access_read) ? ADOF_READ_SRC2(AD_RDSRC2_AECS) : 0;
    uint32_t toggle_aecs_flag = (access_policy & hw_aecs_toggle_rw) ? ADOF_AECS_SEL : 0u;

    this_ptr->op_code_op_flags = ADOF_OPCODE(QPL_OPCODE_DECOMPRESS) |
                                 ADOF_WRITE_SRC2(AD_WRSRC2_ALWAYS)  | read_flag | toggle_aecs_flag;

    this_ptr->decomp_flags = ADDF_ENABLE_DECOMP
                             | ADDF_SUPPRESS_OUTPUT
                             | ADDF_IGNORE_END_BITS(MAX_BIT_IDX & ignore_end_bits);

    this_ptr->src2_ptr     = (uint8_t *) aecs_ptr;
    this_ptr->src2_size    = HW_AECS_ANALYTIC_RANDOM_ACCESS_SIZE;


    if (access_policy & hw_aecs_access_read) {
        hw_iaa_aecs_analytic *read_aecs_ptr = (hw_iaa_aecs_analytic *) (this_ptr->src2_ptr +
                                                                        this_ptr->src2_size *
                                                                        (access_policy & hw_aecs_toggle_rw));

        read_aecs_ptr->inflate_options.decompress_state = DEF_STATE_HDR;
    }
}

HW_PATH_IAA_API(void, descriptor_init_inflate_body, (hw_descriptor *const descriptor_ptr,
                                                     hw_iaa_aecs *const aecs_ptr,
                                                     const uint8_t ignore_end_bit)) {
    hw_iaa_analytics_descriptor *const this_ptr = (hw_iaa_analytics_descriptor *) descriptor_ptr;
    hw_iaa_aecs_analytic *this_aecs_ptr = (hw_iaa_aecs_analytic *) aecs_ptr;

    this_ptr->trusted_fields   = 0u;
    this_ptr->op_code_op_flags = ADOF_OPCODE(QPL_OPCODE_DECOMPRESS) | ADOF_READ_SRC2(AD_RDSRC2_AECS);

    this_ptr->decomp_flags = ADDF_ENABLE_DECOMP
                             | ADDF_FLUSH_OUTPUT
                             | ADDF_STOP_ON_EOB
                             | ADDF_IGNORE_END_BITS(MAX_BIT_IDX & ignore_end_bit);

    this_ptr->src2_ptr  = (uint8_t *) this_aecs_ptr;
    this_ptr->src2_size = HW_AECS_ANALYTIC_RANDOM_ACCESS_SIZE;

    this_aecs_ptr->inflate_options.decompress_state = DEF_STATE_LL_TOKEN;
}

HW_PATH_IAA_API(void, descriptor_init_huffman_only_decompress, (hw_descriptor *const descriptor_ptr,
                                                                hw_iaa_aecs *const aecs_ptr,
                                                                const bool huffman_be,
                                                                const uint8_t ignore_end_bits)) {
    hw_iaa_analytics_descriptor *const this_ptr = (hw_iaa_analytics_descriptor *) descriptor_ptr;

    this_ptr->trusted_fields   = 0u;
    this_ptr->op_code_op_flags = ADOF_OPCODE(QPL_OPCODE_DECOMPRESS) | ADOF_READ_SRC2(AD_RDSRC2_AECS);
    this_ptr->decomp_flags = ADDF_ENABLE_DECOMP
                            | ADDF_FLUSH_OUTPUT
                            | (huffman_be ? ADDF_DECOMP_BE : 0u)
                            | ADDF_IGNORE_END_BITS(MAX_BIT_IDX & ignore_end_bits);

    this_ptr->src2_ptr  = (uint8_t *) aecs_ptr;
    this_ptr->src2_size = HW_AECS_ANALYTIC_RANDOM_ACCESS_SIZE;
}

HW_PATH_IAA_API(void, descriptor_set_inflate_stop_check_rule, (hw_descriptor *const descriptor_ptr,
                                                               hw_iaa_decompress_start_stop_rule_t rules,
                                                               bool check_for_eob)) {
    hw_iaa_analytics_descriptor *const this_ptr = (hw_iaa_analytics_descriptor *) descriptor_ptr;

    uint16_t stop_check_mask = ADDF_STOP_ON_EOB | ADDF_SEL_BFINAL_EOB;

    if (check_for_eob) {
        stop_check_mask |= ADDF_CHECK_FOR_EOB;
    }

    this_ptr->decomp_flags |= stop_check_mask & hw_iaa_stop_check_rules[STOP_CHECK_RULE_MASK & rules];
}

HW_PATH_IAA_API(void, descriptor_init_compress_verification, (hw_descriptor * descriptor_ptr)) {
    own_hw_analytic_descriptor *const this_ptr = (own_hw_analytic_descriptor *) descriptor_ptr;

    this_ptr->op_code_op_flags     = ADOF_OPCODE(QPL_OPCODE_DECOMPRESS);
    this_ptr->decompression_flags  = ADDF_ENABLE_DECOMP | ADDF_STOP_ON_EOB | ADDF_SEL_BFINAL_EOB | ADDF_SUPPRESS_OUTPUT;
    this_ptr->filter_flags         = 0u;
    this_ptr->second_source_ptr    = NULL;
    this_ptr->second_source_size   = 0u;
    this_ptr->destination_ptr      = (uint8_t *) this_ptr;
    this_ptr->max_destination_size = 1u;
}

HW_PATH_IAA_API(void, descriptor_compress_verification_write_initial_index, (hw_descriptor *const descriptor_ptr,
                                                                          hw_iaa_aecs_analytic *const aecs_analytic_ptr,
                                                                          uint32_t crc,
                                                                          uint32_t bit_offset)) {
    typedef struct {
        uint32_t bit_offset;
        uint32_t crc;
    } own_index_t;

    own_hw_analytic_descriptor *const this_ptr = (own_hw_analytic_descriptor *) descriptor_ptr;

    own_index_t* initial_index_ptr = (own_index_t *) this_ptr->destination_ptr;

    initial_index_ptr->crc = crc;
    initial_index_ptr->bit_offset = bit_offset;

    this_ptr->destination_ptr += sizeof(own_index_t);
    this_ptr->max_destination_size -= sizeof(own_index_t);

    aecs_analytic_ptr->inflate_options.decompress_state = DEF_STATE_HDR;
    aecs_analytic_ptr->inflate_options.idx_bit_offset   = bit_offset;
}

