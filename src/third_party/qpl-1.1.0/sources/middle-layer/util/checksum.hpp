/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

/*
 *  Intel® Query Processing Library (Intel® QPL)
 *  Middle Layer API (private C++ API)
 */

#ifndef QPL_SOURCES_MIDDLE_LAYER_UTIL_CHECKSUM_HPP_
#define QPL_SOURCES_MIDDLE_LAYER_UTIL_CHECKSUM_HPP_

#include "crc.h"
#include "dispatcher/dispatcher.hpp"

namespace qpl::ml::util {

constexpr uint32_t most_significant_16_bits  = 0xffff0000;
constexpr uint32_t least_significant_16_bits = 0xffff;
constexpr uint32_t adler32_mod               = 65521u;

struct checksum_accumulator {
    uint32_t crc32   = 0;
    uint32_t adler32 = 0;
};

/**
 * @brief Contains supported CRC calculation types for many operations
 */
enum class crc_type_t {
    none,      /**< Do not calculate checksum */
    crc_32,    /**< To use 0x104c11db7 polynomial for crc calculation */
    crc_32c    /**< To use 0x11edc6f41 polynomial for crc calculation, which is the one used by iSCSI */
};

auto adler32(uint8_t *begin, uint32_t size, uint32_t seed) noexcept -> uint32_t;

template <class input_iterator_t>
inline uint32_t crc32_gzip(const input_iterator_t source_begin,
                           const input_iterator_t source_end,
                           uint32_t seed) noexcept {
    // Check if we've got valid iterators
    static_assert(std::is_same<typename std::iterator_traits<input_iterator_t>::iterator_category,
                               std::random_access_iterator_tag>::value,
                  "Passed input iterator doesn't support random access");
    static_assert((std::is_same<typename std::iterator_traits<input_iterator_t>::value_type, uint8_t>::value) ||
                  (std::is_same<typename std::iterator_traits<input_iterator_t>::value_type, uint16_t>::value) ||
                  (std::is_same<typename std::iterator_traits<input_iterator_t>::value_type, uint32_t>::value),
                  "Passed input iterator value type should be uint8_t, uint16_t or uint32_t");

    auto length = static_cast<uint32_t>(std::distance(source_begin, source_end));

    return crc32_gzip_refl(seed,
                           reinterpret_cast<const uint8_t *>(&*source_begin),
                           length * sizeof(typename std::iterator_traits<input_iterator_t>::value_type));
}

template <class input_iterator_t>
inline uint32_t crc32_iscsi_inv(const input_iterator_t source_begin,
                            const input_iterator_t source_end,
                            uint32_t seed) noexcept {

    // Check if we've got valid iterators
    static_assert(std::is_same<typename std::iterator_traits<input_iterator_t>::iterator_category,
                               std::random_access_iterator_tag>::value,
                  "Passed input iterator doesn't support random access");
    static_assert((std::is_same<typename std::iterator_traits<input_iterator_t>::value_type, uint8_t>::value) ||
                  (std::is_same<typename std::iterator_traits<input_iterator_t>::value_type, uint16_t>::value) ||
                  (std::is_same<typename std::iterator_traits<input_iterator_t>::value_type, uint32_t>::value),
                  "Passed input iterator value type should be uint8_t, uint16_t or uint32_t");

    auto length = static_cast<uint32_t>(std::distance(source_begin, source_end));

    return ~crc32_iscsi(reinterpret_cast<const uint8_t *>(&*source_begin),
                        length * sizeof(typename std::iterator_traits<input_iterator_t>::value_type),
                        ~seed);
}

template <class input_iterator_t>
inline uint32_t xor_checksum(const input_iterator_t source_begin,
                             const input_iterator_t source_end,
                             uint32_t seed) noexcept {
    // Check if we've got valid iterators
    static_assert(std::is_same<typename std::iterator_traits<input_iterator_t>::iterator_category,
                               std::random_access_iterator_tag>::value,
                  "Passed input iterator doesn't support random access");
    static_assert((std::is_same<typename std::iterator_traits<input_iterator_t>::value_type, uint8_t>::value) ||
                  (std::is_same<typename std::iterator_traits<input_iterator_t>::value_type, uint16_t>::value) ||
                  (std::is_same<typename std::iterator_traits<input_iterator_t>::value_type, uint32_t>::value),
                  "Passed input iterator value type should be uint8_t, uint16_t or uint32_t");

    auto xor_kernel = dispatcher::kernels_dispatcher::get_instance().get_xor_checksum_table()[0];
    auto length     = static_cast<uint32_t>(std::distance(source_begin, source_end));

    return xor_kernel(reinterpret_cast<const uint8_t *>(&*source_begin),
                      length * sizeof(typename std::iterator_traits<input_iterator_t>::value_type),
                      seed);
}

}

#endif //QPL_SOURCES_MIDDLE_LAYER_UTIL_CHECKSUM_HPP_
