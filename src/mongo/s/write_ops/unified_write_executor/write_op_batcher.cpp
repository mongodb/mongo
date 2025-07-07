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

#include "mongo/s/write_ops/unified_write_executor/write_op_batcher.h"

#include "mongo/logv2/log.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {
namespace unified_write_executor {

boost::optional<WriteBatch> OrderedWriteOpBatcher::getNextBatch(OperationContext* opCtx,
                                                                const RoutingContext& routingCtx) {
    if (unrecoverableError) {
        return boost::none;
    }
    auto writeOp = _producer.peekNext();
    if (!writeOp) {
        return boost::none;
    }
    _producer.advance();

    auto analysis = _analyzer.analyze(opCtx, routingCtx, *writeOp);
    switch (analysis.type) {
        case kSingleShard: {
            tassert(10346700,
                    "Single shard write type should only target a single shard",
                    analysis.shardsAffected.size() == 1);
            std::vector<WriteOp> writeOps{*writeOp};
            std::map<NamespaceString, ShardEndpoint> versionByNss{
                {writeOp->getNss(), analysis.shardsAffected.front()}};
            auto shardId = analysis.shardsAffected.front().shardName;

            while (true) {
                auto nextWriteOp = _producer.peekNext();
                if (!nextWriteOp) {
                    break;
                }
                auto nextAnalysis = _analyzer.analyze(opCtx, routingCtx, *nextWriteOp);
                // Only add consecutive ops targeting the same shard into one batch. The ordered
                // constraint will be maintained on the shard.
                if (nextAnalysis.type != kSingleShard ||
                    nextAnalysis.shardsAffected.front().shardName != shardId) {
                    break;
                }
                tassert(10346701,
                        "Single shard write type should only target a single shard",
                        nextAnalysis.shardsAffected.size() == 1);
                writeOps.push_back(*nextWriteOp);
                auto versionFound = versionByNss.find(nextWriteOp->getNss());
                if (versionFound != versionByNss.end()) {
                    tassert(10483300,
                            "Shard version for the same namespace need to be the same",
                            versionFound->second == nextAnalysis.shardsAffected.front());
                }
                versionByNss.emplace_hint(
                    versionFound, nextWriteOp->getNss(), nextAnalysis.shardsAffected.front());
                _producer.advance();
            }
            return WriteBatch{SimpleWriteBatch{
                {{shardId,
                  SimpleWriteBatch::ShardRequest{std::move(versionByNss), std::move(writeOps)}}}}};
        } break;

        case kMultiShard: {
            SimpleWriteBatch batch;
            for (auto& shardVersion : analysis.shardsAffected) {
                SimpleWriteBatch::ShardRequest shardRequest{
                    {{writeOp->getNss(), shardVersion}},
                    {*writeOp},
                };
                batch.requestByShardId.emplace(shardVersion.shardName, std::move(shardRequest));
            }
            return WriteBatch{batch};
        } break;

        default: {
            MONGO_UNREACHABLE;
        }
    }
}

void OrderedWriteOpBatcher::markUnrecoverableError() {
    unrecoverableError = true;
}

namespace {
bool isNewBatchRequired(SimpleWriteBatch& writeBatch,
                        NamespaceString nss,
                        std::vector<ShardEndpoint>& shardsAffected) {
    for (const auto& shard : shardsAffected) {
        auto it = writeBatch.requestByShardId.find(shard.shardName);
        if (it != writeBatch.requestByShardId.end()) {
            auto versionFound = it->second.versionByNss.find(nss);
            // If the namespace is already in the batch, the shard version must be the same. If it's
            // not, we need a new batch.
            if (versionFound != it->second.versionByNss.end() && versionFound->second != shard) {
                return true;
            }
        }
    }
    return false;
}

void addSingleShardWriteOpToBatch(SimpleWriteBatch& writeBatch,
                                  WriteOp& writeOp,
                                  std::vector<ShardEndpoint>& shardsAffected) {
    const auto shard = shardsAffected.front();
    const auto shardName = shard.shardName;

    tassert(10387000,
            "Single shard write type should only target a single shard",
            shardsAffected.size() == 1);

    auto it = writeBatch.requestByShardId.find(shardName);
    if (it != writeBatch.requestByShardId.end()) {
        SimpleWriteBatch::ShardRequest& request = it->second;
        request.ops.push_back(writeOp);

        auto nss = writeOp.getNss();
        auto versionFound = request.versionByNss.find(nss);
        if (versionFound != request.versionByNss.end()) {
            tassert(10387001,
                    "Shard version for the same namespace need to be the same",
                    versionFound->second == shardsAffected.front());
        }
        request.versionByNss.emplace_hint(versionFound, nss, shardsAffected.front());
    } else {
        writeBatch.requestByShardId.emplace(
            shardName,
            SimpleWriteBatch::ShardRequest{std::map<NamespaceString, ShardEndpoint>{
                                               {writeOp.getNss(), shardsAffected.front()}},
                                           std::vector<WriteOp>{writeOp}});
    }
}

void addMultiShardWriteOpToBatch(SimpleWriteBatch& writeBatch,
                                 WriteOp& writeOp,
                                 std::vector<ShardEndpoint>& shardsAffected) {
    const auto shard = shardsAffected.front();
    const auto shardName = shard.shardName;

    for (const auto& shardEndpoint : shardsAffected) {
        std::vector<ShardEndpoint> shardEndpoints{shardEndpoint};
        addSingleShardWriteOpToBatch(writeBatch, writeOp, shardEndpoints);
    }
}

void addWriteOpToBatch(SimpleWriteBatch& writeBatch, WriteOp& writeOp, Analysis& analysis) {
    switch (analysis.type) {
        case kSingleShard:
            addSingleShardWriteOpToBatch(writeBatch, writeOp, analysis.shardsAffected);
            break;
        case kMultiShard:
            addMultiShardWriteOpToBatch(writeBatch, writeOp, analysis.shardsAffected);
            break;
        default:
            MONGO_UNREACHABLE;
    }
}
}  // namespace

boost::optional<WriteBatch> UnorderedWriteOpBatcher::getNextBatch(
    OperationContext* opCtx, const RoutingContext& routingCtx) {
    SimpleWriteBatch batch;
    while (true) {
        auto writeOp = _producer.peekNext();
        if (!writeOp) {
            // If there are no more operations, return the batch or boost::none.
            if (!batch.requestByShardId.empty()) {
                return WriteBatch{std::move(batch)};
            } else {
                return boost::none;
            }
        }
        auto analysis = _analyzer.analyze(opCtx, routingCtx, *writeOp);

        // A new batch is required if we're targeting a namespace with a shard endpoint that is
        // different to the one in the current batch.
        // TODO SERVER-104264: Revisit this logic once we account for
        // 'OnlyTargetDataOwningShardsForMultiWritesParam' cluster parameter.
        if (isNewBatchRequired(batch, writeOp->getNss(), analysis.shardsAffected)) {
            LOGV2_DEBUG(10387002,
                        4,
                        "New batch required as this namespace was already targeted with a "
                        "different shard version",
                        "nss"_attr = writeOp->getNss());
            return WriteBatch{std::move(batch)};
        }
        _producer.advance();
        addWriteOpToBatch(batch, *writeOp, analysis);
    }
}

}  // namespace unified_write_executor
}  // namespace mongo
