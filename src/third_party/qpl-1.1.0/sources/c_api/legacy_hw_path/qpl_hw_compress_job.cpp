/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

/**
 * @date 3/23/2020
 * @brief Internal HW API functions for @ref hw_descriptor_compress_init_deflate_base API implementation
 */

#include "hardware_state.h"
#include "hardware_defs.h"

#include "hw_descriptors_api.h"
#include "hw_aecs_api.h"

#include "own_checkers.h"
#include "own_ml_submit_operation_api.hpp"

#include "common/bit_reverse.hpp"
#include "compression/stream_decorators/gzip_decorator.hpp"
#include "compression_operations/huffman_table.hpp"

static inline qpl_comp_style own_get_compression_style(const qpl_job *const job_ptr) {
    if (job_ptr->flags & QPL_FLAG_DYNAMIC_HUFFMAN) {
        return qpl_cst_dynamic;
    } else if (job_ptr->huffman_table) {
        return qpl_cst_static;
    } else {
        return qpl_cst_fixed;
    }
}

extern "C" qpl_status hw_descriptor_compress_init_deflate_base(qpl_job *qpl_job_ptr,
                                                    hw_iaa_analytics_descriptor *const descriptor_ptr,
                                                    hw_completion_record *const completion_record_ptr,
                                                    qpl_hw_state *const state_ptr) {
    using namespace qpl::ml::compression;

    auto                 huffman_table_ptr = qpl_job_ptr->huffman_table;
    hw_iaa_aecs_compress *configuration_ptr;
    uint32_t             flags             = qpl_job_ptr->flags;
    qpl_comp_style       compression_style = own_get_compression_style(qpl_job_ptr);
    uint8_t              *next_out_ptr     = qpl_job_ptr->next_out_ptr;
    uint32_t             available_out     = qpl_job_ptr->available_out;

    if (flags & QPL_FLAG_FIRST) {
        uint32_t content_header_size = 0u;
        state_ptr->execution_history.execution_step = (flags & QPL_FLAG_NO_HDRS) ?
                                                      qpl_task_execution_step_data_processing :
                                                      qpl_task_execution_step_header_inserting;
        state_ptr->execution_history.comp_style     = compression_style;
        state_ptr->aecs_hw_read_offset = 0u;

        configuration_ptr = &state_ptr->ccfg[0];
        configuration_ptr->num_output_accum_bits = 0u;
        configuration_ptr->crc                   = 0u;
        configuration_ptr->xor_checksum          = 0u;

        if (flags & QPL_FLAG_GZIP_MODE) {
            HW_IMMEDIATELY_RET((available_out < qpl::ml::compression::OWN_GZIP_HEADER_LENGTH),
                               QPL_STS_DST_IS_SHORT_ERR);
            qpl::ml::compression::gzip_decorator::write_header_unsafe(next_out_ptr, available_out);

            content_header_size += qpl::ml::compression::OWN_GZIP_HEADER_LENGTH;
        }

        qpl_job_ptr->total_in      = 0u;
        qpl_job_ptr->next_out_ptr  += content_header_size;
        qpl_job_ptr->available_out -= content_header_size;
        qpl_job_ptr->total_out      = content_header_size;

    } else {
        // Not first qpl_job_ptr
        HW_IMMEDIATELY_RET((state_ptr->execution_history.execution_step == qpl_task_execution_step_completed),
                           QPL_STS_JOB_NOT_CONTINUABLE_ERR);
        HW_IMMEDIATELY_RET((state_ptr->execution_history.comp_style != compression_style),
                           QPL_STS_INVALID_COMPRESS_STYLE_ERR);
        configuration_ptr = &state_ptr->ccfg[state_ptr->aecs_hw_read_offset];
    }
    // End if first
    state_ptr->execution_history.saved_next_out_ptr = qpl_job_ptr->next_out_ptr;

    // If statistic collection required
    if (flags & QPL_FLAG_DYNAMIC_HUFFMAN) {
        HW_IMMEDIATELY_RET(!((state_ptr->execution_history.execution_step == qpl_task_execution_step_header_inserting)
                            || (flags & QPL_FLAG_NO_HDRS)),
                           QPL_STS_LIBRARY_INTERNAL_ERR);
        hw_iaa_descriptor_init_statistic_collector((hw_descriptor *) descriptor_ptr,
                                                   qpl_job_ptr->next_in_ptr,
                                                   qpl_job_ptr->available_in,
                                                   &configuration_ptr->histogram);
        if (flags & QPL_FLAG_GEN_LITERALS) {
            hw_iaa_descriptor_compress_set_huffman_only_mode((hw_descriptor *) descriptor_ptr);
        }
        hw_iaa_descriptor_compress_set_mini_block_size((hw_descriptor *) descriptor_ptr,
                                                       (hw_iaa_mini_block_size_t) qpl_job_ptr->mini_block_size);
        hw_iaa_descriptor_set_completion_record((hw_descriptor *) descriptor_ptr, completion_record_ptr);
        completion_record_ptr->status = 0u;

        return QPL_STS_OK;
    } else {
        bool is_final_block  = (flags & QPL_FLAG_LAST) ? 1u : 0u;
        bool is_huffman_only = (flags & QPL_FLAG_NO_HDRS) ? true : false;

        if (flags & QPL_FLAG_START_NEW_BLOCK) {
            if (state_ptr->execution_history.execution_step != qpl_task_execution_step_header_inserting) {
                HW_IMMEDIATELY_RET((state_ptr->execution_history.execution_step != qpl_task_execution_step_data_processing),
                                   QPL_STS_LIBRARY_INTERNAL_ERR)
                hw_iaa_aecs_compress_accumulator_insert_eob(configuration_ptr, state_ptr->eob_code);
                state_ptr->execution_history.execution_step = qpl_task_execution_step_header_inserting;
            }
        }

        // Prepare huffman table begin
        if (state_ptr->execution_history.execution_step == qpl_task_execution_step_header_inserting) {
            HW_IMMEDIATELY_RET(((qpl_job_ptr->mini_block_size)
                                && (0u == (flags & (QPL_FLAG_FIRST | QPL_FLAG_START_NEW_BLOCK)))),
                               QPL_STS_INDEX_GENERATION_ERR);
            state_ptr->saved_num_output_accum_bits = configuration_ptr->num_output_accum_bits;

            auto table_impl = use_as_huffman_table<compression_algorithm_e::deflate>(qpl_job_ptr->huffman_table);
            // insert header
            if(huffman_table_ptr) {
                // Static mode used
                uint32_t status = hw_iaa_aecs_compress_write_deflate_dynamic_header(configuration_ptr,
                                                                                    table_impl->get_deflate_header_ptr(),
                                                                                    table_impl->get_deflate_header_bits_size(),
                                                                                    is_final_block);
                HW_IMMEDIATELY_RET((status != QPL_STS_OK), QPL_STS_LIBRARY_INTERNAL_ERR);

                uint32_t code_length  = table_impl->get_literals_lengths_table_ptr()[256];
                uint32_t eob_code_len = code_length >> 15u;
                state_ptr->eob_code.code   = reverse_bits(static_cast<uint16_t>(code_length)) >> (16u - eob_code_len);
                state_ptr->eob_code.length = eob_code_len;
            } else {
                // Fixed mode used
                uint32_t status = hw_iaa_aecs_compress_write_deflate_fixed_header(configuration_ptr,
                                                                                  is_final_block);

                HW_IMMEDIATELY_RET((status != QPL_STS_OK), QPL_STS_LIBRARY_INTERNAL_ERR);

                state_ptr->eob_code.code   = 0u;
                state_ptr->eob_code.length = 7u;
            }
        }

        if (!is_huffman_only)
        {
            if (huffman_table_ptr) {
                auto table_impl = use_as_huffman_table<compression_algorithm_e::deflate>(qpl_job_ptr->huffman_table);

                hw_iaa_aecs_compress_set_deflate_huffman_table(configuration_ptr,
                                                               table_impl->get_literals_lengths_table_ptr(),
                                                               table_impl->get_offsets_table_ptr());
            } else {
                hw_iaa_aecs_compress_set_deflate_huffman_table(configuration_ptr,
                                                               (hw_iaa_huffman_codes *) fixed_literals_table,
                                                               (hw_iaa_huffman_codes *) fixed_offsets_table);
            }
        }

        if (is_huffman_only) {
            if (huffman_table_ptr) {
                auto table_impl = use_as_huffman_table<compression_algorithm_e::huffman_only>(qpl_job_ptr->huffman_table);

                hw_iaa_aecs_compress_set_huffman_only_huffman_table(configuration_ptr,
                                                                    table_impl->get_literals_lengths_table_ptr());
            } else {
                hw_iaa_aecs_compress_set_huffman_only_huffman_table(configuration_ptr,
                                                                    (hw_iaa_huffman_codes *) fixed_literals_table);
            }
        }
        // Prepare huffman table begin-end

        // Skip Gzip footer
        uint32_t max_output_size = qpl_job_ptr->available_out;

        if ((QPL_FLAG_GZIP_MODE | QPL_FLAG_LAST) == ((QPL_FLAG_GZIP_MODE | QPL_FLAG_LAST) & qpl_job_ptr->flags)) {
            max_output_size = (max_output_size > 8u) ? max_output_size - 8u : max_output_size;
        }

        // Prepare Compress task
        hw_iaa_descriptor_init_deflate_body((hw_descriptor *) descriptor_ptr,
                                            qpl_job_ptr->next_in_ptr,
                                            qpl_job_ptr->available_in,
                                            qpl_job_ptr->next_out_ptr,
                                            max_output_size);

        if (flags & QPL_FLAG_HUFFMAN_BE) {
            hw_iaa_descriptor_compress_set_be_output_mode((hw_descriptor *) descriptor_ptr);
        }

        if (flags & QPL_FLAG_GEN_LITERALS) {
            hw_iaa_descriptor_compress_set_huffman_only_mode((hw_descriptor *) descriptor_ptr);
        }

        if (flags & QPL_FLAG_CRC32C) {
            hw_iaa_descriptor_set_crc_rfc3720((hw_descriptor *) descriptor_ptr);
        }

        hw_iaa_descriptor_compress_set_mini_block_size((hw_descriptor *) descriptor_ptr,
                                                       (hw_iaa_mini_block_size_t) qpl_job_ptr->mini_block_size);

        auto access_policy = is_final_block ?
            hw_aecs_access_read | state_ptr->aecs_hw_read_offset :
            hw_aecs_access_read | hw_aecs_access_write | state_ptr->aecs_hw_read_offset;
        hw_iaa_descriptor_compress_set_aecs((hw_descriptor *) descriptor_ptr,
                                            state_ptr->ccfg,
                                            static_cast<hw_iaa_aecs_access_policy>(access_policy));

        if (is_final_block && !is_huffman_only) {
            descriptor_ptr->decomp_flags |=
                    (qpl_task_execution_step_header_inserting == state_ptr->execution_history.execution_step)
                    ? (ADCF_FLUSH_OUTPUT | ADCF_END_PROC(AD_APPEND_EOB))
                    : (ADCF_FLUSH_OUTPUT | ADCF_END_PROC(AD_APPEND_EOB_FINAL_SB));
        }

        hw_iaa_descriptor_set_completion_record((hw_descriptor *) descriptor_ptr, completion_record_ptr);
        completion_record_ptr->status = 0u;

        return QPL_STS_OK;
    }
}

extern "C" void hw_descriptor_compress_init_deflate_dynamic(hw_iaa_analytics_descriptor *desc_ptr,
                                                            qpl_hw_state *state_ptr,
                                                            qpl_job *qpl_job_ptr,
                                                            hw_iaa_aecs_compress *cfg_in_ptr,
                                                            hw_iaa_completion_record *comp_ptr) {
    using namespace qpl::ml::compression;
    uint32_t flags = qpl_job_ptr->flags;
    bool is_huffman_only = (flags & QPL_FLAG_NO_HDRS) ? true : false;
    bool is_final_block  = (flags & QPL_FLAG_LAST) ? 1u : 0u;

    state_ptr->saved_num_output_accum_bits = hw_iaa_aecs_compress_accumulator_get_actual_bits(cfg_in_ptr);

    if (is_huffman_only) {
        hw_iaa_aecs_compress_set_huffman_only_huffman_table_from_histogram(cfg_in_ptr,
                                                                           &cfg_in_ptr->histogram);

        auto table_impl = use_as_huffman_table<compression_algorithm_e::huffman_only>(qpl_job_ptr->huffman_table);

        compression_huffman_table compression_table(table_impl->get_sw_compression_huffman_table_ptr(),
                                                    table_impl->get_isal_compression_huffman_table_ptr(),
                                                    table_impl->get_hw_compression_huffman_table_ptr(),
                                                    table_impl->get_deflate_header_ptr());

        hw_iaa_aecs_compress_store_huffman_only_huffman_table(cfg_in_ptr, compression_table.get_sw_compression_table());

        table_impl->set_deflate_header_bits_size(0u);
    } else {
        hw_iaa_aecs_compress_write_deflate_dynamic_header_from_histogram(cfg_in_ptr,
                                                                         &cfg_in_ptr->histogram,
                                                                         is_final_block);
    }

    uint32_t max_output_size = qpl_job_ptr->available_out;

    if ((QPL_FLAG_GZIP_MODE | QPL_FLAG_LAST) == ((QPL_FLAG_GZIP_MODE | QPL_FLAG_LAST) & qpl_job_ptr->flags)) {
        max_output_size = (max_output_size > 8u) ? max_output_size - 8u : max_output_size;
    }

    hw_iaa_descriptor_init_deflate_body((hw_descriptor *) desc_ptr,
                                        qpl_job_ptr->next_in_ptr,
                                        qpl_job_ptr->available_in,
                                        qpl_job_ptr->next_out_ptr,
                                        max_output_size);

    if (flags & QPL_FLAG_HUFFMAN_BE) {
        hw_iaa_descriptor_compress_set_be_output_mode((hw_descriptor *) desc_ptr);
    }

    if (flags & QPL_FLAG_GEN_LITERALS) {
        hw_iaa_descriptor_compress_set_huffman_only_mode((hw_descriptor *) desc_ptr);
    }

    if (flags & QPL_FLAG_CRC32C) {
        hw_iaa_descriptor_set_crc_rfc3720((hw_descriptor *) desc_ptr);
    }

    hw_iaa_descriptor_compress_set_mini_block_size((hw_descriptor *) desc_ptr,
                                                   (hw_iaa_mini_block_size_t) qpl_job_ptr->mini_block_size);

    auto access_policy = is_final_block ?
        hw_aecs_access_read | state_ptr->aecs_hw_read_offset :
        hw_aecs_access_read | hw_aecs_access_write | state_ptr->aecs_hw_read_offset;
    hw_iaa_descriptor_compress_set_aecs((hw_descriptor *) desc_ptr,
                                        state_ptr->ccfg,
                                        static_cast<hw_iaa_aecs_access_policy>(access_policy));

    hw_iaa_descriptor_set_completion_record((hw_descriptor *) desc_ptr, (hw_completion_record *) comp_ptr);
    comp_ptr->status = 0u;

    if(!is_huffman_only) {
        desc_ptr->decomp_flags |= ADCF_END_PROC(AD_APPEND_EOB);
    }
}

extern "C" void hw_descriptor_compress_init_deflate_canned(qpl_job *const job_ptr) {
    using namespace qpl::ml::compression;

    qpl_hw_state            *const state_ptr      = (qpl_hw_state *) job_ptr->data_ptr.hw_state_ptr;
    hw_iaa_analytics_descriptor *const descriptor_ptr = &state_ptr->desc_ptr;
    uint32_t flags = job_ptr->flags;
    bool is_final_block  = (flags & QPL_FLAG_LAST) ? true : false;
    bool is_first_block  = (flags & QPL_FLAG_FIRST) ? true : false;

    hw_iaa_descriptor_init_deflate_body((hw_descriptor *) descriptor_ptr,
                                        job_ptr->next_in_ptr,
                                        job_ptr->available_in,
                                        job_ptr->next_out_ptr,
                                        job_ptr->available_out);

    if (flags & QPL_FLAG_HUFFMAN_BE) {
        hw_iaa_descriptor_compress_set_be_output_mode((hw_descriptor *) descriptor_ptr);
    }

    if (flags & QPL_FLAG_CRC32C) {
        hw_iaa_descriptor_set_crc_rfc3720((hw_descriptor *) descriptor_ptr);
    }

    if (is_first_block) {
        auto table_impl = use_as_huffman_table<compression_algorithm_e::deflate>(job_ptr->huffman_table);

        hw_iaa_aecs_compress_set_deflate_huffman_table(state_ptr->ccfg,
                                                       table_impl->get_literals_lengths_table_ptr(),
                                                       table_impl->get_offsets_table_ptr());
    }

    auto access_policy = is_final_block ? hw_aecs_access_read : hw_aecs_access_read | hw_aecs_access_write;
    hw_iaa_descriptor_compress_set_aecs((hw_descriptor *) descriptor_ptr,
                                        state_ptr->ccfg,
                                        static_cast<hw_iaa_aecs_access_policy>(access_policy));

    descriptor_ptr->decomp_flags |= ADCF_END_PROC(AD_APPEND_EOB);

    hw_iaa_descriptor_set_completion_record((hw_descriptor *) descriptor_ptr,
                                            (hw_completion_record *) &state_ptr->comp_ptr);
}
