// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/shard_role/shard_catalog/inbound_foreign_keys.h"

#include <absl/container/flat_hash_set.h>

namespace mongo {

InboundForeignKeyConstraints InboundForeignKeyMap::get(const UUID& parent) const {
    const auto* inbound = _map.find(parent);
    return inbound ? *inbound : InboundForeignKeyConstraints{};
}

std::vector<UUID> InboundForeignKeyMap::update(
    const UUID& child,
    const std::vector<ForeignKeyConstraint>& oldConstraints,
    const std::vector<ForeignKeyConstraint>& newConstraints) {
    std::vector<UUID> changedParents;
    auto reconcile = [&](const UUID& parent, bool referenced) {
        const auto* current = _map.find(parent);
        if (referenced == (current && current->children.count(child))) {
            return;
        }

        auto inbound = current ? *current : InboundForeignKeyConstraints{};
        inbound.children = referenced ? std::move(inbound.children).insert(child)
                                      : std::move(inbound.children).erase(child);
        _map = inbound.children.empty() ? _map.erase(parent) : _map.set(parent, std::move(inbound));
        changedParents.push_back(parent);
    };

    // A parent of 'oldConstraints' stays referenced only if 'newConstraints' also names it.
    // Registration and deregistration pass one empty list, so only a move between parents fills
    // this set.
    if (!oldConstraints.empty()) {
        absl::flat_hash_set<UUID, UUID::Hash> newParents;
        newParents.reserve(newConstraints.size());
        for (const auto& constraint : newConstraints) {
            newParents.insert(constraint.getReferences().getParentCollection());
        }
        for (const auto& constraint : oldConstraints) {
            const auto& parent = constraint.getReferences().getParentCollection();
            reconcile(parent, newParents.contains(parent));
        }
    }
    // Every parent of 'newConstraints' is referenced; a repeated parent already has the child.
    for (const auto& constraint : newConstraints) {
        reconcile(constraint.getReferences().getParentCollection(), /*referenced=*/true);
    }
    return changedParents;
}

}  // namespace mongo
