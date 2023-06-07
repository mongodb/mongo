/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

#ifndef QPL_MIDDLE_LAYER_COMPRESSION_UTILS_HPP
#define QPL_MIDDLE_LAYER_COMPRESSION_UTILS_HPP

#include "compression/deflate/utils/compression_defs.hpp"
#include "compression/huffman_table/inflate_huffman_table.hpp"

namespace qpl::ml::compression {
static inline auto bytes_per_mini_block(const mini_block_size_t mini_block_size) noexcept -> uint32_t {
    return 1u << (mini_block_size + minimal_mini_block_size_power);
}

static inline auto get_complete_mini_blocks_number(const uint32_t source_size,
                                                   const mini_block_size_t mini_block_size) noexcept -> uint32_t {
    const uint32_t power_of_mini_block_size = mini_block_size + minimal_mini_block_size_power;

    return (source_size) >> power_of_mini_block_size;
}

static inline auto reset_inflate_state(isal_inflate_state *state) {
    state->block_state            = ISAL_BLOCK_NEW_HDR;
    state->dict_length            = 0;
    state->bfinal                 = 0;
    state->hist_bits              = 0;
    state->tmp_in_size            = 0;
    state->wrapper_flag           = 0;
    state->read_in                = 0;
    state->read_in_length         = 0;
    state->tmp_out_valid          = 0;
    state->tmp_out_processed      = 0;
    state->crc                    = 0;
    state->crc_flag               = 0;
    state->count                  = 0;
    state->write_overflow_lits    = 0;
    state->write_overflow_len     = 0;
    state->copy_overflow_length   = 0;
    state->copy_overflow_distance = 0;
    state->mini_block_size        = 0;
    state->eob_code_and_len       = 0;
    state->decomp_end_proc        = 0;
}

} // namespace qpl::ml::compression

#endif // QPL_MIDDLE_LAYER_COMPRESSION_UTILS_HPP
