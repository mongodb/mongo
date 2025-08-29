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

#include "mongo/db/exec/agg/plan_cache_stats_stage.h"

#include "mongo/base/string_data.h"
#include "mongo/db/exec/agg/document_source_to_stage_registry.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_plan_cache_stats.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/intrusive_counter.h"

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
    StringData stageName,
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
