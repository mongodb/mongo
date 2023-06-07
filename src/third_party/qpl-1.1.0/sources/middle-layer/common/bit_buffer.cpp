/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

#include <algorithm>

#include "bit_buffer.hpp"
#include "util/util.hpp"

namespace qpl::ml {
void bit_reader::load_buffer(uint8_t number_of_bits) noexcept {
    if (bits_in_buffer_ == 0 &&
        static_cast<uint32_t>(source_end_ptr_ - current_source_ptr_) >= 8 &&
        number_of_bits == 64) {
        buffer_ = * (uint64_t *) current_source_ptr_;
        current_source_ptr_ += 8;
        bits_in_buffer_ = 64;
    } else {
        while (bits_in_buffer_ < 57u && number_of_bits > 0) {
            if (static_cast<uint32_t>(source_end_ptr_ - current_source_ptr_) == 0) {
                is_overflowed_ = true;
                return;
            } else if (static_cast<uint32_t>(source_end_ptr_ - current_source_ptr_) == 1) {
                // If there's only 1 byte left, build mask only for valid bytes and load them
                auto bit_count = (last_bits_offset_ == 0) ? 8 : last_bits_offset_;
                auto bits_to_load = static_cast<uint64_t>(*current_source_ptr_) & util::build_mask<uint64_t>(bit_count);
                buffer_ |= (bits_to_load << bits_in_buffer_);
                bits_in_buffer_ += bit_count;
                current_source_ptr_++;
                number_of_bits -= bit_count;

                if (number_of_bits > 0) {
                    is_overflowed_ = true;
                }

                return;
            } else {
                buffer_ |= (static_cast<uint64_t>(*current_source_ptr_) << bits_in_buffer_);
                bits_in_buffer_ += 8u;
                current_source_ptr_++;
                number_of_bits -= 8u;
            }
        }
    }
}

auto bit_reader::peak_bits(uint8_t number_of_bits) noexcept -> uint16_t {
    if (number_of_bits > bits_in_buffer_) {
        uint32_t bits_still_needed = std::max(8, number_of_bits - bits_in_buffer_);
        bits_still_needed = util::bit_to_byte(bits_still_needed) * byte_bits_size;
        load_buffer(bits_still_needed);
    }

    uint16_t result = 0u;

    auto mask = util::build_mask<uint16_t>(number_of_bits);

    result = static_cast<uint16_t>(buffer_) & mask;

    return result;
}

void bit_reader::shift_bits(uint8_t number_of_bits) noexcept {
    if (bits_in_buffer_ > number_of_bits) {
        buffer_ >>= number_of_bits;
        bits_in_buffer_ -= number_of_bits;
    } else {
        buffer_ = 0;
        bits_in_buffer_ = 0;
    }
}

auto bit_reader::get_total_bytes_read() noexcept -> uint32_t {
    return static_cast<uint32_t>(current_source_ptr_ - source_begin_ptr_);
}

auto bit_reader::get_buffer_bit_count() noexcept -> uint32_t {
    return bits_in_buffer_;
}

void bit_reader::set_last_bits_offset(uint8_t value) noexcept {
    last_bits_offset_ = value;
}

auto bit_reader::is_overflowed() noexcept -> bool {
    return is_overflowed_;
}

auto bit_reader::is_source_end() noexcept -> bool {
    return ((current_source_ptr_ >= source_end_ptr_) && (0 == bits_in_buffer_));
}

}
