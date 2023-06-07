/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

#include "compression_units.hpp"

#include "util/util.hpp"
#include "util/memory.hpp"

#include "deflate_slow.h"
#include "deflate_slow_utils.h"

#include "compression/deflate/implementations/deflate_implementation.hpp"
#include "compression/utils.hpp"

#include "igzip_lib.h"
#include "bitbuf2.h"

#include "dispatcher/dispatcher.hpp"
#include "qplc_deflate_utils.h"

static inline qplc_slow_deflate_body_t_ptr qplc_slow_deflate_body() {
    return (qplc_slow_deflate_body_t_ptr)(qpl::ml::dispatcher::kernels_dispatcher::get_instance().get_deflate_fix_table()[0]);
}

extern "C" {
extern void isal_deflate_body(struct isal_zstream *stream);
extern void isal_deflate_finish(struct isal_zstream *stream);
}
namespace qpl::ml::compression {

auto write_header(deflate_state<execution_path_t::software> &stream, compression_state_t &state) noexcept -> qpl_ml_status {
    if (stream.compression_mode() == canned_mode ||
        !stream.should_start_new_block()) {
        state = compression_state_t::compression_body;

        return status_list::ok;
    }

    if (stream.mini_blocks_support() == mini_blocks_support_t::enabled) {
        stream.write_mini_block_index();
    }

    auto isal_state = &stream.isal_stream_ptr_->internal_state;
    auto bit_buffer = &isal_state->bitbuf;

    uint8_t  *deflate_header           = stream.isal_stream_ptr_->hufftables->deflate_hdr;
    uint32_t deflate_header_count      = stream.isal_stream_ptr_->hufftables->deflate_hdr_count;
    uint32_t deflate_header_extra_bits = stream.isal_stream_ptr_->hufftables->deflate_hdr_extra_bits;

    isal_state->has_eob_hdr = 1;

    uint32_t hdr_extra_bits = deflate_header[deflate_header_count];
    uint32_t count          = deflate_header_count - isal_state->count;

    if (count != 0) {
        if (count > stream.isal_stream_ptr_->avail_out) {
            count = stream.isal_stream_ptr_->avail_out;
        }

        // By defalut the header has the first bit = 1 (Final Deflate block marker)
        if (!stream.isal_stream_ptr_->end_of_stream && isal_state->count == 0) {
            /* Assumes the final block bit is the first bit */
            deflate_header[0] ^= 0x1;
            isal_state->has_eob_hdr = !isal_state->has_eob_hdr;
        }

        if (bit_buffer->m_bit_count != 0) {
            if (stream.isal_stream_ptr_->avail_out < 8) {
                return status_list::more_output_needed;
            }

            stream.reset_bit_buffer();

            for (uint32_t i = isal_state->count; i < deflate_header_count && !is_full(bit_buffer); i++) {
                write_bits(bit_buffer, deflate_header[i], byte_bit_size);
            }

            stream.isal_stream_ptr_->next_out = buffer_ptr(bit_buffer);
            count = buffer_used(bit_buffer);
        } else {
            uint8_t *rest_or_deflate_header = deflate_header + isal_state->count;
            util::copy(rest_or_deflate_header, rest_or_deflate_header + count, stream.isal_stream_ptr_->next_out);

            stream.isal_stream_ptr_->next_out += count;
        }

        if (!stream.isal_stream_ptr_->end_of_stream && isal_state->count == 0) {
            /* Assumes the final block bit is the first bit */
            deflate_header[0] ^= 0x1;
        }

        stream.isal_stream_ptr_->avail_out -= count;
        stream.isal_stream_ptr_->total_out += count;
        isal_state->count += count;

        count = deflate_header_count - isal_state->count;
    } else if (!stream.isal_stream_ptr_->end_of_stream && deflate_header_count == 0) {
        /* Assumes the final block bit is the first bit */
        hdr_extra_bits ^= 0x1;
        isal_state->has_eob_hdr = !isal_state->has_eob_hdr;
    }

    if ((count == 0) && (stream.isal_stream_ptr_->avail_out >= 8)) {

        stream.reset_bit_buffer();
        write_bits(bit_buffer, hdr_extra_bits, deflate_header_extra_bits);

        isal_state->count = 0;

        stream.dump_bit_buffer();
    }

    state = compression_state_t::compression_body;

    return status_list::ok;
}

auto slow_deflate_body(deflate_state<execution_path_t::software> &stream, compression_state_t &state) noexcept -> qpl_ml_status {
    auto isal_state = &stream.isal_stream_ptr_->internal_state;
    auto bit_buffer = &isal_state->bitbuf;

    stream.reset_bit_buffer();

    uint32_t bytes_processed = qplc_slow_deflate_body()(stream.isal_stream_ptr_->next_in,
                                                 stream.isal_stream_ptr_->next_in - stream.isal_stream_ptr_->total_in,
                                                 stream.isal_stream_ptr_->next_in + stream.isal_stream_ptr_->avail_in,
                                                 &stream.hash_table_,
                                                 stream.isal_stream_ptr_->hufftables,
                                                 bit_buffer);

    isal_state->block_end = isal_state->block_end + bytes_processed;

    stream.isal_stream_ptr_->next_in += bytes_processed;
    stream.isal_stream_ptr_->avail_in -= bytes_processed;
    stream.isal_stream_ptr_->total_in += bytes_processed;

    if (is_full(bit_buffer)) {
        if (stream.is_first_chunk() &&
            stream.is_last_chunk() &&
            stream.mini_blocks_support() == mini_blocks_support_t::disabled) {
                state = compression_state_t::write_stored_block;
                return status_list::ok;
            }

            return status_list::more_output_needed;
    } else {
        stream.dump_bit_buffer();
    }

    if (stream.isal_stream_ptr_->end_of_stream && stream.mini_blocks_support() == mini_blocks_support_t::disabled) {
        auto status = write_end_of_block(stream, state);

        if (stream.is_first_chunk() && !(stream.compression_mode() == canned_mode) &&
            (stream.isal_stream_ptr_->total_out > get_stored_blocks_size(stream.source_size_) ||
             status == status_list::more_output_needed)) {
            state = compression_state_t::write_stored_block;

            status = status_list::ok;
        } else {
            state = compression_state_t::finish_deflate_block;
        }

        if (status) {
            return status;
        }

    } else {
        state = compression_state_t::flush_bit_buffer;
    }

    return status_list::ok;
}

auto deflate_body(deflate_state<execution_path_t::software> &stream, compression_state_t &state) noexcept -> qpl_ml_status {
    stream.reset_bit_buffer();

    isal_deflate_body(stream.isal_stream_ptr_);

    if (is_full(&stream.isal_stream_ptr_->internal_state.bitbuf)) {
        return status_list::more_output_needed;
    }

    if (stream.isal_stream_ptr_->internal_state.state == ZSTATE_FLUSH_READ_BUFFER) {
        state = compression_state_t::compress_rest_data;
    } else {
        if (stream.is_first_chunk() &&
            stream.is_last_chunk() &&
            stream.mini_blocks_support() == mini_blocks_support_t::disabled) {
            state = compression_state_t::write_stored_block;
            return status_list::ok;
        }

        return status_list::more_output_needed;
    }

    return status_list::ok;
}

auto deflate_finish(deflate_state<execution_path_t::software> &stream, compression_state_t &state) noexcept -> qpl_ml_status {
    stream.reset_bit_buffer();
    isal_deflate_finish(stream.isal_stream_ptr_);

    auto status = status_list::ok;

    if (is_full(&stream.isal_stream_ptr_->internal_state.bitbuf) ||
        stream.isal_stream_ptr_->avail_in) {
        status = status_list::more_output_needed;
    }

    if (stream.is_last_chunk() && stream.mini_blocks_support() == mini_blocks_support_t::disabled) {
        if (status_list::ok == status) {
            status = write_end_of_block(stream, state);
        }

        if (stream.is_first_chunk() && !(stream.compression_mode() == canned_mode) &&
            (stream.isal_stream_ptr_->total_out > get_stored_blocks_size(stream.source_size_) ||
             status == status_list::more_output_needed)) {
            state = compression_state_t::write_stored_block;

            status = status_list::ok;
        } else {
            state = compression_state_t::finish_deflate_block;
        }
    } else {
        state = compression_state_t::flush_bit_buffer;
    }

    return status;
}

auto write_end_of_block(deflate_state<execution_path_t::software> &stream, 
                        compression_state_t &UNREFERENCED_PARAMETER(state)) noexcept -> qpl_ml_status {
    auto isal_state = &stream.isal_stream_ptr_->internal_state;
    auto bit_buffer = &isal_state->bitbuf;

    stream.reset_bit_buffer();

    stream.write_mini_block_index();

    // Write End Of Block
    uint64_t literal_code        = 0u;
    uint32_t literal_code_length = 0u;

    get_literal_code(stream.isal_stream_ptr_->hufftables,
                     end_of_block_code_index,
                     &literal_code,
                     &literal_code_length);

    if (bit_buffer->m_out_buf <= bit_buffer->m_out_end) {
        write_bits(bit_buffer, literal_code, literal_code_length);
    } else {
        if ((bit_buffer->m_bit_count + literal_code_length) <= 64) {
            bit_buffer->m_bits |= literal_code << bit_buffer->m_bit_count;
            bit_buffer->m_bit_count += literal_code_length;
            while ((bit_buffer->m_bit_count > 0) &&
                (bit_buffer->m_out_buf < (bit_buffer->m_out_end + 8))) {
                *bit_buffer->m_out_buf++ = (uint8_t)bit_buffer->m_bits;
                bit_buffer->m_bits >>= 8;
                bit_buffer->m_bit_count = (bit_buffer->m_bit_count >= 8) ? bit_buffer->m_bit_count - 8 : 0;
            }
        }
    }

    if (is_full(bit_buffer)) {
        return status_list::more_output_needed;
    }

    stream.dump_bit_buffer();

    return status_list::ok;
}

auto process_by_mini_blocks_body(deflate_state<execution_path_t::software> &stream,
                                 compression_state_t &state) noexcept -> qpl_ml_status {
    auto implementation = build_implementation<block_type_t::mini_block>(stream.compression_level(),
                                                                         stream.compression_mode(),
                                                                         mini_blocks_support_t::disabled,
                                                                         dictionary_support_t::disabled);
    
    auto source_begin = stream.source_begin_ptr_;
    auto source_size  = stream.source_size_;

    qpl_ml_status status = status_list::ok;
    
    auto compress_block = [&] (uint8_t *source_begin, uint32_t source_size) -> void {
        compression_state_t state = compression_state_t::init_compression;

        stream.set_source(source_begin, source_size);

        stream.write_mini_block_index();

        do {
            status = implementation.execute(stream, state);
        } while (!status && state != compression_state_t::finish_compression_process);

        if (!status) {
            stream.update_checksum(source_begin, source_size);
        }

        stream.dump_isal_stream();
    };

    auto mini_block_size             = bytes_per_mini_block(stream.mini_block_size());
    auto complete_mini_blocks_number = get_complete_mini_blocks_number(source_size, stream.mini_block_size());
    auto last_mini_block_size        = source_size % mini_block_size;
    auto current_mini_block_begin    = source_begin;

    for (uint32_t index = 0; index < complete_mini_blocks_number; index++) {
        compress_block(current_mini_block_begin, mini_block_size);

        if (status) {
            return status;
        }

        current_mini_block_begin += mini_block_size;
    }

    if (last_mini_block_size) {
        compress_block(current_mini_block_begin, last_mini_block_size);
    }

    stream.restore_isal_stream();

    if (stream.is_last_chunk()) {
        auto status = write_end_of_block(stream, state);

        if (status) {
            return status;
        }

        state = compression_state_t::finish_deflate_block;
    } else {
        state = compression_state_t::flush_bit_buffer;
    }

    return status;
}

auto build_huffman_table(deflate_state<execution_path_t::software> &stream, compression_state_t &state) noexcept -> qpl_ml_status {
    auto status = preprocess_static_block(stream, state);
    
    auto isal_state = &stream.isal_stream_ptr_->internal_state;
    
    isal_huff_histogram *histogram = reinterpret_cast<isal_huff_histogram *>(isal_state->buffer);

    isal_update_histogram(stream.isal_stream_ptr_->next_in, stream.isal_stream_ptr_->avail_in, histogram);
    isal_create_hufftables(stream.isal_stream_ptr_->hufftables, histogram);

    state = compression_state_t::start_new_block;

    return status;
}

auto preprocess_static_block(deflate_state<execution_path_t::software> &stream, compression_state_t &state) noexcept -> qpl_ml_status {
    if (!stream.is_first_chunk() && 
        stream.compression_mode() != canned_mode &&
        stream.should_start_new_block()) {
        auto status = write_end_of_block(stream, state);
        
        if (status) {
            return status;
        }
    }

    state = compression_state_t::start_new_block;

    return status_list::ok;
}

auto skip_header(deflate_state<execution_path_t::software> &UNREFERENCED_PARAMETER(stream), 
                 compression_state_t &state) noexcept -> qpl_ml_status {
    state = compression_state_t::compression_body;

    return status_list::ok;
}

auto skip_preprocessing(deflate_state<execution_path_t::software> &UNREFERENCED_PARAMETER(stream), 
                        compression_state_t &state) noexcept -> qpl_ml_status {
    state = compression_state_t::start_new_block;

    return status_list::ok;
}

} // namespace qpl::ml::compression
