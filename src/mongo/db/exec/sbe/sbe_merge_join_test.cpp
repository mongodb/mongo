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

#include "mongo/platform/basic.h"

#include "mongo/db/exec/sbe/sbe_plan_stage_test.h"
#include "mongo/db/exec/sbe/stages/merge_join.h"
#include "mongo/db/query/sbe_stage_builder_helpers.h"

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
    auto innerData = BSON_ARRAY(BSON_ARRAY(1 << BSON_ARRAY("a"
                                                           << "a"))
                                << BSON_ARRAY(1 << BSON_ARRAY("b"
                                                              << "b"))
                                << BSON_ARRAY(1 << BSON_ARRAY("c"
                                                              << "c"))
                                << BSON_ARRAY(2 << BSON_ARRAY("d"
                                                              << "d")));
    // Expect to have every possible combination of outer / inner data joined but should ignore the
    // last row of 'innerData' since there is no match on that key.
    auto expectedData = BSON_ARRAY(BSON_ARRAY(1 << 1 << BSON_ARRAY(1 << 1)
                                                << BSON_ARRAY("a"
                                                              << "a"))
                                   << BSON_ARRAY(1 << 1 << BSON_ARRAY(2 << 2)
                                                   << BSON_ARRAY("a"
                                                                 << "a"))
                                   << BSON_ARRAY(1 << 1 << BSON_ARRAY(3 << 3)
                                                   << BSON_ARRAY("a"
                                                                 << "a"))
                                   << BSON_ARRAY(1 << 1 << BSON_ARRAY(1 << 1)
                                                   << BSON_ARRAY("b"
                                                                 << "b"))
                                   << BSON_ARRAY(1 << 1 << BSON_ARRAY(2 << 2)
                                                   << BSON_ARRAY("b"
                                                                 << "b"))
                                   << BSON_ARRAY(1 << 1 << BSON_ARRAY(3 << 3)
                                                   << BSON_ARRAY("b"
                                                                 << "b"))
                                   << BSON_ARRAY(1 << 1 << BSON_ARRAY(1 << 1)
                                                   << BSON_ARRAY("c"
                                                                 << "c"))
                                   << BSON_ARRAY(1 << 1 << BSON_ARRAY(2 << 2)
                                                   << BSON_ARRAY("c"
                                                                 << "c"))
                                   << BSON_ARRAY(1 << 1 << BSON_ARRAY(3 << 3)
                                                   << BSON_ARRAY("c"
                                                                 << "c")));

    runTest(outerData, innerData, expectedData);
}

TEST_F(MergeJoinStageTest, MergeJoinSingleToManyIdenticalKeys) {
    auto outerData =
        BSON_ARRAY(BSON_ARRAY(1 << BSON_ARRAY(1 << 1))
                   << BSON_ARRAY(1 << BSON_ARRAY(2 << 2)) << BSON_ARRAY(1 << BSON_ARRAY(3 << 3)));
    auto innerData = BSON_ARRAY(BSON_ARRAY(1 << BSON_ARRAY("a"
                                                           << "a")));
    auto expectedData = BSON_ARRAY(BSON_ARRAY(1 << 1 << BSON_ARRAY(1 << 1)
                                                << BSON_ARRAY("a"
                                                              << "a"))
                                   << BSON_ARRAY(1 << 1 << BSON_ARRAY(2 << 2)
                                                   << BSON_ARRAY("a"
                                                                 << "a"))
                                   << BSON_ARRAY(1 << 1 << BSON_ARRAY(3 << 3)
                                                   << BSON_ARRAY("a"
                                                                 << "a")));

    runTest(outerData, innerData, expectedData);
}

TEST_F(MergeJoinStageTest, MergeJoinUniqueKeys) {
    auto outerData =
        BSON_ARRAY(BSON_ARRAY(1 << BSON_ARRAY(1 << 1))
                   << BSON_ARRAY(2 << BSON_ARRAY(2 << 2)) << BSON_ARRAY(3 << BSON_ARRAY(3 << 3)));
    auto innerData = BSON_ARRAY(BSON_ARRAY(2 << BSON_ARRAY("a"
                                                           << "a"
                                                           << "a"))
                                << BSON_ARRAY(3 << BSON_ARRAY("b"
                                                              << "b"
                                                              << "b"))
                                << BSON_ARRAY(4 << BSON("c" << 100)));
    // Expect to join on keys '2' and '3' and have appropriate projects from both outer and inner.
    auto expectedData = BSON_ARRAY(BSON_ARRAY(2 << 2 << BSON_ARRAY(2 << 2)
                                                << BSON_ARRAY("a"
                                                              << "a"
                                                              << "a"))
                                   << BSON_ARRAY(3 << 3 << BSON_ARRAY(3 << 3)
                                                   << BSON_ARRAY("b"
                                                                 << "b"
                                                                 << "b")));

    runTest(outerData, innerData, expectedData);
}

TEST_F(MergeJoinStageTest, MergeJoinLastRowMatch) {
    auto outerData =
        BSON_ARRAY(BSON_ARRAY(1 << BSON_ARRAY(1 << 1)) << BSON_ARRAY(2 << BSON_ARRAY(2 << 2))
                                                       << BSON_ARRAY(10 << BSON_ARRAY(10 << 10)));
    auto innerData = BSON_ARRAY(BSON_ARRAY(4 << BSON_ARRAY("a"
                                                           << "a"
                                                           << "a"))
                                << BSON_ARRAY(5 << BSON_ARRAY("b"
                                                              << "b"
                                                              << "b"))
                                << BSON_ARRAY(10 << BSON_ARRAY("c"
                                                               << "c"
                                                               << "c")));
    // Expect to skip all keys except for key '10' on last rows of both outer and inner sides.
    auto expectedData = BSON_ARRAY(BSON_ARRAY(10 << 10 << BSON_ARRAY(10 << 10)
                                                 << BSON_ARRAY("c"
                                                               << "c"
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
    auto innerData = BSON_ARRAY(BSON_ARRAY(3 << BSON_ARRAY("a"
                                                           << "a"
                                                           << "a"))
                                << BSON_ARRAY(2 << BSON_ARRAY("b"
                                                              << "b"
                                                              << "b"))
                                << BSON_ARRAY(1 << BSON_ARRAY("c"
                                                              << "c"
                                                              << "c")));
    auto expectedData = BSON_ARRAY(BSON_ARRAY(2 << 2 << BSON_ARRAY(2 << 2)
                                                << BSON_ARRAY("b"
                                                              << "b"
                                                              << "b"))
                                   << BSON_ARRAY(1 << 1 << BSON_ARRAY(1 << 1)
                                                   << BSON_ARRAY("c"
                                                                 << "c"
                                                                 << "c")));

    runTest(outerData, innerData, expectedData, value::SortDirection::Descending);
}
}  // namespace mongo::sbe
