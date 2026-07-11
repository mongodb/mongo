// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/db/query/find.h"

#include "mongo/db/curop.h"
#include "mongo/db/pipeline/expression_context_builder.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/client_cursor/clientcursor.h"
#include "mongo/db/query/client_cursor/collect_query_stats_mongod.h"
#include "mongo/db/query/collection_index_usage_tracker_decoration.h"
#include "mongo/db/query/explain_options.h"
#include "mongo/db/query/find_command.h"
#include "mongo/db/query/plan_explainer.h"
#include "mongo/db/query/plan_summary_stats.h"
#include "mongo/util/fail_point.h"

#include <memory>
#include <utility>

#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery


namespace mongo {


// Failpoint for checking whether we've received a getmore.
MONGO_FAIL_POINT_DEFINE(failReceivedGetmore);

bool shouldSaveCursor(OperationContext* opCtx,
                      const CollectionPtr& collection,
                      PlanExecutor* exec) {
    const FindCommandRequest& findCommand = exec->getCanonicalQuery()->getFindCommandRequest();
    if (findCommand.getSingleBatch()) {
        return false;
    }

    // We keep a tailable cursor around unless the collection we're tailing has no
    // records.
    //
    // SERVER-13955: we should be able to create a tailable cursor that waits on
    // an empty collection. Right now we do not keep a cursor if the collection
    // has zero records.
    if (findCommand.getTailable()) {
        return collection && !collection->isEmpty(opCtx);
    }

    return !exec->isEOF();
}

bool shouldSaveCursorGetMore(PlanExecutor* exec, bool isTailable) {
    return isTailable || !exec->isEOF();
}

void endQueryOp(OperationContext* opCtx,
                const CollectionPtr& collection,
                const PlanExecutor& exec,
                long long numResults,
                boost::optional<ClientCursorPin&> cursor,
                const BSONObj& cmdObj) {
    auto curOp = CurOp::get(opCtx);

    // Fill out basic CurOp query exec properties. More metrics (nreturned and executionTime)
    // are collected within collectQueryStatsMongod.
    curOp->debug().cursorid = (cursor.has_value() ? cursor->getCursor()->cursorid() : -1);
    curOp->debug().cursorExhausted = !cursor.has_value();
    curOp->debug().getAdditiveMetrics().nBatches = 1;

    // Fill out CurOp based on explain summary statistics.
    PlanSummaryStats summaryStats;
    auto&& explainer = exec.getPlanExplainer();
    explainer.getSummaryStats(&summaryStats);

    if (collection) {
        CollectionIndexUsageTrackerDecoration::recordCollectionIndexUsage(
            collection.get(),
            summaryStats.collectionScans,
            summaryStats.collectionScansNonTailable,
            summaryStats.indexesUsed);
    }

    curOp->debug().setPlanSummaryMetrics(std::move(summaryStats));
    curOp->setEndOfOpMetrics(numResults);

    if (cursor) {
        collectQueryStatsMongod(opCtx, *cursor);
    } else {
        auto* cq = exec.getCanonicalQuery();
        const auto& expCtx = cq ? cq->getExpCtx() : makeBlankExpressionContext(opCtx, exec.nss());
        collectQueryStatsMongod(opCtx, expCtx, std::move(curOp->debug().getQueryStatsInfo().key));
    }

    if (curOp->shouldDBProfile()) {
        auto&& [stats, _] = explainer.getWinningPlanStats(ExplainOptions::Verbosity::kExecStats);
        curOp->debug().execStats = std::move(stats);
    }
}

}  // namespace mongo
