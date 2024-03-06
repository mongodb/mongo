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

#include <algorithm>
#include <vector>

#include <boost/optional/optional.hpp>

#include "mongo/bson/bsonobj.h"
#include "mongo/db/basic_types.h"
#include "mongo/db/exec/document_value/document_metadata_fields.h"
#include "mongo/db/exec/sbe/match_path.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/field_path.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/query/find_command.h"
#include "mongo/db/query/projection.h"
#include "mongo/db/query/query_decorations.h"
#include "mongo/db/query/query_feature_flags_gen.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/string_map.h"

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

bool isSortSbeCompatible(const SortPattern& sortPattern) {
    // If the sort has meta or numeric path components, we cannot use SBE.
    return std::all_of(sortPattern.begin(), sortPattern.end(), [](auto&& part) {
        return part.fieldPath &&
            !sbe::MatchPath(part.fieldPath->fullPath()).hasNumericPathComponents();
    });
}

bool isQuerySbeCompatible(const CollectionPtr* collection, const CanonicalQuery* cq) {
    tassert(6071400,
            "Expected CanonicalQuery and Collection pointer to not be nullptr",
            cq && collection);
    auto expCtx = cq->getExpCtxRaw();

    // If we don't support all expressions used or the query is eligible for IDHack, don't use SBE.
    if (!expCtx || expCtx->sbeCompatibility == SbeCompatibility::notCompatible ||
        expCtx->sbePipelineCompatibility == SbeCompatibility::notCompatible ||
        (*collection &&
         isIdHackEligibleQuery(*collection, cq->getFindCommandRequest(), cq->getCollator()))) {
        return false;
    }

    const auto* proj = cq->getProj();
    if (proj && (proj->requiresMatchDetails() || proj->containsElemMatch())) {
        return false;
    }

    const auto& nss = cq->nss();

    auto& queryKnob = QueryKnobConfiguration::decoration(cq->getExpCtxRaw()->opCtx);
    if ((!feature_flags::gFeatureFlagTimeSeriesInSbe.isEnabled(
             serverGlobalParams.featureCompatibility.acquireFCVSnapshot()) ||
         queryKnob.getSbeDisableTimeSeriesForOp()) &&
        nss.isTimeseriesBucketsCollection()) {
        return false;
    }

    // Queries against the oplog or a change collection are not supported. Also queries on the inner
    // side of a $lookup are not considered for SBE except search queries.
    if ((expCtx->inLookup && !cq->isSearchQuery()) || nss.isOplog() || nss.isChangeCollection() ||
        !cq->metadataDeps().none()) {
        return false;
    }

    const auto& sortPattern = cq->getSortPattern();
    return !sortPattern || isSortSbeCompatible(*sortPattern);
}

}  // namespace mongo
