/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

#ifndef QPL_MIDDLE_LAYER_COMPRESSION_COMPRESSION_UNITS_COMPRESSION_UNITS_HPP
#define QPL_MIDDLE_LAYER_COMPRESSION_COMPRESSION_UNITS_COMPRESSION_UNITS_HPP

#include "compression/deflate/streams/sw_deflate_state.hpp"
#include "compression/deflate/utils/compression_defs.hpp"
#include "common/defs.hpp"

namespace qpl::ml::compression {

auto write_header(deflate_state<execution_path_t::software> &stream, compression_state_t &state) noexcept -> qpl_ml_status;

auto slow_deflate_body(deflate_state<execution_path_t::software> &stream, compression_state_t &state) noexcept -> qpl_ml_status;

auto deflate_body(deflate_state<execution_path_t::software> &stream, compression_state_t &state) noexcept -> qpl_ml_status;

auto deflate_finish(deflate_state<execution_path_t::software> &stream, compression_state_t &state) noexcept -> qpl_ml_status;

auto write_end_of_block(deflate_state<execution_path_t::software> &stream, compression_state_t &state) noexcept -> qpl_ml_status;

auto process_by_mini_blocks_body(deflate_state<execution_path_t::software> &stream, compression_state_t &state) noexcept -> qpl_ml_status;

auto build_huffman_table(deflate_state<execution_path_t::software> &stream, compression_state_t &state) noexcept -> qpl_ml_status;

auto preprocess_static_block(deflate_state<execution_path_t::software> &stream, compression_state_t &state) noexcept -> qpl_ml_status;

auto skip_header(deflate_state<execution_path_t::software> &stream, compression_state_t &state) noexcept -> qpl_ml_status;

auto skip_preprocessing(deflate_state<execution_path_t::software> &stream, compression_state_t &state) noexcept -> qpl_ml_status;

} // namespace qpl::ml::compression

#endif // QPL_MIDDLE_LAYER_COMPRESSION_COMPRESSION_UNITS_COMPRESSION_UNITS_HPP
