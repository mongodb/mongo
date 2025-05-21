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

#include <boost/optional.hpp>

#include "mongo/s/grid.h"
#include "mongo/s/write_ops/unified_write_executor/write_batch_executor.h"

namespace mongo {
namespace unified_write_executor {

WriteBatchResponse WriteBatchExecutor::execute(OperationContext* opCtx, const WriteBatch& batch) {
    return std::visit(OverloadedVisitor{[&](const auto& batchData) {
                          return _execute(opCtx, batchData);
                      }},
                      batch);
}

WriteBatchResponse WriteBatchExecutor::_execute(OperationContext* opCtx,
                                                const SimpleWriteBatch& batch) {
    std::vector<AsyncRequestsSender::Request> requestsToSent;
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

            // Reassigns the namespace index for the list of ops
            auto bulkOp = op.getBulkWriteOp();
            visit(OverloadedVisitor{
                      [&](mongo::BulkWriteInsertOp& value) { return value.setInsert(nsIndex); },
                      [&](mongo::BulkWriteUpdateOp& value) { return value.setUpdate(nsIndex); },
                      [&](mongo::BulkWriteDeleteOp& value) {
                          return value.setDeleteCommand(nsIndex);
                      }},
                  bulkOp);
            bulkOps.emplace_back(bulkOp);
        }

        auto bulkRequest = BulkWriteCommandRequest(std::move(bulkOps), std::move(nsInfos));
        BSONObjBuilder builder;
        bulkRequest.serialize(&builder);
        logical_session_id_helpers::serializeLsidAndTxnNumber(opCtx, &builder);
        builder.append(WriteConcernOptions::kWriteConcernField, opCtx->getWriteConcern().toBSON());
        requestsToSent.emplace_back(shardId, builder.obj());
    }

    auto sender = MultiStatementTransactionRequestsSender(
        opCtx,
        Grid::get(opCtx)->getExecutorPool()->getArbitraryExecutor(),
        DatabaseName::kAdmin,
        std::move(requestsToSent),
        ReadPreferenceSetting(ReadPreference::PrimaryOnly),
        Shard::RetryPolicy::kNoRetry);

    WriteBatchResponse shardResponses;
    while (!sender.done()) {
        auto shardResponse = sender.next();
        shardResponses.emplace(std::move(shardResponse.shardId),
                               std::move(shardResponse.swResponse));
    }
    tassert(10346800,
            "There should same number of requests and responses from a simple write batch",
            shardResponses.size() == batch.requestByShardId.size());
    return shardResponses;
}

}  // namespace unified_write_executor
}  // namespace mongo
