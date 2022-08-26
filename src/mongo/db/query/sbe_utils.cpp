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


#include "mongo/db/query/sbe_utils.h"

#include "mongo/db/query/query_planner_params.h"

namespace mongo::sbe {
bool isQuerySbeCompatible(const CollectionPtr* collection,
                          const CanonicalQuery* cq,
                          size_t plannerOptions) {
    tassert(6071400, "Expected CanonicalQuery pointer to not be nullptr", cq);
    invariant(cq);
    auto expCtx = cq->getExpCtxRaw();
    const auto& sortPattern = cq->getSortPattern();
    const bool allExpressionsSupported = expCtx && expCtx->sbeCompatible;
    const bool isNotCount = !(plannerOptions & QueryPlannerParams::IS_COUNT);
    const bool isNotOplog = !cq->nss().isOplog();
    const bool isNotChangeCollection = !cq->nss().isChangeCollection();
    const bool doesNotContainMetadataRequirements = cq->metadataDeps().none();
    const bool doesNotSortOnMetaOrPathWithNumericComponents =
        !sortPattern || std::all_of(sortPattern->begin(), sortPattern->end(), [](auto&& part) {
            return part.fieldPath &&
                !FieldRef(part.fieldPath->fullPath()).hasNumericPathComponents();
        });

    // Queries against a time-series collection are not currently supported by SBE.
    const bool isQueryNotAgainstTimeseriesCollection = !(cq->nss().isTimeseriesBucketsCollection());

    // Queries against a clustered collection are not currently supported by SBE.
    tassert(6038600, "Expected CollectionPtr to not be nullptr", collection);
    const bool isQueryNotAgainstClusteredCollection =
        !(collection->get() && collection->get()->isClustered());

    const bool doesNotRequireMatchDetails =
        !cq->getProj() || !cq->getProj()->requiresMatchDetails();

    const bool doesNotHaveElemMatchProject = !cq->getProj() || !cq->getProj()->containsElemMatch();

    const bool isNotInnerSideOfLookup = !(expCtx && expCtx->inLookup);

    return allExpressionsSupported && isNotCount && doesNotContainMetadataRequirements &&
        isQueryNotAgainstTimeseriesCollection && isQueryNotAgainstClusteredCollection &&
        doesNotSortOnMetaOrPathWithNumericComponents && isNotOplog && doesNotRequireMatchDetails &&
        doesNotHaveElemMatchProject && isNotChangeCollection && isNotInnerSideOfLookup;
}
}  // namespace mongo::sbe
