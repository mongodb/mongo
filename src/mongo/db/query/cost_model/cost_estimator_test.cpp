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

#include "mongo/db/query/cost_model/cost_estimator_impl.h"
#include "mongo/db/query/cost_model/cost_model_gen.h"
#include "mongo/db/query/cost_model/cost_model_utils.h"
#include "mongo/db/query/optimizer/cascades/memo.h"
#include "mongo/unittest/unittest.h"

namespace mongo::cost_model {

/**
 * Test that the PhysicalScan's cost is calculated correctly.
 */
TEST(CostEstimatorTest, PhysicalScanCost) {
    const double startupCost = 1;
    const double scanCost = 3;
    const optimizer::CEType ce{1000.0};

    CostModelCoefficients costModel{};
    initializeTestCostModel(costModel);
    costModel.setScanStartupCost(startupCost);
    costModel.setScanIncrementalCost(scanCost);

    CostEstimatorImpl costEstimator{costModel};

    optimizer::Metadata metadata{{}};
    optimizer::cascades::Memo memo{};

    // Mimic properties from PhysicalScan group. Only DistributionRequirement is really required.
    optimizer::properties::ProjectionRequirement pr{{optimizer::ProjectionNameVector{"root"}}};
    optimizer::properties::DistributionRequirement dr{{optimizer::DistributionType::Centralized}};
    dr.setDisableExchanges(true);
    optimizer::properties::IndexingRequirement ir{optimizer::IndexReqTarget::Complete,
                                                  /*dedupRID*/ true,
                                                  /*satisfiedPartialIndexesGroupId*/ 0};
    optimizer::properties::PhysProps physProps{};
    optimizer::properties::setProperty(physProps, pr);
    optimizer::properties::setProperty(physProps, dr);
    optimizer::properties::setProperty(physProps, ir);

    auto scanNode = optimizer::ABT::make<optimizer::PhysicalScanNode>(
        optimizer::FieldProjectionMap{{}, {optimizer::ProjectionName{"root"}}, {}}, "c1", false);
    optimizer::ChildPropsType childProps{};
    optimizer::NodeCEMap nodeCEMap{{scanNode.cast<optimizer::Node>(), ce}};

    auto costAndCE =
        costEstimator.deriveCost(metadata, memo, physProps, scanNode.ref(), childProps, nodeCEMap);

    // Test the cost.
    ASSERT_EQ(startupCost + scanCost * costAndCE._ce._value, costAndCE._cost.getCost());

    // CE is not expected to be adjusted for the given set of PhysicalProperties.
    ASSERT_EQ(ce, costAndCE._ce._value);
}

/**
 * Test that the PhysicalScan's cost is calculated correctly with adjusted CE.
 */
TEST(CostEstimatorTest, PhysicalScanCostWithAdjustedCE) {
    const double startupCost = 1;
    const double scanCost = 3;
    const optimizer::CEType ce{1000.0};
    const optimizer::CEType limitEstimateCE{10.0};

    CostModelCoefficients costModel{};
    initializeTestCostModel(costModel);
    costModel.setScanStartupCost(startupCost);
    costModel.setScanIncrementalCost(scanCost);

    CostEstimatorImpl costEstimator{costModel};

    optimizer::Metadata metadata{{}};
    optimizer::cascades::Memo memo{};

    optimizer::properties::DistributionRequirement dr{{optimizer::DistributionType::Centralized}};
    optimizer::properties::LimitEstimate le{limitEstimateCE};
    optimizer::properties::PhysProps physProps{};
    optimizer::properties::setProperty(physProps, dr);
    optimizer::properties::setProperty(physProps, le);

    auto scanNode = optimizer::ABT::make<optimizer::PhysicalScanNode>(
        optimizer::FieldProjectionMap{{}, {}, {}}, "c1", false);
    optimizer::ChildPropsType childProps{};
    optimizer::NodeCEMap nodeCEMap{{scanNode.cast<optimizer::Node>(), ce}};

    auto costAndCE =
        costEstimator.deriveCost(metadata, memo, physProps, scanNode.ref(), childProps, nodeCEMap);

    // Test the cost.
    ASSERT_EQ(startupCost + scanCost * costAndCE._ce._value, costAndCE._cost.getCost());

    // CE is expected to be adjusted.
    ASSERT_EQ(limitEstimateCE, costAndCE._ce._value);
}

TEST(CostEstimatorTest, IndexScanCost) {
    const double startupCost = 1;
    const double indexScanCost = 3;
    const optimizer::CEType ce{1000.0};

    CostModelCoefficients costModel{};
    initializeTestCostModel(costModel);
    costModel.setIndexScanStartupCost(startupCost);
    costModel.setIndexScanIncrementalCost(indexScanCost);

    CostEstimatorImpl costEstimator{costModel};

    optimizer::Metadata metadata{{}};
    optimizer::cascades::Memo memo{};

    optimizer::properties::DistributionRequirement dr{{optimizer::DistributionType::Centralized}};
    dr.setDisableExchanges(true);
    optimizer::properties::IndexingRequirement ir{optimizer::IndexReqTarget::Complete,
                                                  /*dedupRID*/ true,
                                                  /*satisfiedPartialIndexesGroupId*/ 0};
    optimizer::properties::PhysProps physProps{};
    optimizer::properties::setProperty(physProps, dr);
    optimizer::properties::setProperty(physProps, ir);

    auto indexScanNode = optimizer::ABT::make<optimizer::IndexScanNode>(
        optimizer::FieldProjectionMap{{}, {optimizer::ProjectionName{"root"}}, {}},
        "c1",
        "a_1",
        optimizer::CompoundIntervalRequirement{},
        false);
    optimizer::ChildPropsType childProps{};
    optimizer::NodeCEMap nodeCEMap{{indexScanNode.cast<optimizer::Node>(), ce}};

    auto costAndCE = costEstimator.deriveCost(
        metadata, memo, physProps, indexScanNode.ref(), childProps, nodeCEMap);

    // Test the cost.
    ASSERT_EQ(startupCost + indexScanCost * costAndCE._ce._value, costAndCE._cost.getCost());
}

TEST(CostEstimatorTest, FilterAndEvaluationCost) {
    const double startupCost = 1;
    const double filterCost = 2;
    const double scanCost = 3;
    const double evalCost = 4;
    const optimizer::CEType ce{1000.0};

    CostModelCoefficients costModel{};
    initializeTestCostModel(costModel);
    costModel.setFilterStartupCost(startupCost);
    costModel.setFilterIncrementalCost(filterCost);
    costModel.setScanStartupCost(startupCost);
    costModel.setScanIncrementalCost(scanCost);
    costModel.setEvalIncrementalCost(evalCost);
    costModel.setEvalStartupCost(startupCost);

    CostEstimatorImpl costEstimator{costModel};

    optimizer::Metadata metadata{{}};
    optimizer::cascades::Memo memo{};

    optimizer::properties::DistributionRequirement dr{{optimizer::DistributionType::Centralized}};
    dr.setDisableExchanges(true);
    optimizer::properties::PhysProps physProps{};
    optimizer::properties::setProperty(physProps, dr);

    auto scanNode = optimizer::ABT::make<optimizer::PhysicalScanNode>(
        optimizer::FieldProjectionMap{{}, {}, {}}, "c1", false);
    optimizer::ChildPropsType childProps{};
    optimizer::NodeCEMap nodeCEMap{{scanNode.cast<optimizer::Node>(), ce}};

    auto projectionNode = optimizer::ABT::make<optimizer::EvaluationNode>(
        "p2",
        optimizer::ABT::make<optimizer::EvalPath>(optimizer::ABT::make<optimizer::PathIdentity>(),
                                                  optimizer::ABT::make<optimizer::Variable>("p1")),
        std::move(scanNode));

    nodeCEMap.emplace(projectionNode.cast<optimizer::Node>(), ce);

    auto filterNode = optimizer::ABT::make<optimizer::FilterNode>(
        optimizer::ABT::make<optimizer::EvalFilter>(
            optimizer::ABT::make<optimizer::PathIdentity>(),
            optimizer::ABT::make<optimizer::Variable>("p1")),
        std::move(projectionNode));

    nodeCEMap.emplace(filterNode.cast<optimizer::Node>(), ce);

    auto costAndCE = costEstimator.deriveCost(
        metadata, memo, physProps, filterNode.ref(), childProps, nodeCEMap);

    // Test the cost. The cost should be the cost of FilterNode's child if FilterNode is "trivial".
    // Same rule applies to 'EvaluationNode'.
    ASSERT_EQ(startupCost + scanCost * costAndCE._ce._value, costAndCE._cost.getCost());

    // Test non-trivial EvaluationNode's cost should account for the cost of the node itself and its
    // child.
    auto scanNodeForEval = optimizer::ABT::make<optimizer::PhysicalScanNode>(
        optimizer::FieldProjectionMap{{}, {}, {}}, "c1", false);
    optimizer::NodeCEMap nodeCEMapForEval{{scanNodeForEval.cast<optimizer::Node>(), ce}};
    auto evalNode = optimizer::ABT::make<optimizer::EvaluationNode>(
        "evalProjA",
        optimizer::ABT::make<optimizer::EvalPath>(
            optimizer::ABT::make<optimizer::PathGet>(
                "a", optimizer::ABT::make<optimizer::PathIdentity>()),
            optimizer::ABT::make<optimizer::Variable>("scanProj1")),
        std::move(scanNodeForEval));

    nodeCEMapForEval.emplace(evalNode.cast<optimizer::Node>(), ce);

    auto costAndCEEval = costEstimator.deriveCost(
        metadata, memo, physProps, evalNode.ref(), childProps, nodeCEMapForEval);

    auto scanNodeCost = startupCost + scanCost * ce._value;
    auto evalNodeCost = startupCost + evalCost * ce._value;
    ASSERT_EQ(scanNodeCost + evalNodeCost, costAndCEEval._cost.getCost());
}

TEST(CostEstimatorTest, MergeJoinCost) {
    const double startupCost = 1;
    const double scanCost = 3;
    const double mergeJoinCost = 5;
    const optimizer::CEType ce{1000.0};

    CostModelCoefficients costModel{};
    initializeTestCostModel(costModel);
    costModel.setScanStartupCost(startupCost);
    costModel.setScanIncrementalCost(scanCost);
    costModel.setMergeJoinStartupCost(startupCost);
    costModel.setMergeJoinIncrementalCost(mergeJoinCost);

    auto scanNodeLeft = optimizer::ABT::make<optimizer::PhysicalScanNode>(
        optimizer::FieldProjectionMap{{}, {}, {}}, "c1", false);
    optimizer::ChildPropsType childProps{};
    optimizer::NodeCEMap nodeCEMap{{scanNodeLeft.cast<optimizer::Node>(), ce}};

    auto scanNodeRight = optimizer::ABT::make<optimizer::PhysicalScanNode>(
        optimizer::FieldProjectionMap{{}, {}, {}}, "c1", false);

    nodeCEMap.emplace(scanNodeRight.cast<optimizer::Node>(), ce);

    auto evalNodeLeft = optimizer::ABT::make<optimizer::EvaluationNode>(
        "evalProjA",
        optimizer::ABT::make<optimizer::EvalPath>(optimizer::ABT::make<optimizer::PathIdentity>(),
                                                  optimizer::ABT::make<optimizer::Variable>("p1")),
        std::move(scanNodeLeft));

    nodeCEMap.emplace(evalNodeLeft.cast<optimizer::Node>(), ce);

    auto evalNodeRight = optimizer::ABT::make<optimizer::EvaluationNode>(
        "evalProjB",
        optimizer::ABT::make<optimizer::EvalPath>(optimizer::ABT::make<optimizer::PathIdentity>(),
                                                  optimizer::ABT::make<optimizer::Variable>("p2")),
        std::move(scanNodeRight));

    nodeCEMap.emplace(evalNodeRight.cast<optimizer::Node>(), ce);

    CostEstimatorImpl costEstimator{costModel};

    optimizer::Metadata metadata{{}};
    optimizer::cascades::Memo memo{};

    optimizer::properties::DistributionRequirement dr{{optimizer::DistributionType::Centralized}};
    dr.setDisableExchanges(true);
    optimizer::properties::PhysProps physProps{};
    optimizer::properties::setProperty(physProps, dr);

    // Test the cost of MergeJoin's children.
    auto leftChildCost = costEstimator.deriveCost(
        metadata, memo, physProps, evalNodeLeft.ref(), childProps, nodeCEMap);
    auto expectedCostChild = startupCost + scanCost * ce._value;

    ASSERT_EQ(expectedCostChild, leftChildCost._cost.getCost());

    auto rightChildCost = costEstimator.deriveCost(
        metadata, memo, physProps, evalNodeRight.ref(), childProps, nodeCEMap);

    ASSERT_EQ(expectedCostChild, rightChildCost._cost.getCost());

    auto joinNode = optimizer::ABT::make<optimizer::MergeJoinNode>(
        optimizer::ProjectionNameVector{"evalProjA"},
        optimizer::ProjectionNameVector{"evalProjB"},
        std::vector<optimizer::CollationOp>{optimizer::CollationOp::Ascending},
        std::move(evalNodeLeft),
        std::move(evalNodeRight));
    nodeCEMap.emplace(joinNode.cast<optimizer::Node>(), ce);

    // Test the cost of MergeJoin. The cost should be based on the cost of MergeJoin's children and
    // itself.
    auto costAndCE =
        costEstimator.deriveCost(metadata, memo, physProps, joinNode.ref(), childProps, nodeCEMap);
    ASSERT_EQ(expectedCostChild * 2 + startupCost + costAndCE._ce._value * 2 * mergeJoinCost,
              costAndCE._cost.getCost());
}
}  // namespace mongo::cost_model
