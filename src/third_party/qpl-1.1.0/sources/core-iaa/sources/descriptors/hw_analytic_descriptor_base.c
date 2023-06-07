/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

#include "hw_descriptors_api.h"
#include "own_analytic_descriptor.h"

#define OWN_FILTER_FLAGS_SET_SOURCE_1_PARSER(x)     (((x) & 3u) << 0u)            /**< @todo */
#define OWN_FILTER_FLAGS_SET_SOURCE_1_BIT_WIDTH(x)  (((x) & 0x1Fu) << 2u)         /**< @todo */
#define OWN_FILTER_FLAGS_SET_OUT_BIT_WIDTH(x)       (((x) & 3u) << 13u)           /**< @todo */
#define OWN_FILTER_FLAGS_GET_SOURCE_1_BIT_WIDTH(x)  ((((x) >> 2u) & 0x1Fu ) + 1)  /**< @todo */

HW_PATH_IAA_API(uint32_t, descriptor_get_source1_bit_width, (const hw_descriptor *const descriptor_ptr)) {
    hw_iaa_analytics_descriptor *const this_ptr = (hw_iaa_analytics_descriptor *) descriptor_ptr;
    return OWN_FILTER_FLAGS_GET_SOURCE_1_BIT_WIDTH(this_ptr->filter_flags);
}

HW_PATH_IAA_API(void, descriptor_analytic_set_filter_input, (hw_descriptor *const descriptor_ptr,
                                                             uint8_t *const source_ptr,
                                                             const uint32_t source_size,
                                                             const uint32_t elements_count,
                                                             const hw_iaa_input_format input_format,
                                                             const uint32_t input_bit_width)) {
    own_hw_analytic_descriptor *const this_ptr = (own_hw_analytic_descriptor*) descriptor_ptr;

    uint32_t input_bit_width_field = (input_format != hw_iaa_input_format_prle) ?
                                     OWN_FILTER_FLAGS_SET_SOURCE_1_BIT_WIDTH(input_bit_width - 1u) :
                                     0u;

    uint32_t input_format_field    = OWN_FILTER_FLAGS_SET_SOURCE_1_PARSER(input_format);

    this_ptr->first_source_ptr   = source_ptr;
    this_ptr->first_source_size  = source_size;
    this_ptr->input_elements     = elements_count;
    this_ptr->filter_flags      |= input_format_field | input_bit_width_field;
}


HW_PATH_IAA_API(void, descriptor_analytic_set_filter_output, (hw_descriptor *const descriptor_ptr,
                                                              uint8_t *const output_ptr,
                                                              const uint32_t output_size,
                                                              const hw_iaa_output_format output_format)) {
    own_hw_analytic_descriptor *const this_ptr = (own_hw_analytic_descriptor*) descriptor_ptr;

    uint32_t output_format_field   = OWN_FILTER_FLAGS_SET_OUT_BIT_WIDTH(output_format);

    uint32_t output_modifiers = (output_format & (hw_iaa_output_modifier_big_endian | hw_iaa_output_modifier_inverse));

    this_ptr->destination_ptr      = output_ptr;
    this_ptr->max_destination_size = output_size;
    this_ptr->filter_flags        |= output_format_field | output_modifiers;
}
