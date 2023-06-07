/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

#ifndef QPL_MIDDLE_LAYER_COMPRESSION_VERIFY_HPP
#define QPL_MIDDLE_LAYER_COMPRESSION_VERIFY_HPP

#include <cstdint>
#include "compression/inflate/inflate_defs.hpp"
#include "compression/deflate/containers/index_table.hpp"
#include "verification_state.hpp"
#include "common/defs.hpp"
#include "compression/huffman_only/huffman_only_decompression_state.hpp"

namespace qpl::ml::compression {

template <execution_path_t path,
          verification_mode_t mode>
auto perform_verification(verify_state<path> &state) -> verification_result_t;
}

#endif // QPL_MIDDLE_LAYER_COMPRESSION_VERIFY_HPP
