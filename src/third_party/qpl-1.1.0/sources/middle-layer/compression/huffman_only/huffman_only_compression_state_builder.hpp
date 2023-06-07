/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

/*
 *  Intel® Query Processing Library (Intel® QPL)
 *  Middle Layer API (private C++ API)
 */

#ifndef QPL_SOURCES_MIDDLE_LAYER_COMPRESSION_HUFFMAN_ONLY_HUFFMAN_ONLY_COMPRESSION_STATE_BUILDER_HPP_
#define QPL_SOURCES_MIDDLE_LAYER_COMPRESSION_HUFFMAN_ONLY_HUFFMAN_ONLY_COMPRESSION_STATE_BUILDER_HPP_

#include "common/linear_allocator.hpp"
#include "compression/huffman_only/huffman_only_compression_state.hpp"
#include "core_deflate_api.h"

extern "C" const struct isal_hufftables hufftables_static;

namespace qpl::ml::compression {

template <execution_path_t path>
class huffman_only_compression_state_builder;

template<>
class huffman_only_compression_state_builder<execution_path_t::software> {
public:
    explicit huffman_only_compression_state_builder(const qpl::ml::util::linear_allocator &allocator)
    : stream_(allocator), allocator_(allocator) {
        // No actions required
    }

    inline auto output(uint8_t *begin, uint32_t size) noexcept -> huffman_only_compression_state_builder &;

    inline auto compress_table(compression_huffman_table &huffman_table) noexcept -> huffman_only_compression_state_builder &;

    inline auto crc_seed(uint32_t seed) noexcept -> huffman_only_compression_state_builder &;

    inline auto be_output(bool value) noexcept -> huffman_only_compression_state_builder &;

    inline auto collect_statistics_step(bool value) noexcept -> huffman_only_compression_state_builder &;

    inline auto verify(bool value) noexcept -> huffman_only_compression_state_builder &;

    inline auto total_out(uint32_t total_out_value) noexcept -> huffman_only_compression_state_builder&;

    inline auto build() noexcept -> huffman_only_state<execution_path_t::software>;

private:
    huffman_only_state<execution_path_t::software> stream_;
    const qpl::ml::util::linear_allocator          &allocator_;
};

template<>
class huffman_only_compression_state_builder<execution_path_t::hardware> {
public:
    explicit huffman_only_compression_state_builder(const qpl::ml::util::linear_allocator &allocator): allocator_(allocator) {
        stream_.descriptor_compress_ = allocator.allocate<hw_descriptor, qpl::ml::util::memory_block_t::aligned_64u>();
        stream_.completion_record_ = allocator.allocate<hw_completion_record , qpl::ml::util::memory_block_t::aligned_64u>();
        stream_.compress_aecs_ = allocator.allocate<hw_iaa_aecs_compress, qpl::ml::util::memory_block_t::aligned_64u>();

        hw_iaa_descriptor_init_compress_body(stream_.descriptor_compress_);
        hw_iaa_descriptor_compress_set_huffman_only_mode(stream_.descriptor_compress_);
    }

    inline auto output(uint8_t *begin, uint32_t size) noexcept -> huffman_only_compression_state_builder &;

    inline auto compress_table(compression_huffman_table &huffman_table) noexcept -> huffman_only_compression_state_builder &;

    inline auto crc_seed(uint32_t seed) noexcept -> huffman_only_compression_state_builder &;

    inline auto be_output(bool value) noexcept -> huffman_only_compression_state_builder &;

    inline auto collect_statistics_step(bool value) noexcept -> huffman_only_compression_state_builder &;

    inline auto verify(bool value) noexcept -> huffman_only_compression_state_builder &;

    inline auto total_out(uint32_t total_out_value) noexcept -> huffman_only_compression_state_builder&;

    inline auto build() noexcept -> huffman_only_state<execution_path_t::hardware>;

private:
    huffman_only_state<execution_path_t::hardware> stream_;
    const qpl::ml::util::linear_allocator          &allocator_;
};

// ------ Software ------ //

inline auto huffman_only_compression_state_builder<execution_path_t::software>::compress_table(compression_huffman_table &huffman_table) noexcept -> huffman_only_compression_state_builder & {
    stream_.isal_stream_ptr_->hufftables = allocator_.allocate<isal_hufftables>();
    stream_.huffman_table_ptr_ = &huffman_table;

    huffman_table_convert(*(huffman_table.get_sw_compression_table()), *(stream_.isal_stream_ptr_->hufftables));

    return *this;
}

inline auto huffman_only_compression_state_builder<execution_path_t::software>::output(uint8_t *data_ptr, uint32_t size) noexcept -> huffman_only_compression_state_builder & {
    stream_.isal_stream_ptr_->next_out  = data_ptr;
    stream_.isal_stream_ptr_->avail_out = size;

    return *this;
}

inline auto huffman_only_compression_state_builder<execution_path_t::software>::crc_seed(uint32_t seed)
noexcept -> huffman_only_compression_state_builder & {
    stream_.crc_seed_ = seed;

    return *this;
}

inline auto huffman_only_compression_state_builder<execution_path_t::software>::be_output(bool value)
noexcept -> huffman_only_compression_state_builder & {
    stream_.endianness_ = (value) ? big_endian : little_endian;

    return *this;
}

inline auto huffman_only_compression_state_builder<execution_path_t::software>::collect_statistics_step(bool value)
noexcept -> huffman_only_compression_state_builder & {
    if (value) {
        stream_.collect_statistic_ = true;
        stream_.compression_mode_  = dynamic_mode;
    }

    return *this;
}

auto huffman_only_compression_state_builder<execution_path_t::software>::verify(bool UNREFERENCED_PARAMETER(value)) noexcept -> huffman_only_compression_state_builder & {
    stream_.is_verification_enabled_ = true;

    return *this;
}

inline auto huffman_only_compression_state_builder<execution_path_t::software>::total_out(uint32_t total_out_value)
noexcept -> huffman_only_compression_state_builder& {
    stream_.isal_stream_ptr_->total_out = total_out_value;

    return *this;
}

inline auto huffman_only_compression_state_builder<execution_path_t::software>::build()
noexcept -> huffman_only_state<execution_path_t::software> {
    return stream_;
}

// ------ Hardware ------ //

inline auto huffman_only_compression_state_builder<execution_path_t::hardware>::compress_table(compression_huffman_table &huffman_table) noexcept -> huffman_only_compression_state_builder & {
    stream_.huffman_table_ptr_ = &huffman_table;

    return *this;
}

inline auto huffman_only_compression_state_builder<execution_path_t::hardware>::output(uint8_t *data_ptr, uint32_t size) noexcept -> huffman_only_compression_state_builder & {
    hw_iaa_descriptor_set_output_buffer(stream_.descriptor_compress_, data_ptr, size);

    return *this;
}

inline auto huffman_only_compression_state_builder<execution_path_t::hardware>::crc_seed(uint32_t seed)
noexcept -> huffman_only_compression_state_builder & {
    hw_iaa_aecs_compress_set_checksums(stream_.compress_aecs_, seed, 0u);

    return *this;
}

inline auto huffman_only_compression_state_builder<execution_path_t::hardware>::be_output(bool value)
noexcept -> huffman_only_compression_state_builder & {
    if (value) {
        hw_iaa_descriptor_compress_set_be_output_mode(stream_.descriptor_compress_);
    }

    return *this;
}

inline auto huffman_only_compression_state_builder<execution_path_t::hardware>::collect_statistics_step(bool value)
noexcept -> huffman_only_compression_state_builder & {
    if (value) {
        stream_.descriptor_collect_statistic_ = allocator_.allocate<hw_descriptor,
                                                                    qpl::ml::util::memory_block_t::aligned_64u>();

        hw_iaa_descriptor_init_statistic_collector(stream_.descriptor_collect_statistic_,
                                                   nullptr, // Will be set later
                                                   0u,  // Will be set later,
                                                   &stream_.compress_aecs_->histogram);

        hw_iaa_descriptor_compress_set_huffman_only_mode(stream_.descriptor_collect_statistic_);
    }

    return *this;
}

auto huffman_only_compression_state_builder<execution_path_t::hardware>::verify(bool UNREFERENCED_PARAMETER(value)) noexcept -> huffman_only_compression_state_builder & {
    stream_.is_verification_enabled_ = true;

    return *this;
}


inline auto huffman_only_compression_state_builder<execution_path_t::hardware>::total_out(uint32_t UNREFERENCED_PARAMETER(total_out_value))
noexcept -> huffman_only_compression_state_builder& {

    return *this;
}


inline auto huffman_only_compression_state_builder<execution_path_t::hardware>::build()
noexcept -> huffman_only_state<execution_path_t::hardware> {
    if (!stream_.descriptor_collect_statistic_) {
        auto literal_table_ptr = (stream_.huffman_table_ptr_) ?
                                 stream_.huffman_table_ptr_->get_sw_compression_table()->literals_matches:
                                 (hw_iaa_huffman_codes *) fixed_literals_table;

        hw_iaa_aecs_compress_set_huffman_only_huffman_table(stream_.compress_aecs_, literal_table_ptr);

        hw_iaa_descriptor_compress_set_aecs(stream_.descriptor_compress_,
                                            stream_.compress_aecs_,
                                            hw_aecs_access_read);
    }

    return stream_;
}


}

#endif //QPL_SOURCES_MIDDLE_LAYER_COMPRESSION_HUFFMAN_ONLY_HUFFMAN_ONLY_COMPRESSION_STATE_BUILDER_HPP_
