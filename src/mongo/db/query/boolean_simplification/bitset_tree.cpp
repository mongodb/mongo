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

#include "mongo/db/query/boolean_simplification/bitset_tree.h"

#include "mongo/util/assert_util.h"
#include "mongo/util/stream_utils.h"

namespace mongo::boolean_simplification {
namespace {
Minterm makeMintermFromConjunction(const BitsetTerm& conjunctionTerm) {
    return Minterm{conjunctionTerm.predicates, conjunctionTerm.mask};
}

Maxterm makeMaxtermFromDisjunction(const BitsetTerm& disjunctionTerm) {
    Maxterm result{disjunctionTerm.size()};
    for (size_t pos = disjunctionTerm.mask.find_first(); pos < disjunctionTerm.mask.size();
         pos = disjunctionTerm.mask.find_next(pos)) {
        result.append(pos, disjunctionTerm.predicates[pos]);
    }

    return result;
}

/**
 * Append children of AND term to the given Maxterm.
 */
void appendAndTerm(const std::vector<BitsetTreeNode>& children, Maxterm& maxterm) {
    for (const auto& child : children) {
        maxterm &= convertToDNF(child);
    }
}

/**
 * Append children of OR term to the given Maxterm.
 */
void appendOrTerm(const std::vector<BitsetTreeNode>& children, Maxterm& maxterm) {
    for (const auto& child : children) {
        maxterm |= convertToDNF(child);
    }
}

std::ostream& operator<<(std::ostream& os, const BitsetTreeNode::Type& type) {
    switch (type) {
        case BitsetTreeNode::Type::And:
            os << "and";
            break;
        case BitsetTreeNode::Type::Or:
            os << "or";
            break;
    }
    return os;
}

/**
 * Restore BitsetTree from a minterm.
 */
BitsetTreeNode restoreBitsetTree(const Minterm& minterm) {
    BitsetTreeNode node{BitsetTreeNode::And, false};
    node.leafChildren = BitsetTerm{minterm.predicates, minterm.mask};
    return node;
}
}  // namespace

void BitsetTreeNode::ensureBitsetSize(size_t size) {
    leafChildren.resize(size);
    for (auto& child : internalChildren) {
        child.ensureBitsetSize(size);
    }
}

Maxterm convertToDNF(const BitsetTreeNode& node) {
    Maxterm result{node.leafChildren.size()};

    switch (node.type) {
        case BitsetTreeNode::And:
            result.minterms.emplace_back(makeMintermFromConjunction(node.leafChildren));
            appendAndTerm(node.internalChildren, result);
            break;
        case BitsetTreeNode::Or:
            result = makeMaxtermFromDisjunction(node.leafChildren);
            appendOrTerm(node.internalChildren, result);
            break;
    };

    return node.isNegated ? ~result : result;
}

BitsetTreeNode convertToBitsetTree(const Maxterm& maxterm) {
    if (maxterm.minterms.size() == 1) {
        return restoreBitsetTree(maxterm.minterms.front());
    } else {
        BitsetTreeNode node{BitsetTreeNode::Or, false};
        for (const auto& minterm : maxterm.minterms) {
            if (minterm.mask.count() == 1) {
                const size_t bitIndex = minterm.mask.find_first();
                node.leafChildren.set(bitIndex, minterm.predicates[bitIndex]);
            } else {
                node.internalChildren.emplace_back(restoreBitsetTree(minterm));
            }
        }
        return node;
    }
}

std::ostream& operator<<(std::ostream& os, const BitsetTreeNode& tree) {
    using mongo::operator<<;
    os << tree.type << ":" << tree.isNegated << "--" << tree.leafChildren << " "
       << tree.internalChildren;
    return os;
}
}  // namespace mongo::boolean_simplification
