/**
 *    Copyright (C) 2021-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include "mongo/db/catalog/clustered_collection_util.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/query/indexability.h"
#include "mongo/db/query/query_knobs_gen.h"
#include "mongo/db/query/sort_pattern.h"

namespace mongo {
/**
 * Returns 'true' if 'sortPattern' contains any sort pattern parts that share a common prefix, false
 * otherwise.
 */
bool sortPatternHasPartsWithCommonPrefix(const SortPattern& sortPattern);

/**
 * Returns 'true' if 'query' on the given 'collection' can be answered using a special IDHACK plan,
 * without taking into account the collators.
 */
inline bool isIdHackEligibleQueryWithoutCollator(const FindCommandRequest& findCommand) {
    return !findCommand.getShowRecordId() && findCommand.getHint().isEmpty() &&
        findCommand.getMin().isEmpty() && findCommand.getMax().isEmpty() &&
        !findCommand.getSkip() && CanonicalQuery::isSimpleIdQuery(findCommand.getFilter()) &&
        !findCommand.getTailable();
}

/**
 * Returns 'true' if 'query' on the given 'collection' can be answered using a special IDHACK plan.
 */
inline bool isIdHackEligibleQuery(const CollectionPtr& collection,
                                  const FindCommandRequest& findCommand,
                                  const CollatorInterface* queryCollator) {

    return isIdHackEligibleQueryWithoutCollator(findCommand) &&
        CollatorInterface::collatorsMatch(queryCollator, collection->getDefaultCollator());
}

/**
 * Returns 'true' if 'query' on the given 'collection' can be answered using a special IXSCAN +
 * FETCH plan. Among other restrictions, the query must be a single-field equality generating exact
 * bounds.
 */
inline bool isEqualityExpressEligibleQuery(const CollectionPtr& collection,
                                           const CanonicalQuery& cq) {
    const auto& findCommand = cq.getFindCommandRequest();
    auto me = cq.getPrimaryMatchExpression();

    if (internalQueryDisableSingleFieldExpressExecutor.load()) {
        return false;
    }

    return
        // Properties of the find command.
        !findCommand.getShowRecordId() && findCommand.getHint().isEmpty() &&
        findCommand.getMin().isEmpty() && findCommand.getMax().isEmpty() &&
        findCommand.getProjection().isEmpty() && findCommand.getSort().isEmpty() &&
        !findCommand.getSkip() && !findCommand.getTailable() &&
        // Properties of the query's match expression.
        me->matchType() == MatchExpression::EQ &&
        Indexability::isExactBoundsGenerating(
            static_cast<ComparisonMatchExpressionBase*>(me)->getData());
}

/**
 * Describes whether or not a query is eligible for the express executor.
 */
enum ExpressEligibility {
    // For an ineligible query that should go through regular query optimization and execution.
    Ineligible = 0,
    // For a point query that can fulfilled via a single lookup into the _id index or with a direct
    // lookup into a clustered collection.
    IdPointQueryEligible,
    // For an equality query that *may* use the express executor if a suitable index is found.
    IndexedEqualityEligible,
};
inline ExpressEligibility isExpressEligible(OperationContext* opCtx,
                                            const CollectionPtr& coll,
                                            const CanonicalQuery& cq) {
    // Not eligible to use the express path if a particular query framework is set.
    if (auto queryFramework = cq.getExpCtx()->getQuerySettings().getQueryFramework()) {
        return ExpressEligibility::Ineligible;
    }

    const auto& findCommandReq = cq.getFindCommandRequest();

    if (!coll || findCommandReq.getReturnKey() || findCommandReq.getBatchSize() ||
        (cq.getProj() != nullptr && !cq.getProj()->isSimple())) {
        return ExpressEligibility::Ineligible;
    }

    if (isIdHackEligibleQuery(coll, findCommandReq, cq.getExpCtx()->getCollator()) &&
        (coll->getIndexCatalog()->haveIdIndex(opCtx) ||
         clustered_util::isClusteredOnId(coll->getClusteredInfo()))) {
        return ExpressEligibility::IdPointQueryEligible;
    }

    if (isEqualityExpressEligibleQuery(coll, cq) && coll->getIndexCatalog()->haveAnyIndexes() &&
        !coll->getClusteredInfo()) {
        return ExpressEligibility::IndexedEqualityEligible;
    }

    return ExpressEligibility::Ineligible;
}

bool isSortSbeCompatible(const SortPattern& sortPattern);

/**
 * Checks if the given query can be executed with the SBE engine based on the canonical query.
 *
 * This method determines whether the query may be compatible with SBE based only on high-level
 * information from the canonical query, before query planning has taken place (such as ineligible
 * expressions or collections).
 *
 * If this method returns true, query planning should be done, followed by another layer of
 * validation to make sure the query plan can be executed with SBE. If it returns false, SBE query
 * planning can be short-circuited as it is already known that the query is ineligible for SBE.
 */
bool isQuerySbeCompatible(const CollectionPtr* collection, const CanonicalQuery* cq);
}  // namespace mongo
