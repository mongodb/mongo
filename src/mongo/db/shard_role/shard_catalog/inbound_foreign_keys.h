// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/shard_role/shard_catalog/foreign_key_constraint_gen.h"
#include "mongo/util/immutable/unordered_map.h"
#include "mongo/util/immutable/unordered_set.h"
#include "mongo/util/modules.h"
#include "mongo/util/uuid.h"

#include <vector>

namespace mongo {

struct [[MONGO_MOD_PUBLIC]] InboundForeignKeyConstraints {
    immutable::unordered_set<UUID, UUID::Hash> children;
};

/**
 * Reverse foreign key index: parent UUID -> UUIDs of the children that reference it. Copies are
 * cheap.
 */
class [[MONGO_MOD_PRIVATE]] InboundForeignKeyMap {
public:
    /**
     * Returns the inbound foreign key constraints of 'parent', or an empty set if no child
     * references it.
     */
    InboundForeignKeyConstraints get(const UUID& parent) const;

    /**
     * Moves 'child' from the parents referenced by 'oldConstraints' to the parents referenced by
     * 'newConstraints'. Returns the parents whose constraints changed.
     */
    std::vector<UUID> update(const UUID& child,
                             const std::vector<ForeignKeyConstraint>& oldConstraints,
                             const std::vector<ForeignKeyConstraint>& newConstraints);

private:
    immutable::unordered_map<UUID, InboundForeignKeyConstraints, UUID::Hash> _map;
};

}  // namespace mongo
