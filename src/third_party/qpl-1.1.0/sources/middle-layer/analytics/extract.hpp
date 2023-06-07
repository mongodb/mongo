/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

#ifndef EXTRACT_OPERATION_HPP
#define EXTRACT_OPERATION_HPP

#include "input_stream.hpp"
#include "output_stream.hpp"

namespace qpl::ml::analytics {

template <execution_path_t path>
auto call_extract(input_stream_t &input_stream,
                  output_stream_t<array_stream> &output_stream,
                  uint32_t param_low,
                  uint32_t param_high,
                  limited_buffer_t &temporary_buffer,
                  int32_t numa_id = -1) noexcept -> analytic_operation_result_t;

} // namespace qpl::ml::analytics

#endif // EXTRACT_OPERATION_HPP
