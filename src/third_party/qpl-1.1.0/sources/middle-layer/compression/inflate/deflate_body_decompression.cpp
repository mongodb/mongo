/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

#include "deflate_body_decompression.hpp"
#include "util/memory.hpp"

namespace qpl::ml::compression {

auto decode_literal_block(isal_inflate_state &inflate_state) noexcept -> qpl_ml_status {
    uint32_t block_byte_size    = inflate_state.type0_block_len;
    uint32_t bytes_already_read = inflate_state.read_in_length / 8u;

    /* If the block is uncompressed, perform a memcopy while
     * updating state data */
    inflate_state.block_state = inflate_state.bfinal ? ISAL_BLOCK_INPUT_DONE : ISAL_BLOCK_NEW_HDR;

    if (inflate_state.avail_out < block_byte_size) {
        // We can process up to inflate_state.avail_out bytes
        block_byte_size = inflate_state.avail_out;
        inflate_state.block_state = ISAL_BLOCK_TYPE0;
    }

    if (inflate_state.avail_in + bytes_already_read < block_byte_size) {
        // Literal block decompressing will not be done at this pass
        block_byte_size = inflate_state.avail_in + bytes_already_read;
        inflate_state.block_state = ISAL_BLOCK_TYPE0;
    }

    auto *bits_buffer_ptr = reinterpret_cast<uint8_t *>(&inflate_state.read_in);

    util::copy(bits_buffer_ptr,
               bits_buffer_ptr + bytes_already_read,
               inflate_state.next_out);

    if (inflate_state.read_in_length) {
        if (block_byte_size >= bytes_already_read) {
            // We already read a part of current block
            inflate_state.next_out += bytes_already_read;
            inflate_state.avail_out -= bytes_already_read;
            inflate_state.total_out += bytes_already_read;
            inflate_state.type0_block_len -= bytes_already_read;

            inflate_state.read_in        = 0;
            inflate_state.read_in_length = 0;
            block_byte_size -= bytes_already_read;
            bytes_already_read           = 0;

        } else {
            // Current block is stored in internal buffer
            inflate_state.next_out += block_byte_size;
            inflate_state.avail_out -= block_byte_size;
            inflate_state.total_out += block_byte_size;
            inflate_state.type0_block_len -= block_byte_size;

            inflate_state.read_in >>= 8 * block_byte_size;
            inflate_state.read_in_length -= 8 * block_byte_size;
            bytes_already_read -= block_byte_size;
            block_byte_size = 0;
        }
    }

    util::copy(inflate_state.next_in,
               inflate_state.next_in + block_byte_size,
               inflate_state.next_out);

    // Update input / output fields
    inflate_state.next_out += block_byte_size;
    inflate_state.avail_out -= block_byte_size;
    inflate_state.total_out += block_byte_size;
    inflate_state.next_in += block_byte_size;
    inflate_state.avail_in -= block_byte_size;

    inflate_state.type0_block_len -= block_byte_size;

    if (inflate_state.avail_in + bytes_already_read == 0 &&
        inflate_state.block_state != ISAL_BLOCK_INPUT_DONE) {
        return status_list::input_too_small;
    }

    if (inflate_state.avail_out == 0 &&
        inflate_state.type0_block_len > 0) {
        return status_list::more_output_needed;
    }

    return status_list::ok;
}

} // namespace qpl::ml::compression
