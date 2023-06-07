/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

#include "auxiliary_units.hpp"

#include "util/memory.hpp"
#include "util/util.hpp"
#include "util/checksum.hpp"

#include "huffman.h"
#include "igzip_lib.h"

#include "deflate_slow_utils.h"

#include "qplc_deflate_utils.h"
#include "dispatcher/dispatcher.hpp"

#include "compression/huffman_only/huffman_only_compression_state.hpp"
#include "compression/deflate/implementations/deflate_implementation.hpp"

extern "C" {
extern void isal_deflate_hash(struct isal_zstream *stream, uint8_t *dict, uint32_t dict_len);
}

static inline qplc_slow_deflate_body_t_ptr slow_deflate_body() {
    return (qplc_slow_deflate_body_t_ptr)(qpl::ml::dispatcher::kernels_dispatcher::get_instance().get_deflate_fix_table()[0]);
}

static inline qplc_setup_dictionary_t_ptr qplc_setup_dictionary() {
    return (qplc_setup_dictionary_t_ptr)(qpl::ml::dispatcher::kernels_dispatcher::get_instance().get_setup_dictionary_table()[0]);
}

namespace qpl::ml::compression {

void huffman_only_state<execution_path_t::software>::reset_bit_buffer() noexcept {
    set_buf(&isal_stream_ptr_->internal_state.bitbuf, isal_stream_ptr_->next_out, isal_stream_ptr_->avail_out);
}

void huffman_only_state<execution_path_t::software>::dump_bit_buffer() noexcept {
    auto isal_state = &isal_stream_ptr_->internal_state;

    uint32_t bytes = buffer_used(&isal_state->bitbuf);

    isal_stream_ptr_->next_out = buffer_ptr(&isal_state->bitbuf);
    isal_stream_ptr_->avail_out -= bytes;
    isal_stream_ptr_->total_out += bytes;
}

template <typename stream_t>
auto init_compression(stream_t &stream, compression_state_t &state) noexcept -> qpl_ml_status {
    auto isal_state = &stream.isal_stream_ptr_->internal_state;
    auto bit_buffer = &isal_state->bitbuf;
    stream.isal_stream_ptr_->flush = QPL_PARTIAL_FLUSH;

    if constexpr (std::is_same_v<deflate_state<execution_path_t::software>, stream_t>) {
        stream.isal_stream_ptr_->end_of_stream = stream.is_last_chunk();
    } else {
        stream.isal_stream_ptr_->end_of_stream = true;
    }

    isal_state->block_next  = stream.isal_stream_ptr_->total_in;
    isal_state->block_end   = stream.isal_stream_ptr_->total_in;
    isal_state->has_eob_hdr = 0;
    isal_state->crc         = 0;

    isal_state->has_level_buf_init = 0;

    stream.isal_stream_ptr_->level = stream.compression_mode_ == dynamic_mode ? 3 : 0;
    isal_state->hash_mask          = stream.compression_mode_ == dynamic_mode ? LVL3_HASH_MASK : LVL0_HASH_MASK;

    if constexpr (std::is_same_v<stream_t, deflate_state<execution_path_t::software>>) {
        if (stream.mini_blocks_support() == mini_blocks_support_t::enabled) {
            isal_state->hash_mask          = LVL0_HASH_MASK;
            stream.isal_stream_ptr_->level = 0;
        }
    }

    if (isal_state->hash_mask > 2 * stream.isal_stream_ptr_->avail_in && stream.isal_stream_ptr_->end_of_stream) {
        isal_state->hash_mask = (1 << bsr(stream.isal_stream_ptr_->avail_in)) - 1;
    }

    if (stream.compression_mode_ == dynamic_mode) {
            uint32_t stored_len = (stream.isal_stream_ptr_->avail_in == 0) ?
                stored_block_header_length :
                stored_block_header_length * ((stream.isal_stream_ptr_->avail_in + stored_block_max_length - 1) /
                                            stored_block_max_length
                ) + stream.isal_stream_ptr_->avail_in;
        stored_len += buffer_used(bit_buffer);

        if constexpr (std::is_same_v<deflate_state<execution_path_t::software>, stream_t>) {
            if (stream.mini_blocks_support() == mini_blocks_support_t::disabled) {
                stored_len = !stream.is_last_chunk() ? stored_len : stored_len + bit_buffer_slope_bytes * 2;

                if (stream.isal_stream_ptr_->avail_out >= stored_len) {
                    stream.isal_stream_ptr_->avail_out = stored_len;
                }
            }
        } else {
            stored_len += bit_buffer_slope_bytes * 2;

            if (stream.isal_stream_ptr_->avail_out >= stored_len) {
                stream.isal_stream_ptr_->avail_out = stored_len;
            }
        }
    }

    isal_state->count = 0;

    if constexpr (std::is_same_v<deflate_state<execution_path_t::software>, stream_t>) {
        stream.reset_match_history();
    }

    state = compression_state_t::start_new_block;

    if constexpr (std::is_same_v<deflate_state<execution_path_t::software>, stream_t>) {
        if (stream.compression_mode_ != dynamic_mode || 
            stream.mini_blocks_support() == mini_blocks_support_t::enabled) {
            state = compression_state_t::preprocess_new_block;
        }
    }

    return status_list::ok;
}

template
auto init_compression<deflate_state<execution_path_t::software>>(deflate_state<execution_path_t::software> &stream,
                                                                 compression_state_t &state) noexcept -> qpl_ml_status;

template
auto init_compression<huffman_only_state<execution_path_t::software>>(huffman_only_state<execution_path_t::software> &stream,
                                                                      compression_state_t &state) noexcept -> qpl_ml_status;

template <typename stream_t>
auto finish_deflate_block(stream_t &stream, compression_state_t &state) noexcept -> qpl_ml_status {
    auto isal_state = &stream.isal_stream_ptr_->internal_state;
    auto bit_buffer = &isal_state->bitbuf;

    if (stream.isal_stream_ptr_->avail_out >= bit_buffer_slope_bytes) {
        stream.reset_bit_buffer();
    }

    if (!isal_state->has_eob_hdr) {
        /* If the final header has not been written, write a
         * final block. This block is a static huffman block
         * which only contains the end of block symbol. The code
         * that happens to do this is the fist 10 bits of
         * 0x003 */
        if (stream.isal_stream_ptr_->avail_out < bit_buffer_slope_bytes) {
            return status_list::more_output_needed;
        }

        isal_state->has_eob_hdr = 1;
        write_bits(bit_buffer, 0x003, 10);
        if (is_full(bit_buffer)) {
            stream.dump_bit_buffer();

            return status_list::more_output_needed;
        }
    }

    if (bit_buffer->m_bit_count) {
        /* the flush() will pad to the next byte and write up to 8 bytes
         * to the output stream/buffer.
         */
        if (stream.isal_stream_ptr_->avail_out < bit_buffer_slope_bytes) {
            return status_list::more_output_needed;
        }

        flush(bit_buffer);

        stream.dump_bit_buffer();
    } else if (bit_buffer->m_out_buf != nullptr && buffer_used(bit_buffer)) {
        stream.dump_bit_buffer();
    }

    if constexpr (std::is_same_v<deflate_state<execution_path_t::software>, stream_t>) {
        if (stream.mini_blocks_support() == mini_blocks_support_t::enabled &&
            stream.compression_mode() != canned_mode) {
            stream.write_mini_block_index();
        }
    }

    state = compression_state_t::finish_compression_process;

    return status_list::ok;
}

template
auto finish_deflate_block<deflate_state<execution_path_t::software>>(deflate_state<execution_path_t::software> &stream,
                                                                     compression_state_t &state) noexcept -> qpl_ml_status;

template
auto finish_deflate_block<huffman_only_state<execution_path_t::software>>(huffman_only_state<execution_path_t::software> &stream,
                                                                          compression_state_t &state) noexcept -> qpl_ml_status;

template<class stream_t>
constexpr bool is_last_chunk(stream_t &stream) {
    if constexpr(std::is_same_v<deflate_state<execution_path_t::software>, stream_t>) {
        return stream.is_last_chunk();
    } else {
        return true;
    }
}

template <typename stream_t>
auto flush_bit_buffer(stream_t &stream, compression_state_t &state) noexcept -> qpl_ml_status {
    auto isal_state = &stream.isal_stream_ptr_->internal_state;
    auto bit_buffer = &isal_state->bitbuf;

    uint64_t bits_to_write = 0xFFFF0000;
    uint32_t bits_length   = 0;
    int      flush_size    = 0;

    if (is_last_chunk(stream)) {
        if (stream.isal_stream_ptr_->avail_out < bit_buffer_slope_bytes && bit_buffer->m_bit_count) {
            return status_list::more_output_needed;
        }

        stream.reset_bit_buffer();

        flush_size  = static_cast<int>(static_cast<uint64_t>(-static_cast<int>(bit_buffer->m_bit_count + 3))
                                       % byte_bit_size);

        bits_to_write <<= flush_size + 3;
        bits_length = uint32_bit_size + flush_size + 3;

        write_bits(bit_buffer, bits_to_write, bits_length);

        stream.dump_bit_buffer();
    } else {
        if constexpr(std::is_same_v<stream_t, deflate_state<execution_path_t::software>>) {
            util::copy(reinterpret_cast<uint8_t *>(bit_buffer),
                       reinterpret_cast<uint8_t *>(bit_buffer) + sizeof(*bit_buffer),
                       reinterpret_cast<uint8_t *>(stream.bit_buffer_ptr));
        }
    }

    state = compression_state_t::finish_compression_process;
    isal_state->has_eob = 0;

    return status_list::ok;
}

template
auto flush_bit_buffer<deflate_state<execution_path_t::software>>(deflate_state<execution_path_t::software> &stream,
                                                                 compression_state_t &state) noexcept -> qpl_ml_status;

template
auto flush_bit_buffer<huffman_only_state<execution_path_t::software>>(huffman_only_state<execution_path_t::software> &stream,
                                                                      compression_state_t &state) noexcept -> qpl_ml_status;

template <typename stream_t>
auto flush_write_buffer(stream_t &stream, compression_state_t &state) noexcept -> qpl_ml_status {
    auto isal_state = &stream.isal_stream_ptr_->internal_state;
    auto bit_buffer = &isal_state->bitbuf;

    if (stream.isal_stream_ptr_->avail_out >= bit_buffer_slope_bytes) {
        stream.reset_bit_buffer();

        flush(bit_buffer);

        stream.dump_bit_buffer();

        state = compression_state_t::finish_compression_process;
    }

    return is_full(bit_buffer)
           ? status_list::more_output_needed
           : status_list::ok;
}

auto skip_rest_units(deflate_state<execution_path_t::software> &UNREFERENCED_PARAMETER(stream), 
                     compression_state_t &state) noexcept -> qpl_ml_status {
    state = compression_state_t::finish_compression_process;
    
    return status_list::ok;
}

template
auto flush_write_buffer<deflate_state<execution_path_t::software>>(deflate_state<execution_path_t::software> &stream,
                                                                   compression_state_t &state) noexcept -> qpl_ml_status;

template
auto flush_write_buffer<huffman_only_state<execution_path_t::software>>(huffman_only_state<execution_path_t::software> &stream,
                                                                        compression_state_t &state) noexcept -> qpl_ml_status;

auto update_checksum(deflate_state<execution_path_t::software> &stream) noexcept -> qpl_ml_status {
    stream.update_checksum(stream.source_begin_ptr_,
                           static_cast<uint32_t>(stream.isal_stream_ptr_->next_in - stream.source_begin_ptr_));

    return status_list::ok;
}

auto finish_compression_process(deflate_state<execution_path_t::software> &stream) noexcept -> qpl_ml_status {
    stream.dump_isal_stream();

    return status_list::ok;
}

void update_hash(deflate_state<execution_path_t::software> &stream, uint8_t *dictionary_ptr, uint32_t dictionary_size) noexcept {
    if (stream.compression_level() == high_level) {
        qplc_setup_dictionary()(dictionary_ptr, dictionary_size, stream.hash_table());
    } else {
        isal_deflate_hash(stream.isal_stream_ptr_, dictionary_ptr, dictionary_size);
    }
}

auto get_history_size(deflate_state<execution_path_t::software> &stream,
                      uint8_t *start_in,
                      int32_t buffered_history_start) noexcept -> uint32_t {
    isal_zstate *isal_state      = &stream.isal_stream_ptr_->internal_state;
    uint32_t    input_history    = 0;
    uint32_t    buffered_history = 0;

    buffered_history = (isal_state->has_hist) ? isal_state->b_bytes_processed - buffered_history_start : 0;
    input_history    = static_cast<uint32_t>(stream.isal_stream_ptr_->next_in - start_in);

    /* Calculate history required for deflate window */
    uint32_t history_size     = (buffered_history >= input_history) ? buffered_history : input_history;
    uint32_t max_history_size = (1 << stream.isal_stream_ptr_->hist_bits);
    if (history_size > max_history_size) {
        history_size = max_history_size;
    }

    return history_size;
}

auto deflate_body_with_dictionary(deflate_state<execution_path_t::software> &stream,
                                  compression_state_t &state) noexcept -> qpl_ml_status {
    isal_zstate   *isal_state    = &stream.isal_stream_ptr_->internal_state;
    qpl_ml_status status         = status_list::ok;
    bool          internal       = false;
    uint32_t      buffered_size  = isal_state->b_bytes_valid - isal_state->b_bytes_processed;
    uint8_t       *start_in      = stream.isal_stream_ptr_->next_in;
    uint32_t      total_start    = stream.isal_stream_ptr_->total_in;
    uint8_t       *next_in       = stream.isal_stream_ptr_->next_in;
    uint32_t      avail_in       = stream.isal_stream_ptr_->avail_in;
    int32_t       buf_hist_start = 0;
    uint32_t      size           = 0;

    update_hash(stream, isal_state->buffer, isal_state->b_bytes_processed);
    uint32_t history_size = get_history_size(stream, start_in, buf_hist_start);

    do {
        uint8_t *buf_start_in = nullptr;
        
        internal = false;
        if (stream.isal_stream_ptr_->total_in - total_start < history_size + buffered_size) {
            internal = true;

            if (isal_state->b_bytes_processed > history_size) {
                uint32_t copy_start_offset = isal_state->b_bytes_processed - history_size;

                auto copy_down_src  = &isal_state->buffer[copy_start_offset];
                auto copy_down_size = isal_state->b_bytes_valid - copy_start_offset;
                
                util::copy(copy_down_src, copy_down_src + copy_down_size, isal_state->buffer);

                auto start_offset = static_cast<uint32_t>(copy_down_src - isal_state->buffer);

                isal_state->b_bytes_valid     -= start_offset;
                isal_state->b_bytes_processed -= start_offset;
                buf_hist_start                -= start_offset;

                if (buf_hist_start < 0) {
                    buf_hist_start = 0;
                }

                stream.isal_stream_ptr_->next_in  += size;
                stream.isal_stream_ptr_->avail_in -= size;
                stream.isal_stream_ptr_->total_in += size;
                isal_state->b_bytes_valid         += size;
                buffered_size                     += size;

                next_in  = stream.isal_stream_ptr_->next_in;
                avail_in = stream.isal_stream_ptr_->avail_in;
            }

            size = stream.isal_stream_ptr_->avail_in;
            if (size > sizeof(isal_state->buffer) - isal_state->b_bytes_valid) {
                size = sizeof(isal_state->buffer) - isal_state->b_bytes_valid;
            }

            util::copy(stream.isal_stream_ptr_->next_in,
                       stream.isal_stream_ptr_->next_in + size,
                       &isal_state->buffer[isal_state->b_bytes_valid]);

            stream.isal_stream_ptr_->next_in  += size;
            stream.isal_stream_ptr_->avail_in -= size;
            stream.isal_stream_ptr_->total_in += size;
            isal_state->b_bytes_valid         += size;
            buffered_size                     += size;

            next_in  = stream.isal_stream_ptr_->next_in;
            avail_in = stream.isal_stream_ptr_->avail_in;

            if (avail_in) {
                stream.isal_stream_ptr_->flush         = NO_FLUSH;
                stream.isal_stream_ptr_->end_of_stream = 0;
            }

            stream.isal_stream_ptr_->next_in   = &isal_state->buffer[isal_state->b_bytes_processed];
            stream.isal_stream_ptr_->avail_in  = buffered_size;
            stream.isal_stream_ptr_->total_in -= buffered_size;

            buf_start_in = isal_state->buffer;
        } else if (buffered_size) {
            stream.isal_stream_ptr_->next_in  -= buffered_size;
            stream.isal_stream_ptr_->avail_in += buffered_size;
            stream.isal_stream_ptr_->total_in -= buffered_size;

            isal_state->b_bytes_valid     = 0;
            isal_state->b_bytes_processed = 0;
            buffered_size                 = 0;
        }

        uint8_t *next_in_pre = stream.isal_stream_ptr_->next_in;

        auto implementation = build_implementation<block_type_t::deflate_block>(stream.compression_level(),
                                                                                stream.compression_mode(),
                                                                                stream.mini_blocks_support(),
                                                                                dictionary_support_t::disabled);
        auto internal_state = compression_state_t::compression_body;

        status = implementation.execute(stream, internal_state);
        if (!status && internal_state == compression_state_t::compress_rest_data) {
            status = implementation.execute(stream, internal_state);
        }

        uint32_t processed = static_cast<uint32_t>(stream.isal_stream_ptr_->next_in - next_in_pre);
        history_size = get_history_size(stream, buf_start_in, buf_hist_start);

        /* Restore compression to unbuffered input when compressing to internal buffer */
        if (internal) {
            isal_state->b_bytes_processed += processed;
            buffered_size                 -= processed;

            stream.isal_stream_ptr_->end_of_stream = stream.is_last_chunk();
            stream.isal_stream_ptr_->total_in     += buffered_size;

            stream.isal_stream_ptr_->next_in  = next_in;
            stream.isal_stream_ptr_->avail_in = avail_in;
        }

        state = internal_state;
    } while (!status && internal && stream.isal_stream_ptr_->avail_in && stream.isal_stream_ptr_->avail_out);

    if (!internal) {
        stream.isal_stream_ptr_->next_in  -= buffered_size;
        stream.isal_stream_ptr_->avail_in += buffered_size;
        stream.isal_stream_ptr_->total_in -= buffered_size;

        memmove(isal_state->buffer, stream.isal_stream_ptr_->next_in - history_size, history_size);
        isal_state->b_bytes_processed = history_size;
        isal_state->b_bytes_valid     = history_size;
        buffered_size                 = 0;
    }

    return status;
}

} // namespace qpl::ml::compression
