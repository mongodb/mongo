/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

/**
 * @date 8/10/2020
 * @brief
 *
 */

#ifndef QPL_MIDDLE_LAYER_COMPRESSION_DEFLATE_DEFLATE_HPP
#define QPL_MIDDLE_LAYER_COMPRESSION_DEFLATE_DEFLATE_HPP

#include "type_traits"

#include "common/defs.hpp"
#include "compression/compression_defs.hpp"
#include "compression/deflate/utils/compression_defs.hpp"
#include "compression/deflate/utils/compression_traits.hpp"

namespace qpl::ml::compression {

enum class deflate_mode_t {
    deflate_no_headers,
    deflate_default,
};

template <execution_path_t path,
          deflate_mode_t mode,
          class stream_t = deflate_state<path>>
auto deflate(stream_t &stream,
             uint8_t *begin,
             const uint32_t size) noexcept -> compression_operation_result_t;

} // namespace qpl::ml::compression

#endif // QPL_MIDDLE_LAYER_COMPRESSION_DEFLATE_DEFLATE_HPP
