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
    JoinGraph graph;
    auto aNss = NamespaceString::createNamespaceString_forTest("a");
    auto bNss = NamespaceString::createNamespaceString_forTest("b");
    auto aCQ = makeCanonicalQuery(aNss);
    auto bCQ = makeCanonicalQuery(bNss);
    auto aNodeId = *graph.addNode(aNss, std::move(aCQ), boost::none);
    auto bNodeId = *graph.addNode(bNss, std::move(bCQ), FieldPath{"b"});

    std::vector<ResolvedPath> paths;
    paths.push_back(ResolvedPath{.nodeId = aNodeId, .fieldName = "foo"});
    paths.push_back(ResolvedPath{.nodeId = bNodeId, .fieldName = "foo"});

    graph.addSimpleEqualityEdge(aNodeId, bNodeId, 0, 1);

    SamplingEstimatorMap samplingEstimators;
    auto aSamplingEstimator = std::make_unique<FakeNdvEstimator>(
        CardinalityEstimate{CardinalityType{10}, EstimationSource::Sampling});
    aSamplingEstimator->addFakeNDVEstimate(
        {FieldPath("foo")}, CardinalityEstimate{CardinalityType{5}, EstimationSource::Sampling});
    samplingEstimators[aNss] = std::move(aSamplingEstimator);
    samplingEstimators[bNss] = std::make_unique<FakeNdvEstimator>(
        CardinalityEstimate{CardinalityType{20}, EstimationSource::Sampling});

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
    JoinGraph graph;
    auto aNss = NamespaceString::createNamespaceString_forTest("a");
    auto bNss = NamespaceString::createNamespaceString_forTest("b");
    auto aCQ = makeCanonicalQuery(aNss);
    auto bCQ = makeCanonicalQuery(bNss);
    auto aNodeId = *graph.addNode(aNss, std::move(aCQ), boost::none);
    auto bNodeId = *graph.addNode(bNss, std::move(bCQ), FieldPath{"b"});

    std::vector<ResolvedPath> paths;
    paths.push_back(ResolvedPath{.nodeId = aNodeId, .fieldName = "foo"});
    paths.push_back(ResolvedPath{.nodeId = bNodeId, .fieldName = "foo"});

    graph.addSimpleEqualityEdge(aNodeId, bNodeId, 0, 1);

    SamplingEstimatorMap samplingEstimators;
    samplingEstimators[aNss] = std::make_unique<FakeNdvEstimator>(
        CardinalityEstimate{CardinalityType{20}, EstimationSource::Sampling});
    // Ensure "b" collection has smaller CE. Only add fake estimates for "b" estimator.
    auto bSamplingEstimator = std::make_unique<FakeNdvEstimator>(
        CardinalityEstimate{CardinalityType{10}, EstimationSource::Sampling});
    bSamplingEstimator->addFakeNDVEstimate(
        {FieldPath("foo")}, CardinalityEstimate{CardinalityType{5}, EstimationSource::Sampling});
    samplingEstimators[bNss] = std::move(bSamplingEstimator);

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
    JoinGraph graph;
    auto aNss = NamespaceString::createNamespaceString_forTest("a");
    auto bNss = NamespaceString::createNamespaceString_forTest("b");
    auto aCQ = makeCanonicalQuery(aNss);
    auto bCQ = makeCanonicalQuery(bNss);
    auto aNodeId = *graph.addNode(aNss, std::move(aCQ), boost::none);
    auto bNodeId = *graph.addNode(bNss, std::move(bCQ), FieldPath{"b"});

    std::vector<ResolvedPath> paths;
    paths.push_back(ResolvedPath{.nodeId = aNodeId, .fieldName = "foo"});
    paths.push_back(ResolvedPath{.nodeId = bNodeId, .fieldName = "foo"});
    paths.push_back(ResolvedPath{.nodeId = aNodeId, .fieldName = "bar"});
    paths.push_back(ResolvedPath{.nodeId = bNodeId, .fieldName = "bar"});

    // a.foo = b.foo && a.bar = b.bar
    graph.addSimpleEqualityEdge(aNodeId, bNodeId, 0, 1);
    graph.addSimpleEqualityEdge(aNodeId, bNodeId, 2, 3);

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

TEST_F(JoinPredicateEstimatorFixture, ExtractNodeCardinalities) {
    JoinGraph graph;
    std::vector<ResolvedPath> paths;
    JoinReorderingContext ctx{graph, paths};

    auto aNss = NamespaceString::createNamespaceString_forTest("a");
    auto bNss = NamespaceString::createNamespaceString_forTest("b");
    auto aCQ = makeCanonicalQuery(aNss);
    auto bCQ = makeCanonicalQuery(bNss);
    auto aNodeId = *graph.addNode(aNss, std::move(aCQ), boost::none);
    auto bNodeId = *graph.addNode(bNss, std::move(bCQ), FieldPath{"b"});

    const auto inCE = CardinalityEstimate{CardinalityType{100}, EstimationSource::Sampling};
    const auto aCE = CardinalityEstimate{CardinalityType{10}, EstimationSource::Sampling};
    const auto bCE = CardinalityEstimate{CardinalityType{20}, EstimationSource::Sampling};

    SingleTableAccessPlansResult singleTablePlansRes;
    {
        auto aPlan = makeCollScanPlan(aNss);
        singleTablePlansRes.estimate[aPlan->root()] = {inCE, aCE};
        singleTablePlansRes.solns[graph.getNode(aNodeId).accessPath.get()] = std::move(aPlan);
    }
    {
        auto bPlan = makeCollScanPlan(bNss);
        singleTablePlansRes.estimate[bPlan->root()] = {inCE, bCE};
        singleTablePlansRes.solns[graph.getNode(bNodeId).accessPath.get()] = std::move(bPlan);
    }

    auto nodeCardinalities =
        JoinCardinalityEstimator::extractNodeCardinalities(ctx, singleTablePlansRes);
    ASSERT_EQ(2U, nodeCardinalities.size());
    ASSERT_EQ(aCE, nodeCardinalities[aNodeId]);
    ASSERT_EQ(bCE, nodeCardinalities[bNodeId]);
}
}  // namespace mongo::join_ordering
