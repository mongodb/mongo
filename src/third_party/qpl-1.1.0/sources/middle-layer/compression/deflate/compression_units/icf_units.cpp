/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

#include "icf_units.hpp"

#include "util/util.hpp"
#include "util/memory.hpp"

#include "dispatcher/dispatcher.hpp"
#include "qplc_deflate_utils.h"

#include "deflate_slow_icf.h"

#include "igzip_lib.h"
#include "bitbuf2.h"

extern "C" {
extern void isal_deflate_icf_body_lvl3(struct isal_zstream *);
extern void isal_deflate_icf_finish_lvl3(struct isal_zstream *);
}

static inline qplc_slow_deflate_icf_body_t_ptr qplc_slow_deflate_icf_body() {
    return (qplc_slow_deflate_icf_body_t_ptr)(qpl::ml::dispatcher::kernels_dispatcher::get_instance().get_deflate_table()[0]);
}

namespace qpl::ml::compression {

auto write_buffered_icf_header(deflate_state<execution_path_t::software> &stream, compression_state_t &state) noexcept -> qpl_ml_status {
    auto isal_state   = &stream.isal_stream_ptr_->internal_state;
    auto bit_buffer   = &isal_state->bitbuf;
    auto level_buffer = reinterpret_cast<level_buf *>(stream.isal_stream_ptr_->level_buf);

    uint8_t  *deflate_header      = level_buffer->deflate_hdr;
    uint32_t deflate_header_count = level_buffer->deflate_hdr_count;
    uint32_t extra_bits_count     = level_buffer->deflate_hdr_extra_bits;

    uint32_t header_extra_bits = deflate_header[deflate_header_count];
    uint32_t count             = deflate_header_count - isal_state->count;

    if (bit_buffer->m_bit_count != 0) {
        if (stream.isal_stream_ptr_->avail_out < bit_buffer_slope_bytes) {
            return status_list::more_output_needed;
        }

        stream.reset_bit_buffer();
        flush(bit_buffer);

        stream.dump_bit_buffer();
    }

    if (count != 0) {
        if (count > stream.isal_stream_ptr_->avail_out) {
            count = stream.isal_stream_ptr_->avail_out;
        }

        auto *header_begin = deflate_header + isal_state->count;

        util::copy(header_begin, header_begin + count, stream.isal_stream_ptr_->next_out);

        stream.isal_stream_ptr_->next_out += count;
        stream.isal_stream_ptr_->avail_out -= count;
        stream.isal_stream_ptr_->total_out += count;

        isal_state->count += count;

        count = deflate_header_count - isal_state->count;
    }

    if (count == 0 && stream.isal_stream_ptr_->avail_out >= bit_buffer_slope_bytes) {
        stream.reset_bit_buffer();
        write_bits(bit_buffer, header_extra_bits, extra_bits_count);

        isal_state->count = 0;

        stream.dump_bit_buffer();
    } else {
        return status_list::more_output_needed;
    }

    state = compression_state_t::flush_icf_buffer;

    return status_list::ok;
}

auto create_icf_block_header(deflate_state<execution_path_t::software> &stream, compression_state_t &state) noexcept -> qpl_ml_status {
    auto isal_state   = &stream.isal_stream_ptr_->internal_state;
    auto bit_buffer   = &isal_state->bitbuf;
    auto level_buffer = reinterpret_cast<level_buf *>(stream.isal_stream_ptr_->level_buf);

    BitBuf2 bit_writer_tmp{};

    uint32_t out_size      = stream.isal_stream_ptr_->avail_out;
    uint8_t  *end_out      = stream.isal_stream_ptr_->next_out + out_size;
    uint64_t block_in_size = isal_state->block_end - isal_state->block_next;

    int buffer_header = 0;

    uint64_t block_size = stored_block_header_length *
                          ((block_in_size + stored_block_max_length - 1) /
                          stored_block_max_length
                          ) + block_in_size;
    block_size          = block_size ? block_size : stored_block_header_length;

    util::copy(reinterpret_cast<uint8_t *>(bit_buffer),
               reinterpret_cast<uint8_t *>(bit_buffer) + sizeof(BitBuf2),
               reinterpret_cast<uint8_t *>(&bit_writer_tmp));

    /* Write EOB in icf_buf */
    level_buffer->hist.ll_hist[end_of_block_code_index] = 1;
    level_buffer->icf_buf_next->lit_len    = 0x100;
    level_buffer->icf_buf_next->lit_dist   = NULL_DIST_SYM;
    level_buffer->icf_buf_next->dist_extra = 0;

    level_buffer->icf_buf_next++;

    isal_state->has_eob_hdr = (stream.isal_stream_ptr_->end_of_stream && stream.are_buffers_empty()) ? 1 : 0;

    if (end_out - stream.isal_stream_ptr_->next_out >= ISAL_DEF_MAX_HDR_SIZE) {
        /* Assumes ISAL_DEF_MAX_HDR_SIZE is large enough to contain a
         * max length header and a gzip header */
        stream.reset_bit_buffer();
        buffer_header = 0;

    } else {
        /* Start writing into temporary buffer */
        set_buf(bit_buffer, level_buffer->deflate_hdr, ISAL_DEF_MAX_HDR_SIZE);
        buffer_header = 1;
    }

    prepare_histogram(&level_buffer->hist);

    build_huffman_table_icf(stream.huffman_table_icf_, &level_buffer->hist);

    uint64_t bit_count = write_huffman_table_icf(bit_buffer,
                                                 stream.huffman_table_icf_,
                                                 &level_buffer->hist,
                                                 stream.compression_mode(),
                                                 isal_state->has_eob_hdr);

    stream.huffman_table_icf_.expand_huffman_tables();

    /* Assumes that type 0 block has size less than 4G */
    uint32_t block_start_offset = (stream.isal_stream_ptr_->total_in - isal_state->block_next);
    uint8_t  *block_start       = stream.isal_stream_ptr_->next_in - block_start_offset;
    uint32_t avail_output       = stream.isal_stream_ptr_->avail_out + sizeof(isal_state->buffer) -
                                  (stream.isal_stream_ptr_->total_in - isal_state->block_end);

    if ((bit_count / byte_bit_size >= block_size ||
         (bit_count + bit_buffer_slope_bits) / byte_bit_size > stream.isal_stream_ptr_->avail_out
        ) &&
        block_start >= stream.source_begin() &&
        block_size <= avail_output) {
        /* Reset stream for writing out a stored block */
        isal_state->has_eob_hdr = 0;
        auto *bit_writer_tmp_ptr = reinterpret_cast<uint8_t *>(&bit_writer_tmp);
        util::copy(bit_writer_tmp_ptr,
                   bit_writer_tmp_ptr + sizeof(BitBuf2),
                   reinterpret_cast<uint8_t *>(bit_buffer));
        state = compression_state_t::write_stored_block;

    } else if (buffer_header) {
        /* Setup stream to write out a buffered header */
        level_buffer->deflate_hdr_count      = buffer_used(bit_buffer);
        level_buffer->deflate_hdr_extra_bits = bit_buffer->m_bit_count;

        flush(bit_buffer);
        util::copy(reinterpret_cast<uint8_t *>(&bit_writer_tmp),
                   reinterpret_cast<uint8_t *>(&bit_writer_tmp) + sizeof(BitBuf2),
                   reinterpret_cast<uint8_t *>(bit_buffer));

        bit_buffer->m_bits      = 0;
        bit_buffer->m_bit_count = 0;

        state = compression_state_t::write_buffered_icf_header;
    } else {
        stream.dump_bit_buffer();
        state = compression_state_t::flush_icf_buffer;
    }

    return status_list::ok;
}

auto flush_icf_block(deflate_state<execution_path_t::software> &stream, compression_state_t &state) noexcept -> qpl_ml_status {
    auto isal_state   = &stream.isal_stream_ptr_->internal_state;
    auto bit_buffer   = &isal_state->bitbuf;
    auto level_buffer = reinterpret_cast<level_buf *>(stream.isal_stream_ptr_->level_buf);

    stream.reset_bit_buffer();

    deflate_icf *icf_buf_encoded_next = encode_deflate_icf(level_buffer->icf_buf_start + isal_state->count,
                                                           level_buffer->icf_buf_next,
                                                           bit_buffer,
                                                           stream.huffman_table_icf_.get_isal_huffman_tables());

    isal_state->count = static_cast<uint32_t>(icf_buf_encoded_next - level_buffer->icf_buf_start);

    stream.dump_bit_buffer();

    if (level_buffer->icf_buf_next <= icf_buf_encoded_next) {
        isal_state->count = 0;
        if (stream.isal_stream_ptr_->avail_in == 0 && stream.isal_stream_ptr_->end_of_stream) {
            state = compression_state_t::finish_deflate_block;
        } else if (stream.isal_stream_ptr_->avail_in == 0 && stream.isal_stream_ptr_->flush != NO_FLUSH) {
            state = compression_state_t::flush_bit_buffer;
        } else {
            state = compression_state_t::start_new_block;
        }
    } else {
        return status_list::more_output_needed;
    }

    return status_list::ok;
}

auto init_new_icf_block(deflate_state<execution_path_t::software> &stream, compression_state_t &state) noexcept -> qpl_ml_status {
    auto isal_state   = &stream.isal_stream_ptr_->internal_state;
    auto level_buffer = reinterpret_cast<level_buf *>(stream.isal_stream_ptr_->level_buf);

    int level_struct_size = stream.init_level_buffer();

    isal_state->block_next = isal_state->block_end;

    level_buffer->icf_buf_start =
            reinterpret_cast<deflate_icf *>(stream.isal_stream_ptr_->level_buf + level_struct_size);

    level_buffer->icf_buf_next      = level_buffer->icf_buf_start;
    level_buffer->icf_buf_avail_out = stream.isal_stream_ptr_->level_buf_size - level_struct_size - sizeof(deflate_icf);

    util::set_zeros(reinterpret_cast<uint8_t *>(&level_buffer->hist), sizeof(isal_mod_hist));

    state = compression_state_t::compression_body;

    return status_list::ok;
}

auto deflate_icf_finish(deflate_state<execution_path_t::software> &stream, compression_state_t &state) noexcept -> qpl_ml_status {
    auto isal_state = &stream.isal_stream_ptr_->internal_state;

    isal_state->state = ZSTATE_FLUSH_READ_BUFFER;

    isal_deflate_icf_finish_lvl3(stream.isal_stream_ptr_);

    if (isal_state->state == ZSTATE_CREATE_HDR) {
        state = compression_state_t::create_icf_header;

        return status_list::ok;
    } else {
        return status_list::more_output_needed;
    }
}

auto deflate_icf_body(deflate_state<execution_path_t::software> &stream, compression_state_t &state) noexcept -> qpl_ml_status {
    auto isal_state = &stream.isal_stream_ptr_->internal_state;

    isal_state->state = ZSTATE_BODY;

    isal_deflate_icf_body_lvl3(stream.isal_stream_ptr_);

    if (isal_state->state == ZSTATE_CREATE_HDR) {
        state = compression_state_t::create_icf_header;
    } else if (isal_state->state == ZSTATE_FLUSH_READ_BUFFER) {
        state = compression_state_t::compress_rest_data;
    }

    return status_list::ok;
}

auto slow_deflate_icf_body(deflate_state<execution_path_t::software> &stream, compression_state_t &state) noexcept -> qpl_ml_status {
    auto level_buffer = reinterpret_cast<level_buf *>(stream.isal_stream_ptr_->level_buf);

    deflate_icf *icf_buffer_begin = level_buffer->icf_buf_next;
    deflate_icf *icf_buffer_end   = icf_buffer_begin + (level_buffer->icf_buf_avail_out / sizeof(deflate_icf));

    deflate_icf_stream icf_stream = {icf_buffer_begin, icf_buffer_begin, icf_buffer_end};

    uint32_t bytes_processed = qplc_slow_deflate_icf_body()(stream.isal_stream_ptr_->next_in,
                                                            stream.isal_stream_ptr_->next_in
                                                            - stream.isal_stream_ptr_->total_in,
                                                            stream.isal_stream_ptr_->next_in
                                                            + stream.isal_stream_ptr_->avail_in,
                                                            &stream.hash_table_,
                                                            &level_buffer->hist,
                                                            &icf_stream);

    stream.isal_stream_ptr_->internal_state.block_end =
            stream.isal_stream_ptr_->internal_state.block_end + bytes_processed;

    stream.isal_stream_ptr_->next_in += bytes_processed;
    stream.isal_stream_ptr_->avail_in -= bytes_processed;
    stream.isal_stream_ptr_->total_in += bytes_processed;

    level_buffer->icf_buf_next = icf_stream.next_ptr;
    level_buffer->icf_buf_avail_out -= static_cast<uint32_t>(icf_stream.next_ptr -
                                                             icf_stream.begin_ptr) * sizeof(deflate_icf);

    state = compression_state_t::create_icf_header;

    return status_list::ok;
}

} // namespace qpl::ml::compression
