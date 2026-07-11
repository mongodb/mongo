// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/debug_util.h"
#include "mongo/util/modules.h"

/**
 * This header file contains non-trivial default initializers for query knobs.
 */
namespace mongo {
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
