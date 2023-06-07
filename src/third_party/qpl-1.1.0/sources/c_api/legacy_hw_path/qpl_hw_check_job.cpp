/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

/**
 * @date 03/23/2020
 * @brief Internal HW API functions for @ref hw_check_job API implementation
 */

#include "util/descriptor_processing.hpp"
#include "job.hpp"
#include "util/memory.hpp"
#include "util/completion_record.hpp"
#include "compression/deflate/compression_units/stored_block_units.hpp"
#include "compression/stream_decorators/gzip_decorator.hpp"

#include "own_defs.h"
#include "hardware_state.h"
#include "hw_iaa_flags.h"
#include "hardware_defs.h"
#include "hw_accelerator_api.h"
#include "own_ml_submit_operation_api.hpp"
#include "util/checkers.hpp"
#include "common/defs.hpp"

namespace qpl::ml {

#define AECS_WRITTEN(p) ((((p)->op_code_op_flags >> 18u) & 3u) == AD_WRSRC2_ALWAYS)

qpl_status own_hw_compress_finalize(qpl_job *const job_ptr,
                                    qpl_hw_state *state_ptr,
                                    const uint32_t checksum) {
    state_ptr->execution_history.execution_step = qpl_task_execution_step_completed;
    if (QPL_FLAG_GZIP_MODE & job_ptr->flags) {
        if (sizeof(qpl::ml::compression::gzip_decorator::gzip_trailer) > job_ptr->available_out) {
            return QPL_STS_DST_IS_SHORT_ERR;
        }

        qpl::ml::compression::gzip_decorator::gzip_trailer trailer{};
        trailer.crc32 = checksum;
        trailer.input_size = job_ptr->total_in;

        qpl::ml::compression::gzip_decorator::write_trailer_unsafe(job_ptr->next_out_ptr, job_ptr->available_out, trailer);

        job_ptr->next_out_ptr  += sizeof(trailer);
        job_ptr->available_out -= sizeof(trailer);
        job_ptr->total_out     += sizeof(trailer);
    }

    return QPL_STS_OK;
}

qpl_status hw_check_compress_job(qpl_job *qpl_job_ptr) {
    using namespace qpl::ml::util;

    auto *const state_ptr = reinterpret_cast<qpl_hw_state *>(job::get_state(qpl_job_ptr));
    hw_iaa_analytics_descriptor  *const desc_ptr   = &state_ptr->desc_ptr;
    hw_iaa_completion_record *const comp_ptr   = &state_ptr->comp_ptr;
    hw_iaa_aecs_compress     *const cfg_in_ptr = &state_ptr->ccfg[state_ptr->aecs_hw_read_offset];
    hw_iaa_aecs_compress     *const cfg_out_ptr = &state_ptr->ccfg[state_ptr->aecs_hw_read_offset ^ 1u];
    bool  is_final_block                           = QPL_FLAG_LAST & qpl_job_ptr->flags;

    // Check verification step
    if (QPL_OPCODE_DECOMPRESS == ADOF_GET_OPCODE(desc_ptr->op_code_op_flags)) {
        OWN_QPL_CHECK_STATUS(convert_status_iaa_to_qpl(reinterpret_cast<hw_completion_record *>(comp_ptr)))

        qpl_job_ptr->idx_num_written += comp_ptr->output_size / sizeof(uint64_t);

        state_ptr->aecs_hw_read_offset ^= 1u;

        return (is_final_block && (comp_ptr->crc != state_ptr->execution_history.compress_crc))
        ? QPL_STS_INTL_VERIFY_ERR
        : QPL_STS_OK;
    }

    // Software fallback for overflowing
    if (comp_ptr->error_code == AD_ERROR_CODE_UNRECOVERABLE_OUTPUT_OVERFLOW) {
        HW_IMMEDIATELY_RET((qpl_job_ptr->flags & (QPL_FLAG_NO_HDRS)), QPL_STS_DST_IS_SHORT_ERR);

        if (qpl_job_ptr->mini_block_size) {
            HW_IMMEDIATELY_RET((64u * 1024u <= qpl_job_ptr->available_in), QPL_STS_INDEX_GENERATION_ERR)
            HW_IMMEDIATELY_RET((((qpl_job_ptr->flags & (QPL_FLAG_FIRST | QPL_FLAG_LAST)) != (QPL_FLAG_FIRST | QPL_FLAG_LAST))
            && (0u == (qpl_job_ptr->flags & (QPL_FLAG_FIRST | QPL_FLAG_START_NEW_BLOCK | QPL_FLAG_DYNAMIC_HUFFMAN)))),
                               QPL_STS_INDEX_GENERATION_ERR)
        }

        // Get AECS buffers accumulated bit size
        uint32_t bits_to_flush = (qpl_task_execution_step_header_inserting == state_ptr->execution_history.execution_step)
                ? state_ptr->saved_num_output_accum_bits
                : hw_iaa_aecs_compress_accumulator_get_actual_bits(cfg_in_ptr);

        // Insert EOB
        if (qpl_task_execution_step_header_inserting != state_ptr->execution_history.execution_step) {
            hw_iaa_aecs_compress_accumulator_insert_eob(cfg_in_ptr, state_ptr->eob_code);
            bits_to_flush +=  state_ptr->eob_code.length; // @todo recheck logic
        }

        auto stored_blocks_required_size = compression::calculate_size_needed(qpl_job_ptr->available_in, bits_to_flush);

        HW_IMMEDIATELY_RET((stored_blocks_required_size > qpl_job_ptr->available_out), QPL_STS_MORE_OUTPUT_NEEDED)

        const uint8_t *const input_data_ptr = qpl_job_ptr->next_in_ptr;
        const uint32_t input_data_size      = qpl_job_ptr->available_in;

        uint8_t *output_ptr        = qpl_job_ptr->next_out_ptr;
        const uint32_t output_size = qpl_job_ptr->available_out;
        uint32_t bytes_written     = 0u;

        // Flush AECS buffers
        HW_IMMEDIATELY_RET((256u + 64u <= bits_to_flush), QPL_STS_LIBRARY_INTERNAL_ERR);

        if (bits_to_flush) {
            hw_iaa_aecs_compress_accumulator_flush(cfg_in_ptr, &output_ptr, bits_to_flush);
            bytes_written += bits_to_flush / 8u;
        }

        bytes_written += qpl::ml::compression::write_stored_blocks(const_cast<uint8_t *>(input_data_ptr),
                                                                   input_data_size,
                                                                   output_ptr,
                                                                   output_size,
                                                                   bits_to_flush & 7u,
                                                                   is_final_block);

        // Calculate checksums
        uint32_t crc, xor_checksum;
        hw_iaa_aecs_compress_get_checksums(cfg_in_ptr, &crc, &xor_checksum);

        crc = !(qpl_job_ptr->flags & QPL_FLAG_CRC32C) ?
              util::crc32_gzip(input_data_ptr, input_data_ptr + input_data_size, crc) :
              util::crc32_iscsi_inv(input_data_ptr, input_data_ptr + input_data_size, crc);

        xor_checksum = util::xor_checksum(input_data_ptr, input_data_ptr + input_data_size, xor_checksum);

        hw_iaa_aecs_compress_set_checksums(cfg_out_ptr, crc, xor_checksum);

        job::update_input_stream(qpl_job_ptr, input_data_size);
        job::update_output_stream(qpl_job_ptr, bytes_written, bytes_written);
        job::update_checksums(qpl_job_ptr, crc, xor_checksum);

        if (is_final_block) {
            own_hw_compress_finalize(qpl_job_ptr, state_ptr, crc);
        } else {
            state_ptr->execution_history.execution_step = qpl_task_execution_step_header_inserting;
        }

        if (!(QPL_FLAG_OMIT_VERIFY & qpl_job_ptr->flags)) {
            return hw_submit_verify_job(qpl_job_ptr);
        }

        state_ptr->aecs_hw_read_offset ^= 1;

        return QPL_STS_OK;
    }

    // Validate descriptor result


    OWN_QPL_CHECK_STATUS(convert_status_iaa_to_qpl(reinterpret_cast<hw_completion_record *> (comp_ptr)))


    // Fix for QPL_FLAG_HUFFMAN_BE in ver 1.0, shall be ommited for future HW versions
    // The workaround: When writing to the AECS compress Huffman table, if V1 and the job is a LAST job,
    // and the job specifies Big-Endian-16 mode: set the Huffman code for LL[256] to be 8 bits of 00.
    // Also, set the compression flag for append EOB at end.
    // When such a job completes (i.e. one modified as above), then the output size should have
    // the low-order bit cleared (i.e. rounded down to a multiple of 2).
    if (((qpl_job_ptr->flags & (QPL_FLAG_HUFFMAN_BE | QPL_FLAG_LAST)) == (QPL_FLAG_HUFFMAN_BE | QPL_FLAG_LAST))) {
        // V1 work-around
        comp_ptr->output_size &= ~1u;
    }

    // Statistic collection: Process statistic and resubmit deflate task
    if (ADCF_STATS_MODE & desc_ptr->decomp_flags) {
        qpl_job_ptr->crc          = comp_ptr->crc;
        qpl_job_ptr->xor_checksum = comp_ptr->xor_checksum;

        hw_descriptor_compress_init_deflate_dynamic(desc_ptr, state_ptr, qpl_job_ptr, cfg_in_ptr, comp_ptr);

        auto status = process_descriptor<qpl_status,
                                         execution_mode_t::async>((hw_descriptor *) desc_ptr,
                                                                  (hw_completion_record *) &state_ptr->comp_ptr,
                                                                  qpl_job_ptr->numa_id);

        HW_IMMEDIATELY_RET(0u != status, QPL_STS_QUEUES_ARE_BUSY_ERR);

        return QPL_STS_BEING_PROCESSED;
    }

    // Body encoding step: Update Job with descriptor results
    state_ptr->config_valid = AECS_WRITTEN(desc_ptr);

    job::update_input_stream(qpl_job_ptr, qpl_job_ptr->available_in);
    job::update_output_stream(qpl_job_ptr, comp_ptr->output_size, comp_ptr->output_bits);
    job::update_checksums(qpl_job_ptr, comp_ptr->crc, comp_ptr->xor_checksum);

    if (is_final_block) {
        own_hw_compress_finalize(qpl_job_ptr, state_ptr, comp_ptr->crc);
    } else if (!(QPL_FLAG_DYNAMIC_HUFFMAN & qpl_job_ptr->flags)) {
        // Not last
        state_ptr->execution_history.execution_step = qpl_task_execution_step_data_processing;
    }

    if (!(QPL_FLAG_OMIT_VERIFY & qpl_job_ptr->flags)) {
        return hw_submit_verify_job(qpl_job_ptr);
    }

    state_ptr->aecs_hw_read_offset ^= 1u;

    return QPL_STS_OK;
}

}

extern "C" qpl_status hw_check_job (qpl_job * qpl_job_ptr) {
    using namespace qpl;
    using namespace qpl::ml::util;
    auto *const state_ptr = reinterpret_cast<qpl_hw_state *>(job::get_state(qpl_job_ptr));

    const auto *desc_ptr = &state_ptr->desc_ptr;
    const auto *comp_ptr = &state_ptr->comp_ptr;
    auto *cfg_ptr  = GET_DCFG(state_ptr);

    if (!state_ptr->job_is_submitted) {
        return QPL_STS_JOB_NOT_SUBMITTED;
    }

    if (0u == comp_ptr->status) {
        return QPL_STS_BEING_PROCESSED;
    }

    if (TRIVIAL_COMPLETE == comp_ptr->status) {
        job::update_input_stream (qpl_job_ptr, comp_ptr->bytes_completed);

        return QPL_STS_OK;
    }

    if (qpl_op_compress == qpl_job_ptr->op) {
        if(job::is_canned_mode_compression(qpl_job_ptr))
        {
            auto status = convert_status_iaa_to_qpl(reinterpret_cast<const hw_completion_record *>(comp_ptr));
            // Align with the behavior of non-canned mode compression overflow (stored block also doesn't fit), which replaces
            // the returned error code "destination_is_short_error" from Intel® In-Memory Analytics Accelerator
            // with "more_output_needed"
            if (status == qpl::ml::status_list::destination_is_short_error) {
                status = qpl::ml::status_list::more_output_needed;
            }
            OWN_QPL_CHECK_STATUS(status)

            job::update_input_stream(qpl_job_ptr, qpl_job_ptr->available_in);
            job::update_output_stream(qpl_job_ptr, comp_ptr->output_size, comp_ptr->output_bits);
            job::update_checksums(qpl_job_ptr, comp_ptr->crc, comp_ptr->xor_checksum);

            return QPL_STS_OK;
        }

        return qpl::ml::hw_check_compress_job(qpl_job_ptr);
    }

    if (job::is_canned_mode_decompression(qpl_job_ptr)) {
        OWN_QPL_CHECK_STATUS(convert_status_iaa_to_qpl(reinterpret_cast<const hw_completion_record *>(comp_ptr)))

        job::update_aggregates(qpl_job_ptr, comp_ptr->sum_agg, comp_ptr->min_first_agg, comp_ptr->max_last_agg);
        job::update_checksums(qpl_job_ptr, comp_ptr->crc, comp_ptr->xor_checksum);
        job::update_input_stream(qpl_job_ptr, desc_ptr->src1_size);
        job::update_output_stream(qpl_job_ptr, comp_ptr->output_size, comp_ptr->output_bits);

        return QPL_STS_OK;
    }

    if ((AD_STATUS_SUCCESS != comp_ptr->status) && (AD_STATUS_OUTPUT_OVERFLOW != comp_ptr->status)) {
        return static_cast<qpl_status>(convert_status_iaa_to_qpl(reinterpret_cast<const hw_completion_record *>(comp_ptr)));
    }

    HW_IMMEDIATELY_RET((0u != comp_ptr->error_code), ml::status_list::hardware_error_base + comp_ptr->error_code);

    if ((IS_RND_ACCESS_BODY(qpl_job_ptr->flags)) && (0 != qpl_job_ptr->ignore_start_bits)) {
        hw_iaa_aecs_decompress_clean_input_accumulator(&cfg_ptr->inflate_options);
    }

    if (!(IS_RND_ACCESS_BODY(qpl_job_ptr->flags))) {
        uint32_t wrSrc2 = (desc_ptr->op_code_op_flags >> 18u) & 3u;
        state_ptr->config_valid = ((AD_WRSRC2_ALWAYS == wrSrc2) || ((AD_WRSRC2_MAYBE == wrSrc2) &&
                                                                    (AD_STATUS_OUTPUT_OVERFLOW == comp_ptr->status)))
                                  ? 1u
                                  : 0u;
        FLIP_AECS_OFFSET(state_ptr);
    }

    // Update Aggregates
    if (!(qpl_op_crc64 == qpl_job_ptr->op)) {
        job::update_aggregates(qpl_job_ptr, comp_ptr->sum_agg, comp_ptr->min_first_agg, comp_ptr->max_last_agg);
        job::update_checksums(qpl_job_ptr, comp_ptr->crc, comp_ptr->xor_checksum);
    } else {
        // CRC64 Operations
        job::update_crc(qpl_job_ptr, ((uint64_t)comp_ptr->sum_agg << 32u)
                                    | (uint64_t)comp_ptr->max_last_agg);
    }

    // Update output
    uint32_t available_out = qpl_job_ptr->available_out - desc_ptr->max_dst_size;
    uint32_t bytes_written = comp_ptr->output_size;

    job::update_output_stream(qpl_job_ptr, bytes_written, comp_ptr->output_bits);

    // Update input stream
    uint32_t size;
    if (AD_STATUS_SUCCESS == comp_ptr->status) {
        size = desc_ptr->src1_size;
    } else if (AD_STATUS_OUTPUT_OVERFLOW == comp_ptr->status) {
        size = comp_ptr->bytes_completed;
    } else {
        size = 0u;
    }

    if (0u != state_ptr->accumulation_buffer.actual_bytes) {
        HW_IMMEDIATELY_RET((size > state_ptr->accumulation_buffer.actual_bytes),
                           QPL_STS_LIBRARY_INTERNAL_ERR);

        state_ptr->accumulation_buffer.actual_bytes -= size;

        HW_IMMEDIATELY_RET(((0u != state_ptr->accumulation_buffer.actual_bytes)
                            && (AD_STATUS_OUTPUT_OVERFLOW != comp_ptr->status)),
                           QPL_STS_LIBRARY_INTERNAL_ERR);

        if (0u != state_ptr->accumulation_buffer.actual_bytes) {
            ml::util::move(state_ptr->accumulation_buffer.data + size,
                           state_ptr->accumulation_buffer.data + size + state_ptr->accumulation_buffer.actual_bytes,
                           state_ptr->accumulation_buffer.data);
        }
    } else {
        job::update_input_stream(qpl_job_ptr, size);
    }

    // Handle output overflow
    if (AD_STATUS_OUTPUT_OVERFLOW == comp_ptr->status) {
        if (0u == comp_ptr->output_size) {
            // No progress was made
            return QPL_STS_DST_IS_SHORT_ERR;
        }
        if (0u == available_out) {
            return QPL_STS_MORE_OUTPUT_NEEDED;
        }
        // The application gave us a large output buffer, but we could only use 2MB of it,
        // which filled up, so we need to use more of it in a new qpl_job_ptr.
    }

    if (0u != qpl_job_ptr->available_in) {
        // This should only happen if buffer > 2GB, or if buffering
        qpl_status status = hw_submit_decompress_job(qpl_job_ptr,
                                                     qpl_job_ptr->flags & QPL_FLAG_LAST,
                                                     qpl_job_ptr->next_in_ptr,
                                                     qpl_job_ptr->available_in);

        return (QPL_STS_OK != status) ? status : QPL_STS_BEING_PROCESSED;
    }

    return QPL_STS_OK;
}
