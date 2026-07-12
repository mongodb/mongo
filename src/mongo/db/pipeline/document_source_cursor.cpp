// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/pipeline/document_source_cursor.h"

#include "mongo/bson/bsontypes.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/query/collection_index_usage_tracker_decoration.h"
#include "mongo/db/query/explain.h"
#include "mongo/db/query/explain_options.h"
#include "mongo/db/query/explain_policy.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/logv2/log.h"
#include "mongo/util/serialization_context.h"

#include <string_view>

#include <boost/smart_ptr/intrusive_ptr.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo {

ALLOCATE_DOCUMENT_SOURCE_ID(cursor, DocumentSourceCursor::id);

using boost::intrusive_ptr;
using std::string;

std::string_view DocumentSourceCursor::getSourceName() const {
    return kStageName;
}

Value DocumentSourceCursor::serialize(const query_shape::SerializationOptions& opts) const {
    // We never parse a DocumentSourceCursor, so we only serialize for explain. Since it's never
    // part of user input, there's no need to compute its query shape.
    if (!opts.isSerializingForExplain() || opts.isSerializingForQueryStats()) {
        return Value();
    }

    tassert(10422502, "PlanExecutor is null", _sharedState->exec);

    // TODO SERVER-130810 For a V3 explain, Explain::explainPipeline() serializes the pipeline at
    // the legacy verbosity that V3 currently reuses, while the ExpressionContext holds the
    // originally requested V3 verbosity, so the two intentionally differ. Skip the exact
    // cross-check for V3 requests; it should be restored once the V3 pipeline output format is
    // implemented.
    const auto ctxVerbosity = getExpCtx()->getExplain();
    uassert(50660,
            "Mismatch between verbosity passed to serialize() and expression context verbosity",
            (ctxVerbosity && ExplainOptions::isV3Verbosity(*ctxVerbosity)) ||
                opts.verbosity == ctxVerbosity);

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
    tassert(11294806, "Missing queryPlanner field in explain stats", explainStats["queryPlanner"]);
    out["queryPlanner"] = Value(explainStats["queryPlanner"]);

    if (explainPolicyFor(opts.verbosity.value()).hasExecStats()) {
        tassert(11294805,
                "Missing executionStats field in explain stats",
                explainStats["executionStats"]);
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
    std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> exec,
    const intrusive_ptr<ExpressionContext>& pCtx,
    CursorType cursorType,
    ResumeTrackingType resumeTrackingType)
    : DocumentSource(kStageName, pCtx),
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
}

void DocumentSourceCursor::bindCatalogInfo(
    const MultipleCollectionAccessor& collections,
    boost::intrusive_ptr<ShardRoleTransactionResourcesStasherForPipeline> stasher) {
    _catalogResourceHandle = make_intrusive<DSCursorCatalogResourceHandle>(stasher);

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
    for (const auto& [nss, acq] : collections.getSecondaryCollectionAcquisitions()) {
        const auto& coll = acq.getCollectionPtr();
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
    std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> exec,
    const intrusive_ptr<ExpressionContext>& pExpCtx,
    CursorType cursorType,
    ResumeTrackingType resumeTrackingType) {
    return make_intrusive<DocumentSourceCursor>(
        std::move(exec), pExpCtx, cursorType, resumeTrackingType);
}
}  // namespace mongo
