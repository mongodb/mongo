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
