// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/query/query_planner_params.h"
#include "mongo/util/modules.h"

namespace mongo {
class CanonicalQuery;
class MultipleCollectionAccessor;
class Pipeline;

/**
 * Removes the first 'stagesToRemove' stages from the pipeline. This function is meant to be paired
 * with a call to attachPipelineStages() - the caller must first get the stages for push down, add
 * them to the canonical query, and only then remove them from the pipeline.
 */
void finalizePipelineStages(Pipeline* pipeline, const CanonicalQuery* canonicalQuery);

/**
 * Identifies the prefix of the 'pipeline' that is eligible for running in SBE and adds it to the
 * provided 'canonicalQuery'.
 */
void attachPipelineStages(const MultipleCollectionAccessor& collections,
                          const Pipeline* pipeline,
                          bool needsMerge,
                          CanonicalQuery* canonicalQuery,
                          std::unique_ptr<QueryPlannerParams> plannerParams);

/**
 * Increments in the operation context of the CanonicalQuery the non-leading pushdown counters. They
 * track whether the given query contains at least 1 non-leading $match, $project, $addFields and
 * $replaceRoot that was pushed down to SBE.
 *
 * The function assumes that CqPipeline won't be changed anymore, that is, that it contains the
 * final stages that will run in SBE.
 */
void incrementNonLeadingPushdownCounters(const CanonicalQuery& cq);
}  // namespace mongo
