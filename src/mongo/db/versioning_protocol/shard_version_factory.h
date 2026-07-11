// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#pragma once

#include "mongo/db/global_catalog/chunk_manager.h"
#include "mongo/db/shard_role/shard_catalog/collection_metadata.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/db/versioning_protocol/chunk_version.h"
#include "mongo/db/versioning_protocol/shard_version.h"
#include "mongo/util/modules.h"

#include <boost/optional/optional.hpp>

namespace mongo {

/**
 * This class is used to build shard version objects.
 */
class [[MONGO_MOD_NEEDS_REPLACEMENT]] ShardVersionFactory {
public:
    static ShardVersion make(const ChunkManager& chunkManager);

    static ShardVersion make(const ChunkManager& chunkManager, const ShardId& shardId);

    static ShardVersion make(const CollectionMetadata& cm,
                             boost::optional<NamespaceString> nss = boost::none);

    // The other three builders should be used instead of this one whenever possible.
    static ShardVersion make(const ChunkVersion& chunkVersion);
};

}  // namespace mongo
