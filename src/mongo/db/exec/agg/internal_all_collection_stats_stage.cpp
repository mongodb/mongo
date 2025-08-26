/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/db/exec/agg/internal_all_collection_stats_stage.h"

#include "mongo/bson/bsontypes.h"
#include "mongo/db/exec/agg/coll_stats_stage.h"
#include "mongo/db/exec/agg/document_source_to_stage_registry.h"
#include "mongo/db/exec/matcher/matcher.h"
#include "mongo/db/pipeline/document_source_internal_all_collection_stats.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/util/assert_util.h"

#include <utility>

#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

boost::intrusive_ptr<exec::agg::Stage> documentSourceInternalAllCollectionStatsToStageFn(
    const boost::intrusive_ptr<DocumentSource>& documentSource) {
    auto* internalAllCollectionStatsDS =
        dynamic_cast<DocumentSourceInternalAllCollectionStats*>(documentSource.get());

    tassert(10812302,
            "expected 'DocumentSourceInternalAllCollectionStats' type",
            internalAllCollectionStatsDS);

    return make_intrusive<exec::agg::InternalAllCollectionStatsStage>(
        internalAllCollectionStatsDS->kStageNameInternal,
        internalAllCollectionStatsDS->getExpCtx(),
        internalAllCollectionStatsDS->_internalAllCollectionStatsSpec,
        internalAllCollectionStatsDS->_absorbedMatch,
        internalAllCollectionStatsDS->_projectFilter
            ? boost::make_optional(internalAllCollectionStatsDS->_projectFilter->getOwned())
            : boost::none);
}

namespace exec::agg {

REGISTER_AGG_STAGE_MAPPING(_internalAllCollectionStats,
                           DocumentSourceInternalAllCollectionStats::id,
                           documentSourceInternalAllCollectionStatsToStageFn)

InternalAllCollectionStatsStage::InternalAllCollectionStatsStage(
    StringData stageName,
    const boost::intrusive_ptr<ExpressionContext>& pExpCtx,
    DocumentSourceInternalAllCollectionStatsSpec internalAllCollectionStatsSpec,
    const boost::intrusive_ptr<DocumentSourceMatch>& absorbedMatch,
    boost::optional<BSONObj> projectFilter)
    : Stage(stageName, pExpCtx),
      _internalAllCollectionStatsSpec(std::move(internalAllCollectionStatsSpec)),
      _absorbedMatch(absorbedMatch),
      _projectFilter(std::move(projectFilter)) {}

GetNextResult InternalAllCollectionStatsStage::doGetNext() {
    if (!_catalogDocs) {
        _catalogDocs =
            pExpCtx->getMongoProcessInterface()->listCatalog(pExpCtx->getOperationContext());
    }

    while (!_catalogDocs->empty()) {
        BSONObj obj(std::move(_catalogDocs->front()));
        const auto nss = NamespaceStringUtil::parseFromStringExpectTenantIdInMultitenancyMode(
            obj["ns"].String());

        _catalogDocs->pop_front();

        // Avoid computing stats for collections that do not match the absorbed filter on the 'ns'
        // field.
        if (_absorbedMatch &&
            !exec::matcher::matchesBSON(_absorbedMatch->getMatchExpression(), obj)) {
            continue;
        }

        if (const auto& stats = _internalAllCollectionStatsSpec.getStats()) {
            try {
                return {Document{
                    CollStatsStage::makeStatsForNs(pExpCtx, nss, stats.get(), _projectFilter)}};
            } catch (const ExceptionFor<ErrorCodes::CommandNotSupportedOnView>&) {
                // We don't want to retrieve data for views, only for collections.
                continue;
            } catch (const ExceptionFor<ErrorCodes::NamespaceNotFound>&) {
                // The collection no longer exists.
                continue;
            }
        }
    }

    return GetNextResult::makeEOF();
}

}  // namespace exec::agg
}  // namespace mongo
