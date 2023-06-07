/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

/*
 *  Intel® Query Processing Library (Intel® QPL)
 *  Middle Layer API (private C++ API)
 */

#ifndef QPL_SOURCES_MIDDLE_LAYER_COMPRESSION_HUFFMAN_ONLY_HUFFMAN_ONLY_STREAM_HPP_
#define QPL_SOURCES_MIDDLE_LAYER_COMPRESSION_HUFFMAN_ONLY_HUFFMAN_ONLY_STREAM_HPP_

#include "igzip_lib.h"
#include "compression/compression_defs.hpp"
#include "compression/deflate/utils/compression_defs.hpp"

#include "util/checksum.hpp"
#include "common/linear_allocator.hpp"

#include "hw_definitions.h"
#include "hw_aecs_api.h"

extern "C" const struct isal_hufftables hufftables_static;

namespace qpl::ml::compression {

class compression_huffman_table;

template <execution_path_t path>
class huffman_only_compression_state_builder;

template <execution_path_t path>
class huffman_only_state;

template <>
class huffman_only_state<execution_path_t::software> {
    friend class huffman_only_compression_state_builder<execution_path_t::software>;

public:
    inline void set_input(uint8_t *data_ptr, uint32_t size) noexcept {
        isal_stream_ptr_->next_in  = data_ptr;
        isal_stream_ptr_->avail_in = size;

        source_begin_ptr_ = data_ptr;
    }

    [[nodiscard]] inline auto endianness() noexcept -> endianness_t {
        return endianness_;
    }

    [[nodiscard]] inline auto checksum() noexcept -> util::checksum_accumulator & {
        return checksum_;
    }

    [[nodiscard]] inline auto get_software_compression_table() noexcept -> isal_hufftables * {
        return isal_stream_ptr_->hufftables;
    }

    [[nodiscard]] inline auto get_last_bits_offset() noexcept -> uint32_t {
        return last_bits_offset_;
    }

    [[nodiscard]] static constexpr inline auto get_buffer_size() noexcept -> uint32_t {
        size_t size = 0;

        size += sizeof(isal_zstream);
        size += sizeof(isal_hufftables);

        return static_cast<uint32_t>(size);
    }

protected:
    void reset_bit_buffer() noexcept;

    void dump_bit_buffer() noexcept;

private:
    util::checksum_accumulator      checksum_                = {};

    isal_zstream              *isal_stream_ptr_        = nullptr;
    compression_huffman_table *huffman_table_ptr_      = nullptr;
    uint8_t                   *source_begin_ptr_       = nullptr;
    endianness_t              endianness_              = little_endian;
    uint32_t                  crc_seed_                = 0u;
    bool                      collect_statistic_       = false;
    compression_mode_t        compression_mode_        = fixed_mode;
    uint8_t                   last_bits_offset_        = 0u;

    // Verification
    bool is_verification_enabled_ = false;

    // Other
    const util::linear_allocator& allocator_;

    explicit huffman_only_state(const util::linear_allocator &allocator) noexcept: allocator_(allocator) {
        isal_stream_ptr_ = allocator_.allocate<isal_zstream>();
        isal_stream_ptr_->hufftables = const_cast<isal_hufftables *>(&hufftables_static);
    }

    template<typename stream_t>
    friend auto init_compression(stream_t &stream, compression_state_t &state) noexcept -> qpl_ml_status;

    template<typename stream_t>
    friend auto finish_deflate_block(stream_t &stream,
                                     compression_state_t &state) noexcept -> qpl_ml_status;

    template<typename stream_t>
    friend auto flush_bit_buffer(stream_t &stream,
                                 compression_state_t &state) noexcept -> qpl_ml_status;

    template<typename stream_t>
    friend auto flush_write_buffer(stream_t &stream,
                                   compression_state_t &stat) noexcept -> qpl_ml_status;

    friend void update_state(huffman_only_state<execution_path_t::software> &stream,
                             uint8_t *start_in_ptr,
                             uint8_t *next_in_ptr,
                             uint8_t *end_in_ptr) noexcept;

    friend auto huffman_only_compress_block(huffman_only_state<execution_path_t::software> &stream,
                                            compression_state_t &state) noexcept -> qpl_ml_status;

    friend auto huffman_only_finalize(huffman_only_state<execution_path_t::software> &stream,
                                      compression_state_t &state) noexcept -> qpl_ml_status;

    friend auto huffman_only_create_huffman_table(huffman_only_state<execution_path_t::software> &stream,
                                                  compression_state_t &state) noexcept -> qpl_ml_status;

    friend auto convert_output_to_big_endian(huffman_only_state<execution_path_t::software> &stream,
                                             compression_state_t &state) noexcept -> qpl_ml_status;

    template <execution_path_t path, class stream_t>
    friend auto compress_huffman_only(uint8_t *begin,
                               const uint32_t size,
                               stream_t &stream) noexcept -> compression_operation_result_t;
};

template <>
class huffman_only_state<execution_path_t::hardware> {
    friend class huffman_only_compression_state_builder<execution_path_t::hardware>;

public:
    [[nodiscard]] static constexpr inline auto get_buffer_size() noexcept -> uint32_t {
        size_t size = 0;

        size += util::align_size(sizeof(hw_descriptor))*2;
        size += util::align_size(sizeof(hw_completion_record));
        size += util::align_size(sizeof(hw_iaa_aecs_compress));

        return static_cast<uint32_t>(util::align_size(size, 1_kb));
    }

private:
    hw_descriptor             *descriptor_collect_statistic_  = nullptr;
    hw_descriptor             *descriptor_compress_           = nullptr;
    HW_PATH_VOLATILE hw_completion_record *completion_record_ = nullptr;
    hw_iaa_aecs_compress                  *compress_aecs_     = nullptr;
    compression_huffman_table             *huffman_table_ptr_ = nullptr;

    // Verification
    bool is_verification_enabled_ = false;

    template <execution_path_t path, class stream_t>
    friend auto compress_huffman_only(uint8_t *begin,
                                      const uint32_t size,
                                      stream_t &stream) noexcept -> compression_operation_result_t;
};
}

#endif //QPL_SOURCES_MIDDLE_LAYER_COMPRESSION_HUFFMAN_ONLY_HUFFMAN_ONLY_STREAM_HPP_
