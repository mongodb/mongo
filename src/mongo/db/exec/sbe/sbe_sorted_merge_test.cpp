// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/bson/bsonelement.h"
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

    value::TagValueOwned expected =
        value::TagValueOwned::fromRaw(stage_builder::makeValue(BSON_ARRAY(1 << 2 << 3 << 4)));

    value::TagValueOwned results =
        value::TagValueOwned::fromRaw(getAllResults(sortedMerge.get(), resultAccessors[0]));

    ASSERT_TRUE(valueEquals(results.tag(), results.value(), expected.tag(), expected.value()));
}

TEST_F(SortedMergeStageTest, TwoChildrenBasicDescending) {
    auto [resultAccessors, sortedMerge] = makeSortedMerge(
        {BSON_ARRAY(BSON_ARRAY(5 << 5) << BSON_ARRAY(3 << 3)),
         BSON_ARRAY(BSON_ARRAY(4 << 4) << BSON_ARRAY(2 << 2) << BSON_ARRAY(1 << 1))},
        std::vector<value::SortDirection>{value::SortDirection::Descending});

    value::TagValueOwned expected =
        value::TagValueOwned::fromRaw(stage_builder::makeValue(BSON_ARRAY(5 << 4 << 3 << 2 << 1)));

    value::TagValueOwned results =
        value::TagValueOwned::fromRaw(getAllResults(sortedMerge.get(), resultAccessors[0]));

    ASSERT_TRUE(valueEquals(results.tag(), results.value(), expected.tag(), expected.value()));
}

TEST_F(SortedMergeStageTest, TwoChildrenOneEmpty) {
    auto [resultAccessors, sortedMerge] =
        makeSortedMerge({BSONArray(), BSON_ARRAY(BSON_ARRAY(1 << 1) << BSON_ARRAY(2 << 2))},
                        std::vector<value::SortDirection>{value::SortDirection::Ascending});

    value::TagValueOwned expected =
        value::TagValueOwned::fromRaw(stage_builder::makeValue(BSON_ARRAY(1 << 2)));

    value::TagValueOwned results =
        value::TagValueOwned::fromRaw(getAllResults(sortedMerge.get(), resultAccessors[0]));

    ASSERT_TRUE(valueEquals(results.tag(), results.value(), expected.tag(), expected.value()));
}

TEST_F(SortedMergeStageTest, TwoChildrenWithDuplicates) {
    auto [resultAccessors, sortedMerge] =
        makeSortedMerge({BSON_ARRAY(BSON_ARRAY(1 << 1) << BSON_ARRAY(1 << 1)),
                         BSON_ARRAY(BSON_ARRAY(1 << 1) << BSON_ARRAY(2 << 2))},
                        std::vector<value::SortDirection>{value::SortDirection::Ascending});

    value::TagValueOwned expected =
        value::TagValueOwned::fromRaw(stage_builder::makeValue(BSON_ARRAY(1 << 1 << 1 << 2)));

    value::TagValueOwned results =
        value::TagValueOwned::fromRaw(getAllResults(sortedMerge.get(), resultAccessors[0]));

    ASSERT_TRUE(valueEquals(results.tag(), results.value(), expected.tag(), expected.value()));
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

    value::TagValueOwned expected = value::TagValueOwned::fromRaw(stage_builder::makeValue(
        BSON_ARRAY(1 << 1 << 1 << 1 << 2 << 3 << 4 << 4 << 5 << 10 << 11 << 12)));

    value::TagValueOwned results =
        value::TagValueOwned::fromRaw(getAllResults(sortedMerge.get(), resultAccessors[0]));

    ASSERT_TRUE(valueEquals(results.tag(), results.value(), expected.tag(), expected.value()));
}
}  // namespace mongo::sbe
