/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

#ifndef QPL_SOURCES_MIDDLE_LAYER_DISPATCHER_NUMA_HPP_
#define QPL_SOURCES_MIDDLE_LAYER_DISPATCHER_NUMA_HPP_


#include "common/defs.hpp"

namespace qpl::ml::util {

int32_t get_numa_id() noexcept;

}

#endif //QPL_SOURCES_MIDDLE_LAYER_DISPATCHER_NUMA_HPP_
