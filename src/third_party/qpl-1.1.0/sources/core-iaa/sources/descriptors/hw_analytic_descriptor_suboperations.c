/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

#include <stdbool.h>
#include "hw_descriptors_api.h"
#include "own_hw_definitions.h"
#include "own_analytic_descriptor.h"

#define OWN_MAX_BIT_IDX 7u /**< @todo */

HW_PATH_IAA_API(void, descriptor_analytic_enable_decompress, (hw_descriptor *const descriptor_ptr,
                                                              bool is_big_endian_compressed_stream,
                                                              uint32_t ignore_last_bits)) {
    own_hw_analytic_descriptor *const this_ptr = (own_hw_analytic_descriptor *) descriptor_ptr;

    this_ptr->decompression_flags |= ADDF_ENABLE_DECOMP
                                     | ADDF_FLUSH_OUTPUT
                                     | ADDF_IGNORE_END_BITS(ignore_last_bits & OWN_MAX_BIT_IDX)
                                     | (is_big_endian_compressed_stream ? ADDF_DECOMP_BE : 0u);
}
