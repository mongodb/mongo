/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

#include "huffman_only_units.hpp"

#include "util/util.hpp"
#include "util/memory.hpp"

#include "igzip_lib.h"
#include "bitbuf2.h"

#include "compression/deflate/containers/huffman_table.hpp"

namespace qpl::ml::compression {

static inline void get_literal_code(const isal_hufftables *const huffman_table_ptr,
                                    const uint32_t literal,
                                    uint32_t &code,
                                    uint32_t &code_length) {
    code        = huffman_table_ptr->lit_table[literal];
    code_length = huffman_table_ptr->lit_table_sizes[literal];
}

void update_state(huffman_only_state<execution_path_t::software> &stream,
                  uint8_t *start_in_ptr,
                  uint8_t *next_in_ptr,
                  uint8_t *end_in_ptr) noexcept {
    auto isal_state = &stream.isal_stream_ptr_->internal_state;

    if (next_in_ptr - start_in_ptr > 0) {
        isal_state->has_hist = IGZIP_HIST;
    }

    stream.isal_stream_ptr_->next_in  = next_in_ptr;
    stream.isal_stream_ptr_->total_in += static_cast<uint32_t>(std::distance(start_in_ptr, next_in_ptr));
    stream.isal_stream_ptr_->avail_in = static_cast<uint32_t>(std::distance(next_in_ptr, end_in_ptr));

    stream.dump_bit_buffer();
}

auto huffman_only_compress_block(huffman_only_state<execution_path_t::software> &stream,
                                 compression_state_t &state) noexcept -> qpl_ml_status {
    auto isal_state = &stream.isal_stream_ptr_->internal_state;
    auto bit_buffer = &isal_state->bitbuf;

    uint32_t literal       = 0;
    uint8_t  *start_in_ptr = stream.isal_stream_ptr_->next_in;
    uint8_t  *next_in_ptr  = start_in_ptr;
    uint8_t  *end_in_ptr   = start_in_ptr + stream.isal_stream_ptr_->avail_in;

    uint32_t literal_code = 0;
    uint32_t code_length  = 0;

    if (stream.isal_stream_ptr_->avail_in == 0) {
        if (stream.isal_stream_ptr_->end_of_stream) {
            state = compression_state_t::compress_rest_data;
        } else {
            state = compression_state_t::finish_compression_process;
        }

        return status_list::ok;
    }

    stream.reset_bit_buffer();

    while (next_in_ptr + ISAL_LOOK_AHEAD < end_in_ptr) {

        if (is_full(bit_buffer)) {
            update_state(stream, start_in_ptr, next_in_ptr, end_in_ptr);
            return status_list::more_output_needed;
        }

        literal = *reinterpret_cast<uint32_t *>(next_in_ptr);

        get_literal_code(stream.isal_stream_ptr_->hufftables,
                         literal & max_uint8,
                         literal_code,
                         code_length);
        write_bits(bit_buffer, literal_code, code_length);

        next_in_ptr++;
    }

    update_state(stream, start_in_ptr, next_in_ptr, end_in_ptr);

    assert(stream.isal_stream_ptr_->avail_in <= ISAL_LOOK_AHEAD);
    if (stream.isal_stream_ptr_->end_of_stream) {
        state = compression_state_t::compress_rest_data;
    } else {
        state = compression_state_t::finish_compression_process;
    }

    return status_list::ok;
}

auto huffman_only_finalize(huffman_only_state<execution_path_t::software> &stream,
                           compression_state_t &state) noexcept -> qpl_ml_status {
    auto isal_state = &stream.isal_stream_ptr_->internal_state;
    auto bit_buffer = &isal_state->bitbuf;

    uint32_t literal       = 0;
    uint8_t  *start_in_ptr = stream.isal_stream_ptr_->next_in;
    uint8_t  *next_in_ptr  = start_in_ptr;
    uint8_t  *end_in_ptr   = start_in_ptr + stream.isal_stream_ptr_->avail_in;

    uint32_t literal_code = 0;
    uint32_t code_length  = 0;

    stream.reset_bit_buffer();

    start_in_ptr = stream.isal_stream_ptr_->next_in;
    end_in_ptr   = start_in_ptr + stream.isal_stream_ptr_->avail_in;
    next_in_ptr  = start_in_ptr;

    if (stream.isal_stream_ptr_->avail_in != 0) {
        while (next_in_ptr + 3 < end_in_ptr) {
            if (is_full(bit_buffer)) {
                update_state(stream, start_in_ptr, next_in_ptr, end_in_ptr);
                return status_list::more_output_needed;
            }

            literal = *reinterpret_cast<uint32_t *>(next_in_ptr);

            get_literal_code(stream.isal_stream_ptr_->hufftables,
                             literal & max_uint8,
                             literal_code,
                             code_length);
            write_bits(bit_buffer, literal_code, code_length);

            next_in_ptr++;

        }

        while (next_in_ptr < end_in_ptr) {
            if (is_full(bit_buffer)) {
                update_state(stream, start_in_ptr, next_in_ptr, end_in_ptr);
                return status_list::more_output_needed;
            }

            literal = *next_in_ptr;

            get_literal_code(stream.isal_stream_ptr_->hufftables,
                             literal & max_uint8,
                             literal_code,
                             code_length);
            write_bits(bit_buffer, literal_code, code_length);

            next_in_ptr++;
        }
    }

    if (!is_full(bit_buffer)) {
        state = compression_state_t::flush_write_buffer;
        stream.last_bits_offset_ = bit_buffer->m_bit_count & 7;
    }

    update_state(stream, start_in_ptr, next_in_ptr, end_in_ptr);

    return is_full(bit_buffer)
           ? status_list::more_output_needed
           : status_list::ok;
}

auto huffman_only_create_huffman_table(huffman_only_state<execution_path_t::software> &stream,
                                       compression_state_t &state) noexcept -> qpl_ml_status {
    auto isal_state = &stream.isal_stream_ptr_->internal_state;

    if (stream.compression_mode_ == dynamic_mode) {
        isal_huff_histogram *histogram = reinterpret_cast<isal_huff_histogram *>(isal_state->buffer);
        memset(isal_state->buffer, 0, sizeof(isal_huff_histogram));

        // Manually creating histogram since no need for dist histogram in huffman only compression
        // ISAL routine fills in dist histogram which results in unoptimal compression for huffman only
        
        for (uint32_t i = 0; i < stream.isal_stream_ptr_->avail_in; i++) {
            histogram->lit_len_histogram[stream.isal_stream_ptr_->next_in[i]]++;
        }
        
        isal_create_hufftables(stream.isal_stream_ptr_->hufftables, histogram);
    }

    state = compression_state_t::compression_body;

    return status_list::ok;
}

auto convert_output_to_big_endian(huffman_only_state<execution_path_t::software> &stream,
                                  compression_state_t &UNREFERENCED_PARAMETER(state)) noexcept -> qpl_ml_status {
    // Setting new pointer to treat stream as uint16_t
    const uint32_t actual_length = stream.isal_stream_ptr_->total_out >> 1u;
    auto           *array_ptr    = reinterpret_cast<uint16_t *>(stream.isal_stream_ptr_->next_out -
                                                                stream.isal_stream_ptr_->total_out);

    // Main cycle
    for (uint32_t i = 0; i < actual_length; i++) {
        array_ptr[i] = reverse_bits(array_ptr[i], 16);
    }

    // Check if the last byte should be bit reversed (in case of odd stream length)
    if (stream.isal_stream_ptr_->total_out % 2 == 1) {
        uint16_t temporary_array_value = *(reinterpret_cast<uint8_t *>(array_ptr + actual_length));
        array_ptr[actual_length] = reverse_bits(temporary_array_value, 16);
    }

    return status_list::ok;
}

} // namespace qpl::ml::compression
