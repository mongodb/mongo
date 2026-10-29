// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#pragma once

#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/query/compiler/optimizer/join/agg_join_model.h"
#include "mongo/db/query/multiple_collection_accessor.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/modules.h"

namespace mongo::join_ordering {

/**
 * Failpoint that introduces a sleep during join optimization. Used by tests to verify that
 * optimizationTimeMillis is correctly measured and reported in explain output.
 */
extern FailPoint sleepWhileJoinOptimizing;

struct JoinReorderedExecutorResult {
    // Executor for pushed-down & reordered SBE prefix.
    std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> executor;
    // Model describing the join graph extracted from a pipeline and the DocumentSource suffix that
    // still needs to be attached to the executor.
    AggJoinModel model;
};

/**
 * Attempts to apply join optimization to the given aggregation, but if it fails to extract a join
 * model, returns an error status.
 */
StatusWith<JoinReorderedExecutorResult> getJoinReorderedExecutor(
    const MultipleCollectionAccessor& mca,
    const Pipeline& pipeline,
    const boost::optional<BSONObj>& queryHint,
    OperationContext* opCtx,
    boost::intrusive_ptr<ExpressionContext> expCtx);
}  // namespace mongo::join_ordering
