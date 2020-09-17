/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#include "mongo/platform/basic.h"

#include "mongo/db/exec/trial_period_utils.h"

#include "mongo/db/catalog/collection.h"

namespace mongo::trial_period {
size_t getTrialPeriodMaxWorks(OperationContext* opCtx, const CollectionPtr& collection) {
    // Run each plan some number of times. This number is at least as great as
    // 'internalQueryPlanEvaluationWorks', but may be larger for big collections.
    size_t numWorks = internalQueryPlanEvaluationWorks.load();
    if (collection) {
        // For large collections, the number of works is set to be this fraction of the collection
        // size.
        double fraction = internalQueryPlanEvaluationCollFraction;

        numWorks = std::max(static_cast<size_t>(internalQueryPlanEvaluationWorks.load()),
                            static_cast<size_t>(fraction * collection->numRecords(opCtx)));
    }

    return numWorks;
}

size_t getTrialPeriodNumToReturn(const CanonicalQuery& query) {
    // Determine the number of results which we will produce during the plan ranking phase before
    // stopping.
    size_t numResults = static_cast<size_t>(internalQueryPlanEvaluationMaxResults.load());
    if (query.getQueryRequest().getNToReturn()) {
        numResults =
            std::min(static_cast<size_t>(*query.getQueryRequest().getNToReturn()), numResults);
    } else if (query.getQueryRequest().getLimit()) {
        numResults = std::min(static_cast<size_t>(*query.getQueryRequest().getLimit()), numResults);
    }

    return numResults;
}
}  // namespace mongo::trial_period
