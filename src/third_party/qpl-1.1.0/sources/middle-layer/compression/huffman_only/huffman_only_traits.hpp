/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

/*
 *  Intel® Query Processing Library (Intel® QPL)
 *  Middle Layer API (private C++ API)
 */

#ifndef QPL_SOURCES_MIDDLE_LAYER_COMPRESSION_HUFFMAN_ONLY_TRAITS_HPP_
#define QPL_SOURCES_MIDDLE_LAYER_COMPRESSION_HUFFMAN_ONLY_TRAITS_HPP_

namespace qpl::ml::compression {

template <execution_path_t path>
class huffman_only_state;

namespace traits {
    template <execution_path_t path>
    struct common_type_for_huffman_only_stream {
        using type = huffman_only_state<path>;
    };

    template <>
    struct common_type_for_huffman_only_stream<execution_path_t::software> {
        using type = huffman_only_state<execution_path_t::software>;
    };

    template <>
    struct common_type_for_huffman_only_stream<execution_path_t::hardware> {
        using type = huffman_only_state<execution_path_t::hardware>;
    };
}
}

#endif //QPL_SOURCES_MIDDLE_LAYER_COMPRESSION_HUFFMAN_ONLY_TRAITS_HPP_
