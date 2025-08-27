/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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


#include "mongo/db/pipeline/document_source_cursor.h"

#include "mongo/bson/bsontypes.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/local_catalog/catalog_raii.h"
#include "mongo/db/local_catalog/db_raii.h"
#include "mongo/db/pipeline/initialize_auto_get_helper.h"
#include "mongo/db/query/collection_index_usage_tracker_decoration.h"
#include "mongo/db/query/explain.h"
#include "mongo/db/query/explain_options.h"
#include "mongo/db/query/find_common.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/logv2/log.h"
#include "mongo/util/serialization_context.h"

#include <boost/move/utility_core.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo {

ALLOCATE_DOCUMENT_SOURCE_ID(cursor, DocumentSourceCursor::id);

using boost::intrusive_ptr;
using std::string;

const char* DocumentSourceCursor::getSourceName() const {
    return kStageName.data();
}

Value DocumentSourceCursor::serialize(const SerializationOptions& opts) const {
    // We never parse a DocumentSourceCursor, so we only serialize for explain. Since it's never
    // part of user input, there's no need to compute its query shape.
    if (!opts.isSerializingForExplain() || opts.isSerializingForQueryStats()) {
        return Value();
    }

    tassert(10422502, "PlanExecutor is null", _sharedState->exec);

    uassert(50660,
            "Mismatch between verbosity passed to serialize() and expression context verbosity",
            opts.verbosity == getExpCtx()->getExplain());

    MutableDocument out;

    BSONObjBuilder explainStatsBuilder;
    tassert(
        10769400, "Expected the plannerContext to be set for explain", _plannerContext.has_value());

    BSONObj extraInfo = BSON("cursorType" << toString(_cursorType));
    Explain::explainStages(
        _sharedState->exec.get(),
        *_plannerContext,
        opts.verbosity.value(),
        _sharedState->execStatus,
        _winningPlanTrialStats,
        extraInfo,
        SerializationContext::stateCommandReply(getExpCtx()->getSerializationContext()),
        BSONObj(),
        &explainStatsBuilder);

    BSONObj explainStats = explainStatsBuilder.obj();
    invariant(explainStats["queryPlanner"]);
    out["queryPlanner"] = Value(explainStats["queryPlanner"]);

    if (opts.verbosity.value() >= ExplainOptions::Verbosity::kExecStats) {
        invariant(explainStats["executionStats"]);
        out["executionStats"] = Value(explainStats["executionStats"]);
    }

    return Value(DOC(getSourceName() << out.freezeToValue()));
}

void DocumentSourceCursor::detachSourceFromOperationContext() {
    // Only detach the underlying executor if it hasn't been detached already.
    if (_sharedState->exec && _sharedState->exec->getOpCtx()) {
        _sharedState->exec->detachFromOperationContext();
    }
}

void DocumentSourceCursor::reattachSourceToOperationContext(OperationContext* opCtx) {
    if (_sharedState->exec && _sharedState->exec->getOpCtx() != opCtx) {
        _sharedState->exec->reattachToOperationContext(opCtx);
    }
}

DocumentSourceCursor::~DocumentSourceCursor() {
    if (!_sharedState->execStageCreated) {
        _sharedState->exec->dispose(getExpCtx()->getOperationContext());
    }
}

DocumentSourceCursor::DocumentSourceCursor(
    const MultipleCollectionAccessor& collections,
    std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> exec,
    const boost::intrusive_ptr<CatalogResourceHandle>& catalogResourceHandle,
    const intrusive_ptr<ExpressionContext>& pCtx,
    CursorType cursorType,
    ResumeTrackingType resumeTrackingType)
    : DocumentSource(kStageName, pCtx),
      _catalogResourceHandle(catalogResourceHandle),
      _resumeTrackingType(resumeTrackingType),
      _cursorType(cursorType),
      _queryFramework(exec->getQueryFramework()),
      _sharedState(std::make_shared<CursorSharedState>(
          CursorSharedState{.exec = std::move(exec), .execStatus = Status::OK()})) {
    // It is illegal for both 'kEmptyDocuments' to be set and resumeTrackingType to be other than
    // 'kNone'.
    uassert(ErrorCodes::InvalidOptions,
            "The resumeToken is not compatible with this query",
            cursorType != CursorType::kEmptyDocuments ||
                resumeTrackingType == ResumeTrackingType::kNone);

    tassert(10240803,
            "Expected enclosed executor to use ShardRole",
            _sharedState->exec->usesCollectionAcquisitions());

    // Later code in the DocumentSourceCursor lifecycle expects that '_exec' is in a saved state.
    _sharedState->exec->saveState();

    auto&& explainer = _sharedState->exec->getPlanExplainer();
    explainer.getSummaryStats(&_sharedState->stats.planSummaryStats);

    if (getExpCtx()->getExplain()) {
        // It's safe to access the executor even if we don't have the collection lock since we're
        // just going to call getStats() on it.
        _winningPlanTrialStats = explainer.getWinningPlanTrialStats();
        _plannerContext = Explain::makePlannerContext(*_sharedState->exec, collections);
    }

    if (collections.hasMainCollection()) {
        const auto& coll = collections.getMainCollection();
        CollectionIndexUsageTrackerDecoration::recordCollectionIndexUsage(
            coll.get(),
            _sharedState->stats.planSummaryStats.collectionScans,
            _sharedState->stats.planSummaryStats.collectionScansNonTailable,
            _sharedState->stats.planSummaryStats.indexesUsed);
    }
    for (auto& [nss, coll] : collections.getSecondaryCollections()) {
        if (coll) {
            PlanSummaryStats stats;
            explainer.getSecondarySummaryStats(nss, &stats);
            CollectionIndexUsageTrackerDecoration::recordCollectionIndexUsage(
                coll.get(),
                stats.collectionScans,
                stats.collectionScansNonTailable,
                stats.indexesUsed);
        }
    }
}

intrusive_ptr<DocumentSourceCursor> DocumentSourceCursor::create(
    const MultipleCollectionAccessor& collections,
    std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> exec,
    const boost::intrusive_ptr<CatalogResourceHandle>& catalogResourceHandle,
    const intrusive_ptr<ExpressionContext>& pExpCtx,
    CursorType cursorType,
    ResumeTrackingType resumeTrackingType) {
    intrusive_ptr<DocumentSourceCursor> source(new DocumentSourceCursor(collections,
                                                                        std::move(exec),
                                                                        catalogResourceHandle,
                                                                        pExpCtx,
                                                                        cursorType,
                                                                        resumeTrackingType));
    return source;
}
}  // namespace mongo
