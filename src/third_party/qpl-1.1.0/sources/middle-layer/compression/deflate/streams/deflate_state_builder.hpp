/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

#ifndef QPL_MIDDLE_LAYER_COMPRESSION_STREAMS_COMPRESSION_STREAM_BUILDER_HPP
#define QPL_MIDDLE_LAYER_COMPRESSION_STREAMS_COMPRESSION_STREAM_BUILDER_HPP

#include "type_traits"

#include "util/checksum.hpp"
#include "compression/deflate/utils/compression_defs.hpp"
#include "compression/huffman_table/deflate_huffman_table.hpp"
#include "compression/deflate/utils/compression_traits.hpp"
#include "compression/deflate/streams/sw_deflate_state.hpp"
#include "compression/deflate/streams/hw_deflate_state.hpp"
#include "compression/deflate/containers/index_table.hpp"
#include "compression/dictionary/dictionary_utils.hpp"
#include "util/memory.hpp"
#include "common/defs.hpp"

#include "common/linear_allocator.hpp"

#include "igzip_lib.h"

#include "hw_descriptors_api.h"
#include "hw_aecs_api.h"
#include "hw_deflate_state.hpp"

extern "C" const uint32_t fixed_literals_table[];
extern "C" const uint32_t fixed_offsets_table[];

namespace qpl::ml::compression {

template <execution_path_t path>
class deflate_state_builder;

template<>
class deflate_state_builder<execution_path_t::software> {
protected:
    using common_type = typename traits::common_type_for_compression_stream_builder<execution_path_t::software>::type;
    using state_type = typename traits::common_type_for_compression_stream<execution_path_t::software>::type;

public:
    deflate_state_builder() = delete;

    static auto create(const util::linear_allocator &allocator) noexcept -> common_type {
        common_type builder(allocator);

        builder.stream_.chunk_type_.is_first = true;

        return builder;
    }

    static auto restore(const util::linear_allocator &allocator) noexcept -> common_type {
        return common_type(allocator);
    };

    auto compression_level(compression_level_t level) noexcept -> common_type &;

    auto start_new_block(bool value) noexcept -> common_type &;

    auto load_current_position(uint32_t total_bytes_written) noexcept -> common_type &;

    [[nodiscard]] auto build() noexcept -> state_type {
        init();

        return stream_;
    }

    auto output(uint8_t *buffer_ptr, uint32_t buffer_size) noexcept -> common_type &;

    auto terminate(bool value) noexcept -> common_type & {
        stream_.chunk_type_.is_last = value;

        return *reinterpret_cast<common_type *>(this);
    }

    auto crc_seed(util::checksum_accumulator value) noexcept -> common_type & {
        stream_.checksum_ = value;

        return *reinterpret_cast<common_type *>(this);
    }

    auto enable_indexing(mini_block_size_t indexed_mini_block_size,
                         uint64_t *index_table_ptr,
                         uint32_t index_table_size,
                         uint32_t index_table_capacity) noexcept -> common_type & {
        stream_.mini_block_size_ = indexed_mini_block_size;
        stream_.index_table_.initialize(index_table_ptr, index_table_size, index_table_capacity);

        return *reinterpret_cast<common_type *>(this);
    }

    auto collect_statistics_step(bool UNREFERENCED_PARAMETER(value)) noexcept -> common_type & {
        stream_.compression_mode_ = dynamic_mode;

        return *reinterpret_cast<common_type *>(this);
    };

    auto compression_table(qpl_compression_huffman_table *huffman_table) noexcept -> common_type & {
        stream_.compression_mode_ = static_mode;
        set_huffman_table(huffman_table);

        return *reinterpret_cast<common_type *>(this);
    };

    auto dictionary(qpl_dictionary &dictionary) noexcept -> common_type &;

    auto verify(bool value) noexcept -> common_type & {
        stream_.is_verification_enabled_ = value;

        return *reinterpret_cast<common_type *>(this);
    }

protected:
    auto set_isal_internal_buffers(uint8_t *const level_buffer_ptr,
                                   const uint32_t level_buffer_size,
                                   isal_hufftables *const huffman_tables_ptr,
                                   BitBuf2 *const bit_buffer_ptr) noexcept -> void;

    void set_huffman_table(qpl_compression_huffman_table *huffman_table) noexcept;

    void init() noexcept;

    state_type stream_;

private:
    explicit deflate_state_builder(const qpl::ml::util::linear_allocator &allocator) noexcept : stream_(allocator) {

        // allocations required for internal isal stream buffers
        uint8_t *level_buffer_ptr = nullptr;
        uint8_t *bit_buffer_ptr   = nullptr;
        uint8_t *static_huffman_table_buffer_ptr = nullptr;

        uint32_t level_buffer_size      = static_cast<uint32_t>(util::align_size(isal_level_buffer_size));
        uint32_t bit_buffer_size        = static_cast<uint32_t>(util::align_size(sizeof(BitBuf2)));
        uint32_t static_huff_table_size = static_cast<uint32_t>(util::align_size(sizeof(struct isal_hufftables)));

        uint32_t buffer_size = level_buffer_size + bit_buffer_size + static_huff_table_size;
        auto buffer_ptr  = allocator.allocate<uint8_t, qpl::ml::util::memory_block_t::aligned_64u>(buffer_size);

        // do not initialize isal buffers if previous allocation failed
        // , but leave them in default nullptr state
        if (nullptr != buffer_ptr) {
            level_buffer_ptr = buffer_ptr;
            bit_buffer_ptr   = level_buffer_ptr + level_buffer_size;
            static_huffman_table_buffer_ptr = bit_buffer_ptr + bit_buffer_size;

            // initialization of internal isal stream buffers
            if (stream_.is_first_chunk()) {
                isal_deflate_init(stream_.isal_stream_ptr_);
            } else {
                isal_deflate_reset(stream_.isal_stream_ptr_);
            }

            stream_.bit_buffer_ptr = reinterpret_cast<BitBuf2 *>(bit_buffer_ptr);
            set_isal_internal_buffers(level_buffer_ptr,
                                      level_buffer_size,
                                      reinterpret_cast<isal_hufftables *>(static_huffman_table_buffer_ptr),
                                      reinterpret_cast<BitBuf2 *>(bit_buffer_ptr));
        }
    }
};

template<>
class deflate_state_builder<execution_path_t::hardware> {
    using common_type = deflate_state_builder<execution_path_t::hardware>;
    using state_type  = deflate_state<execution_path_t::hardware>;

public:

    static auto create(const util::linear_allocator &allocator) noexcept -> common_type {
        common_type builder(allocator);

        builder.state_.processing_step = util::multitask_status::multi_chunk_first_chunk;
        builder.state_.meta_data_->aecs_index     = 0u;
        builder.state_.meta_data_->prologue_size_ = 0u;

        hw_iaa_aecs_compress_clean_accumulator(builder.state_.meta_data_->aecs_);

        return builder;
    }

    static auto restore(const util::linear_allocator &allocator) noexcept -> common_type {
        return common_type(allocator);
    };

    inline auto compression_level(compression_level_t level) noexcept -> common_type &;

    inline auto start_new_block(bool value) noexcept -> common_type &;

    inline auto load_current_position(uint32_t total_bytes_written) noexcept -> common_type &;

    [[nodiscard]] inline auto build() noexcept -> state_type;

    inline auto output(uint8_t *buffer_ptr, uint32_t buffer_size) noexcept -> common_type &;

    inline auto terminate(bool value) noexcept -> common_type &;

    inline auto crc_seed(util::checksum_accumulator value) noexcept -> common_type &;

    inline auto enable_indexing(mini_block_size_t indexed_mini_block_size,
                                uint64_t *index_table_ptr,
                                uint32_t index_table_size,
                                uint32_t index_table_capacity) noexcept -> common_type &;

    inline auto collect_statistics_step(bool value) noexcept -> common_type &;

    inline auto compression_table(qpl_compression_huffman_table *huffman_table) noexcept -> common_type &;

    inline auto verify(bool value) noexcept -> common_type &;

protected:
    state_type                            state_;
    const qpl::ml::util::linear_allocator &allocator_;

private:
    explicit deflate_state_builder(const qpl::ml::util::linear_allocator &allocator) noexcept : state_(allocator), allocator_(allocator) {
        // No actions required
    }
};

/* ------ HARDWARE ------ */
inline auto deflate_state_builder<execution_path_t::hardware>::compression_level(compression_level_t UNREFERENCED_PARAMETER(level)) noexcept -> common_type & {
    // HW supports only default level
    return *this;
}

inline auto deflate_state_builder<execution_path_t::hardware>::start_new_block(bool value) noexcept -> common_type & {
    state_.start_new_block = value;

    return *this;
}

inline auto deflate_state_builder<execution_path_t::hardware>::load_current_position(uint32_t UNREFERENCED_PARAMETER(total_bytes_written)) noexcept -> common_type & {
    return *this;
}

inline auto deflate_state_builder<execution_path_t::hardware>::output(uint8_t *buffer_ptr, uint32_t buffer_size) noexcept -> common_type & {
    hw_iaa_descriptor_set_output_buffer(state_.compress_descriptor_, buffer_ptr, buffer_size);

    return *this;
}

inline auto deflate_state_builder<execution_path_t::hardware>::terminate(bool value) noexcept -> common_type & {
    if (value) {
        state_.processing_step = static_cast<util::multitask_status>(state_.processing_step |
                                                                            util::multitask_status::multi_chunk_last_chunk);
    }

    return *this;
}

inline auto deflate_state_builder<execution_path_t::hardware>::crc_seed(util::checksum_accumulator value) noexcept -> common_type & {
    hw_iaa_aecs_compress_set_checksums(&state_.meta_data_->aecs_[state_.meta_data_->aecs_index], value.crc32, 0u);

    return *this;
}

inline auto deflate_state_builder<execution_path_t::hardware>::enable_indexing(mini_block_size_t indexed_mini_block_size,
                                                                               uint64_t *index_table_ptr,
                                                                               uint32_t index_table_size,
                                                                               uint32_t index_table_capacity) noexcept -> common_type & {
    state_.meta_data_->mini_block_size_ = static_cast<hw_iaa_mini_block_size_t>(indexed_mini_block_size);
    hw_iaa_descriptor_compress_set_mini_block_size(state_.compress_descriptor_, state_.meta_data_->mini_block_size_);

    if (indexed_mini_block_size) {
        if (state_.verify_descriptor_ == nullptr) {
            state_.allocate_verification_state();
        }

        hw_iaa_descriptor_decompress_set_mini_block_size(state_.verify_descriptor_, state_.meta_data_->mini_block_size_);
        hw_iaa_descriptor_compress_verification_set_index_table(state_.verify_descriptor_,
                                                                index_table_ptr,
                                                                index_table_size,
                                                                index_table_capacity);
        state_.prev_written_indexes = index_table_size;
    }

    return *this;
}

inline auto deflate_state_builder<execution_path_t::hardware>::collect_statistics_step(bool value) noexcept -> common_type & {
    if (value) {
        state_.collect_statistic_descriptor_ = allocator_.allocate<hw_descriptor, qpl::ml::util::memory_block_t::aligned_64u>();
    }

    return *this;
}

inline auto deflate_state_builder<execution_path_t::hardware>::compression_table(qpl_compression_huffman_table *huffman_table) noexcept -> common_type & {
    state_.huffman_table_ = huffman_table;
    hw_iaa_aecs_compress_set_deflate_huffman_table(&state_.meta_data_->aecs_[state_.meta_data_->aecs_index],
                                                   get_literals_lengths_table_ptr(state_.huffman_table_),
                                                   get_offsets_table_ptr(state_.huffman_table_));

    uint32_t code_length  = get_literals_lengths_table_ptr(state_.huffman_table_)[256];
    uint32_t eob_code_len = code_length >> 15u;
    state_.meta_data_->eob_code.code   = util::revert_bits((uint16_t) code_length) >> (16u - eob_code_len);
    state_.meta_data_->eob_code.length = eob_code_len;

    return *this;
}

inline auto deflate_state_builder<execution_path_t::hardware>::verify(bool value) noexcept -> common_type & {
    if (value && state_.verify_descriptor_ == nullptr) {
        state_.allocate_verification_state();
    }

    return *reinterpret_cast<common_type *>(this);
}

[[nodiscard]] inline auto deflate_state_builder<execution_path_t::hardware>::build() noexcept -> state_type {
    if (!state_.collect_statistic_descriptor_ && !state_.huffman_table_) {
        hw_iaa_aecs_compress_set_deflate_huffman_table(&state_.meta_data_->aecs_[state_.meta_data_->aecs_index],
                                                       reinterpret_cast<const void *>(fixed_literals_table),
                                                       reinterpret_cast<const void *>(fixed_offsets_table));
        state_.meta_data_->eob_code.code   = 0u;
        state_.meta_data_->eob_code.length = 7u;
    }

    return state_;
}

} // namespace qpl::ml::compression

#endif // QPL_MIDDLE_LAYER_COMPRESSION_STREAMS_COMPRESSION_STREAM_BUILDER_HPP
