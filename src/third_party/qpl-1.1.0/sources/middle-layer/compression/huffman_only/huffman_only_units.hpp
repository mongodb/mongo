/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

#ifndef QPL_MIDDLE_LAYER_COMPRESSION_COMPRESSION_UNITS_HUFFMAN_ONLY_UNITS_HPP
#define QPL_MIDDLE_LAYER_COMPRESSION_COMPRESSION_UNITS_HUFFMAN_ONLY_UNITS_HPP

#include "compression/huffman_only/huffman_only_compression_state.hpp"

#include "compression/deflate/utils/compression_defs.hpp"
#include "common/defs.hpp"

namespace qpl::ml::compression {

auto huffman_only_compress_block(huffman_only_state<execution_path_t::software> &stream,
                                 compression_state_t &state) noexcept -> qpl_ml_status;

auto huffman_only_finalize(huffman_only_state<execution_path_t::software> &stream,
                           compression_state_t &state) noexcept -> qpl_ml_status;

auto huffman_only_create_huffman_table(huffman_only_state<execution_path_t::software> &stream,
                                       compression_state_t &state) noexcept -> qpl_ml_status;

// @todo it is not relative for huffman_only only
auto convert_output_to_big_endian(huffman_only_state<execution_path_t::software> &stream,
                                  compression_state_t &state) noexcept -> qpl_ml_status;

} // namespace qpl::ml::compression

#endif // QPL_MIDDLE_LAYER_COMPRESSION_COMPRESSION_UNITS_HUFFMAN_ONLY_UNITS_HPP
