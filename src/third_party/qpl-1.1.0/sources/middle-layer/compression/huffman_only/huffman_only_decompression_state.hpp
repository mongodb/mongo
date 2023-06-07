/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

#ifndef QPL_SOURCES_MIDDLE_LAYER_COMPRESSION_HUFFMAN_ONLY_DECOMPRESSION_STATE_HPP
#define QPL_SOURCES_MIDDLE_LAYER_COMPRESSION_HUFFMAN_ONLY_DECOMPRESSION_STATE_HPP

#include <cstdint>
#include "common/defs.hpp"
#include "common/linear_allocator.hpp"
#include "compression/deflate/utils/compression_defs.hpp"
#include "hw_aecs_api.h"
#include "hw_descriptors_api.h"

namespace qpl::ml::compression {

template <execution_path_t path>
class huffman_only_decompression_state;

constexpr uint32_t huffman_only_be_buffer_size    = 4096;
constexpr uint32_t huffman_only_lookup_table_size = 0xFFFF;

template <>
class huffman_only_decompression_state<execution_path_t::software> {
public:
    struct internal_state_fields_t {
        uint8_t  *current_source_ptr;
        uint32_t source_available;
        uint8_t  *current_destination_ptr;
        uint32_t destination_available;
        uint32_t crc_value;
        uint8_t last_bits_offset;
    };

    explicit huffman_only_decompression_state(const qpl::ml::util::linear_allocator &allocator) {
        // Allocate internal buffers
        state_ = allocator.allocate<internal_state_fields_t, qpl::ml::util::memory_block_t::not_aligned>(1u);

        lookup_table_ptr_ = allocator.allocate<uint8_t, qpl::ml::util::memory_block_t::not_aligned>(
                huffman_only_lookup_table_size);

        huffman_only_buffer_ptr_ = allocator.allocate<uint8_t, qpl::ml::util::memory_block_t::not_aligned>(
                huffman_only_be_buffer_size);

        // Initialize internal state
        state_->current_source_ptr      = nullptr;
        state_->current_destination_ptr = nullptr;

        state_->source_available      = 0;
        state_->destination_available = 0;

        state_->crc_value = 0;
    };

    template <class iterator_t>
    inline auto output(iterator_t begin, iterator_t end) noexcept -> huffman_only_decompression_state &;

    template <class iterator_t>
    inline auto input(iterator_t begin, iterator_t end) noexcept -> huffman_only_decompression_state &;

    inline auto crc_seed(uint32_t seed) noexcept -> huffman_only_decompression_state &;

    inline auto endianness(endianness_t endianness) noexcept -> huffman_only_decompression_state &;

    inline auto last_bits_offset(uint8_t value) noexcept -> huffman_only_decompression_state &;

    [[nodiscard]] inline auto get_input_size() const noexcept -> uint32_t;

    [[nodiscard]] inline auto get_fields() noexcept -> internal_state_fields_t &;

    [[nodiscard]] inline auto get_endianness() noexcept -> endianness_t;

    [[nodiscard]] inline auto get_lookup_table() noexcept -> uint8_t *;

    [[nodiscard]] inline auto get_buffer() noexcept -> uint8_t *;

    [[nodiscard]] static constexpr inline auto get_buffer_size() noexcept -> uint32_t {
        size_t size = 0;

        size += sizeof(internal_state_fields_t);
        size += sizeof(uint8_t)*huffman_only_lookup_table_size;
        size += sizeof(uint8_t)*huffman_only_be_buffer_size;
        size += sizeof(uint8_t)*4_kb; // for compress + verify

        return static_cast<uint32_t>(size);
    }

    static constexpr auto execution_path = execution_path_t::software;

private:
    internal_state_fields_t *state_;
    uint8_t                 *lookup_table_ptr_;
    uint8_t                 *huffman_only_buffer_ptr_;
    endianness_t            endianness_ = endianness_t::little_endian;
};

template <>
class huffman_only_decompression_state<execution_path_t::hardware> {
public:
    uint8_t ignore_end_bits = 0;

    explicit huffman_only_decompression_state(const qpl::ml::util::linear_allocator &allocator) {
        descriptor_        = allocator.allocate<hw_descriptor, qpl::ml::util::memory_block_t::aligned_64u>(1u);
        completion_record_ = allocator.allocate<hw_completion_record, qpl::ml::util::memory_block_t::aligned_64u>(1u);
    };

    template <class iterator_t>
    inline auto output(iterator_t begin, iterator_t end) noexcept -> huffman_only_decompression_state &;

    template <class iterator_t>
    inline auto input(iterator_t begin, iterator_t end) noexcept -> huffman_only_decompression_state &;

    inline auto crc_seed(uint32_t seed) noexcept -> huffman_only_decompression_state &;

    inline auto endianness(endianness_t endianness) noexcept -> huffman_only_decompression_state &;

    inline auto decompress_table(decompression_huffman_table &decompression_table) noexcept -> huffman_only_decompression_state &;

    [[nodiscard]] inline auto get_endianness(endianness_t endianness) noexcept -> endianness_t;

    [[nodiscard]] inline auto build_descriptor() noexcept -> hw_descriptor *;

    [[nodiscard]] inline auto handler() const noexcept -> HW_PATH_VOLATILE hw_completion_record *;

    [[nodiscard]] inline auto get_input_size() const noexcept -> uint32_t;

    [[nodiscard]] static constexpr inline auto get_buffer_size() noexcept -> uint32_t {
        size_t size = 0;

        size += util::align_size(sizeof(hw_descriptor));
        size += util::align_size(sizeof(hw_completion_record));

        return static_cast<uint32_t>(util::align_size(size, 1_kb));
    }

    static constexpr auto execution_path = execution_path_t::hardware;

private:
    hw_descriptor        *descriptor_                         = nullptr;
    HW_PATH_VOLATILE hw_completion_record *completion_record_ = nullptr;
    hw_iaa_aecs_analytic                  *decompress_aecs_   = nullptr;
    uint32_t                              input_size_         = 0u;
    uint32_t             crc_                                 = 0u;
    endianness_t         endianness_                          = endianness_t::little_endian;
};

/* ------ SOFTWARE STATE METHODS ------ */
template <class iterator_t>
inline auto huffman_only_decompression_state<execution_path_t::software>::output(
        iterator_t begin, iterator_t end) noexcept -> huffman_only_decompression_state & {
    state_->current_destination_ptr = begin;
    state_->destination_available   = static_cast<uint32_t>(std::distance(begin, end));

    return *this;
}

template <class iterator_t>
inline auto huffman_only_decompression_state<execution_path_t::software>::input(
        iterator_t begin, iterator_t end) noexcept -> huffman_only_decompression_state & {
    state_->current_source_ptr = begin;
    state_->source_available   = static_cast<uint32_t>(std::distance(begin, end));

    return *this;
}

inline auto huffman_only_decompression_state<execution_path_t::software>::crc_seed(
        uint32_t seed) noexcept -> huffman_only_decompression_state & {
    state_->crc_value = seed;

    return *this;
}

inline auto huffman_only_decompression_state<execution_path_t::software>::endianness(
        endianness_t endianness) noexcept -> huffman_only_decompression_state & {
    endianness_ = endianness;

    return *this;
}

inline auto huffman_only_decompression_state<execution_path_t::software>::last_bits_offset(
        uint8_t value) noexcept -> huffman_only_decompression_state & {
    state_->last_bits_offset = value;

    return *this;
}

[[nodiscard]] inline auto huffman_only_decompression_state<execution_path_t::software>::get_input_size() const noexcept -> uint32_t {
    return state_->source_available;
}

[[nodiscard]] inline auto huffman_only_decompression_state<execution_path_t::software>::get_endianness() noexcept -> endianness_t {
    return endianness_;
}

[[nodiscard]] inline auto huffman_only_decompression_state<execution_path_t::software>::get_lookup_table() noexcept -> uint8_t * {
    return lookup_table_ptr_;
}

[[nodiscard]] inline auto huffman_only_decompression_state<execution_path_t::software>::get_buffer() noexcept -> uint8_t * {
    return huffman_only_buffer_ptr_;
}

[[nodiscard]] inline auto huffman_only_decompression_state<execution_path_t::software>::get_fields() noexcept -> internal_state_fields_t & {
    return *state_;
}

/* ------ HARDWARE STATE METHODS ------ */
template <class iterator_t>
inline auto huffman_only_decompression_state<execution_path_t::hardware>::output(iterator_t begin, iterator_t end)
noexcept -> huffman_only_decompression_state & {
    hw_iaa_descriptor_set_output_buffer(descriptor_, begin, static_cast<uint32_t>(std::distance(begin, end)));

    return *this;
}

template <class iterator_t>
inline auto huffman_only_decompression_state<execution_path_t::hardware>::input(iterator_t begin, iterator_t end)
noexcept -> huffman_only_decompression_state & {
    input_size_ = static_cast<uint32_t>(std::distance(begin, end));
    hw_iaa_descriptor_set_input_buffer(descriptor_, begin, input_size_);

    return *this;
}

inline auto huffman_only_decompression_state<execution_path_t::hardware>::crc_seed(
        uint32_t seed) noexcept -> huffman_only_decompression_state & {
    crc_ = seed;

    return *this;
}

inline auto huffman_only_decompression_state<execution_path_t::hardware>::endianness(
        endianness_t endianness) noexcept -> huffman_only_decompression_state & {
    endianness_ = endianness;

    return *this;
}

inline auto huffman_only_decompression_state<execution_path_t::hardware>::decompress_table(
        decompression_huffman_table &decompression_table) noexcept -> huffman_only_decompression_state & {
    //todo move logic to canned utilities
    {
        auto aecs_ptr = reinterpret_cast<hw_iaa_aecs_analytic *>(decompression_table.get_hw_decompression_state());
        auto huffman_table_ptr = decompression_table.get_sw_decompression_table();

        hw_iaa_aecs_decompress_set_huffman_only_huffman_table(&aecs_ptr->inflate_options, huffman_table_ptr);
    }

    decompress_aecs_ = reinterpret_cast<hw_iaa_aecs_analytic *>(decompression_table.get_hw_decompression_state());
    return *this;
}

inline auto huffman_only_decompression_state<execution_path_t::hardware>::build_descriptor() noexcept -> hw_descriptor * {
    hw_iaa_aecs_decompress_set_crc_seed(decompress_aecs_, crc_);

    hw_iaa_descriptor_init_huffman_only_decompress(descriptor_, decompress_aecs_, endianness_, ignore_end_bits);

    hw_iaa_descriptor_set_completion_record(descriptor_, completion_record_);

    return descriptor_;
}

[[nodiscard]] inline auto huffman_only_decompression_state<execution_path_t::hardware>::handler()
const noexcept -> HW_PATH_VOLATILE hw_completion_record * {
    return completion_record_;
}

[[nodiscard]] inline auto huffman_only_decompression_state<execution_path_t::hardware>::get_input_size() const noexcept -> uint32_t {
    return input_size_;
}

}
#endif // QPL_SOURCES_MIDDLE_LAYER_COMPRESSION_HUFFMAN_ONLY_DECOMPRESSION_STATE_HPP
