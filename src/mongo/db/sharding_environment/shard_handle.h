// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/util/modules.h"
#include "mongo/util/uuid.h"

#include <utility>

[[MONGO_MOD_PUBLIC]];

namespace mongo {

/**
 * Data type that bundles a shard's ShardId with its UUID (if available),
 * supporting lookup operations through the ShardRegistry API.
 */
class [[MONGO_MOD_PUBLIC]] ShardHandle {
public:
    ShardHandle(ShardId name, boost::optional<UUID> uuid)
        : _name(std::move(name)), _uuid(std::move(uuid)) {}

    const ShardId& name() const {
        return _name;
    }

    const boost::optional<UUID>& uuid() const {
        return _uuid;
    }

private:
    // The Shard ID.
    ShardId _name;
    // The Shard internal UUID. Declared as optional for backward compatibility.
    boost::optional<UUID> _uuid;
};

}  // namespace mongo
