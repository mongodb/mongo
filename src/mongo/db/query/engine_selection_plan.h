// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/query/compiler/physical_model/query_solution/query_solution.h"
#include "mongo/db/query/engine_selection.h"
#include "mongo/util/modules.h"

namespace mongo {

class IncrementalFeatureRolloutContext;

/**
 * Returns 'false' for query plans that can not be executed in SBE.
 */
bool isPlanSbeCompatible(const QuerySolution* solution);

/**
 * Returns the engine of choice for executing the specified query plan.
 *
 * 'ifrContext' is used to snapshot IFR flag values for consistent reads within a single query
 * operation. Tests that construct query solution trees directly should pass a
 * default-constructed IncrementalFeatureRolloutContext, which falls back to direct flag reads.
 */
EngineSelectionResult engineSelectionForPlan(const QuerySolution* solution,
                                             const QuerySolutionNode* dataAccessNode,
                                             IncrementalFeatureRolloutContext& ifrContext);

/**
 * Returns true iff 'keyPattern' has fields A and B where all of the following hold
 *
 *   - A is a path prefix of B
 *   - A is a hashed field in the index
 *   - B is a non-hashed field in the index
 *
 * TODO SERVER-99889 this is a workaround for an SBE stage builder bug.
 */
bool indexHasHashedPathPrefixOfNonHashedPath(const BSONObj& keyPattern);

}  // namespace mongo
