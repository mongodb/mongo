// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/agg/plan_cache_stats_stage.h"

#include "mongo/db/exec/agg/document_source_to_stage_registry.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_plan_cache_stats.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/intrusive_counter.h"

#include <string_view>

namespace mongo {

boost::intrusive_ptr<exec::agg::Stage> documentSourcePlanCacheStatsToStageFn(
    const boost::intrusive_ptr<DocumentSource>& documentSource) {
    auto* planCacheStatsDS = dynamic_cast<DocumentSourcePlanCacheStats*>(documentSource.get());

    tassert(10970600, "expected 'DocumentSourcePlanCacheStats' type", planCacheStatsDS);

    return make_intrusive<exec::agg::PlanCacheStatsStage>(planCacheStatsDS->kStageName,
                                                          planCacheStatsDS->getExpCtx(),
                                                          planCacheStatsDS->_absorbedMatch);
}

REGISTER_AGG_STAGE_MAPPING(planCacheStats,
                           DocumentSourcePlanCacheStats::id,
                           documentSourcePlanCacheStatsToStageFn);

namespace exec::agg {

PlanCacheStatsStage::PlanCacheStatsStage(
    std::string_view stageName,
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const boost::intrusive_ptr<DocumentSourceMatch>& absorbedMatch)
    : Stage(stageName, expCtx), _absorbedMatch(absorbedMatch) {}

GetNextResult PlanCacheStatsStage::doGetNext() {
    if (!_haveRetrievedStats) {
        const auto matchExpr = _absorbedMatch ? _absorbedMatch->getMatchExpression() : nullptr;
        _results = pExpCtx->getMongoProcessInterface()->getMatchingPlanCacheEntryStats(
            pExpCtx->getOperationContext(), pExpCtx->getNamespaceString(), matchExpr);

        _resultsIter = _results.begin();
        _haveRetrievedStats = true;
    }

    if (_resultsIter == _results.end()) {
        return GetNextResult::makeEOF();
    }

    MutableDocument nextPlanCacheEntry{Document{*_resultsIter++}};

    // Augment each plan cache entry with this node's host and port string.
    if (_hostAndPort.empty()) {
        _hostAndPort =
            pExpCtx->getMongoProcessInterface()->getHostAndPort(pExpCtx->getOperationContext());
        uassert(31386,
                "Aggregation request specified 'fromRouter' but unable to retrieve host name "
                "for $planCacheStats pipeline stage.",
                !_hostAndPort.empty());
    }
    nextPlanCacheEntry.setField("host", Value{_hostAndPort});

    // If we're returning results to mongos, then additionally augment each plan cache entry with
    // the shard name, for the node from which we're collecting plan cache information.
    if (pExpCtx->getFromRouter()) {
        if (_shardName.empty()) {
            _shardName =
                pExpCtx->getMongoProcessInterface()->getShardName(pExpCtx->getOperationContext());
            uassert(31385,
                    "Aggregation request specified 'fromRouter' but unable to retrieve shard name "
                    "for $planCacheStats pipeline stage.",
                    !_shardName.empty());
        }
        nextPlanCacheEntry.setField("shard", Value{_shardName});
    }

    return nextPlanCacheEntry.freeze();
}

}  // namespace exec::agg
}  // namespace mongo
