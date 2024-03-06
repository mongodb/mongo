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

inline bool isExpressEligible(OperationContext* opCtx,
                              const CollectionPtr& coll,
                              const CanonicalQuery& cq) {
    const auto& findCommandReq = cq.getFindCommandRequest();
    return (coll && (cq.getProj() == nullptr || cq.getProj()->isSimple()) &&
            isIdHackEligibleQuery(coll, findCommandReq, cq.getExpCtx()->getCollator()) &&
            !findCommandReq.getReturnKey() && !findCommandReq.getBatchSize() &&
            (coll->getIndexCatalog()->haveIdIndex(opCtx) ||
             clustered_util::isClusteredOnId(coll->getClusteredInfo())));
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
