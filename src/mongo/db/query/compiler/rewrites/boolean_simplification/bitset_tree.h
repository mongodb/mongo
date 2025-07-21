/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include "mongo/db/query/compiler/rewrites/boolean_simplification/bitset_algebra.h"

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
