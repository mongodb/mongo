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

#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/ordering.h"
#include "mongo/db/exec/sbe/expressions/expression.h"
#include "mongo/db/exec/sbe/sbe_plan_stage_test.h"
#include "mongo/db/exec/sbe/stages/branch.h"
#include "mongo/db/exec/sbe/stages/bson_scan.h"
#include "mongo/db/exec/sbe/stages/co_scan.h"
#include "mongo/db/exec/sbe/stages/exchange.h"
#include "mongo/db/exec/sbe/stages/filter.h"
#include "mongo/db/exec/sbe/stages/hash_agg.h"
#include "mongo/db/exec/sbe/stages/hash_agg_accumulator.h"
#include "mongo/db/exec/sbe/stages/hash_join.h"
#include "mongo/db/exec/sbe/stages/ix_scan.h"
#include "mongo/db/exec/sbe/stages/limit_skip.h"
#include "mongo/db/exec/sbe/stages/loop_join.h"
#include "mongo/db/exec/sbe/stages/makeobj.h"
#include "mongo/db/exec/sbe/stages/merge_join.h"
#include "mongo/db/exec/sbe/stages/project.h"
#include "mongo/db/exec/sbe/stages/scan.h"
#include "mongo/db/exec/sbe/stages/sort.h"
#include "mongo/db/exec/sbe/stages/sorted_merge.h"
#include "mongo/db/exec/sbe/stages/spool.h"
#include "mongo/db/exec/sbe/stages/stages.h"
#include "mongo/db/exec/sbe/stages/union.h"
#include "mongo/db/exec/sbe/stages/unique.h"
#include "mongo/db/exec/sbe/stages/unwind.h"
#include "mongo/db/exec/sbe/values/slot.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/query/compiler/physical_model/query_solution/stage_types.h"
#include "mongo/db/storage/key_string/key_string.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/id_generator.h"
#include "mongo/util/uuid.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>


namespace mongo::sbe {

class PlanSizeTest : public unittest::Test {
public:
    void setUp() override {
        _slotIdGenerator = std::make_unique<value::SlotIdGenerator>();
    }

    void tearDown() override {
        _slotIdGenerator.reset();
    }

    value::SlotId generateSlotId() {
        return _slotIdGenerator->generate();
    }

    static std::unique_ptr<PlanStage> mockS() {
        return makeS<CoScanStage>(kEmptyPlanNodeId);
    }

    value::SlotVector mockSV() {
        return makeSV(generateSlotId());
    }

    std::unique_ptr<EExpression> mockE() {
        return makeE<EConstant>(value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(1));
    }

    /**
     * PlanSize is planform-dependent so here we just assert that the size is a reasonable number:
     * bigger then zero and not too big.
     * A too big number might mean unwanted wrapping around on unsigned integer.
     */
    void assertPlanSize(const PlanStage& stage) {
        size_t size = stage.estimateCompileTimeSize();
        ASSERT_LT(0ul, size);
        ASSERT_GT(10000ul, size);
    }

private:
    std::unique_ptr<value::SlotIdGenerator> _slotIdGenerator;
};

TEST_F(PlanSizeTest, Branch) {
    auto stage = makeS<BranchStage>(
        mockS(), mockS(), mockE(), mockSV(), mockSV(), mockSV(), kEmptyPlanNodeId);
    assertPlanSize(*stage);
}

TEST_F(PlanSizeTest, BsonScan) {
    auto stage = makeS<BSONScanStage>(std::vector<BSONObj>{},
                                      generateSlotId(),
                                      kEmptyPlanNodeId,
                                      std::vector<std::string>{2},
                                      mockSV());
    assertPlanSize(*stage);
}

TEST_F(PlanSizeTest, CoScan) {
    auto stage = makeS<CoScanStage>(kEmptyPlanNodeId);
    assertPlanSize(*stage);
}

TEST_F(PlanSizeTest, Exchange) {
    auto stage = makeS<ExchangeConsumer>(
        mockS(), 1, makeSV(), ExchangePolicy::broadcast, nullptr, mockE(), kEmptyPlanNodeId);
    assertPlanSize(*stage);
}

TEST_F(PlanSizeTest, Filter) {
    auto stage = makeS<FilterStage<true>>(mockS(), mockE(), kEmptyPlanNodeId);
    assertPlanSize(*stage);
}

TEST_F(PlanSizeTest, HashAgg) {
    auto stage = makeS<HashAggStage>(
        mockS(),
        mockSV(),
        makeHashAggAccumulatorList(std::make_unique<CompiledHashAggAccumulator>(
                                       generateSlotId(), generateSlotId(), mockE(), mockE()),
                                   std::make_unique<ArithmeticAverageHashAggAccumulatorTerminal>(
                                       generateSlotId(), generateSlotId(), mockE(), boost::none)),
        makeSV(),
        true,
        generateSlotId(),
        false,
        nullptr /* yieldPolicy */,
        kEmptyPlanNodeId);
    assertPlanSize(*stage);
}

TEST_F(PlanSizeTest, HashJoin) {
    auto stage = makeS<HashJoinStage>(mockS(),
                                      mockS(),
                                      mockSV(),
                                      makeSV(),
                                      mockSV(),
                                      makeSV(),
                                      generateSlotId(),
                                      nullptr /* yieldPolicy */,
                                      kEmptyPlanNodeId);
    assertPlanSize(*stage);
}

TEST_F(PlanSizeTest, SimpleIndexScanStage) {
    auto collUuid = UUID::parse("00000000-0000-0000-0000-000000000000").getValue();
    auto stage = makeS<SimpleIndexScanStage>(collUuid,
                                             DatabaseName(),
                                             StringData(),
                                             true,
                                             generateSlotId(),
                                             generateSlotId(),
                                             generateSlotId(),
                                             generateSlotId(),
                                             IndexKeysInclusionSet(1),
                                             mockSV(),
                                             makeE<EVariable>(generateSlotId()),
                                             makeE<EVariable>(generateSlotId()),
                                             nullptr,
                                             kEmptyPlanNodeId);
    assertPlanSize(*stage);
}

TEST_F(PlanSizeTest, GenericIndexScanStage) {
    auto collUuid = UUID::parse("00000000-0000-0000-0000-000000000000").getValue();
    GenericIndexScanStageParams params{makeE<EVariable>(generateSlotId()),
                                       {},
                                       1,
                                       key_string::Version{0},
                                       Ordering::allAscending()};
    auto stage = makeS<GenericIndexScanStage>(collUuid,
                                              DatabaseName(),
                                              StringData(),
                                              std::move(params),
                                              generateSlotId(),
                                              generateSlotId(),
                                              generateSlotId(),
                                              generateSlotId(),
                                              IndexKeysInclusionSet(1),
                                              mockSV(),
                                              nullptr,
                                              kEmptyPlanNodeId);
    assertPlanSize(*stage);
}

TEST_F(PlanSizeTest, LimitSkip) {
    auto stage = makeS<LimitSkipStage>(mockS(),
                                       makeE<EConstant>(value::TypeTags::NumberInt64, 200),
                                       makeE<EConstant>(value::TypeTags::NumberInt64, 300),
                                       kEmptyPlanNodeId);
    assertPlanSize(*stage);
}

TEST_F(PlanSizeTest, LoopJoin) {
    auto stage =
        makeS<LoopJoinStage>(mockS(), mockS(), makeSV(), makeSV(), nullptr, kEmptyPlanNodeId);
    assertPlanSize(*stage);
}

TEST_F(PlanSizeTest, MakeObj) {
    auto stage = makeS<MakeObjStage>(mockS(),
                                     generateSlotId(),
                                     generateSlotId(),
                                     MakeObjFieldBehavior::keep,
                                     std::vector<std::string>(),
                                     std::vector<std::string>(),
                                     makeSV(),
                                     true,
                                     false,
                                     kEmptyPlanNodeId);
    assertPlanSize(*stage);
}

TEST_F(PlanSizeTest, MergeJoin) {
    std::vector<value::SortDirection> sortDirs(1, value::SortDirection::Ascending);
    auto stage = makeS<MergeJoinStage>(mockS(),
                                       mockS(),
                                       mockSV(),
                                       mockSV(),
                                       mockSV(),
                                       mockSV(),
                                       std::move(sortDirs),
                                       kEmptyPlanNodeId);
    assertPlanSize(*stage);
}

TEST_F(PlanSizeTest, Project) {
    auto stage = makeProjectStage(
        mockS(), kEmptyPlanNodeId, generateSlotId(), mockE(), generateSlotId(), mockE());
    assertPlanSize(*stage);
}

TEST_F(PlanSizeTest, Scan) {
    auto collUuid = UUID::parse("00000000-0000-0000-0000-000000000000").getValue();
    auto stage = makeS<sbe::ScanStage>(collUuid,
                                       DatabaseName(),
                                       generateSlotId() /* recordSlot */,
                                       generateSlotId() /* recordIdSlot */,
                                       generateSlotId() /* snapshotIdSlot */,
                                       generateSlotId() /* indexIdSlot */,
                                       generateSlotId() /* indexKeySlot */,
                                       generateSlotId() /* indexKeyPatternSlot */,
                                       std::vector<std::string>{"field"} /* scanFieldNames */,
                                       mockSV() /* scanFieldSlots */,
                                       generateSlotId() /* seekRecordIdSlot */,
                                       generateSlotId() /* minRecordIdSlot */,
                                       generateSlotId() /* maxRecordIdSlot */,
                                       true /* forward */,
                                       nullptr /* yieldPolicy */,
                                       kEmptyPlanNodeId /* nodeId */,
                                       ScanCallbacks());
    assertPlanSize(*stage);
}

TEST_F(PlanSizeTest, ParallelScan) {
    auto collUuid = UUID::parse("00000000-0000-0000-0000-000000000000").getValue();
    auto stage =
        makeS<sbe::ParallelScanStage>(collUuid,
                                      DatabaseName(),
                                      generateSlotId() /* recordSlot */,
                                      generateSlotId() /* recordIdSlot */,
                                      generateSlotId() /* snapshotIdSlot */,
                                      generateSlotId() /* indexIdSlot */,
                                      generateSlotId() /* indexKeySlot */,
                                      generateSlotId() /* indexKeyPatternSlot */,
                                      std::vector<std::string>{"field"} /* scanFieldNames */,
                                      mockSV() /* scanFieldSlots */,
                                      nullptr /* yieldPolicy */,
                                      kEmptyPlanNodeId /* nodeId */,
                                      ScanCallbacks());
    assertPlanSize(*stage);
}

TEST_F(PlanSizeTest, Sort) {
    auto stage =
        makeS<SortStage>(mockS(),
                         mockSV(),
                         std::vector<value::SortDirection>{value::SortDirection::Ascending},
                         mockSV(),
                         nullptr /*limit*/,
                         204857600,
                         false,
                         nullptr /* yieldPolicy */,
                         kEmptyPlanNodeId);
    assertPlanSize(*stage);
}

TEST_F(PlanSizeTest, SortedMerge) {
    std::vector<value::SortDirection> sortDir{value::SortDirection::Ascending};
    const size_t numSlots = 4;

    PlanStage::Vector inputScans;
    std::vector<value::SlotVector> inputSlots;
    std::vector<value::SlotVector> inputKeys;
    std::vector<value::SlotVector> inputVals;
    for (size_t i = 0; i < numSlots; ++i) {
        inputScans.push_back(mockS());
        inputKeys.push_back(mockSV());
        inputVals.push_back(mockSV());
    }

    auto stage = makeS<SortedMergeStage>(std::move(inputScans),
                                         std::move(inputKeys),
                                         std::move(sortDir),
                                         std::move(inputVals),
                                         mockSV(),
                                         kEmptyPlanNodeId);
    assertPlanSize(*stage);
}

TEST_F(PlanSizeTest, SpoolLazyProducer) {
    auto stage = makeS<SpoolLazyProducerStage>(
        mockS(), 1, mockSV(), nullptr /* yieldPolicy */, kEmptyPlanNodeId);
    assertPlanSize(*stage);
}

TEST_F(PlanSizeTest, SpoolConsumer) {
    auto stage =
        makeS<SpoolConsumerStage<true>>(1, mockSV(), nullptr /* yieldPolicy */, kEmptyPlanNodeId);
    assertPlanSize(*stage);
}

TEST_F(PlanSizeTest, Union) {
    auto scanStages = makeSs(mockS(), mockS());
    std::vector<value::SlotVector> scanInputVals{mockSV(), mockSV()};
    auto stage = makeS<UnionStage>(
        std::move(scanStages), std::move(scanInputVals), mockSV(), kEmptyPlanNodeId);
    assertPlanSize(*stage);
}

TEST_F(PlanSizeTest, Unique) {
    auto stage = makeS<UniqueStage>(mockS(), mockSV(), kEmptyPlanNodeId);
    assertPlanSize(*stage);
}

TEST_F(PlanSizeTest, Unwind) {
    auto stage = makeS<UnwindStage>(
        mockS(), generateSlotId(), generateSlotId(), generateSlotId(), false, kEmptyPlanNodeId);
    assertPlanSize(*stage);
}
}  // namespace mongo::sbe
