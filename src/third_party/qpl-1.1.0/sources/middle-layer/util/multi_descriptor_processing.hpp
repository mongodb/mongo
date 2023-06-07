/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/


#ifndef QPL_MULTI_DESCRIPTOR_PROCESSING_HPP
#define QPL_MULTI_DESCRIPTOR_PROCESSING_HPP

#include <array>
#include "hw_definitions.h"
#include "hw_descriptors_api.h"
#include "analytics/input_stream.hpp"
#include "analytics/output_stream.hpp"

namespace qpl::ml::analytics {

template <qpl_operation operation,uint32_t number_of_descriptors>
void split_descriptors(hw_descriptor &reference_descriptor,
                       std::array<hw_descriptor, number_of_descriptors> &descriptors) noexcept;

template <>
void split_descriptors<qpl_operation::qpl_op_scan_eq, 8>(hw_descriptor &reference_descriptor,
                                                         std::array<hw_descriptor, 8> &descriptors) noexcept;

auto is_hw_configuration_good_for_splitting() noexcept -> bool;

auto is_operation_splittable(const input_stream_t &input_stream,
                             const output_stream_t<output_stream_type_t::bit_stream> &output_stream) noexcept -> bool;
}

#endif //QPL_MULTI_DESCRIPTOR_PROCESSING_HPP
