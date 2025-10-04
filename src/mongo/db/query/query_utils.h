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

#include "mongo/db/local_catalog/clustered_collection_util.h"
#include "mongo/db/local_catalog/collection.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/query/compiler/logical_model/sort_pattern/sort_pattern.h"
#include "mongo/db/query/indexability.h"
#include "mongo/db/query/query_knobs_gen.h"

namespace mongo {
/**
 * Returns 'true' if 'sortPattern' contains any sort pattern parts that share a common prefix, false
 * otherwise.
 */
bool sortPatternHasPartsWithCommonPrefix(const SortPattern& sortPattern);

/**
 * Returns 'true' if the given match expression is of the shape {_id: {$eq: ...}}.
 */
bool isMatchIdHackEligible(MatchExpression* me);

/**
 * Returns true if 'query' describes an exact-match query on _id.
 */
bool isSimpleIdQuery(const BSONObj& query);

/**
 * Returns 'true' if 'query' on the given 'collection' can be answered using a special IDHACK plan,
 * without taking into account the collators.
 */
inline bool isIdHackEligibleQueryWithoutCollator(const FindCommandRequest& findCommand,
                                                 MatchExpression* me = nullptr) {
    return !findCommand.getShowRecordId() && findCommand.getHint().isEmpty() &&
        findCommand.getMin().isEmpty() && findCommand.getMax().isEmpty() &&
        !findCommand.getSkip() &&
        (isSimpleIdQuery(findCommand.getFilter()) || isMatchIdHackEligible(me)) &&
        !findCommand.getTailable();
}

/**
 * Returns 'true' if 'query' on the given 'collection' can be answered using a special IDHACK plan.
 * TODO: remove this method in favor of ExpCtx::isIdHackQuery() checks.
 */
inline bool isIdHackEligibleQuery(const CollectionPtr& collection, const CanonicalQuery& cq) {
    return isIdHackEligibleQueryWithoutCollator(cq.getFindCommandRequest(),
                                                cq.getPrimaryMatchExpression()) &&
        CollatorInterface::collatorsMatch(cq.getCollator(), collection->getDefaultCollator());
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

    const bool isProjectionEligible = cq.getProj() == nullptr || cq.getProj()->isSimple();

    return
        // Properties of the find command.
        isProjectionEligible && !findCommand.getShowRecordId() && findCommand.getHint().isEmpty() &&
        findCommand.getMin().isEmpty() && findCommand.getMax().isEmpty() &&
        findCommand.getSort().isEmpty() && !findCommand.getSkip() && !findCommand.getTailable() &&
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

    // If a query needs metadata, it is ineligible for express path.
    if (cq.metadataDeps().any()) {
        return ExpressEligibility::Ineligible;
    }

    const auto& findCommandReq = cq.getFindCommandRequest();

    if (!coll || findCommandReq.getReturnKey() || findCommandReq.getBatchSize() ||
        (cq.getProj() != nullptr && !cq.getProj()->isSimple())) {
        return ExpressEligibility::Ineligible;
    }
    if (isIdHackEligibleQuery(coll, cq) &&
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
}  // namespace mongo
