// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#pragma once

#include "mongo/db/query/compiler/ce/sampling/sampling_estimator.h"
#include "mongo/db/query/compiler/optimizer/cost_based_ranker/estimates_storage.h"
#include "mongo/db/query/compiler/optimizer/join/cardinality_estimation_types.h"
#include "mongo/db/query/compiler/optimizer/join/catalog_stats.h"
#include "mongo/db/query/compiler/optimizer/join/join_graph.h"
#include "mongo/db/query/compiler/physical_model/query_solution/query_solution.h"
#include "mongo/db/shard_role/shard_catalog/index_catalog_entry.h"
#include "mongo/util/modules.h"

#include <absl/container/flat_hash_map.h>

namespace mongo::join_ordering {

// Alias for container maintaining association between a CanonicalQuery and its corresponding
// solution.
using QuerySolutionMap = stdx::unordered_map<CanonicalQuery*, std::unique_ptr<QuerySolution>>;

// Maps namespaces to indexes that are available for use by join reordering.
using AvailableIndexes =
    absl::flat_hash_map<NamespaceString, std::vector<std::shared_ptr<const IndexCatalogEntry>>>;

using PerCollUniqueFieldInfo = absl::flat_hash_map<NamespaceString, UniqueFieldInformation>;

// Maps namespaces to their corresponding sampling-based cardinality estimators.
using SamplingEstimatorMap =
    stdx::unordered_map<NamespaceString, std::unique_ptr<ce::SamplingEstimator>>;

/**
 * Struct containing results from 'singleTableAccessPlans()' function.
 */
struct SingleTableAccessPlansResult {
    QuerySolutionMap cbrCqQsns;
    cost_based_ranker::EstimateMap estimate;

    // Stores cardinality estimates for nodes after single-table predicates are applied.
    NodeCardinalities nodeCardinalities;

    // Stores cardinalities for the underlying collections as reported by the catalog.
    NodeCardinalities collCardinalities;

    // Per-node CBR costs for the winning single-table plans.
    NodeCBRCosts nodeCBRCosts;
};

/**
 * A struct tracking all information needed to reorder joins and generate a join plan.
 */
struct JoinReorderingContext {
    const JoinGraph& joinGraph;
    const std::vector<ResolvedPath>& resolvedPaths;
    SingleTableAccessPlansResult singleTableAccess;
    AvailableIndexes perCollIdxs;
    CatalogStats catStats;

    // The information about what combinations are fields are unique based on index metadata. If
    // there is no information known for a given collection, it may not exist in the map.
    PerCollUniqueFieldInfo uniqueFieldInfo;

    // Sampling estimators per collection, used to estimate NDV of index key fields during costing.
    // May be null (e.g. in tests) in which case NDV-based costing falls back to using docsOutput.
    const SamplingEstimatorMap* samplingEstimators = nullptr;

    // Whether or not we are explaining the query. Based on this flag we may, for example, keep
    // around costing/CE information to display in the explain output.
    bool explain = false;
};

}  // namespace mongo::join_ordering
