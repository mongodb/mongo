/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

#include "hw_descriptors_api.h"
#include "own_analytic_descriptor.h"

#define OWN_5_BIT_MASK          0x1fu /**< Mask for 5-bit integer */

#define OWN_FILTER_FLAGS_SET_SOURCE_2_BIT_WIDTH(x)  (((x) & 0x1Fu) << 7u)   /**< @todo */
#define OWN_FILTER_FLAGS_SET_SOURCE_2_BE            (1u << 12u)             /**< @todo */
#define OWN_FILTER_FLAGS_SET_DROP_LOW_BITS(x)       (((x) & 0x1Fu) << 17u)  /**< @todo */
#define OWN_FILTER_FLAGS_SET_DROP_HIGH_BITS(x)      (((x) & 0x1Fu) << 22u)  /**< @todo */

static inline void own_hw_descriptor_single_source_filter_set_second_source(hw_descriptor *const descriptor_ptr,
                                                                            hw_iaa_aecs_analytic *const filter_config_ptr) {
    own_hw_analytic_descriptor *const this_ptr = (own_hw_analytic_descriptor*) descriptor_ptr;

    this_ptr->second_source_ptr  = (uint8_t *) filter_config_ptr;
    this_ptr->second_source_size = HW_AECS_ANALYTIC_FILTER_ONLY_SIZE;

    this_ptr->op_code_op_flags |= ADOF_READ_SRC2(AD_RDSRC2_AECS);
}

static inline void own_hw_descriptor_double_source_filter_set_second_source(hw_descriptor *const descriptor_ptr,
                                                                            uint8_t *const second_source_ptr,
                                                                            const uint32_t second_source_size,
                                                                            bool is_big_endian) {
    own_hw_analytic_descriptor *const this_ptr = (own_hw_analytic_descriptor*) descriptor_ptr;

    this_ptr->op_code_op_flags  |= ADOF_READ_SRC2(AD_RDSRC2_FF_INPUT) | ADOF_WRITE_SRC2(AD_WRSRC2_NEVER);
    this_ptr->second_source_ptr  = second_source_ptr;
    this_ptr->second_source_size = second_source_size;

    if (is_big_endian) {
        this_ptr->filter_flags |= OWN_FILTER_FLAGS_SET_SOURCE_2_BE;
    }
}

HW_PATH_IAA_API(void, descriptor_analytic_set_scan_operation, (hw_descriptor *const descriptor_ptr,
                                                               const uint32_t low_border,
                                                               const uint32_t high_border,
                                                               hw_iaa_aecs_analytic *const filter_config_ptr)) {
    own_hw_analytic_descriptor *const this_ptr = (own_hw_analytic_descriptor *) descriptor_ptr;
    this_ptr->op_code_op_flags |= ADOF_OPCODE(QPL_OPCODE_SCAN);
    filter_config_ptr->filtering_options.filter_low  = low_border;
    filter_config_ptr->filtering_options.filter_high = high_border;

    own_hw_descriptor_single_source_filter_set_second_source((hw_descriptor *) this_ptr, filter_config_ptr);
}


HW_PATH_IAA_API(void, descriptor_analytic_set_extract_operation, (hw_descriptor *const descriptor_ptr,
                                                                  const uint32_t first_element_index,
                                                                  const uint32_t last_element_index,
                                                                  hw_iaa_aecs_analytic *const filter_config_ptr)) {
    own_hw_analytic_descriptor *const this_ptr = (own_hw_analytic_descriptor *) descriptor_ptr;
    this_ptr->op_code_op_flags |= ADOF_OPCODE(QPL_OPCODE_EXTRACT);

    filter_config_ptr->filtering_options.filter_low     = first_element_index;
    filter_config_ptr->filtering_options.filter_high    = last_element_index;

    filter_config_ptr->filtering_options.crc          = 0u;
    filter_config_ptr->filtering_options.xor_checksum = 0u;

    own_hw_descriptor_single_source_filter_set_second_source((hw_descriptor *) this_ptr, filter_config_ptr);
}

HW_PATH_IAA_API(void, descriptor_analytic_set_select_operation, (hw_descriptor *const descriptor_ptr,
                                                                 uint8_t *const mask_ptr,
                                                                 const uint32_t mask_size,
                                                                 const bool is_mask_big_endian)) {
    own_hw_analytic_descriptor *const this_ptr = (own_hw_analytic_descriptor *) descriptor_ptr;
    this_ptr->op_code_op_flags |= ADOF_OPCODE(QPL_OPCODE_SELECT);

    this_ptr->filter_flags |= OWN_FILTER_FLAGS_SET_SOURCE_2_BIT_WIDTH(0u);

    own_hw_descriptor_double_source_filter_set_second_source((hw_descriptor *) this_ptr, mask_ptr, mask_size, is_mask_big_endian);
}

HW_PATH_IAA_API(void, descriptor_analytic_set_expand_operation, (hw_descriptor *const descriptor_ptr,
                                                                 uint8_t *const mask_ptr,
                                                                 const uint32_t mask_size,
                                                                 const bool is_mask_big_endian)) {
    own_hw_analytic_descriptor *const this_ptr = (own_hw_analytic_descriptor *) descriptor_ptr;
    this_ptr->op_code_op_flags |= ADOF_OPCODE(QPL_OPCODE_EXPAND);

    this_ptr->filter_flags |= OWN_FILTER_FLAGS_SET_SOURCE_2_BIT_WIDTH(0u);

    own_hw_descriptor_double_source_filter_set_second_source((hw_descriptor *) this_ptr,
                                                             mask_ptr,
                                                             mask_size,
                                                             is_mask_big_endian);
}

