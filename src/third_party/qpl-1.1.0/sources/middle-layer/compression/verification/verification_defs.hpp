/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

#ifndef QPL_MIDDLE_LAYER_COMPRESSION_VERIFICATION_DEFS_HPP
#define QPL_MIDDLE_LAYER_COMPRESSION_VERIFICATION_DEFS_HPP

#include <cstdint>
#include "compression/huffman_table/inflate_huffman_table.hpp"

namespace qpl::ml::compression {
enum verification_mode_t {
    verify_deflate_default,
    verify_deflate_no_headers
};

enum parser_position_t {
    verify_header,
    verify_body
};

enum class parser_status_t {
    ok,
    need_more_input,
    end_of_block,
    end_of_mini_block,
    final_end_of_block,
    error
};

struct verification_result_t {
    uint32_t bits_read = 0;
    uint32_t bytes_written = 0;
    uint32_t crc_value = 0;
    parser_status_t status = parser_status_t::ok;

    auto operator +=(const verification_result_t &other) noexcept -> verification_result_t & {
        this->bits_read += other.bits_read;
        this->bytes_written += other.bytes_written;
        this->crc_value = other.crc_value;
        this->status = other.status;
        return *this;
    }
};

// @todo: remove
struct verification_state_buffer {
    isal_inflate_state inflate_state;
    uint32_t total_bytes_written;
    uint32_t current_deflate_block_bytes_written;
    uint32_t total_bits_read;
    uint32_t crc;
    parser_position_t parser_position;
};

}

#endif // QPL_MIDDLE_LAYER_COMPRESSION_VERIFICATION_DEFS_HPP
