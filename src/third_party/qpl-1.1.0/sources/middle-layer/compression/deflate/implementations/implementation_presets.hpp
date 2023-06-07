/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

#ifndef QPL_MIDDLE_LAYER_COMPRESSION_IMPLEMENTATIONS_IMPLEMENTATION_PRESETS_HPP
#define QPL_MIDDLE_LAYER_COMPRESSION_IMPLEMENTATIONS_IMPLEMENTATION_PRESETS_HPP

#include "compression/deflate/implementations/implementation.hpp"
#include "compression/deflate/utils/compression_defs.hpp"
#include "compression/deflate/streams/sw_deflate_state.hpp"

#include "compression/deflate/compression_units/auxiliary_units.hpp"
#include "compression/deflate/compression_units/icf_units.hpp"
#include "compression/deflate/compression_units/compression_units.hpp"
#include "compression/deflate/compression_units/stored_block_units.hpp"

#include <type_traits>

namespace qpl::ml::compression {

template<compression_level_t level,
         compression_mode_t mode,
         block_type_t block_type>
struct deflate_implementation;

template<compression_mode_t mode>
struct deflate_by_mini_blocks_implementation;

template<compression_mode_t mode>
struct deflate_dictionary_implementation;

template<>
struct deflate_implementation<default_level, dynamic_mode, block_type_t::deflate_block> {
    static constexpr auto instance = implementation<deflate_state<execution_path_t::software>>(
        {
                {compression_state_t::init_compression,          &init_compression},
                {compression_state_t::start_new_block,           &init_new_icf_block},
                {compression_state_t::compression_body,          &deflate_icf_body},
                {compression_state_t::compress_rest_data,        &deflate_icf_finish},
                {compression_state_t::create_icf_header,         &create_icf_block_header},
                {compression_state_t::write_buffered_icf_header, &write_buffered_icf_header},
                {compression_state_t::flush_icf_buffer,          &flush_icf_block},
                {compression_state_t::write_stored_block_header, &write_stored_block_header},
                {compression_state_t::write_stored_block,        &write_stored_block},
                {compression_state_t::flush_bit_buffer,          &flush_bit_buffer},
                {compression_state_t::finish_deflate_block,      &finish_deflate_block}
        });
};

template<>
struct deflate_implementation<high_level, dynamic_mode, block_type_t::deflate_block> {
    static constexpr auto instance = implementation<deflate_state<execution_path_t::software>>(
        {
                {compression_state_t::init_compression,          &init_compression},
                {compression_state_t::start_new_block,           &init_new_icf_block},
                {compression_state_t::compression_body,          &slow_deflate_icf_body},
                {compression_state_t::create_icf_header,         &create_icf_block_header},
                {compression_state_t::write_buffered_icf_header, &write_buffered_icf_header},
                {compression_state_t::flush_icf_buffer,          &flush_icf_block},
                {compression_state_t::write_stored_block_header, &write_stored_block_header},
                {compression_state_t::write_stored_block,        &write_stored_block},
                {compression_state_t::flush_bit_buffer,          &flush_bit_buffer},
                {compression_state_t::finish_deflate_block,      &finish_deflate_block}
        });
};

template<>
struct deflate_implementation<default_level, static_mode, block_type_t::deflate_block> {
    static constexpr auto instance = implementation<deflate_state<execution_path_t::software>>(
        {
                {compression_state_t::init_compression,     &init_compression},
                {compression_state_t::preprocess_new_block, &preprocess_static_block},
                {compression_state_t::start_new_block,      &write_header},
                {compression_state_t::compression_body,     &deflate_body},
                {compression_state_t::compress_rest_data,   &deflate_finish},
                {compression_state_t::write_stored_block,   &recover_and_write_stored_blocks},
                {compression_state_t::flush_bit_buffer,     &flush_bit_buffer},
                {compression_state_t::finish_deflate_block, &finish_deflate_block}
        });
};

template<>
struct deflate_implementation<high_level, static_mode, block_type_t::deflate_block> {
    static constexpr auto instance = implementation<deflate_state<execution_path_t::software>>(
        {
                {compression_state_t::init_compression,     &init_compression},
                {compression_state_t::preprocess_new_block, &preprocess_static_block},
                {compression_state_t::start_new_block,      &write_header},
                {compression_state_t::compression_body,     &slow_deflate_body},
                {compression_state_t::write_stored_block,   &recover_and_write_stored_blocks},
                {compression_state_t::flush_bit_buffer,     &flush_bit_buffer},
                {compression_state_t::finish_deflate_block, &finish_deflate_block}
        });
};

template<>
struct deflate_implementation<default_level, static_mode, block_type_t::mini_block> {
    static constexpr auto instance = implementation<deflate_state<execution_path_t::software>>(
        {
                {compression_state_t::init_compression,     &init_compression},
                {compression_state_t::preprocess_new_block, &skip_preprocessing},
                {compression_state_t::start_new_block,      &skip_header},
                {compression_state_t::compression_body,     &deflate_body},
                {compression_state_t::compress_rest_data,   &deflate_finish},
                {compression_state_t::flush_bit_buffer,     &skip_rest_units},
                {compression_state_t::finish_deflate_block, &skip_rest_units}
        });
};

template<>
struct deflate_implementation<high_level, static_mode, block_type_t::mini_block> {
    static constexpr auto instance = implementation<deflate_state<execution_path_t::software>>(
        {
                {compression_state_t::init_compression,     &init_compression},
                {compression_state_t::preprocess_new_block, &skip_preprocessing},
                {compression_state_t::start_new_block,      &skip_header},
                {compression_state_t::compression_body,     &slow_deflate_body},
                {compression_state_t::flush_bit_buffer,     &skip_rest_units},
                {compression_state_t::finish_deflate_block, &skip_rest_units}
        });
};

template<>
struct deflate_implementation<default_level, dynamic_mode, block_type_t::mini_block> {
    static constexpr auto instance = implementation<deflate_state<execution_path_t::software>>(
        {
                {compression_state_t::init_compression,     &init_compression},
                {compression_state_t::preprocess_new_block, &skip_preprocessing},
                {compression_state_t::start_new_block,      &skip_header},
                {compression_state_t::compression_body,     &deflate_body},
                {compression_state_t::compress_rest_data,   &deflate_finish},
                {compression_state_t::flush_bit_buffer,     &skip_rest_units},
                {compression_state_t::finish_deflate_block, &skip_rest_units}
        });
};

template<>
struct deflate_implementation<high_level, dynamic_mode, block_type_t::mini_block> {
    static constexpr auto instance = implementation<deflate_state<execution_path_t::software>>(
        {
                {compression_state_t::init_compression,     &init_compression},
                {compression_state_t::preprocess_new_block, &skip_preprocessing},
                {compression_state_t::start_new_block,      &skip_header},
                {compression_state_t::compression_body,     &slow_deflate_body},
                {compression_state_t::flush_bit_buffer,     &skip_rest_units},
                {compression_state_t::finish_deflate_block, &skip_rest_units}
        });
};

template<>
struct deflate_by_mini_blocks_implementation<static_mode> {
    static constexpr auto instance = implementation<deflate_state<execution_path_t::software>>(
        {
                {compression_state_t::init_compression,     &init_compression<deflate_state<execution_path_t::software>>},
                {compression_state_t::preprocess_new_block, &preprocess_static_block},
                {compression_state_t::start_new_block,      &write_header},
                {compression_state_t::compression_body,     &process_by_mini_blocks_body},
                {compression_state_t::flush_bit_buffer,     &flush_bit_buffer<deflate_state<execution_path_t::software>>},
                {compression_state_t::finish_deflate_block, &finish_deflate_block<deflate_state<execution_path_t::software>>}
        });
};

template<>
struct deflate_by_mini_blocks_implementation<dynamic_mode> {
    static constexpr auto instance = implementation<deflate_state<execution_path_t::software>>(
        {
                {compression_state_t::init_compression,     &init_compression<deflate_state<execution_path_t::software>>},
                {compression_state_t::preprocess_new_block, &build_huffman_table},
                {compression_state_t::start_new_block,      &write_header},
                {compression_state_t::compression_body,     &process_by_mini_blocks_body},
                {compression_state_t::flush_bit_buffer,     &flush_bit_buffer<deflate_state<execution_path_t::software>>},
                {compression_state_t::finish_deflate_block, &finish_deflate_block<deflate_state<execution_path_t::software>>}
        });
};

template<>
struct deflate_dictionary_implementation<dynamic_mode> {
    static constexpr auto instance = implementation<deflate_state<execution_path_t::software>>(
        {
                {compression_state_t::init_compression,          &init_compression},
                {compression_state_t::start_new_block,           &init_new_icf_block},
                {compression_state_t::compression_body,          &deflate_body_with_dictionary},
                {compression_state_t::compress_rest_data,        &deflate_icf_finish},
                {compression_state_t::create_icf_header,         &create_icf_block_header},
                {compression_state_t::write_buffered_icf_header, &write_buffered_icf_header},
                {compression_state_t::flush_icf_buffer,          &flush_icf_block},
                {compression_state_t::write_stored_block_header, &write_stored_block_header},
                {compression_state_t::write_stored_block,        &write_stored_block},
                {compression_state_t::flush_bit_buffer,          &flush_bit_buffer},
                {compression_state_t::finish_deflate_block,      &finish_deflate_block}
        });
};

template<>
struct deflate_dictionary_implementation<static_mode> {
    static constexpr auto instance = implementation<deflate_state<execution_path_t::software>>(
        {
                {compression_state_t::init_compression,     &init_compression},
                {compression_state_t::preprocess_new_block, &preprocess_static_block},
                {compression_state_t::start_new_block,      &write_header},
                {compression_state_t::compression_body,     &deflate_body_with_dictionary},
                {compression_state_t::compress_rest_data,   &deflate_finish},
                {compression_state_t::write_stored_block_header, &write_stored_block_header},
                {compression_state_t::write_stored_block,        &write_stored_block},
                {compression_state_t::flush_bit_buffer,     &flush_bit_buffer},
                {compression_state_t::finish_deflate_block, &finish_deflate_block}
        });
};

template<>
struct deflate_dictionary_implementation<fixed_mode> {
    static constexpr auto instance = implementation<deflate_state<execution_path_t::software>>(
        {
                {compression_state_t::init_compression,     &init_compression},
                {compression_state_t::preprocess_new_block, &preprocess_static_block},
                {compression_state_t::start_new_block,      &write_header},
                {compression_state_t::compression_body,     &deflate_body_with_dictionary},
                {compression_state_t::compress_rest_data,   &deflate_finish},
                {compression_state_t::write_stored_block_header, &write_stored_block_header},
                {compression_state_t::write_stored_block,        &write_stored_block},
                {compression_state_t::flush_bit_buffer,     &flush_bit_buffer},
                {compression_state_t::finish_deflate_block, &finish_deflate_block}
        });
};


template<>
struct deflate_dictionary_implementation<canned_mode> {
    static constexpr auto instance = implementation<deflate_state<execution_path_t::software>>(
        {
                {compression_state_t::init_compression,     &init_compression},
                {compression_state_t::preprocess_new_block, &preprocess_static_block},
                {compression_state_t::start_new_block,      &write_header},
                {compression_state_t::compression_body,     &deflate_body_with_dictionary},
                {compression_state_t::compress_rest_data,   &deflate_finish},
                {compression_state_t::write_stored_block_header, &write_stored_block_header},
                {compression_state_t::write_stored_block,        &write_stored_block},
                {compression_state_t::flush_bit_buffer,     &flush_bit_buffer},
                {compression_state_t::finish_deflate_block, &finish_deflate_block}
        });
};


} // namespace qpl::ml::compression

#endif // QPL_MIDDLE_LAYER_COMPRESSION_IMPLEMENTATIONS_IMPLEMENTATION_PRESETS_HPP
