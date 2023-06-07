/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

#ifndef MIDDLE_LAYER_UTIL_MEMORY_HPP
#define MIDDLE_LAYER_UTIL_MEMORY_HPP

#include "dispatcher/dispatcher.hpp"
#include "cstddef"

namespace qpl::ml::util {

template <class input_iterator_t,
          class output_iterator_t>
inline void copy(const input_iterator_t source_begin,
                 const input_iterator_t source_end,
                 output_iterator_t destination) noexcept {
    constexpr auto default_bit_width = 8u;

    // Check if we've got valid iterators
    static_assert(std::is_same<typename std::iterator_traits<input_iterator_t>::iterator_category,
                               std::random_access_iterator_tag>::value,
                  "Passed input iterator doesn't support random access");
    static_assert((std::is_same<typename std::iterator_traits<input_iterator_t>::value_type, uint8_t>::value) ||
                  (std::is_same<typename std::iterator_traits<input_iterator_t>::value_type, uint16_t>::value) ||
                  (std::is_same<typename std::iterator_traits<input_iterator_t>::value_type, uint32_t>::value),
                  "Passed input iterator value type should be uint8_t, uint16_t or uint32_t");

    static_assert(std::is_same<typename std::iterator_traits<output_iterator_t>::iterator_category,
                               std::random_access_iterator_tag>::value,
                  "Passed output iterator doesn't support random access");
    static_assert((std::is_same<typename std::iterator_traits<output_iterator_t>::value_type, uint8_t>::value) ||
                  (std::is_same<typename std::iterator_traits<output_iterator_t>::value_type, uint16_t>::value) ||
                  (std::is_same<typename std::iterator_traits<output_iterator_t>::value_type, uint32_t>::value),
                  "Passed output iterator value type should be uint8_t, uint16_t or uint32_t");

    auto copy_index  = dispatcher::get_memory_copy_index(default_bit_width);
    auto copy_kernel = dispatcher::kernels_dispatcher::get_instance().get_memory_copy_table()[copy_index];

    auto length = static_cast<uint32_t>(std::distance(source_begin, source_end));

    copy_kernel(reinterpret_cast<const uint8_t *>(&*source_begin),
                reinterpret_cast<uint8_t *>(&*destination),
                length * sizeof(typename std::iterator_traits<input_iterator_t>::value_type));
}

template <class input_iterator_t,
          class output_iterator_t>
inline void move(const input_iterator_t source_begin,
                 const input_iterator_t source_end,
                 output_iterator_t destination) noexcept {
    constexpr auto default_bit_width = 8u;

    // Check if we've got valid iterators
    static_assert(std::is_same<typename std::iterator_traits<input_iterator_t>::iterator_category,
                               std::random_access_iterator_tag>::value,
                  "Passed input iterator doesn't support random access");
    static_assert((std::is_same<typename std::iterator_traits<input_iterator_t>::value_type, uint8_t>::value) ||
                  (std::is_same<typename std::iterator_traits<input_iterator_t>::value_type, uint16_t>::value) ||
                  (std::is_same<typename std::iterator_traits<input_iterator_t>::value_type, uint32_t>::value),
                  "Passed input iterator value type should be uint8_t, uint16_t or uint32_t");

    static_assert(std::is_same<typename std::iterator_traits<output_iterator_t>::iterator_category,
                               std::random_access_iterator_tag>::value,
                  "Passed output iterator doesn't support random access");
    static_assert((std::is_same<typename std::iterator_traits<output_iterator_t>::value_type, uint8_t>::value) ||
                  (std::is_same<typename std::iterator_traits<output_iterator_t>::value_type, uint16_t>::value) ||
                  (std::is_same<typename std::iterator_traits<output_iterator_t>::value_type, uint32_t>::value),
                  "Passed output iterator value type should be uint8_t, uint16_t or uint32_t");

    auto move_index  = dispatcher::get_memory_copy_index(default_bit_width);
    auto move_kernel = qpl::ml::dispatcher::kernels_dispatcher::get_instance().get_move_table()[move_index];

    auto length = static_cast<uint32_t>(std::distance(source_begin, source_end));

    move_kernel(reinterpret_cast<const uint8_t *>(&*source_begin),
                reinterpret_cast<uint8_t *>(&*destination),
                length * sizeof(typename std::iterator_traits<input_iterator_t>::value_type));
}

template<class output_iterator_t>
inline void set_zeros(output_iterator_t destination_begin,
                      size_t destination_bytes_length) noexcept {
    const auto set_zero_kernel = dispatcher::kernels_dispatcher::get_instance().get_zero_table()[0u];
    set_zero_kernel(reinterpret_cast<uint8_t *>(destination_begin), static_cast<uint32_t>(destination_bytes_length));
}

}

#endif // MIDDLE_LAYER_UTIL_MEMORY_HPP
