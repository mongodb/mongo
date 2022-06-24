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

#include "mongo/db/exec/sbe/stages/branch.h"
#include "mongo/db/exec/sbe/stages/bson_scan.h"
#include "mongo/db/exec/sbe/stages/check_bounds.h"
#include "mongo/db/exec/sbe/stages/co_scan.h"
#include "mongo/db/exec/sbe/stages/exchange.h"
#include "mongo/db/exec/sbe/stages/filter.h"
#include "mongo/db/exec/sbe/stages/hash_agg.h"
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
#include "mongo/db/exec/sbe/stages/traverse.h"
#include "mongo/db/exec/sbe/stages/union.h"
#include "mongo/db/exec/sbe/stages/unique.h"
#include "mongo/db/exec/sbe/stages/unwind.h"
#include "mongo/db/exec/sbe/values/slot.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/unittest/unittest.h"

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
    auto stage = makeS<BSONScanStage>(nullptr,
                                      nullptr,
                                      generateSlotId(),
                                      std::vector<std::string>{2},
                                      mockSV(),
                                      kEmptyPlanNodeId);
    assertPlanSize(*stage);
}

TEST_F(PlanSizeTest, CheckBounds) {
    CheckBoundsParams params{
        {IndexBounds()}, BSONObj{}, int{}, KeyString::Version{}, Ordering::allAscending()};
    auto stage = makeS<CheckBoundsStage>(
        mockS(), params, generateSlotId(), generateSlotId(), generateSlotId(), kEmptyPlanNodeId);
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
    auto stage = makeS<HashAggStage>(mockS(),
                                     mockSV(),
                                     makeEM(generateSlotId(), mockE()),
                                     makeSV(),
                                     true,
                                     generateSlotId(),
                                     false,
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
                                      kEmptyPlanNodeId);
    assertPlanSize(*stage);
}

TEST_F(PlanSizeTest, IndexScan) {
    auto collUuid = UUID::parse("00000000-0000-0000-0000-000000000000").getValue();
    auto stage = makeS<IndexScanStage>(collUuid,
                                       StringData(),
                                       true,
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

TEST_F(PlanSizeTest, LimitSkip) {
    auto stage = makeS<LimitSkipStage>(mockS(), 200, 300, kEmptyPlanNodeId);
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
    auto stage = makeS<ScanStage>(collUuid,
                                  generateSlotId(),
                                  generateSlotId(),
                                  generateSlotId(),
                                  generateSlotId(),
                                  generateSlotId(),
                                  generateSlotId(),
                                  boost::none,
                                  std::vector<std::string>{"field"},
                                  mockSV(),
                                  generateSlotId(),
                                  true,
                                  nullptr,
                                  kEmptyPlanNodeId,
                                  ScanCallbacks());
    assertPlanSize(*stage);
}

TEST_F(PlanSizeTest, Sort) {
    auto stage =
        makeS<SortStage>(mockS(),
                         mockSV(),
                         std::vector<value::SortDirection>{value::SortDirection::Ascending},
                         mockSV(),
                         std::numeric_limits<std::size_t>::max(),
                         204857600,
                         false,
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
    auto stage = makeS<SpoolLazyProducerStage>(mockS(), 1, mockSV(), nullptr, kEmptyPlanNodeId);
    assertPlanSize(*stage);
}

TEST_F(PlanSizeTest, SpoolConsumer) {
    auto stage = makeS<SpoolConsumerStage<true>>(1, mockSV(), kEmptyPlanNodeId);
    assertPlanSize(*stage);
}

TEST_F(PlanSizeTest, Traverse) {
    auto stage = makeS<TraverseStage>(mockS(),
                                      mockS(),
                                      generateSlotId(),
                                      generateSlotId(),
                                      generateSlotId(),
                                      mockSV(),
                                      nullptr,
                                      nullptr,
                                      kEmptyPlanNodeId,
                                      boost::none);
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
