/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

/*
 *  Intel® Query Processing Library (Intel® QPL)
 *  Middle Layer API (private C++ API)
 */

#ifndef QPL_SOURCES_MIDDLE_LAYER_COMPRESSION_HUFFMAN_ONLY_HUFFMAN_ONLY_IMPLEMENTATION_HPP_
#define QPL_SOURCES_MIDDLE_LAYER_COMPRESSION_HUFFMAN_ONLY_HUFFMAN_ONLY_IMPLEMENTATION_HPP_

#include "compression/deflate/utils/compression_defs.hpp"
#include "compression/deflate/implementations/implementation.hpp"
#include "compression/huffman_only/huffman_only_compression_state.hpp"

#include "compression/deflate/compression_units/auxiliary_units.hpp"
#include "compression/huffman_only/huffman_only_units.hpp"

namespace qpl::ml::compression {

constexpr auto build_huffman_only_implementation() {
    using state_t = huffman_only_state<execution_path_t::software>;
    return implementation<state_t>(
            {
                    {compression_state_t::init_compression,     &init_compression<state_t>},
                    {compression_state_t::start_new_block,      &huffman_only_create_huffman_table},
                    {compression_state_t::compression_body,     &huffman_only_compress_block},
                    {compression_state_t::compress_rest_data,   &huffman_only_finalize},
                    {compression_state_t::flush_write_buffer,   &flush_write_buffer<state_t>},
                    {compression_state_t::flush_bit_buffer,     &flush_bit_buffer<state_t>},
                    {compression_state_t::finish_deflate_block, &finish_deflate_block<state_t>}
            });
}
}

#endif //QPL_SOURCES_MIDDLE_LAYER_COMPRESSION_HUFFMAN_ONLY_HUFFMAN_ONLY_IMPLEMENTATION_HPP_
