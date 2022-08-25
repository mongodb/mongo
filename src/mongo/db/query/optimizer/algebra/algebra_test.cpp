/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/db/query/optimizer/algebra/operator.h"
#include "mongo/db/query/optimizer/algebra/polyvalue.h"
#include "mongo/unittest/unittest.h"

namespace mongo::optimizer::algebra {

namespace {

class Leaf;
class BinaryNode;
class NaryNode;
class AtLeastBinaryNode;
using Tree = PolyValue<Leaf, BinaryNode, NaryNode, AtLeastBinaryNode>;

/**
 * A leaf in the tree. Just contains data - in this case a double.
 */
class Leaf : public OpSpecificArity<Tree, 0> {
public:
    Leaf(double x) : x(x) {}

    double x;
};

/**
 * An inner node in the tree with exactly two children.
 */
class BinaryNode : public OpSpecificArity<Tree, 2> {
public:
    BinaryNode(Tree left, Tree right)
        : OpSpecificArity<Tree, 2>(std::move(left), std::move(right)) {}
};

/**
 * An inner node in the tree with any number of children, zero or greater.
 */
class NaryNode : public OpSpecificDynamicArity<Tree, 0> {
public:
    NaryNode(std::vector<Tree> children) : OpSpecificDynamicArity<Tree, 0>(std::move(children)) {}
};

/**
 * An inner node in the tree with 2 or more nodes.
 */
class AtLeastBinaryNode : public OpSpecificDynamicArity<Tree, 2> {
public:
    /**
     * Notice the required number of nodes are given as separate arguments from the vector.
     */
    AtLeastBinaryNode(std::vector<Tree> children, Tree left, Tree right)
        : OpSpecificDynamicArity<Tree, 2>(std::move(children), std::move(left), std::move(right)) {}
};

/**
 * A visitor of the tree with methods to visit each kind of node.
 *
 * This is a very basic visitor to just demonstrate the transport() API - all it does is sum up
 * doubles in the leaf nodes of the tree.
 *
 * Notice that each kind of node did not need to fill out some boilerplate "visit()" method or
 * anything like that. The PolyValue templating magic took care of all the boilerplate for us, and
 * the operator classes (e.g. OpSpecificArity) exposes the tree structure and children.
 */
class NodeTransporter {
public:
    double transport(Leaf& leaf) {
        return leaf.x;
    }
    double transport(BinaryNode& node, double child0, double child1) {
        return child0 + child1;
    }
    double transport(NaryNode& node, std::vector<double> children) {
        return std::accumulate(children.begin(), children.end(), 0.0);
    }
    double transport(AtLeastBinaryNode& node,
                     std::vector<double> children,
                     double child0,
                     double child1) {
        return child0 + child1 + std::accumulate(children.begin(), children.end(), 0.0);
    }
};

/**
 * A visitor of the tree with methods to visit each kind of node. This visitor also takes a
 * reference to the Tree itself. Unused here, this reference can be used to mutate or replace the
 * node itself while the walking takes place.
 */
class TreeTransporter {
public:
    double transport(Tree& tree, Leaf& leaf) {
        return leaf.x;
    }
    double transport(Tree& tree, BinaryNode& node, double child0, double child1) {
        return child0 + child1;
    }
    double transport(Tree& tree, NaryNode& node, std::vector<double> children) {
        return std::accumulate(children.begin(), children.end(), 0.0);
    }
    double transport(Tree& tree,
                     AtLeastBinaryNode& node,
                     std::vector<double> children,
                     double child0,
                     double child1) {
        return child0 + child1 + std::accumulate(children.begin(), children.end(), 0.0);
    }
};

TEST(PolyValueTest, SumTransportFixedArity) {
    NodeTransporter nodeTransporter;
    TreeTransporter treeTransporter;
    {
        Tree simple = Tree::make<BinaryNode>(Tree::make<Leaf>(2.0), Tree::make<Leaf>(1.0));
        // Notice the template parameter true or false matches whether the walker expects to have a
        // Tree& parameter first in the transport implementations.
        double result = transport<false>(simple, nodeTransporter);
        ASSERT_EQ(result, 3.0);
        // This 'true' template means we expect the 'Tree&' argument to come first in all the
        // 'transport()' implementations.
        result = transport<true>(simple, treeTransporter);
        ASSERT_EQ(result, 3.0);
    }

    {
        Tree deeper = Tree::make<BinaryNode>(
            Tree::make<BinaryNode>(Tree::make<Leaf>(2.0), Tree::make<Leaf>(1.0)),
            Tree::make<BinaryNode>(Tree::make<Leaf>(2.0), Tree::make<Leaf>(1.0)));
        double result = transport<false>(deeper, nodeTransporter);
        ASSERT_EQ(result, 6.0);
        result = transport<true>(deeper, treeTransporter);
        ASSERT_EQ(result, 6.0);
    }
}

/**
 * Prove out that the walking/visiting can hit the variadic NaryNode.
 */
TEST(PolyValueTest, SumTransportVariadic) {
    NodeTransporter nodeTransporter;
    TreeTransporter treeTransporter;
    Tree naryDemoTree = Tree::make<NaryNode>(
        std::vector<Tree>{Tree::make<Leaf>(6.0),
                          Tree::make<Leaf>(5.0),
                          Tree::make<NaryNode>(std::vector<Tree>{
                              Tree::make<Leaf>(4.0), Tree::make<Leaf>(3.0), Tree::make<Leaf>(2.0)}),
                          Tree::make<Leaf>(1.0)});

    double result = transport<false>(naryDemoTree, nodeTransporter);
    ASSERT_EQ(result, 21.0);
    result = transport<true>(naryDemoTree, treeTransporter);
    ASSERT_EQ(result, 21.0);
}

TEST(PolyValueTest, SumTransportAtLeast2Children) {
    NodeTransporter nodeTransporter;
    TreeTransporter treeTransporter;
    Tree demoTree = Tree::make<AtLeastBinaryNode>(
        std::vector<Tree>{Tree::make<Leaf>(7.0), Tree::make<Leaf>(6.0)},
        Tree::make<Leaf>(5.0),
        Tree::make<AtLeastBinaryNode>(
            std::vector<Tree>{Tree::make<Leaf>(4.0), Tree::make<Leaf>(3.0)},
            Tree::make<Leaf>(2.0),
            Tree::make<Leaf>(1.0)));
    double result = transport<false>(demoTree, nodeTransporter);
    ASSERT_EQ(result, 28.0);
    result = transport<true>(demoTree, treeTransporter);
    ASSERT_EQ(result, 28.0);
}

/**
 * A visitor of the tree like those above but which takes const references so is forbidden from
 * modifying the tree or nodes.
 *
 * This visitor creates a copy of the tree but with the values at the leaves doubled.
 */
class ConstTransporterCopyAndDouble {
public:
    Tree transport(const Leaf& leaf) {
        return Tree::make<Leaf>(2 * leaf.x);
    }
    Tree transport(const BinaryNode& node, Tree child0, Tree child1) {
        return Tree::make<BinaryNode>(std::move(child0), std::move(child1));
    }
    Tree transport(const NaryNode& node, std::vector<Tree> children) {
        return Tree::make<NaryNode>(std::move(children));
    }
    Tree transport(const AtLeastBinaryNode& node,
                   std::vector<Tree> children,
                   Tree child0,
                   Tree child1) {
        return Tree::make<AtLeastBinaryNode>(
            std::move(children), std::move(child0), std::move(child1));
    }

    // Add all the same walkers with the optional 'tree' argument. Note this is also const.
    Tree transport(const Tree& tree, const Leaf& leaf) {
        return Tree::make<Leaf>(2 * leaf.x);
    }
    Tree transport(const Tree& tree, const BinaryNode& node, Tree child0, Tree child1) {
        return Tree::make<BinaryNode>(std::move(child0), std::move(child1));
    }
    Tree transport(const Tree& tree, const NaryNode& node, std::vector<Tree> children) {
        return Tree::make<NaryNode>(std::move(children));
    }
    Tree transport(const Tree& tree,
                   const AtLeastBinaryNode& node,
                   std::vector<Tree> children,
                   Tree child0,
                   Tree child1) {
        return Tree::make<AtLeastBinaryNode>(
            std::move(children), std::move(child0), std::move(child1));
    }
};

TEST(PolyValueTest, CopyAndDoubleTreeConst) {
    // Test that we can create a copy of a tree and walk with a const transporter to provide extra
    // proof that it's actually a deep copy.
    ConstTransporterCopyAndDouble transporter;
    {
        const Tree simple = Tree::make<BinaryNode>(Tree::make<Leaf>(2.0), Tree::make<Leaf>(1.0));
        // Notice 'simple' is const.
        Tree result = transport<false>(simple, transporter);
        BinaryNode* newRoot = result.cast<BinaryNode>();
        ASSERT(newRoot);
        Leaf* newLeafLeft = newRoot->get<0>().cast<Leaf>();
        ASSERT(newLeafLeft);
        ASSERT_EQ(newLeafLeft->x, 4.0);

        Leaf* newLeafRight = newRoot->get<1>().cast<Leaf>();
        ASSERT(newLeafRight);
        ASSERT_EQ(newLeafRight->x, 2.0);
    }
    {
        // Do the same test but walk with the tree reference (pass 'true' to transport).
        const Tree simple = Tree::make<BinaryNode>(Tree::make<Leaf>(2.0), Tree::make<Leaf>(1.0));
        // Notice 'simple' is const.
        Tree result = transport<true>(simple, transporter);
        BinaryNode* newRoot = result.cast<BinaryNode>();
        ASSERT(newRoot);
        Leaf* newLeafLeft = newRoot->get<0>().cast<Leaf>();
        ASSERT(newLeafLeft);
        ASSERT_EQ(newLeafLeft->x, 4.0);

        Leaf* newLeafRight = newRoot->get<1>().cast<Leaf>();
        ASSERT(newLeafRight);
        ASSERT_EQ(newLeafRight->x, 2.0);
    }
}

/**
 * A walker which accumulates all nodes into a std::set to demonstrate which nodes are visited.
 *
 * The order of the visitation is not guaranteed, except that we visit "bottom-up" so leaves must
 * happen before parents. This much must be true since the API to visit a node depends on the
 * results of its children being pre-computed.
 */
class AccumulateToSetTransporter {
public:
    std::set<double> transport(Leaf& leaf) {
        return {leaf.x};
    }

    std::set<double> transport(BinaryNode& node,
                               std::set<double> visitedChild0,
                               std::set<double> visitedChild1) {
        // 'visistedChild0' and 'visitedChild1' represent the accumulated results of their visited
        // numbers. Here we just merge the two.
        std::set<double> merged;
        std::merge(visitedChild0.begin(),
                   visitedChild0.end(),
                   visitedChild1.begin(),
                   visitedChild1.end(),
                   std::inserter(merged, merged.begin()));
        return merged;
    }

    std::set<double> transport(NaryNode& node, std::vector<std::set<double>> childrenVisitedSets) {
        return std::accumulate(childrenVisitedSets.begin(),
                               childrenVisitedSets.end(),
                               std::set<double>{},
                               [](auto&& visited1, auto&& visited2) {
                                   std::set<double> merged;
                                   std::merge(visited1.begin(),
                                              visited1.end(),
                                              visited2.begin(),
                                              visited2.end(),
                                              std::inserter(merged, merged.begin()));
                                   return merged;
                               });
    }

    std::set<double> transport(AtLeastBinaryNode& node,
                               std::vector<std::set<double>> childrenVisitedSets,
                               std::set<double> visitedChild0,
                               std::set<double> visitedChild1) {
        std::set<double> merged;
        std::merge(visitedChild0.begin(),
                   visitedChild0.end(),
                   visitedChild1.begin(),
                   visitedChild1.end(),
                   std::inserter(merged, merged.begin()));

        return std::accumulate(childrenVisitedSets.begin(),
                               childrenVisitedSets.end(),
                               merged,
                               [](auto&& visited1, auto&& visited2) {
                                   std::set<double> merged;
                                   std::merge(visited1.begin(),
                                              visited1.end(),
                                              visited2.begin(),
                                              visited2.end(),
                                              std::inserter(merged, merged.begin()));
                                   return merged;
                               });
    }
};

/**
 * Here we see a test which walks all the various types of nodes at once, and in this case
 * accumulates into a std::set any visited leaves.
 */
TEST(PolyValueTest, AccumulateAllDoubles) {
    AccumulateToSetTransporter nodeTransporter;

    {
        Tree simple = Tree::make<AtLeastBinaryNode>(
            std::vector<Tree>{Tree::make<Leaf>(4.0)},
            Tree::make<Leaf>(3.0),
            Tree::make<BinaryNode>(Tree::make<Leaf>(2.0),
                                   Tree::make<NaryNode>(std::vector<Tree>{Tree::make<Leaf>(1.0)})));
        std::set<double> result = transport<false>(simple, nodeTransporter);
        ASSERT_EQ(result.size(), 4UL);
        ASSERT_EQ(result.count(1.0), 1UL);
        ASSERT_EQ(result.count(2.0), 1UL);
        ASSERT_EQ(result.count(3.0), 1UL);
        ASSERT_EQ(result.count(4.0), 1UL);
    }
    {
        Tree complex = Tree::make<AtLeastBinaryNode>(
            std::vector<Tree>{Tree::make<Leaf>(1.0), Tree::make<Leaf>(2.0)},
            Tree::make<Leaf>(3.0),
            Tree::make<BinaryNode>(
                Tree::make<Leaf>(4.0),
                Tree::make<NaryNode>(std::vector<Tree>{
                    Tree::make<Leaf>(5.0),
                    Tree::make<BinaryNode>(Tree::make<Leaf>(6.0), Tree::make<Leaf>(7.0))})));
        std::set<double> result = transport<false>(complex, nodeTransporter);
        ASSERT_EQ(result.size(), 7UL);
        ASSERT_EQ(result.count(1.0), 1UL);
        ASSERT_EQ(result.count(2.0), 1UL);
        ASSERT_EQ(result.count(3.0), 1UL);
        ASSERT_EQ(result.count(4.0), 1UL);
        ASSERT_EQ(result.count(5.0), 1UL);
        ASSERT_EQ(result.count(6.0), 1UL);
        ASSERT_EQ(result.count(7.0), 1UL);
    }
}


/**
 * A walker which accepts an extra 'multiplier' argument to each transport call.
 */
class NodeTransporterWithExtraArg {
public:
    double transport(Leaf& leaf, double multiplier) {
        return leaf.x * multiplier;
    }
    double transport(BinaryNode& node, double multiplier, double child0, double child1) {
        return child0 +
            child1;  // No need to apply multiplier here, would be applied in the children already.
    }
    double transport(NaryNode& node, double multiplier, std::vector<double> children) {
        return std::accumulate(children.begin(), children.end(), 0.0);
    }
    double transport(AtLeastBinaryNode& node,
                     double multiplier,
                     std::vector<double> children,
                     double child0,
                     double child1) {
        return child0 + child1 + std::accumulate(children.begin(), children.end(), 0.0);
    }
};

TEST(PolyValueTest, TransporterWithAnExtrArgument) {
    NodeTransporterWithExtraArg nodeTransporter;

    Tree simple = Tree::make<AtLeastBinaryNode>(
        std::vector<Tree>{Tree::make<Leaf>(4.0)},
        Tree::make<Leaf>(3.0),
        Tree::make<BinaryNode>(Tree::make<Leaf>(2.0),
                               Tree::make<NaryNode>(std::vector<Tree>{Tree::make<Leaf>(1.0)})));
    double result = transport<false>(simple, nodeTransporter, 2.0);
    ASSERT_EQ(result, 20.0);
}

/**
 * A simple walker which trackes whether it has seen a zero. While the task is simple, this walker
 * demosntrates:
 *  - A walker with state attached ('iHaveSeenAZero'). Note it could be done without tracking state
 *    also.
 *  - The capability of 'transport' to return void.
 *  - You can add a templated 'transport()' to avoid needing to fill in each and every instantiation
 * for the PolyValue.
 */
class TemplatedNodeTransporterWithContext {
public:
    bool iHaveSeenAZero = false;

    void transport(Leaf& leaf) {
        if (leaf.x == 0.0) {
            iHaveSeenAZero = true;
        }
    }

    /**
     * Template to handle all other cases - we don't care or need to do anything here, so we knock
     * out all the other required implementations at once with this template.
     */
    template <typename T, typename... Args>
    void transport(T&& node, Args&&... args) {
        return;
    }
};

TEST(PolyValueTest, TransporterTrackingState) {
    TemplatedNodeTransporterWithContext templatedNodeTransporter;

    Tree noZero = Tree::make<AtLeastBinaryNode>(
        std::vector<Tree>{Tree::make<Leaf>(4.0)},
        Tree::make<Leaf>(3.0),
        Tree::make<BinaryNode>(Tree::make<Leaf>(2.0),
                               Tree::make<NaryNode>(std::vector<Tree>{Tree::make<Leaf>(1.0)})));
    transport<false>(noZero, templatedNodeTransporter);
    ASSERT_EQ(templatedNodeTransporter.iHaveSeenAZero, false);

    Tree yesZero = Tree::make<AtLeastBinaryNode>(
        std::vector<Tree>{Tree::make<Leaf>(3.0)},
        Tree::make<Leaf>(2.0),
        Tree::make<BinaryNode>(Tree::make<Leaf>(1.0),
                               Tree::make<NaryNode>(std::vector<Tree>{Tree::make<Leaf>(0.0)})));
    transport<false>(yesZero, templatedNodeTransporter);
    ASSERT_EQ(templatedNodeTransporter.iHaveSeenAZero, true);
}

/**
 * A walker demonstrating the 'prepare()' API which tracks the depth and weights things deeper in
 * the tree at factors of 10 higher. So the top level is worth 1x, second level 10x, third level
 * 100x, etc.
 */
class NodeTransporterTrackingDepth {
    int _depthMultiplier = 1;

public:
    double transport(Leaf& leaf) {
        return leaf.x * _depthMultiplier;
    }

    void prepare(Leaf&) {
        // Noop. Just here to prevent from yet another 10x multiplication if we were to fall into
        // the generic 'prepare()'.
    }

    /**
     * 'prepare()' is called as we descend the tree before we walk/visit the children.
     */
    template <typename T, typename... Args>
    void prepare(T&& node, Args&&... args) {
        _depthMultiplier *= 10;
    }

    double transport(BinaryNode& node, double child0, double child1) {
        _depthMultiplier /= 10;
        return child0 + child1;
    }
    double transport(NaryNode& node, std::vector<double> children) {
        _depthMultiplier /= 10;
        return std::accumulate(children.begin(), children.end(), 0.0);
    }
    double transport(AtLeastBinaryNode& node,
                     std::vector<double> children,
                     double child0,
                     double child1) {
        _depthMultiplier /= 10;
        return child0 + child1 + std::accumulate(children.begin(), children.end(), 0.0);
    }
};

TEST(PolyValueTest, TransporterUsingPrepare) {
    NodeTransporterTrackingDepth nodeTransporter;

    Tree demoTree = Tree::make<AtLeastBinaryNode>(
        std::vector<Tree>{Tree::make<Leaf>(4.0)},
        Tree::make<Leaf>(3.0),
        Tree::make<BinaryNode>(Tree::make<Leaf>(2.0),
                               Tree::make<NaryNode>(std::vector<Tree>{Tree::make<Leaf>(1.0)})));
    const double result = transport<false>(demoTree, nodeTransporter);
    /*
    demoTree
    1x level:     root
                 / | \
    10x level:  4  3  binary
                       / \
    100x level:       2   nary
                            \
    1000x level:             1
    */
    ASSERT_EQ(result, 1270.0);
}

class NodeWalkerIsLeaf {
public:
    bool walk(Leaf& leaf) {
        return true;
    }

    bool walk(BinaryNode& node, Tree& leftChild, Tree& rightChild) {
        return false;
    }

    bool walk(AtLeastBinaryNode& node,
              std::vector<Tree>& extraChildren,
              Tree& leftChild,
              Tree& rightChild) {
        return false;
    }

    bool walk(NaryNode& node, std::vector<Tree>& children) {
        return false;
    }
};

TEST(PolyValueTest, WalkerBasic) {
    NodeWalkerIsLeaf walker;
    auto tree = Tree::make<BinaryNode>(Tree::make<Leaf>(1.0), Tree::make<Leaf>(2.0));
    ASSERT(!walk<false>(tree, walker));
    ASSERT(walk<false>(tree.cast<BinaryNode>()->get<0>(), walker));
    ASSERT(walk<false>(tree.cast<BinaryNode>()->get<1>(), walker));
}

}  // namespace
}  // namespace mongo::optimizer::algebra
