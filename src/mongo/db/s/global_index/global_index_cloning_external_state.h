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

#pragma once

#include "mongo/db/s/global_index/global_index_cloning_service.h"

#include "mongo/s/chunk_manager.h"

namespace mongo {
namespace global_index {

/**
 * Interface for using functionality from other modules (mostly sharding) to allow mocking in tests.
 */
class GlobalIndexCloningService::CloningExternalState {
public:
    virtual ~CloningExternalState() = default;

    virtual ShardId myShardId(ServiceContext* service) const = 0;

    virtual ChunkManager getShardedCollectionRoutingInfo(OperationContext* opCtx,
                                                         const NamespaceString& nss) const = 0;
};

class GlobalIndexCloningStateImpl : public GlobalIndexCloningService::CloningExternalState {
public:
    ShardId myShardId(ServiceContext* service) const override;

    ChunkManager getShardedCollectionRoutingInfo(OperationContext* opCtx,
                                                 const NamespaceString& nss) const override;
};

}  // namespace global_index
}  // namespace mongo
