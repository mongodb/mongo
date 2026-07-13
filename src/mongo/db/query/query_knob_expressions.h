// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/debug_util.h"
#include "mongo/util/modules.h"
#include "mongo/util/processinfo.h"

#include <algorithm>

/**
 * This header file contains non-trivial default initializers for query knobs.
 */
namespace mongo {
/**
 * Returns the default value of the 'internalQueryMaxMemoryUsageBytesPerOperation' query knob given
 * the total memory available to the process: max(1GB, 20% of 'availableMemoryBytes'). This overload
 * takes the memory size as a parameter so it can be unit tested with injected values.
 */
inline long long defaultInternalQueryMaxMemoryUsageBytesPerOperation(
    unsigned long long availableMemoryBytes) {
    constexpr unsigned long long kOneGB = 1ULL * 1024 * 1024 * 1024;
    // availableMemoryBytes / 5 (20%) is at most ULLONG_MAX/5 (~3.7e18), which is always less than
    // LLONG_MAX (~9.2e18), so the narrowing cast can never overflow or wrap negative.
    return static_cast<long long>(std::max(kOneGB, availableMemoryBytes / 5));
}

/**
 * Production entry point: resolves the percentage against ProcessInfo::getMemSizeBytes(), which
 * respects cgroup/container limits rather than raw hardware size.
 */
inline long long defaultInternalQueryMaxMemoryUsageBytesPerOperation() {
    return defaultInternalQueryMaxMemoryUsageBytesPerOperation(ProcessInfo::getMemSizeBytes());
}

/**
 * Returns the default value of the 'internalPipelineLengthLimit' query knob.
 */
constexpr int defaultInternalPipelineLengthLimit() {
    // If you change this function please update the 'getExpectedPipelineLimit()' function in
    // aggregation_pipeline_utils.js accordingly.
    if constexpr (kDebugBuild) {
        return 200;
    }

#if defined(__s390x__)
    return 700;
#else
    return 1000;
#endif
}
}  // namespace mongo
