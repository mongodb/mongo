/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

/*
 *  Intel® Query Processing Library (Intel® QPL)
 *  Middle Layer API (private C++ API)
 */

#ifndef QPL_COMPRESSION_DEFS_HPP_
#define QPL_COMPRESSION_DEFS_HPP_

#include "common/defs.hpp"

namespace qpl::ml::compression {

enum class compression_algorithm_e : uint8_t {
    deflate,
    canned,
    huffman_only
};

struct decompression_operation_result_t {
    uint32_t    status_code_     = 0u;
    uint32_t    output_bytes_    = 0u;
    uint32_t    completed_bytes_ = 0u;
    checksums_t checksums_       = {};
};

struct compression_operation_result_t {
    uint32_t    status_code_     = 0u;
    uint32_t    output_bytes_    = 0u;
    uint32_t    completed_bytes_ = 0u;
    uint32_t    indexes_written_ = 0u;
    uint32_t    last_bit_offset  = 0u;
    checksums_t checksums_       = {};
};

struct verification_pass_result_t {
    uint32_t    status_code_     = 0u;
    uint32_t    indexes_written_ = 0u;
    uint32_t    completed_bytes_ = 0u;
    checksums_t checksums_       = {};
};

}

#endif //QPL_COMPRESSION_DEFS_HPP_
