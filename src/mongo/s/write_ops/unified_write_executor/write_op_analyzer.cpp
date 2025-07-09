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

Analysis WriteOpAnalyzer::analyze(OperationContext* opCtx,
                                  const RoutingContext& routingCtx,
                                  const WriteOp& op) {
    auto cri = routingCtx.getCollectionRoutingInfo(op.getNss());
    // TODO SERVER-103782 Don't use CRITargeter.
    CollectionRoutingInfoTargeter targeter(op.getNss(), cri);
    // TODO SERVER-103780 Add support for kNoKey.
    // TODO SERVER-103781 Add support for kParitalKeyWithId.
    // TODO SERVER-103146 Add kChangesOwnership.
    std::vector<ShardEndpoint> shardsAffected = [&]() {
        switch (op.getType()) {
            case WriteType::kInsert: {
                return std::vector<ShardEndpoint>{
                    targeter.targetInsert(opCtx, op.getRef().getDocument())};
            }
            case WriteType::kUpdate: {
                return targeter.targetUpdate(opCtx, op.getRef()).endpoints;
            }
            case WriteType::kDelete: {
                return targeter.targetDelete(opCtx, op.getRef()).endpoints;
            }
            case WriteType::kFindAndMod:
                MONGO_UNIMPLEMENTED;
        }
        MONGO_UNREACHABLE;
    }();
    tassert(10346500, "Expected write to affect at least one shard", !shardsAffected.empty());
    if (shardsAffected.size() == 1) {
        return {BatchType::kSingleShard, std::move(shardsAffected)};
    } else {
        return {BatchType::kMultiShard, std::move(shardsAffected)};
    }
}

}  // namespace unified_write_executor
}  // namespace mongo
