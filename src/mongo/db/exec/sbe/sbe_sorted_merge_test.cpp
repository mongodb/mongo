/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/exec/sbe/sbe_plan_stage_test.h"
#include "mongo/db/exec/sbe/stages/sorted_merge.h"
#include "mongo/db/exec/sbe/stages/stages.h"
#include "mongo/db/exec/sbe/values/slot.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/query/compiler/physical_model/query_solution/stage_types.h"
#include "mongo/db/query/stage_builder/sbe/gen_helpers.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

#include <memory>
#include <utility>
#include <vector>

#include <absl/container/inlined_vector.h>
#include <boost/move/utility_core.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>

namespace mongo::sbe {
/**
 * This file contains tests for sbe::SortedMergeStage.
 */
class SortedMergeStageTest : public PlanStageTestFixture {
public:
    /**
     * Constructs a SortedMerge stage with MockCollectionScans as children.
     *
     * 'inputStreams' is an array of arrays. Each top-level array represents one input stream.
     * Each subarray is expected to have one element per slot.
     *
     * 'sortDir' represents the sort direction. The first K slots are assumed to be the sort
     * key.
     */
    std::pair<std::vector<value::SlotAccessor*>, std::unique_ptr<PlanStage>> makeSortedMerge(
        std::vector<BSONArray> inputStreams, std::vector<value::SortDirection> sortDir) {
        invariant(inputStreams.size() > 0);
        const int numKeySlots = sortDir.size();
        const int numSlots = checkNumSlots(inputStreams);
        invariant(numKeySlots < numSlots);

        PlanStage::Vector inputScans;
        std::vector<value::SlotVector> inputSlots;
        for (auto&& array : inputStreams) {
            auto [tag, val] = stage_builder::makeValue(array);

            auto [slotVector, scan] = generateVirtualScanMulti(numSlots, tag, val);
            inputScans.push_back(std::move(scan));
            inputSlots.push_back(std::move(slotVector));
        }

        std::vector<value::SlotVector> inputKeys;
        std::vector<value::SlotVector> inputVals;
        for (auto&& slotVector : inputSlots) {
            inputKeys.push_back(
                value::SlotVector(slotVector.begin(), slotVector.begin() + numKeySlots));
            inputVals.push_back(
                value::SlotVector(slotVector.begin() + numKeySlots, slotVector.end()));
        }

        value::SlotVector outputVals;
        for (int i = numKeySlots; i < numSlots; ++i) {
            outputVals.push_back(generateSlotId());
        }

        auto sortedMerge = makeS<SortedMergeStage>(std::move(inputScans),
                                                   std::move(inputKeys),
                                                   std::move(sortDir),
                                                   std::move(inputVals),
                                                   outputVals,
                                                   kEmptyPlanNodeId);

        auto ctx = makeCompileCtx();
        auto resultAccessors = prepareTree(ctx.get(), sortedMerge.get(), outputVals);
        return {resultAccessors, std::move(sortedMerge)};
    }

private:
    // Helper for figuring out how many slots there are in each input stream.
    int checkNumSlots(const std::vector<BSONArray>& inputStreams) {
        boost::optional<int> numSlots;
        for (auto&& stream : inputStreams) {
            if (!stream.isEmpty() && !numSlots) {
                numSlots = stream.firstElement().embeddedObject().nFields();
            }

            for (auto&& array : stream) {
                invariant(array.embeddedObject().nFields() == numSlots);
            }
        }

        invariant(numSlots);
        return *numSlots;
    }
};

TEST_F(SortedMergeStageTest, TwoChildrenBasicAscending) {
    auto [resultAccessors, sortedMerge] =
        makeSortedMerge({BSON_ARRAY(BSON_ARRAY(1 << 1) << BSON_ARRAY(3 << 3)),
                         BSON_ARRAY(BSON_ARRAY(2 << 2) << BSON_ARRAY(4 << 4))},
                        std::vector<value::SortDirection>{value::SortDirection::Ascending});

    auto [expectedTag, expectedVal] = stage_builder::makeValue(BSON_ARRAY(1 << 2 << 3 << 4));
    value::ValueGuard expectedGuard{expectedTag, expectedVal};

    auto [resultsTag, resultsVal] = getAllResults(sortedMerge.get(), resultAccessors[0]);
    value::ValueGuard resultGuard{resultsTag, resultsVal};

    ASSERT_TRUE(valueEquals(resultsTag, resultsVal, expectedTag, expectedVal));
}

TEST_F(SortedMergeStageTest, TwoChildrenBasicDescending) {
    auto [resultAccessors, sortedMerge] = makeSortedMerge(
        {BSON_ARRAY(BSON_ARRAY(5 << 5) << BSON_ARRAY(3 << 3)),
         BSON_ARRAY(BSON_ARRAY(4 << 4) << BSON_ARRAY(2 << 2) << BSON_ARRAY(1 << 1))},
        std::vector<value::SortDirection>{value::SortDirection::Descending});

    auto [expectedTag, expectedVal] = stage_builder::makeValue(BSON_ARRAY(5 << 4 << 3 << 2 << 1));
    value::ValueGuard expectedGuard{expectedTag, expectedVal};

    auto [resultsTag, resultsVal] = getAllResults(sortedMerge.get(), resultAccessors[0]);
    value::ValueGuard resultGuard{resultsTag, resultsVal};

    ASSERT_TRUE(valueEquals(resultsTag, resultsVal, expectedTag, expectedVal));
}

TEST_F(SortedMergeStageTest, TwoChildrenOneEmpty) {
    auto [resultAccessors, sortedMerge] =
        makeSortedMerge({BSONArray(), BSON_ARRAY(BSON_ARRAY(1 << 1) << BSON_ARRAY(2 << 2))},
                        std::vector<value::SortDirection>{value::SortDirection::Ascending});

    auto [expectedTag, expectedVal] = stage_builder::makeValue(BSON_ARRAY(1 << 2));
    value::ValueGuard expectedGuard{expectedTag, expectedVal};

    auto [resultsTag, resultsVal] = getAllResults(sortedMerge.get(), resultAccessors[0]);
    value::ValueGuard resultGuard{resultsTag, resultsVal};

    ASSERT_TRUE(valueEquals(resultsTag, resultsVal, expectedTag, expectedVal));
}

TEST_F(SortedMergeStageTest, TwoChildrenWithDuplicates) {
    auto [resultAccessors, sortedMerge] =
        makeSortedMerge({BSON_ARRAY(BSON_ARRAY(1 << 1) << BSON_ARRAY(1 << 1)),
                         BSON_ARRAY(BSON_ARRAY(1 << 1) << BSON_ARRAY(2 << 2))},
                        std::vector<value::SortDirection>{value::SortDirection::Ascending});

    auto [expectedTag, expectedVal] = stage_builder::makeValue(BSON_ARRAY(1 << 1 << 1 << 2));
    value::ValueGuard expectedGuard{expectedTag, expectedVal};

    auto [resultsTag, resultsVal] = getAllResults(sortedMerge.get(), resultAccessors[0]);
    value::ValueGuard resultGuard{resultsTag, resultsVal};

    ASSERT_TRUE(valueEquals(resultsTag, resultsVal, expectedTag, expectedVal));
}

TEST_F(SortedMergeStageTest, FiveChildren) {
    auto [resultAccessors, sortedMerge] = makeSortedMerge(
        {BSON_ARRAY(BSON_ARRAY(1 << 1) << BSON_ARRAY(1 << 1)),
         BSON_ARRAY(BSON_ARRAY(1 << 1) << BSON_ARRAY(3 << 3)),
         BSON_ARRAY(BSON_ARRAY(4 << 4) << BSON_ARRAY(5 << 5)),
         BSON_ARRAY(BSON_ARRAY(1 << 1) << BSON_ARRAY(2 << 2)),
         BSON_ARRAY(BSON_ARRAY(4 << 4)
                    << BSON_ARRAY(10 << 10) << BSON_ARRAY(11 << 11) << BSON_ARRAY(12 << 12))},
        std::vector<value::SortDirection>{value::SortDirection::Ascending});

    auto [expectedTag, expectedVal] = stage_builder::makeValue(
        BSON_ARRAY(1 << 1 << 1 << 1 << 2 << 3 << 4 << 4 << 5 << 10 << 11 << 12));
    value::ValueGuard expectedGuard{expectedTag, expectedVal};

    auto [resultsTag, resultsVal] = getAllResults(sortedMerge.get(), resultAccessors[0]);
    value::ValueGuard resultGuard{resultsTag, resultsVal};

    ASSERT_TRUE(valueEquals(resultsTag, resultsVal, expectedTag, expectedVal));
}
}  // namespace mongo::sbe
