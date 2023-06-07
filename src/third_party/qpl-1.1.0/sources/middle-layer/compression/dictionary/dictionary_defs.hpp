/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

/*
 *  Intel® Query Processing Library (Intel® QPL)
 *  Middle Layer API (private C++ API)
 */

#ifndef QPL_COMPRESSION_DICTIONARY_DICTIONARY_DEFS_HPP_
#define QPL_COMPRESSION_DICTIONARY_DICTIONARY_DEFS_HPP_

#include <cstdint>
#include <cstddef>

constexpr int dict_none = -1;

enum class software_compression_level {
    SW_NONE = dict_none,
    LEVEL_0 = 0,
    LEVEL_1 = 1,
    LEVEL_2 = 2,
    LEVEL_3 = 3,
    LEVEL_4 = 4,
    LEVEL_9 = 9
};

enum class hardware_compression_level {
    HW_NONE = dict_none,
    SMALL   = 0,
    LARGE   = 1
};

struct qpl_dictionary {
    software_compression_level sw_level;
    hardware_compression_level hw_level;
    size_t                     raw_dictionary_size;
    uint32_t                   dictionary_id;
    uint32_t                   sw_hash_table_offset;
    uint32_t                   hw_hash_table_offset;
    uint32_t                   raw_dictionary_offset;
};

// namespace qpl::ml::compression {
// }

#endif // QPL_COMPRESSION_DICTIONARY_DICTIONARY_DEFS_HPP_
