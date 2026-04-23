/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/bson/json.h"
#include "mongo/db/query/compiler/optimizer/join/cardinality_estimator.h"
#include "mongo/db/query/compiler/optimizer/join/unit_test_helpers.h"
#include "mongo/unittest/unittest.h"

namespace mongo::join_ordering {

using JoinPredicateEstimatorFixture = JoinOrderingTestFixture;
using namespace cost_based_ranker;

// Join graph: A -- B with edge A.foo = B.foo and 'A' being the main collection
// The cardinality estimate for 'A' is smaller, so we assert that we use NDV(A.foo) for the join
// predicate selectivity estimate.
TEST_F(JoinPredicateEstimatorFixture, NDVSmallerCollection) {
    MutableJoinGraph mgraph;
    auto aNss = NamespaceString::createNamespaceString_forTest("a");
    auto bNss = NamespaceString::createNamespaceString_forTest("b");
    auto aCQ = makeCanonicalQuery(aNss);
    auto bCQ = makeCanonicalQuery(bNss);
    auto aNodeId = *mgraph.addNode(aNss, std::move(aCQ), boost::none);
    auto bNodeId = *mgraph.addNode(bNss, std::move(bCQ), FieldPath{"b"});

    std::vector<ResolvedPath> paths;
    paths.push_back(ResolvedPath{.nodeId = aNodeId, .fieldName = "foo"});
    paths.push_back(ResolvedPath{.nodeId = bNodeId, .fieldName = "foo"});

    mgraph.addSimpleEqualityEdge(aNodeId, bNodeId, 0, 1);

    SamplingEstimatorMap samplingEstimators;
    auto aSamplingEstimator = std::make_unique<FakeNdvEstimator>(
        CardinalityEstimate{CardinalityType{10}, EstimationSource::Sampling});
    aSamplingEstimator->addFakeNDVEstimate(
        {FieldPath("foo")}, CardinalityEstimate{CardinalityType{5}, EstimationSource::Sampling});
    samplingEstimators[aNss] = std::move(aSamplingEstimator);
    samplingEstimators[bNss] = std::make_unique<FakeNdvEstimator>(
        CardinalityEstimate{CardinalityType{20}, EstimationSource::Sampling});

    JoinGraph graph(std::move(mgraph));
    JoinReorderingContext ctx{graph, paths};
    auto selEst =
        JoinCardinalityEstimator::joinPredicateSel(ctx, samplingEstimators, graph.getEdge(0));
    // The selectivity estimate comes from 1 / NDV(A.foo) = 1 / 5 = 0.2
    auto expectedSel = SelectivityEstimate{SelectivityType{0.2}, EstimationSource::Sampling};
    ASSERT_EQ(expectedSel, selEst);

    auto edgeSels = JoinCardinalityEstimator::estimateEdgeSelectivities(ctx, samplingEstimators);
    ASSERT_EQ(1U, edgeSels.size());
    ASSERT_EQ(expectedSel, edgeSels[0]);
}

// Join graph: A -- B with edge A.foo = B.foo and 'A' being the main collection
// The cardinality estimate for 'B' is smaller, so we assert that we use NDV(B.foo) for the join
// predicate selectivity estimate. This verifies that an embedded node can still be used for join
// predicate estimatation.
TEST_F(JoinPredicateEstimatorFixture, NDVSmallerCollectionEmbedPath) {
    MutableJoinGraph mgraph;
    auto aNss = NamespaceString::createNamespaceString_forTest("a");
    auto bNss = NamespaceString::createNamespaceString_forTest("b");
    auto aCQ = makeCanonicalQuery(aNss);
    auto bCQ = makeCanonicalQuery(bNss);
    auto aNodeId = *mgraph.addNode(aNss, std::move(aCQ), boost::none);
    auto bNodeId = *mgraph.addNode(bNss, std::move(bCQ), FieldPath{"b"});

    std::vector<ResolvedPath> paths;
    paths.push_back(ResolvedPath{.nodeId = aNodeId, .fieldName = "foo"});
    paths.push_back(ResolvedPath{.nodeId = bNodeId, .fieldName = "foo"});

    mgraph.addSimpleEqualityEdge(aNodeId, bNodeId, 0, 1);

    SamplingEstimatorMap samplingEstimators;
    samplingEstimators[aNss] = std::make_unique<FakeNdvEstimator>(
        CardinalityEstimate{CardinalityType{20}, EstimationSource::Sampling});
    // Ensure "b" collection has smaller CE. Only add fake estimates for "b" estimator.
    auto bSamplingEstimator = std::make_unique<FakeNdvEstimator>(
        CardinalityEstimate{CardinalityType{10}, EstimationSource::Sampling});
    bSamplingEstimator->addFakeNDVEstimate(
        {FieldPath("foo")}, CardinalityEstimate{CardinalityType{5}, EstimationSource::Sampling});
    samplingEstimators[bNss] = std::move(bSamplingEstimator);

    JoinGraph graph(std::move(mgraph));
    JoinReorderingContext ctx{graph, paths};
    auto selEst =
        JoinCardinalityEstimator::joinPredicateSel(ctx, samplingEstimators, graph.getEdge(0));
    // The selectivity estimate comes from 1 / NDV(B.foo) = 1 / 5 = 0.2
    auto expectedSel = SelectivityEstimate{SelectivityType{0.2}, EstimationSource::Sampling};
    ASSERT_EQ(expectedSel, selEst);

    auto edgeSels = JoinCardinalityEstimator::estimateEdgeSelectivities(ctx, samplingEstimators);
    ASSERT_EQ(1U, edgeSels.size());
    ASSERT_EQ(expectedSel, edgeSels[0]);
}

// Join graph: A -- B with compound edge A.foo = B.foo && A.bar = B.bar and 'A' being the main
// collection. The cardinality estimate for 'A' is smaller, so we assert that we use the tuple
// NDV(A.foo, A.bar) for the join predicate selectivity estimate.
TEST_F(JoinPredicateEstimatorFixture, NDVCompoundJoinKey) {
    MutableJoinGraph mgraph;
    auto aNss = NamespaceString::createNamespaceString_forTest("a");
    auto bNss = NamespaceString::createNamespaceString_forTest("b");
    auto aCQ = makeCanonicalQuery(aNss);
    auto bCQ = makeCanonicalQuery(bNss);
    auto aNodeId = *mgraph.addNode(aNss, std::move(aCQ), boost::none);
    auto bNodeId = *mgraph.addNode(bNss, std::move(bCQ), FieldPath{"b"});

    std::vector<ResolvedPath> paths;
    paths.push_back(ResolvedPath{.nodeId = aNodeId, .fieldName = "foo"});
    paths.push_back(ResolvedPath{.nodeId = bNodeId, .fieldName = "foo"});
    paths.push_back(ResolvedPath{.nodeId = aNodeId, .fieldName = "bar"});
    paths.push_back(ResolvedPath{.nodeId = bNodeId, .fieldName = "bar"});

    // a.foo = b.foo && a.bar = b.bar
    mgraph.addSimpleEqualityEdge(aNodeId, bNodeId, 0, 1);
    mgraph.addSimpleEqualityEdge(aNodeId, bNodeId, 2, 3);

    SamplingEstimatorMap samplingEstimators;
    auto aSamplingEstimator = std::make_unique<FakeNdvEstimator>(
        CardinalityEstimate{CardinalityType{10}, EstimationSource::Sampling});
    // We should end up using the NDV from (foo, bar) and not from foo or bar.
    aSamplingEstimator->addFakeNDVEstimate(
        {FieldPath("foo"), FieldPath("bar")},
        CardinalityEstimate{CardinalityType{5}, EstimationSource::Sampling});
    aSamplingEstimator->addFakeNDVEstimate(
        {FieldPath("foo")}, CardinalityEstimate{CardinalityType{2}, EstimationSource::Sampling});
    aSamplingEstimator->addFakeNDVEstimate(
        {FieldPath("bar")}, CardinalityEstimate{CardinalityType{3}, EstimationSource::Sampling});
    samplingEstimators[aNss] = std::move(aSamplingEstimator);
    samplingEstimators[bNss] = std::make_unique<FakeNdvEstimator>(
        CardinalityEstimate{CardinalityType{20}, EstimationSource::Sampling});

    JoinGraph graph(std::move(mgraph));
    JoinReorderingContext ctx{graph, paths};
    auto selEst =
        JoinCardinalityEstimator::joinPredicateSel(ctx, samplingEstimators, graph.getEdge(0));
    // The selectivity estimate comes from 1 / NDV(A.foo, A.bar) = 1 / 5 = 0.2
    auto expectedSel = SelectivityEstimate{SelectivityType{0.2}, EstimationSource::Sampling};
    ASSERT_EQ(expectedSel, selEst);

    auto edgeSels = JoinCardinalityEstimator::estimateEdgeSelectivities(ctx, samplingEstimators);
    ASSERT_EQ(1U, edgeSels.size());
    ASSERT_EQ(expectedSel, edgeSels[0]);
}

namespace {
void pushNNodes(MutableJoinGraph& graph, size_t n) {
    for (size_t i = 0; i < n; i++) {
        auto nss =
            NamespaceString::createNamespaceString_forTest("test", str::stream() << "nss" << i);
        graph.addNode(nss, nullptr, boost::none);
    }
}
}  // namespace

TEST_F(JoinPredicateEstimatorFixture, EstimateSubsetCardinality) {
    // Construct 6 nodes, with single-table CEs that are multiples of 10. Node 0 will be ignored in
    // the rest of the test; it is only there for easy math.
    size_t numNodes = 6;
    pushNNodes(graph, numNodes);
    NodeCardinalities nodeCEs{oneCE, oneCE * 10, oneCE * 20, oneCE * 30, oneCE * 40, oneCE * 50};

    /**
     * Construct a graph like so
     * 0 -- 1 -- 2 -- 3
     *               /  \
     *              4 -- 5
     * With edge selectivies that are multiples of 0.1.
     * Note: There is one cycle here between nodes 3, 4, and 5. There are no other cycles (implicit
     * or explicit).
     */
    graph.addSimpleEqualityEdge(NodeId(0), NodeId(1), 0, 1);
    graph.addSimpleEqualityEdge(NodeId(1), NodeId(2), 2, 3);
    graph.addSimpleEqualityEdge(NodeId(2), NodeId(3), 4, 5);
    graph.addSimpleEqualityEdge(NodeId(3), NodeId(4), 6, 7);
    graph.addSimpleEqualityEdge(NodeId(4), NodeId(5), 7, 8);
    graph.addSimpleEqualityEdge(NodeId(3), NodeId(5), 6, 8);

    EdgeSelectivities edgeSels;
    for (size_t i = 0; i < numNodes; i++) {
        edgeSels.push_back(cost_based_ranker::SelectivityEstimate(SelectivityType(i * 0.1),
                                                                  EstimationSource::Sampling));
    }

    nodeCards = nodeCEs;
    auto jCtx = makeContext();
    JoinCardinalityEstimator jce(jCtx, edgeSels);
    {
        // Cardinality for subset of size 1 is pulled directly from the CE map.
        ASSERT_EQ(oneCE * 10, jce.getOrEstimateSubsetCardinality(makeNodeSet(1)));
        ASSERT_EQ(oneCE * 20, jce.getOrEstimateSubsetCardinality(makeNodeSet(2)));
        ASSERT_EQ(oneCE * 30, jce.getOrEstimateSubsetCardinality(makeNodeSet(3)));
        ASSERT_EQ(oneCE * 40, jce.getOrEstimateSubsetCardinality(makeNodeSet(4)));
        ASSERT_EQ(oneCE * 50, jce.getOrEstimateSubsetCardinality(makeNodeSet(5)));
    }
    {
        // Connected sub-graph cardinality is a combo of single-table CEs and edge selectivities.
        ASSERT_EQ(oneCE * 10 * 20 * 0.1, jce.getOrEstimateSubsetCardinality(makeNodeSet(1, 2)));
        ASSERT_EQ(oneCE * 30 * 40 * 0.3, jce.getOrEstimateSubsetCardinality(makeNodeSet(3, 4)));

        ASSERT_EQ(oneCE * 10 * 20 * 30 * 0.1 * 0.2,
                  jce.getOrEstimateSubsetCardinality(makeNodeSet(1, 2, 3)));
        ASSERT_EQ(oneCE * 20 * 30 * 40 * 0.2 * 0.3,
                  jce.getOrEstimateSubsetCardinality(makeNodeSet(2, 3, 4)));

        ASSERT_EQ(oneCE * 10 * 20 * 30 * 50 * 0.1 * 0.2 * 0.5,
                  jce.getOrEstimateSubsetCardinality(makeNodeSet(1, 2, 3, 5)));
    }

    {
        // Disconnected sub-graph cardinality includes some cross-products.
        ASSERT_EQ(oneCE * 20 * 40, jce.getOrEstimateSubsetCardinality(makeNodeSet(2, 4)));
        ASSERT_EQ(oneCE * 10 * 30 * 40 * 0.3,
                  jce.getOrEstimateSubsetCardinality(makeNodeSet(1, 3, 4)));
    }

    {
        // Cycle cardinality estimation does not involve all edges in the cycle.
        // Note: Edge with selectivity 0.4 has been removed from both examples below.
        ASSERT_EQ(oneCE * 30 * 40 * 50 * 0.3 * 0.5,
                  jce.getOrEstimateSubsetCardinality(makeNodeSet(3, 4, 5)));

        ASSERT_EQ(oneCE * 10 * 20 * 30 * 40 * 50 * 0.1 * 0.2 * 0.3 * 0.5,
                  jce.getOrEstimateSubsetCardinality(makeNodeSet(1, 2, 3, 4, 5)));
    }
}

// Similar to the test above, but verifies that path IDs are considered when determining the
// presence of cycles.
TEST_F(JoinPredicateEstimatorFixture, EstimateSubsetCardinalityAlmostCycle) {
    size_t numNodes = 4;
    pushNNodes(graph, numNodes);
    NodeCardinalities nodeCEs{oneCE, oneCE * 10, oneCE * 20, oneCE * 30};

    /**
     * Construct a graph like so
     * 0 -- 1
     *     /  \
     *    2 -- 3
     * Note: There is NO cycle here, because the path IDs chosen for the edges are different.
     */
    graph.addSimpleEqualityEdge(NodeId(0), NodeId(1), 0, 1);
    graph.addSimpleEqualityEdge(NodeId(1), NodeId(2), 2, 3);
    graph.addSimpleEqualityEdge(NodeId(2), NodeId(3), 4, 5);
    graph.addSimpleEqualityEdge(NodeId(1), NodeId(3), 6, 7);

    EdgeSelectivities edgeSels;
    for (size_t i = 0; i < numNodes; i++) {
        edgeSels.push_back(cost_based_ranker::SelectivityEstimate(SelectivityType(i * 0.1),
                                                                  EstimationSource::Sampling));
    }

    nodeCards = nodeCEs;
    auto jCtx = makeContext();
    JoinCardinalityEstimator jce(jCtx, edgeSels);
    ASSERT_EQ(oneCE * 10 * 20 * 30 * 0.1 * 0.2 * 0.3,
              jce.getOrEstimateSubsetCardinality(makeNodeSet(1, 2, 3)));
}

TEST_F(JoinPredicateEstimatorFixture, EstimateSubsetCardinalitySameCollectionPresentTwice) {
    auto nssOne = NamespaceString::createNamespaceString_forTest("test", str::stream() << "nssOne");
    auto nssTwo = NamespaceString::createNamespaceString_forTest("test", str::stream() << "nssTwo");

    std::string fieldNameA = str::stream() << "a" << 0;
    auto filterBSONA = BSON(fieldNameA << BSON("$gt" << 0));

    std::string fieldNameB = str::stream() << "b" << 0;
    auto filterBSONB = BSON(fieldNameB << BSON("$gt" << 0));

    // The first reference to nssOne has a filter on field "a".
    auto cqA = makeCanonicalQuery(nssOne, filterBSONA);
    ASSERT_TRUE(graph.addNode(nssOne, std::move(cqA), boost::none).has_value());

    // The second reference to nssOne has a filter on field "b". This node will have larger CE.
    auto cqB = makeCanonicalQuery(nssOne, filterBSONB);
    ASSERT_TRUE(graph.addNode(nssOne, std::move(cqB), boost::none).has_value());

    // Finally, there is a node in between for nssTwo.
    auto cqNssTwo = makeCanonicalQuery(nssTwo, filterBSONA);
    ASSERT_TRUE(graph.addNode(nssTwo, std::move(cqNssTwo), boost::none).has_value());

    // Finalize graph:
    // 0   1
    //  \ /
    //   2
    graph.addSimpleEqualityEdge(NodeId(0), NodeId(2), 0, 1);
    graph.addSimpleEqualityEdge(NodeId(1), NodeId(2), 2, 3);
    EdgeSelectivities edgeSels;
    for (size_t i = 0; i < 2; i++) {
        edgeSels.push_back(cost_based_ranker::SelectivityEstimate(SelectivityType((i + 1) * 0.1),
                                                                  EstimationSource::Sampling));
    }
    NodeCardinalities nodeCEs{
        oneCE * 10,
        oneCE * 20,
        oneCE * 30,
    };

    nodeCards = nodeCEs;
    auto jCtx = makeContext();
    JoinCardinalityEstimator jce(jCtx, edgeSels);

    // Show that even though the namespace is the same for two of the nodes, we are able to
    // correctly associate CE with the particular filters associated with those nodes.
    ASSERT_EQ(oneCE * 10, jce.getOrEstimateSubsetCardinality(makeNodeSet(0)));
    ASSERT_EQ(oneCE * 20, jce.getOrEstimateSubsetCardinality(makeNodeSet(1)));
    ASSERT_EQ(oneCE * 30, jce.getOrEstimateSubsetCardinality(makeNodeSet(2)));

    ASSERT_EQ(oneCE * 10 * 20, jce.getOrEstimateSubsetCardinality(makeNodeSet(0, 1)));
    ASSERT_EQ(oneCE * 10 * 30 * 0.1, jce.getOrEstimateSubsetCardinality(makeNodeSet(0, 2)));
    ASSERT_EQ(oneCE * 20 * 30 * 0.2, jce.getOrEstimateSubsetCardinality(makeNodeSet(1, 2)));

    ASSERT_EQ(oneCE * 10 * 20 * 30 * 0.1 * 0.2,
              jce.getOrEstimateSubsetCardinality(makeNodeSet(0, 1, 2)));
}

TEST_F(JoinPredicateEstimatorFixture, JoinPredicateSelUsesUniqueFields) {
    // Create a join graph with two nodes joined by an edge: a -- b.
    auto aNss = NamespaceString::createNamespaceString_forTest("a");
    auto bNss = NamespaceString::createNamespaceString_forTest("b");
    auto aNodeId = *graph.addNode(aNss, nullptr, boost::none);
    auto bNodeId = *graph.addNode(bNss, nullptr, FieldPath{"b"});

    // The edge represents a.foo == b.bar.
    resolvedPaths.push_back(ResolvedPath{.nodeId = aNodeId, .fieldName = "foo"});
    resolvedPaths.push_back(ResolvedPath{.nodeId = bNodeId, .fieldName = "bar"});
    graph.addSimpleEqualityEdge(aNodeId, bNodeId, 0, 1);

    // Establish that |a| is smaller, so that we will use it for NDV estimation.
    SamplingEstimatorMap samplingEstimators;
    auto aSamplingEstimator = std::make_unique<FakeNdvEstimator>(
        CardinalityEstimate{CardinalityType{10}, EstimationSource::Sampling});
    aSamplingEstimator->addFakeNDVEstimate(
        {FieldPath("foo")}, CardinalityEstimate{CardinalityType{5}, EstimationSource::Sampling});
    samplingEstimators[aNss] = std::move(aSamplingEstimator);
    samplingEstimators[bNss] = std::make_unique<FakeNdvEstimator>(
        CardinalityEstimate{CardinalityType{20}, EstimationSource::Sampling});
    auto jCtx = makeContext();

    // Selectivity test without unique information. Here the selectivity estimate comes from
    // 1 / NDV(a.foo) = 1 / 5 = 0.2
    {
        auto selEst = JoinCardinalityEstimator::joinPredicateSel(
            jCtx, samplingEstimators, jCtx.joinGraph.getEdge(0));
        auto expectedSel = SelectivityEstimate{SelectivityType{0.2}, EstimationSource::Sampling};
        ASSERT_EQ(expectedSel, selEst);
    }
    // Selectivity test with unique information. Tell the context that "foo" is a unique field. This
    // should change our estimate for NDV(a.foo) to |a| = 10, giving a new selectivity of 0.1.
    {
        jCtx.uniqueFieldInfo.emplace(aNss, buildUniqueFieldInfo({fromjson("{foo: 1}")}));
        auto selEst = JoinCardinalityEstimator::joinPredicateSel(
            jCtx, samplingEstimators, jCtx.joinGraph.getEdge(0));
        auto expectedSel = SelectivityEstimate{SelectivityType{0.1}, EstimationSource::Sampling};
        ASSERT_EQ(expectedSel, selEst);
    }
}

TEST_F(JoinPredicateEstimatorFixture, JoinPredicateSelUsesUniqueFieldsCompoundJoinPred) {
    // Create a join graph with two nodes joined by a compound edge: a -- b.
    auto aNss = NamespaceString::createNamespaceString_forTest("a");
    auto bNss = NamespaceString::createNamespaceString_forTest("b");
    auto aNodeId = *graph.addNode(aNss, nullptr, boost::none);
    auto bNodeId = *graph.addNode(bNss, nullptr, FieldPath{"b"});

    // Compound edge is a.foo == b.baz && a.bar == foo.baz.
    resolvedPaths.push_back(ResolvedPath{.nodeId = aNodeId, .fieldName = "foo"});  // 0
    resolvedPaths.push_back(ResolvedPath{.nodeId = aNodeId, .fieldName = "bar"});  // 1
    resolvedPaths.push_back(ResolvedPath{.nodeId = bNodeId, .fieldName = "baz"});  // 2
    graph.addSimpleEqualityEdge(aNodeId, bNodeId, 0, 2);
    graph.addSimpleEqualityEdge(aNodeId, bNodeId, 1, 2);

    // Establish that |a| is smaller, so that we will use it for NDV estimation.
    SamplingEstimatorMap samplingEstimators;
    auto aSamplingEstimator = std::make_unique<FakeNdvEstimator>(
        CardinalityEstimate{CardinalityType{10}, EstimationSource::Sampling});
    aSamplingEstimator->addFakeNDVEstimate(
        {{"foo"}, {"bar"}}, CardinalityEstimate{CardinalityType{5}, EstimationSource::Sampling});
    samplingEstimators[aNss] = std::move(aSamplingEstimator);
    samplingEstimators[bNss] = std::make_unique<FakeNdvEstimator>(
        CardinalityEstimate{CardinalityType{20}, EstimationSource::Sampling});
    auto jCtx = makeContext();

    // Selectivity test without unique information. Here the selectivity estimate comes from
    // 1 / NDV(a.foo, a.bar) = 1 / 5 = 0.2
    {
        auto selEst = JoinCardinalityEstimator::joinPredicateSel(
            jCtx, samplingEstimators, jCtx.joinGraph.getEdge(0));
        auto expectedSel = SelectivityEstimate{SelectivityType{0.2}, EstimationSource::Sampling};
        ASSERT_EQ(expectedSel, selEst);
    }
    // Selectivity test with unique information. Tell the context that {"foo", "bar"} are unique.
    // This should change our NDV estimate to 10, giving a new selectivity of 0.1.
    {
        jCtx.uniqueFieldInfo.emplace(aNss, buildUniqueFieldInfo({fromjson("{foo: 1, bar: 1}")}));
        auto selEst = JoinCardinalityEstimator::joinPredicateSel(
            jCtx, samplingEstimators, jCtx.joinGraph.getEdge(0));
        auto expectedSel = SelectivityEstimate{SelectivityType{0.1}, EstimationSource::Sampling};
        ASSERT_EQ(expectedSel, selEst);
    }
}

}  // namespace mongo::join_ordering
