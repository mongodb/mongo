/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/db/global_catalog/chunk_manager.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/db/topology/shard_registry.h"

// TODO (SERVER-126212): Remove this file once the chunk manager cannot contain a mix of shardId
// and shard UUIDs.

namespace mongo {
namespace chunk_manager_shard_resolver {
namespace {

using ChunkManagerShardResolver = std::function<ShardRefToHandleMap(OperationContext*)>;

const ServiceContext::Decoration<boost::optional<ChunkManagerShardResolver>>
    chunkManagerShardResolver_forTest =
        ServiceContext::declareDecoration<boost::optional<ChunkManagerShardResolver>>();

}  // namespace

ShardRefToHandleMap resolveShardHandlesForChunkManager(OperationContext* opCtx) {
    if (auto& resolver = chunkManagerShardResolver_forTest(opCtx->getServiceContext());
        MONGO_unlikely(static_cast<bool>(resolver))) {
        return (*resolver)(opCtx);
    }

    return Grid::get(opCtx)->shardRegistry()->getShardRefToHandleMap(opCtx);
}

void setChunkManagerShardResolver_forTest(OperationContext* opCtx,
                                          ShardRefToHandleMap shardRefToHandleMap) {
    chunkManagerShardResolver_forTest(opCtx->getServiceContext()) =
        [shardRefToHandleMap = std::move(shardRefToHandleMap)](OperationContext*) {
            return shardRefToHandleMap;
        };
}

void clearChunkManagerShardResolver_forTest(OperationContext* opCtx) {
    chunkManagerShardResolver_forTest(opCtx->getServiceContext()) = boost::none;
}

}  // namespace chunk_manager_shard_resolver
}  // namespace mongo
