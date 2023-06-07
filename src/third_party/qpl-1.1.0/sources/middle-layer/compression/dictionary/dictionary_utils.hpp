/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

/*
 *  Intel® Query Processing Library (Intel® QPL)
 *  Middle Layer API (private C++ API)
 */

#ifndef QPL_COMPRESSION_DICTIONARY_DICTIONARY_UTILS_HPP_
#define QPL_COMPRESSION_DICTIONARY_DICTIONARY_UTILS_HPP_

#include "dictionary_defs.hpp"
#include "common/defs.hpp"

namespace qpl::ml::compression {
auto get_dictionary_size(software_compression_level sw_level,
                         hardware_compression_level hw_level,
                         size_t raw_dictionary_size) noexcept -> size_t;

auto build_dictionary(qpl_dictionary &dictionary,
                      software_compression_level sw_level,
                      hardware_compression_level hw_level,
                      const uint8_t *raw_dict_ptr,
                      size_t raw_dict_size) noexcept -> qpl_ml_status;

auto get_dictionary_data(qpl_dictionary &dictionary) noexcept -> uint8_t *;
}

#endif // QPL_COMPRESSION_DICTIONARY_DICTIONARY_UTILS_HPP_
