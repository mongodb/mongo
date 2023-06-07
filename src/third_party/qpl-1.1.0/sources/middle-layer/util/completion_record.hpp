/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

#ifndef QPL_COMPLETION_RECORD_HPP
#define QPL_COMPLETION_RECORD_HPP

#include "common/defs.hpp"
#include "analytics/analytics_defs.hpp"
#include "other/other_defs.hpp"
#include "compression/zero_defs.hpp"
#include "compression/compression_defs.hpp"
#include "hw_completion_record_api.h"

namespace qpl::ml::util {


static inline auto convert_status_iaa_to_qpl(HW_PATH_VOLATILE const hw_completion_record *const completion_record) {
    if (completion_record->error) {
        return status_list::hardware_error_base + completion_record->error;
    }

    if (AD_STATUS_SUCCESS != completion_record->status) {
        return status_list::hardware_status_base + completion_record->status;
    }

    return status_list::ok;
}

template <class return_t>
inline auto completion_record_convert_to_result(HW_PATH_VOLATILE hw_completion_record *completion_record_ptr) noexcept -> return_t;

template <>
inline auto completion_record_convert_to_result<uint32_t>(HW_PATH_VOLATILE hw_completion_record *completion_record_ptr) noexcept -> qpl_ml_status {
    return convert_status_iaa_to_qpl(completion_record_ptr);
}

template <>
inline auto completion_record_convert_to_result<analytics::analytic_operation_result_t>(HW_PATH_VOLATILE hw_completion_record * completion_record_ptr)
noexcept -> analytics::analytic_operation_result_t {
    analytics::analytic_operation_result_t analytic_operation_result{};

    auto *const analytic_completion_record = reinterpret_cast<HW_PATH_VOLATILE hw_iaa_completion_record *>(completion_record_ptr);

    analytic_operation_result.status_code_           = convert_status_iaa_to_qpl(completion_record_ptr);
    analytic_operation_result.last_bit_offset_       = analytic_completion_record->output_bits;
    analytic_operation_result.aggregates_.min_value_ = analytic_completion_record->min_first_agg;
    analytic_operation_result.aggregates_.max_value_ = analytic_completion_record->max_last_agg;
    analytic_operation_result.aggregates_.sum_       = analytic_completion_record->sum_agg;
    analytic_operation_result.checksums_.crc32_      = analytic_completion_record->crc;
    analytic_operation_result.checksums_.xor_        = analytic_completion_record->xor_checksum;
    analytic_operation_result.output_bytes_          = analytic_completion_record->output_size;

    return analytic_operation_result;
}

template <>
inline auto completion_record_convert_to_result<other::crc_operation_result_t>(HW_PATH_VOLATILE hw_completion_record *completion_record_ptr)
noexcept -> other::crc_operation_result_t {
    other::crc_operation_result_t crc_operation_result{};

    auto *const analytic_completion_record = reinterpret_cast<HW_PATH_VOLATILE hw_iaa_completion_record *>(completion_record_ptr);

    crc_operation_result.status_code_ = convert_status_iaa_to_qpl(completion_record_ptr);
    crc_operation_result.crc_ = (static_cast<uint64_t>(analytic_completion_record->sum_agg) << 32u)
                                | static_cast<uint64_t>(analytic_completion_record->max_last_agg);

    return crc_operation_result;
}

template <>
inline auto completion_record_convert_to_result<compression::zero_operation_result_t>(HW_PATH_VOLATILE hw_completion_record * completion_record_ptr)
noexcept -> compression::zero_operation_result_t {
    compression::zero_operation_result_t zero_operation_result{};

    auto *const zero_completion_record = reinterpret_cast<HW_PATH_VOLATILE hw_iaa_completion_record *>(completion_record_ptr);

    zero_operation_result.status_code_           = convert_status_iaa_to_qpl(completion_record_ptr);
    zero_operation_result.aggregates_.min_value_ = zero_completion_record->min_first_agg;
    zero_operation_result.aggregates_.max_value_ = zero_completion_record->max_last_agg;
    zero_operation_result.aggregates_.sum_       = zero_completion_record->sum_agg;
    zero_operation_result.checksums_.crc32_      = zero_completion_record->crc;
    zero_operation_result.checksums_.xor_        = zero_completion_record->xor_checksum;
    zero_operation_result.output_bytes_          = zero_completion_record->output_size;

    return zero_operation_result;
}

template <>
inline auto completion_record_convert_to_result<compression::decompression_operation_result_t>(HW_PATH_VOLATILE hw_completion_record * completion_record_ptr)
noexcept -> compression::decompression_operation_result_t {
    compression::decompression_operation_result_t decompression_operation_result{};

    auto *const completion_record = reinterpret_cast<HW_PATH_VOLATILE hw_iaa_completion_record *>(completion_record_ptr);

    decompression_operation_result.status_code_      = convert_status_iaa_to_qpl(completion_record_ptr);
    decompression_operation_result.output_bytes_     = completion_record->output_size;
    decompression_operation_result.completed_bytes_  = completion_record->bytes_completed;
    decompression_operation_result.checksums_.crc32_ = completion_record->crc;
    decompression_operation_result.checksums_.xor_ = completion_record->xor_checksum;

    return decompression_operation_result;
}

template <>
inline auto completion_record_convert_to_result<compression::compression_operation_result_t>(HW_PATH_VOLATILE hw_completion_record * completion_record_ptr)
noexcept -> compression::compression_operation_result_t {
    compression::compression_operation_result_t compression_operation_result{};

    auto *const completion_record = reinterpret_cast<HW_PATH_VOLATILE hw_iaa_completion_record *>(completion_record_ptr);

    compression_operation_result.status_code_      = convert_status_iaa_to_qpl(completion_record_ptr);
    compression_operation_result.output_bytes_     = completion_record->output_size;
    compression_operation_result.completed_bytes_  = completion_record->bytes_completed;
    compression_operation_result.last_bit_offset   = completion_record->output_bits & 7;
    compression_operation_result.checksums_.crc32_ = completion_record->crc;
    compression_operation_result.checksums_.xor_ = completion_record->xor_checksum;

    return compression_operation_result;
}

template <>
inline auto completion_record_convert_to_result<compression::verification_pass_result_t>(HW_PATH_VOLATILE hw_completion_record * completion_record_ptr)
noexcept -> compression::verification_pass_result_t {
    compression::verification_pass_result_t verification_pass_result{};

    auto *const completion_record = reinterpret_cast<HW_PATH_VOLATILE hw_iaa_completion_record *>(completion_record_ptr);

    verification_pass_result.status_code_      = convert_status_iaa_to_qpl(completion_record_ptr);
    verification_pass_result.indexes_written_  = completion_record->output_size / sizeof (uint64_t);
    verification_pass_result.checksums_.crc32_ = completion_record->crc;
    verification_pass_result.checksums_.xor_ = completion_record->xor_checksum;

    return verification_pass_result;
}

} // namespace qpl::ml::util

#endif //QPL_COMPLETION_RECORD_HPP
