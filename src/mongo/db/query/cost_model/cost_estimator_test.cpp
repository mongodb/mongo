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

#include "mongo/db/query/cost_model/cost_estimator.h"
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
    const double ce = 1000;

    CostModelCoefficients costModel{};
    initializeTestCostModel(costModel);
    costModel.setScanStartupCost(startupCost);
    costModel.setScanIncrementalCost(scanCost);

    CostEstimator costEstimator{costModel};

    mongo::optimizer::Metadata metadata{{}};
    mongo::optimizer::cascades::Memo memo{};

    // Mimic properties from PhysicalScan group. Only DistributionRequirement is really required.
    mongo::optimizer::properties::ProjectionRequirement pr{
        {mongo::optimizer::ProjectionNameVector{"root"}}};
    mongo::optimizer::properties::DistributionRequirement dr{
        {mongo::optimizer::DistributionType::Centralized}};
    dr.setDisableExchanges(true);
    mongo::optimizer::properties::IndexingRequirement ir{mongo::optimizer::IndexReqTarget::Complete,
                                                         /*dedupRID*/ true,
                                                         /*satisfiedPartialIndexesGroupId*/ 0};
    mongo::optimizer::properties::PhysProps physProps{};
    mongo::optimizer::properties::setProperty(physProps, pr);
    mongo::optimizer::properties::setProperty(physProps, dr);
    mongo::optimizer::properties::setProperty(physProps, ir);

    auto scanNode = mongo::optimizer::ABT::make<optimizer::PhysicalScanNode>(
        mongo::optimizer::FieldProjectionMap{{}, {mongo::optimizer::ProjectionName{"root"}}, {}},
        "c1",
        false);
    mongo::optimizer::ChildPropsType childProps{};
    mongo::optimizer::NodeCEMap nodeCEMap{{scanNode.cast<mongo::optimizer::Node>(), ce}};

    auto costAndCE =
        costEstimator.deriveCost(metadata, memo, physProps, scanNode.ref(), childProps, nodeCEMap);

    // Test the cost.
    ASSERT_EQ(startupCost + scanCost * costAndCE._ce, costAndCE._cost.getCost());

    // CE is not expected to be adjusted for the given set of PhysicalProperties.
    ASSERT_EQ(ce, costAndCE._ce);
}

/**
 * Test that the PhysicalScan's cost is calculated correctly with adjusted CE.
 */
TEST(CostEstimatorTest, PhysicalScanCostWithAdjustedCE) {
    const double startupCost = 1;
    const double scanCost = 3;
    const double ce = 1000;
    const double limitEstimateCE = 10;

    CostModelCoefficients costModel{};
    initializeTestCostModel(costModel);
    costModel.setScanStartupCost(startupCost);
    costModel.setScanIncrementalCost(scanCost);

    CostEstimator costEstimator{costModel};

    mongo::optimizer::Metadata metadata{{}};
    mongo::optimizer::cascades::Memo memo{};

    mongo::optimizer::properties::DistributionRequirement dr{
        {mongo::optimizer::DistributionType::Centralized}};
    mongo::optimizer::properties::LimitEstimate le{limitEstimateCE};
    mongo::optimizer::properties::PhysProps physProps{};
    mongo::optimizer::properties::setProperty(physProps, dr);
    mongo::optimizer::properties::setProperty(physProps, le);

    auto scanNode = mongo::optimizer::ABT::make<optimizer::PhysicalScanNode>(
        mongo::optimizer::FieldProjectionMap{{}, {}, {}}, "c1", false);
    mongo::optimizer::ChildPropsType childProps{};
    mongo::optimizer::NodeCEMap nodeCEMap{{scanNode.cast<mongo::optimizer::Node>(), ce}};

    auto costAndCE =
        costEstimator.deriveCost(metadata, memo, physProps, scanNode.ref(), childProps, nodeCEMap);

    // Test the cost.
    ASSERT_EQ(startupCost + scanCost * costAndCE._ce, costAndCE._cost.getCost());

    // CE is expected to be adjusted.
    ASSERT_EQ(limitEstimateCE, costAndCE._ce);
}
}  // namespace mongo::cost_model
