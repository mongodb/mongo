/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

#ifndef QPL_SOURCES_MIDDLE_LAYER_VERIFICATION_VERIFY_STATE_HPP
#define QPL_SOURCES_MIDDLE_LAYER_VERIFICATION_VERIFY_STATE_HPP

#include "compression/inflate/deflate_header_decompression.hpp"
#include "common/defs.hpp"
#include "util/memory.hpp"
#include "common/linear_allocator.hpp"
#include "verification_defs.hpp"
#include "compression/huffman_table/inflate_huffman_table.hpp"
#include "util/util.hpp"
#include "compression/utils.hpp"

namespace qpl::ml::compression {
template <execution_path_t path>
class verify_state;

template<execution_path_t path>
class verification_state_builder;

template <>
class verify_state<execution_path_t::software> {
private:
    friend class verification_state_builder<execution_path_t::software>;

public:
    template <class iterator_t>
    inline auto input(iterator_t begin, iterator_t end) noexcept -> verify_state &;

    inline auto first(bool value) noexcept -> verify_state &;

    inline auto decompress_table(uint8_t *deflate_header_ptr,
                                 uint32_t deflate_header_bits) noexcept -> verify_state &;

    inline auto required_crc(uint32_t crc) noexcept -> verify_state &;

    inline auto crc_seed(uint32_t seed) noexcept -> verify_state &;

    inline auto set_parser_position(parser_position_t value) noexcept -> verify_state &;

    inline auto get_parser_position() noexcept -> parser_position_t;

    inline auto reset_miniblock_state() noexcept -> verify_state &;

    inline auto reset_state() noexcept -> verify_state &;

    [[nodiscard]] inline auto is_first() const noexcept -> bool;

    [[nodiscard]] inline auto get_input_data() const noexcept -> uint8_t *;

    [[nodiscard]] inline auto get_input_size() const noexcept -> uint32_t;

    [[nodiscard]] inline auto get_required_crc() const noexcept -> uint32_t;

    [[nodiscard]] inline auto get_output_data() const noexcept -> uint8_t *;

    [[nodiscard]] inline auto get_crc() const noexcept -> uint32_t;

    [[nodiscard]] inline auto get_state() -> isal_inflate_state *;

    [[nodiscard]] constexpr static inline auto get_buffer_size() noexcept -> uint32_t {
        size_t size = 0;
        size += sizeof(state_buffer);
        size += sizeof(uint8_t)*32_kb;

        return static_cast<uint32_t>(util::align_size(size, 1_kb));
    }

private:
    inline void reset() noexcept {
        verify_state_ptr->state_ptr.disable_multisymbol_lookup_table = 1u;
    }

    explicit verify_state(const qpl::ml::util::linear_allocator &allocator) {
        verify_state_ptr = allocator.allocate<state_buffer, qpl::ml::util::memory_block_t::not_aligned>(1u);
        verify_state_ptr->decompression_buffer_ptr  = allocator.allocate<uint8_t, util::memory_block_t::not_aligned>(32_kb);
        verify_state_ptr->decompression_buffer_size = 32_kb;
    };

    struct state_buffer {
        parser_position_t  parser_position = parser_position_t::verify_header;
        uint8_t            *decompression_buffer_ptr;
        uint32_t           decompression_buffer_size;
        isal_inflate_state state_ptr;
        uint32_t           crc;
    } *verify_state_ptr;

    bool     is_first_;
    uint32_t required_crc_value;
};

template <>
class verify_state<execution_path_t::hardware> {
public:
    // for symmetry, no allocations are required for this state
    [[nodiscard]] constexpr static inline auto get_buffer_size() noexcept -> uint32_t {
        return 0;
    }
};

// ------ SOFTWARE PATH ------ //
template <class iterator_t>
inline auto verify_state<execution_path_t::software>::input(iterator_t begin,
                                                            iterator_t end) noexcept -> verify_state & {
    verify_state_ptr->state_ptr.next_in  = begin;
    verify_state_ptr->state_ptr.avail_in = static_cast<uint32_t>(std::distance(begin, end));
    return *this;
}

inline auto verify_state<execution_path_t::software>::decompress_table(uint8_t *deflate_header_ptr,
                                                                       uint32_t deflate_header_bits) noexcept -> verify_state & {
    auto saved_next_in_ptr    = verify_state_ptr->state_ptr.next_in;
    auto saved_avail_in       = verify_state_ptr->state_ptr.avail_in;
    auto saved_read_in        = verify_state_ptr->state_ptr.read_in;
    auto saved_read_in_length = verify_state_ptr->state_ptr.read_in_length;

    verify_state_ptr->state_ptr.read_in        = 0;
    verify_state_ptr->state_ptr.read_in_length = 0;
    verify_state_ptr->state_ptr.next_in        = deflate_header_ptr;
    verify_state_ptr->state_ptr.avail_in       = (deflate_header_bits + 7u) >> 3;

    auto status = read_header_stateful(verify_state_ptr->state_ptr);
    MAYBE_UNUSED(status);

    verify_state_ptr->state_ptr.next_in        = saved_next_in_ptr;
    verify_state_ptr->state_ptr.avail_in       = saved_avail_in;
    verify_state_ptr->state_ptr.read_in        = saved_read_in;
    verify_state_ptr->state_ptr.read_in_length = saved_read_in_length;

    verify_state_ptr->state_ptr.block_state = ISAL_BLOCK_CODED;

    return *this;
}

inline auto verify_state<execution_path_t::software>::required_crc(uint32_t crc) noexcept -> verify_state & {
    required_crc_value = crc;
    return *this;
}

inline auto verify_state<execution_path_t::software>::set_parser_position(parser_position_t value) noexcept -> verify_state & {
    verify_state_ptr->parser_position = value;
    return *this;
}

inline auto verify_state<execution_path_t::software>::first(bool value) noexcept -> verify_state & {
    is_first_ = value;
    return *this;
}

inline auto verify_state<execution_path_t::software>::get_parser_position() noexcept -> parser_position_t {
    return verify_state_ptr->parser_position;
}

inline auto verify_state<execution_path_t::software>::crc_seed(uint32_t seed) noexcept -> verify_state & {
    verify_state_ptr->crc = seed;

    return *this;
}

inline auto verify_state<execution_path_t::software>::reset_miniblock_state() noexcept -> verify_state & {
    verify_state_ptr->state_ptr.next_out  = verify_state_ptr->decompression_buffer_ptr;
    verify_state_ptr->state_ptr.avail_out = verify_state_ptr->decompression_buffer_size;
    verify_state_ptr->state_ptr.total_out = 0;

    return *this;
}

inline auto verify_state<execution_path_t::software>::reset_state() noexcept -> verify_state & {
    reset_inflate_state(&verify_state_ptr->state_ptr);

    return *this;
}

[[nodiscard]] inline auto verify_state<execution_path_t::software>::is_first() const noexcept -> bool {
    return is_first_;
}

[[nodiscard]] inline auto verify_state<execution_path_t::software>::get_input_data() const noexcept -> uint8_t * {
    return verify_state_ptr->state_ptr.next_in;
}

[[nodiscard]] inline auto verify_state<execution_path_t::software>::get_input_size() const noexcept -> uint32_t {
    return verify_state_ptr->state_ptr.avail_in
           + util::bit_to_byte(static_cast<uint32_t>(verify_state_ptr->state_ptr.read_in_length));
}

[[nodiscard]] inline auto verify_state<execution_path_t::software>::get_required_crc() const noexcept -> uint32_t {
    return required_crc_value;
}

[[nodiscard]] inline auto verify_state<execution_path_t::software>::get_output_data() const noexcept -> uint8_t * {
    return verify_state_ptr->decompression_buffer_ptr;
}

[[nodiscard]] inline auto verify_state<execution_path_t::software>::get_crc() const noexcept -> uint32_t {
    return verify_state_ptr->crc;
}

[[nodiscard]] inline auto verify_state<execution_path_t::software>::get_state() -> isal_inflate_state * {
    return &verify_state_ptr->state_ptr;
}

// ------ HARDWARE PATH ------ //
// implementation goes there

}
#endif // QPL_SOURCES_MIDDLE_LAYER_VERIFICATION_VERIFY_STATE_HPP
