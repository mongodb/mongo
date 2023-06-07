/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

/*
 *  Intel® Query Processing Library (Intel® QPL)
 *  Job API (public C API)
 */

// C_API headers
#include "qpl/qpl.h"
#include "job.hpp"
#include "compression_operations/compressor.hpp"
#include "filter_operations/filter_operations.hpp"
#include "filter_operations/analytics_state_t.h"
#include "other_operations/crc64.hpp"

// Middle layer headers
#include "util/checksum.hpp"

// Legacy
#include "own_defs.h"
#include "legacy_hw_path/async_hw_api.h"
#include "legacy_hw_path/hardware_state.h"

//#define KEEP_DESCRIPTOR_ENABLED

QPL_FUN("C" qpl_status, qpl_submit_job, (qpl_job * qpl_job_ptr)) {
    using namespace qpl;

    QPL_BAD_PTR_RET(qpl_job_ptr);
    QPL_BAD_PTR_RET(qpl_job_ptr->next_in_ptr);
    QPL_BAD_PTR_RET(qpl_job_ptr->data_ptr.compress_state_ptr);
    QPL_BAD_PTR_RET(qpl_job_ptr->data_ptr.decompress_state_ptr);
    QPL_BAD_PTR_RET(qpl_job_ptr->data_ptr.analytics_state_ptr);
    QPL_BAD_PTR_RET(qpl_job_ptr->data_ptr.hw_state_ptr);
    QPL_BAD_OP_RET(qpl_job_ptr->op);

    uint32_t status = QPL_STS_OK;

    qpl_path_t path = qpl_job_ptr->data_ptr.path;

    if (qpl_path_hardware == path) {
        if ((qpl_op_compress == qpl_job_ptr->op) && (qpl_high_level == qpl_job_ptr->level)) {
            return QPL_STS_UNSUPPORTED_COMPRESSION_LEVEL;
        }
        if (QPL_FLAG_ZLIB_MODE & qpl_job_ptr->flags) {
            return QPL_STS_NOT_SUPPORTED_MODE_ERR;
        }
    }

    if (qpl_path_hardware == qpl_job_ptr->data_ptr.path || qpl_path_auto == qpl_job_ptr->data_ptr.path) {
        auto *state_ptr = reinterpret_cast<qpl_hw_state *>(job::get_state(qpl_job_ptr));

#if defined(KEEP_DESCRIPTOR_ENABLED)
        if (state_ptr->descriptor_not_submitted) {
            status = hw_enqueue_descriptor(&state_ptr->desc_ptr, qpl_job_ptr->numa_id);

            if (status == QPL_STS_OK) {
                state_ptr->descriptor_not_submitted = false;
            }

            return static_cast<qpl_status>(status);
        }
#endif

        status = hw_submit_job(qpl_job_ptr);

        if (status == QPL_STS_OK) {
            state_ptr->job_is_submitted = true;
        }

#if defined(KEEP_DESCRIPTOR_ENABLED)
        if (status == QPL_STS_QUEUES_ARE_BUSY_ERR && qpl_path_hardware == qpl_job_ptr->data_ptr.path) {
            state_ptr->descriptor_not_submitted = true;
        }
#endif

        // Call SW-path fallback in case if HW limits are exceeded
        if (status != QPL_STS_OK && qpl_job_ptr->data_ptr.path == qpl_path_auto) {
            qpl_job_ptr->data_ptr.path = qpl_path_software;
        } else {
            return static_cast<qpl_status>(status);
        }
    }

    qpl_job_ptr->first_index_min_value = UINT32_MAX;

    auto *const analytics_state_ptr = reinterpret_cast<own_analytics_state_t *>(qpl_job_ptr->data_ptr.analytics_state_ptr);

    switch (qpl_job_ptr->op) {
        // processing compression
        case qpl_op_decompress: {
            status = perform_decompress<qpl::ml::execution_path_t::software>(qpl_job_ptr);

            if (qpl_job_ptr->flags & QPL_FLAG_LAST && QPL_STS_OK == status) {
                auto *const data_begin_ptr = qpl_job_ptr->next_out_ptr - qpl_job_ptr->total_out;
                auto *const data_end_ptr = qpl_job_ptr->next_out_ptr;

                qpl_job_ptr->xor_checksum = ml::util::xor_checksum(data_begin_ptr, data_end_ptr, 0);
            }
            break;
        }
        case qpl_op_compress: {
            status = perform_compression<qpl::ml::execution_path_t::software>(qpl_job_ptr);
            break;
        } // other operations
        case qpl_op_crc64: {
            status = perform_crc64(qpl_job_ptr);
            break;
        }
        case qpl_op_scan_eq:
        case qpl_op_scan_ne:
        case qpl_op_scan_lt:
        case qpl_op_scan_le:
        case qpl_op_scan_gt:
        case qpl_op_scan_ge:
        case qpl_op_scan_range:
        case qpl_op_scan_not_range: {
            status = perform_scan(qpl_job_ptr,
                                  analytics_state_ptr->unpack_buf_ptr,
                                  analytics_state_ptr->unpack_buf_size);
            break;
        }
        case qpl_op_extract: {
            if (qpl_job_ptr->param_low > qpl_job_ptr->param_high) {
                qpl_job_ptr->first_index_min_value = 0u;

                return QPL_STS_OK;
            }

            status = perform_extract(qpl_job_ptr,
                                     analytics_state_ptr->unpack_buf_ptr,
                                     analytics_state_ptr->unpack_buf_size);
            break;
        }
        case qpl_op_select: {
            status = perform_select(qpl_job_ptr,
                                    analytics_state_ptr->unpack_buf_ptr,
                                    analytics_state_ptr->unpack_buf_size,
                                    analytics_state_ptr->set_buf_ptr,
                                    analytics_state_ptr->set_buf_size,
                                    analytics_state_ptr->src2_buf_ptr,
                                    analytics_state_ptr->src2_buf_size);
            break;
        }
        case qpl_op_expand: {
            status = perform_expand(qpl_job_ptr,
                                    analytics_state_ptr->unpack_buf_ptr,
                                    analytics_state_ptr->unpack_buf_size,
                                    analytics_state_ptr->set_buf_ptr,
                                    analytics_state_ptr->set_buf_size,
                                    analytics_state_ptr->src2_buf_ptr,
                                    analytics_state_ptr->src2_buf_size);
            break;
        }
        default: {
            status = QPL_STS_OPERATION_ERR;
        }
    }

    qpl_job_ptr->data_ptr.path = path;

    return static_cast<qpl_status>(status);
}

QPL_FUN("C" qpl_status, qpl_check_job, (qpl_job *qpl_job_ptr)) {
    QPL_BAD_PTR_RET(qpl_job_ptr);
    uint32_t status = QPL_STS_OK;

    if (qpl::job::hardware_supported(qpl_job_ptr)) {
        status = hw_check_job(qpl_job_ptr);
    }

    return static_cast<qpl_status>(status);
}

QPL_FUN("C" qpl_status, qpl_wait_job, (qpl_job *qpl_job_ptr)) {
    QPL_BAD_PTR_RET(qpl_job_ptr);

    uint32_t status = QPL_STS_OK;
    // HW path doesn't support qpl_high_level compression ratio and ZLIB headers/trailers
    if (qpl::job::hardware_supported(qpl_job_ptr)) {
        do {
            status = hw_check_job(qpl_job_ptr);
        } while (QPL_STS_BEING_PROCESSED == status);
    }

    return static_cast<qpl_status>(status);
}

QPL_FUN("C" qpl_status, qpl_execute_job, (qpl_job * qpl_job_ptr)) {
    using namespace qpl;

    QPL_BAD_PTR_RET(qpl_job_ptr);

    if (job::hardware_supported(qpl_job_ptr)) {
        auto *const analytics_state_ptr = reinterpret_cast<own_analytics_state_t *>(qpl_job_ptr->data_ptr.analytics_state_ptr);

        if (job::is_extract(qpl_job_ptr)) {
            return static_cast<qpl_status>(perform_extract(qpl_job_ptr,
                                                           analytics_state_ptr->unpack_buf_ptr,
                                                           analytics_state_ptr->unpack_buf_size));
        }

        if (job::is_scan(qpl_job_ptr)) {
            return static_cast<qpl_status>(perform_scan(qpl_job_ptr,
                                                        analytics_state_ptr->unpack_buf_ptr,
                                                        analytics_state_ptr->unpack_buf_size));
        }

        if (job::is_select(qpl_job_ptr)) {
            return static_cast<qpl_status>(perform_select(qpl_job_ptr,
                                                          analytics_state_ptr->unpack_buf_ptr,
                                                          analytics_state_ptr->unpack_buf_size,
                                                          analytics_state_ptr->set_buf_ptr,
                                                          analytics_state_ptr->set_buf_size,
                                                          analytics_state_ptr->src2_buf_ptr,
                                                          analytics_state_ptr->src2_buf_size));
        }

        if (job::is_expand(qpl_job_ptr)) {
            return static_cast<qpl_status>(perform_expand(qpl_job_ptr,
                                                          analytics_state_ptr->unpack_buf_ptr,
                                                          analytics_state_ptr->unpack_buf_size,
                                                          analytics_state_ptr->set_buf_ptr,
                                                          analytics_state_ptr->set_buf_size,
                                                          analytics_state_ptr->src2_buf_ptr,
                                                          analytics_state_ptr->src2_buf_size));
        }

        if (job::is_decompression(qpl_job_ptr)) {
            return static_cast<qpl_status>(perform_decompress<ml::execution_path_t::hardware>(qpl_job_ptr));
        }

        if (job::is_compression(qpl_job_ptr) &&
            !(job::is_indexing_enabled(qpl_job_ptr) && job::is_multi_job(qpl_job_ptr))) {
            return static_cast<qpl_status>(perform_compression<ml::execution_path_t::hardware>(qpl_job_ptr));
        }

        qpl_status status = hw_submit_job(qpl_job_ptr);

        if (status == QPL_STS_OK) {
            auto *state_ptr = reinterpret_cast<qpl_hw_state *>(job::get_state(qpl_job_ptr));
            state_ptr->job_is_submitted = true;
        }

        return (QPL_STS_OK == status) ? qpl_wait_job(qpl_job_ptr) : status;
    }

    return qpl_submit_job(qpl_job_ptr);
}
