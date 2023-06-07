/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

/*
 *  Intel® Query Processing Library (Intel® QPL)
 *  Middle Layer API (private C++ API)
 */

#ifndef QPL_ML_COMPRESSION_INFLATE_HUFFMAN_ONLY_DECOMPRESSION_HPP
#define QPL_ML_COMPRESSION_INFLATE_HUFFMAN_ONLY_DECOMPRESSION_HPP

#include <array>

#include "compression/inflate/inflate_defs.hpp"
#include "common/bit_buffer.hpp"
#include "common/defs.hpp"
#include "compression/compression_defs.hpp"
#include "compression/huffman_only/huffman_only_decompression_state.hpp"
#include "compression/huffman_only/huffman_only_traits.hpp"

namespace qpl::ml::compression {

constexpr uint32_t huffman_only_number_of_literals = 256;
constexpr uint32_t huffman_code_bit_length         = 15;

template <execution_path_t path,
        class stream_t = traits::common_type_for_huffman_only_stream<path>>
auto compress_huffman_only(uint8_t *begin,
                           const uint32_t size,
                           stream_t &stream) noexcept -> compression_operation_result_t;

template <execution_path_t path>
auto decompress_huffman_only(huffman_only_decompression_state<path> &decompression_state,
                             decompression_huffman_table &decompression_table) noexcept -> decompression_operation_result_t;

template <execution_path_t path>
auto verify_huffman_only(huffman_only_decompression_state<path> &state,
                         decompression_huffman_table &decompression_table,
                         uint32_t required_crc) noexcept -> qpl_ml_status;
}
#endif // QPL_ML_COMPRESSION_INFLATE_HUFFMAN_ONLY_DECOMPRESSION_HPP
