/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

#include "deflate_header_decompression.hpp"
#include "util/memory.hpp"
#include "isal_kernels_wrappers.hpp"

namespace qpl::ml::compression {

auto read_header_stateful(isal_inflate_state &inflate_state) noexcept-> qpl_ml_status {
    auto read_in_start = inflate_state.read_in;
    auto read_in_length_start = inflate_state.read_in_length;
    auto avail_in_start = inflate_state.avail_in;
    auto *next_in_start_ptr = inflate_state.next_in;
    auto block_state_start = inflate_state.block_state;

    if (ISAL_BLOCK_HDR == block_state_start) {
        /* Setup so read_header decodes data in tmp_in_buffer */
        auto copy_size = static_cast<uint32_t>(ISAL_DEF_MAX_HDR_SIZE - inflate_state.tmp_in_size);

        if (copy_size > inflate_state.avail_in) {
            copy_size = inflate_state.avail_in;
        }

        util::copy(inflate_state.next_in,
                   inflate_state.next_in + copy_size,
                   &inflate_state.tmp_in_buffer[inflate_state.tmp_in_size]);

        inflate_state.next_in = inflate_state.tmp_in_buffer;
        inflate_state.avail_in = inflate_state.tmp_in_size + copy_size;
    }

    auto status = isal_kernels::read_deflate_header(inflate_state);

    if (status > status_list::hardware_error_base) {
        return status;
    }

    if (ISAL_BLOCK_HDR == block_state_start) {
        /* Setup so state is restored to a valid state */
        auto bytes_read = static_cast<int>(inflate_state.next_in - inflate_state.tmp_in_buffer - inflate_state.tmp_in_size);

        if (bytes_read < 0) {
            bytes_read = 0;
        }

        inflate_state.next_in = next_in_start_ptr + bytes_read;
        inflate_state.avail_in = avail_in_start - bytes_read;
    }

    // Check if header reading is complete
    if (status_list::input_too_small == status) {
        /* Save off data so header can be decoded again with more data */
        inflate_state.read_in = read_in_start;
        inflate_state.read_in_length = read_in_length_start;

        util::copy(next_in_start_ptr,
                   next_in_start_ptr + avail_in_start,
                   &inflate_state.tmp_in_buffer[inflate_state.tmp_in_size]);

        inflate_state.tmp_in_size += avail_in_start;
        inflate_state.avail_in = 0;
        inflate_state.next_in = next_in_start_ptr + avail_in_start;

        if (inflate_state.tmp_in_size > 0) {
            inflate_state.block_state = ISAL_BLOCK_HDR; /* Changing block state */
        }
    }
    else {
        inflate_state.tmp_in_size = 0;
    }

    return status;
}

} // namespace qpl::ml::compression
