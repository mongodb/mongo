/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

#include "output_stream.hpp"

namespace qpl::ml::analytics {

template <>
auto output_stream_t<bit_stream>::perform_pack(const uint8_t *buffer_ptr,
                                               const uint32_t elements_count,
                                               const bool is_start_bit_used) noexcept -> uint32_t {
    uint32_t status = status_list::ok;

    if (1u == actual_bit_width_) {
        if (elements_count > capacity_) {
            return static_cast<uint32_t>(status_list::destination_is_short_error);
        }

        status = pack_index_kernel(buffer_ptr,
                                   elements_count,
                                   &destination_current_ptr_,
                                   ((is_start_bit_used) ? start_bit_ : 0u),
                                   &current_output_index_);

        start_bit_ = (start_bit_ + elements_count) & 7u;
    } else {
        // Pack array of indices, destination overflow
        status = pack_index_kernel(buffer_ptr,
                                   elements_count,
                                   &destination_current_ptr_,
                                   bytes_available(),
                                   &current_output_index_);
    }

    elements_written_ += elements_count;
    capacity_ -= elements_count;

    return status;
}

template <>
uint32_t output_stream_t<array_stream>::perform_pack(const uint8_t *buffer_ptr,
                                                     const uint32_t elements_count,
                                                     const bool UNREFERENCED_PARAMETER(is_start_bit_used)) noexcept {
    uint32_t status = status_list::ok;

    if (bit_width_format_ == output_bit_width_format_t::same_as_input ||
        input_buffer_bit_width_ > 1u) {
        if (elements_count > capacity_) {
            return static_cast<uint32_t>(status_list::destination_is_short_error);
        }

        // For this branch 4th parameter of pack kernel means bit offset
        status = pack_index_kernel(buffer_ptr,
                                   elements_count,
                                   &destination_current_ptr_,
                                   start_bit_,
                                   &current_output_index_);

        // Calculate new bit start bit for the following pack
        start_bit_ = (start_bit_ + elements_count * actual_bit_width_) & max_bit_index;
    } else {
        status = pack_index_kernel(buffer_ptr,
                                   elements_count,
                                   &destination_current_ptr_,
                                   bytes_available(), // Otherwise 4th parameter is destination length
                                   &current_output_index_);
    }

    elements_written_ += elements_count;
    capacity_ -= elements_count;

    return status;
}

} // namespace qpl::ml::analytics
