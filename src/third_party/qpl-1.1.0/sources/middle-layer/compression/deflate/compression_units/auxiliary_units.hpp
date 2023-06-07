/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

#ifndef QPL_MIDDLE_LAYER_COMPRESSION_COMPRESSION_UNITS_AUXILIARY_UNITS_HPP
#define QPL_MIDDLE_LAYER_COMPRESSION_COMPRESSION_UNITS_AUXILIARY_UNITS_HPP

#include "compression/deflate/streams/sw_deflate_state.hpp"
#include "compression/deflate/utils/compression_defs.hpp"
#include "common/defs.hpp"

namespace qpl::ml::compression {

template<typename stream_t>
auto init_compression(stream_t &stream, compression_state_t &state) noexcept -> qpl_ml_status;

template<typename stream_t>
auto finish_deflate_block(stream_t &stream, compression_state_t &state) noexcept -> qpl_ml_status;

template<typename stream_t>
auto flush_bit_buffer(stream_t &stream, compression_state_t &state) noexcept -> qpl_ml_status;

template<typename stream_t>
auto flush_write_buffer(stream_t &stream, compression_state_t &state) noexcept -> qpl_ml_status;

auto skip_rest_units(deflate_state<execution_path_t::software> &stream, compression_state_t &state) noexcept -> qpl_ml_status;

auto update_checksum(deflate_state<execution_path_t::software> &stream) noexcept -> qpl_ml_status;

auto finish_compression_process(deflate_state<execution_path_t::software> &stream) noexcept -> qpl_ml_status;

auto deflate_body_with_dictionary(deflate_state<execution_path_t::software> &stream,
                              compression_state_t &state) noexcept -> qpl_ml_status;

} // namespace qpl::ml::compression

#endif // QPL_MIDDLE_LAYER_COMPRESSION_COMPRESSION_UNITS_AUXILIARY_UNITS_HPP
