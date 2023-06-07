/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

#ifndef QPL_MIDDLE_LAYER_COMPRESSION_IMPLEMENTATIONS_DEFLATE_IMPLEMENTATION_HPP
#define QPL_MIDDLE_LAYER_COMPRESSION_IMPLEMENTATIONS_DEFLATE_IMPLEMENTATION_HPP

#include "compression/deflate/implementations/implementation.hpp"
#include "compression/deflate/utils/compression_defs.hpp"
#include "compression/deflate/streams/sw_deflate_state.hpp"

#include "compression/deflate/compression_units/auxiliary_units.hpp"
#include "compression/deflate/compression_units/icf_units.hpp"
#include "compression/deflate/compression_units/compression_units.hpp"
#include "compression/deflate/compression_units/stored_block_units.hpp"

#include "compression/deflate/implementations/implementation_presets.hpp"

namespace qpl::ml::compression {

template<block_type_t block_type>
constexpr auto build_deflate_implementation(compression_level_t level,
                                            compression_mode_t mode,
                                            dictionary_support_t dictionary_support)
                                            -> implementation<deflate_state<execution_path_t::software>> {
    if (dictionary_support == dictionary_support_t::enabled) {
        if (mode == dynamic_mode)
            return deflate_dictionary_implementation<dynamic_mode>::instance;
        if (mode == fixed_mode)
            return deflate_dictionary_implementation<fixed_mode>::instance;
        if (mode == canned_mode)
            return deflate_dictionary_implementation<canned_mode>::instance;
        return deflate_dictionary_implementation<static_mode>::instance;
    }
    
    if (mode == dynamic_mode) {
        return level == default_level
               ? deflate_implementation<default_level, dynamic_mode, block_type>::instance
               : deflate_implementation<high_level, dynamic_mode, block_type>::instance;
    }

    return level == default_level
        ? deflate_implementation<default_level, static_mode, block_type>::instance
        : deflate_implementation<high_level, static_mode, block_type>::instance;
}

constexpr auto build_deflate_by_mini_blocks_implementation(compression_mode_t mode) ->
                                                           implementation<deflate_state<execution_path_t::software>> {
    if (mode == dynamic_mode) {
        return deflate_by_mini_blocks_implementation<dynamic_mode>::instance;
    }
    return deflate_by_mini_blocks_implementation<static_mode>::instance;
}

template<block_type_t block_type = block_type_t::deflate_block>
constexpr auto build_implementation(compression_level_t level,
                                    compression_mode_t mode,
                                    mini_blocks_support_t mini_blocks_supprt,
                                    dictionary_support_t dictionary_support)
        -> implementation<deflate_state<execution_path_t::software>> {
    if (mini_blocks_supprt == mini_blocks_support_t::disabled) {
        return build_deflate_implementation<block_type>(level, mode, dictionary_support);
    }

    return build_deflate_by_mini_blocks_implementation(mode);
}

} // namespace qpl::ml::compression

#endif // QPL_MIDDLE_LAYER_COMPRESSION_IMPLEMENTATIONS_DEFLATE_IMPLEMENTATION_HPP
