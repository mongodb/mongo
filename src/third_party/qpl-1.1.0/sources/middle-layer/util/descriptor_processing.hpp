/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

#ifndef QPL_DESCRIPTOR_PROCESSING_HPP
#define QPL_DESCRIPTOR_PROCESSING_HPP

#include <array>
#include <emmintrin.h>

#include "hw_definitions.h"
#include "hw_accelerator_api.h"
#include "util/hw_status_converting.hpp"
#include "util/completion_record.hpp"
#include "util/awaiter.hpp"

namespace qpl::ml::util {

enum class execution_mode_t {
    sync,
    async
};

template <typename return_t>
inline auto wait_descriptor_result(HW_PATH_VOLATILE hw_completion_record *const completion_record_ptr) -> return_t {
    awaiter::wait_for(&completion_record_ptr->status, AD_STATUS_INPROG);

    return ml::util::completion_record_convert_to_result<return_t>(completion_record_ptr);
}

template <typename return_t, execution_mode_t mode>
inline auto process_descriptor(hw_descriptor *const descriptor_ptr,
                               HW_PATH_VOLATILE hw_completion_record *const completion_record_ptr,
                               int32_t numa_id = -1) noexcept -> return_t {
    return_t operation_result;

    hw_iaa_descriptor_set_completion_record(descriptor_ptr, completion_record_ptr);
    completion_record_ptr->status = AD_STATUS_INPROG; // Mark completion record as not completed

    auto accel_status = hw_enqueue_descriptor(descriptor_ptr, numa_id);

    if constexpr (mode == execution_mode_t::sync) {
        uint32_t status = convert_hw_accelerator_status_to_qpl_status(accel_status);
        if (status_list::ok != status) {
            if constexpr(std::is_same<decltype(status), return_t>::value) {
                return status;
            } else {
                operation_result.status_code_ = status;
                return operation_result;
            }
        }

        operation_result = wait_descriptor_result<return_t>(completion_record_ptr);

        if constexpr (std::is_same<other::crc_operation_result_t, return_t>::value) {
            operation_result.processed_bytes_ = reinterpret_cast<hw_iaa_analytics_descriptor *>(descriptor_ptr)->src1_size;
        }
    } else {
        if constexpr (std::is_same<other::crc_operation_result_t, return_t>::value) {
            operation_result.status_code_ = convert_hw_accelerator_status_to_qpl_status(accel_status);
        } else {
            operation_result = static_cast<return_t>(convert_hw_accelerator_status_to_qpl_status(accel_status));
        }
    }

    return operation_result;
}

template <typename return_t, uint32_t number_of_descriptors>
inline auto process_descriptor(std::array<hw_descriptor, number_of_descriptors> &descriptors,
                               std::array<hw_completion_record, number_of_descriptors> &completion_records,
                               int32_t numa_id) noexcept -> return_t {
    return_t operation_result{};

    for (uint32_t i = 0; i < descriptors.size(); i++) {
        hw_iaa_descriptor_set_completion_record(&descriptors[i], &completion_records[i]);
        completion_records[i].status = AD_STATUS_INPROG; // Mark completion record as not completed

        if constexpr (std::is_same_v<return_t, uint32_t>) {
            operation_result = process_descriptor<uint32_t, execution_mode_t::async>(&descriptors[i],
                                                                                     &completion_records[i],
                                                                                     numa_id);
            if (operation_result != status_list::ok) {
                return operation_result;
            }
        } else {
            operation_result.status_code_ = process_descriptor<uint32_t, execution_mode_t::async>(&descriptors[i],
                                                                                                  &completion_records[i],
                                                                                                  numa_id);
            if (operation_result.status_code_ != status_list::ok) {
                return operation_result;
            }
        }
    }

    for (uint32_t i = 0; i < descriptors.size(); i++) {
        auto execution_status = ml::util::wait_descriptor_result<return_t>(&completion_records[i]);

        if (execution_status.status_code_ != status_list::ok) {
            operation_result.status_code_ = execution_status.status_code_;
            return operation_result;
        } else {
            operation_result.output_bytes_ += execution_status.output_bytes_;
            operation_result.last_bit_offset_ = execution_status.last_bit_offset_; // TODO: In case of number_of_elements per descriptor modification should be adapted
        }
    }
    return operation_result;
}

}

#endif //QPL_DESCRIPTOR_PROCESSING_HPP
