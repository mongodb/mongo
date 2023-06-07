/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/


#include <util/checksum.hpp>
#include "iterator"

#include "gzip_decorator.hpp"

#include "compression/inflate/inflate.hpp"
#include "compression/inflate/inflate_state.hpp"

#include "compression/deflate/deflate.hpp"
#include "compression/deflate/streams/sw_deflate_state.hpp"
#include "compression/deflate/streams/hw_deflate_state.hpp"

#include "compression/verification/verify.hpp"

namespace qpl::ml::compression {

namespace gzip_sizes {
constexpr size_t gzip_header_size  = 10;
constexpr size_t gzip_trailer_size = 8;
}

namespace gzip_fields {
constexpr uint8_t  ID1_RFC_VALUE             = 31u;
constexpr uint8_t  ID2_RFC_VALUE             = 139u;
constexpr uint8_t  CM_RFC_VALUE              = 8u;
constexpr uint32_t GZIP_HEADER_MIN_BYTE_SIZE = 10u;
}

namespace gzip_flags {
constexpr uint8_t text           = 1u;
constexpr uint8_t crc16          = 2u;
constexpr uint8_t extra          = 4u;
constexpr uint8_t name           = 8u;
constexpr uint8_t comment        = 0x10;
constexpr uint8_t reserverd_bits = 0x20u | 0x40u | 0x80u;
}

struct wrapper_result_t {
    uint32_t status_code_;
    uint32_t bytes_done_;
};

std::array<uint8_t, gzip_sizes::gzip_header_size> default_gzip_header = {0x1f, 0x8b,
                                                                         0x08, 0x00,
                                                                         0x00, 0x00,
                                                                         0x00, 0x00,
                                                                         0x00, 0xff};

static inline auto seek_until_zero(const uint8_t **begin_ptr, const uint8_t *end_ptr) noexcept -> qpl_ml_status {
    auto current_ptr = begin_ptr;

    do {
        if (*current_ptr == end_ptr) {
            return status_list::input_too_small;
        } else {
            (*current_ptr)++;
        }
    } while ((**current_ptr) != 0u);

    return status_list::ok;
}

static inline bool parse_gzip_flags(const uint8_t *begin_ptr,
                                    const uint8_t *end_ptr,
                                    uint8_t flags,
                                    uint32_t &size) noexcept {
    const uint8_t *current_stream_ptr = begin_ptr;

    if (flags & gzip_flags::reserverd_bits) {
        return status_list::gzip_header_error; // gzip err
    }

    if (flags & gzip_flags::extra) {
        if (current_stream_ptr + 1 >= end_ptr) {
            return status_list::input_too_small;
        }

        uint16_t extra_length = *(reinterpret_cast<const uint16_t *>(current_stream_ptr));
        current_stream_ptr += 2;

        if (current_stream_ptr + extra_length > end_ptr) {
            return status_list::input_too_small;
        }

        current_stream_ptr += extra_length;
    }

    if (flags & gzip_flags::name) {
        auto status = seek_until_zero(&current_stream_ptr, end_ptr);

        if (status) {
            return status;
        }
    }

    if (flags & gzip_flags::comment) {
        auto status = seek_until_zero(&current_stream_ptr, end_ptr);

        if (status) {
            return status;
        }
    }

    if (flags & gzip_flags::crc16) {
        if (current_stream_ptr + 2 > end_ptr) {
            return status_list::input_too_small;
        } else {
            // header.crc16 = *(static_cast<const uint16_t *>(current_stream_ptr))
            current_stream_ptr += 2;
        }
    }

    size = static_cast<uint32_t>(std::distance(begin_ptr, current_stream_ptr));
    return status_list::ok;
}

auto gzip_decorator::read_header(const uint8_t *stream_ptr,
                                 uint32_t stream_size,
                                 gzip_header &header) noexcept -> qpl_ml_status {
    if (stream_size < gzip_fields::GZIP_HEADER_MIN_BYTE_SIZE) {
        return false;
    }

    const uint8_t *stream_end_ptr     = stream_ptr + stream_size;
    const uint8_t *current_stream_ptr = stream_ptr;

    header.byte_size = gzip_fields::GZIP_HEADER_MIN_BYTE_SIZE;

    const uint8_t  id1                = current_stream_ptr[0];
    const uint8_t  id2                = current_stream_ptr[1];
    const uint8_t  compression_method = current_stream_ptr[2];
    const uint8_t  flags              = current_stream_ptr[3];
    const uint32_t modification_time  = *(reinterpret_cast<const uint32_t *>(current_stream_ptr + 4));

    // const uint8_t extra_flags = current_stream_ptr[8];
    const uint8_t os          = current_stream_ptr[9];

    current_stream_ptr += gzip_fields::GZIP_HEADER_MIN_BYTE_SIZE;

    if (gzip_fields::ID1_RFC_VALUE != id1 || gzip_fields::ID2_RFC_VALUE != id2) {
        return status_list::gzip_header_error; // gzip err
    }

    if (gzip_fields::CM_RFC_VALUE != compression_method) {
        return status_list::gzip_header_error; // gzip err
    }

    header.ID1                = id1;
    header.ID2                = id2;
    header.compression_method = compression_method;
    header.modification_time  = modification_time;
    header.os                 = os;

    header.flags = flags;

    uint32_t gzip_extra_bytes = 0u;

    auto status = parse_gzip_flags(current_stream_ptr, stream_end_ptr, flags, gzip_extra_bytes);

    if (status) {
        return status;
    } else {
        current_stream_ptr += gzip_extra_bytes;
    }

    header.byte_size = static_cast<uint32_t>(std::distance(stream_ptr, current_stream_ptr));

    return status;
}

template <class F, class state_t, class ...arguments>
auto gzip_decorator::unwrap(F function, state_t &state, arguments... args) noexcept -> decompression_operation_result_t {
    uint8_t* saved_output_ptr  = state.get_output_data(); //state.get_output_buffer;
    uint32_t origin_input_size = state.get_input_size();
    uint32_t wrapper_bytes     = 0;

    decompression_operation_result_t result{};

    if (state.is_first()) {
        gzip_header header;
        uint8_t* input_ptr  = state.get_input_data();

        auto status = read_header(input_ptr, origin_input_size, header);

        if (status_list::ok != status) {
            result.status_code_ = status;

            return result;
        }

        state.input(input_ptr + header.byte_size, input_ptr + origin_input_size);
        wrapper_bytes = header.byte_size;
    }

    if (state.is_last()) {
        state.input(state.get_input_data(), state.get_input_data() + state.get_input_size() - sizeof(gzip_trailer));

        wrapper_bytes += sizeof(gzip_trailer);
    }

    result = function(state, args...);

    if (result.status_code_) {
        return result;
    }

    auto crc = state.get_crc();

    if constexpr (state_t::execution_path == execution_path_t::hardware) {
        crc = result.checksums_.crc32_;
    } else {
        crc = util::crc32_gzip(saved_output_ptr, saved_output_ptr + result.output_bytes_, crc);
    }

    if (state.is_last() && origin_input_size - result.completed_bytes_ < sizeof(gzip_trailer)) {
        auto trailer = reinterpret_cast<gzip_trailer *> (state.get_input_data());
        if (trailer->crc32 != crc ||
            trailer->input_size != result.output_bytes_) {
            result.status_code_ = qpl::ml::status_list::verify_error;
        }
    }

    state.crc_seed(crc);
    result.checksums_.crc32_ = crc;
    result.completed_bytes_ += wrapper_bytes;

    return result;
}

template <execution_path_t path>
using inflate_t = decltype(inflate<path, inflate_mode_t::inflate_default>)*;

template
auto gzip_decorator::unwrap<inflate_t<execution_path_t::software>,
                            inflate_state<execution_path_t::software>,
                            end_processing_condition_t>(inflate_t<execution_path_t::software> function,
                                                        inflate_state<execution_path_t::software> &state,
                                                        end_processing_condition_t end_processing_condition)
noexcept -> decompression_operation_result_t;

template
auto gzip_decorator::unwrap<inflate_t<execution_path_t::hardware>,
                            inflate_state<execution_path_t::hardware>,
                            end_processing_condition_t>(inflate_t<execution_path_t::hardware> function,
                                                        inflate_state<execution_path_t::hardware> &state,
                                                        end_processing_condition_t end_processing_condition)
noexcept -> decompression_operation_result_t;

/* ------ GZIP WRAP ------ */

static inline auto write_gzip_header(uint8_t *const destination_ptr, const uint32_t size) noexcept -> wrapper_result_t {
    wrapper_result_t result{};

    if (size < gzip_sizes::gzip_header_size) {
        result.status_code_ = status_list::more_output_needed;
        return result;
    }

    util::copy(default_gzip_header.data(), default_gzip_header.data() + gzip_sizes::gzip_header_size, destination_ptr);

    result.status_code_ = status_list::ok;
    result.bytes_done_  = gzip_sizes::gzip_header_size;

    return result;
}

static inline auto write_gzip_trailer(uint8_t *destination_ptr,
                                      const uint32_t size,
                                      const uint32_t length,
                                      const uint32_t crc) noexcept -> wrapper_result_t {
    wrapper_result_t result{};

    if (size < gzip_sizes::gzip_trailer_size) {
        result.status_code_ = status_list::more_output_needed;
        return result;
    }

    auto gzip_trailer = static_cast<uint64_t>(length) << 32u | crc;
    auto data_ptr = reinterpret_cast<uint8_t *>(&gzip_trailer);

    util::copy(data_ptr, data_ptr + gzip_sizes::gzip_trailer_size, destination_ptr);

    result.status_code_ = status_list::ok;
    result.bytes_done_  = gzip_sizes::gzip_trailer_size;

    return result;
}

template <class F, class state_t, class ...arguments>
auto gzip_decorator::wrap(F function, state_t &state, arguments... args) noexcept -> compression_operation_result_t {
    compression_operation_result_t result{};

    auto data_ptr      = state.next_out();
    auto data_size     = state.avail_out();
    auto wrapper_bytes = 0u;

    if (state.is_first_chunk()) {
        auto wrapper_result = write_gzip_header(data_ptr, data_size);

        if (wrapper_result.status_code_) {
            return result;
        }

        wrapper_bytes = wrapper_result.bytes_done_;
        state.set_output_prologue(wrapper_bytes);
    }

    result = function(state, args...);

    result.output_bytes_ += wrapper_bytes;

    if (!result.status_code_ && state.is_last_chunk()) {
        auto wrapper_result = write_gzip_trailer(data_ptr + result.output_bytes_,
                                                 data_size - result.output_bytes_,
                                                 result.output_bytes_,
                                                 result.checksums_.crc32_);

        if (wrapper_result.status_code_) {
            return result;
        }

        result.output_bytes_ += wrapper_result.bytes_done_;
    }

    return result;
}

template <execution_path_t path>
using deflate_t = decltype(deflate<path, deflate_mode_t::deflate_default>)*;

template
auto gzip_decorator::wrap<deflate_t<execution_path_t::software>,
                          deflate_state<execution_path_t::software>,
                          uint8_t *, uint32_t>(deflate_t<execution_path_t::software> function,
                                               deflate_state<execution_path_t::software> &state,
                                               uint8_t *begin,
                                               const uint32_t size) noexcept -> compression_operation_result_t;

template
auto gzip_decorator::wrap<deflate_t<execution_path_t::hardware>,
                          deflate_state<execution_path_t::hardware>,
                          uint8_t *, uint32_t>(deflate_t<execution_path_t::hardware> function,
                                               deflate_state<execution_path_t::hardware> &state,
                                               uint8_t *begin,
                                               const uint32_t size) noexcept -> compression_operation_result_t;

}
