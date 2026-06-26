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

#pragma once

#include "mongo/db/operation_context.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/db/sharding_environment/shard_ref.h"
#include "mongo/db/sharding_environment/sharding_feature_flags_gen.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/util/modules.h"
#include "mongo/util/uuid.h"

#include <utility>

MONGO_MOD_PUBLIC;

namespace mongo {

/**
 * Data type that bundles a shard's ShardId with its UUID (if available),
 * supporting lookup operations through the ShardRegistry API.
 */
class MONGO_MOD_PUBLIC ShardHandle {
public:
    // Hardcoded identifier for the config server.
    static const ShardHandle kConfigServerHandle;

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

    bool operator<(const ShardHandle& other) const {
        if (_name != other._name)
            return _name < other._name;
        // Consistent with == operator -> if names are equal and one of them has no uuid, they are
        // equal.
        if (!_uuid.has_value() || !other._uuid.has_value())
            return false;
        return *_uuid < *other._uuid;
    }

    bool operator>(const ShardHandle& other) const {
        return other < *this;
    }

    bool operator<=(const ShardHandle& other) const {
        return !(other < *this);
    }

    bool operator>=(const ShardHandle& other) const {
        return !(*this < other);
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

using ShardRefToHandleMap = stdx::unordered_map<ShardRef, ShardHandle, ShardRef::Hasher>;

}  // namespace mongo
