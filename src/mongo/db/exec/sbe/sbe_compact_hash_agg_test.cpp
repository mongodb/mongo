// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

/**
 * This file contains tests for sbe::CompactHashAggStage.
 */

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/json.h"
#include "mongo/db/exec/sbe/expressions/compile_ctx.h"
#include "mongo/db/exec/sbe/sbe_plan_stage_test.h"
#include "mongo/db/exec/sbe/stages/compact_hash_agg.h"
#include "mongo/db/exec/sbe/stages/compact_hash_agg_accumulator.h"
#include "mongo/db/exec/sbe/stages/hash_agg.h"
#include "mongo/db/exec/sbe/stages/hash_agg_accumulator.h"
#include "mongo/db/exec/sbe/stages/plan_stats.h"
#include "mongo/db/exec/sbe/stages/stages.h"
#include "mongo/db/exec/sbe/values/bson.h"
#include "mongo/db/exec/sbe/values/slot.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/exec/sbe/vm/vm.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/query/collation/collator_interface_mock.h"
#include "mongo/db/record_id.h"
#include "mongo/db/shard_role/lock_manager/d_concurrency.h"
#include "mongo/db/shard_role/lock_manager/lock_manager_defs.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/server_parameter_guard.h"
#include "mongo/unittest/unittest.h"

#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <boost/optional/optional.hpp>

namespace mongo::sbe {
namespace {

using value::TagValueView;
using Groups = std::map<std::string, std::vector<BSONObj>>;

BSONArray jsonArray(std::string_view json) {
    return BSONArray(fromjson(json));
}

BSONObj toBson(TagValueView view) {
    BSONObjBuilder builder;
    bson::appendValueToBsonObj(builder, "", view.tag, view.value);
    return builder.obj();
}

Groups jsonGroups(std::string_view json) {
    Groups groups;
    auto rows = fromjson(json);
    for (auto&& row : rows) {
        auto fields = row.Array();
        std::vector<BSONObj> outputs;
        for (size_t idx = 1; idx < fields.size(); ++idx) {
            outputs.push_back(fields[idx].wrap(""));
        }
        groups.emplace(fields[0].wrap("").toString(), std::move(outputs));
    }
    return groups;
}

std::vector<std::unique_ptr<CompactHashAggAccumulator>> sums(value::SlotId input,
                                                             value::SlotId final,
                                                             value::SlotId partial) {
    std::vector<std::unique_ptr<CompactHashAggAccumulator>> accumulators;
    accumulators.push_back(
        std::make_unique<CompactSumHashAggAccumulatorTerminal>(final, makeVariable(input)));
    accumulators.push_back(
        std::make_unique<CompactSumHashAggAccumulatorPartial>(partial, makeVariable(input)));
    return accumulators;
}

struct CountedState {
    CountedState() {
        ++live;
    }
    ~CountedState() {
        --live;
    }
    inline static int live = 0;
};

class CountingAccumulator final : public TypedCompactHashAggAccumulator<CountedState> {
public:
    using TypedCompactHashAggAccumulator::TypedCompactHashAggAccumulator;

    std::unique_ptr<CompactHashAggAccumulator> clone() const final {
        return std::make_unique<CountingAccumulator>(_outSlot, _inputExpr->clone());
    }

    size_t estimateSize() const final {
        return estimateSizeWithInput(sizeof(*this));
    }

protected:
    void accumulateState(vm::ByteCode&, CountedState&) const final {}

    value::TagValueMaybeOwned finalizeState(CountedState& state) const final {
        return value::TagValueMaybeOwned::nothing();
    }

    size_t stateHeapMemUsage(const CountedState& state) const final {
        return 0;
    }

    std::string getDebugName() const final {
        return "_counting";
    }
};

void assertSameGroups(const Groups& expected, const Groups& actual) {
    ASSERT_EQ(expected.size(), actual.size());
    for (const auto& [key, outputs] : expected) {
        auto it = actual.find(key);
        ASSERT_TRUE(it != actual.end()) << key;
        ASSERT_EQ(outputs.size(), it->second.size());
        for (size_t idx = 0; idx < outputs.size(); ++idx) {
            ASSERT_TRUE(outputs[idx].binaryEqual(it->second[idx]))
                << key << ": " << outputs[idx] << " != " << it->second[idx];
        }
    }
}

class CompactHashAggStageTest : public PlanStageTestFixture {
public:
    void setUp() override {
        PlanStageTestFixture::setUp();
        _globalLock = std::make_unique<Lock::GlobalLock>(operationContext(), MODE_IS);
    }

    void tearDown() override {
        _globalLock.reset();
        PlanStageTestFixture::tearDown();
    }

    // Groups the input by its single field and asserts that the resulting group keys equal
    // 'expectedKeys'. With a collator, each group keeps the first key inserted.
    void performCompactHashAgg(
        const BSONArray& inputArr,
        std::set<std::string> expectedKeys,
        std::unique_ptr<mongo::CollatorInterfaceMock> optionalCollator = nullptr);

    void checkMemoryStats(mongo::sbe::PlanStage* stage) {
        ASSERT_GT(stage->getMemoryTracker()->peakTrackedMemoryBytes(), 0);
        ASSERT_GT(stage->getMemoryTracker()->inUseTrackedMemoryBytes(), 0);
        auto stats = static_cast<const HashAggStats*>(stage->getSpecificStats());
        ASSERT_GT(stats->peakTrackedMemBytes, 0);
    }

    // Drives getNext() to completion and returns the distinct group-by keys.
    std::set<int32_t> collectIntKeys(mongo::sbe::PlanStage* stage, value::SlotAccessor* accessor) {
        std::set<int32_t> results;
        while (stage->getNext() == PlanState::ADVANCED) {
            auto [tag, val] = accessor->getViewOfValue();
            ASSERT_EQ(value::TypeTags::NumberInt32, tag);
            results.insert(value::bitcastTo<int32_t>(val));
        }
        return results;
    }

    Groups drainGroups(PlanStage* stage, const std::vector<value::SlotAccessor*>& accessors) {
        Groups groups;
        while (stage->getNext() == PlanState::ADVANCED) {
            std::vector<BSONObj> outputs;
            for (size_t idx = 1; idx < accessors.size(); ++idx) {
                outputs.push_back(toBson(accessors[idx]->getViewOfValue()));
            }
            groups.emplace(toBson(accessors[0]->getViewOfValue()).toString(), std::move(outputs));
        }
        return groups;
    }

    Groups runSums(const BSONArray& rows,
                   std::unique_ptr<CollatorInterfaceMock> collator = nullptr) {
        auto ctx = makeCompileCtx();
        value::OwnedValueAccessor collatorAccessor;
        boost::optional<value::SlotId> collatorSlot;
        if (collator) {
            collatorSlot = generateSlotId();
            ctx->pushCorrelated(*collatorSlot, &collatorAccessor);
            collatorAccessor.reset(value::TypeTags::collator,
                                   value::bitcastFrom<CollatorInterface*>(collator.release()));
        }
        auto [scanSlots, scanStage] = generateVirtualScanMulti(2, rows);
        auto groupByOutSlot = generateSlotId();
        auto final = generateSlotId();
        auto partial = generateSlotId();
        auto stage = makeS<CompactHashAggStage>(std::move(scanStage),
                                                makeSV(scanSlots[0]),
                                                makeSV(groupByOutSlot),
                                                sums(scanSlots[1], final, partial),
                                                collatorSlot,
                                                /*yieldPolicy=*/nullptr,
                                                kEmptyPlanNodeId);
        auto groups = drainGroups(
            stage.get(), prepareTree(ctx.get(), stage.get(), {groupByOutSlot, final, partial}));
        stage->close();
        return groups;
    }

    std::unique_ptr<PlanStage> makeCountingStage(const BSONArray& keys, size_t count) {
        CountedState::live = 0;
        auto [scanSlot, scanStage] = generateVirtualScan(keys);
        std::vector<std::unique_ptr<CompactHashAggAccumulator>> accumulators;
        for (size_t idx = 0; idx < count; ++idx) {
            accumulators.push_back(
                std::make_unique<CountingAccumulator>(generateSlotId(), makeVariable(scanSlot)));
        }
        auto stage = makeS<CompactHashAggStage>(std::move(scanStage),
                                                makeSV(scanSlot),
                                                makeSV(generateSlotId()),
                                                std::move(accumulators),
                                                boost::none,
                                                /*yieldPolicy=*/nullptr,
                                                kEmptyPlanNodeId);
        _countingCtx = makeCompileCtx();
        prepareTree(_countingCtx.get(), stage.get());
        return stage;
    }

private:
    std::unique_ptr<CompileCtx> _countingCtx;
    std::unique_ptr<Lock::GlobalLock> _globalLock;
};

void CompactHashAggStageTest::performCompactHashAgg(
    const BSONArray& inputArr,
    std::set<std::string> expectedKeys,
    std::unique_ptr<mongo::CollatorInterfaceMock> optionalCollator) {
    auto collatorSlot = generateSlotId();
    auto shouldUseCollator = optionalCollator != nullptr;

    auto ctx = makeCompileCtx();

    value::OwnedValueAccessor collatorAccessor;
    if (shouldUseCollator) {
        ctx->pushCorrelated(collatorSlot, &collatorAccessor);
        collatorAccessor.reset(value::TypeTags::collator,
                               value::bitcastFrom<CollatorInterface*>(optionalCollator.release()));
    }

    // Generate a mock scan from 'input' with a single output slot.
    auto [scanSlot, scanStage] = generateVirtualScan(inputArr);

    auto groupByOutSlot = generateSlotId();
    auto stage =
        makeS<CompactHashAggStage>(std::move(scanStage),
                                   makeSV(scanSlot),
                                   makeSV(groupByOutSlot),
                                   std::vector<std::unique_ptr<CompactHashAggAccumulator>>{},
                                   boost::optional<value::SlotId>{shouldUseCollator, collatorSlot},
                                   /*yieldPolicy=*/nullptr,
                                   kEmptyPlanNodeId);
    auto resultAccessor = prepareTree(ctx.get(), stage.get(), groupByOutSlot);

    std::set<std::string> results;
    while (stage->getNext() == PlanState::ADVANCED) {
        auto [tag, val] = resultAccessor->getViewOfValue();
        ASSERT_EQ(value::TypeTags::StringSmall, tag);
        results.insert(std::string(value::getStringView(tag, val)));
    }

    ASSERT_EQ(expectedKeys, results);
    ASSERT_GT(stage->getMemoryTracker()->peakTrackedMemoryBytes(), 0);
    ASSERT_GT(stage->getMemoryTracker()->inUseTrackedMemoryBytes(), 0);
}

TEST_F(CompactHashAggStageTest, CompactHashAggCollationTest) {
    auto inputArr = jsonArray(R"(["A", "a", "b", "c", "B"])");

    // With a collator, "A" groups with "a" and "b" groups with "B".
    auto lowerStringCollator =
        std::make_unique<CollatorInterfaceMock>(CollatorInterfaceMock::MockType::kToLowerString);
    performCompactHashAgg(inputArr, {"A", "b", "c"}, std::move(lowerStringCollator));

    // Without a collator, every input value is its own group.
    performCompactHashAgg(inputArr, {"A", "a", "b", "B", "c"});
}

using CompactHashAggStageDeathTest = CompactHashAggStageTest;

DEATH_TEST_REGEX_F(CompactHashAggStageDeathTest,
                   GroupByOutputSlotsMustMatchGroupBySlots,
                   "Tripwire assertion.*13913000") {
    auto [scanSlot, scanStage] = generateVirtualScan(
        value::TagValueOwned::fromRaw(stage_builder::makeValue(BSON_ARRAY(1 << 2))));
    makeS<CompactHashAggStage>(std::move(scanStage),
                               makeSV(scanSlot),
                               makeSV(),
                               std::vector<std::unique_ptr<CompactHashAggAccumulator>>{},
                               boost::none,
                               /*yieldPolicy=*/nullptr,
                               kEmptyPlanNodeId);
}

DEATH_TEST_REGEX_F(CompactHashAggStageDeathTest,
                   GroupByOutputSlotsMustNotRepeat,
                   "Tripwire assertion.*13605503") {
    auto [scanSlots, scanStage] =
        generateVirtualScanMulti(2,
                                 value::TagValueOwned::fromRaw(stage_builder::makeValue(
                                     BSON_ARRAY(BSON_ARRAY(1 << 2) << BSON_ARRAY(3 << 4)))));
    auto outSlot = generateSlotId();
    auto stage =
        makeS<CompactHashAggStage>(std::move(scanStage),
                                   scanSlots,
                                   makeSV(outSlot, outSlot),
                                   std::vector<std::unique_ptr<CompactHashAggAccumulator>>{},
                                   boost::none,
                                   /*yieldPolicy=*/nullptr,
                                   kEmptyPlanNodeId);
    auto ctx = makeCompileCtx();
    prepareTree(ctx.get(), stage.get(), outSlot);
}

TEST_F(CompactHashAggStageTest, CompactHashAggBasicNoSpill) {
    // The compact stage never spills; with plentiful memory the query completes in memory.
    auto ctx = makeCompileCtx();

    // Build a scan of the [5,6,7,5,6,7,6,7,7] input array.
    auto [scanSlot, scanStage] = generateVirtualScan(jsonArray("[5, 6, 7, 5, 6, 7, 6, 7, 7]"));

    auto groupByOutSlot = generateSlotId();
    auto stage =
        makeS<CompactHashAggStage>(std::move(scanStage),
                                   makeSV(scanSlot),
                                   makeSV(groupByOutSlot),
                                   std::vector<std::unique_ptr<CompactHashAggAccumulator>>{},
                                   boost::none,
                                   /*yieldPolicy=*/nullptr,
                                   kEmptyPlanNodeId);
    auto resultAccessor = prepareTree(ctx.get(), stage.get(), groupByOutSlot);

    // Check that the results match the expected: one group per distinct key.
    auto results = collectIntKeys(stage.get(), resultAccessor);
    ASSERT_EQ(3, results.size());
    ASSERT_EQ(1, results.count(5));
    ASSERT_EQ(1, results.count(6));
    ASSERT_EQ(1, results.count(7));

    // Check that the spilling behavior matches the expected.
    auto stats = static_cast<const HashAggStats*>(stage->getSpecificStats());
    ASSERT_FALSE(stats->usedDisk);
    ASSERT_EQ(0, stats->spillingStats.getSpills());
    ASSERT_EQ(0, stats->spillingStats.getSpilledRecords());

    checkMemoryStats(stage.get());
    stage->close();
}

TEST_F(CompactHashAggStageTest, CompactHashAggNoGroupByProducesSingleGroup) {
    auto ctx = makeCompileCtx();

    auto [scanSlot, scanStage] = generateVirtualScan(jsonArray("[1.0, 2.0, 3.0, 4.0, 5.0]"));

    // Build a CompactHashAggStage with an empty group-by slot list.
    auto stage =
        makeS<CompactHashAggStage>(std::move(scanStage),
                                   makeSV(),
                                   makeSV(),
                                   std::vector<std::unique_ptr<CompactHashAggAccumulator>>{},
                                   boost::none,
                                   /*yieldPolicy=*/nullptr,
                                   kEmptyPlanNodeId);

    // There are no output slots, so prepare the tree without requesting any and count the
    // results by driving getNext() directly.
    prepareTree(ctx.get(), stage.get());
    size_t numResults = 0;
    while (stage->getNext() == PlanState::ADVANCED) {
        ++numResults;
    }

    // All input rows aggregate into a single group.
    ASSERT_EQ(1, numResults);

    // Check that it did not touch disk.
    auto stats = static_cast<const HashAggStats*>(stage->getSpecificStats());
    ASSERT_FALSE(stats->usedDisk);
    ASSERT_EQ(0, stats->spillingStats.getSpills());
    ASSERT_EQ(0, stats->spillingStats.getSpilledRecords());

    // The group-by key is empty, so we never compute an estimate for the amount of memory.
    ASSERT_EQ(stage->getMemoryTracker()->peakTrackedMemoryBytes(), 0);
    ASSERT_EQ(stage->getMemoryTracker()->inUseTrackedMemoryBytes(), 0);

    stage->close();
}

TEST_F(CompactHashAggStageTest, CompactHashAggCloneTest) {
    auto ctx = makeCompileCtx();

    // Build a scan of the [5,6,7,5,6,7,6,7,7] input array.
    auto [scanSlot, scanStage] = generateVirtualScan(jsonArray("[5, 6, 7, 5, 6, 7, 6, 7, 7]"));

    auto groupByOutSlot = generateSlotId();
    auto stage =
        makeS<CompactHashAggStage>(std::move(scanStage),
                                   makeSV(scanSlot),
                                   makeSV(groupByOutSlot),
                                   std::vector<std::unique_ptr<CompactHashAggAccumulator>>{},
                                   boost::none,
                                   /*yieldPolicy=*/nullptr,
                                   kEmptyPlanNodeId);
    auto resultAccessor = prepareTree(ctx.get(), stage.get(), groupByOutSlot);
    auto results = collectIntKeys(stage.get(), resultAccessor);
    stage->close();

    // The clone is independently prepared and produces the same groups.
    auto clonedCtx = makeCompileCtx();
    auto clone = stage->clone();
    auto cloneAccessor = prepareTree(clonedCtx.get(), clone.get(), groupByOutSlot);
    auto clonedResults = collectIntKeys(clone.get(), cloneAccessor);
    clone->close();

    ASSERT_EQ(results, clonedResults);
    ASSERT_EQ(3, results.size());
    ASSERT_EQ(1, results.count(5));
    ASSERT_EQ(1, results.count(6));
    ASSERT_EQ(1, results.count(7));
}

TEST_F(CompactHashAggStageTest, CompactHashAggWithRecordIds) {
    auto ctx = makeCompileCtx();

    // Build a scan of a few record ids.
    std::vector<int64_t> ids{10, 999, 10, 999, 1, 999, 8589869056, 999, 10, 8589869056};
    auto [inputTag, inputVal] = sbe::value::makeNewArray();
    auto testData = sbe::value::getArrayView(inputVal);
    for (auto id : ids) {
        auto [ridTag, ridVal] = sbe::value::makeNewRecordId(id);
        testData->push_back_raw(ridTag, ridVal);
    }
    auto [scanSlot, scanStage] =
        generateVirtualScan(value::TagValueMaybeOwned::fromRaw(true, inputTag, inputVal));

    auto groupByOutSlot = generateSlotId();
    auto stage =
        makeS<CompactHashAggStage>(std::move(scanStage),
                                   makeSV(scanSlot),
                                   makeSV(groupByOutSlot),
                                   std::vector<std::unique_ptr<CompactHashAggAccumulator>>{},
                                   boost::none,
                                   /*yieldPolicy=*/nullptr,
                                   kEmptyPlanNodeId);
    auto resultAccessor = prepareTree(ctx.get(), stage.get(), groupByOutSlot);

    // Read in all of the results.
    std::set<int64_t> results;
    while (stage->getNext() == PlanState::ADVANCED) {
        auto [resScanTag, resScanVal] = resultAccessor->getViewOfValue();
        ASSERT_EQ(value::TypeTags::RecordId, resScanTag);
        ASSERT_TRUE(results
                        .insert(value::bitcastFrom<int64_t>(
                            sbe::value::getRecordIdView(resScanVal)->getLong()))
                        .second);
    }

    // Assert that the results are as expected: one group per distinct record id.
    ASSERT_EQ(4, results.size());
    ASSERT_EQ(1, results.count(1));
    ASSERT_EQ(1, results.count(10));
    ASSERT_EQ(1, results.count(999));
    ASSERT_EQ(1, results.count(8589869056));

    checkMemoryStats(stage.get());
    stage->close();
}

TEST_F(CompactHashAggStageTest, CompactHashAggMemoryLimitNoDiskUse) {
    // Set the memory threshold low enough that only a couple of groups fit in memory. The
    // compact stage does not allow disk use, so exceeding the budget must fail the query with
    // 'QueryExceededMemoryLimitNoDiskUseAllowed' instead of spilling.
    unittest::ServerParameterGuard maxMemoryLimit(
        "internalQuerySlotBasedExecutionHashAggApproxMemoryUseInBytesBeforeSpill", 64);

    auto ctx = makeCompileCtx();

    // Build a scan of the [5,6,7,5,6,7,6,7,7] input array.
    auto [scanSlot, scanStage] = generateVirtualScan(jsonArray("[5, 6, 7, 5, 6, 7, 6, 7, 7]"));

    auto groupByOutSlot = generateSlotId();
    auto stage =
        makeS<CompactHashAggStage>(std::move(scanStage),
                                   makeSV(scanSlot),
                                   makeSV(groupByOutSlot),
                                   std::vector<std::unique_ptr<CompactHashAggAccumulator>>{},
                                   boost::none,
                                   /*yieldPolicy=*/nullptr,
                                   kEmptyPlanNodeId);

    // The stage must throw QueryExceededMemoryLimitNoDiskUseAllowed while building the hash
    // table, instead of spilling.
    ASSERT_THROWS_CODE(
        [&] {
            auto resultAccessor = prepareTree(ctx.get(), stage.get(), groupByOutSlot);
            getAllResults(stage.get(), resultAccessor);
        }(),
        DBException,
        ErrorCodes::QueryExceededMemoryLimitNoDiskUseAllowed);
}

TEST_F(CompactHashAggStageTest, CompactHashAggPushMemoryLimitNoDiskUse) {
    unittest::ServerParameterGuard maxMemoryLimit(
        "internalQuerySlotBasedExecutionHashAggApproxMemoryUseInBytesBeforeSpill", 16 * 1024);

    BSONArrayBuilder rows;
    for (int32_t row = 0; row < 200; ++row) {
        rows << BSON_ARRAY(row % 4 << std::string(1000, 'x'));
    }
    auto ctx = makeCompileCtx();
    auto [scanSlots, scanStage] = generateVirtualScanMulti(2, rows.arr());
    std::vector<std::unique_ptr<CompactHashAggAccumulator>> accumulators;
    accumulators.push_back(std::make_unique<CompactPushHashAggAccumulator>(
        generateSlotId(), makeVariable(scanSlots[1]), std::numeric_limits<int32_t>::max()));
    auto stage = makeS<CompactHashAggStage>(std::move(scanStage),
                                            makeSV(scanSlots[0]),
                                            makeSV(generateSlotId()),
                                            std::move(accumulators),
                                            boost::none,
                                            /*yieldPolicy=*/nullptr,
                                            kEmptyPlanNodeId);

    ASSERT_THROWS_CODE(prepareTree(ctx.get(), stage.get()),
                       DBException,
                       ErrorCodes::QueryExceededMemoryLimitNoDiskUseAllowed);
}

TEST_F(CompactHashAggStageTest, CompactHashAggResetsOutputsOnClose) {
    auto ctx = makeCompileCtx();
    auto [scanSlots, scanStage] = generateVirtualScanMulti(2, jsonArray("[[1, 2], [2, 3]]"));
    auto out = generateSlotId();
    std::vector<std::unique_ptr<CompactHashAggAccumulator>> accumulators;
    accumulators.push_back(std::make_unique<CompactPushHashAggAccumulator>(
        out, makeVariable(scanSlots[1]), std::numeric_limits<int32_t>::max()));
    auto stage = makeS<CompactHashAggStage>(std::move(scanStage),
                                            makeSV(scanSlots[0]),
                                            makeSV(generateSlotId()),
                                            std::move(accumulators),
                                            boost::none,
                                            /*yieldPolicy=*/nullptr,
                                            kEmptyPlanNodeId);
    auto accessor = prepareTree(ctx.get(), stage.get(), out);

    ASSERT_EQ(PlanState::ADVANCED, stage->getNext());
    ASSERT_EQ(value::TypeTags::Array, accessor->getViewOfValue().tag);
    stage->open(true);

    ASSERT_EQ(PlanState::ADVANCED, stage->getNext());
    ASSERT_EQ(value::TypeTags::Array, accessor->getViewOfValue().tag);
    stage->close();
    ASSERT_EQ(value::TypeTags::Nothing, accessor->getViewOfValue().tag);
}

TEST_F(CompactHashAggStageTest, CompactHashAggSum) {
    auto groups = runSums(jsonArray(R"([
        [1, 2],
        [2, 1.5],
        [1, 3],
        [2, NumberLong(4)],
        [3, null],
        [1, NumberDecimal("0.5")]
    ])"));

    assertSameGroups(jsonGroups(R"([
                         [1, NumberDecimal("5.5"), [16, 5.0, 0.0, NumberDecimal("0.5")]],
                         [2, 5.5, [1, 5.5, 0.0]],
                         [3, 0, [16, 0.0, 0.0]]
                     ])"),
                     groups);
}

TEST_F(CompactHashAggStageTest, CompactHashAggSumWithCollator) {
    auto groups = runSums(
        jsonArray(R"([["a", 1], ["A", 2], ["b", 4]])"),
        std::make_unique<CollatorInterfaceMock>(CollatorInterfaceMock::MockType::kToLowerString));

    assertSameGroups(jsonGroups(R"([["a", 3, [16, 3.0, 0.0]], ["b", 4, [16, 4.0, 0.0]]])"), groups);
}

TEST_F(CompactHashAggStageTest, CompactHashAggSumNoGroupBy) {
    auto ctx = makeCompileCtx();
    auto [scanSlots, scanStage] =
        generateVirtualScanMulti(2, jsonArray(R"([[1, 2], [2, 3.5], [3, "x"]])"));
    auto final = generateSlotId();
    auto partial = generateSlotId();
    auto stage = makeS<CompactHashAggStage>(std::move(scanStage),
                                            makeSV(),
                                            makeSV(),
                                            sums(scanSlots[1], final, partial),
                                            boost::none,
                                            /*yieldPolicy=*/nullptr,
                                            kEmptyPlanNodeId);
    auto accessors = prepareTree(ctx.get(), stage.get(), {final, partial});

    ASSERT_EQ(PlanState::ADVANCED, stage->getNext());
    ASSERT_BSONOBJ_EQ(fromjson(R"({"": 5.5})"), toBson(accessors[0]->getViewOfValue()));
    ASSERT_BSONOBJ_EQ(fromjson(R"({"": [1, 5.5, 0.0]})"), toBson(accessors[1]->getViewOfValue()));
    ASSERT_EQ(PlanState::IS_EOF, stage->getNext());
    stage->close();
}

TEST_F(CompactHashAggStageTest, CompactHashAggSumReopenAndClone) {
    auto ctx = makeCompileCtx();
    auto [scanSlots, scanStage] =
        generateVirtualScanMulti(2, jsonArray("[[1, 2], [2, 3.5], [1, 4], [2, 1]]"));
    auto groupByOutSlot = generateSlotId();
    auto final = generateSlotId();
    auto partial = generateSlotId();
    auto stage = makeS<CompactHashAggStage>(std::move(scanStage),
                                            makeSV(scanSlots[0]),
                                            makeSV(groupByOutSlot),
                                            sums(scanSlots[1], final, partial),
                                            boost::none,
                                            /*yieldPolicy=*/nullptr,
                                            kEmptyPlanNodeId);
    auto accessors = prepareTree(ctx.get(), stage.get(), {groupByOutSlot, final, partial});
    auto groups = drainGroups(stage.get(), accessors);
    ASSERT_EQ(2, groups.size());

    stage->open(true);
    assertSameGroups(groups, drainGroups(stage.get(), accessors));
    stage->close();

    auto cloneCtx = makeCompileCtx();
    auto clone = stage->clone();
    assertSameGroups(
        groups,
        drainGroups(clone.get(),
                    prepareTree(cloneCtx.get(), clone.get(), {groupByOutSlot, final, partial})));
    clone->close();
}

TEST_F(CompactHashAggStageTest, CompactHashAggSumMatchesHashAggStage) {
    auto values = fromjson(R"([
        2147483647,
        NumberLong(9223372036854775807),
        -0.0,
        0.1,
        1.7976931348623157e+308,
        Infinity,
        -Infinity,
        NaN,
        null,
        NumberDecimal("1.5"),
        NumberDecimal("Infinity"),
        NumberDecimal("NaN")
    ])");
    BSONArrayBuilder rowsBuilder;
    int32_t key = 0;
    for (auto&& first : values) {
        for (auto&& second : values) {
            rowsBuilder << BSON_ARRAY(key << first) << BSON_ARRAY(key << second);
            ++key;
        }
    }
    for (auto&& value : values) {
        rowsBuilder << BSON_ARRAY(key << value);
    }
    auto rows = rowsBuilder.arr();

    auto ctx = makeCompileCtx();
    auto [scanSlots, scanStage] = generateVirtualScanMulti(2, rows);
    auto keySlot = generateSlotId();
    auto stateSlot = generateSlotId();
    auto spillSlot = generateSlotId();
    auto stage = makeS<HashAggStage>(
        std::move(scanStage),
        makeSV(scanSlots[0]),
        makeSV(keySlot),
        makeHashAggAccumulatorList(std::make_unique<CompiledHashAggAccumulator>(
            stateSlot,
            spillSlot,
            makeFunction(EFn::kAggDoubleDoubleSum, makeVariable(scanSlots[1])),
            makeFunction(EFn::kAggMergeDoubleDoubleSums, makeVariable(spillSlot)))),
        true,
        boost::none,
        /*allowDiskUse=*/false,
        /*yieldPolicy=*/nullptr,
        kEmptyPlanNodeId);
    auto accessors = prepareTree(ctx.get(), stage.get(), {keySlot, stateSlot});
    Groups expected;
    while (stage->getNext() == PlanState::ADVANCED) {
        auto state = accessors[1]->getViewOfValue();
        expected.emplace(
            toBson(accessors[0]->getViewOfValue()).toString(),
            std::vector<BSONObj>{toBson(vm::ByteCode::aggDoubleDoubleSumFinalizeImpl(
                                            value::getArrayView(state.value))
                                            .view()),
                                 toBson(vm::ByteCode::builtinDoubleDoublePartialSumFinalizeImpl(
                                            state.tag, state.value)
                                            .view())});
    }
    stage->close();

    size_t count = values.nFields();
    ASSERT_EQ(count * count + 1, expected.size());
    assertSameGroups(expected, runSums(rows));
}

TEST_F(CompactHashAggStageTest, CompactHashAggSumWithCollatorMatchesHashAggStageOnReopenAndClose) {
    auto rows = jsonArray(R"([["a", 1], ["A", 2], ["b", 4], ["B", 8.5], ["c", NumberLong(3)]])");
    auto makeCollator = [] {
        return std::make_unique<CollatorInterfaceMock>(
            CollatorInterfaceMock::MockType::kToLowerString);
    };

    Groups expected;
    {
        auto ctx = makeCompileCtx();
        auto collatorSlot = generateSlotId();
        value::OwnedValueAccessor collatorAccessor;
        ctx->pushCorrelated(collatorSlot, &collatorAccessor);
        collatorAccessor.reset(value::TypeTags::collator,
                               value::bitcastFrom<CollatorInterface*>(makeCollator().release()));
        auto [scanSlots, scanStage] = generateVirtualScanMulti(2, rows);
        auto keySlot = generateSlotId();
        auto stateSlot = generateSlotId();
        auto spillSlot = generateSlotId();
        auto stage = makeS<HashAggStage>(
            std::move(scanStage),
            makeSV(scanSlots[0]),
            makeSV(keySlot),
            makeHashAggAccumulatorList(std::make_unique<CompiledHashAggAccumulator>(
                stateSlot,
                spillSlot,
                makeFunction(EFn::kAggDoubleDoubleSum, makeVariable(scanSlots[1])),
                makeFunction(EFn::kAggMergeDoubleDoubleSums, makeVariable(spillSlot)))),
            true,
            collatorSlot,
            /*allowDiskUse=*/false,
            /*yieldPolicy=*/nullptr,
            kEmptyPlanNodeId);
        auto accessors = prepareTree(ctx.get(), stage.get(), {keySlot, stateSlot});
        while (stage->getNext() == PlanState::ADVANCED) {
            auto state = accessors[1]->getViewOfValue();
            expected.emplace(
                toBson(accessors[0]->getViewOfValue()).toString(),
                std::vector<BSONObj>{toBson(vm::ByteCode::aggDoubleDoubleSumFinalizeImpl(
                                                value::getArrayView(state.value))
                                                .view()),
                                     toBson(vm::ByteCode::builtinDoubleDoublePartialSumFinalizeImpl(
                                                state.tag, state.value)
                                                .view())});
        }
        stage->close();
    }
    ASSERT_EQ(3, expected.size());

    auto ctx = makeCompileCtx();
    auto collatorSlot = generateSlotId();
    value::OwnedValueAccessor collatorAccessor;
    ctx->pushCorrelated(collatorSlot, &collatorAccessor);
    collatorAccessor.reset(value::TypeTags::collator,
                           value::bitcastFrom<CollatorInterface*>(makeCollator().release()));
    auto [scanSlots, scanStage] = generateVirtualScanMulti(2, rows);
    auto groupByOutSlot = generateSlotId();
    auto final = generateSlotId();
    auto partial = generateSlotId();
    auto stage = makeS<CompactHashAggStage>(std::move(scanStage),
                                            makeSV(scanSlots[0]),
                                            makeSV(groupByOutSlot),
                                            sums(scanSlots[1], final, partial),
                                            collatorSlot,
                                            /*yieldPolicy=*/nullptr,
                                            kEmptyPlanNodeId);
    auto accessors = prepareTree(ctx.get(), stage.get(), {groupByOutSlot, final, partial});
    assertSameGroups(expected, drainGroups(stage.get(), accessors));

    stage->open(true);
    assertSameGroups(expected, drainGroups(stage.get(), accessors));

    stage->close();
    stage->open(false);
    assertSameGroups(expected, drainGroups(stage.get(), accessors));

    stage->close();
}

TEST_F(CompactHashAggStageTest, CompactHashAggDestroysStatesOnEarlyClose) {
    auto stage = makeCountingStage(jsonArray("[1, 2, 3, 1]"), 2);
    ASSERT_EQ(6, CountedState::live);
    ASSERT_EQ(PlanState::ADVANCED, stage->getNext());
    stage->close();
    ASSERT_EQ(0, CountedState::live);
}

TEST_F(CompactHashAggStageTest, CompactHashAggDestroysStatesOnReopen) {
    auto stage = makeCountingStage(jsonArray("[1, 2, 3, 1]"), 1);
    stage->open(true);
    ASSERT_EQ(3, CountedState::live);
    stage->close();
    ASSERT_EQ(0, CountedState::live);
}

TEST_F(CompactHashAggStageTest, CompactHashAggDestroysStatesOnDestruction) {
    auto stage = makeCountingStage(jsonArray("[1, 2, 3, 1]"), 1);
    ASSERT_EQ(3, CountedState::live);
    stage.reset();
    ASSERT_EQ(0, CountedState::live);
}

TEST_F(CompactHashAggStageTest, CompactHashAggDestroysStatesWhenOpenThrows) {
    unittest::ServerParameterGuard maxMemoryLimit(
        "internalQuerySlotBasedExecutionHashAggApproxMemoryUseInBytesBeforeSpill", 64);
    ASSERT_THROWS_CODE(makeCountingStage(jsonArray("[1, 2, 3, 4, 5, 6]"), 1),
                       DBException,
                       ErrorCodes::QueryExceededMemoryLimitNoDiskUseAllowed);
    ASSERT_EQ(0, CountedState::live);
}

}  // namespace
}  // namespace mongo::sbe
