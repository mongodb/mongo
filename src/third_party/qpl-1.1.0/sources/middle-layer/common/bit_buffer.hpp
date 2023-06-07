/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

#ifndef QPL_ML_COMMON_BIT_BUFFER_HPP
#define QPL_ML_COMMON_BIT_BUFFER_HPP

#include <cstdint>

namespace qpl::ml {
class bit_reader {
public:
    template<typename input_iterator>
    bit_reader(const input_iterator source_begin,
               const input_iterator source_end) :
        source_begin_ptr_(source_begin),
        current_source_ptr_(source_begin),
        source_end_ptr_(source_end) {
        buffer_ = 0u;
        bits_in_buffer_ = 0u;
        last_bits_offset_ = 0u;
        is_overflowed_ = false;
    }

    template<typename input_iterator>
    void set_source(const input_iterator source_begin,
                    const input_iterator source_end) noexcept {
        source_begin_ptr_ = source_begin;
        current_source_ptr_ = source_begin;
        source_end_ptr_ = source_end;
        is_overflowed_ = false;
    }

    void load_buffer(uint8_t number_of_bits = 64) noexcept;

    auto peak_bits(uint8_t number_of_bits) noexcept -> uint16_t;

    void shift_bits(uint8_t nubmer_of_bits) noexcept;

    auto get_total_bytes_read() noexcept -> uint32_t;

    auto get_buffer_bit_count() noexcept -> uint32_t;

    void set_last_bits_offset(uint8_t value) noexcept;

    auto is_overflowed() noexcept -> bool;

    auto is_source_end() noexcept -> bool;

private:
    const uint8_t *source_begin_ptr_;
    const uint8_t *current_source_ptr_;
    const uint8_t *source_end_ptr_;

    uint8_t last_bits_offset_;
    uint64_t buffer_;
    uint8_t  bits_in_buffer_;

    bool is_overflowed_;
};
}

#endif // QPL_ML_COMMON_BIT_BUFFER_HPP
