/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/query/compiler/physical_model/query_solution/query_solution.h"
#include "mongo/db/query/shard_filterer_factory_interface.h"
#include "mongo/db/query/stage_builder/sbe/builder.h"
#include "mongo/db/query/stage_builder/sbe/gen_helpers.h"
#include "mongo/db/query/stage_builder/sbe/tests/sbe_builder_test_fixture.h"
#include "mongo/unittest/unittest.h"

#include <memory>
#include <utility>
#include <vector>

namespace mongo {

const NamespaceString kTestNss =
    NamespaceString::createNamespaceString_forTest("TestDB", "TestColl");

class SbeAndSortedTest : public SbeStageBuilderTestFixture {
protected:
    /**
     * Builds an AndSortedNode with as many VirtualScanNode children as there are
     * vectors of BSONArrays in docsVec.
     */
    std::unique_ptr<QuerySolutionNode> makeAndSortedTree(
        std::vector<std::vector<BSONArray>> docsVec) {
        auto andSortedNode = std::make_unique<AndSortedNode>();
        for (const auto& docs : docsVec) {
            auto virtScan =
                std::make_unique<VirtualScanNode>(docs, VirtualScanNode::ScanType::kCollScan, true);
            andSortedNode->children.push_back(std::move(virtScan));
        }
        return std::move(andSortedNode);
    }

    /**
     * Runs a unit test and asserts that the results match the expected docs.
     */
    void runTest(std::vector<std::vector<BSONArray>> docsVec, const BSONArray& expected) {
        auto querySolution = makeQuerySolution(makeAndSortedTree(docsVec));

        // Translate the QuerySolution to a PlanStage tree.
        auto [resultSlots, stage, data, _] =
            buildPlanStage(std::move(querySolution), false, nullptr);

        // Prepare the sbe::PlanStage for execution and collect all results.
        auto resultAccessors = prepareTree(&data.env.ctx, stage.get(), resultSlots);
        auto [resultsTag, resultsVal] = getAllResults(stage.get(), resultAccessors[0]);
        sbe::value::ValueGuard resultGuard{resultsTag, resultsVal};

        // Convert the expected results to an SBE value and assert results.
        auto [expectedTag, expectedVal] = stage_builder::makeValue(expected);
        sbe::value::ValueGuard expectedGuard{expectedTag, expectedVal};
        ASSERT_TRUE(valueEquals(resultsTag, resultsVal, expectedTag, expectedVal));
    }
};

TEST_F(SbeAndSortedTest, TestTwoIndexIntersection) {
    auto docs = std::vector<std::vector<BSONArray>>{{BSON_ARRAY(1 << BSON("a" << 1 << "b" << 1)),
                                                     BSON_ARRAY(2 << BSON("a" << 1 << "b" << 2)),
                                                     BSON_ARRAY(3 << BSON("a" << 1 << "b" << 3))},
                                                    {BSON_ARRAY(2 << BSON("a" << 1 << "b" << 2)),
                                                     BSON_ARRAY(3 << BSON("a" << 1 << "b" << 3)),
                                                     BSON_ARRAY(4 << BSON("a" << 100))}};
    auto expected = BSON_ARRAY(BSON("a" << 1 << "b" << 2) << BSON("a" << 1 << "b" << 3));
    runTest(docs, expected);
}

TEST_F(SbeAndSortedTest, TestSameKeyIndexIntersection) {
    auto docs = std::vector<std::vector<BSONArray>>{{BSON_ARRAY(1 << BSON("a" << 1 << "b" << 1)),
                                                     BSON_ARRAY(1 << BSON("a" << 1 << "b" << 2)),
                                                     BSON_ARRAY(1 << BSON("a" << 1 << "b" << 3))},
                                                    {BSON_ARRAY(1 << BSON("a" << 1))}};
    auto expected = BSON_ARRAY(BSON("a" << 1) << BSON("a" << 1) << BSON("a" << 1));
    runTest(docs, expected);
}

TEST_F(SbeAndSortedTest, TestSameKeyOuterDoneEarlyIndexIntersection) {
    auto docs = std::vector<std::vector<BSONArray>>{
        {BSON_ARRAY(1 << BSON("a" << 1 << "b" << 1)),
         BSON_ARRAY(1 << BSON("a" << 1 << "b" << 2)),
         BSON_ARRAY(1 << BSON("a" << 1 << "b" << 3))},
        {BSON_ARRAY(1 << BSON("a" << 1 << "b" << 4)), BSON_ARRAY(2 << BSON("a" << 2 << "b" << 4))}};
    auto expected = BSON_ARRAY(BSON("a" << 1 << "b" << 4)
                               << BSON("a" << 1 << "b" << 4) << BSON("a" << 1 << "b" << 4));
    runTest(docs, expected);
}

TEST_F(SbeAndSortedTest, TestSameKeyOuterToMultiInnerIntersection) {
    auto docs = std::vector<std::vector<BSONArray>>{{BSON_ARRAY(1 << BSON("a" << 1))},
                                                    {BSON_ARRAY(1 << BSON("b" << 2)),
                                                     BSON_ARRAY(1 << BSON("b" << 3)),
                                                     BSON_ARRAY(1 << BSON("b" << 4))}};
    auto expected = BSON_ARRAY(BSON("b" << 2) << BSON("b" << 3) << BSON("b" << 4));
    runTest(docs, expected);
}

TEST_F(SbeAndSortedTest, TestThreeIndexIntersection) {
    auto docs = std::vector<std::vector<BSONArray>>{
        {BSON_ARRAY(1 << BSON("a" << 1 << "b" << 2)),
         BSON_ARRAY(2 << BSON("a" << 2 << "b" << 2)),
         BSON_ARRAY(3 << BSON("a" << 3 << "b" << 2))},
        {BSON_ARRAY(2 << BSON("a" << 2 << "b" << 2)),
         BSON_ARRAY(3 << BSON("a" << 3 << "b" << 2)),
         BSON_ARRAY(4 << BSON("a" << 1))},
        {BSON_ARRAY(1 << BSON("a" << 1 << "b" << 2)), BSON_ARRAY(2 << BSON("a" << 2 << "b" << 2))}};
    auto expected = BSON_ARRAY(BSON("a" << 2 << "b" << 2));
    runTest(docs, expected);
}

TEST_F(SbeAndSortedTest, TestLastRowOnlyIndexIntersection) {
    auto docs =
        std::vector<std::vector<BSONArray>>{{BSON_ARRAY(1 << BSON("a" << 1 << "b" << 1)),
                                             BSON_ARRAY(2 << BSON("a" << 1 << "b" << 2)),
                                             BSON_ARRAY(3 << BSON("a" << 1 << "b" << 3)),
                                             BSON_ARRAY(10 << BSON("a" << 10 << "b" << 10))},
                                            {BSON_ARRAY(4 << BSON("a" << 1 << "b" << 2)),
                                             BSON_ARRAY(5 << BSON("a" << 1 << "b" << 3)),
                                             BSON_ARRAY(6 << BSON("a" << 100)),
                                             BSON_ARRAY(10 << BSON("a" << 10 << "b" << 10))}};
    auto expected = BSON_ARRAY(BSON("a" << 10 << "b" << 10));
    runTest(docs, expected);
}

TEST_F(SbeAndSortedTest, TestNoIndexIntersection) {
    auto docs = std::vector<std::vector<BSONArray>>{{BSON_ARRAY(1 << BSON("a" << 1 << "b" << 1)),
                                                     BSON_ARRAY(2 << BSON("a" << 1 << "b" << 2)),
                                                     BSON_ARRAY(3 << BSON("a" << 1 << "b" << 3))},
                                                    {BSON_ARRAY(4 << BSON("a" << 1 << "b" << 1)),
                                                     BSON_ARRAY(5 << BSON("a" << 1 << "b" << 2)),
                                                     BSON_ARRAY(6 << BSON("a" << 1 << "b" << 3))}};
    runTest(docs, BSONArray());
}
}  // namespace mongo
