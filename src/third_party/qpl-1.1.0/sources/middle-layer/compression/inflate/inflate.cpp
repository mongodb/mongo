/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

#include <cstring>

#include "inflate.hpp"
#include "inflate_state.hpp"

#include "common/bit_buffer.hpp"
#include "deflate_header_decompression.hpp"
#include "deflate_body_decompression.hpp"
#include "isal_kernels_wrappers.hpp"

#include "util/memory.hpp"
#include "util/descriptor_processing.hpp"

namespace qpl::ml::compression {

namespace utility {

static auto inline is_inflate_complete(end_processing_condition_t end_condition,
                                       isal_inflate_state &inflate_state) noexcept -> bool;

static auto try_to_setup_decoding_into_internal_buffer(isal_inflate_state &inflate_state) noexcept -> bool;

static void handle_internal_buffers_overflow(isal_inflate_state &inflate_state) noexcept;

static auto flush_tmp_out_buffer(isal_inflate_state &inflate_state) noexcept -> qpl_ml_status;

static inline void ignore_last_bits(isal_inflate_state &inflate_state, uint32_t number_of_bits) noexcept;

}

// ------ SOFTWARE PATH ------ //

static auto inflate_pass(isal_inflate_state &inflate_state,
                         uint8_t *output_start_ptr) noexcept -> qpl_ml_status;

static auto own_inflate(inflate_state<execution_path_t::software> &decompression_state,
                        end_processing_condition_t end_processing_condition) noexcept -> decompression_operation_result_t;

static auto own_inflate_random(inflate_state<execution_path_t::software> &decompression_state) noexcept -> decompression_operation_result_t;

template<execution_path_t path, inflate_mode_t mode>
auto inflate(inflate_state<path> &decompression_state,
             end_processing_condition_t end_processing_condition) noexcept -> decompression_operation_result_t {
    decompression_operation_result_t result;
    auto inflate_state      = decompression_state.build_state();
    auto saved_next_in_ptr  = inflate_state->next_in;
    auto saved_next_out_ptr = inflate_state->next_out;

    // Work with mini-blocks
    if (decompression_state.access_properties_.is_random) {
        result = own_inflate_random(decompression_state);
    } else {
        result = own_inflate(decompression_state, end_processing_condition);
    }

    utility::ignore_last_bits(*inflate_state, decompression_state.access_properties_.ignore_end_bits);

    result.completed_bytes_  = static_cast<uint32_t>(inflate_state->next_in - saved_next_in_ptr);
    result.output_bytes_     = static_cast<uint32_t>(inflate_state->next_out - saved_next_out_ptr);

    decompression_state.in_progress();

    // Prevent overwrite of inflate errors by buffer overflow errors
    if (result.status_code_ == status_list::ok){
        if (decompression_state.is_final() && (decompression_state.get_state()->tmp_out_processed != 
                                               decompression_state.get_state()->tmp_out_valid)) {
            result.status_code_ = status_list::more_output_needed;
        }
    }

    return result;
}

template
auto inflate<execution_path_t::software, inflate_mode_t::inflate_default>(
        inflate_state<execution_path_t::software> &decompression_state,
        end_processing_condition_t end_processing_condition) noexcept -> decompression_operation_result_t;

template
auto inflate<execution_path_t::software, inflate_mode_t::inflate_header>(
        inflate_state<execution_path_t::software> &decompression_state,
        end_processing_condition_t end_processing_condition) noexcept -> decompression_operation_result_t;

template
auto inflate<execution_path_t::software, inflate_mode_t::inflate_body>(
        inflate_state<execution_path_t::software> &decompression_state,
        end_processing_condition_t end_processing_condition) noexcept -> decompression_operation_result_t;

static auto own_inflate(inflate_state<execution_path_t::software> &decompression_state,
                        end_processing_condition_t end_processing_condition) noexcept -> decompression_operation_result_t {

    auto inflate_state_ptr = decompression_state.get_state();

    decompression_operation_result_t result;

    uint8_t  *output_start_ptr      = inflate_state_ptr->next_out;
    uint32_t saved_output_available = inflate_state_ptr->avail_out;

    bool is_internal_buffer_available = utility::try_to_setup_decoding_into_internal_buffer(*inflate_state_ptr);

    if (is_internal_buffer_available) {
        // As we're decompressing into tmp_out_buffer, get corresponding start_out_ptr
        uint8_t *temporary_start_out_ptr = inflate_state_ptr->tmp_out_buffer;
        bool    do_next_inflate_pass     = (inflate_state_ptr->block_state != ISAL_BLOCK_INPUT_DONE);

        // Main pipeline cycle
        while (do_next_inflate_pass) {
            result.status_code_ = inflate_pass(*inflate_state_ptr, temporary_start_out_ptr);

            if (status_list::ok != result.status_code_) {
                break;
            }

            // Check if inflate should be stopped
            do_next_inflate_pass = !utility::is_inflate_complete(end_processing_condition,
                                                                 *inflate_state_ptr);
        }
        inflate_state_ptr->tmp_out_valid = static_cast<int32_t>(inflate_state_ptr->next_out - inflate_state_ptr->tmp_out_buffer);

        utility::handle_internal_buffers_overflow(*inflate_state_ptr);

        /* Setup state for decompressing into out_buffer */
        inflate_state_ptr->next_out  = output_start_ptr;
        inflate_state_ptr->avail_out = saved_output_available;
    }

    auto flush_status = utility::flush_tmp_out_buffer(*inflate_state_ptr);
    /* Prevent overwrite of inflate pass errors by flush_tmp_out_buffer errors */
    if (status_list::ok != flush_status && status_list::ok == result.status_code_) { 
        result.status_code_ = flush_status;
    }

    if (result.status_code_ >= status_list::hardware_error_base) {
        /* Set total_out to not count data in tmp_out_buffer */
        inflate_state_ptr->total_out -= inflate_state_ptr->tmp_out_valid - inflate_state_ptr->tmp_out_processed;

        return result;
    }

    /* If all data from tmp_out buffer has been processed, start
     * decompressing into the out buffer */
    if (inflate_state_ptr->tmp_out_processed == inflate_state_ptr->tmp_out_valid) {
        // Check if inflate should be continued
        bool do_next_inflate_pass = !utility::is_inflate_complete(end_processing_condition,
                                                                  *inflate_state_ptr);

        // If there was no decompression into internal buffer and we are ready to
        // Read new deflate header, then next_inflate_pass should be true, regardless of
        // what end_processing_condition is
        if (is_internal_buffer_available == false &&
            inflate_state_ptr->block_state == ISAL_BLOCK_NEW_HDR) {
            do_next_inflate_pass = true;
        }

        // Main pipeline cycle
        while (do_next_inflate_pass) {
            result.status_code_ = inflate_pass(*inflate_state_ptr, output_start_ptr);

            if (status_list::ok != result.status_code_) {
                break; //todo really break?
            }

            // Check if inflate should be stopped
            do_next_inflate_pass = !utility::is_inflate_complete(end_processing_condition,
                                                                 *inflate_state_ptr);
        }
    }

    const uint32_t bytes_in_internal_buffer = inflate_state_ptr->copy_overflow_length +
                                              inflate_state_ptr->write_overflow_len +
                                              inflate_state_ptr->tmp_out_valid;

    bool is_internal_buffer_overflowed = bytes_in_internal_buffer > sizeof(inflate_state_ptr->tmp_out_buffer);

    if (ISAL_BLOCK_INPUT_DONE != inflate_state_ptr->block_state ||
        is_internal_buffer_overflowed) {

        if (inflate_state_ptr->tmp_out_valid == inflate_state_ptr->tmp_out_processed
            // Save decompression history into tmp_out buffer
            && saved_output_available - inflate_state_ptr->avail_out >= ISAL_DEF_HIST_SIZE) {

            util::copy(inflate_state_ptr->next_out - ISAL_DEF_HIST_SIZE,
                       inflate_state_ptr->next_out,
                       inflate_state_ptr->tmp_out_buffer);

            inflate_state_ptr->tmp_out_valid     = ISAL_DEF_HIST_SIZE;
            inflate_state_ptr->tmp_out_processed = ISAL_DEF_HIST_SIZE;

        } else if (inflate_state_ptr->tmp_out_processed >= ISAL_DEF_HIST_SIZE) {
            // Move decompression history to the beginning of the buffer
            uint32_t shift_size = inflate_state_ptr->tmp_out_valid - ISAL_DEF_HIST_SIZE;

            if (shift_size > static_cast<uint32_t>(inflate_state_ptr->tmp_out_processed)) {
                shift_size = static_cast<uint32_t>(inflate_state_ptr->tmp_out_processed);
            }

            memmove(inflate_state_ptr->tmp_out_buffer,
                    &inflate_state_ptr->tmp_out_buffer[shift_size],
                    inflate_state_ptr->tmp_out_valid - shift_size);

            inflate_state_ptr->tmp_out_valid -= shift_size;
            inflate_state_ptr->tmp_out_processed -= shift_size;
        }
    }

    utility::handle_internal_buffers_overflow(*inflate_state_ptr);

    if (inflate_state_ptr->block_state == ISAL_BLOCK_INPUT_DONE &&
        inflate_state_ptr->tmp_out_valid == inflate_state_ptr->tmp_out_processed) {
        inflate_state_ptr->block_state = ISAL_BLOCK_FINISH;

        // todo isal read all bytes in the source and we need to set avail int to position after feob symbol
        // actualize_avail_in(inflate_state_ptr);
    }

    // Don't count bytes, which has not been processed yet
    inflate_state_ptr->total_out -= inflate_state_ptr->tmp_out_valid - inflate_state_ptr->tmp_out_processed;

    result.status_code_ = qpl::ml::status_list::ok;

    return result;
}


auto own_inflate_random(inflate_state<execution_path_t::software> &decompression_state) noexcept -> decompression_operation_result_t {
    auto inflate_state_ptr = decompression_state.get_state();

    // Obtain eob symbol properties
    const uint32_t eob_code_length = inflate_state_ptr->eob_code_and_len >> (byte_bits_size * 2);
    const auto     eob_mask        = util::build_mask<uint32_t>(eob_code_length);
    const uint32_t eob_code        = inflate_state_ptr->eob_code_and_len & eob_mask;

    uint64_t tail_bytes = 0u;
    uint32_t bit_count  = 0u;

    if (inflate_state_ptr->avail_in > 0u) {
        tail_bytes = inflate_state_ptr->next_in[inflate_state_ptr->avail_in - 1u] &
                     ((1u << (byte_bits_size - decompression_state.access_properties_.ignore_end_bits)) - 1u);

        // Avoiding the decoding of the last byte
        inflate_state_ptr->avail_in--;
        bit_count = (byte_bits_size - decompression_state.access_properties_.ignore_end_bits) + eob_code_length;
        tail_bytes |= (eob_code << (byte_bits_size - decompression_state.access_properties_.ignore_end_bits));
    } else {
        bit_count  = eob_code_length;
        tail_bytes = eob_code;
    }

    inflate_state_ptr->total_out   = 0u;
    inflate_state_ptr->block_state = ISAL_BLOCK_CODED; // todo: Incorrect behavior in case if block is actually stored, fix

    // Decompress mini block body
    auto result = own_inflate(decompression_state, stop_on_any_eob);

    // Now perform tail bytes decompression
    if (status_list::ok == result.status_code_) {
        inflate_state_ptr->read_in |= (tail_bytes << static_cast<uint32_t>(inflate_state_ptr->read_in_length));
        inflate_state_ptr->read_in_length += bit_count;
        inflate_state_ptr->next_in = reinterpret_cast<uint8_t *>(&tail_bytes);

        result = own_inflate(decompression_state, stop_on_any_eob);
    }

    return result;
}

static auto inflate_pass(isal_inflate_state &inflate_state,
                         uint8_t *output_start_ptr) noexcept -> qpl_ml_status {
    auto status = status_list::ok;

    if (ISAL_BLOCK_NEW_HDR == inflate_state.block_state ||
        ISAL_BLOCK_HDR == inflate_state.block_state) {
        // Read new deflate header here
        status = read_header_stateful(inflate_state);

        if (status) {
            return status;
        }
    }

    if (ISAL_BLOCK_TYPE0 == inflate_state.block_state) {
        status = decode_literal_block(inflate_state);
    } else {
        status = isal_kernels::decode_huffman_code_block(inflate_state, output_start_ptr);
    }

    return status;
}

// ------ HARDWARE PATH ------ //

// @todo refactor
template<inflate_mode_t mode>
static inline auto own_inflate(inflate_state<execution_path_t::hardware> &decompression_state) {
    auto descriptor        = decompression_state.build_descriptor<mode>();
    auto completion_record = decompression_state.handler();

    auto result = util::process_descriptor<decompression_operation_result_t,
                                           util::execution_mode_t::sync>(descriptor, completion_record);

    if (result.status_code_ == status_list::ok) {
        result.completed_bytes_ = decompression_state.get_input_size();
    }

    return result;
}

template<>
auto inflate<execution_path_t::hardware, inflate_mode_t::inflate_default>(
        inflate_state<execution_path_t::hardware> &decompression_state,
        end_processing_condition_t end_processing_condition) noexcept -> decompression_operation_result_t {
    decompression_state.stop_check_condition(end_processing_condition);

    return own_inflate<inflate_mode_t::inflate_default>(decompression_state);
}

template<>
auto inflate<execution_path_t::hardware, inflate_mode_t::inflate_header>(
        inflate_state<execution_path_t::hardware> &decompression_state,
        end_processing_condition_t UNREFERENCED_PARAMETER(end_processing_condition)) noexcept -> decompression_operation_result_t {

    auto result = own_inflate<inflate_mode_t::inflate_header>(decompression_state);

    // Save block type after parsing header. This information is required during body processing
    decompression_state.set_block_type(decompression_state.get_current_block_type());

    return result;
}

template<>
auto inflate<execution_path_t::hardware, inflate_mode_t::inflate_body>(
        inflate_state<execution_path_t::hardware> &decompression_state,
        end_processing_condition_t UNREFERENCED_PARAMETER(end_processing_condition)) noexcept -> decompression_operation_result_t {

    return own_inflate<inflate_mode_t::inflate_body>(decompression_state);
}

namespace utility {
static inline void ignore_last_bits(isal_inflate_state &inflate_state,
                                    uint32_t number_of_bits) noexcept {
    if (number_of_bits > 0 && inflate_state.read_in_length > 0) {
        uint32_t ignore_bits_count = static_cast<uint32_t>(inflate_state.read_in_length) > number_of_bits
                                     ? (inflate_state.read_in_length - number_of_bits)
                                     : inflate_state.read_in_length;

        inflate_state.read_in >>= ignore_bits_count;
        inflate_state.read_in_length -= ignore_bits_count;
    }
}

static auto flush_tmp_out_buffer(isal_inflate_state &inflate_state) noexcept -> qpl_ml_status {
    // Determinate maximum copy size
    qpl_ml_status status = status_list::ok;

    uint32_t copy_size = inflate_state.tmp_out_valid - inflate_state.tmp_out_processed;
    if (copy_size > inflate_state.avail_out) {
        copy_size = inflate_state.avail_out;
        status = status_list::more_output_needed;
    }

    util::copy(&inflate_state.tmp_out_buffer[inflate_state.tmp_out_processed],
               &inflate_state.tmp_out_buffer[inflate_state.tmp_out_processed] + copy_size,
               inflate_state.next_out);

    // Update related fields
    inflate_state.tmp_out_processed += copy_size;
    inflate_state.avail_out -= copy_size;
    inflate_state.next_out += copy_size;

    return status;
}

static auto inline is_inflate_complete(end_processing_condition_t end_condition,
                                       isal_inflate_state &inflate_state) noexcept -> bool {
    switch (inflate_state.block_state) {
        case ISAL_BLOCK_NEW_HDR:
            if (end_condition == stop_and_check_any_eob ||
                end_condition == stop_on_any_eob) {
                // The block was done and we should stop at any EOB and finish decompressing
                return true;
            } else {
                return false;
            }

        case ISAL_BLOCK_FINISH:
        case ISAL_BLOCK_INPUT_DONE:
            if (inflate_state.avail_in > 0u &&
                (end_condition == dont_stop_or_check ||
                 end_condition == check_for_any_eob
                )) {
                // Correct block state, so decompression could be continued
                inflate_state.block_state = ISAL_BLOCK_NEW_HDR;
                return false;
            } else {
                return true;
            }

        default:
            return false;
    }
}

static auto try_to_setup_decoding_into_internal_buffer(isal_inflate_state &inflate_state) noexcept -> bool {
    bool is_state_decoding_into_internal_buffer = true;

    if (ISAL_BLOCK_FINISH == inflate_state.block_state) {
        is_state_decoding_into_internal_buffer = false;
    } else {
        // Add bytes that has been previously compressed to total_out field
        inflate_state.total_out += (inflate_state.tmp_out_valid - inflate_state.tmp_out_processed);

        // If there's enough space in tmp_out buffer, decompress into it
        if (inflate_state.tmp_out_valid < 2 * ISAL_DEF_HIST_SIZE) {
            // next_out is the first unused byte in the tmp_out buffer
            inflate_state.next_out  = &inflate_state.tmp_out_buffer[inflate_state.tmp_out_valid];
            inflate_state.avail_out = sizeof(inflate_state.tmp_out_buffer) -
                                      ISAL_LOOK_AHEAD -
                                      inflate_state.tmp_out_valid;

            if ((int32_t) inflate_state.avail_out < 0) {
                inflate_state.avail_out = 0u;
            }
        } else {
            is_state_decoding_into_internal_buffer = false;
        }
    }

    return is_state_decoding_into_internal_buffer;
}

static void handle_internal_buffers_overflow(isal_inflate_state &inflate_state) noexcept {
    auto     *current_out_ptr = &inflate_state.tmp_out_buffer[inflate_state.tmp_out_valid];

    if (0u != inflate_state.write_overflow_len) {
        *(uint32_t *) current_out_ptr = inflate_state.write_overflow_lits;
        inflate_state.tmp_out_valid += inflate_state.write_overflow_len;
        inflate_state.total_out += inflate_state.write_overflow_len;
        current_out_ptr += inflate_state.write_overflow_len;

        inflate_state.write_overflow_len  = 0u;
        inflate_state.write_overflow_lits = 0u;
    }

    if (0u != inflate_state.copy_overflow_length) {
        auto lookback_ptr = (&inflate_state.tmp_out_buffer[inflate_state.tmp_out_valid]) -
                            inflate_state.copy_overflow_distance;

        for (int32_t i = 0u; i < inflate_state.copy_overflow_length; i++) {
            *current_out_ptr = *lookback_ptr;
            current_out_ptr++;
            lookback_ptr++;
        }

        inflate_state.tmp_out_valid += inflate_state.copy_overflow_length;
        inflate_state.total_out += inflate_state.copy_overflow_length;
        inflate_state.copy_overflow_distance = 0;
        inflate_state.copy_overflow_length   = 0;

    }
}

} // namespace utility

} // namespace qpl::ml::compression
