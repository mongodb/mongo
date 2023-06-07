/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

/*
 *  Intel® Query Processing Library (Intel® QPL)
 *  Job API (public C API)
 */

#include "job.hpp"
#include "crc64.hpp"
#include "other/crc.hpp"
#include "arguments_check.hpp"

uint32_t perform_crc64(qpl_job *const job_ptr) noexcept {
    using namespace qpl::ml;

    auto validate_result = qpl::job::validate_operation<qpl_op_crc64>(job_ptr);

    if (validate_result) return validate_result;

    other::crc_operation_result_t result;

    bool is_be_bit_order = job_ptr->flags & QPL_FLAG_CRC64_BE;
    bool is_inverse = job_ptr->flags & QPL_FLAG_CRC64_INV;

    switch (qpl::job::get_execution_path(job_ptr)) {
        case execution_path_t::auto_detect:
            result = other::call_crc<execution_path_t::auto_detect>(job_ptr->next_in_ptr,
                                                                    job_ptr->available_in,
                                                                    job_ptr->crc64_poly,
                                                                    is_be_bit_order,
                                                                    is_inverse,
                                                                    job_ptr->numa_id);
            break;
        case execution_path_t::hardware:
            result = other::call_crc<execution_path_t::hardware>(job_ptr->next_in_ptr,
                                                                 job_ptr->available_in,
                                                                 job_ptr->crc64_poly,
                                                                 is_be_bit_order,
                                                                 is_inverse,
                                                                 job_ptr->numa_id);
            break;
        case execution_path_t::software:
            result = other::call_crc<execution_path_t::software>(job_ptr->next_in_ptr,
                                                                 job_ptr->available_in,
                                                                 job_ptr->crc64_poly,
                                                                 is_be_bit_order,
                                                                 is_inverse,
                                                                 job_ptr->numa_id);
            break;
    }

    qpl::job::update_crc(job_ptr, result.crc_);
    qpl::job::update_input_stream(job_ptr, result.processed_bytes_);

    return result.status_code_;
}
