/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

#ifndef INPUT_STREAM_HPP
#define INPUT_STREAM_HPP

#include <cstdint>
#include <iterator>

#include "common/allocation_buffer_t.hpp"
#include "common/linear_allocator.hpp"
#include "compression/inflate/inflate.hpp"
#include "compression/inflate/inflate_state.hpp"
#include "compression/stream_decorators/default_decorator.hpp"
#include "analytics_defs.hpp"
#include "dispatcher/dispatcher.hpp"
#include "common/limited_buffer.hpp"
#include "util/checksum.hpp"

namespace qpl::ml::analytics {

class input_stream_t final : public buffer_t {
public:
    class builder;

    struct unpack_result_t {
        uint32_t status            = status_list::ok;
        uint32_t unpacked_elements = 0;
        uint32_t unpacked_bytes    = 0;

        explicit unpack_result_t(uint32_t status) noexcept
                : status(status) {
            // Empty constructor
        };

        explicit unpack_result_t(uint32_t status,
                                 uint32_t unpacked_elements,
                                 uint32_t unpacked_bytes) noexcept
                : status(status),
                  unpacked_elements(unpacked_elements),
                  unpacked_bytes(unpacked_bytes) {
            // Empty constructor
        };

        constexpr unpack_result_t(const unpack_result_t &other) noexcept = default;

        constexpr unpack_result_t(unpack_result_t &&other) noexcept = default;

        auto operator=(const unpack_result_t &other) noexcept -> unpack_result_t & = default;

        auto operator=(unpack_result_t &&other) noexcept -> unpack_result_t & = default;
    };

    struct compression_meta_t {
        qpl_decomp_end_proc end_processing_style;
        uint32_t            ignore_last_bits;
    };

    enum class crc_t : uint32_t {
        gzip  = 0,
        iscsi = 1
    };

    template <analytic_pipeline pipeline>
    auto unpack(limited_buffer_t &output_buffer) noexcept -> unpack_result_t;

    template <analytic_pipeline pipeline>
    auto unpack(limited_buffer_t &output_buffer, size_t required_elements) noexcept -> unpack_result_t;

    [[nodiscard]] inline auto bit_width() const noexcept -> uint32_t {
        return bit_width_;
    }

    [[nodiscard]] inline auto stream_format() const noexcept -> stream_format_t {
        return stream_format_;
    }

    [[nodiscard]] inline auto source_size() const noexcept -> uint32_t {
        return current_source_size_;
    }

    [[nodiscard]] inline auto is_processed() const noexcept -> bool {
        return (current_number_of_elements_ == 0);
    }

    [[nodiscard]] inline auto is_compressed() const noexcept -> bool {
        return is_compressed_;
    }

    [[nodiscard]] inline auto current_ptr() const noexcept -> uint8_t * {
        return current_source_ptr_;
    }

    [[nodiscard]] inline auto compression_meta() const noexcept -> compression_meta_t {
        return compression_meta_;
    }

    inline void add_elements_processed(const uint32_t elements_processed) noexcept {
        current_number_of_elements_ -= elements_processed;
    }

    [[nodiscard]] inline auto skip_prologue(limited_buffer_t &output_buffer) noexcept -> uint32_t {
        if (!is_compressed()) {
            current_source_ptr_ += prologue_size_;
            return QPL_STS_OK;
        }

        auto prologue_size_left = prologue_size_;

        while (prologue_size_left > 0u) {
            auto prologue_part_size = std::min(prologue_size_left, output_buffer.max_elements_count());

            auto result = ml::compression::default_decorator::unwrap(
                ml::compression::inflate<execution_path_t::software, compression::inflate_mode_t::inflate_default>,
                state_.output(output_buffer.begin(), output_buffer.begin() + prologue_part_size),
                compression::end_processing_condition_t::stop_and_check_for_bfinal_eob);

            auto status = result.status_code_;
            if ((QPL_STS_OK != status) && (QPL_STS_MORE_OUTPUT_NEEDED != status)) {
                return status;
            }
            if (0u == result.completed_bytes_) {
                return QPL_STS_SIZE_ERR;
            }
            prologue_size_left -= result.output_bytes_;
        }

        return QPL_STS_OK;
    }

    inline void calculate_checksums() noexcept {
        if (!omit_checksums_calculation_) {
            if (is_compressed())
            {
                /**< @todo Move decompression checksums from state to input stream */
                checksums_.crc32_ = state_.get_crc();
            }
            else {
                checksums_.crc32_ = (crc_type_ == crc_t::gzip) ? util::crc32_gzip(data(), data() + size(), checksums_.crc32_)
                    : util::crc32_iscsi_inv(data(), data() + size(), checksums_.crc32_);
                checksums_.xor_ = util::xor_checksum(data(), data() + size(), checksums_.xor_);
            }
        }
    }

    inline void shift_current_ptr(const uint32_t shift_in_bytes) noexcept {
        current_source_ptr_ += shift_in_bytes;
        current_source_size_ -= shift_in_bytes;
    }

    [[nodiscard]] inline auto elements_left() const noexcept -> uint32_t {
        return current_number_of_elements_;
    }

    [[nodiscard]] inline auto crc_checksum() const noexcept -> uint32_t {
        return checksums_.crc32_;
    }

    [[nodiscard]] inline auto xor_checksum() const noexcept -> uint32_t {
        return checksums_.xor_;
    }

    [[nodiscard]] inline auto crc_type() const noexcept -> crc_t {
        return crc_type_;
    }

    [[nodiscard]] inline auto are_aggregates_disabled() const noexcept -> bool {
        return omit_aggregates_calculation_;
    }

    [[nodiscard]] inline auto is_checksum_disabled() const noexcept -> bool {
        return omit_checksums_calculation_;
    }

    [[nodiscard]] inline auto total_elements_count() const noexcept -> uint32_t {
        return number_of_elements_;
    }

    [[nodiscard]] inline auto prologue_size() const noexcept -> uint32_t {
        return prologue_size_;
    }

    [[nodiscard]] inline auto decompression_status() const noexcept -> uint32_t {
        return decompression_status_;
    }

protected:
    template <class iterator_t>
    input_stream_t(iterator_t begin, iterator_t end) noexcept
            : buffer_t(begin, end) {

    }

private:
    auto initialize_sw_kernels() noexcept -> void;

    dispatcher::unpack_table_t::value_type unpack_kernel_           = nullptr;
    dispatcher::unpack_prle_table_t::value_type unpack_prle_kernel_ = nullptr;

    ml::compression::inflate_state<execution_path_t::software> state_;

    uint8_t            *current_source_ptr_         = nullptr;
    uint8_t            *decompress_begin_           = nullptr;
    uint8_t            *decompress_end_             = nullptr;
    uint8_t            *current_decompress_         = nullptr;
    uint32_t           prev_decompressed_bytes_     = 0u;
    bool               omit_checksums_calculation_  = false;
    bool               omit_aggregates_calculation_ = false;
    bool               is_compressed_               = false;
    checksums_t        checksums_                   = {0u, 0u};
    uint32_t           current_source_size_         = 0u;
    uint32_t           number_of_elements_          = 0u;
    uint32_t           current_number_of_elements_  = 0u;
    uint32_t           prologue_size_               = 0u;
    int32_t            prle_count_                  = 0u;
    uint32_t           prle_value_                  = 0u;
    uint32_t           prle_index_                  = 0u;
    uint8_t            bit_width_                   = 0u;
    crc_t              crc_type_                    = crc_t::gzip;
    stream_format_t    stream_format_               = stream_format_t::le_format;
    compression_meta_t compression_meta_            = {};
    uint32_t           decompression_status_        = status_list::ok;
};

class input_stream_t::builder {
public:
    template <class iterator_t>
    builder(iterator_t begin, iterator_t end)
            : stream_(begin, end) {

    }

    inline auto omit_checksums(bool value) noexcept -> builder & {
        stream_.omit_checksums_calculation_ = value;

        return *this;
    }

    inline auto omit_aggregates(bool value) noexcept -> builder & {
        stream_.omit_aggregates_calculation_ = value;

        return *this;
    }

    inline auto compressed(bool value,
                           qpl_decomp_end_proc end_strategy = qpl_stop_and_check_for_bfinal_eob,
                           uint32_t ignore_last_bits = 0) noexcept -> builder & {
        stream_.is_compressed_    = value;
        stream_.compression_meta_ = {end_strategy, ignore_last_bits};

        return *this;
    }

    template <execution_path_t path>
    inline auto decompress_buffer(uint8_t* UNREFERENCED_PARAMETER(begin),
                                  uint8_t* UNREFERENCED_PARAMETER(end)) noexcept -> builder & {
        if constexpr(path == execution_path_t::software || path == execution_path_t::auto_detect) {
            stream_.decompress_begin_   = begin;
            stream_.current_decompress_ = begin;
            stream_.decompress_end_     = end;
        }

        return *this;
    }

    inline auto stream_format(stream_format_t format, uint32_t bit_width) noexcept -> builder & {
        stream_.stream_format_ = format;
        stream_.bit_width_     = bit_width;

        return *this;
    }

    inline auto ignore_bytes(uint32_t value) noexcept -> builder & {
        stream_.prologue_size_ = value;

        return *this;
    }

    inline auto crc_type(crc_t value) noexcept -> builder & {
        stream_.crc_type_ = value;

        return *this;
    }

    inline auto element_count(size_t value) noexcept -> builder & {
        stream_.number_of_elements_ = static_cast<uint32_t>(value);

        return *this;
    }

    template <execution_path_t path>
    inline auto build(allocation_buffer_t UNREFERENCED_PARAMETER(buffer)
                          = allocation_buffer_t::empty()) -> input_stream_t {
        stream_.current_source_ptr_         = stream_.data();
        stream_.current_number_of_elements_ = stream_.number_of_elements_;
        stream_.current_source_size_        = stream_.size();

        if (stream_.stream_format_ == stream_format_t::prle_format && !stream_.is_compressed_) {
            stream_.bit_width_ = *stream_.current_source_ptr_;
            stream_.current_source_ptr_++;
            stream_.current_source_size_--;
        }

        if constexpr(path == execution_path_t::software || path == execution_path_t::auto_detect) {
            if (stream_.is_compressed_) {
                const ml::util::linear_allocator allocator(buffer);

                stream_.state_ = compression::inflate_state<execution_path_t::software>::create<true>(allocator)
                        .input(stream_.current_source_ptr_, stream_.current_source_ptr_ + stream_.current_source_size_);

                if (stream_.stream_format_ == stream_format_t::prle_format) {
                    stream_.state_.output(&stream_.bit_width_, &stream_.bit_width_ + 1);

                    auto result = ml::compression::default_decorator::unwrap(
                            ml::compression::inflate<execution_path_t::software, compression::inflate_mode_t::inflate_default>,
                            stream_.state_,
                            compression::end_processing_condition_t::stop_and_check_for_bfinal_eob);

                    stream_.decompression_status_ = result.status_code_;
                }
            }

            stream_.initialize_sw_kernels();
        }

        return std::move(stream_);
    }

private:
    input_stream_t stream_;
};

} // namespace qpl::ml::analytics

#endif // INPUT_STREAM_HPP
