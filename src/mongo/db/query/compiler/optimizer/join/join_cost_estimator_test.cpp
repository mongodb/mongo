/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/db/query/compiler/optimizer/join/join_cost_estimator_impl.h"
#include "mongo/db/query/compiler/optimizer/join/unit_test_helpers.h"
#include "mongo/unittest/unittest.h"

namespace mongo::join_ordering {

class JoinCostEstimatorTest : public JoinOrderingTestFixture {};

TEST_F(JoinCostEstimatorTest, LargerCollectionHasHigherCost) {
    auto smallNss = NamespaceString::createNamespaceString_forTest("foo");
    auto largeNss = NamespaceString::createNamespaceString_forTest("bar");
    auto smallNodeId = graph.addNode(smallNss, makeCanonicalQuery(smallNss), boost::none);
    auto largeNodeId = graph.addNode(largeNss, makeCanonicalQuery(largeNss), boost::none);

    auto jCtx = makeContext();

    std::unique_ptr<JoinCardinalityEstimator> cardEstimator =
        std::make_unique<FakeJoinCardinalityEstimator>(jCtx);
    CatalogStats catalogStats{.collStats = {
                                  {smallNss, CollectionStats{.allocatedDataPageBytes = 32 * 1024}},
                                  {largeNss, CollectionStats{.allocatedDataPageBytes = 64 * 1024}},
                              }};

    std::unique_ptr<JoinCostEstimator> joinCostEstimator =
        std::make_unique<JoinCostEstimatorImpl>(jCtx, *cardEstimator, catalogStats);

    auto smallCost = joinCostEstimator->costCollScanFragment(*smallNodeId);
    auto largeCost = joinCostEstimator->costCollScanFragment(*largeNodeId);
    std::cout << smallCost.getTotalCost().toDouble() << " " << largeCost.getTotalCost().toDouble()
              << std::endl;
    ASSERT_GT(largeCost.getTotalCost(), smallCost.getTotalCost());
}

}  // namespace mongo::join_ordering
