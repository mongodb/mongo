/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

#if defined(linux)

#include <x86intrin.h>

#endif

#include "numa.hpp"

namespace qpl::ml::util {

int32_t get_numa_id() noexcept {
#if defined(linux)
    uint32_t tsc_aux = 0;

    __rdtscp(&tsc_aux);

    // Linux encodes NUMA node into [32:12] of TSC_AUX
    return static_cast<int32_t>(tsc_aux >> 12);
#else
    // Not supported in Windows yet
    return -1;
#endif
}

}
