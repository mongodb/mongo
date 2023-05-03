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


#include "mongo/platform/basic.h"

#include "mongo/db/query/find.h"

#include <memory>

#include "mongo/base/error_codes.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/client.h"
#include "mongo/db/clientcursor.h"
#include "mongo/db/commands.h"
#include "mongo/db/curop.h"
#include "mongo/db/curop_failpoint_helpers.h"
#include "mongo/db/cursor_manager.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/exec/filter.h"
#include "mongo/db/exec/working_set_common.h"
#include "mongo/db/keypattern.h"
#include "mongo/db/matcher/extensions_callback_real.h"
#include "mongo/db/query/collection_query_info.h"
#include "mongo/db/query/explain.h"
#include "mongo/db/query/find_common.h"
#include "mongo/db/query/get_executor.h"
#include "mongo/db/query/internal_plans.h"
#include "mongo/db/query/plan_summary_stats.h"
#include "mongo/db/query/query_planner_params.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/s/collection_sharding_state.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
#include "mongo/db/stats/resource_consumption_metrics.h"
#include "mongo/db/stats/top.h"
#include "mongo/db/storage/storage_options.h"
#include "mongo/logv2/log.h"
#include "mongo/s/stale_exception.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/str.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery


namespace mongo {

using std::unique_ptr;

// Failpoint for checking whether we've received a getmore.
MONGO_FAIL_POINT_DEFINE(failReceivedGetmore);

bool shouldSaveCursor(OperationContext* opCtx,
                      const CollectionPtr& collection,
                      PlanExecutor::ExecState finalState,
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
    // are collected within collectTelemetryMongod.
    curOp->debug().cursorid = (cursor.has_value() ? cursor->getCursor()->cursorid() : -1);
    curOp->debug().cursorExhausted = !cursor.has_value();
    curOp->debug().additiveMetrics.nBatches = 1;

    // Fill out CurOp based on explain summary statistics.
    PlanSummaryStats summaryStats;
    auto&& explainer = exec.getPlanExplainer();
    explainer.getSummaryStats(&summaryStats);
    curOp->debug().setPlanSummaryMetrics(summaryStats);
    curOp->setEndOfOpMetrics(numResults);

    if (cursor) {
        collectTelemetryMongod(opCtx, *cursor);
    } else {
        collectTelemetryMongod(opCtx, cmdObj);
    }

    if (collection) {
        CollectionQueryInfo::get(collection).notifyOfQuery(opCtx, collection, summaryStats);
    }

    if (curOp->shouldDBProfile()) {
        auto&& [stats, _] = explainer.getWinningPlanStats(ExplainOptions::Verbosity::kExecStats);
        curOp->debug().execStats = std::move(stats);
    }
}

}  // namespace mongo
