/**
 *    Copyright (C) 2024-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

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
