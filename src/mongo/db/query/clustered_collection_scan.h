// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonelement.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/query/compiler/physical_model/query_solution/query_solution.h"
#include "mongo/db/query/query_planner_params.h"
#include "mongo/db/query/record_id_range.h"

#include <functional>
#include <memory>
#include <set>
#include <string_view>

namespace mongo {

/**
 * Helpers for clustered collection scans, plus the legacy planner that produces a contiguous range.
 */
class ClusteredCollectionScanPlanner {
public:
    /**
     * Returns true if the element type is affected by a collator (i.e. it is or contains
     * a String). Shared by both the legacy and multi-range clustered scan strategies.
     */
    static bool affectedByCollator(const BSONElement& element);

    /**
     * Returns true if the element is not affected by collators, or if the query and
     * collection collators are compatible. Shared by both strategies.
     */
    static bool compatibleCollator(const CollatorInterface* collCollator,
                                   const CollatorInterface* queryCollator,
                                   const BSONElement& element);

    /**
     * Pre-multi-range bound extraction for clustered collection scans. Collapses the
     * MatchExpression into a single contiguous [min, max] RecordIdRange. This is kept here as a
     * fallback in case the featureFlagClusteredCollScanMultiRange is disabled.
     *
     * Helper method to add an RID range to collection scans. If the query solution tree contains a
     * collection scan node with a suitable comparison predicate on '_id', we add a minRecord and
     * maxRecord on the collection node.
     *
     * Returns true if the MatchExpression is a comparison against the cluster key which either:
     * 1) is guaranteed to exclude values of the cluster key which are affected by collation or
     * 2) may return values of the cluster key which are affected by collation, but the query and
     *    collection collations match.
     * Otherwise, returns false.
     *
     * For example, assuming the cluster key is "_id":
     * Given {a: {$eq: 2}}, we return false, because the comparison is not against the cluster key.
     * Given {_id: {$gte: 5}}, we return true, because this comparison against the cluster key
     *    excludes keys which are affected by collations.
     * Given {_id: {$eq: "str"}}, we return true only if the query and collection collations match.
     *
     * Arguments
     *   (in) conjunct - current query's match expression (or subexpression in recursive calls)
     *   (in) queryCollator - current query's collator
     *   (in) ccCollator - clustered collection's collator
     *   (in) clusterKeyFieldName - only "_id" is officially supported, but this may change someday
     *   (out) recordRange - scan start/end bounds
     *   (out) redundant - if provided, will be called with pointers to expressions which
     *                     do not require a filter, _if_ the collection scan can enforce
     *                     recordRange
     */
    [[nodiscard]] static bool handleRIDRangeScanLegacy(
        const MatchExpression* conjunct,
        const CollatorInterface* queryCollator,
        const CollatorInterface* ccCollator,
        std::string_view clusterKeyFieldName,
        RecordIdRange& recordRange,
        const std::function<void(const MatchExpression*)>& redundant);

    /**
     * Removes from a MatchExpression tree any sub-expressions that are in 'toRemove'.
     * Only descends into $and (not $or), matching the legacy bound-extraction behavior.
     */
    static void simplifyFilterLegacy(std::unique_ptr<MatchExpression>& expr,
                                     const std::set<const MatchExpression*>& toRemove);

    /**
     * Populates 'csn->rangeList' with a single contiguous RecordIdRange derived from the
     * query's filter and explicit min()/max() cursor args, updates 'csn->hasCompatibleCollation',
     * and simplifies 'csn->filter' when 'canSimplifyFilter' is true.
     *
     * This is the flag-off counterpart to the multi-range path in
     * QueryPlannerAccess::makeCollectionScan. It must be called only when
     * 'csn->isClustered && !csn->resumeScanPoint'.
     */
    static void buildLegacyBounds(CollectionScanNode* csn,
                                  const CanonicalQuery& query,
                                  const QueryPlannerParams& params,
                                  const CollatorInterface* queryCollator,
                                  const CollatorInterface* collCollator,
                                  bool canSimplifyFilter);
};

}  // namespace mongo
