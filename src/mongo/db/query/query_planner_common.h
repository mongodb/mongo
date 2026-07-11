// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/simple_bsonelement_comparator.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/compiler/physical_model/query_solution/query_solution.h"
#include "mongo/db/query/find_command.h"
#include "mongo/util/modules.h"

#include <cstddef>

#include <boost/optional/optional.hpp>

namespace mongo {

/**
 * Methods used by several parts of the planning process.
 */
class [[MONGO_MOD_NEEDS_REPLACEMENT]] QueryPlannerCommon {
public:
    /**
     * Does the tree rooted at 'root' have a node with matchType 'type'?
     *
     * If 'out' is not NULL, sets 'out' to the first node of type 'type' encountered.
     */
    static bool hasNode(const MatchExpression* root,
                        MatchExpression::MatchType type,
                        const MatchExpression** out = nullptr) {
        if (type == root->matchType()) {
            if (nullptr != out) {
                *out = root;
            }
            return true;
        }

        for (size_t i = 0; i < root->numChildren(); ++i) {
            if (hasNode(root->getChild(i), type, out)) {
                return true;
            }
        }
        return false;
    }

    /**
     * Returns a count of 'type' nodes in expression tree.
     */
    static size_t countNodes(const MatchExpression* root, MatchExpression::MatchType type) {
        size_t sum = 0;
        if (type == root->matchType()) {
            sum = 1;
        }
        for (size_t i = 0; i < root->numChildren(); ++i) {
            sum += countNodes(root->getChild(i), type);
        }
        return sum;
    }

    /**
     * Assumes the provided BSONObj is of the form {field1: -+1, ..., field2: -+1}
     * Returns a BSONObj with the values negated.
     */
    static BSONObj reverseSortObj(const BSONObj& sortObj) {
        BSONObjBuilder reverseBob;
        BSONObjIterator it(sortObj);
        while (it.more()) {
            BSONElement elt = it.next();
            reverseBob.append(elt.fieldName(), elt.numberInt() * -1);
        }
        return reverseBob.obj();
    }

    /**
     * Traverses the tree rooted at 'node'. Tests scan directions recursively to see if they are
     * equal to the given direction argument. Returns true if they are and false otherwise.
     */
    static bool scanDirectionsEqual(QuerySolutionNode* node, int direction);

    /**
     * Traverses the tree rooted at 'node'.  For every STAGE_IXSCAN encountered, reverse
     * the scan direction and index bounds, unless reverseCollScans equals true, in which case
     * STAGE_COLLSCAN is reversed as well.
     */
    static void reverseScans(QuerySolutionNode* node, bool reverseCollScans = false);

    static bool providesSort(const CanonicalQuery& query, const BSONObj& kp) {
        return query.getFindCommandRequest().getSort().isPrefixOf(
            kp, SimpleBSONElementComparator::kInstance);
    }

    /**
     * Returns true if 'query' has a limit that is positive, i.e. one that can exempt the query
     * from an unbounded-scan restriction such as maxEstimatedScanBytes or notablescan.
     */
    static bool hasEffectiveLimit(const CanonicalQuery& query) {
        return query.getFindCommandRequest().getLimit().value_or(0) > 0;
    }

    static bool providesSortRequirementForDistinct(
        const boost::optional<CanonicalDistinct>& distinct, const BSONObj& kp) {
        return distinct && distinct->getSortRequirement() &&
            distinct->getSerializedSortRequirement().isPrefixOf(
                kp, SimpleBSONElementComparator::kInstance);
    }

    /**
     * Determine whether this query has a sort that can be provided by the collection's clustering
     * index, if so, which direction the scan should be. If the collection is not clustered, or the
     * sort cannot be provided, returns 'boost::none'.
     */
    static boost::optional<int> determineClusteredScanDirection(
        const CanonicalQuery& query,
        const boost::optional<ClusteredCollectionInfo>& clusteredInfo,
        const CollatorInterface* clusteredCollectionCollator);
};

}  // namespace mongo
