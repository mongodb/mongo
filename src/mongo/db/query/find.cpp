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

#include <boost/move/utility_core.hpp>
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
        return collection && collection->numRecords(opCtx) != 0U;
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
    curOp->debug().additiveMetrics.nBatches = 1;

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
        collectQueryStatsMongod(opCtx, expCtx, std::move(curOp->debug().queryStatsInfo.key));
    }

    if (curOp->shouldDBProfile()) {
        auto&& [stats, _] = explainer.getWinningPlanStats(ExplainOptions::Verbosity::kExecStats);
        curOp->debug().execStats = std::move(stats);
    }
}

}  // namespace mongo
