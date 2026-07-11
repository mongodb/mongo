// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/s/query/exec/store_possible_cursor.h"

#include "mongo/base/status_with.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/client/read_preference.h"
#include "mongo/db/api_parameters.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/curop.h"
#include "mongo/db/query/client_cursor/cursor_id.h"
#include "mongo/db/query/client_cursor/cursor_response.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/session/logical_session_id_gen.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/executor/task_executor_pool.h"
#include "mongo/s/query/exec/async_results_merger_params_gen.h"
#include "mongo/s/query/exec/cluster_client_cursor.h"
#include "mongo/s/query/exec/cluster_client_cursor_guard.h"
#include "mongo/s/query/exec/cluster_client_cursor_impl.h"
#include "mongo/s/query/exec/cluster_client_cursor_params.h"
#include "mongo/s/query/exec/cluster_cursor_manager.h"
#include "mongo/s/query/exec/collect_query_stats_mongos.h"
#include "mongo/s/query/exec/router_stage_merge.h"
#include "mongo/s/query/exec/router_stage_transform.h"
#include "mongo/s/transaction_router.h"
#include "mongo/util/decorable.h"

#include <algorithm>
#include <functional>
#include <utility>
#include <vector>

#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

StatusWith<BSONObj> storePossibleCursor(OperationContext* opCtx,
                                        const NamespaceString& requestedNss,
                                        OwnedRemoteCursor&& remoteCursor,
                                        PrivilegeVector privileges,
                                        TailableModeEnum tailableMode,
                                        bool allowPartialResults) {
    auto executorPool = Grid::get(opCtx)->getExecutorPool();
    auto result = storePossibleCursor(opCtx,
                                      std::string{remoteCursor->getShardId()},
                                      remoteCursor->getHostAndPort(),
                                      std::move(remoteCursor->getCursorResponse()),
                                      requestedNss,
                                      executorPool->getArbitraryExecutor(),
                                      Grid::get(opCtx)->getCursorManager(),
                                      std::move(privileges),
                                      tailableMode,
                                      boost::none,
                                      allowPartialResults);

    // On success, release ownership of the cursor because it has been registered with the cursor
    // manager and is now owned there.
    remoteCursor.releaseCursor();
    return result;
}

StatusWith<BSONObj> storePossibleCursor(OperationContext* opCtx,
                                        const ShardId& shardId,
                                        const HostAndPort& server,
                                        const BSONObj& cmdResult,
                                        const NamespaceString& requestedNss,
                                        std::shared_ptr<executor::TaskExecutor> executor,
                                        ClusterCursorManager* cursorManager,
                                        PrivilegeVector privileges,
                                        TailableModeEnum tailableMode,
                                        boost::optional<BSONObj> routerSort,
                                        bool allowPartialResults) {
    if (!cmdResult["ok"].trueValue() || !cmdResult.hasField("cursor")) {
        return cmdResult;
    }

    auto incomingCursorResponse = CursorResponse::parseFromBSON(cmdResult);
    if (!incomingCursorResponse.isOK()) {
        return incomingCursorResponse.getStatus();
    }

    auto& response = incomingCursorResponse.getValue();
    if (const auto& cursorMetrics = response.getCursorMetrics()) {
        CurOp::get(opCtx)->debug().getAdditiveMetrics().aggregateCursorMetrics(*cursorMetrics);
    }

    return storePossibleCursor(opCtx,
                               shardId,
                               server,
                               std::move(response),
                               requestedNss,
                               std::move(executor),
                               cursorManager,
                               privileges,
                               tailableMode,
                               routerSort,
                               allowPartialResults);
}

StatusWith<BSONObj> storePossibleCursor(OperationContext* opCtx,
                                        const ShardId& shardId,
                                        const HostAndPort& server,
                                        CursorResponse&& incomingCursorResponse,
                                        const NamespaceString& requestedNss,
                                        std::shared_ptr<executor::TaskExecutor> executor,
                                        ClusterCursorManager* cursorManager,
                                        PrivilegeVector privileges,
                                        TailableModeEnum tailableMode,
                                        boost::optional<BSONObj> routerSort,
                                        bool allowPartialResults) {
    auto&& opDebug = CurOp::get(opCtx)->debug();
    opDebug.getAdditiveMetrics().nBatches = 1;
    // If nShards has already been set, then we are storing the forwarding $mergeCursors cursor from
    // a split aggregation pipeline, and the shards half of that pipeline may have targeted multiple
    // shards. In that case, leave the current value as-is.
    opDebug.nShards = std::max(opDebug.nShards, 1);
    CurOp::get(opCtx)->setEndOfOpMetrics(incomingCursorResponse.getBatch().size());

    if (incomingCursorResponse.getCursorId() == CursorId(0)) {
        opDebug.cursorExhausted = true;
        collectQueryStatsMongos(opCtx, std::move(opDebug.getQueryStatsInfo().key));
        CursorResponse exhaustedResponse(requestedNss,
                                         CursorId(0),
                                         incomingCursorResponse.releaseBatch(),
                                         incomingCursorResponse.getAtClusterTime(),
                                         incomingCursorResponse.getPostBatchResumeToken());
        exhaustedResponse.setPartialResultsReturned(
            incomingCursorResponse.getPartialResultsReturned());
        return exhaustedResponse.toBSON(CursorResponse::ResponseType::InitialResponse);
    }

    ClusterClientCursorParams params(incomingCursorResponse.getNSS(),
                                     APIParameters::get(opCtx),
                                     boost::none /* ReadPreferenceSetting */,
                                     repl::ReadConcernArgs::get(opCtx),
                                     [&] {
                                         if (!opCtx->getLogicalSessionId())
                                             return OperationSessionInfoFromClient();

                                         OperationSessionInfoFromClient osi{
                                             *opCtx->getLogicalSessionId(), opCtx->getTxnNumber()};
                                         if (TransactionRouter::get(opCtx)) {
                                             osi.setAutocommit(false);
                                         }
                                         return osi;
                                     }());
    params.isAllowPartialResults = allowPartialResults;
    params.remotes.emplace_back();
    auto& remoteCursor = params.remotes.back();
    remoteCursor.setShardId(shardId.toString());
    remoteCursor.setHostAndPort(server);
    CursorResponse remoteCursorResponse(incomingCursorResponse.getNSS(),
                                        incomingCursorResponse.getCursorId(),
                                        {}, /* batch */
                                        incomingCursorResponse.getAtClusterTime(),
                                        incomingCursorResponse.getPostBatchResumeToken());
    remoteCursorResponse.setPartialResultsReturned(
        incomingCursorResponse.getPartialResultsReturned());
    remoteCursor.setCursorResponse(std::move(remoteCursorResponse));
    params.originatingCommandObj = CurOp::get(opCtx)->opDescription().getOwned();
    params.tailableMode = tailableMode;
    params.originatingPrivileges = std::move(privileges);
    if (routerSort) {
        params.sortToApplyOnRouter = *routerSort;
    }
    if (incomingCursorResponse.getCursorMetrics().has_value()) {
        IncludeMetrics im;
        im.setQueryStats(true);
        params.remoteMetricsToInclude = std::move(im);
    }

    auto ccc = ClusterClientCursorImpl::make(opCtx, std::move(executor), std::move(params));
    collectQueryStatsMongos(opCtx, ccc);
    // We don't expect to use this cursor until a subsequent getMore, so detach from the current
    // OperationContext until then.
    ccc->detachFromOperationContext();
    auto authUser = AuthorizationSession::get(opCtx->getClient())->getAuthenticatedUserName();
    auto clusterCursorId =
        cursorManager->registerCursor(opCtx,
                                      ccc.releaseCursor(),
                                      requestedNss,
                                      ClusterCursorManager::CursorType::SingleTarget,
                                      ClusterCursorManager::CursorLifetime::Mortal,
                                      authUser);
    if (!clusterCursorId.isOK()) {
        return clusterCursorId.getStatus();
    }

    opDebug.cursorid = clusterCursorId.getValue();

    CursorResponse outgoingCursorResponse(requestedNss,
                                          clusterCursorId.getValue(),
                                          incomingCursorResponse.releaseBatch(),
                                          incomingCursorResponse.getAtClusterTime(),
                                          incomingCursorResponse.getPostBatchResumeToken());
    outgoingCursorResponse.setPartialResultsReturned(
        incomingCursorResponse.getPartialResultsReturned());
    return outgoingCursorResponse.toBSON(CursorResponse::ResponseType::InitialResponse);
}

StatusWith<BSONObj> storePossibleCursor(OperationContext* opCtx,
                                        const ShardId& shardId,
                                        const HostAndPort& server,
                                        const BSONObj& cmdResult,
                                        const NamespaceString& requestedNss,
                                        std::shared_ptr<executor::TaskExecutor> executor,
                                        ClusterCursorManager* cursorManager,
                                        PrivilegeVector privileges,
                                        std::function<BSONObj(BSONObj)> documentTransform,
                                        TailableModeEnum tailableMode,
                                        boost::optional<BSONObj> routerSort,
                                        bool allowPartialResults) {
    if (!cmdResult["ok"].trueValue() || !cmdResult.hasField("cursor")) {
        return cmdResult;
    }

    auto incomingCursorResponse = CursorResponse::parseFromBSON(cmdResult);
    if (!incomingCursorResponse.isOK()) {
        return incomingCursorResponse.getStatus();
    }

    auto& response = incomingCursorResponse.getValue();
    if (const auto& cursorMetrics = response.getCursorMetrics()) {
        CurOp::get(opCtx)->debug().getAdditiveMetrics().aggregateCursorMetrics(*cursorMetrics);
    }

    auto&& opDebug = CurOp::get(opCtx)->debug();
    opDebug.getAdditiveMetrics().nBatches = 1;
    opDebug.nShards = std::max(opDebug.nShards, 1);
    CurOp::get(opCtx)->setEndOfOpMetrics(response.getBatch().size());

    // Apply the transform to the first batch, which comes directly from the shard response.
    std::vector<BSONObj> transformedFirstBatch;
    transformedFirstBatch.reserve(response.getBatch().size());
    for (const auto& doc : response.getBatch()) {
        transformedFirstBatch.push_back(documentTransform(doc));
    }

    if (response.getCursorId() == CursorId(0)) {
        // Cursor exhausted in the first batch, no proxy cursor is needed.
        opDebug.cursorExhausted = true;
        collectQueryStatsMongos(opCtx, std::move(opDebug.getQueryStatsInfo().key));
        CursorResponse exhaustedResponse(requestedNss,
                                         CursorId(0),
                                         std::move(transformedFirstBatch),
                                         response.getAtClusterTime(),
                                         response.getPostBatchResumeToken());
        exhaustedResponse.setPartialResultsReturned(response.getPartialResultsReturned());
        return exhaustedResponse.toBSON(CursorResponse::ResponseType::InitialResponse);
    }

    // Build the cursor params pointing at the shard's open cursor.
    ClusterClientCursorParams params(response.getNSS(),
                                     APIParameters::get(opCtx),
                                     boost::none /* ReadPreferenceSetting */,
                                     repl::ReadConcernArgs::get(opCtx),
                                     [&] {
                                         if (!opCtx->getLogicalSessionId())
                                             return OperationSessionInfoFromClient();
                                         OperationSessionInfoFromClient osi{
                                             *opCtx->getLogicalSessionId(), opCtx->getTxnNumber()};
                                         if (TransactionRouter::get(opCtx)) {
                                             osi.setAutocommit(false);
                                         }
                                         return osi;
                                     }());
    params.isAllowPartialResults = allowPartialResults;
    params.remotes.emplace_back();
    auto& remoteCursor = params.remotes.back();
    remoteCursor.setShardId(shardId.toString());
    remoteCursor.setHostAndPort(server);
    CursorResponse remoteCursorResponse(response.getNSS(),
                                        response.getCursorId(),
                                        {} /* first batch served above */,
                                        response.getAtClusterTime(),
                                        response.getPostBatchResumeToken());
    remoteCursorResponse.setPartialResultsReturned(response.getPartialResultsReturned());
    remoteCursor.setCursorResponse(std::move(remoteCursorResponse));
    params.originatingCommandObj = CurOp::get(opCtx)->opDescription().getOwned();
    params.tailableMode = tailableMode;
    params.originatingPrivileges = std::move(privileges);
    if (routerSort) {
        params.sortToApplyOnRouter = *routerSort;
    }
    if (response.getCursorMetrics().has_value()) {
        IncludeMetrics im;
        im.setQueryStats(true);
        params.remoteMetricsToInclude = std::move(im);
    }

    // Build the execution plan: RouterStageTransform wraps RouterStageMerge so the
    // documentTransform is applied to every document in every batch, including getMore batches,
    // without buffering the full result set in mongos memory.
    //
    // extractARMParams() moves 'remotes' out of params. The remaining fields in params are still
    // needed by ClusterClientCursorImpl for auth, session, and metrics bookkeeping.
    auto armParams = params.extractARMParams(opCtx);
    auto mergeStage = std::make_unique<RouterStageMerge>(opCtx, executor, std::move(armParams));
    auto transformStage = std::make_unique<RouterStageTransform>(
        opCtx, std::move(mergeStage), std::move(documentTransform));

    auto ccc = ClusterClientCursorImpl::make(opCtx, std::move(transformStage), std::move(params));
    collectQueryStatsMongos(opCtx, ccc);
    ccc->detachFromOperationContext();

    auto authUser = AuthorizationSession::get(opCtx->getClient())->getAuthenticatedUserName();
    auto clusterCursorId =
        cursorManager->registerCursor(opCtx,
                                      ccc.releaseCursor(),
                                      requestedNss,
                                      ClusterCursorManager::CursorType::SingleTarget,
                                      ClusterCursorManager::CursorLifetime::Mortal,
                                      authUser);
    if (!clusterCursorId.isOK()) {
        return clusterCursorId.getStatus();
    }

    opDebug.cursorid = clusterCursorId.getValue();

    CursorResponse outgoingCursorResponse(requestedNss,
                                          clusterCursorId.getValue(),
                                          std::move(transformedFirstBatch),
                                          response.getAtClusterTime(),
                                          response.getPostBatchResumeToken());
    outgoingCursorResponse.setPartialResultsReturned(response.getPartialResultsReturned());
    return outgoingCursorResponse.toBSON(CursorResponse::ResponseType::InitialResponse);
}

}  // namespace mongo
