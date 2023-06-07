/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

#ifndef QPL_MIDDLE_LAYER_COMPRESSION_INFLATE_INFLATE_HPP
#define QPL_MIDDLE_LAYER_COMPRESSION_INFLATE_INFLATE_HPP

#include <cstdint>
#include "common/defs.hpp"
#include "inflate_defs.hpp"
#include "compression/compression_defs.hpp"

namespace qpl::ml::compression {

template<execution_path_t path, inflate_mode_t mode = inflate_default>
auto inflate(inflate_state<path> &decompression_state,
             end_processing_condition_t end_processing_condition) noexcept -> decompression_operation_result_t;

}
#endif // QPL_MIDDLE_LAYER_COMPRESSION_INFLATE_INFLATE_HPP
