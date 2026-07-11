// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/db/global_catalog/chunk_manager.h"
#include "mongo/db/global_catalog/shard_key_pattern.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/shard_role/shard_catalog/scoped_collection_metadata.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/util/modules.h"

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

/**
 * TODO (SPM-3971): Remove this class once resharding stops relying on the destinedRecipient oplog
 * field.
 */
class [[MONGO_MOD_NEEDS_REPLACEMENT]] ShardingWriteRouter {
public:
    ShardingWriteRouter(OperationContext* opCtx, const NamespaceString& nss);

    const boost::optional<ScopedCollectionDescription>& getCollDesc() const {
        return _collDesc;
    }

    boost::optional<ShardId> getReshardingDestinedRecipient(const BSONObj& fullDocument) const;

private:
    boost::optional<ScopedCollectionDescription> _collDesc;

    boost::optional<ScopedCollectionFilter> _ownershipFilter;

    boost::optional<ShardKeyPattern> _reshardingKeyPattern;
    boost::optional<ChunkManager> _reshardingChunkMgr;
};

}  // namespace mongo
