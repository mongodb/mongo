/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

#include "compression/deflate/streams/sw_deflate_state.hpp"
#include "compression/deflate/utils/compression_defs.hpp"
#include "common/defs.hpp"

#ifndef QPL_MIDDLE_LAYER_COMPRESSION_COMPRESSION_UNITS_STORED_BLOCK_UNITS_HPP
#define QPL_MIDDLE_LAYER_COMPRESSION_COMPRESSION_UNITS_STORED_BLOCK_UNITS_HPP


namespace qpl::ml::compression {

auto write_stored_blocks(uint8_t *source_ptr,
                         uint32_t source_size,
                         uint8_t *output_ptr,
                         uint32_t output_max_size,
                         uint32_t start_bit_offset,
                         bool is_final) noexcept -> uint32_t;

auto write_stored_block_header(deflate_state<execution_path_t::software> &stream,
                               compression_state_t &state) noexcept -> qpl_ml_status;

auto write_stored_block(deflate_state<execution_path_t::software> &stream,
                        compression_state_t &state) noexcept -> qpl_ml_status;

auto write_stored_block(deflate_state<execution_path_t::hardware> &state) noexcept -> compression_operation_result_t;

auto recover_and_write_stored_blocks(deflate_state<execution_path_t::software> &stream,
                                     compression_state_t &state) noexcept -> qpl_ml_status;

auto calculate_size_needed(uint32_t input_data_size, uint32_t bit_size) noexcept -> uint32_t;

static inline auto get_stored_blocks_size(uint32_t source_size) noexcept {
    const uint32_t number_of_stored_blocks = (source_size + stored_block_max_length - 1) / stored_block_max_length;
    return source_size + number_of_stored_blocks * stored_block_header_length;
}

} // namespace qpl::ml::compression

#endif // QPL_MIDDLE_LAYER_COMPRESSION_COMPRESSION_UNITS_STORED_BLOCK_UNITS_HPP
