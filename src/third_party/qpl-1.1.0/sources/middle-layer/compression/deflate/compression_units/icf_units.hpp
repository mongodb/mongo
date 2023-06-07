/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

#include "compression/deflate/streams/sw_deflate_state.hpp"
#include "compression/deflate/utils/compression_defs.hpp"
#include "common/defs.hpp"

#ifndef QPL_MIDDLE_LAYER_COMPRESSION_COMPRESSION_UNITS_ICF_UNITS_HPP
#define QPL_MIDDLE_LAYER_COMPRESSION_COMPRESSION_UNITS_ICF_UNITS_HPP


namespace qpl::ml::compression {

auto write_buffered_icf_header(deflate_state<execution_path_t::software> &stream, compression_state_t &state) noexcept -> qpl_ml_status;

auto create_icf_block_header(deflate_state<execution_path_t::software> &stream, compression_state_t &state) noexcept -> qpl_ml_status;

auto flush_icf_block(deflate_state<execution_path_t::software> &stream, compression_state_t &state) noexcept -> qpl_ml_status;

auto init_new_icf_block(deflate_state<execution_path_t::software> &stream, compression_state_t &state) noexcept -> qpl_ml_status;

auto deflate_icf_finish(deflate_state<execution_path_t::software> &stream, compression_state_t &state) noexcept -> qpl_ml_status;

auto deflate_icf_body(deflate_state<execution_path_t::software> &stream, compression_state_t &state) noexcept -> qpl_ml_status;

auto slow_deflate_icf_body(deflate_state<execution_path_t::software> &stream, compression_state_t &state) noexcept -> qpl_ml_status;

} // namespace qpl::ml::compression

#endif // QPL_MIDDLE_LAYER_COMPRESSION_COMPRESSION_UNITS_ICF_UNITS_HPP
