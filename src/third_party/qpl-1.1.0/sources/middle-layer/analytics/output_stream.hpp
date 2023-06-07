/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

#ifndef OUTPUT_STREAM_HPP
#define OUTPUT_STREAM_HPP

#include <cstdint>
#include <iterator>

#include "analytics_defs.hpp"
#include "dispatcher/dispatcher.hpp"
#include "common/buffer.hpp"
#include "util/util.hpp"

namespace qpl::ml::analytics {

enum output_stream_type_t {
    array_stream = 0,
    bit_stream   = 1
};

template <output_stream_type_t output_stream_type>
class output_stream_t final : public buffer_t {
public:
    class builder;

    output_stream_t() = delete;

    auto perform_pack(const uint8_t *buffer_ptr,
                      uint32_t elements_count,
                      bool is_start_bit_used = true) noexcept -> uint32_t;

    [[nodiscard]] inline auto elements_written() -> uint32_t {
        return elements_written_;
    }

    [[nodiscard]] inline auto bit_width() noexcept -> uint32_t {
        return actual_bit_width_;
    }

    [[nodiscard]] inline auto stream_format() const noexcept -> stream_format_t {
        return stream_format_;
    }

    [[nodiscard]] inline auto bytes_written() const noexcept -> uint32_t {
        return static_cast<uint32_t>(std::distance(data(), destination_current_ptr_));
    }

    [[nodiscard]] auto bytes_available() const noexcept -> uint32_t {
        return size() - bytes_written();
    }

    [[nodiscard]] auto output_bit_width_format() const noexcept -> output_bit_width_format_t {
        return bit_width_format_;
    }

    [[nodiscard]] auto initial_output_index() const noexcept -> uint32_t {
        return initial_output_index_;
    }

    [[nodiscard]] auto is_inverted() const noexcept -> uint32_t {
        return is_inverted_;
    }

    auto invert_data() noexcept -> void {
        is_inverted_ = !is_inverted_;
    }

protected:
    template <class iterator_t>
    output_stream_t(iterator_t begin, iterator_t end)
            : buffer_t(begin, end) {

    }

private:
    dispatcher::pack_index_table_t::value_type pack_index_kernel    = nullptr;
    uint8_t                               *destination_current_ptr_ = nullptr;
    bool                                  is_inverted_              = false;
    bool                                  is_nominal_               = false;
    stream_format_t                       stream_format_            = stream_format_t::le_format;
    output_bit_width_format_t             bit_width_format_         = output_bit_width_format_t::same_as_input;
    uint32_t                              start_bit_                = 0u;
    uint32_t                              current_output_index_     = 0u;
    uint32_t                              initial_output_index_     = 0u;
    uint8_t                               actual_bit_width_         = 0u;
    uint8_t                               input_buffer_bit_width_   = 0u;
    uint32_t                              elements_written_         = 0u;
    size_t                                capacity_                 = 0u;
};

template <output_stream_type_t stream_type>
class output_stream_t<stream_type>::builder {
public:
    template <class iterator_t>
    builder(iterator_t begin, iterator_t end)
            : stream_(begin, end) {
    }

    inline auto stream_format(stream_format_t format) noexcept -> builder & {

        stream_.stream_format_ = format;

        return *this;
    }

    inline auto bit_format(output_bit_width_format_t format, uint32_t bit_width) noexcept -> builder & {
        stream_.bit_width_format_       = format;
        stream_.input_buffer_bit_width_ = bit_width;

        if (output_bit_width_format_t::same_as_input == stream_.bit_width_format_) {
            stream_.actual_bit_width_ = (bit_width) ? bit_width : 32u; // @todo Add PRLE Support
        } else {
            stream_.actual_bit_width_ = 1u << (static_cast<uint32_t>(stream_.bit_width_format_) + 2u);
        }

        return *this;
    }

    inline auto initial_output_index(uint32_t value) noexcept -> builder & {
        stream_.initial_output_index_ = value;

        return *this;
    }

    inline auto ignore_bits(uint32_t value) noexcept -> builder & {
        stream_.start_bit_ = value;

        return *this;
    }

    inline auto input_bit_width(uint32_t value) noexcept -> builder & {
        stream_.input_buffer_bit_width_ = value;

        return *this;
    }

    inline auto nominal(bool value) noexcept -> builder & {
        stream_.is_nominal_ = value;

        return *this;
    }

    inline auto inverted(bool value) noexcept -> builder & {
        stream_.is_inverted_ = value;

        return *this;
    }

    template <execution_path_t path>
    inline auto build() noexcept -> output_stream_t<stream_type> {
        stream_.destination_current_ptr_ = stream_.data();

        if constexpr(path == execution_path_t::software || path == execution_path_t::auto_detect) {
            auto pack_table = dispatcher::kernels_dispatcher::get_instance().get_pack_index_table();
            stream_.capacity_ = (std::distance(stream_.begin(), stream_.end()) * byte_bits_size)
                                / stream_.actual_bit_width_;

            bool     is_output_be = (stream_.stream_format_ == stream_format_t::be_format);
            uint32_t pack_index   = dispatcher::get_pack_index(is_output_be,
                                                               static_cast<uint32_t>(stream_.bit_width_format_),
                                                               static_cast<uint32_t>(stream_.is_nominal_));

            stream_.pack_index_kernel = pack_table[pack_index];

            if constexpr(stream_type == array_stream) {
                if (output_bit_width_format_t::same_as_input == stream_.bit_width_format_
                    || stream_.input_buffer_bit_width_ > 1) {
                    stream_.current_output_index_ = dispatcher::get_pack_bits_index(is_output_be,
                                                                                    stream_.input_buffer_bit_width_,
                                                                                    static_cast<uint32_t>(stream_.bit_width_format_));
                }
                else {
                    stream_.current_output_index_ = stream_.initial_output_index_;
                }
            } else {
                if (output_bit_width_format_t::same_as_input == stream_.bit_width_format_) {
                    stream_.current_output_index_ = dispatcher::get_pack_bits_index(is_output_be,
                                                                                    stream_.input_buffer_bit_width_,
                                                                                    static_cast<uint32_t>(stream_.bit_width_format_));
                }
                else {
                    stream_.current_output_index_ = stream_.initial_output_index_;
                }
            }
        }

        return std::move(stream_);
    }

private:
    output_stream_t<stream_type> stream_;
};

} // namespace qpl::ml::analytics

#endif // OUTPUT_STREAM_HPP
