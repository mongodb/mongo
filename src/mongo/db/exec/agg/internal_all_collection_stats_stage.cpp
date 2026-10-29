// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/agg/internal_all_collection_stats_stage.h"

#include "mongo/bson/bsontypes.h"
#include "mongo/db/exec/agg/coll_stats_stage.h"
#include "mongo/db/exec/agg/document_source_to_stage_registry.h"
#include "mongo/db/exec/matcher/matcher.h"
#include "mongo/db/pipeline/document_source_internal_all_collection_stats.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/util/assert_util.h"

#include <string_view>
#include <utility>

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
        internalAllCollectionStatsDS->kStageName,
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
    std::string_view stageName,
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
