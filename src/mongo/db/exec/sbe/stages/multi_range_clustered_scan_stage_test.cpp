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
#include "mongo/db/exec/sbe/stages/multi_range_clustered_scan_stage.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/exec/sbe/expressions/compile_ctx.h"
#include "mongo/db/exec/sbe/expressions/runtime_environment.h"
#include "mongo/db/exec/sbe/sbe_plan_stage_test.h"
#include "mongo/db/exec/sbe/stages/clustered_scan_stage_test_fixtures.h"
#include "mongo/db/exec/sbe/stages/stages.h"
#include "mongo/db/exec/sbe/values/slot.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/query/multiple_collection_accessor.h"
#include "mongo/db/query/record_id_bound.h"
#include "mongo/db/query/record_id_range.h"
#include "mongo/db/query/record_id_range_list.h"
#include "mongo/db/record_id_helpers.h"
#include "mongo/db/shard_role/shard_catalog/clustered_collection_util.h"
#include "mongo/db/shard_role/shard_catalog/collection.h"
#include "mongo/unittest/unittest.h"

#include <memory>
#include <utility>
#include <vector>

#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

namespace mongo::sbe {

class MultiRangeClusteredScanStageTest : public ClusteredScanStageTestFixture {
public:
    MultiRangeClusteredScanStageTest()
        : ClusteredScanStageTestFixture(NamespaceString::createNamespaceString_forTest(
              "testdb.multi_range_clustered_scan_stage")) {}

    /**
     * Builds a MultiRangeClusteredScanStage over 'colls' with the given 'rangeList'. The stage is
     * prepared via PlanStageTestFixture::prepareTree, so the returned stage is ready to use (with
     * an open cursor).
     *
     * The caller can then drive open()/getNext() directly to exercise behavior such as reOpen.
     * The recordId slot accessor is returned so tests can read the produced RecordIds.
     */
    struct PreparedStage {
        std::unique_ptr<PlanStage> stage;
        std::unique_ptr<CompileCtx> ctx;
        value::SlotAccessor* recordIdAccessor;
    };

    PreparedStage makeStage(const MultipleCollectionAccessor& colls,
                            RecordIdRangeList rangeList,
                            bool forward) {
        const value::SlotId resultSlot = generateSlotId();
        const value::SlotId recordIdSlot = generateSlotId();

        auto env = std::make_unique<RuntimeEnvironment>();

        std::unique_ptr<PlanStage> stage = std::make_unique<MultiRangeClusteredScanStage>(
            colls.getMainCollection()->uuid(),
            _nss.dbName(),
            resultSlot,
            recordIdSlot,
            boost::none /* snapshotIdSlot */,
            boost::none /* indexIdentSlot */,
            boost::none /* indexKeySlot */,
            boost::none /* indexKeyPatternSlot */,
            std::vector<std::string>{},
            value::SlotVector{},
            std::move(rangeList),
            forward,
            nullptr /* yieldPolicy */,
            kEmptyPlanNodeId,
            nullptr /* scanOpenCallback */,
            false /* participateInTrialRunTracking */);

        auto ctx = makeCompileCtx(std::move(env));
        attachCollectionAcquisition(colls);
        auto* recordIdAccessor = prepareTree(ctx.get(), stage.get(), recordIdSlot);

        return PreparedStage{std::move(stage), std::move(ctx), recordIdAccessor};
    }

    // Run the stage to completion and return all RecordIds returned through 'recordIdAccessor'.
    static std::vector<RecordId> drain(PlanStage* stage, value::SlotAccessor* recordIdAccessor) {
        std::vector<RecordId> ids;
        for (auto state = stage->getNext(); state == PlanState::ADVANCED;
             state = stage->getNext()) {
            auto [tag, val] = recordIdAccessor->getViewOfValue();
            ASSERT_EQ(tag, value::TypeTags::RecordId);
            ids.push_back(*value::getRecordIdView(val));
        }
        return ids;
    }
};

// Test that reopening a multi-bound clustered collection scan
// correctly resets the cursor to the initial seek point.
TEST_F(MultiRangeClusteredScanStageTest, ReOpenNoStartBoundForward) {
    std::vector<BSONObj> docs;
    for (int i = 0; i < 8; ++i)
        docs.push_back(BSON("_id" << i));
    auto colls = createClusteredCollection(docs);

    auto rangeList = RecordIdRangeList::makeUnion({
        makeIntRange(boost::none, true, 3, true),  // (−∞, 3]  — no start bound for forward
        makeIntRange(5, true, 6, true),            // [5, 6]
    });
    ASSERT_EQ(rangeList.getRanges().size(), 2u);

    auto p = makeStage(colls, rangeList, /*forward=*/true);

    // First pass.
    auto first = drain(p.stage.get(), p.recordIdAccessor);
    std::vector<RecordId> expected;
    for (int i : {0, 1, 2, 3, 5, 6})
        expected.push_back(record_id_helpers::keyForElem(BSON("_id" << i).firstElement()));
    ASSERT_EQ(first, expected);

    // Now reOpen and drain again — must produce the same documents.
    p.stage->open(true /* reOpen */);
    auto second = drain(p.stage.get(), p.recordIdAccessor);
    ASSERT_EQ(second, expected);
}

// Same idea for the BACKWARD direction: when the first range traversed has an unbounded max,
// there's no initial seek and the cursor must still be reset on reOpen.
TEST_F(MultiRangeClusteredScanStageTest, ReOpenNoStartBoundBackward) {
    std::vector<BSONObj> docs;
    for (int i = 0; i < 8; ++i)
        docs.push_back(BSON("_id" << i));
    auto colls = createClusteredCollection(docs);

    auto rangeList = RecordIdRangeList::makeUnion({
        makeIntRange(0, true, 1, true),            // [0, 1]
        makeIntRange(4, true, boost::none, true),  // [4, +∞)  — no max bound (backward start)
    });
    ASSERT_EQ(rangeList.getRanges().size(), 2u);

    auto p = makeStage(colls, rangeList, /*forward=*/false);

    auto first = drain(p.stage.get(), p.recordIdAccessor);
    std::vector<RecordId> expected;
    for (int i : {7, 6, 5, 4, 1, 0})
        expected.push_back(record_id_helpers::keyForElem(BSON("_id" << i).firstElement()));
    ASSERT_EQ(first, expected);

    p.stage->open(true /* reOpen */);
    auto second = drain(p.stage.get(), p.recordIdAccessor);
    ASSERT_EQ(second, expected);
}

// Also test reOpen with a bounded start.
TEST_F(MultiRangeClusteredScanStageTest, ReOpenWithStartBoundForward) {
    std::vector<BSONObj> docs;
    for (int i = 0; i < 8; ++i)
        docs.push_back(BSON("_id" << i));
    auto colls = createClusteredCollection(docs);

    auto rangeList = RecordIdRangeList::makeUnion({
        makeIntRange(1, true, 3, true),  // [1, 3]
        makeIntRange(5, true, 6, true),  // [5, 6]
    });

    auto p = makeStage(colls, rangeList, /*forward=*/true);

    auto first = drain(p.stage.get(), p.recordIdAccessor);
    std::vector<RecordId> expected;
    for (int i : {1, 2, 3, 5, 6})
        expected.push_back(record_id_helpers::keyForElem(BSON("_id" << i).firstElement()));
    ASSERT_EQ(first, expected);

    p.stage->open(true /* reOpen */);
    auto second = drain(p.stage.get(), p.recordIdAccessor);
    ASSERT_EQ(second, expected);
}

}  // namespace mongo::sbe
