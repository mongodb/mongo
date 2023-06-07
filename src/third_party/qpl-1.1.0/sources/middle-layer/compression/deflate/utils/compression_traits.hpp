/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

#ifndef MIDDLE_LAYER_COMPRESSION_UTILS_COMPRESSION_TRAITS_HPP
#define MIDDLE_LAYER_COMPRESSION_UTILS_COMPRESSION_TRAITS_HPP

#include "compression/deflate/utils/compression_defs.hpp"
#include "compression/deflate/streams/compression_stream.hpp"
#include "common/defs.hpp"

namespace qpl::ml::compression {

template<execution_path_t path>
class deflate_state;

template<execution_path_t path>
class deflate_state_builder;

namespace traits {
template <execution_path_t path>
struct common_type_for_compression_stream_builder {
    using type = deflate_state_builder<execution_path_t::software>;
};

template <>
struct common_type_for_compression_stream_builder<execution_path_t::software> {
    using type = deflate_state_builder<execution_path_t::software>;
};

template <>
struct common_type_for_compression_stream_builder<execution_path_t::hardware> {
    using type = deflate_state_builder<execution_path_t::hardware>;
};

template <execution_path_t path>
struct common_type_for_compression_stream {
    using type = deflate_state<execution_path_t::software>;
};

template <>
struct common_type_for_compression_stream<execution_path_t::software> {
    using type = deflate_state<execution_path_t::software>;
};

template <>
struct common_type_for_compression_stream<execution_path_t::hardware> {
    using type = deflate_state<execution_path_t::hardware>;
};

template <compression_mode_t mode>
constexpr auto need_huffman_table_mode() noexcept -> bool {
    if constexpr(static_mode == mode || canned_mode == mode) {
        return true;
    }

    return false;
}

} // namespace traits

} // namespace qpl::ml::compression

#endif // MIDDLE_LAYER_COMPRESSION_UTILS_COMPRESSION_TRAITS_HPP
