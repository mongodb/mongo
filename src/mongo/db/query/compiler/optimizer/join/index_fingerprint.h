// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/compiler/optimizer/join/join_graph.h"
#include "mongo/db/query/compiler/optimizer/join/join_reordering_context.h"
#include "mongo/db/query/compiler/optimizer/join/logical_defs.h"
#include "mongo/db/query/plan_cache/join_plan_cache.h"
#include "mongo/db/shard_role/shard_catalog/index_catalog_entry.h"
#include "mongo/util/string_map.h"

#include <memory>
#include <vector>

namespace mongo::join_ordering {

/**
 * Computes one fingerprint per node of 'graph', in ascending NodeId order, from the INLJ-eligible
 * indexes in 'perCollIdxs' and the indexes 'plan' uses. Used both when populating a join plan cache
 * entry and when validating one whose collection version tags no longer match
 *
 * 'perCollIdxs' must contain an entry for every namespace referenced by 'graph'.
 */
std::vector<NodeFingerprint> makeNodeFingerprints(const JoinGraph& graph,
                                                  const std::vector<ResolvedPath>& resolvedPaths,
                                                  const AvailableIndexes& perCollIdxs,
                                                  const CachedJoinPlan& plan);

/**
 * Whether a cached plan whose node was fingerprinted as 'cached' can still be reused now that the
 * same node fingerprints as 'current'.
 *
 * An index the plan uses changing in any way forces a replan, as does any index becoming newly
 * relevant. Dropping a relevant index the plan did not use cannot change which plan would be
 * chosen, so the cached one is kept.
 */
bool canReuseNodeFingerprint(const NodeFingerprint& cached, const NodeFingerprint& current);

/**
 * The names of the indexes 'plan' uses, per node of the join graph, in ascending NodeId order.
 * 'numNodes' is the number of nodes in the graph 'plan' was cached for.
 *
 * Exposed for testing only.
 */
std::vector<StringSet> usedIndexNamesPerNode(const CachedJoinPlan& plan, size_t numNodes);

/**
 * Every field path that 'cq' references, across its filter, projection and sort.
 *
 * Intentionally a superset of the fields the optimizer will actually build index bounds on: this
 * feeds cache invalidation, where including an irrelevant field only costs an unnecessary replan
 * but omitting a relevant one lets a plan referencing a dropped index be resurrected.
 *
 * Exposed for testing only.
 */
StringSet relevantFieldsForQuery(const CanonicalQuery& cq);

/**
 * For each node of 'graph', in ascending NodeId order, the field paths on its base collection for
 * which an index could plausibly change the plan chosen for that node. Per node this is the union
 * of:
 *  - every field the node's access path query references, per 'relevantFieldsForQuery', and
 *  - the join predicate field paths the node owns, since those are the fields an INLJ probe index
 * would be built on.
 *
 * Computed for the whole graph at once so that the join predicates are visited once in total rather
 * than once per node.
 *
 * Exposed for testing only.
 */
std::vector<StringSet> relevantFieldsPerNode(const JoinGraph& graph,
                                             const std::vector<ResolvedPath>& resolvedPaths);

/**
 * Hashes those of 'inljEligibleIndexes' that are relevant to 'relevantFields', i.e. those with a
 * key pattern field -- in any position, not just the leading one -- that is one of those fields.
 *
 * Returns one hash per relevant index. The result set is sorted since the index catalog enumerates
 * its entries in a non-deterministic order.
 *
 * Exposed for testing only.
 */
std::vector<IndexFingerprint> computeRelevantIndexHashes(
    const std::vector<std::shared_ptr<const IndexCatalogEntry>>& inljEligibleIndexes,
    const StringSet& relevantFields);

/**
 * Hashes those of 'inljEligibleIndexes' whose name is in 'usedIndexNames', which are the indexes a
 * cached plan uses on one node.
 *
 * Exposed for testing only.
 */
IndexFingerprint computeUsedIndexFingerprint(
    const std::vector<std::shared_ptr<const IndexCatalogEntry>>& inljEligibleIndexes,
    const StringSet& usedIndexNames);

}  // namespace mongo::join_ordering
