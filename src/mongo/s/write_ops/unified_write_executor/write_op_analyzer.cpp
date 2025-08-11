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

#include "mongo/s/write_ops/unified_write_executor/write_op_analyzer.h"

#include "mongo/s/collection_routing_info_targeter.h"

namespace mongo {
namespace unified_write_executor {

Analysis WriteOpAnalyzerImpl::analyze(OperationContext* opCtx,
                                      const RoutingContext& routingCtx,
                                      const WriteOp& op) {
    const auto& cri = routingCtx.getCollectionRoutingInfo(op.getNss());
    // TODO SERVER-103782 Don't use CRITargeter.
    CollectionRoutingInfoTargeter targeter(op.getNss(), cri);
    // TODO SERVER-103781 Add support for kPartialKeyWithId.
    // TODO SERVER-103146 Add kChangesOwnership.
    // TODO SERVER-103781 Add support for "WriteWithoutShardKeyWithId" writes.
    NSTargeter::TargetingResult tr;
    switch (op.getType()) {
        case WriteType::kInsert: {
            tr.endpoints.emplace_back(
                targeter.targetInsert(opCtx, op.getItemRef().getInsertOp().getDocument()));
        } break;
        case WriteType::kUpdate: {
            tr = targeter.targetUpdate(opCtx, op.getItemRef());
        } break;
        case WriteType::kDelete: {
            tr = targeter.targetDelete(opCtx, op.getItemRef());
        } break;
        case WriteType::kFindAndMod: {
            MONGO_UNIMPLEMENTED;
        } break;
        default: {
            MONGO_UNREACHABLE;
        } break;
    }

    size_t nsIdx = BulkWriteCRUDOp(op.getBulkWriteOp()).getNsInfoIdx();
    _stats.recordTargetingStats(tr.endpoints,
                                nsIdx,
                                targeter.isTargetedCollectionSharded(),
                                targeter.getAproxNShardsOwningChunks(),
                                op.getType());

    tassert(10346500, "Expected write to affect at least one shard", !tr.endpoints.empty());

    if (tr.useTwoPhaseWriteProtocol || tr.isNonTargetedRetryableWriteWithId) {
        return {BatchType::kNonTargetedWrite, std::move(tr.endpoints)};
    } else if (tr.endpoints.size() == 1) {
        return {BatchType::kSingleShard, std::move(tr.endpoints)};
    } else {
        return {BatchType::kMultiShard, std::move(tr.endpoints)};
    }
}

}  // namespace unified_write_executor
}  // namespace mongo
