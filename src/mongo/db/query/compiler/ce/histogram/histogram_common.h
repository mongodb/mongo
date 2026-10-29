// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/query/compiler/ce/ce_common.h"
#include "mongo/db/query/compiler/stats/ce_histogram.h"
#include "mongo/util/modules.h"

namespace mongo::ce {

/**
 * Distinguish cardinality estimation algorithms for range queries over array values.
 * Given an example interval a: [x, y]
 * - kConjunctArrayCE estimates cardinality for queries that translate the interval into
 * conjunctions during planning. e.g, [x,y] is translated into
 * {$and: [ {a: { $lte: y }}, {a: { $gte: x }}]} i.e., (-inf, y] AND [x, +inf)
 * This approach will estimate the number of arrays that values collectively satisfy the
 * conjunction i.e., at least on that satisfies (-inf, y] and at least one that satisfies [x, +inf).
 * - kExactArrayCE estimates the cardinality of arrays that have at least one value that falls in
 * the given interval.
 */
enum class ArrayRangeEstimationAlgo { kConjunctArrayCE, kExactArrayCE };

}  // namespace mongo::ce
