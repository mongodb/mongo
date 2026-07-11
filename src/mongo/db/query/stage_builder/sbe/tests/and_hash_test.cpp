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

class SbeAndHashTest : public SbeStageBuilderTestFixture {
protected:
    /**
     * Makes a new QuerySolutionNode consisting of a FetchNode as the parent of a a HashAndNode with
     * as many VirtualScanNode children as there are vectors of BSONArrays in docsVec.
     */
    std::unique_ptr<QuerySolutionNode> makeHashAndTree(
        std::vector<std::vector<BSONArray>> docsVec) {
        auto andHashNode = std::make_unique<AndHashNode>();
        for (const auto& docs : docsVec) {
            auto virtScan =
                std::make_unique<VirtualScanNode>(docs, VirtualScanNode::ScanType::kCollScan, true);
            andHashNode->children.push_back(std::move(virtScan));
        }
        return std::move(andHashNode);
    }

    /**
     * Runs a unit test and asserts that the results match the expected docs.
     */
    void runTest(std::vector<std::vector<BSONArray>> docsVec, const BSONArray& expected) {
        auto querySolution = makeQuerySolution(makeHashAndTree(docsVec));

        // Translate the QuerySolution to a PlanStage tree.
        auto [resultSlots, stage, data, _] =
            buildPlanStage(std::move(querySolution), false, nullptr);

        // Prepare the sbe::PlanStage for execution and collect all results.
        auto resultAccessors = prepareTree(&data.env.ctx, stage.get(), resultSlots);
        auto [resultsTag, resultsVal] = getAllResults(stage.get(), resultAccessors[0]);
        sbe::value::ValueGuard resultGuard{resultsTag, resultsVal};

        // Convert the expected results to an sbe value and assert results.
        auto [expectedTag, expectedVal] = stage_builder::makeValue(expected);
        sbe::value::ValueGuard expectedGuard{expectedTag, expectedVal};
        ASSERT_TRUE(valueEquals(resultsTag, resultsVal, expectedTag, expectedVal));
    }
};

TEST_F(SbeAndHashTest, TestTwoIndexIntersection) {
    auto docs = std::vector<std::vector<BSONArray>>{
        {BSON_ARRAY(1 << BSON("_id" << 1 << "a" << 1 << "b" << 2)),
         BSON_ARRAY(2 << BSON("_id" << 2 << "a" << 2 << "b" << 2)),
         BSON_ARRAY(3 << BSON("_id" << 3 << "a" << 3 << "b" << 2))},
        {BSON_ARRAY(2 << BSON("_id" << 2 << "a" << 2 << "b" << 2)),
         BSON_ARRAY(3 << BSON("_id" << 3 << "a" << 3 << "b" << 2)),
         BSON_ARRAY(4 << BSON("_id" << 4 << "a" << 1))}};
    auto expected = BSON_ARRAY(BSON("_id" << 2 << "a" << 2 << "b" << 2)
                               << BSON("_id" << 3 << "a" << 3 << "b" << 2));
    runTest(docs, expected);
}

TEST_F(SbeAndHashTest, TestThreeIndexIntersection) {
    auto docs = std::vector<std::vector<BSONArray>>{
        {BSON_ARRAY(1 << BSON("_id" << 1 << "a" << 1 << "b" << 2)),
         BSON_ARRAY(2 << BSON("_id" << 2 << "a" << 2 << "b" << 2)),
         BSON_ARRAY(3 << BSON("_id" << 3 << "a" << 3 << "b" << 2))},
        {BSON_ARRAY(2 << BSON("_id" << 2 << "a" << 2 << "b" << 2)),
         BSON_ARRAY(3 << BSON("_id" << 3 << "a" << 3 << "b" << 2)),
         BSON_ARRAY(4 << BSON("_id" << 4 << "a" << 1))},
        {BSON_ARRAY(1 << BSON("_id" << 1 << "a" << 1 << "b" << 2)),
         BSON_ARRAY(2 << BSON("_id" << 2 << "a" << 2 << "b" << 2))}};
    auto expected = BSON_ARRAY(BSON("_id" << 2 << "a" << 2 << "b" << 2));
    runTest(docs, expected);
}

TEST_F(SbeAndHashTest, TestManyIndexIntersection) {
    std::vector<std::vector<BSONArray>> docs;

    for (int i = 0; i < 10; i++) {
        docs.push_back(std::vector<BSONArray>{
            BSON_ARRAY(243 << BSON("_id" << 243 << "a" << 1 << "b" << 2 << "c" << 23)),
            BSON_ARRAY(566 << BSON("_id" << 566 << "a" << 1 << "b" << 2 << "c" << 42)),
            BSON_ARRAY(567 << BSON("_id" << 567 << "a" << 1 << "b" << 2 << "c" << 49)),
            BSON_ARRAY((i * 3) << BSON("_id" << (i * 3) << "a" << 1 << "b" << 2 << "c" << 23)),
            BSON_ARRAY((i * 3 + 1) << BSON("_id" << (i * 3 + 1) << "a" << i << "b" << 2)),
            BSON_ARRAY((i * 3 + 2) << BSON("_id" << (i * 3 + 2) << "a" << 3 << "b" << (i % 6)))});
    }

    auto expected = BSON_ARRAY(BSON("_id" << 243 << "a" << 1 << "b" << 2 << "c" << 23)
                               << BSON("_id" << 566 << "a" << 1 << "b" << 2 << "c" << 42)
                               << BSON("_id" << 567 << "a" << 1 << "b" << 2 << "c" << 49));
    runTest(docs, expected);
}

TEST_F(SbeAndHashTest, TestTwoIdenticalIndexIntersection) {
    auto docs = std::vector<std::vector<BSONArray>>{
        {BSON_ARRAY(1 << BSON("_id" << 1 << "a" << 1 << "b" << 2)),
         BSON_ARRAY(2 << BSON("_id" << 2 << "a" << 2 << "b" << 2)),
         BSON_ARRAY(3 << BSON("_id" << 3 << "a" << 3 << "b" << 2))},
        {BSON_ARRAY(1 << BSON("_id" << 1 << "a" << 1 << "b" << 2)),
         BSON_ARRAY(2 << BSON("_id" << 2 << "a" << 2 << "b" << 2)),
         BSON_ARRAY(3 << BSON("_id" << 3 << "a" << 3 << "b" << 2))}};
    auto expected = BSON_ARRAY(BSON("_id" << 1 << "a" << 1 << "b" << 2)
                               << BSON("_id" << 2 << "a" << 2 << "b" << 2)
                               << BSON("_id" << 3 << "a" << 3 << "b" << 2));
    runTest(docs, expected);
}

TEST_F(SbeAndHashTest, TestTwoIndexEmptyIntersection) {
    auto docs = std::vector<std::vector<BSONArray>>{
        {BSON_ARRAY(1 << BSON("_id" << 1 << "a" << 1 << "b" << 2)),
         BSON_ARRAY(2 << BSON("_id" << 2 << "a" << 2 << "b" << 2)),
         BSON_ARRAY(3 << BSON("_id" << 3 << "a" << 3 << "b" << 2))},
        {BSON_ARRAY(4 << BSON("_id" << 4 << "a" << 2 << "b" << 2)),
         BSON_ARRAY(5 << BSON("_id" << 5 << "a" << 3 << "b" << 2)),
         BSON_ARRAY(6 << BSON("_id" << 6 << "a" << 1))}};
    auto expected = mongo::BSONArray();
    runTest(docs, expected);
}

TEST_F(SbeAndHashTest, TestTwoIndexOneOrBothEmptyIntersection) {
    // Test left index empty.
    auto docsLeftEmpty = std::vector<std::vector<BSONArray>>{
        {},
        {BSON_ARRAY(2 << BSON("_id" << 2 << "a" << 2 << "b" << 2)),
         BSON_ARRAY(3 << BSON("_id" << 3 << "a" << 3 << "b" << 2)),
         BSON_ARRAY(4 << BSON("_id" << 4 << "a" << 1))}};
    auto expectedLeftEmpty = mongo::BSONArray();
    runTest(docsLeftEmpty, expectedLeftEmpty);

    // Test right index empty.
    auto docsRightEmpty = std::vector<std::vector<BSONArray>>{
        {BSON_ARRAY(4 << BSON("_id" << 4 << "a" << 2 << "b" << 2)),
         BSON_ARRAY(5 << BSON("_id" << 5 << "a" << 3 << "b" << 2)),
         BSON_ARRAY(6 << BSON("_id" << 6 << "a" << 1))},
        {}};
    auto expectedRightEmpty = mongo::BSONArray();
    runTest(docsLeftEmpty, expectedLeftEmpty);

    // Test both indices empty.
    auto docsEmpty = std::vector<std::vector<BSONArray>>{{}, {}};
    auto expectedEmpty = mongo::BSONArray();
    runTest(docsEmpty, expectedEmpty);
}

TEST_F(SbeAndHashTest, TestTwoIndexArrays) {
    auto docs = std::vector<std::vector<BSONArray>>{
        {BSON_ARRAY(1 << BSON("_id" << 1 << "a" << BSON_ARRAY(1 << 2 << 3))),
         BSON_ARRAY(2 << BSON("_id" << 2 << "a" << BSON_ARRAY(1 << 2 << 3))),
         BSON_ARRAY(3 << BSON("_id" << 3 << "a" << BSON_ARRAY(4 << 5 << 6))),
         BSON_ARRAY(4 << BSON("_id" << 4 << "a" << BSON_ARRAY(7 << 8 << 9)))},
        {BSON_ARRAY(1 << BSON("_id" << 1 << "a" << BSON_ARRAY(1 << 2 << 3))),
         BSON_ARRAY(2 << BSON("_id" << 2 << "a" << BSON_ARRAY(1 << 2 << 3))),
         BSON_ARRAY(3 << BSON("_id" << 3 << "a" << BSON_ARRAY(4 << 5 << 6)))}};
    auto expected = BSON_ARRAY(BSON("_id" << 1 << "a" << BSON_ARRAY(1 << 2 << 3))
                               << BSON("_id" << 2 << "a" << BSON_ARRAY(1 << 2 << 3))
                               << BSON("_id" << 3 << "a" << BSON_ARRAY(4 << 5 << 6)));
    runTest(docs, expected);
}

}  // namespace mongo
