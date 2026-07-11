// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/versioning_protocol/shard_version_factory.h"

#include "mongo/util/testing_proctor.h"

#include <boost/optional/optional.hpp>

namespace mongo {

namespace {
// Returns the namespace if testing mode is enabled, otherwise returns boost::none.
// This is used to attach namespace information for validation in test environments.
boost::optional<NamespaceString> extractNssIfTesting(const boost::optional<NamespaceString>& nss) {
    const auto& proctor = TestingProctor::instance();
    return (proctor.isInitialized() && proctor.isEnabled() && nss) ? nss : boost::none;
}

// Extracts namespace from ChunkManager if it has a routing table and testing is enabled.
boost::optional<NamespaceString> extractNssIfTesting(const ChunkManager& chunkManager) {
    if (chunkManager.hasRoutingTable()) {
        return extractNssIfTesting(chunkManager.getNss());
    }
    return boost::none;
}
}  // namespace

ShardVersion ShardVersionFactory::make(const ChunkManager& chunkManager) {
    return ShardVersion(chunkManager.getVersion(), boost::none, extractNssIfTesting(chunkManager));
}

ShardVersion ShardVersionFactory::make(const ChunkManager& chunkManager, const ShardId& shardId) {
    return ShardVersion(
        chunkManager.getVersion(shardId), boost::none, extractNssIfTesting(chunkManager));
}

ShardVersion ShardVersionFactory::make(const CollectionMetadata& cm,
                                       boost::optional<NamespaceString> nss) {
    return ShardVersion(cm.getShardPlacementVersion(), boost::none, extractNssIfTesting(nss));
}


// The other three constructors should be used instead of this one whenever possible.
ShardVersion ShardVersionFactory::make(const ChunkVersion& chunkVersion) {
    return ShardVersion(chunkVersion, boost::none, boost::none);
}
}  // namespace mongo
