// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#pragma once

#include "mongo/db/matcher/expression.h"
#include "mongo/db/query/timeseries/bucket_spec.h"
#include "mongo/util/modules.h"

#include <memory>

namespace mongo::timeseries {

/**
 * A class encapsulating the logic for generating _id predicates from previously generated
 * predicates that use the control block.
 *
 * Searches for AND nodes that contain comparison with control.min.time or control.max.time, and
 * updates them to have predicates that compare on _id as well.
 *
 * For comparison directly against extended range values, may also replace existing predicates with
 * always-true or always-false.
 *
 * The match expression is modified in-place. If any changes were made, returns true, and false
 * otherwise.
 */
struct BucketLevelIdPredicateGenerator {
    static bool generateIdPredicates(const ExpressionContext& pExpCtx,
                                     const BucketSpec& bucketSpec,
                                     int bucketMaxSpanSeconds,
                                     MatchExpression* matchExpr);
};

}  // namespace mongo::timeseries
