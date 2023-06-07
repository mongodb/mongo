/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

#ifndef OTHER_DEFS_HPP_
#define OTHER_DEFS_HPP_

#include <cstdint>

namespace qpl::ml::other {

struct crc_operation_result_t {
    uint32_t status_code_     = 0u;
    uint32_t processed_bytes_ = 0u;
    uint64_t crc_             = 0u;
};

} // namespace qpl::ml::other

#endif // OTHER_DEFS_HPP_
