// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/compiler/rewrites/boolean_simplification/bitset_tree.h"

#include "mongo/db/query/compiler/rewrites/boolean_simplification/bitset_test_util.h"
#include "mongo/unittest/unittest.h"

namespace mongo::boolean_simplification {
TEST(ConvertToBitsetTreeTests, AlwaysTrue) {
    Maxterm maxterm{Minterm{0}};

    BitsetTreeNode expectedTree{BitsetTreeNode::And, false};

    auto tree = convertToBitsetTree(maxterm);
    ASSERT_EQ(expectedTree, tree);
}

TEST(ConvertToBitsetTreeTests, AlwaysFalse) {
    Maxterm maxterm{0};

    BitsetTreeNode expectedTree{BitsetTreeNode::Or, false};

    auto tree = convertToBitsetTree(maxterm);
    ASSERT_EQ(expectedTree, tree);
}

TEST(ConvertToBitsetTreeTests, GtExpression) {
    Maxterm maxterm{
        {"1", "1"},
    };

    BitsetTreeNode expectedTree{BitsetTreeNode::And, false};
    expectedTree.leafChildren = makeBitsetTerm(maxterm.minterms.front());

    auto tree = convertToBitsetTree(maxterm);
    ASSERT_EQ(expectedTree, tree);
}

TEST(ConvertToBitsetTreeTests, AndExpression) {
    Maxterm maxterm{
        {"01", "11"},
    };

    BitsetTreeNode expectedTree{BitsetTreeNode::And, false};
    expectedTree.leafChildren = makeBitsetTerm(maxterm.minterms.front());

    auto tree = convertToBitsetTree(maxterm);
    ASSERT_EQ(expectedTree, tree);
}

TEST(ConvertToBitsetTreeTests, OrExpression) {
    Maxterm maxterm{
        {"001", "011"},
        {"111", "111"},
    };

    BitsetTreeNode expectedTree{BitsetTreeNode::Or, false};
    {
        BitsetTreeNode orOperand{BitsetTreeNode::And, false};
        orOperand.leafChildren = makeBitsetTerm(maxterm.minterms[0]);
        expectedTree.internalChildren.emplace_back(std::move(orOperand));
    }
    {
        BitsetTreeNode orOperand{BitsetTreeNode::And, false};
        orOperand.leafChildren = makeBitsetTerm(maxterm.minterms[1]);
        expectedTree.internalChildren.emplace_back(std::move(orOperand));
    }

    auto tree = convertToBitsetTree(maxterm);
    ASSERT_EQ(expectedTree, tree);
}

TEST(ConvertToBitsetTreeTests, NorExpression) {
    Maxterm maxterm{{
        Minterm{"100", "101"},
        Minterm{"000", "011"},
        Minterm{"000", "101"},
        Minterm{"010", "011"},
        Minterm{"010", "110"},
    }};

    BitsetTreeNode expectedTree{BitsetTreeNode::Or, false};
    for (const auto& minterm : maxterm.minterms) {
        BitsetTreeNode orOperand{BitsetTreeNode::And, false};
        orOperand.leafChildren = makeBitsetTerm(minterm);
        expectedTree.internalChildren.emplace_back(std::move(orOperand));
    }

    auto tree = convertToBitsetTree(maxterm);
    ASSERT_EQ(expectedTree, tree);
}

// {a: $elemMatch: {$gt: 5, $eq: 10, $lt: 10}}
TEST(ConvertToBitsetTreeTests, ElemMatch) {
    Maxterm maxterm{
        Minterm{"1", "1"},
    };

    BitsetTreeNode expectedTree{BitsetTreeNode::And, false};
    expectedTree.leafChildren = makeBitsetTerm("1", "1");

    auto tree = convertToBitsetTree(maxterm);
    ASSERT_EQ(expectedTree, tree);
}

// {a: $elemMatch: {b: {$gt: 5, $eq: 10, $lt: 10}}}
TEST(ConvertToBitsetTreeTests, ElemMatchObject) {
    Maxterm maxterm{
        Minterm{"1", "1"},
    };

    BitsetTreeNode expectedTree{BitsetTreeNode::And, false};
    expectedTree.leafChildren = makeBitsetTerm("1", "1");

    auto tree = convertToBitsetTree(maxterm);
    ASSERT_EQ(expectedTree, tree);
}

// {$and: [{a: {$elemMatch: {$not: {$gt: 21}}}}, {a: {$not: {$elemMatch: {$lt: 21}}}}]}
TEST(ConvertToBitsetTreeTests, TwoElemMatches) {
    Maxterm maxterm{
        Minterm{"01", "11"},
    };

    BitsetTreeNode expectedTree{BitsetTreeNode::And, false};
    expectedTree.leafChildren = makeBitsetTerm("01", "11");

    auto tree = convertToBitsetTree(maxterm);
    ASSERT_EQ(expectedTree, tree);
}

TEST(BitsetTreeTests, ApplyDeMorgan) {
    BitsetTreeNode root(BitsetTreeNode::Or, true);  // rooted $nor
    root.leafChildren = makeBitsetTerm("0010", "1010");
    root.internalChildren.emplace_back(BitsetTreeNode::Or, true);  // nested $nor
    root.internalChildren.back().leafChildren = makeBitsetTerm("0010", "0111");
    root.internalChildren.emplace_back(BitsetTreeNode::Or, false);
    root.internalChildren.back().leafChildren = makeBitsetTerm("0010", "0111");
    root.internalChildren.emplace_back(BitsetTreeNode::And, false);
    root.internalChildren.back().leafChildren = makeBitsetTerm("0010", "0011");

    BitsetTreeNode expectedRoot(BitsetTreeNode::And, false);
    expectedRoot.leafChildren = makeBitsetTerm("1000", "1010");
    expectedRoot.internalChildren.emplace_back(BitsetTreeNode::Or, false);
    expectedRoot.internalChildren.back().leafChildren = makeBitsetTerm("0010", "0111");
    expectedRoot.internalChildren.emplace_back(BitsetTreeNode::And, false);
    expectedRoot.internalChildren.back().leafChildren = makeBitsetTerm("0101", "0111");
    expectedRoot.internalChildren.emplace_back(BitsetTreeNode::Or, false);
    expectedRoot.internalChildren.back().leafChildren = makeBitsetTerm("0001", "0011");

    root.applyDeMorgan();
    ASSERT_EQ(expectedRoot, root);
}
}  // namespace mongo::boolean_simplification
