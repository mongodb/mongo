// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/client/connection_string.h"
#include "mongo/client/remote_command_targeter_factory.h"
#include "mongo/db/sharding_environment/client/shard.h"
#include "mongo/util/modules.h"

#include <functional>
#include <map>
#include <memory>
#include <string>

namespace mongo {

/**
 * An object factory for creating Shard instances via calling registered builders.
 */
class [[MONGO_MOD_PUBLIC]] ShardFactory {
    ShardFactory(const ShardFactory&) = delete;
    ShardFactory& operator=(const ShardFactory&) = delete;

public:
    using BuilderCallable =
        std::function<std::unique_ptr<Shard>(const ShardHandle&, const ConnectionString&)>;
    using BuildersMap = std::map<ConnectionString::ConnectionType, BuilderCallable>;

    ShardFactory(BuildersMap&&, std::unique_ptr<RemoteCommandTargeterFactory>);
    ~ShardFactory() = default;

    /**
     * Deprecated. Creates a unique_ptr with a new instance of a Shard with the provided handle
     * and connection string. This method is currently only used for addShard.
     */
    std::unique_ptr<Shard> createUniqueShard(const ShardHandle& handle,
                                             const ConnectionString& connStr);

    /**
     * Creates a shared_ptr with a new instance of a Shard with the provided handle and connection
     * string.
     */
    std::shared_ptr<Shard> createShard(const ShardHandle& handle, const ConnectionString& connStr);

private:
    // Map from ConnectionType to a function that can be used to build a Shard object for that
    // ConnectionType. This map must be set up at the initialization of the ShardFactory instance.
    BuildersMap _builders;

    // Even though ShardFactory doesn't use _targeterFactory directly, the functions contained in
    // _builders may, so ShardFactory must own _targeterFactory so that their lifetimes are tied
    // together
    std::unique_ptr<RemoteCommandTargeterFactory> _targeterFactory;
};

}  // namespace mongo
