/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

/*
 *  Intel® Query Processing Library (Intel® QPL)
 *  Job API (public C API)
 */

#ifndef QPL_SOURCES_C_API_COMPRESSION_OPERATIONS_ARGUMENTS_CHECK_HPP_
#define QPL_SOURCES_C_API_COMPRESSION_OPERATIONS_ARGUMENTS_CHECK_HPP_

#include "job.hpp"
#include "own_checkers.h"
#include "compression_state_t.h"

#include "util/checkers.hpp"

namespace qpl::job {

namespace details {

auto inline bad_arguments_check(const qpl_job *const job_ptr) -> uint32_t {
    // Check ignore bits fields
    if (job_ptr->ignore_start_bits > 7u ||
        job_ptr->ignore_end_bits > 7u) {
        return QPL_STS_INVALID_PARAM_ERR;
    }

    // Check decomp_end_processing field
    if (job_ptr->decomp_end_processing > qpl_check_on_nonlast_block) {
        return QPL_STS_INVALID_DECOMP_END_PROC_ERR;
    }

    if (job_ptr->decomp_end_processing == OWN_RESERVED_INFLATE_MANIPULATOR) {
        return QPL_STS_INVALID_DECOMP_END_PROC_ERR;
    }

    // Check flags correctness
    if ((job_ptr->flags & QPL_FLAG_RND_ACCESS) &&
        (job_ptr->flags & QPL_FLAG_NO_HDRS)) {
        return QPL_STS_FLAG_CONFLICT_ERR;
    }

    if ((job_ptr->flags & QPL_FLAG_GZIP_MODE) &&
        (job_ptr->flags & QPL_FLAG_NO_HDRS)) {
        return QPL_STS_FLAG_CONFLICT_ERR;
    }

    if ((job_ptr->flags & QPL_FLAG_ZLIB_MODE) &&
        (job_ptr->flags & QPL_FLAG_NO_HDRS)) {
        return QPL_STS_FLAG_CONFLICT_ERR;
    }

    if ((job_ptr->flags & QPL_FLAG_ZLIB_MODE) &&
        (job_ptr->flags & QPL_FLAG_GZIP_MODE)) {
        return QPL_STS_FLAG_CONFLICT_ERR;
    }

    return QPL_STS_OK;
}
}

template <>
inline auto validate_mode<qpl_operation::qpl_op_compress>(const qpl_job * const qpl_job_ptr) noexcept {
    if (qpl_job_ptr->level != qpl_high_level && qpl_job_ptr->level != qpl_default_level) {
        return ml::status_list::not_supported_level_err;
    }

    if (qpl_job_ptr->level == qpl_high_level &&
        (job::get_execution_path(qpl_job_ptr) == ml::execution_path_t::hardware)) {
        return ml::status_list::not_supported_level_err;
    }

    constexpr auto USER_HUFFMAN_TABLE_USED = 1u;
    constexpr auto NO_USER_HUFFMAN_TABLE   = 0u;

    auto *compression_state = reinterpret_cast<own_compression_state_t *>(qpl_job_ptr->data_ptr.compress_state_ptr);

    auto compression_style = (qpl_job_ptr->flags & QPL_FLAG_DYNAMIC_HUFFMAN) |
                             ((qpl_job_ptr->huffman_table) ?
                              USER_HUFFMAN_TABLE_USED : NO_USER_HUFFMAN_TABLE);

    if (!(qpl_job_ptr->flags & QPL_FLAG_FIRST) &&
        compression_state->middle_layer_compression_style != compression_style) {
        return ml::status_list::invalid_compression_style_error;
    } else {
        // Set Compression Style
        compression_state->middle_layer_compression_style = compression_style;
    }

    return ml::status_list::ok;
}

template <>
inline auto validate_flags<qpl_operation::qpl_op_compress>(const qpl_job *const job_ptr) noexcept {
    auto const job = const_cast<qpl_job *>(job_ptr);

    if (job->flags & QPL_FLAG_HUFFMAN_BE && !(job->flags & QPL_FLAG_NO_HDRS)) {
        return QPL_STS_FLAG_CONFLICT_ERR;
    }

    if (job->flags & QPL_FLAG_NO_HDRS && job->flags & QPL_FLAG_GEN_LITERALS) {
        job->level = qpl_default_level;
    } else if (job->flags & QPL_FLAG_NO_HDRS &&
               job->flags & QPL_FLAG_DYNAMIC_HUFFMAN &&
               !(is_single_job(job_ptr))) {
        return QPL_STS_FLAG_CONFLICT_ERR;
    }

    if (job->flags & (QPL_FLAG_ZLIB_MODE | QPL_FLAG_GZIP_MODE) && job::is_dictionary(job)) {
        return QPL_STS_NOT_SUPPORTED_MODE_ERR;
    }

    if (!(job->flags & QPL_FLAG_OMIT_VERIFY) && job::is_dictionary(job)) {
        return QPL_STS_NOT_SUPPORTED_MODE_ERR;
    }

    if (!(job->huffman_table) && job::is_huffman_only_compression(job)) {
        return QPL_STS_NOT_SUPPORTED_MODE_ERR;
    }

    return QPL_STS_OK;
}


template<>
inline auto validate_operation<qpl_op_compress>(const qpl_job *const job_ptr) noexcept {
    if (ml::bad_argument::check_for_nullptr(job_ptr, job_ptr->next_in_ptr, job_ptr->next_out_ptr)) {
        return QPL_STS_NULL_PTR_ERR;
    }

    if (ml::bad_argument::buffers_overlap(job_ptr->next_in_ptr, job_ptr->available_in,
                                      job_ptr->next_out_ptr, job_ptr->available_out)) {
        return QPL_STS_BUFFER_OVERLAP_ERR;
    }

    if (job_ptr->available_in == 0 || job_ptr->available_out == 0) {
        return QPL_STS_SIZE_ERR;
    }

    if (job_ptr->mini_block_size != qpl_mblk_size_none && job_ptr->idx_array == nullptr) {
        return QPL_STS_MISSING_INDEX_TABLE_ERR;
    }

    OWN_QPL_CHECK_STATUS(job::validate_flags<qpl_operation::qpl_op_compress>(job_ptr));
    OWN_QPL_CHECK_STATUS(job::validate_mode<qpl_operation::qpl_op_compress>(job_ptr));

    return QPL_STS_OK;
}

template<>
inline auto validate_operation<qpl_op_decompress>(const qpl_job *const job_ptr) noexcept {
    if (ml::bad_argument::check_for_nullptr(job_ptr, job_ptr->next_in_ptr, job_ptr->next_out_ptr)) {
        return QPL_STS_NULL_PTR_ERR;
    }

    if (ml::bad_argument::buffers_overlap(job_ptr->next_in_ptr, job_ptr->available_in,
                                      job_ptr->next_out_ptr, job_ptr->available_out)) {
        return QPL_STS_BUFFER_OVERLAP_ERR;
    }

    OWN_QPL_CHECK_STATUS(details::bad_arguments_check(job_ptr))

    if (!(job_ptr->huffman_table) && job::is_huffman_only_decompression(job_ptr)) {
        return QPL_STS_NOT_SUPPORTED_MODE_ERR;
    }

    return QPL_STS_OK;
}

}

#endif //QPL_SOURCES_C_API_COMPRESSION_OPERATIONS_ARGUMENTS_CHECK_HPP_
