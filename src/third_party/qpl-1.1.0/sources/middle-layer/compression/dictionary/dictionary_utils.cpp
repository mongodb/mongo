/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

/*
 *  Intel® Query Processing Library (Intel® QPL)
 *  Middle Layer API (private C++ API)
 */

#include "dictionary_utils.hpp"
#include "util/memory.hpp"

namespace qpl::ml::compression {

constexpr uint32_t software_hash_table_size[] = {
        16 * qpl_1k,
        16 * qpl_1k,
        64 * qpl_1k,
        64 * qpl_1k,
        256 * qpl_1k,
        32 * qpl_1k,
        32 * qpl_1k,
        32 * qpl_1k,
        32 * qpl_1k,
        32 * qpl_1k
};

constexpr uint32_t hardware_hash_table_size[] = {
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0
};

auto get_dictionary_size(software_compression_level sw_level,
                         hardware_compression_level hw_level,
                         size_t raw_dictionary_size) noexcept -> size_t {
    raw_dictionary_size = std::min(raw_dictionary_size, static_cast<size_t>(max_history_size));
    size_t result_size = raw_dictionary_size + sizeof(qpl_dictionary);

    if (software_compression_level::SW_NONE != sw_level) {
        result_size += software_hash_table_size[static_cast<uint32_t>(sw_level)];
    }

    if (hardware_compression_level::HW_NONE != hw_level) {
        result_size += hardware_hash_table_size[static_cast<uint32_t>(hw_level)];
    }

    return result_size;
}

auto build_dictionary(qpl_dictionary &dictionary,
                      software_compression_level sw_level,
                      hardware_compression_level hw_level,
                      const uint8_t *raw_dict_ptr,
                      size_t raw_dict_size) noexcept -> qpl_ml_status {
    dictionary.sw_hash_table_offset  = 0;
    dictionary.hw_hash_table_offset  = 0;
    dictionary.raw_dictionary_offset = 0;
    dictionary.sw_level              = sw_level;
    dictionary.hw_level              = hw_level;

    uint32_t current_offset = sizeof(qpl_dictionary);

    if (software_compression_level::SW_NONE != sw_level) {
        dictionary.sw_hash_table_offset = current_offset;
        current_offset += software_hash_table_size[static_cast<uint32_t>(sw_level)];
    }

    if (hardware_compression_level::HW_NONE != hw_level) {
        dictionary.hw_hash_table_offset = current_offset;
        current_offset += hardware_hash_table_size[static_cast<uint32_t>(hw_level)];
    }

    dictionary.raw_dictionary_offset = current_offset;

    if (raw_dict_size > max_history_size) {
        // In case when passed dictionary is larger than 4k
        // Build dictionary from last 4k bytes
        raw_dict_ptr += (raw_dict_size - max_history_size);
        raw_dict_size = max_history_size;
    }

    dictionary.raw_dictionary_size = raw_dict_size;

    util::copy(raw_dict_ptr,
               raw_dict_ptr + raw_dict_size,
               reinterpret_cast<uint8_t *>(&dictionary) + current_offset);

    return status_list::ok;
}

auto get_dictionary_data(qpl_dictionary &dictionary) noexcept -> uint8_t * {
    return (reinterpret_cast<uint8_t *>(&dictionary) + dictionary.raw_dictionary_offset);
}
}
