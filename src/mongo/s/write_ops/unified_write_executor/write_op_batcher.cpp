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

namespace mongo {
namespace unified_write_executor {

boost::optional<WriteBatch> OrderedWriteOpBatcher::getNextBatch(OperationContext* opCtx,
                                                                RoutingContext& routingCtx) {
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

}  // namespace unified_write_executor
}  // namespace mongo
