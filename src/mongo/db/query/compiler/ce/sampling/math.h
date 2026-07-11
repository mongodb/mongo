// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/compiler/ce/ce_common.h"
#include "mongo/util/modules.h"

namespace mongo::ce {
/**
 * Use Newton-Raphson iteration to approximate the solution to the Method-of-Moments (MM) estimation
 * for the number of distinct values in the data set, given the number of distinct values in the
 * sample.
 */
CardinalityEstimate newtonRaphsonNDV(size_t sampleNDV, size_t sampleSize);
}  // namespace mongo::ce
