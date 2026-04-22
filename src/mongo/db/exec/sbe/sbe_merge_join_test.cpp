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
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/exec/sbe/expressions/expression.h"
#include "mongo/db/exec/sbe/sbe_plan_stage_test.h"
#include "mongo/db/exec/sbe/stages/merge_join.h"
#include "mongo/db/exec/sbe/stages/stages.h"
#include "mongo/db/exec/sbe/values/slot.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/query/compiler/physical_model/query_solution/stage_types.h"
#include "mongo/db/query/stage_builder/sbe/gen_helpers.h"
#include "mongo/idl/server_parameter_test_controller.h"
#include "mongo/unittest/unittest.h"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

namespace mongo::sbe {
/**
 * This file contains tests for sbe::MergeJoinStage.
 */
class MergeJoinStageTest : public PlanStageTestFixture {
public:
    /**
     * Constructs and returns a MergeJoin stage with a virtual scan node outer child stage
     * containing 'outerSideData' and a virtual scan node inner child stage containing
     * 'innerSideData'.
     */
    std::pair<std::vector<value::SlotAccessor*>, std::unique_ptr<PlanStage>> makeMergeJoin(
        const BSONArray& outerSideData,
        const BSONArray& innerSideData,
        value::SortDirection sortDir) {

        auto [outerTag, outerVal] = stage_builder::makeValue(outerSideData);
        auto [innerTag, innerVal] = stage_builder::makeValue(innerSideData);

        auto [outerSlots, outerStage] =
            generateVirtualScanMulti(2 /* Two slots for key, project. */, outerTag, outerVal);
        auto [innerSlots, innerStage] =
            generateVirtualScanMulti(2 /* Two slots for key, project. */, innerTag, innerVal);

        auto outerKeySlots = makeSV(outerSlots[0]);
        auto outerProjectsSlots = makeSV(outerSlots[1]);
        auto innerKeySlots = makeSV(innerSlots[0]);
        auto innerProjectsSlots = makeSV(innerSlots[1]);

        std::vector<value::SortDirection> sortDirs(1, sortDir);

        auto mergeJoin = makeS<MergeJoinStage>(std::move(outerStage),
                                               std::move(innerStage),
                                               outerKeySlots,
                                               outerProjectsSlots,
                                               innerKeySlots,
                                               innerProjectsSlots,
                                               std::move(sortDirs),
                                               kEmptyPlanNodeId);

        // Get back a full generated row with the keys joined upon, projects from the outer side,
        // and projects from the inner side.
        auto resultSlots = makeSV(outerSlots[0], innerSlots[0], outerSlots[1], innerSlots[1]);

        auto ctx = makeCompileCtx();
        auto resultAccessors = prepareTree(ctx.get(), mergeJoin.get(), resultSlots);

        return {resultAccessors, std::move(mergeJoin)};
    }

    /**
     * Runs a MergeJoin stage test and asserts that the generated results equal the expected data.
     *
     * Optionally accepts a 'dataSortDir' sort direction of the data that defaults to 'Ascending'.
     **/
    void runTest(const BSONArray& outerData,
                 const BSONArray& innerData,
                 const BSONArray& expectedData,
                 value::SortDirection dataSortDir = value::SortDirection::Ascending) {

        auto [resultAccessors, mergeJoinStage] = makeMergeJoin(outerData, innerData, dataSortDir);

        auto [resultsTag, resultsVal] = getAllResultsMulti(mergeJoinStage.get(), resultAccessors);
        value::ValueGuard resultGuard{resultsTag, resultsVal};

        auto [expectedTag, expectedVal] = stage_builder::makeValue(expectedData);
        sbe::value::ValueGuard expectedGuard{expectedTag, expectedVal};

        ASSERT_TRUE(valueEquals(resultsTag, resultsVal, expectedTag, expectedVal));
    }
};

TEST_F(MergeJoinStageTest, MergeJoinManyToManyIdenticalKeys) {
    auto outerData =
        BSON_ARRAY(BSON_ARRAY(1 << BSON_ARRAY(1 << 1))
                   << BSON_ARRAY(1 << BSON_ARRAY(2 << 2)) << BSON_ARRAY(1 << BSON_ARRAY(3 << 3)));
    auto innerData = BSON_ARRAY(BSON_ARRAY(1 << BSON_ARRAY("a" << "a"))
                                << BSON_ARRAY(1 << BSON_ARRAY("b" << "b"))
                                << BSON_ARRAY(1 << BSON_ARRAY("c" << "c"))
                                << BSON_ARRAY(2 << BSON_ARRAY("d" << "d")));
    // Expect to have every possible combination of outer / inner data joined but should ignore the
    // last row of 'innerData' since there is no match on that key.
    auto expectedData =
        BSON_ARRAY(BSON_ARRAY(1 << 1 << BSON_ARRAY(1 << 1) << BSON_ARRAY("a" << "a"))
                   << BSON_ARRAY(1 << 1 << BSON_ARRAY(2 << 2) << BSON_ARRAY("a" << "a"))
                   << BSON_ARRAY(1 << 1 << BSON_ARRAY(3 << 3) << BSON_ARRAY("a" << "a"))
                   << BSON_ARRAY(1 << 1 << BSON_ARRAY(1 << 1) << BSON_ARRAY("b" << "b"))
                   << BSON_ARRAY(1 << 1 << BSON_ARRAY(2 << 2) << BSON_ARRAY("b" << "b"))
                   << BSON_ARRAY(1 << 1 << BSON_ARRAY(3 << 3) << BSON_ARRAY("b" << "b"))
                   << BSON_ARRAY(1 << 1 << BSON_ARRAY(1 << 1) << BSON_ARRAY("c" << "c"))
                   << BSON_ARRAY(1 << 1 << BSON_ARRAY(2 << 2) << BSON_ARRAY("c" << "c"))
                   << BSON_ARRAY(1 << 1 << BSON_ARRAY(3 << 3) << BSON_ARRAY("c" << "c")));

    runTest(outerData, innerData, expectedData);
}

TEST_F(MergeJoinStageTest, MergeJoinSingleToManyIdenticalKeys) {
    auto outerData =
        BSON_ARRAY(BSON_ARRAY(1 << BSON_ARRAY(1 << 1))
                   << BSON_ARRAY(1 << BSON_ARRAY(2 << 2)) << BSON_ARRAY(1 << BSON_ARRAY(3 << 3)));
    auto innerData = BSON_ARRAY(BSON_ARRAY(1 << BSON_ARRAY("a" << "a")));
    auto expectedData =
        BSON_ARRAY(BSON_ARRAY(1 << 1 << BSON_ARRAY(1 << 1) << BSON_ARRAY("a" << "a"))
                   << BSON_ARRAY(1 << 1 << BSON_ARRAY(2 << 2) << BSON_ARRAY("a" << "a"))
                   << BSON_ARRAY(1 << 1 << BSON_ARRAY(3 << 3) << BSON_ARRAY("a" << "a")));

    runTest(outerData, innerData, expectedData);
}

TEST_F(MergeJoinStageTest, MergeJoinUniqueKeys) {
    auto outerData =
        BSON_ARRAY(BSON_ARRAY(1 << BSON_ARRAY(1 << 1))
                   << BSON_ARRAY(2 << BSON_ARRAY(2 << 2)) << BSON_ARRAY(3 << BSON_ARRAY(3 << 3)));
    auto innerData = BSON_ARRAY(BSON_ARRAY(2 << BSON_ARRAY("a" << "a"
                                                               << "a"))
                                << BSON_ARRAY(3 << BSON_ARRAY("b" << "b"
                                                                  << "b"))
                                << BSON_ARRAY(4 << BSON("c" << 100)));
    // Expect to join on keys '2' and '3' and have appropriate projects from both outer and inner.
    auto expectedData = BSON_ARRAY(BSON_ARRAY(2 << 2 << BSON_ARRAY(2 << 2)
                                                << BSON_ARRAY("a" << "a"
                                                                  << "a"))
                                   << BSON_ARRAY(3 << 3 << BSON_ARRAY(3 << 3)
                                                   << BSON_ARRAY("b" << "b"
                                                                     << "b")));

    runTest(outerData, innerData, expectedData);
}

TEST_F(MergeJoinStageTest, MergeJoinLastRowMatch) {
    auto outerData =
        BSON_ARRAY(BSON_ARRAY(1 << BSON_ARRAY(1 << 1)) << BSON_ARRAY(2 << BSON_ARRAY(2 << 2))
                                                       << BSON_ARRAY(10 << BSON_ARRAY(10 << 10)));
    auto innerData = BSON_ARRAY(BSON_ARRAY(4 << BSON_ARRAY("a" << "a"
                                                               << "a"))
                                << BSON_ARRAY(5 << BSON_ARRAY("b" << "b"
                                                                  << "b"))
                                << BSON_ARRAY(10 << BSON_ARRAY("c" << "c"
                                                                   << "c")));
    // Expect to skip all keys except for key '10' on last rows of both outer and inner sides.
    auto expectedData = BSON_ARRAY(BSON_ARRAY(10 << 10 << BSON_ARRAY(10 << 10)
                                                 << BSON_ARRAY("c" << "c"
                                                                   << "c")));

    runTest(outerData, innerData, expectedData);
}

TEST_F(MergeJoinStageTest, MergeJoinNoOuterMatch) {
    auto outerData = BSON_ARRAY(BSON_ARRAY(1 << BSON_ARRAY(1 << 1)));
    auto innerData =
        BSON_ARRAY(BSON_ARRAY(4 << BSON_ARRAY(1 << 1))
                   << BSON_ARRAY(5 << BSON_ARRAY(2 << 2)) << BSON_ARRAY(6 << BSON_ARRAY(3 << 3)));
    auto expectedData = BSONArray();

    runTest(outerData, innerData, expectedData);
}

TEST_F(MergeJoinStageTest, MergeJoinNoMatchingKeys) {
    auto outerData =
        BSON_ARRAY(BSON_ARRAY(1 << BSON_ARRAY(1 << 1))
                   << BSON_ARRAY(2 << BSON_ARRAY(2 << 2)) << BSON_ARRAY(3 << BSON_ARRAY(3 << 3)));
    auto innerData =
        BSON_ARRAY(BSON_ARRAY(4 << BSON_ARRAY(1 << 1))
                   << BSON_ARRAY(5 << BSON_ARRAY(2 << 2)) << BSON_ARRAY(6 << BSON_ARRAY(3 << 3)));
    auto expectedData = BSONArray();

    runTest(outerData, innerData, expectedData);
}

TEST_F(MergeJoinStageTest, MergeJoinEmptyOuterInner) {
    auto outerData = BSONArray();
    auto innerData = BSONArray();
    auto expectedData = BSONArray();

    runTest(outerData, innerData, expectedData);
}

TEST_F(MergeJoinStageTest, MergeJoinEmptyOuter) {
    auto outerData = BSONArray();
    auto innerData =
        BSON_ARRAY(BSON_ARRAY(1 << BSON_ARRAY(1 << 1))
                   << BSON_ARRAY(2 << BSON_ARRAY(2 << 2)) << BSON_ARRAY(3 << BSON_ARRAY(3 << 3)));
    auto expectedData = BSONArray();

    runTest(outerData, innerData, expectedData);
}

TEST_F(MergeJoinStageTest, MergeJoinEmptyInner) {
    auto outerData =
        BSON_ARRAY(BSON_ARRAY(1 << BSON_ARRAY(1 << 1))
                   << BSON_ARRAY(2 << BSON_ARRAY(2 << 2)) << BSON_ARRAY(3 << BSON_ARRAY(3 << 3)));
    auto innerData = BSONArray();
    auto expectedData = BSONArray();

    runTest(outerData, innerData, expectedData);
}

TEST_F(MergeJoinStageTest, MergeJoinSortDirectionDescending) {
    auto outerData =
        BSON_ARRAY(BSON_ARRAY(10 << BSON_ARRAY(3 << 3))
                   << BSON_ARRAY(2 << BSON_ARRAY(2 << 2)) << BSON_ARRAY(1 << BSON_ARRAY(1 << 1)));
    auto innerData = BSON_ARRAY(BSON_ARRAY(3 << BSON_ARRAY("a" << "a"
                                                               << "a"))
                                << BSON_ARRAY(2 << BSON_ARRAY("b" << "b"
                                                                  << "b"))
                                << BSON_ARRAY(1 << BSON_ARRAY("c" << "c"
                                                                  << "c")));
    auto expectedData = BSON_ARRAY(BSON_ARRAY(2 << 2 << BSON_ARRAY(2 << 2)
                                                << BSON_ARRAY("b" << "b"
                                                                  << "b"))
                                   << BSON_ARRAY(1 << 1 << BSON_ARRAY(1 << 1)
                                                   << BSON_ARRAY("c" << "c"
                                                                     << "c")));

    runTest(outerData, innerData, expectedData, value::SortDirection::Descending);
}

TEST_F(MergeJoinStageTest, MergeJoinMemoryLimitExceeded) {
    // Set a 1-byte limit so any buffered row immediately exceeds it.
    RAIIServerParameterControllerForTest maxMemoryLimit(
        "internalSlotBasedExecutionMergeJoinStageMaxMemoryBytes", 1);

    // Two outer rows share the same key so the second emplace_back enters the loop and hits the
    // memory check (12321800). A single outer row would not reach that path.
    auto outerData =
        BSON_ARRAY(BSON_ARRAY(1 << BSON_ARRAY(1 << 1)) << BSON_ARRAY(1 << BSON_ARRAY(2 << 2)));
    auto innerData = BSON_ARRAY(BSON_ARRAY(1 << BSON_ARRAY("a" << "a")));

    auto [resultAccessors, mergeJoinStage] =
        makeMergeJoin(outerData, innerData, value::SortDirection::Ascending);

    mergeJoinStage->open(false);

    // First getNext() takes the initial-match path (no limit check) and returns a result.
    ASSERT_EQ(mergeJoinStage->getNext(), PlanState::ADVANCED);
    ASSERT_THROWS_CODE(mergeJoinStage->getNext(), DBException, 12321800);
    mergeJoinStage->close();
}

TEST_F(MergeJoinStageTest, MergeJoinMemoryTracking) {
    // Three outer rows all share key=1; they are all buffered together when the first match is
    // found. Scalar project values (10, 20, 30 outer; 100, 200 inner) for easy verification.
    auto outerData = BSON_ARRAY(BSON_ARRAY(1 << 10) << BSON_ARRAY(1 << 20) << BSON_ARRAY(1 << 30));
    auto innerData = BSON_ARRAY(BSON_ARRAY(1 << 100) << BSON_ARRAY(1 << 200));

    auto [resultAccessors, mergeJoinStage] =
        makeMergeJoin(outerData, innerData, value::SortDirection::Ascending);

    mergeJoinStage->open(false);

    // After the first result the outer buffer holds all 3 outer rows with key=1.
    ASSERT_EQ(mergeJoinStage->getNext(), PlanState::ADVANCED);
    ASSERT_GT(mergeJoinStage->getMemoryTracker()->inUseTrackedMemoryBytes(), 0);

    // Collect results. Start with the one already fetched, then drain the rest.
    std::vector<std::pair<int32_t, int32_t>> results;
    auto collectRow = [&] {
        auto [outerKeyTag, outerKeyVal] = resultAccessors[0]->getViewOfValue();
        ASSERT_EQ(outerKeyTag, value::TypeTags::NumberInt32);
        ASSERT_EQ(value::bitcastTo<int32_t>(outerKeyVal), 1);
        auto [innerKeyTag, innerKeyVal] = resultAccessors[1]->getViewOfValue();
        ASSERT_EQ(innerKeyTag, value::TypeTags::NumberInt32);
        ASSERT_EQ(value::bitcastTo<int32_t>(innerKeyVal), 1);
        auto [outerProjTag, outerProjVal] = resultAccessors[2]->getViewOfValue();
        ASSERT_EQ(outerProjTag, value::TypeTags::NumberInt32);
        auto [innerProjTag, innerProjVal] = resultAccessors[3]->getViewOfValue();
        ASSERT_EQ(innerProjTag, value::TypeTags::NumberInt32);
        results.emplace_back(value::bitcastTo<int32_t>(outerProjVal),
                             value::bitcastTo<int32_t>(innerProjVal));
    };

    collectRow();
    while (mergeJoinStage->getNext() == PlanState::ADVANCED) {
        collectRow();
    }

    // 3 outer rows x 2 inner rows = 6 results.
    ASSERT_EQ(results.size(), 6u);
    // Each outer project value {10, 20, 30} must appear paired with each inner {100, 200}.
    std::sort(results.begin(), results.end());
    ASSERT_EQ(results,
              (std::vector<std::pair<int32_t, int32_t>>{
                  {10, 100}, {10, 200}, {20, 100}, {20, 200}, {30, 100}, {30, 200}}));

    mergeJoinStage->close();

    // After close the buffer is cleared and the peak is captured in specific stats.
    ASSERT_EQ(mergeJoinStage->getMemoryTracker()->inUseTrackedMemoryBytes(), 0);
    auto* stats = static_cast<const MergeJoinStats*>(mergeJoinStage->getSpecificStats());
    ASSERT_GT(stats->peakTrackedMemBytes, 0);
}
}  // namespace mongo::sbe
