/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

#ifndef MIDDLE_LAYER_LIMITED_BUFFER_HPP
#define MIDDLE_LAYER_LIMITED_BUFFER_HPP

#include "buffer.hpp"
#include "util/util.hpp"

namespace qpl::ml {

class limited_buffer_t : public buffer_t {
public:
    limited_buffer_t() = delete;

    template <class iterator_t>
    limited_buffer_t(iterator_t buffer_begin,
                     iterator_t buffer_end,
                     const uint8_t bit_width)
            : buffer_t(buffer_begin, buffer_end) {
        max_elements_in_buffer_ = size() / util::bit_to_byte(util::bit_width_to_bits(bit_width));
        bit_width_              = bit_width;
    }

    [[nodiscard]] auto max_elements_count() const noexcept -> uint32_t;

    [[nodiscard]] auto data() const noexcept -> uint8_t * override;

    inline void set_byte_shift(uint32_t byte_shift) {
        byte_shift_ = byte_shift;
    }

private:
    uint8_t  bit_width_              = byte_bits_size;
    uint32_t max_elements_in_buffer_ = 0;
    uint32_t byte_shift_             = 0;
};

} // namespace qpl::ml

#endif // MIDDLE_LAYER_LIMITED_BUFFER_HPP
