// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/compiler/rewrites/boolean_simplification/bitset_tree.h"

#include "mongo/util/assert_util.h"

#include <string_view>

namespace mongo::boolean_simplification {
namespace {
boost::optional<Maxterm> convertToDNF(const BitsetTreeNode& node,
                                      size_t maximumNumberOfMinterms,
                                      bool isNegated);

Minterm makeMintermFromConjunction(const BitsetTerm& conjunctionTerm, bool isNegated) {
    Minterm result{conjunctionTerm.predicates, conjunctionTerm.mask};
    if (isNegated) {
        result.flip();
    }
    return result;
}

Maxterm makeMaxtermFromDisjunction(const BitsetTerm& disjunctionTerm, bool isNegated) {
    Maxterm result{disjunctionTerm.size()};
    for (size_t pos = 0; pos < disjunctionTerm.mask.size(); ++pos) {
        if (disjunctionTerm.mask[pos]) {
            result.append(pos, disjunctionTerm.predicates[pos] ^ isNegated);
        }
    }

    return result;
}

/**
 * Append children of AND term to the given Maxterm.
 */
bool appendAndTerm(const std::vector<BitsetTreeNode>& children,
                   Maxterm& maxterm,
                   size_t maximumNumberOfMinterms,
                   bool isNegated) {
    for (const auto& child : children) {
        auto dnfChild = convertToDNF(child, maximumNumberOfMinterms, isNegated);
        if (!dnfChild ||
            maxterm.minterms.size() * dnfChild->minterms.size() > maximumNumberOfMinterms) {
            return false;
        }

        maxterm &= *dnfChild;
    }

    return true;
}

/**
 * Append children of OR term to the given Maxterm.
 */
bool appendOrTerm(const std::vector<BitsetTreeNode>& children,
                  Maxterm& maxterm,
                  size_t maximumNumberOfMinterms,
                  bool isNegated) {
    for (const auto& child : children) {
        auto dnfChild = convertToDNF(child, maximumNumberOfMinterms, isNegated);
        if (!dnfChild ||
            maxterm.minterms.size() + dnfChild->minterms.size() > maximumNumberOfMinterms) {
            return false;
        }
        maxterm |= *dnfChild;
    }

    return true;
}

boost::optional<Maxterm> convertToDNF(const BitsetTreeNode& node,
                                      size_t maximumNumberOfMinterms,
                                      bool isNegated) {
    isNegated = node.isNegated ^ isNegated;
    BitsetTreeNode::Type nodeType;
    switch (node.type) {
        case BitsetTreeNode::And:
            nodeType = isNegated ? BitsetTreeNode::Or : BitsetTreeNode::And;
            break;
        case BitsetTreeNode::Or:
            nodeType = isNegated ? BitsetTreeNode::And : BitsetTreeNode::Or;
            break;
        default:
            MONGO_UNREACHABLE_TASSERT(8163010);
    }

    Maxterm result{node.leafChildren.size()};

    switch (nodeType) {
        case BitsetTreeNode::And:
            result.minterms.emplace_back(makeMintermFromConjunction(node.leafChildren, isNegated));
            if (!appendAndTerm(node.internalChildren, result, maximumNumberOfMinterms, isNegated)) {
                return boost::none;
            }
            break;
        case BitsetTreeNode::Or:
            result = makeMaxtermFromDisjunction(node.leafChildren, isNegated);
            if (!appendOrTerm(node.internalChildren, result, maximumNumberOfMinterms, isNegated)) {
                return boost::none;
            }
            break;
    };

    return result;
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

void BitsetTreeNode::applyDeMorganImpl(bool isParentNegated) {
    const bool isThisNegated = isNegated ^ isParentNegated;
    isNegated = false;

    if (isThisNegated) {
        switch (type) {
            case BitsetTreeNode::And:
                type = BitsetTreeNode::Or;
                break;
            case BitsetTreeNode::Or:
                type = BitsetTreeNode::And;
                break;
            default:
                MONGO_UNREACHABLE_TASSERT(8316200);
        }

        leafChildren.flip();
    }

    for (auto& child : internalChildren) {
        child.applyDeMorganImpl(isThisNegated);
    }
}

boost::optional<Maxterm> convertToDNF(const BitsetTreeNode& node, size_t maximumNumberOfMinterms) {
    return convertToDNF(node, maximumNumberOfMinterms, /* isNegated */ false);
}

BitsetTreeNode convertToBitsetTree(const Maxterm& maxterm) {
    if (maxterm.minterms.size() == 1) {
        return restoreBitsetTree(maxterm.minterms.front());
    } else {
        BitsetTreeNode node{BitsetTreeNode::Or, false};
        for (const auto& minterm : maxterm.minterms) {
            if (minterm.mask.count() == 1) {
                const size_t bitIndex = minterm.mask.findFirst();
                node.leafChildren.set(bitIndex, minterm.predicates[bitIndex]);
            } else {
                node.internalChildren.emplace_back(restoreBitsetTree(minterm));
            }
        }
        return node;
    }
}

std::ostream& operator<<(std::ostream& os, const BitsetTreeNode& tree) {
    os << tree.type << ":" << tree.isNegated << "--" << tree.leafChildren << " ";
    os << "[";
    std::string_view sep;
    for (auto&& node : tree.internalChildren)
        os << std::exchange(sep, ", ") << node;
    os << "]";
    return os;
}
}  // namespace mongo::boolean_simplification
