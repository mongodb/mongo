// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/shard_role/shard_catalog/inbound_foreign_keys.h"

#include "mongo/unittest/unittest.h"

#include <algorithm>
#include <string>
#include <vector>

namespace mongo {
namespace {

// Returns one foreign key to each of 'parents'.
std::vector<ForeignKeyConstraint> constraintsTo(std::initializer_list<UUID> parents) {
    std::vector<ForeignKeyConstraint> constraints;
    for (const auto& parent : parents) {
        constraints.emplace_back("fk_" + std::to_string(constraints.size()),
                                 std::vector<std::string>{"parentId"},
                                 ForeignKeyConstraintReferences(parent, {"_id"}),
                                 ForeignKeyConstraintStateEnum::kUnvalidated,
                                 false);
    }
    return constraints;
}

// Asserts that 'changed' holds exactly 'parents', in any order.
void assertChanged(const std::vector<UUID>& changed, std::initializer_list<UUID> parents) {
    ASSERT_TRUE(
        std::is_permutation(changed.begin(), changed.end(), parents.begin(), parents.end()));
}

// Asserts that exactly 'children' reference 'parent'.
void assertChildren(const InboundForeignKeyMap& inboundForeignKeys,
                    const UUID& parent,
                    std::initializer_list<UUID> children) {
    const auto inbound = inboundForeignKeys.get(parent);
    ASSERT_EQ(inbound.children.size(), children.size());
    for (const auto& child : children) {
        ASSERT_TRUE(inbound.children.count(child));
    }
}

TEST(InboundForeignKeyMapTest, AddChildToParents) {
    const auto childUUID = UUID::gen();
    const auto otherChildUUID = UUID::gen();
    const auto sharedParentUUID = UUID::gen();
    const auto newParentUUID = UUID::gen();
    const auto constraints = constraintsTo({sharedParentUUID, newParentUUID});
    InboundForeignKeyMap inboundForeignKeys;
    inboundForeignKeys.update(
        otherChildUUID, /*oldConstraints=*/{}, constraintsTo({sharedParentUUID}));

    // Every new parent changes and keeps its other children.
    assertChanged(inboundForeignKeys.update(childUUID, /*oldConstraints=*/{}, constraints),
                  {sharedParentUUID, newParentUUID});
    assertChildren(inboundForeignKeys, sharedParentUUID, {otherChildUUID, childUUID});
    assertChildren(inboundForeignKeys, newParentUUID, {childUUID});
}

TEST(InboundForeignKeyMapTest, RemoveChildFromParents) {
    const auto childUUID = UUID::gen();
    const auto otherChildUUID = UUID::gen();
    const auto sharedParentUUID = UUID::gen();
    const auto onlyParentUUID = UUID::gen();
    const auto constraints = constraintsTo({sharedParentUUID, onlyParentUUID});
    InboundForeignKeyMap inboundForeignKeys;
    inboundForeignKeys.update(
        otherChildUUID, /*oldConstraints=*/{}, constraintsTo({sharedParentUUID}));
    inboundForeignKeys.update(childUUID, /*oldConstraints=*/{}, constraints);

    // Every old parent changes and keeps its other children.
    assertChanged(inboundForeignKeys.update(childUUID, constraints, /*newConstraints=*/{}),
                  {sharedParentUUID, onlyParentUUID});
    assertChildren(inboundForeignKeys, sharedParentUUID, {otherChildUUID});
    assertChildren(inboundForeignKeys, onlyParentUUID, {});
}

TEST(InboundForeignKeyMapTest, MoveChildBetweenParents) {
    const auto childUUID = UUID::gen();
    const auto removedUUID = UUID::gen();
    const auto keptUUID = UUID::gen();
    const auto addedUUID = UUID::gen();
    const auto before = constraintsTo({removedUUID, keptUUID});
    const auto after = constraintsTo({keptUUID, addedUUID});
    InboundForeignKeyMap inboundForeignKeys;
    inboundForeignKeys.update(childUUID, /*oldConstraints=*/{}, before);

    // Only the parents that the child stops or starts referencing change.
    assertChanged(inboundForeignKeys.update(childUUID, before, after), {removedUUID, addedUUID});
    assertChildren(inboundForeignKeys, removedUUID, {});
    assertChildren(inboundForeignKeys, keptUUID, {childUUID});
    assertChildren(inboundForeignKeys, addedUUID, {childUUID});
}

}  // namespace
}  // namespace mongo
