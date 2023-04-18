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

#include "mongo/s/sessions_collection_sharded.h"

#include "mongo/db/matcher/extensions_callback_noop.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/query_request_helper.h"
#include "mongo/db/session/sessions_collection_rs.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/rpc/op_msg_rpc_impls.h"
#include "mongo/s/catalog_cache.h"
#include "mongo/s/client/shard.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/cluster_write.h"
#include "mongo/s/grid.h"
#include "mongo/s/query/cluster_find.h"
#include "mongo/s/write_ops/batch_write_exec.h"
#include "mongo/s/write_ops/batched_command_request.h"
#include "mongo/s/write_ops/batched_command_response.h"
#include "mongo/util/assert_util.h"

namespace mongo {
namespace {

BSONObj lsidQuery(const LogicalSessionId& lsid) {
    return BSON(LogicalSessionRecord::kIdFieldName << lsid.toBSON());
}

}  // namespace

std::vector<LogicalSessionId> SessionsCollectionSharded::_groupSessionIdsByOwningShard(
    OperationContext* opCtx, const LogicalSessionIdSet& sessions) {
    const auto [cm, _] = uassertStatusOK(Grid::get(opCtx)->catalogCache()->getCollectionRoutingInfo(
        opCtx, NamespaceString::kLogicalSessionsNamespace));
    uassert(ErrorCodes::NamespaceNotSharded,
            str::stream() << "Collection "
                          << NamespaceString::kLogicalSessionsNamespace.toStringForErrorMsg()
                          << " is not sharded",
            cm.isSharded());

    std::multimap<ShardId, LogicalSessionId> sessionIdsByOwningShard;
    for (const auto& session : sessions) {
        sessionIdsByOwningShard.emplace(
            cm.findIntersectingChunkWithSimpleCollation(session.getId().toBSON()).getShardId(),
            session);
    }

    std::vector<LogicalSessionId> sessionIdsGroupedByShard;
    sessionIdsGroupedByShard.reserve(sessions.size());
    for (auto& session : sessionIdsByOwningShard) {
        sessionIdsGroupedByShard.emplace_back(std::move(session.second));
    }

    return sessionIdsGroupedByShard;
}

std::vector<LogicalSessionRecord> SessionsCollectionSharded::_groupSessionRecordsByOwningShard(
    OperationContext* opCtx, const LogicalSessionRecordSet& sessions) {
    const auto [cm, _] = uassertStatusOK(Grid::get(opCtx)->catalogCache()->getCollectionRoutingInfo(
        opCtx, NamespaceString::kLogicalSessionsNamespace));
    uassert(ErrorCodes::NamespaceNotSharded,
            str::stream() << "Collection "
                          << NamespaceString::kLogicalSessionsNamespace.toStringForErrorMsg()
                          << " is not sharded",
            cm.isSharded());

    std::multimap<ShardId, LogicalSessionRecord> sessionsByOwningShard;
    for (const auto& session : sessions) {
        sessionsByOwningShard.emplace(
            cm.findIntersectingChunkWithSimpleCollation(session.getId().toBSON()).getShardId(),
            session);
    }

    std::vector<LogicalSessionRecord> sessionRecordsGroupedByShard;
    sessionRecordsGroupedByShard.reserve(sessions.size());
    for (auto& session : sessionsByOwningShard) {
        sessionRecordsGroupedByShard.emplace_back(std::move(session.second));
    }

    return sessionRecordsGroupedByShard;
}

void SessionsCollectionSharded::setupSessionsCollection(OperationContext* opCtx) {
    checkSessionsCollectionExists(opCtx);
}

void SessionsCollectionSharded::checkSessionsCollectionExists(OperationContext* opCtx) {
    uassert(ErrorCodes::ShardingStateNotInitialized,
            "sharding state is not yet initialized",
            Grid::get(opCtx)->isShardingInitialized());

    // If the collection doesn't exist, fail. Only the config servers generate it.
    const auto [cm, _] = uassertStatusOK(
        Grid::get(opCtx)->catalogCache()->getShardedCollectionRoutingInfoWithPlacementRefresh(
            opCtx, NamespaceString::kLogicalSessionsNamespace));
}

void SessionsCollectionSharded::refreshSessions(OperationContext* opCtx,
                                                const LogicalSessionRecordSet& sessions) {
    auto send = [&](BSONObj toSend) {
        auto opMsg =
            OpMsgRequest::fromDBAndBody(NamespaceString::kLogicalSessionsNamespace.db(), toSend);
        auto request = BatchedCommandRequest::parseUpdate(opMsg);

        BatchedCommandResponse response;
        BatchWriteExecStats stats;

        cluster::write(opCtx, request, &stats, &response);
        uassertStatusOK(response.toStatus());
    };

    _doRefresh(NamespaceString::kLogicalSessionsNamespace,
               _groupSessionRecordsByOwningShard(opCtx, sessions),
               send);
}

void SessionsCollectionSharded::removeRecords(OperationContext* opCtx,
                                              const LogicalSessionIdSet& sessions) {
    auto send = [&](BSONObj toSend) {
        auto opMsg =
            OpMsgRequest::fromDBAndBody(NamespaceString::kLogicalSessionsNamespace.db(), toSend);
        auto request = BatchedCommandRequest::parseDelete(opMsg);

        BatchedCommandResponse response;
        BatchWriteExecStats stats;

        cluster::write(opCtx, request, &stats, &response);
        uassertStatusOK(response.toStatus());
    };

    _doRemove(NamespaceString::kLogicalSessionsNamespace,
              _groupSessionIdsByOwningShard(opCtx, sessions),
              send);
}

LogicalSessionIdSet SessionsCollectionSharded::findRemovedSessions(
    OperationContext* opCtx, const LogicalSessionIdSet& sessions) {

    bool apiStrict = APIParameters::get(opCtx).getAPIStrict().value_or(false);
    auto send = [&](BSONObj toSend) -> BSONObj {
        // If there is no '$db', append it.
        toSend =
            OpMsgRequest::fromDBAndBody(NamespaceString::kLogicalSessionsNamespace.db(), toSend)
                .body;
        auto findCommand = query_request_helper::makeFromFindCommand(
            toSend,
            static_cast<const NamespaceString&>(NamespaceString::kLogicalSessionsNamespace),
            apiStrict);

        const boost::intrusive_ptr<ExpressionContext> expCtx;
        auto cq = uassertStatusOK(
            CanonicalQuery::canonicalize(opCtx,
                                         std::move(findCommand),
                                         false,
                                         expCtx,
                                         ExtensionsCallbackNoop(),
                                         MatchExpressionParser::kBanAllSpecialFeatures));

        // Do the work to generate the first batch of results. This blocks waiting to get responses
        // from the shard(s).
        std::vector<BSONObj> batch;
        CursorId cursorId;
        cursorId = ClusterFind::runQuery(opCtx, *cq, ReadPreferenceSetting::get(opCtx), &batch);

        rpc::OpMsgReplyBuilder replyBuilder;
        CursorResponseBuilder::Options options;
        options.isInitialResponse = true;
        CursorResponseBuilder firstBatch(&replyBuilder, options);
        for (const auto& obj : batch) {
            firstBatch.append(obj);
        }
        firstBatch.done(cursorId, NamespaceString::kLogicalSessionsNamespace);

        return replyBuilder.releaseBody();
    };

    return _doFindRemoved(NamespaceString::kLogicalSessionsNamespace,
                          _groupSessionIdsByOwningShard(opCtx, sessions),
                          send);
}

}  // namespace mongo
