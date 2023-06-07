/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

#include "input_stream.hpp"

namespace qpl::ml::analytics {

template <>
auto input_stream_t::unpack<analytic_pipeline::simple>(limited_buffer_t &output_buffer,
                                                       size_t required_elements) noexcept -> unpack_result_t {
    uint32_t elements_to_unpack = std::min(current_number_of_elements_, static_cast<uint32_t>(required_elements));

    unpack_kernel_(current_source_ptr_, elements_to_unpack, 0, output_buffer.data());

    current_number_of_elements_ -= elements_to_unpack;
    uint32_t bytes_processed = util::bit_to_byte(elements_to_unpack * bit_width_);

    current_source_ptr_ += bytes_processed;
    current_source_size_ -= bytes_processed;

    return unpack_result_t(status_list::ok, elements_to_unpack, bytes_processed);
}

template <>
auto input_stream_t::unpack<analytic_pipeline::simple>(limited_buffer_t &output_buffer) noexcept -> unpack_result_t {
    return input_stream_t::unpack<analytic_pipeline::simple>(output_buffer, output_buffer.max_elements_count());
}

template <>
auto input_stream_t::unpack<analytic_pipeline::prle>(limited_buffer_t &output_buffer,
                                                     size_t required_elements) noexcept -> unpack_result_t {
    uint8_t *saved_source_ptr = current_source_ptr_;
    uint8_t *current_ptr      = output_buffer.data();

    required_elements = std::min(static_cast<uint32_t>(required_elements), current_number_of_elements_);

    auto status = unpack_prle_kernel_(&current_source_ptr_,
                                      current_source_size_,
                                      bit_width_,
                                      &current_ptr,
                                      static_cast<uint32_t>(required_elements),
                                      &prle_count_,
                                      &prle_value_);

    uint32_t elements_processed = (static_cast<uint32_t>(current_ptr - output_buffer.data())) >> prle_index_;

    if ((status_list::source_is_short_error == status || status_list::destination_is_short_error == status)
        && elements_processed == 0) {
        return unpack_result_t(status);
    }

    auto unpacked_bytes = static_cast<uint32_t>(current_source_ptr_ - saved_source_ptr);

    elements_processed = (current_number_of_elements_ < elements_processed)
                         ? current_number_of_elements_
                         : elements_processed;

    // If all input data is unpacked, but a number of decompressed elements is less than number_elements - return error
    if (status_list::source_is_short_error == status && elements_processed < input_stream_t::elements_left()) {
        return unpack_result_t(status);
    }

    input_stream_t::add_elements_processed(elements_processed);

    // There was unpacked more than source length
    if (input_stream_t::elements_left() > 0 && unpacked_bytes >= current_source_size_) {
        // Situation when (prle_count_ == elements_left(), but current_source_size == 0) means the following:
        // The whole PRLE stream was unpacked, but there are not enough place in internal buffer to store all these values.
        // So all remaining repeating values will be stored in the next iteration, everything's OK, that's not an error.
        if ((uint32_t)prle_count_ != input_stream_t::elements_left()) {
            return unpack_result_t(status_list::source_is_short_error);
        }
    }

    current_source_size_ -= unpacked_bytes;

    return unpack_result_t(status_list::ok, elements_processed, unpacked_bytes);
}

template <>
auto input_stream_t::unpack<analytic_pipeline::prle>(limited_buffer_t &output_buffer) noexcept -> unpack_result_t {
    return input_stream_t::unpack<analytic_pipeline::prle>(output_buffer, output_buffer.max_elements_count());
}

template <>
auto input_stream_t::unpack<analytic_pipeline::inflate>(limited_buffer_t &output_buffer,
                                                        size_t required_elements) noexcept -> unpack_result_t {
    auto elements_to_decompress = (required_elements >> 3u) << 3u;
    auto bytes_to_decompress    = (elements_to_decompress * bit_width_) / byte_bits_size;

    if (elements_to_decompress <= current_number_of_elements_) {
        state_.terminate();
    }

    auto result = ml::compression::default_decorator::unwrap(
            ml::compression::inflate<execution_path_t::software, compression::inflate_mode_t::inflate_default>,
            state_.output(decompress_begin_, decompress_begin_ + bytes_to_decompress),
            compression::end_processing_condition_t::stop_and_check_for_bfinal_eob);

    auto decompressed_elements = (result.output_bytes_ * byte_bits_size) / bit_width_;
    auto elements_to_unpack    = std::min(decompressed_elements, current_number_of_elements_);
    auto unpacked_bytes        = util::bit_to_byte(elements_to_unpack * bit_width_);

    unpack_kernel_(decompress_begin_, elements_to_unpack, 0, output_buffer.data());

    input_stream_t::add_elements_processed(elements_to_unpack);

    if (result.status_code_ == status_list::more_output_needed) {
        result.status_code_ = status_list::ok;
    }

    return unpack_result_t(result.status_code_, elements_to_unpack, unpacked_bytes);
}

template <>
auto input_stream_t::unpack<analytic_pipeline::inflate>(limited_buffer_t &output_buffer) noexcept
-> unpack_result_t {
    return input_stream_t::unpack<analytic_pipeline::inflate>(output_buffer, output_buffer.max_elements_count());
}

template <>
auto input_stream_t::unpack<analytic_pipeline::inflate_prle>(limited_buffer_t &output_buffer,
                                                             size_t required_elements) noexcept -> unpack_result_t {
    auto result = ml::compression::default_decorator::unwrap(
            ml::compression::inflate<execution_path_t::software, compression::inflate_mode_t::inflate_default>,
            state_.output(current_decompress_, decompress_end_),
            compression::end_processing_condition_t::stop_and_check_for_bfinal_eob);

    if (result.status_code_ != status_list::ok) {
        return unpack_result_t(result.status_code_);
    }

    uint8_t *unpack_source_ptr = decompress_begin_;
    uint8_t *saved_source_ptr  = decompress_begin_;
    uint8_t *current_ptr       = output_buffer.data();

    required_elements = std::min(static_cast<uint32_t>(required_elements), current_number_of_elements_);

    auto status = unpack_prle_kernel_(&unpack_source_ptr,
                                      static_cast<uint32_t>(std::distance(unpack_source_ptr, decompress_end_)),
                                      bit_width_,
                                      &current_ptr,
                                      static_cast<uint32_t>(required_elements),
                                      &prle_count_,
                                      &prle_value_);

    uint32_t elements_processed       = (static_cast<uint32_t>(current_ptr - output_buffer.data())) >> prle_index_;
    uint32_t valid_decompressed_bytes = result.output_bytes_ + prev_decompressed_bytes_;

    if (unpack_source_ptr != (decompress_begin_ + valid_decompressed_bytes)) {
        util::copy(unpack_source_ptr, decompress_begin_ + valid_decompressed_bytes, decompress_begin_);

        current_decompress_      = decompress_begin_ + std::distance(unpack_source_ptr,
                                                                     decompress_begin_ + valid_decompressed_bytes);
        prev_decompressed_bytes_ = static_cast<uint32_t>(std::distance(decompress_begin_, current_decompress_));
    }

    if ((status_list::source_is_short_error == status || status_list::destination_is_short_error == status)
        && elements_processed == 0) {
        return unpack_result_t(status);
    }

    auto unpacked_bytes = static_cast<uint32_t>(unpack_source_ptr - saved_source_ptr);

    elements_processed = (current_number_of_elements_ < elements_processed)
                         ? current_number_of_elements_
                         : elements_processed;

    // If all input data is unpacked, but a number of decompressed elements is less than number_elements - return error
    if (status_list::source_is_short_error == status && elements_processed < input_stream_t::elements_left()) {
        return unpack_result_t(status);
    }

    input_stream_t::add_elements_processed(elements_processed);

    return unpack_result_t(status_list::ok, elements_processed, unpacked_bytes);
}

template <>
auto input_stream_t::unpack<analytic_pipeline::inflate_prle>(limited_buffer_t &output_buffer) noexcept
-> unpack_result_t {
    return input_stream_t::unpack<analytic_pipeline::inflate_prle>(output_buffer, output_buffer.max_elements_count());
}

auto input_stream_t::initialize_sw_kernels() noexcept -> void {
    auto unpack_table      = dispatcher::kernels_dispatcher::get_instance().get_unpack_table();
    auto unpack_prle_table = dispatcher::kernels_dispatcher::get_instance().get_unpack_prle_table();

    uint32_t is_stream_be = (stream_format_ == stream_format_t::be_format) ? 1 : 0;

    uint32_t unpack_index = dispatcher::get_unpack_index(is_stream_be, bit_width_);
    prle_index_ = dispatcher::get_unpack_prle_index(bit_width_);

    if (stream_format_ != stream_format_t::prle_format) {
        unpack_kernel_ = unpack_table[unpack_index];
    } else {
        unpack_prle_kernel_ = unpack_prle_table[prle_index_];
    }
}

} // namespace qpl::ml::analytics
