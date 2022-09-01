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

#include "mongo/db/pipeline/process_interface/common_process_interface.h"

#include "mongo/bson/mutable/document.h"
#include "mongo/config.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/client.h"
#include "mongo/db/curop.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/operation_time_tracker.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/service_context.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/platform/mutex.h"
#include "mongo/s/grid.h"
#include "mongo/util/net/socket_utils.h"

#ifndef MONGO_CONFIG_USE_RAW_LATCHES
#include "mongo/util/diagnostic_info.h"
#endif

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault


namespace mongo {

std::vector<BSONObj> CommonProcessInterface::getCurrentOps(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    CurrentOpConnectionsMode connMode,
    CurrentOpSessionsMode sessionMode,
    CurrentOpUserMode userMode,
    CurrentOpTruncateMode truncateMode,
    CurrentOpCursorMode cursorMode,
    CurrentOpBacktraceMode backtraceMode) const {
    OperationContext* opCtx = expCtx->opCtx;
    AuthorizationSession* ctxAuth = AuthorizationSession::get(opCtx->getClient());

    std::vector<BSONObj> ops;

#ifndef MONGO_CONFIG_USE_RAW_LATCHES
    auto blockedOpGuard = DiagnosticInfo::maybeMakeBlockedOpForTest(opCtx->getClient());
#endif

    for (ServiceContext::LockedClientsCursor cursor(opCtx->getClient()->getServiceContext());
         Client* client = cursor.next();) {
        invariant(client);

        stdx::lock_guard<Client> lk(*client);

        // If auth is disabled, ignore the allUsers parameter.
        if (ctxAuth->getAuthorizationManager().isAuthEnabled() &&
            userMode == CurrentOpUserMode::kExcludeOthers &&
            !ctxAuth->isCoauthorizedWithClient(client, lk)) {
            continue;
        }

        // Ignore inactive connections unless 'idleConnections' is true.
        if (connMode == CurrentOpConnectionsMode::kExcludeIdle &&
            !client->hasAnyActiveCurrentOp()) {
            continue;
        }

        // Delegate to the mongoD- or mongoS-specific implementation of _reportCurrentOpForClient.
        ops.emplace_back(_reportCurrentOpForClient(opCtx, client, truncateMode, backtraceMode));
    }

    // If 'cursorMode' is set to include idle cursors, retrieve them and add them to ops.
    if (cursorMode == CurrentOpCursorMode::kIncludeCursors) {

        for (auto&& cursor : getIdleCursors(expCtx, userMode)) {
            BSONObjBuilder cursorObj;
            cursorObj.append("type", "idleCursor");
            cursorObj.append("host", getHostNameCachedAndPort());
            // First, extract fields which need to go at the top level out of the GenericCursor.
            auto ns = cursor.getNs();
            cursorObj.append("ns", ns->toString());
            if (auto lsid = cursor.getLsid()) {
                cursorObj.append("lsid", lsid->toBSON());
            }
            if (auto planSummaryData = cursor.getPlanSummary()) {  // Not present on mongos.
                cursorObj.append("planSummary", *planSummaryData);
            }

            // Next, append the stripped-down version of the generic cursor. This will avoid
            // duplicating information reported at the top level.
            cursorObj.append("cursor",
                             CurOp::truncateAndSerializeGenericCursor(&cursor, boost::none));

            ops.emplace_back(cursorObj.obj());
        }
    }

    // If we need to report on idle Sessions, defer to the mongoD or mongoS implementations.
    if (sessionMode == CurrentOpSessionsMode::kIncludeIdle) {
        _reportCurrentOpsForIdleSessions(opCtx, userMode, &ops);
    }

    if (!ctxAuth->getAuthorizationManager().isAuthEnabled() ||
        userMode == CurrentOpUserMode::kIncludeAll) {
        _reportCurrentOpsForTransactionCoordinators(
            opCtx, sessionMode == MongoProcessInterface::CurrentOpSessionsMode::kIncludeIdle, &ops);

        _reportCurrentOpsForPrimaryOnlyServices(opCtx, connMode, sessionMode, &ops);
    }

    return ops;
}

std::vector<FieldPath> CommonProcessInterface::collectDocumentKeyFieldsActingAsRouter(
    OperationContext* opCtx, const NamespaceString& nss) const {
    const auto cm =
        uassertStatusOK(Grid::get(opCtx)->catalogCache()->getCollectionRoutingInfo(opCtx, nss));
    if (cm.isSharded()) {
        return _shardKeyToDocumentKeyFields(cm.getShardKeyPattern().getKeyPatternFields());
    }

    // We have no evidence this collection is sharded, so the document key is just _id.
    return {"_id"};
}

std::unique_ptr<CommonProcessInterface::WriteSizeEstimator>
CommonProcessInterface::getWriteSizeEstimator(OperationContext* opCtx,
                                              const NamespaceString& ns) const {
    return std::make_unique<LocalWriteSizeEstimator>();
}

void CommonProcessInterface::updateClientOperationTime(OperationContext* opCtx) const {
    // In order to support causal consistency in a replica set or a sharded cluster when reading
    // with secondary read preference, the secondary must propagate the primary's operation time
    // to the client so that when the client attempts to read, the secondary will block until it
    // has replicated the primary's writes. As such, the 'operationTime' returned from the
    // primary is explicitly set on the given opCtx's client.
    //
    // Note that the operationTime is attached even when a command fails because writes may succeed
    // while the command fails (such as in a $merge where 'whenMatched' is set to fail). This
    // guarantees that the operation time returned to the client reflects the most recent
    // successful write executed by this client.
    auto replCoord = repl::ReplicationCoordinator::get(opCtx);
    if (replCoord) {
        auto operationTime = OperationTimeTracker::get(opCtx)->getMaxOperationTime();
        repl::ReplClientInfo::forClient(opCtx->getClient())
            .setLastProxyWriteTimestampForward(operationTime.asTimestamp());
    }
}

bool CommonProcessInterface::keyPatternNamesExactPaths(const BSONObj& keyPattern,
                                                       const std::set<FieldPath>& uniqueKeyPaths) {
    size_t nFieldsMatched = 0;
    for (auto&& elem : keyPattern) {
        if (!elem.isNumber()) {
            return false;
        }
        if (uniqueKeyPaths.find(elem.fieldNameStringData()) == uniqueKeyPaths.end()) {
            return false;
        }
        ++nFieldsMatched;
    }
    return nFieldsMatched == uniqueKeyPaths.size();
}

boost::optional<ShardVersion> CommonProcessInterface::refreshAndGetCollectionVersion(
    const boost::intrusive_ptr<ExpressionContext>& expCtx, const NamespaceString& nss) const {
    const auto cm = uassertStatusOK(Grid::get(expCtx->opCtx)
                                        ->catalogCache()
                                        ->getCollectionRoutingInfoWithRefresh(expCtx->opCtx, nss));

    return cm.isSharded() ? boost::make_optional(ShardVersion(
                                cm.getVersion(), CollectionIndexes(cm.getVersion(), boost::none)))
                          : boost::none;
}

std::vector<FieldPath> CommonProcessInterface::_shardKeyToDocumentKeyFields(
    const std::vector<std::unique_ptr<FieldRef>>& keyPatternFields) const {
    std::vector<FieldPath> result;
    bool gotId = false;
    for (auto& field : keyPatternFields) {
        result.emplace_back(field->dottedField());
        gotId |= (result.back().fullPath() == "_id");
    }
    if (!gotId) {  // If not part of the shard key, "_id" comes last.
        result.emplace_back("_id");
    }
    return result;
}

std::string CommonProcessInterface::getHostAndPort(OperationContext* opCtx) const {
    return getHostNameCachedAndPort();
}

}  // namespace mongo
