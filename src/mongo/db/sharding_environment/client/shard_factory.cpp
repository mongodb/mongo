// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/db/sharding_environment/client/shard_factory.h"

#include "mongo/client/connection_string.h"
#include "mongo/util/assert_util.h"

#include <memory>
#include <utility>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding


namespace mongo {

ShardFactory::ShardFactory(BuildersMap&& builders,
                           std::unique_ptr<RemoteCommandTargeterFactory> targeterFactory)
    : _builders(builders), _targeterFactory(std::move(targeterFactory)) {}

std::unique_ptr<Shard> ShardFactory::createUniqueShard(const ShardHandle& handle,
                                                       const ConnectionString& connStr) {
    auto builderIt = _builders.find(connStr.type());
    invariant(builderIt != _builders.end());
    return builderIt->second(handle, connStr);
}

std::shared_ptr<Shard> ShardFactory::createShard(const ShardHandle& handle,
                                                 const ConnectionString& connStr) {
    auto builderIt = _builders.find(connStr.type());
    invariant(builderIt != _builders.end());
    return std::shared_ptr<Shard>(builderIt->second(handle, connStr));
}
}  // namespace mongo
