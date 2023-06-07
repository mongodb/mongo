/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

#include "deflate_state_builder.hpp"

#include "igzip_lib.h"

extern "C" {
extern const struct isal_hufftables hufftables_static;
}

namespace qpl::ml::compression {

auto deflate_state_builder<execution_path_t::software>::output(uint8_t *buffer_ptr, uint32_t buffer_size) noexcept -> common_type & {
    stream_.isal_stream_ptr_->next_out  = buffer_ptr;
    stream_.isal_stream_ptr_->avail_out = buffer_size;

    return *this;
}

auto deflate_state_builder<execution_path_t::software>::set_isal_internal_buffers(uint8_t *const level_buffer_ptr,
                                                                                  const uint32_t level_buffer_size,
                                                                                  isal_hufftables *const huffman_tables_ptr,
                                                                                  BitBuf2 *const bit_buffer_ptr) noexcept -> void {
    stream_.isal_stream_ptr_->level_buf      = level_buffer_ptr;
    stream_.isal_stream_ptr_->level_buf_size = level_buffer_size;
    stream_.isal_huffman_table_ptr_          = huffman_tables_ptr;

    stream_.isal_stream_ptr_->hufftables = stream_.isal_huffman_table_ptr_;

    auto level_buffer = reinterpret_cast<level_buf *>(stream_.isal_stream_ptr_->level_buf);

    stream_.huffman_table_icf_.init_isal_huffman_tables(&level_buffer->encode_tables);

    if (!stream_.is_first_chunk()) {
        util::copy(reinterpret_cast<uint8_t *>(bit_buffer_ptr),
                   reinterpret_cast<uint8_t *>(bit_buffer_ptr) + sizeof(BitBuf2),
                   reinterpret_cast<uint8_t *>(&(&stream_.isal_stream_ptr_->internal_state)->bitbuf));
    }
}

auto deflate_state_builder<execution_path_t::software>::compression_level(compression_level_t level) noexcept -> common_type & {
    stream_.level_ = level;

    return *reinterpret_cast<common_type *>(this);
}

auto deflate_state_builder<execution_path_t::software>::start_new_block(bool UNREFERENCED_PARAMETER(value)) noexcept -> common_type & {
    stream_.start_new_block_ = true;

    return *reinterpret_cast<common_type *>(this);
}

void deflate_state_builder<execution_path_t::software>::set_huffman_table(qpl_compression_huffman_table *huffman_table) noexcept {
    stream_.isal_stream_ptr_->hufftables = stream_.isal_huffman_table_ptr_;
    stream_.compression_table_ = huffman_table;

    const uint8_t *const isal_huffman_table = get_isal_compression_huffman_table_ptr(huffman_table);

    util::copy(isal_huffman_table,
               isal_huffman_table + sizeof(isal_hufftables),
               reinterpret_cast<uint8_t *>(stream_.isal_stream_ptr_->hufftables));
}

auto deflate_state_builder<execution_path_t::software>::load_current_position(uint32_t total_bytes_written) noexcept -> common_type & {
    stream_.total_bytes_written_ = total_bytes_written;

    return *reinterpret_cast<common_type *>(this);
}

void deflate_state_builder<execution_path_t::software>::init() noexcept {
    if (stream_.compression_mode() == fixed_mode) {
        stream_.isal_stream_ptr_->hufftables = const_cast<isal_hufftables *>(&hufftables_static);
    }

    auto isal_state = &stream_.isal_stream_ptr_->internal_state;

    stream_.isal_stream_ptr_->flush         = QPL_PARTIAL_FLUSH;
    stream_.isal_stream_ptr_->end_of_stream = stream_.is_last_chunk();

    stream_.isal_stream_ptr_->hist_bits = isal_history_size_boundary;
    isal_state->mb_mask                 = (1u << (qpl_mblk_size_32k + 8u)) - 1;

    isal_state->dist_mask = util::build_mask<uint32_t>(stream_.isal_stream_ptr_->hist_bits);
    isal_state->hash_mask = stream_.compression_mode_ == dynamic_mode ? LVL3_HASH_MASK : LVL1_HASH_MASK;

    if (stream_.mini_blocks_support() == mini_blocks_support_t::enabled &&
        stream_.compression_mode_ == dynamic_mode) {
        util::set_zeros(isal_state->buffer, sizeof(isal_huff_histogram));

        stream_.start_new_block_ = true;
    }
}

auto deflate_state_builder<execution_path_t::software>::dictionary(qpl_dictionary &dictionary) noexcept -> common_type & {
    isal_deflate_set_dict(stream_.isal_stream_ptr_,
                          get_dictionary_data(dictionary),
                          static_cast<uint32_t>(dictionary.raw_dictionary_size));
    stream_.isal_stream_ptr_->internal_state.max_dist = static_cast<uint32_t>(dictionary.raw_dictionary_size);
    stream_.dictionary_support_ = dictionary_support_t::enabled;

    return *reinterpret_cast<common_type *>(this);
};

} // namespace qpl::ml::compression
