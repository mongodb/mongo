/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/s/write_ops/unified_write_executor/write_batch_executor.h"

#include "mongo/s/grid.h"
#include "mongo/s/write_ops/wc_error.h"

#include <boost/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo {
namespace unified_write_executor {
WriteBatchResponse WriteBatchExecutor::execute(OperationContext* opCtx, const WriteBatch& batch) {
    return std::visit(OverloadedVisitor{[&](const auto& batchData) {
                          return _execute(opCtx, batchData);
                      }},
                      batch);
}

std::vector<AsyncRequestsSender::Request> WriteBatchExecutor::buildBulkWriteRequests(
    OperationContext* opCtx, const SimpleWriteBatch& batch) const {
    std::vector<AsyncRequestsSender::Request> requestsToSend;
    for (auto& [shardId, shardRequest] : batch.requestByShardId) {
        std::vector<BulkWriteOpVariant> bulkOps;
        std::vector<NamespaceInfoEntry> nsInfos;
        std::map<NamespaceString, int> nsIndexMap;
        for (auto& op : shardRequest.ops) {
            auto& nss = op.getNss();
            auto versionIt = shardRequest.versionByNss.find(nss);
            tassert(10346801,
                    "The shard version info should be present in the batch",
                    versionIt != shardRequest.versionByNss.end());
            auto& version = versionIt->second;

            NamespaceInfoEntry nsInfo(nss);
            nsInfo.setShardVersion(version.shardVersion);
            nsInfo.setDatabaseVersion(version.databaseVersion);
            if (nsIndexMap.find(nsInfo.getNs()) == nsIndexMap.end()) {
                nsIndexMap[nsInfo.getNs()] = nsInfos.size();
                nsInfos.push_back(nsInfo);
            }
            auto nsIndex = nsIndexMap[nsInfo.getNs()];

            // Reassigns the namespace index for the list of ops.
            auto bulkOp = op.getBulkWriteOp();
            visit(
                OverloadedVisitor{
                    [&](auto& value) { return value.setNsInfoIdx(nsIndex); },
                },
                bulkOp);
            bulkOps.emplace_back(bulkOp);
        }

        auto bulkRequest = BulkWriteCommandRequest(std::move(bulkOps), std::move(nsInfos));
        bulkRequest.setOrdered(_context.getOrdered());
        BSONObjBuilder builder;
        bulkRequest.serialize(&builder);
        logical_session_id_helpers::serializeLsidAndTxnNumber(opCtx, &builder);
        auto writeConcern = getWriteConcernForShardRequest(opCtx);
        if (writeConcern) {
            builder.append(WriteConcernOptions::kWriteConcernField, writeConcern->toBSON());
        }
        auto bulkRequestObj = builder.obj();
        LOGV2_DEBUG(10605503,
                    4,
                    "Constructed request for shard",
                    "request"_attr = bulkRequestObj,
                    "shardId"_attr = shardId);
        requestsToSend.emplace_back(shardId, std::move(bulkRequestObj));
    }
    return requestsToSend;
}

WriteBatchResponse WriteBatchExecutor::_execute(OperationContext* opCtx,
                                                const SimpleWriteBatch& batch) {
    std::vector<AsyncRequestsSender::Request> requestsToSend = buildBulkWriteRequests(opCtx, batch);

    auto sender = MultiStatementTransactionRequestsSender(
        opCtx,
        Grid::get(opCtx)->getExecutorPool()->getArbitraryExecutor(),
        DatabaseName::kAdmin,
        std::move(requestsToSend),
        ReadPreferenceSetting(ReadPreference::PrimaryOnly),
        Shard::RetryPolicy::kNoRetry);

    WriteBatchResponse shardResponses;
    while (!sender.done()) {
        auto arsResponse = sender.next();
        ShardResponse shardResponse{std::move(arsResponse.swResponse),
                                    batch.requestByShardId.at(arsResponse.shardId).ops};
        shardResponses.emplace(std::move(arsResponse.shardId), std::move(shardResponse));
    }
    tassert(10346800,
            "There should same number of requests and responses from a simple write batch",
            shardResponses.size() == batch.requestByShardId.size());
    return shardResponses;
}

}  // namespace unified_write_executor
}  // namespace mongo
