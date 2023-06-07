/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

#ifndef QPL_MIDDLE_LAYER_COMPRESSION_STREAMS_HW_DEFLATE_STATE_HPP
#define QPL_MIDDLE_LAYER_COMPRESSION_STREAMS_HW_DEFLATE_STATE_HPP

#include <compression/multitask/multi_task.hpp>
#include <common/linear_allocator.hpp>
#include "hw_definitions.h"
#include "hw_aecs_api.h"
#include "hw_iaa_flags.h"
#include "hw_descriptors_api.h"
#include "compression/deflate/deflate.hpp"
#include "compression/deflate/streams/compression_stream.hpp"
#include "compression/deflate/compression_units/stored_block_units.hpp"

namespace qpl::ml::compression {

template <>
class deflate_state<execution_path_t::hardware> final {
    template <execution_path_t path>
    friend
    class deflate_state_builder;

    template <execution_path_t path, deflate_mode_t mode, class stream_t>
    friend auto deflate(stream_t &stream,
                        uint8_t *begin,
                        const uint32_t size) noexcept -> compression_operation_result_t;

    friend auto write_stored_block(deflate_state<execution_path_t::hardware> &state) noexcept -> compression_operation_result_t;

    friend class gzip_decorator;

    friend class zlib_decorator;

public:
    [[nodiscard]] static constexpr inline auto get_buffer_size() noexcept -> uint32_t {
        size_t size = 0;

        size += sizeof(meta_data);
        size += util::align_size(sizeof(hw_descriptor)) * 3;
        size += util::align_size(sizeof(hw_completion_record));
        size += util::align_size(sizeof(hw_iaa_aecs_compress)) * 2;
        size += util::align_size(sizeof(hw_iaa_aecs_analytic)) * 2;

        return static_cast<uint32_t>(util::align_size(size, 1_kb));
    }

    static constexpr auto execution_path = execution_path_t::hardware;

protected:
    inline auto allocate_verification_state() noexcept -> void;

    [[nodiscard]] inline auto is_first_chunk() const noexcept -> bool;

    [[nodiscard]] inline auto is_last_chunk() const noexcept -> bool;

    [[nodiscard]] inline auto next_out() const noexcept -> uint8_t *;

    [[nodiscard]] inline auto avail_out() const noexcept -> uint32_t;

    [[nodiscard]] auto crc() const noexcept -> uint32_t;

    inline void set_output_prologue(uint32_t size) noexcept;

    hw_descriptor                 *collect_statistic_descriptor_ = nullptr;
    hw_descriptor                 *compress_descriptor_          = nullptr;
    hw_descriptor                 *verify_descriptor_            = nullptr;
    HW_PATH_VOLATILE hw_completion_record *completion_record_    = nullptr;
    qpl_compression_huffman_table *huffman_table_                = nullptr;
    bool                          start_new_block                = false;
    util::multitask_status        processing_step                = util::multitask_status::ready;
    uint32_t                      prev_written_indexes           = 0u; // todo align with SW

    struct meta_data {
        uint8_t                  aecs_index       = 0u;
        uint32_t                 stored_bits      = 0u;
        hw_huffman_code          eob_code         = {};
        hw_iaa_aecs_compress     *aecs_           = nullptr;
        hw_iaa_mini_block_size_t mini_block_size_ = static_cast<hw_iaa_mini_block_size_t>(mini_block_size_none);
        uint32_t                 prologue_size_   = 0u;
    };

    meta_data *meta_data_ = nullptr;

    // Verify State
    hw_iaa_aecs_analytic *aecs_verify_ = nullptr;

    const util::linear_allocator &allocator_;

    explicit deflate_state(const qpl::ml::util::linear_allocator &allocator) : allocator_(allocator) {
        meta_data_           = allocator.allocate<deflate_state<execution_path_t::hardware>::meta_data>();
        compress_descriptor_ = allocator.allocate<hw_descriptor, qpl::ml::util::memory_block_t::aligned_64u>();
        completion_record_   = allocator.allocate<hw_completion_record, qpl::ml::util::memory_block_t::aligned_64u>();
        meta_data_->aecs_    = allocator.allocate<hw_iaa_aecs_compress, qpl::ml::util::memory_block_t::aligned_64u>(2u);

        hw_iaa_descriptor_init_compress_body(compress_descriptor_);
    }
};

inline auto deflate_state<execution_path_t::hardware>::is_first_chunk() const noexcept -> bool {
    return processing_step & util::multitask_status::multi_chunk_first_chunk;
}

inline auto deflate_state<execution_path_t::hardware>::is_last_chunk() const noexcept -> bool {
    return processing_step & util::multitask_status::multi_chunk_last_chunk;
}

inline auto deflate_state<execution_path_t::hardware>::next_out() const noexcept -> uint8_t * {
    return ((hw_iaa_analytics_descriptor *) compress_descriptor_)->dst_ptr;
}

inline auto deflate_state<execution_path_t::hardware>::avail_out() const noexcept -> uint32_t {
    return ((hw_iaa_analytics_descriptor *) compress_descriptor_)->max_dst_size;
}

inline void deflate_state<execution_path_t::hardware>::set_output_prologue(uint32_t size) noexcept {
    meta_data_->prologue_size_ = size;
    hw_iaa_descriptor_shift_output_buffer(compress_descriptor_, meta_data_->prologue_size_);
}

inline void deflate_state<execution_path_t::hardware>::allocate_verification_state() noexcept {
    verify_descriptor_ = allocator_.allocate<hw_descriptor, qpl::ml::util::memory_block_t::aligned_64u>();
    aecs_verify_       = allocator_.allocate<hw_iaa_aecs_analytic, qpl::ml::util::memory_block_t::aligned_64u>(2u);

    hw_iaa_descriptor_init_compress_verification(verify_descriptor_);
}

inline auto deflate_state<execution_path_t::hardware>::crc() const noexcept -> uint32_t {
    return meta_data_->aecs_[meta_data_->aecs_index].crc;
}

} // namespace qpl::ml::compression

#endif // QPL_MIDDLE_LAYER_COMPRESSION_STREAMS_HW_DEFLATE_STATE_HPP
