/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

#ifndef QPL_MIDDLE_LAYER_COMPRESSION_INFLATE_ISAL_KERNELS_WRAPPERS_HPP
#define QPL_MIDDLE_LAYER_COMPRESSION_INFLATE_ISAL_KERNELS_WRAPPERS_HPP

#include <cstdint>
#include "common/defs.hpp"

typedef struct inflate_state isal_inflate_state;

namespace qpl::ml::compression {
    namespace isal_kernels {
        auto read_deflate_header(isal_inflate_state &inflate_state) noexcept -> qpl_ml_status;
        auto decode_huffman_code_block(isal_inflate_state &inflate_state, uint8_t *start_out_ptr) noexcept -> qpl_ml_status;
        auto check_gzip_checksum(isal_inflate_state &inflate_state) noexcept -> qpl_ml_status;
    }
}
#endif // QPL_MIDDLE_LAYER_COMPRESSION_INFLATE_ISAL_KERNELS_WRAPPERS_HPP
