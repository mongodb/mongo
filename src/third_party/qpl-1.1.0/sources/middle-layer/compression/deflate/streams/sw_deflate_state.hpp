/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

#ifndef QPL_MIDDLE_LAYER_COMPRESSION_STREAMS_SW_DEFLATE_STATE_HPP
#define QPL_MIDDLE_LAYER_COMPRESSION_STREAMS_SW_DEFLATE_STATE_HPP

#include "common/linear_allocator.hpp"
#include "compression/deflate/deflate.hpp"
#include "compression/deflate/streams/compression_stream.hpp"
#include "compression/deflate/containers/huffman_table.hpp"
#include "compression/deflate/containers/index_table.hpp"
#include "compression/compression_defs.hpp"
#include "compression/deflate/utils/compression_defs.hpp"

#include "util/util.hpp"

#include "igzip_lib.h"
#include "igzip_level_buf_structs.h"

#include "deflate_hash_table.h"
#include "compression/huffman_table/huffman_table_utils.hpp"

namespace qpl::ml::compression {

/**
 * Size of internal buffer for the isal level buffer
 */
constexpr const uint32_t isal_level_buffer_size = 348160u;

template <>
class deflate_state<execution_path_t::software> final : public compression_stream {
    template <execution_path_t path>
    friend
    class deflate_state_builder;

    friend class gzip_decorator;

    friend class zlib_decorator;

    template <execution_path_t path, deflate_mode_t mode, class stream_t>
    friend auto deflate(stream_t &stream,
                        uint8_t *begin,
                        const uint32_t size) noexcept -> compression_operation_result_t;

public:
    [[nodiscard]] static inline auto get_buffer_size() noexcept -> uint32_t {
        size_t size = 0;
        size += sizeof(isal_zstream);
        size += static_cast<uint32_t>(util::align_size(sizeof(isal_hufftables)));
        size += static_cast<uint32_t>(util::align_size(sizeof(BitBuf2)));
        size += static_cast<uint32_t>(util::align_size(isal_level_buffer_size));

        return static_cast<uint32_t>(util::align_size(size, 1_kb));
    }

    auto write_bytes(const uint8_t *data, uint32_t size) noexcept -> qpl_ml_status override;

    void set_source(uint8_t *begin, uint32_t size) noexcept override;

    void save_bit_buffer() noexcept;

    [[nodiscard]] auto compression_level() const noexcept -> compression_level_t;

    [[nodiscard]] auto mini_blocks_support() const noexcept -> mini_blocks_support_t;

    [[nodiscard]] auto dictionary_support() const noexcept -> dictionary_support_t;

    [[nodiscard]] auto crc() const noexcept -> uint32_t;

    static constexpr auto execution_path = execution_path_t::software;

protected:
    void reset_match_history() noexcept;

    void reset_bit_buffer() noexcept;

    void dump_bit_buffer() noexcept;

    void dump_isal_stream() noexcept;

    void restore_isal_stream() noexcept;

    void write_mini_block_index() noexcept;

    auto init_level_buffer() noexcept -> int;

    [[nodiscard]] auto source_begin() const noexcept -> uint8_t *;

    [[nodiscard]] auto should_start_new_block() const noexcept -> bool;

    [[nodiscard]] auto are_buffers_empty() const noexcept -> bool;

    [[nodiscard]] auto hash_table() noexcept -> deflate_hash_table_t *;

    [[nodiscard]] auto bits_written() noexcept -> uint32_t;

    [[nodiscard]] inline auto next_out() const noexcept -> uint8_t * {
        return isal_stream_ptr_->next_out;
    }

    [[nodiscard]] inline auto avail_out() const noexcept -> uint32_t {
        return isal_stream_ptr_->avail_out;
    }

    inline void set_output_prologue(uint32_t size) noexcept {
        isal_stream_ptr_->next_out += size;
        isal_stream_ptr_->avail_out -= size;
        index_table_.index_bit_offset += size * byte_bit_size;
    }

    isal_zstream           *isal_stream_ptr_        = nullptr;
    isal_hufftables        *isal_huffman_table_ptr_ = nullptr;
    huffman_table_icf    huffman_table_icf_ = {};
    deflate_hash_table_t hash_table_        = {};
    index_table_t        index_table_       = {};
    compression_level_t    level_                   = default_level;
    dictionary_support_t   dictionary_support_      = dictionary_support_t::disabled;
    BitBuf2                *bit_buffer_ptr          = nullptr;
    bool                   start_new_block_         = false;
    uint8_t                *source_begin_ptr_       = nullptr;
    uint32_t               source_size_             = 0;
    uint32_t               ignore_start_bits_       = 0;
    uint32_t               total_bytes_written_     = 0;

    // Verification
    bool                   is_verification_enabled_   = false;
    qpl_compression_huffman_table* compression_table_ = nullptr;

    // Other
    const util::linear_allocator& allocator_;

private:
    explicit deflate_state(const util::linear_allocator &allocator) : allocator_(allocator) {
        compression_mode_ = fixed_mode;
        isal_stream_ptr_  = allocator.allocate<isal_zstream>();
    }

    friend auto write_stored_block_header(deflate_state<execution_path_t::software> &stream,
                                          compression_state_t &state) noexcept -> qpl_ml_status;

    friend auto write_stored_block(deflate_state<execution_path_t::software> &stream,
                                   compression_state_t &state) noexcept -> qpl_ml_status;

    friend auto write_buffered_icf_header(deflate_state<execution_path_t::software> &stream,
                                          compression_state_t &state) noexcept -> qpl_ml_status;

    friend auto create_icf_block_header(deflate_state<execution_path_t::software> &stream,
                                        compression_state_t &state) noexcept -> qpl_ml_status;

    friend auto flush_icf_block(deflate_state<execution_path_t::software> &stream,
                                compression_state_t &state) noexcept -> qpl_ml_status;

    friend auto init_new_icf_block(deflate_state<execution_path_t::software> &stream,
                                   compression_state_t &state) noexcept -> qpl_ml_status;

    friend auto deflate_icf_finish(deflate_state<execution_path_t::software> &stream,
                                   compression_state_t &state) noexcept -> qpl_ml_status;

    friend auto deflate_icf_body(deflate_state<execution_path_t::software> &stream,
                                 compression_state_t &state) noexcept -> qpl_ml_status;

    friend auto slow_deflate_icf_body(deflate_state<execution_path_t::software> &stream,
                                      compression_state_t &state) noexcept -> qpl_ml_status;

    friend auto write_header(deflate_state<execution_path_t::software> &stream,
                             compression_state_t &state) noexcept -> qpl_ml_status;

    friend auto slow_deflate_body(deflate_state<execution_path_t::software> &stream,
                                  compression_state_t &state) noexcept -> qpl_ml_status;

    friend auto write_end_of_block(deflate_state<execution_path_t::software> &stream,
                                   compression_state_t &state) noexcept -> qpl_ml_status;

    template <typename stream_t>
    friend auto init_compression(stream_t &stream, compression_state_t &state) noexcept -> qpl_ml_status;

    template <typename stream_t>
    friend auto finish_deflate_block(stream_t &stream,
                                     compression_state_t &state) noexcept -> qpl_ml_status;

    template <typename stream_t>
    friend auto flush_bit_buffer(stream_t &stream,
                                 compression_state_t &state) noexcept -> qpl_ml_status;

    template <typename stream_t>
    friend auto flush_write_buffer(stream_t &stream,
                                   compression_state_t &stat) noexcept -> qpl_ml_status;

    friend auto update_checksum(deflate_state<execution_path_t::software> &stream) noexcept -> qpl_ml_status;

    friend auto deflate_body(deflate_state<execution_path_t::software> &stream,
                             compression_state_t &state) noexcept -> qpl_ml_status;

    friend auto deflate_finish(deflate_state<execution_path_t::software> &stream,
                               compression_state_t &state) noexcept -> qpl_ml_status;

    friend auto process_by_mini_blocks_body(deflate_state<execution_path_t::software> &stream,
                                            compression_state_t &state) noexcept -> qpl_ml_status;

    friend auto build_huffman_table(deflate_state<execution_path_t::software> &stream,
                                    compression_state_t &state) noexcept -> qpl_ml_status;

    friend auto preprocess_static_block(deflate_state<execution_path_t::software> &stream,
                                        compression_state_t &state) noexcept -> qpl_ml_status;

    friend void update_state(deflate_state<execution_path_t::software> &stream,
                             uint8_t *start_in_ptr,
                             uint8_t *next_in_ptr,
                             uint8_t *end_in_ptr) noexcept;

    friend auto get_history_size(deflate_state<execution_path_t::software> &stream,
                                 uint8_t *start_in,
                                 int32_t buf_hist_start) noexcept -> uint32_t;

    friend auto deflate_body_with_dictionary(deflate_state<execution_path_t::software> &stream,
                                             compression_state_t &state) noexcept -> qpl_ml_status;

    friend auto finish_compression_process(deflate_state<execution_path_t::software> &stream) noexcept -> qpl_ml_status;

    friend void update_hash(deflate_state<execution_path_t::software> &stream,
                            uint8_t *dictionary_ptr,
                            uint32_t dictionary_size) noexcept;

    friend auto recover_and_write_stored_blocks(deflate_state<execution_path_t::software> &stream,
                                                compression_state_t &state) noexcept -> qpl_ml_status;
};

} // namespace qpl::ml::compression

#endif // QPL_MIDDLE_LAYER_COMPRESSION_STREAMS_SW_DEFLATE_STATE_HPP
