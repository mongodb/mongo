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


#include "mongo/db/query/query_utils.h"

#include "mongo/db/exec/sbe/match_path.h"

namespace mongo {

bool sortPatternHasPartsWithCommonPrefix(const SortPattern& sortPattern) {
    StringDataSet prefixSet;
    for (const auto& part : sortPattern) {
        // Ignore any $meta sorts that may be present.
        if (!part.fieldPath) {
            continue;
        }
        auto [_, inserted] = prefixSet.insert(part.fieldPath->getFieldName(0));
        if (!inserted) {
            return true;
        }
    }
    return false;
}

bool isIdHackEligibleQuery(const CollectionPtr& collection, const CanonicalQuery& query) {
    const auto& findCommand = query.getFindCommandRequest();
    return !findCommand.getShowRecordId() && findCommand.getHint().isEmpty() &&
        findCommand.getMin().isEmpty() && findCommand.getMax().isEmpty() &&
        !findCommand.getSkip() && CanonicalQuery::isSimpleIdQuery(findCommand.getFilter()) &&
        !findCommand.getTailable() &&
        CollatorInterface::collatorsMatch(query.getCollator(), collection->getDefaultCollator());
}

bool isQuerySbeCompatible(const CollectionPtr* collection, const CanonicalQuery* cq) {
    tassert(6071400, "Expected CanonicalQuery pointer to not be nullptr", cq);
    invariant(cq);
    auto expCtx = cq->getExpCtxRaw();
    const auto& sortPattern = cq->getSortPattern();
    const bool allExpressionsSupported =
        expCtx && expCtx->sbeCompatibility != SbeCompatibility::notCompatible;
    const auto nss = cq->nss();
    const bool isNotOplog = !nss.isOplog();
    const bool isNotChangeCollection = !nss.isChangeCollection();
    const bool doesNotContainMetadataRequirements = cq->metadataDeps().none();
    const bool doesNotSortOnMetaOrPathWithNumericComponents =
        !sortPattern || std::all_of(sortPattern->begin(), sortPattern->end(), [](auto&& part) {
            return part.fieldPath &&
                !sbe::MatchPath(part.fieldPath->fullPath()).hasNumericPathComponents();
        });

    // Queries against a time-series collection are not currently supported by SBE.
    const bool isQueryNotAgainstTimeseriesCollection = !nss.isTimeseriesBucketsCollection();

    // Queries against a clustered collection are not currently supported by SBE.
    tassert(6038600, "Expected CollectionPtr to not be nullptr", collection);
    const bool isQueryNotAgainstClusteredCollection =
        !(collection->get() && collection->get()->isClustered());

    const auto* proj = cq->getProj();

    const bool doesNotRequireMatchDetails = !proj || !proj->requiresMatchDetails();

    const bool doesNotHaveElemMatchProject = !proj || !proj->containsElemMatch();

    const bool isNotInnerSideOfLookup = !(expCtx && expCtx->inLookup);

    return allExpressionsSupported && doesNotContainMetadataRequirements &&
        isQueryNotAgainstTimeseriesCollection && isQueryNotAgainstClusteredCollection &&
        doesNotSortOnMetaOrPathWithNumericComponents && isNotOplog && doesNotRequireMatchDetails &&
        doesNotHaveElemMatchProject && isNotChangeCollection && isNotInnerSideOfLookup &&
        !(*collection && isIdHackEligibleQuery(*collection, *cq));
}

bool isQueryPlanSbeCompatible(const QuerySolution* root) {
    tassert(7061701, "Expected QuerySolution pointer to not be nullptr", root);

    // TODO SERVER-52958: Add support in the SBE stage builders for the COUNT_SCAN stage.
    const bool isNotCountScan = !root->hasNode(StageType::STAGE_COUNT_SCAN);

    return isNotCountScan;
}

}  // namespace mongo
