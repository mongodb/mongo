/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

#ifndef QPL_SOURCES_MIDDLE_LAYER_ANALYTICS_SELECT_HPP_
#define QPL_SOURCES_MIDDLE_LAYER_ANALYTICS_SELECT_HPP_

#include "input_stream.hpp"
#include "output_stream.hpp"

namespace qpl::ml::analytics {

template <execution_path_t path>
auto call_select(input_stream_t &input_stream,
                 input_stream_t &mask_stream,
                 output_stream_t<array_stream> &output_stream,
                 limited_buffer_t &unpack_buffer,
                 limited_buffer_t &set_buffer,
                 limited_buffer_t &output_buffer,
                 int32_t numa_id = -1) noexcept -> analytic_operation_result_t;

} // namespace qpl::ml::analytics

#endif //QPL_SOURCES_MIDDLE_LAYER_ANALYTICS_SELECT_HPP_
