// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/stage_builder/sbe/sbe_stage_blueprint.h"

#include "mongo/util/assert_util.h"

#include <type_traits>

namespace mongo::stage_builder {
namespace {
SbBlueprintNodeIdx& childAt(SbBlueprintNodeIdx& onlyChild, size_t childIdx) {
    tassert(13778600, "Invalid blueprint child index", childIdx == 0);
    return onlyChild;
}

SbBlueprintNodeIdx& childAt(SbBlueprintNodeIdx& first,
                            SbBlueprintNodeIdx& second,
                            size_t childIdx) {
    tassert(13778601, "Invalid blueprint child index", childIdx < 2);
    return childIdx == 0 ? first : second;
}

SbBlueprintNodeIdx& childAt(SbBlueprintChildVector& children, size_t childIdx) {
    tassert(13778602, "Invalid blueprint child index", childIdx < children.size());
    return children[childIdx];
}

template <typename Node, typename... Leaves>
constexpr bool kIsOneOf = (std::is_same_v<Node, Leaves> || ...);

// The node types that have no children. SbBlueprintLowered is not one: it fills the place of a
// node that has been lowered, and asking for its children is an error.
template <typename Node>
constexpr bool kIsLeaf = kIsOneOf<Node,
                                  SbBlueprintCoScan,
                                  SbBlueprintVirtualScan,
                                  SbBlueprintScan,
                                  SbBlueprintMultiRangeScan,
                                  SbBlueprintSimpleIndexScan,
                                  SbBlueprintGenericIndexScan>;

/**
 * Returns the struct that holds the fields of 'n'. A large node type keeps them in a separately
 * allocated 'info' struct.
 */
template <typename Node>
auto& fieldsOf(Node& n) {
    if constexpr (requires { n.info; }) {
        return *n.info;
    } else {
        return n;
    }
}

template <typename Fields>
constexpr bool kHasOneChild = requires(Fields f) { f.child; };

template <typename Fields>
constexpr bool kHasTwoChildren = requires(Fields f) { f.outer; } ||
    requires(Fields f) { f.thenChild; } || requires(Fields f) { f.localStage; };

template <typename Fields>
constexpr bool kHasChildVector = requires(Fields f) { f.children; };

SbBlueprintNodeIdx& firstChild(auto& f) {
    if constexpr (requires { f.outer; }) {
        return f.outer;
    } else if constexpr (requires { f.thenChild; }) {
        return f.thenChild;
    } else {
        return f.localStage;
    }
}

SbBlueprintNodeIdx& secondChild(auto& f) {
    if constexpr (requires { f.inner; }) {
        return f.inner;
    } else if constexpr (requires { f.elseChild; }) {
        return f.elseChild;
    } else {
        return f.foreignStage;
    }
}

/**
 * Returns the number of children of 'n'.
 */
template <typename Node>
size_t childCount(Node& n) {
    auto& f = fieldsOf(n);
    using Fields = std::decay_t<decltype(f)>;
    if constexpr (std::is_same_v<Node, SbBlueprintLowered>) {
        tasserted(13778604, "Blueprint node has already been lowered");
    } else if constexpr (kHasOneChild<Fields>) {
        return 1;
    } else if constexpr (kHasTwoChildren<Fields>) {
        return 2;
    } else if constexpr (kHasChildVector<Fields>) {
        return f.children.size();
    } else {
        static_assert(kIsLeaf<Node>,
                      "Node type has children that numChildren() does not know how to count");
        return 0;
    }
}

/**
 * Returns the field of 'n' that holds its child at position 'childIdx'.
 */
template <typename Node>
SbBlueprintNodeIdx& fieldOfChild(Node& n, size_t childIdx) {
    auto& f = fieldsOf(n);
    using Fields = std::decay_t<decltype(f)>;
    if constexpr (std::is_same_v<Node, SbBlueprintLowered>) {
        tasserted(13778605, "Blueprint node has already been lowered");
    } else if constexpr (kHasOneChild<Fields>) {
        return childAt(f.child, childIdx);
    } else if constexpr (kHasTwoChildren<Fields>) {
        return childAt(firstChild(f), secondChild(f), childIdx);
    } else if constexpr (kHasChildVector<Fields>) {
        return childAt(f.children, childIdx);
    } else {
        // A node type that has children under a name not listed above fails to compile here.
        static_assert(kIsLeaf<Node>,
                      "Node type has children that childField() does not know how to find");
        tasserted(13778603, "Blueprint node has no children");
    }
}
}  // namespace

size_t numChildren(SbBlueprintNode& node) {
    return std::visit([](auto& n) { return childCount(n); }, node);
}

SbBlueprintNodeIdx* childField(SbBlueprintNode& node, size_t childIdx) {
    return std::visit([&](auto& n) { return &fieldOfChild(n, childIdx); }, node);
}
}  // namespace mongo::stage_builder
