/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

#include "sw_deflate_state.hpp"

#include "util/memory.hpp"
#include "util/util.hpp"
#include "deflate_hash_table.h"

namespace qpl::ml::compression {
void deflate_state<execution_path_t::software>::set_source(uint8_t *begin, uint32_t size) noexcept {
    isal_stream_ptr_->next_in  = begin;
    isal_stream_ptr_->avail_in = size;

    source_begin_ptr_ = begin;
    source_size_      = size;
}

auto deflate_state<execution_path_t::software>::write_bytes(const uint8_t *data, uint32_t size) noexcept -> qpl_ml_status {
    if (isal_stream_ptr_->avail_out < size) {
        return status_list::more_output_needed;
    }

    util::copy(data, data + size, isal_stream_ptr_->next_out);

    isal_stream_ptr_->avail_out -= size;
    isal_stream_ptr_->total_out += size;
    isal_stream_ptr_->next_out  += size;

    return status_list::ok;
}

void deflate_state<execution_path_t::software>::save_bit_buffer() noexcept {
    util::copy(reinterpret_cast<uint8_t *>(&isal_stream_ptr_->internal_state.bitbuf),
               reinterpret_cast<uint8_t *>(&isal_stream_ptr_->internal_state.bitbuf) + sizeof(BitBuf2),
               reinterpret_cast<uint8_t *>(bit_buffer_ptr));
}

void deflate_state<execution_path_t::software>::reset_match_history() noexcept {
    const auto &deflate_hash_table_reset = ((qplc_deflate_hash_table_reset_ptr)(qpl::ml::dispatcher::kernels_dispatcher::get_instance().get_deflate_table()[2]));

    auto level_buffer = reinterpret_cast<level_buf *>(isal_stream_ptr_->level_buf);
    
    if (compression_level() == high_level) {
        hash_table_.hash_table_ptr = reinterpret_cast<uint32_t *>(level_buffer->hash_map.hash_table);
        hash_table_.hash_story_ptr = hash_table_.hash_table_ptr + high_hash_table_size;

        if (isal_stream_ptr_->total_in == 0) {
            deflate_hash_table_reset(&hash_table_);

            hash_table_.hash_mask  = util::build_mask<uint32_t, 12u>();
            hash_table_.attempts   = 4096u;
            hash_table_.good_match = 32u;
            hash_table_.nice_match = 258u;
            hash_table_.lazy_match = 258u;
        }
    } else {
        auto isal_state   = &isal_stream_ptr_->internal_state;

        uint16_t *hash_table     = isal_stream_ptr_->level == 3 ? level_buffer->lvl3.hash_table :
                                                                  isal_state->head;
        uint32_t hash_table_size = 2 * (isal_state->hash_mask + 1);

        isal_state->has_hist = IGZIP_NO_HIST;

        if ((isal_stream_ptr_->total_in & 0xFFFF) == 0) {
            memset(hash_table, 0, hash_table_size);
        } else {
            for (uint32_t i = 0; i < hash_table_size / 2; i++) {
                hash_table[i] = static_cast<uint16_t>(isal_stream_ptr_->total_in);
            }
        }
    }
}

void deflate_state<execution_path_t::software>::reset_bit_buffer() noexcept {
    set_buf(&isal_stream_ptr_->internal_state.bitbuf, isal_stream_ptr_->next_out, isal_stream_ptr_->avail_out);
}

void deflate_state<execution_path_t::software>::dump_bit_buffer() noexcept {
    auto isal_state = &isal_stream_ptr_->internal_state;

    uint32_t bytes = buffer_used(&isal_state->bitbuf);

    isal_stream_ptr_->next_out = buffer_ptr(&isal_state->bitbuf);
    isal_stream_ptr_->avail_out -= bytes;
    isal_stream_ptr_->total_out += bytes;
}

void deflate_state<execution_path_t::software>::dump_isal_stream() noexcept {
    bytes_written_   += isal_stream_ptr_->total_out;
    bytes_processed_ += isal_stream_ptr_->total_in;
    
    isal_stream_ptr_->total_out = 0;
    isal_stream_ptr_->total_in  = 0;
}

void deflate_state<execution_path_t::software>::restore_isal_stream() noexcept {
    isal_stream_ptr_->total_out = bytes_written_;
    isal_stream_ptr_->total_in  = bytes_processed_;

    bytes_written_   = 0;
    bytes_processed_ = 0;
}

void deflate_state<execution_path_t::software>::write_mini_block_index() noexcept {
    index_table_.write_new_index(bits_written(), checksum_.crc32);
}

auto deflate_state<execution_path_t::software>::init_level_buffer() noexcept -> int {
    auto isal_state   = &isal_stream_ptr_->internal_state;
    auto level_buffer = reinterpret_cast<level_buf *>(isal_stream_ptr_->level_buf);

    if (!isal_state->has_level_buf_init) {
        level_buffer->hash_map.matches_next = level_buffer->hash_map.matches;
        level_buffer->hash_map.matches_end  = level_buffer->hash_map.matches;
    }

    isal_state->has_level_buf_init = 1;

    return sizeof(level_buf) - MAX_LVL_BUF_SIZE + sizeof(level_buffer->hash_map);
}

[[nodiscard]] auto deflate_state<execution_path_t::software>::source_begin() const noexcept -> uint8_t * {
    return source_begin_ptr_;
}

[[nodiscard]] auto deflate_state<execution_path_t::software>::should_start_new_block() const noexcept -> bool {
    return start_new_block_ || is_first_chunk();
}

[[nodiscard]] auto deflate_state<execution_path_t::software>::compression_level() const noexcept -> compression_level_t {
    return level_;
}

[[nodiscard]] auto deflate_state<execution_path_t::software>::are_buffers_empty() const noexcept -> bool {
    auto level_buffer = reinterpret_cast<level_buf *>(isal_stream_ptr_->level_buf);
    
    return (!isal_stream_ptr_->avail_in &&
            level_buffer->hash_map.matches_next >= level_buffer->hash_map.matches_end
    );
}

[[nodiscard]] auto deflate_state<execution_path_t::software>::mini_blocks_support() const noexcept -> mini_blocks_support_t {
    return mini_block_size_ != mini_block_size_none ?
           mini_blocks_support_t::enabled :
           mini_blocks_support_t::disabled;
}

[[nodiscard]] auto deflate_state<execution_path_t::software>::dictionary_support() const noexcept -> dictionary_support_t {
    return dictionary_support_;
}

[[nodiscard]] auto deflate_state<execution_path_t::software>::hash_table() noexcept -> deflate_hash_table_t * {
    return &hash_table_;
}

[[nodiscard]] auto deflate_state<execution_path_t::software>::bits_written() noexcept -> uint32_t {
    return byte_bit_size * (total_bytes_written_ + bytes_written_ + isal_stream_ptr_->total_out) +
           isal_stream_ptr_->internal_state.bitbuf.m_bit_count;
}

[[nodiscard]] auto deflate_state<execution_path_t::software>::crc() const noexcept -> uint32_t {
    return checksum_.crc32;
}

} // namespace qpl::ml::compression
