// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/query/compiler/rewrites/boolean_simplification/bitset_algebra.h"
#include "mongo/util/modules.h"

#include <iosfwd>

#include <boost/optional.hpp>

namespace mongo::boolean_simplification {
/**
 * This node represents an un-normalized boolean expression. A bitset tree contains predicates in
 * leaf nodes stored in bitsets and internal nodes represent the tree structure. Every internal node
 * might a conjunction (AND) or disjunction (OR) of its children.
 *
 * MQL operators are represented as:
 * - $and => BitsetTreeNode{type: And, isNegated: false}, children are not negated
 * - $or => BitsetTreeNode{type: Or, isNegated: false}, children are not negated
 * - $nor => BitsetTreeNode{type: Or, isNegated: true}, children are not negated
 * - $not => child is negated
 */
struct BitsetTreeNode {
    enum Type { Or, And };

    BitsetTreeNode(Type type, bool isNegated) : type(type), isNegated(isNegated), leafChildren(0) {}

    /**
     * Resize leafChildren to be the same size.
     */
    void ensureBitsetSize(size_t size);

    /**
     * Represents whether the node is conjunction (AND) or disjunction(OR) of its children.
     */
    Type type;

    /**
     * The node is negated if NOT operator applied to it.
     */
    bool isNegated;

    /**
     * Leaf nodes of the tree represented through bitsets.
     */
    BitsetTerm leafChildren;

    /**
     * Internal nodes: ANDs and ORs.
     */
    std::vector<BitsetTreeNode> internalChildren{};

    bool operator==(const BitsetTreeNode&) const = default;

    /**
     * Return total number of the terms and predicates.
     */
    size_t calculateSize() const {
        size_t result = 1 + leafChildren.mask.count();
        for (const auto& child : internalChildren) {
            result += child.calculateSize();
        }
        return result;
    }

    /**
     * Apply De Morgan's Law: recursively push down the tree this node's negation.
     */
    void applyDeMorgan() {
        applyDeMorganImpl(false);
    }

private:
    void applyDeMorganImpl(bool isParentNegated);
};

std::ostream& operator<<(std::ostream& os, const BitsetTreeNode& tree);

/**
 * Converts the given bitset tree into DNF. 'maximumNumberOfMinterms' specifies the limit on the
 * number of minterms during boolean trnsformations. The boost::none will be returned if the linit
 * is exceeded.
 */
boost::optional<Maxterm> convertToDNF(const BitsetTreeNode& node, size_t maximumNumberOfMinterms);

/**
 * Converts the given Maxterm into bitset tree.
 */
BitsetTreeNode convertToBitsetTree(const Maxterm& maxterm);
}  // namespace mongo::boolean_simplification
