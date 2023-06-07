/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

#ifndef ZERO_DEFS_HPP_
#define ZERO_DEFS_HPP_

#include <cstdint>

#include "common/defs.hpp"

namespace qpl::ml::compression {

enum class zero_operation_type {
    compress,
    decompress
};

/**
 * @brief Contains supported input formats for zero operations
 */
enum class zero_input_format_t {
    word_16_bit,    /**< For words series, where the word length is 16-bits */
    word_32_bit     /**< For words series, where the word length is 32-bits */
};

/**
 * @brief Contains supported CRC calculation types for many operations
 */
enum class crc_type_t {
    none,      /**< Do not calculate checksum */
    crc_32,    /**< To use 0x104c11db7 polynomial for crc calculation */
    crc_32c    /**< To use 0x11edc6f41 polynomial for crc calculation, which is the one used by iSCSI */
};

struct zero_operation_result_t {
    uint32_t     status_code_  = 0u;
    uint32_t     output_bytes_ = 0u;
    aggregates_t aggregates_;
    checksums_t  checksums_;
};

} // namespace qpl::ml::compression

#endif // ZERO_DEFS_HPP_
