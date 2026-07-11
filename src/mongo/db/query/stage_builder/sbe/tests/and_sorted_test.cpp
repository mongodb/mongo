// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/query/compiler/physical_model/query_solution/query_solution.h"
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
