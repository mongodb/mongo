/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

#include "verification_units.hpp"
#include "util/checksum.hpp"

namespace qpl::ml::compression {
auto verify_deflate_header(verify_state<execution_path_t::software> &state) noexcept -> verification_result_t {
    verification_result_t result{};

    auto &inflate_state = *state.get_state();

    if (inflate_state.avail_in == 0u &&
        inflate_state.read_in_length == 0u) {
        result.status = parser_status_t::need_more_input;
        return result;
    }

    const uint32_t initial_bits_in_buffer = inflate_state.read_in_length;
    uint8_t *initial_next_in_ptr = inflate_state.next_in;
    uint64_t initial_read_in_buffer = inflate_state.read_in;

    parser_status_t parser_status = parser_status_t::ok;

    auto decompression_status = read_header_stateful(inflate_state);

    if (status_list::ok == decompression_status) {
        const uint32_t bytes_read = static_cast<uint32_t>(std::distance(initial_next_in_ptr, inflate_state.next_in));

        const uint32_t actual_bits_read = bytes_read * byte_bits_size +
                                          initial_bits_in_buffer -
                                          inflate_state.read_in_length;

        result.bits_read += actual_bits_read;

        if (ISAL_BLOCK_HDR == inflate_state.block_state) {
            // Header was not fully read recover previous state, so the header could be read later
            inflate_state.read_in_length = initial_bits_in_buffer;
            inflate_state.read_in = initial_read_in_buffer;
            inflate_state.next_in = initial_next_in_ptr;

            parser_status = parser_status_t::need_more_input;
        } else if (ISAL_BLOCK_TYPE0 == inflate_state.block_state &&
                   0u == inflate_state.type0_block_len) {
            parser_status = parser_status_t::error;
        } else {
            parser_status = parser_status_t::ok;
        }
    } else {
        parser_status = parser_status_t::error;
    }

    result.status = parser_status;
    return result;
}

auto verify_deflate_stream_body(verify_state<execution_path_t::software> &state) noexcept -> verification_result_t {
    verification_result_t result{};
    auto &inflate_state = *state.get_state();
    const uint32_t initial_bits_in_buffer = inflate_state.read_in_length;
    uint8_t *initial_next_in_ptr = inflate_state.next_in;
    uint8_t *initial_next_out_ptr = inflate_state.next_out;

    if (inflate_state.write_overflow_len != 0) {
        *inflate_state.next_out = static_cast<uint8_t>(inflate_state.write_overflow_lits);
        inflate_state.next_out++;
        inflate_state.total_out++;
        inflate_state.avail_out--;

        inflate_state.write_overflow_len--;
    }

    if (inflate_state.copy_overflow_length != 0) {
        // The match didn't fit into the output, mini block is overflowed
        result.status = parser_status_t::error;
        return result;
    }

    if (inflate_state.avail_in == 0u &&
        inflate_state.read_in_length == 0u) {
        result.status = parser_status_t::need_more_input;
        return result;
    }

    auto status = static_cast<qpl_ml_status>(parser_status_t::ok);

    if (inflate_state.block_state == ISAL_BLOCK_CODED) {
        status = isal_kernels::decode_huffman_code_block(inflate_state, state.get_output_data());
    } else if (inflate_state.block_state == ISAL_BLOCK_TYPE0) {
        status = decode_literal_block(inflate_state);
    } else {
        result.status = parser_status_t::error;
        return result;
    }

    // Calculate all bits and bytes recently read and written, update corresponding fields
    const uint32_t bytes_written_during_current_iteration = static_cast<uint32_t>(std::distance(initial_next_out_ptr, inflate_state.next_out));
    result.bytes_written = bytes_written_during_current_iteration;

    const uint32_t bytes_read = static_cast<uint32_t>(std::distance(initial_next_in_ptr, inflate_state.next_in));

    const uint32_t actual_bits_read = bytes_read * 8u +
                                      initial_bits_in_buffer -
                                      inflate_state.read_in_length;

    result.bits_read += actual_bits_read;

    /* Possible states after recent actions:
        1. EOB symbol was met
        2. bfinal EOB symbol was met
        3. Output buffer overflow
        4. Input buffer overflow
        5. Incorrect block
     */

    if (ISAL_BLOCK_NEW_HDR == inflate_state.block_state &&
        inflate_state.bfinal == 0) {
        // EOB symbol (not bfinal) was met
        if (inflate_state.total_out > 0) {
            // EOB symbol could be the first symbol of the compressed stream, so there won't be any actual output
            result.crc_value = util::crc32_gzip(initial_next_out_ptr, initial_next_out_ptr + bytes_written_during_current_iteration, inflate_state.crc);
        }

        result.status = parser_status_t::end_of_block;
    } else if (ISAL_BLOCK_INPUT_DONE == inflate_state.block_state &&
               inflate_state.bfinal == 1) {
        // bfinal EOB symbol was met
        if (inflate_state.total_out > 0) {
            result.crc_value = util::crc32_gzip(initial_next_out_ptr, initial_next_out_ptr + bytes_written_during_current_iteration, inflate_state.crc);
        }

        result.status = parser_status_t::final_end_of_block;
    } else if (status == status_list::more_output_needed) {
        // Output overflow, start the new mini block
        result.crc_value = util::crc32_gzip(initial_next_out_ptr, initial_next_out_ptr + bytes_written_during_current_iteration, inflate_state.crc);
        result.status = parser_status_t::end_of_mini_block;
    } else if (status_list::input_too_small == status) {
        // More input needed
        result.crc_value = util::crc32_gzip(initial_next_out_ptr, initial_next_out_ptr + bytes_written_during_current_iteration, inflate_state.crc);
        result.status = parser_status_t::need_more_input;
    } else {
        result.status = parser_status_t::error;
    }

    return result;
}
}
