/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

/**
 * @date 3/23/2020
 * @brief Internal HW API functions for @ref hw_submit_job API implementation
 *
 * @defgroup HW_SUBMIT_API Submit Job API
 * @ingroup HW_PRIVATE_API
 * @{
 */


// C API Definitions
#include "filter_operations/arguments_check.hpp"
#include "compression_operations/huffman_table.hpp"
#include "compression_operations/arguments_check.hpp"
#include "other_operations/arguments_check.hpp"
#include "util/descriptor_processing.hpp"

// Middle Layer
#include "job.hpp"
#include "compression/stream_decorators/gzip_decorator.hpp"

// Hardware Core
#include "hardware_state.h"
#include "hw_descriptors_api.h"

// Legacy
#include "own_defs.h"
#include "hardware_defs.h"
#include "own_ml_submit_operation_api.hpp"
#include "own_ml_buffer_api.hpp"
#include "own_checkers.h"

typedef enum {
    hw_scan_operator_eq = 0u,
    hw_scan_operator_ne = 1u,
    hw_scan_operator_lt = 2u,
    hw_scan_operator_le = 3u,
    hw_scan_operator_gt = 4u,
    hw_scan_operator_ge = 5u,
} hw_scan_operator_e;

typedef struct {
     uint32_t low;
     uint32_t high;
} hw_scan_range_t;

static inline hw_scan_range_t own_get_scan_one_value_range(hw_scan_operator_e scan_operator,
                                                           uint32_t low_limit,
                                                           uint32_t source_bit_width) {
    hw_scan_range_t range;
    auto range_mask    = static_cast<uint32_t>((1ULL << source_bit_width) - 1u);
    uint32_t param_low = low_limit & range_mask;

    switch (scan_operator) {
        case hw_scan_operator_eq:
        case hw_scan_operator_ne:
            range.low  = param_low;
            range.high = param_low;
            break;

        case hw_scan_operator_lt:
            if (0u == param_low) {
                range.low  = 1u;
                range.high = 0u;
            } else {
                range.low  = 0u;
                range.high = param_low - 1u;
            }
            break;

        case hw_scan_operator_le:
            range.low  = 0u;
            range.high = param_low;
            break;

        case hw_scan_operator_gt:
            if (param_low == range_mask) {
                range.low  = 1u;
                range.high = 0u;
            } else {
                range.low  = param_low + 1u;
                range.high = UINT32_MAX;
            }
            break;

        case hw_scan_operator_ge:
            range.low  = param_low;
            range.high = UINT32_MAX;
            break;
    }

    return range;
}

static inline qpl_status hw_submit_analytic_task(qpl_job *const job_ptr) {
    using namespace qpl;
    using namespace qpl::ml::util;
    auto *const state_ptr = reinterpret_cast<qpl_hw_state *>(job::get_state(job_ptr));

    auto                 *const descriptor_ptr    = (hw_descriptor *) &state_ptr->desc_ptr;
    hw_iaa_aecs_analytic *const filter_config_ptr = state_ptr->dcfg;

    // Reset Output Job fields
    job_ptr->total_in  = 0u;
    job_ptr->total_out = 0u;

    hw_iaa_descriptor_reset(descriptor_ptr);

    hw_iaa_descriptor_analytic_set_filter_input(descriptor_ptr,
                                                job_ptr->next_in_ptr,
                                                job_ptr->available_in,
                                                job_ptr->num_input_elements,
                                                (hw_iaa_input_format) job_ptr->parser,
                                                job_ptr->src1_bit_width);

    auto out_format = ((hw_iaa_output_format) job_ptr->out_bit_width) |
                      ((job_ptr->flags & QPL_FLAG_OUT_BE) ? hw_iaa_output_modifier_big_endian : 0) |
                      ((job_ptr->flags & QPL_FLAG_INV_OUT) ? hw_iaa_output_modifier_inverse : 0);

    if(job_ptr->op == qpl_op_scan_not_range
       || job_ptr->op == qpl_op_scan_ne) {
        out_format ^= hw_iaa_output_modifier_inverse;
    }

    if (job_ptr->flags & QPL_FLAG_CRC32C) {
        hw_iaa_descriptor_set_crc_rfc3720(descriptor_ptr);
    }

    hw_iaa_descriptor_analytic_set_filter_output(descriptor_ptr,
                                                 job_ptr->next_out_ptr,
                                                 job_ptr->available_out,
                                                 static_cast<hw_iaa_output_format>(out_format));

    switch (job_ptr->op) {
        case qpl_op_extract:
            hw_iaa_descriptor_analytic_set_extract_operation(descriptor_ptr,
                                                             job_ptr->param_low,
                                                             job_ptr->param_high,
                                                             filter_config_ptr);

            hw_iaa_aecs_filter_set_initial_output_index(filter_config_ptr, job_ptr->initial_output_index);
            hw_iaa_aecs_filter_set_drop_initial_decompressed_bytes(filter_config_ptr, job_ptr->drop_initial_bytes);

            break;

        case qpl_op_select:
            hw_iaa_descriptor_analytic_set_select_operation(descriptor_ptr,
                                                            job_ptr->next_src2_ptr,
                                                            job_ptr->available_src2,
                                                            job_ptr->flags & QPL_FLAG_SRC2_BE);
            break;

        case qpl_op_expand:
            hw_iaa_descriptor_analytic_set_expand_operation(descriptor_ptr,
                                                            job_ptr->next_src2_ptr,
                                                            job_ptr->available_src2,
                                                            job_ptr->flags & QPL_FLAG_SRC2_BE);
            break;

        case qpl_op_scan_ne:
        case qpl_op_scan_eq:
        case qpl_op_scan_le:
        case qpl_op_scan_lt:
        case qpl_op_scan_gt:
        case qpl_op_scan_ge: {
            hw_scan_range_t range = own_get_scan_one_value_range(static_cast<hw_scan_operator_e>(job_ptr->op & 0x1F),
                                                                 job_ptr->param_low,
                                                                 job_ptr->src1_bit_width);

            hw_iaa_descriptor_analytic_set_scan_operation(descriptor_ptr,
                                                          range.low,
                                                          range.high,
                                                          filter_config_ptr);

            hw_iaa_aecs_filter_set_initial_output_index(filter_config_ptr, job_ptr->initial_output_index);
            hw_iaa_aecs_filter_set_drop_initial_decompressed_bytes(filter_config_ptr, job_ptr->drop_initial_bytes);

            break;
        }

        case qpl_op_scan_range:
        case qpl_op_scan_not_range: {
            auto range_mask = static_cast<uint32_t>((1ULL << job_ptr->src1_bit_width) - 1u);

            hw_iaa_descriptor_analytic_set_scan_operation(descriptor_ptr,
                                                          job_ptr->param_low & range_mask,
                                                          job_ptr->param_high & range_mask,
                                                          filter_config_ptr);

            hw_iaa_aecs_filter_set_initial_output_index(filter_config_ptr, job_ptr->initial_output_index);
            hw_iaa_aecs_filter_set_drop_initial_decompressed_bytes(filter_config_ptr, job_ptr->drop_initial_bytes);

            break;
        }

        default:
            return QPL_STS_OPERATION_ERR;
    }

    if (job_ptr->flags & QPL_FLAG_DECOMPRESS_ENABLE) {
        if (job_ptr->flags & QPL_FLAG_GZIP_MODE) {
            qpl::ml::compression::gzip_decorator::gzip_header header;

            auto status = qpl::ml::compression::gzip_decorator::read_header(job_ptr->next_in_ptr,
                                                                            job_ptr->available_in,
                                                                            header);
            OWN_QPL_CHECK_STATUS(status)

            job::update_input_stream(job_ptr, header.byte_size);
        }

        const bool is_big_endian   = job_ptr->flags & QPL_FLAG_HUFFMAN_BE;
        const bool is_huffman_only = job_ptr->flags & QPL_FLAG_NO_HDRS;
        const uint32_t ignore_last_bits = job_ptr->ignore_end_bits;
        const auto inflate_start_stop_rules = job_ptr->decomp_end_processing;

        hw_iaa_descriptor_analytic_enable_decompress(descriptor_ptr, is_big_endian, ignore_last_bits);

        if (!is_huffman_only) {
            hw_iaa_descriptor_set_inflate_stop_check_rule((hw_descriptor *) descriptor_ptr,
                                                          (hw_iaa_decompress_start_stop_rule_t) inflate_start_stop_rules,
                                                          inflate_start_stop_rules & qpl_check_on_nonlast_block);
        }
    }

    return process_descriptor<qpl_status,
                              execution_mode_t::async>(descriptor_ptr,
                                                       (hw_completion_record *) &state_ptr->comp_ptr,
                                                       job_ptr->numa_id);
}

static inline qpl_status own_bad_argument_validation(qpl_job *const job_ptr) {
    using namespace qpl;

    switch (job_ptr->op) {
        case qpl_op_compress:
            OWN_QPL_CHECK_STATUS(job::validate_operation<qpl_op_compress>(job_ptr))
            break;

        case qpl_op_decompress:
            OWN_QPL_CHECK_STATUS(job::validate_operation<qpl_op_decompress>(job_ptr))
            break;

        case qpl_op_select:
            OWN_QPL_CHECK_STATUS(job::validate_operation<qpl_op_select>(job_ptr))
            break;

        case qpl_op_extract:
            OWN_QPL_CHECK_STATUS(job::validate_operation<qpl_op_extract>(job_ptr))
            break;

        case qpl_op_expand:
            OWN_QPL_CHECK_STATUS(job::validate_operation<qpl_op_expand>(job_ptr))
            break;

        case qpl_op_scan_eq:
        case qpl_op_scan_ne:
        case qpl_op_scan_lt:
        case qpl_op_scan_le:
        case qpl_op_scan_gt:
        case qpl_op_scan_ge:
        case qpl_op_scan_range:
        case qpl_op_scan_not_range:
            OWN_QPL_CHECK_STATUS(job::validate_operation<qpl_op_scan_eq>(job_ptr))

        default:
            break;
    }

    return QPL_STS_OK;
}

static inline void own_job_fix_task_properties(qpl_job *const job_ptr) {
    // HARDWARE FIX - Hardware can't verify stream compressed in `Huffman only BE16` mode correct.
    // So, verify in this case shall be omitted for future HW versions
    if (job_ptr->op == qpl_op_compress
        && ((job_ptr->flags & (QPL_FLAG_NO_HDRS | QPL_FLAG_GEN_LITERALS)) == (QPL_FLAG_NO_HDRS | QPL_FLAG_GEN_LITERALS))
        && (job_ptr->flags & QPL_FLAG_HUFFMAN_BE)) {
        job_ptr->flags |= QPL_FLAG_OMIT_VERIFY;
    }
}

static inline qpl_status hw_submit_task (qpl_job *const job_ptr) {
    using namespace qpl;
    using namespace qpl::ml::util;
    auto *const state_ptr = reinterpret_cast<qpl_hw_state *>(job::get_state(job_ptr));

    auto *const descriptor_ptr = (hw_descriptor *) &state_ptr->desc_ptr;

    switch (job_ptr->op) {
        case qpl_op_crc64:
            hw_iaa_descriptor_init_crc64(descriptor_ptr,
                                         job_ptr->next_in_ptr,
                                         job_ptr->available_in,
                                         job_ptr->crc64_poly,
                                         (job_ptr->flags & QPL_FLAG_CRC64_BE) != 0,
                                         (job_ptr->flags & QPL_FLAG_CRC64_INV) != 0);
            break;

        case qpl_op_compress: {
            if (job::is_canned_mode_compression(job_ptr)) {
                hw_descriptor_compress_init_deflate_canned(job_ptr);

                break;
            }

            qpl_status status = hw_descriptor_compress_init_deflate_base(job_ptr,
                                                                         &state_ptr->desc_ptr,
                                                                         (hw_completion_record *) &state_ptr->comp_ptr,
                                                                         state_ptr);
            OWN_QPL_CHECK_STATUS(status)

            break;
        }

        case qpl_op_decompress:{
            ml::util::set_zeros((uint8_t *) descriptor_ptr, sizeof(hw_descriptor));

            auto table_impl = use_as_huffman_table<qpl::ml::compression::compression_algorithm_e::deflate>(job_ptr->huffman_table);

            hw_iaa_aecs * aecs_ptr = (job_ptr->flags & QPL_FLAG_CANNED_MODE) ?
                                     table_impl->get_aecs_decompress() :
                                     GET_DCFG(state_ptr);

            HW_IMMEDIATELY_RET_NULLPTR(aecs_ptr)

            qpl_status status = hw_descriptor_decompress_init_inflate_body(descriptor_ptr,
                                                                           &job_ptr->next_in_ptr,
                                                                           &job_ptr->available_in,
                                                                           job_ptr->next_out_ptr,
                                                                           job_ptr->available_out,
                                                                           job_ptr->ignore_start_bits,
                                                                           job_ptr->ignore_end_bits,
                                                                           job_ptr->crc,
                                                                           aecs_ptr);
            OWN_QPL_CHECK_STATUS(status)
            break;
        }

        default:
            return QPL_STS_OPERATION_ERR;
    }

    return process_descriptor<qpl_status,
                              execution_mode_t::async>(descriptor_ptr,
                                                       (hw_completion_record *) &state_ptr->comp_ptr,
                                                       job_ptr->numa_id);
}

static inline void own_hw_state_reset(qpl_hw_state *const state_ptr) {
    state_ptr->config_valid                                   = 0u;
    state_ptr->execution_history.first_job_has_been_submitted = false;
    state_ptr->accumulation_buffer.actual_bytes               = 0u;
    state_ptr->aecs_hw_read_offset                            = 0u;
}

#define STOP_CHECK_RULE_COUNT 7u

extern "C" qpl_status hw_submit_job (qpl_job * qpl_job_ptr) {
    QPL_BAD_OP_RET(qpl_job_ptr->op);

    // Variables
    using namespace qpl;
    auto *const state_ptr = reinterpret_cast<qpl_hw_state *>(job::get_state(qpl_job_ptr));

    uint32_t flags = qpl_job_ptr->flags;

    OWN_QPL_CHECK_STATUS(own_bad_argument_validation(qpl_job_ptr))
    own_job_fix_task_properties(qpl_job_ptr);

    switch (qpl_job_ptr->op) {
        case qpl_op_extract:
            if (qpl_job_ptr->param_low > qpl_job_ptr->param_high) {
                hw_iaa_completion_record_init_trivial_completion(&state_ptr->comp_ptr, 0u);

                return QPL_STS_OK;
            }
            [[fallthrough]];
        case qpl_op_select:
        case qpl_op_expand:
        case qpl_op_scan_ne:
        case qpl_op_scan_eq:
        case qpl_op_scan_le:
        case qpl_op_scan_lt:
        case qpl_op_scan_gt:
        case qpl_op_scan_ge:
        case qpl_op_scan_range:
        case qpl_op_scan_not_range:
            HW_IMMEDIATELY_RET((std::max(qpl_job_ptr->available_in, qpl_job_ptr->available_out) > MAX_BUF_SIZE),
                               QPL_STS_BUFFER_TOO_LARGE_ERR);
            HW_IMMEDIATELY_RET((qpl_job_ptr->flags & QPL_FLAG_NO_HDRS) || (qpl_job_ptr->flags & QPL_FLAG_RND_ACCESS),
                               QPL_STS_OPERATION_ERR)
            return hw_submit_analytic_task(qpl_job_ptr);

        case qpl_op_decompress:
            if (qpl_job_ptr->dictionary != NULL && qpl_job_ptr->flags & QPL_FLAG_CANNED_MODE) {
                // dictionary with canned mode
                // TODO: remove once it's supported
                return QPL_STS_NOT_SUPPORTED_MODE_ERR;
            }

            if (!(flags & QPL_FLAG_RND_ACCESS && !(flags & QPL_FLAG_NO_HDRS))
                  && !(flags & QPL_FLAG_CANNED_MODE)) {
                break; // Run legacy code
            }

            if ((flags & QPL_FLAG_FIRST) && !(flags & QPL_FLAG_CANNED_MODE)) {
                break; // Workaround for header reading
            }

            job::reset<qpl_op_decompress>(qpl_job_ptr);
            state_ptr->aecs_size = HW_AECS_ANALYTIC_RANDOM_ACCESS_SIZE;
            return hw_submit_task(qpl_job_ptr);
        case qpl_op_compress:
            if (flags & QPL_FLAG_FIRST) {
                job::reset<qpl_op_compress>(qpl_job_ptr);
            }
            return hw_submit_task(qpl_job_ptr);
        case qpl_op_crc64:
            return hw_submit_task(qpl_job_ptr);

        default: {
            break;
        }
    }

    // Below is a bug: qpl_check_on_nonlast_block in decomp_end_processing field can't be processed as expected.
    HW_IMMEDIATELY_RET((STOP_CHECK_RULE_COUNT <= qpl_job_ptr->decomp_end_processing), QPL_STS_INVALID_PARAM_ERR);

    // This is the first job
    if (flags & QPL_FLAG_FIRST) {
        hw_iaa_analytics_descriptor *desc_ptr = &state_ptr->desc_ptr;

        job::reset<qpl_op_decompress>(qpl_job_ptr);
        own_hw_state_reset(state_ptr);

        state_ptr->aecs_size = (qpl_job_ptr->flags & QPL_FLAG_RND_ACCESS)
                               ? HW_AECS_ANALYTIC_RANDOM_ACCESS_SIZE
                               : sizeof(hw_iaa_aecs_analytic);

        desc_ptr->src2_ptr  = (uint8_t *) state_ptr->dcfg;
        desc_ptr->src2_size = state_ptr->aecs_size;

        if (flags & QPL_FLAG_GZIP_MODE) {
            qpl::ml::compression::gzip_decorator::gzip_header header;

            auto status = qpl::ml::compression::gzip_decorator::read_header(qpl_job_ptr->next_in_ptr,
                                                                            qpl_job_ptr->available_in,
                                                                            header);
            OWN_QPL_CHECK_STATUS(status)

            job::update_input_stream(qpl_job_ptr, header.byte_size);
        }

        desc_ptr->filter_flags          = 0u;
        desc_ptr->completion_record_ptr = (uint8_t *) &state_ptr->comp_ptr;

        state_ptr->dcfg[0].filtering_options.crc          = 0u;
        state_ptr->dcfg[0].filtering_options.xor_checksum = 0u;
    }

    uint8_t *source_ptr  = qpl_job_ptr->next_in_ptr;
    uint32_t source_size = qpl_job_ptr->available_in;

    qpl_buffer *const accumulator_ptr = &state_ptr->accumulation_buffer;
    bool is_last_job = flags & QPL_FLAG_LAST;

    if ((!is_last_job)
        && own_qpl_buffer_touch(accumulator_ptr, source_size)) {
        own_qpl_buffer_fill(accumulator_ptr, source_ptr, source_size);
        hw_iaa_completion_record_init_trivial_completion(&state_ptr->comp_ptr, source_size);

        return QPL_STS_OK;
    }

    // This is not the first HW job, but we don't have a valid config
    HW_IMMEDIATELY_RET(((state_ptr->execution_history.first_job_has_been_submitted)
                        && (!(state_ptr->config_valid))),
                       QPL_STS_JOB_NOT_CONTINUABLE_ERR);

    if (!own_qpl_buffer_is_empty(accumulator_ptr)) {
        source_ptr  = own_qpl_buffer_get_data(accumulator_ptr);
        source_size = own_qpl_buffer_get_size(accumulator_ptr);
        is_last_job = false;
    }

    return hw_submit_decompress_job(qpl_job_ptr, is_last_job, source_ptr, source_size);
}

/** @} */
