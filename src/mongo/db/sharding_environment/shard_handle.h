// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/operation_context.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/db/sharding_environment/shard_ref.h"
#include "mongo/db/sharding_environment/sharding_feature_flags_gen.h"
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
    ShardHandle() = default;

    ShardHandle(ShardId name, boost::optional<UUID> uuid)
        : _name(std::move(name)), _uuid(std::move(uuid)) {}

    const ShardId& name() const {
        return _name;
    }

    const boost::optional<UUID>& uuid() const {
        return _uuid;
    }

    ShardRef toShardRef(OperationContext* opCtx) const {
        // On mongoS, the FCV snapshot will always be latest, making the feature flag enabled on
        // binary 9.0. Since mongoS binaries are changed first during a downgrade, the shards will
        // still be able to handle a mongoS using a uuid. We don't have the same guarantee on mongod
        // so we default to last LTS.
        if (uuid() &&
            feature_flags::gFeatureFlagUniqueShardIdentifiers
                .isEnabledUseLastLTSFCVWhenUninitialized(
                    VersionContext::getDecoration(opCtx),
                    serverGlobalParams.featureCompatibility.acquireFCVSnapshot())) {
            return ShardRef(*uuid());
        } else {
            return ShardRef(name());
        }
    }

    bool operator==(const ShardHandle& other) const {
        // If one shard handle is missing a UUID, it compares as equal to another shard handle with
        // a UUID to allow for comparisons across FCV upgrade/downgrade.
        return _name == other._name &&
            (_uuid == other._uuid || !_uuid.has_value() || !other._uuid.has_value());
    }

    template <typename H>
    friend H AbslHashValue(H h, const ShardHandle& handle) {
        return H::combine(std::move(h), handle._name);
    }

    /**
     * Custom hasher so ShardHandles can be used in unordered data structures.
     * Hashes only _name to stay consistent with operator==, which treats handles with the same
     * name as equal regardless of whether either is missing a UUID.
     * Usage: std::unordered_set<ShardHandle, ShardHandle::HashByName> shardHandleSet;
     */
    struct HashByName {
        std::size_t operator()(const ShardHandle& handle) const {
            return ShardId::Hasher{}(handle._name);
        }
    };

private:
    // The Shard ID.
    ShardId _name;
    // The Shard internal UUID. Declared as optional for backward compatibility.
    boost::optional<UUID> _uuid;
};

}  // namespace mongo
