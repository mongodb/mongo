// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#pragma once

#include "mongo/db/pipeline/optimization/rule_based_rewriter.h"

namespace mongo::rule_based_rewrites::pipeline {

/**
 * Precondition for REDUNDANT_SORT_REMOVAL: returns true if the $sort at the current pipeline
 * position is made redundant by a preceding stage that already guarantees the same or stricter
 * sort order.
 */
bool sortIsRedundantGivenPrecedingStages(const PipelineRewriteContext& ctx);

}  // namespace mongo::rule_based_rewrites::pipeline
