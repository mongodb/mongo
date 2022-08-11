/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/db/s/global_index/global_index_cloning_external_state.h"

#include "mongo/db/s/sharding_state.h"
#include "mongo/s/grid.h"

namespace mongo {
namespace global_index {

ShardId GlobalIndexCloningStateImpl::myShardId(ServiceContext* service) const {
    return ShardingState::get(service)->shardId();
}

ChunkManager GlobalIndexCloningStateImpl::getShardedCollectionRoutingInfo(
    OperationContext* opCtx, const NamespaceString& nss) const {
    auto catalogCache = Grid::get(opCtx)->catalogCache();
    return catalogCache->getShardedCollectionRoutingInfo(opCtx, nss);
}

}  // namespace global_index
}  // namespace mongo
