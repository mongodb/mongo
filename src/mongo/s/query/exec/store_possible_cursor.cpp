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

#include <algorithm>
#include <boost/move/utility_core.hpp>
#include <utility>
#include <vector>

#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
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
#include "mongo/db/shard_id.h"
#include "mongo/executor/task_executor_pool.h"
#include "mongo/s/grid.h"
#include "mongo/s/query/exec/async_results_merger_params_gen.h"
#include "mongo/s/query/exec/cluster_client_cursor.h"
#include "mongo/s/query/exec/cluster_client_cursor_guard.h"
#include "mongo/s/query/exec/cluster_client_cursor_impl.h"
#include "mongo/s/query/exec/cluster_client_cursor_params.h"
#include "mongo/s/query/exec/cluster_cursor_manager.h"
#include "mongo/s/query/exec/store_possible_cursor.h"
#include "mongo/s/transaction_router.h"
#include "mongo/util/decorable.h"

namespace mongo {

StatusWith<BSONObj> storePossibleCursor(OperationContext* opCtx,
                                        const NamespaceString& requestedNss,
                                        OwnedRemoteCursor&& remoteCursor,
                                        PrivilegeVector privileges,
                                        TailableModeEnum tailableMode) {
    auto executorPool = Grid::get(opCtx)->getExecutorPool();
    auto result = storePossibleCursor(opCtx,
                                      remoteCursor->getShardId().toString(),
                                      remoteCursor->getHostAndPort(),
                                      remoteCursor->getCursorResponse(),
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

    const auto& response = incomingCursorResponse.getValue();
    if (const auto& cursorMetrics = response.getCursorMetrics()) {
        CurOp::get(opCtx)->debug().additiveMetrics.aggregateCursorMetrics(*cursorMetrics);
    }

    return storePossibleCursor(opCtx,
                               shardId,
                               server,
                               response,
                               requestedNss,
                               std::move(executor),
                               cursorManager,
                               privileges,
                               tailableMode,
                               routerSort);
}

StatusWith<BSONObj> storePossibleCursor(OperationContext* opCtx,
                                        const ShardId& shardId,
                                        const HostAndPort& server,
                                        const CursorResponse& incomingCursorResponse,
                                        const NamespaceString& requestedNss,
                                        std::shared_ptr<executor::TaskExecutor> executor,
                                        ClusterCursorManager* cursorManager,
                                        PrivilegeVector privileges,
                                        TailableModeEnum tailableMode,
                                        boost::optional<BSONObj> routerSort) {
    auto&& opDebug = CurOp::get(opCtx)->debug();
    opDebug.additiveMetrics.nBatches = 1;
    // If nShards has already been set, then we are storing the forwarding $mergeCursors cursor from
    // a split aggregation pipeline, and the shards half of that pipeline may have targeted multiple
    // shards. In that case, leave the current value as-is.
    opDebug.nShards = std::max(opDebug.nShards, 1);
    CurOp::get(opCtx)->setEndOfOpMetrics(incomingCursorResponse.getBatch().size());

    if (incomingCursorResponse.getCursorId() == CursorId(0)) {
        opDebug.cursorExhausted = true;
        collectQueryStatsMongos(opCtx, std::move(opDebug.queryStatsInfo.key));
        return incomingCursorResponse.toBSON(CursorResponse::ResponseType::InitialResponse);
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
    params.remotes.emplace_back();
    auto& remoteCursor = params.remotes.back();
    remoteCursor.setShardId(shardId.toString());
    remoteCursor.setHostAndPort(server);
    remoteCursor.setCursorResponse(
        CursorResponse(incomingCursorResponse.getNSS(),
                       incomingCursorResponse.getCursorId(),
                       {}, /* batch */
                       incomingCursorResponse.getAtClusterTime(),
                       incomingCursorResponse.getPostBatchResumeToken()));
    params.originatingCommandObj = CurOp::get(opCtx)->opDescription().getOwned();
    params.tailableMode = tailableMode;
    params.originatingPrivileges = std::move(privileges);
    if (routerSort) {
        params.sortToApplyOnRouter = *routerSort;
    }
    params.requestQueryStatsFromRemotes = incomingCursorResponse.getCursorMetrics().has_value();

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
                                          incomingCursorResponse.getBatch(),
                                          incomingCursorResponse.getAtClusterTime(),
                                          incomingCursorResponse.getPostBatchResumeToken());
    return outgoingCursorResponse.toBSON(CursorResponse::ResponseType::InitialResponse);
}

}  // namespace mongo
