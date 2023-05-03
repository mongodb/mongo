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

#include "mongo/s/query/store_possible_cursor.h"

#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/curop.h"
#include "mongo/db/query/cursor_response.h"
#include "mongo/db/shard_id.h"
#include "mongo/executor/task_executor_pool.h"
#include "mongo/s/grid.h"
#include "mongo/s/query/cluster_client_cursor_impl.h"
#include "mongo/s/query/cluster_client_cursor_params.h"
#include "mongo/s/query/cluster_cursor_manager.h"
#include "mongo/s/transaction_router.h"

namespace mongo {

StatusWith<BSONObj> storePossibleCursor(OperationContext* opCtx,
                                        const NamespaceString& requestedNss,
                                        OwnedRemoteCursor&& remoteCursor,
                                        PrivilegeVector privileges,
                                        TailableModeEnum tailableMode) {
    auto executorPool = Grid::get(opCtx)->getExecutorPool();
    auto result = storePossibleCursor(
        opCtx,
        remoteCursor->getShardId().toString(),
        remoteCursor->getHostAndPort(),
        remoteCursor->getCursorResponse().toBSON(CursorResponse::ResponseType::InitialResponse),
        requestedNss,
        executorPool->getArbitraryExecutor(),
        Grid::get(opCtx)->getCursorManager(),
        std::move(privileges),
        tailableMode);

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
                                        boost::optional<BSONObj> routerSort) {
    if (!cmdResult["ok"].trueValue() || !cmdResult.hasField("cursor")) {
        return cmdResult;
    }

    auto incomingCursorResponse = CursorResponse::parseFromBSON(cmdResult);
    if (!incomingCursorResponse.isOK()) {
        return incomingCursorResponse.getStatus();
    }

    auto&& opDebug = CurOp::get(opCtx)->debug();
    opDebug.additiveMetrics.nBatches = 1;
    // If nShards has already been set, then we are storing the forwarding $mergeCursors cursor from
    // a split aggregation pipeline, and the shards half of that pipeline may have targeted multiple
    // shards. In that case, leave the current value as-is.
    opDebug.nShards = std::max(opDebug.nShards, 1);
    CurOp::get(opCtx)->setEndOfOpMetrics(incomingCursorResponse.getValue().getBatch().size());

    if (incomingCursorResponse.getValue().getCursorId() == CursorId(0)) {
        opDebug.cursorExhausted = true;
        collectTelemetryMongos(opCtx, CurOp::get(opCtx)->opDescription());
        return cmdResult;
    }

    ClusterClientCursorParams params(incomingCursorResponse.getValue().getNSS(),
                                     APIParameters::get(opCtx),
                                     boost::none,
                                     repl::ReadConcernArgs::get(opCtx));
    params.remotes.emplace_back();
    auto& remoteCursor = params.remotes.back();
    remoteCursor.setShardId(shardId.toString());
    remoteCursor.setHostAndPort(server);
    remoteCursor.setCursorResponse(
        CursorResponse(incomingCursorResponse.getValue().getNSS(),
                       incomingCursorResponse.getValue().getCursorId(),
                       {}, /* batch */
                       incomingCursorResponse.getValue().getAtClusterTime(),
                       incomingCursorResponse.getValue().getPostBatchResumeToken()));
    params.originatingCommandObj = CurOp::get(opCtx)->opDescription().getOwned();
    params.tailableMode = tailableMode;
    params.lsid = opCtx->getLogicalSessionId();
    params.txnNumber = opCtx->getTxnNumber();
    params.originatingPrivileges = std::move(privileges);
    if (routerSort) {
        params.sortToApplyOnRouter = *routerSort;
    }

    if (TransactionRouter::get(opCtx)) {
        params.isAutoCommit = false;
    }

    auto ccc = ClusterClientCursorImpl::make(opCtx, std::move(executor), std::move(params));
    collectTelemetryMongos(opCtx, ccc);
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

    CursorResponse outgoingCursorResponse(
        requestedNss,
        clusterCursorId.getValue(),
        incomingCursorResponse.getValue().getBatch(),
        incomingCursorResponse.getValue().getAtClusterTime(),
        incomingCursorResponse.getValue().getPostBatchResumeToken());
    return outgoingCursorResponse.toBSON(CursorResponse::ResponseType::InitialResponse);
}

}  // namespace mongo
