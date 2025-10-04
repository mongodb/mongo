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

#include "mongo/db/global_catalog/ddl/sessions_collection_sharded.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/client/read_preference.h"
#include "mongo/db/api_parameters.h"
#include "mongo/db/global_catalog/catalog_cache/catalog_cache.h"
#include "mongo/db/global_catalog/chunk.h"
#include "mongo/db/global_catalog/chunk_manager.h"
#include "mongo/db/matcher/extensions_callback_noop.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/expression_context_builder.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/client_cursor/cursor_id.h"
#include "mongo/db/query/client_cursor/cursor_response.h"
#include "mongo/db/query/compiler/parsers/matcher/expression_parser.h"
#include "mongo/db/query/find_command.h"
#include "mongo/db/query/query_request_helper.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/rpc/op_msg_rpc_impls.h"
#include "mongo/s/cluster_write.h"
#include "mongo/s/query/planner/cluster_find.h"
#include "mongo/s/write_ops/batch_write_exec.h"
#include "mongo/s/write_ops/batched_command_request.h"
#include "mongo/s/write_ops/batched_command_response.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/decorable.h"
#include "mongo/util/intrusive_counter.h"
#include "mongo/util/str.h"
#include "mongo/util/uuid.h"

#include <map>
#include <memory>
#include <utility>

#include <absl/container/node_hash_set.h>
#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {
namespace {

BSONObj lsidQuery(const LogicalSessionId& lsid) {
    return BSON(LogicalSessionRecord::kIdFieldName << lsid.toBSON());
}

}  // namespace

std::vector<LogicalSessionId> SessionsCollectionSharded::_groupSessionIdsByOwningShard(
    OperationContext* opCtx, const LogicalSessionIdSet& sessions) {
    const auto cri = uassertStatusOK(Grid::get(opCtx)->catalogCache()->getCollectionRoutingInfo(
        opCtx, NamespaceString::kLogicalSessionsNamespace));
    uassert(ErrorCodes::NamespaceNotSharded,
            str::stream() << "Collection "
                          << NamespaceString::kLogicalSessionsNamespace.toStringForErrorMsg()
                          << " is not sharded",
            cri.isSharded());

    std::multimap<ShardId, LogicalSessionId> sessionIdsByOwningShard;
    for (const auto& session : sessions) {
        sessionIdsByOwningShard.emplace(
            cri.getChunkManager()
                .findIntersectingChunkWithSimpleCollation(session.getId().toBSON())
                .getShardId(),
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
    const auto cri = uassertStatusOK(Grid::get(opCtx)->catalogCache()->getCollectionRoutingInfo(
        opCtx, NamespaceString::kLogicalSessionsNamespace));
    uassert(ErrorCodes::NamespaceNotSharded,
            str::stream() << "Collection "
                          << NamespaceString::kLogicalSessionsNamespace.toStringForErrorMsg()
                          << " is not sharded",
            cri.isSharded());

    std::multimap<ShardId, LogicalSessionRecord> sessionsByOwningShard;
    for (const auto& session : sessions) {
        sessionsByOwningShard.emplace(
            cri.getChunkManager()
                .findIntersectingChunkWithSimpleCollation(session.getId().toBSON())
                .getShardId(),
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
    const auto cm =
        uassertStatusOK(Grid::get(opCtx)->catalogCache()->getCollectionPlacementInfoWithRefresh(
            opCtx, NamespaceString::kLogicalSessionsNamespace));

    uassert(ErrorCodes::NamespaceNotSharded,
            str::stream() << "Session collection '"
                          << NamespaceString::kLogicalSessionsNamespace.toStringForErrorMsg()
                          << "' is not sharded",
            cm.isSharded());
}

void SessionsCollectionSharded::refreshSessions(OperationContext* opCtx,
                                                const LogicalSessionRecordSet& sessions) {
    auto send = [&](BSONObj toSend) {
        auto opMsg =
            OpMsgRequestBuilder::create(auth::ValidatedTenancyScope::get(opCtx),
                                        NamespaceString::kLogicalSessionsNamespace.dbName(),
                                        toSend);
        auto request = BatchedCommandRequest::parseUpdate(opMsg);

        BatchedCommandResponse response;
        BatchWriteExecStats stats;

        cluster::write(opCtx, request, nullptr /* nss */, &stats, &response);
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
            OpMsgRequestBuilder::create(auth::ValidatedTenancyScope::get(opCtx),
                                        NamespaceString::kLogicalSessionsNamespace.dbName(),
                                        toSend);
        auto request = BatchedCommandRequest::parseDelete(opMsg);

        BatchedCommandResponse response;
        BatchWriteExecStats stats;

        cluster::write(opCtx, request, nullptr, &stats, &response);
        uassertStatusOK(response.toStatus());
    };

    _doRemove(NamespaceString::kLogicalSessionsNamespace,
              _groupSessionIdsByOwningShard(opCtx, sessions),
              send);
}

LogicalSessionIdSet SessionsCollectionSharded::findRemovedSessions(
    OperationContext* opCtx, const LogicalSessionIdSet& sessions) {

    auto send = [&](BSONObj toSend) -> BSONObj {
        // If there is no '$db', append it.
        toSend = OpMsgRequestBuilder::create(auth::ValidatedTenancyScope::get(opCtx),
                                             NamespaceString::kLogicalSessionsNamespace.dbName(),
                                             toSend)
                     .body;
        auto findCommand =
            query_request_helper::makeFromFindCommand(toSend,
                                                      auth::ValidatedTenancyScope::get(opCtx),
                                                      boost::none,
                                                      SerializationContext::stateDefault());

        const auto nss = findCommand->getNamespaceOrUUID().nss();
        rpc::OpMsgReplyBuilder replyBuilder;
        ClusterFind::runQuery(opCtx,
                              std::move(findCommand),
                              nss,
                              ReadPreferenceSetting::get(opCtx),
                              MatchExpressionParser::kBanAllSpecialFeatures,
                              &replyBuilder);

        return replyBuilder.releaseBody();
    };

    return _doFindRemoved(NamespaceString::kLogicalSessionsNamespace,
                          _groupSessionIdsByOwningShard(opCtx, sessions),
                          send);
}

}  // namespace mongo
