// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/sbe/expressions/sbe_fn_names.h"
#include "mongo/db/exec/sbe/sbe_plan_stage_test.h"
#include "mongo/db/exec/sbe/stages/hash_agg_accumulator.h"
#include "mongo/db/exec/sbe/stages/project.h"
#include "mongo/db/exec/sbe/stages/stages.h"
#include "mongo/db/exec/sbe/stages/streaming_agg.h"
#include "mongo/db/exec/sbe/values/slot.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/query/collation/collator_interface_mock.h"
#include "mongo/unittest/tassert_guard.h"
#include "mongo/unittest/unittest.h"

#include <algorithm>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mongo::sbe {
/**
 * Tests for sbe::StreamingAggStage.
 */
class StreamingAggStageTest : public PlanStageTestFixture {
protected:
    static std::unique_ptr<PlanStage> makeStreamingAgg(
        std::unique_ptr<PlanStage> scanStage,
        value::SlotVector keySlots,
        boost::optional<value::SlotId> collatorSlot = boost::none) {
        // No separate cluster key slots, so the cluster keys equal the group keys.
        return makeS<StreamingAggStage>(std::move(scanStage),
                                        keySlots,
                                        keySlots,
                                        collatorSlot,
                                        std::vector<std::unique_ptr<HashAggAccumulator>>{},
                                        kEmptyPlanNodeId);
    }

    struct GroupPlan {
        std::unique_ptr<PlanStage> root;
        PlanStage* agg = nullptr;
        std::unique_ptr<CompileCtx> ctx;
        std::vector<value::SlotAccessor*> accessors;
        value::SlotVector outSlots;
        value::SlotVector keySlots;
        value::SlotVector clusterKeySlots;
        std::unique_ptr<value::OwnedValueAccessor> collatorAccessor;
    };

    GroupPlan makeGroupPlan(const BSONArray& docs,
                            const std::vector<std::string>& keyFields,
                            std::vector<bool> fillEmptyClusterKeys,
                            EFn accumulatorFn,
                            boost::optional<CollatorInterface*> collator = boost::none) {
        return makeGroupPlanAccs(
            docs,
            keyFields,
            std::move(fillEmptyClusterKeys),
            [&](value::SlotId valueSlot, boost::optional<value::SlotId>) {
                return makeHashAggAccumulatorList(std::make_unique<CompiledHashAggAccumulator>(
                    generateSlotId(),
                    generateSlotId(),
                    makeFunction(accumulatorFn, makeVariable(valueSlot)),
                    makeConstant(value::TypeTags::Nothing, 0)));
            },
            collator);
    }

    // Like makeGroupPlan, but the caller supplies the accumulators.
    GroupPlan makeGroupPlanAccs(
        const BSONArray& docs,
        const std::vector<std::string>& keyFields,
        std::vector<bool> fillEmptyClusterKeys,
        const std::function<std::vector<std::unique_ptr<HashAggAccumulator>>(
            value::SlotId, boost::optional<value::SlotId>)>& makeAccumulators,
        boost::optional<CollatorInterface*> collator = boost::none) {
        auto [scanSlot, scanStage] = generateVirtualScan(docs);

        auto getField = [&, scanSlot = scanSlot](value::SlotId slot, std::string_view field) {
            return std::make_pair(
                slot,
                makeFunction(EFn::kGetField, makeVariable(scanSlot), makeStringConstant(field)));
        };

        value::SlotVector keySlots;
        value::SlotVector clusterKeySlots;
        SlotExprPairVector fields;
        for (size_t i = 0; i < keyFields.size(); ++i) {
            auto keySlot = generateSlotId();
            keySlots.push_back(keySlot);
            fields.push_back(getField(keySlot, keyFields[i]));

            if (fillEmptyClusterKeys[i]) {
                auto clusterSlot = generateSlotId();
                clusterKeySlots.push_back(clusterSlot);
                auto [slot, expr] = getField(clusterSlot, keyFields[i]);
                fields.emplace_back(slot,
                                    makeE<EPrimBinary>(EPrimBinary::fillEmpty,
                                                       std::move(expr),
                                                       makeE<EConstant>(value::TypeTags::Null, 0)));
            } else {
                clusterKeySlots.push_back(keySlot);
            }
        }
        auto valueSlot = generateSlotId();
        fields.push_back(getField(valueSlot, "v"));

        boost::optional<value::SlotId> collatorSlot = boost::none;
        if (collator) {
            collatorSlot = generateSlotId();
        }

        auto accumulators = makeAccumulators(valueSlot, collatorSlot);
        value::SlotVector accOutSlots;
        for (const auto& accumulator : accumulators) {
            accOutSlots.push_back(accumulator->getOutSlot());
        }

        auto agg = makeS<StreamingAggStage>(
            makeS<ProjectStage>(std::move(scanStage), std::move(fields), kEmptyPlanNodeId),
            keySlots,
            clusterKeySlots,
            collatorSlot,
            std::move(accumulators),
            kEmptyPlanNodeId);

        value::SlotVector outSlots;
        SlotExprPairVector outFields;
        // Render each group key, using the string "missing" for a Nothing value.
        for (auto keySlot : keySlots) {
            outSlots.push_back(generateSlotId());
            outFields.emplace_back(outSlots.back(),
                                   makeE<EPrimBinary>(EPrimBinary::fillEmpty,
                                                      makeVariable(keySlot),
                                                      makeStringConstant("missing")));
        }
        for (auto accOutSlot : accOutSlots) {
            outSlots.push_back(accOutSlot);
        }

        GroupPlan plan;
        plan.agg = agg.get();
        plan.keySlots = keySlots;
        plan.clusterKeySlots = clusterKeySlots;
        plan.root = makeS<ProjectStage>(std::move(agg), std::move(outFields), kEmptyPlanNodeId);
        plan.ctx = makeCompileCtx();
        if (collator) {
            plan.collatorAccessor = std::make_unique<value::OwnedValueAccessor>();
            plan.ctx->pushCorrelated(*collatorSlot, plan.collatorAccessor.get());
            plan.collatorAccessor->reset(value::TypeTags::collator,
                                         value::bitcastFrom<CollatorInterface*>(*collator));
        }
        plan.accessors = prepareTree(plan.ctx.get(), plan.root.get(), outSlots);
        plan.outSlots = std::move(outSlots);
        return plan;
    }

    static std::vector<value::TagValueView> sortedRows(value::TypeTags tag, value::Value val) {
        std::vector<value::TagValueView> rows;
        for (value::ArrayEnumerator it{tag, val}; !it.atEnd(); it.advance()) {
            rows.push_back(it.getViewOfValue());
        }
        std::sort(rows.begin(), rows.end(), [](const auto& lhs, const auto& rhs) {
            auto [_, cmp] = value::compareValue(lhs.tag, lhs.value, rhs.tag, rhs.value);
            return value::bitcastTo<int32_t>(cmp) < 0;
        });
        return rows;
    }

    void assertSameRows(PlanStage* root,
                        const std::vector<value::SlotAccessor*>& accessors,
                        const BSONArray& expected) {
        auto results = value::TagValueOwned::fromRaw(getAllResultsMulti(root, accessors));
        auto expectedValue = value::TagValueOwned::fromRaw(stage_builder::makeValue(expected));
        auto actualRows = sortedRows(results.tag(), results.value());
        auto expectedRows = sortedRows(expectedValue.tag(), expectedValue.value());
        ASSERT_EQ(actualRows.size(), expectedRows.size());
        for (size_t idx = 0; idx < actualRows.size(); ++idx) {
            EXPECT_TRUE(valueEquals(actualRows[idx].tag,
                                    actualRows[idx].value,
                                    expectedRows[idx].tag,
                                    expectedRows[idx].value))
                << "row " << idx << ", actual: " << actualRows[idx]
                << ", expected: " << expectedRows[idx];
        }
    }

    void runGroupTest(const BSONArray& docs,
                      const std::vector<std::string>& keyFields,
                      std::vector<bool> fillEmptyClusterKeys,
                      EFn accumulatorFn,
                      const BSONArray& expected,
                      boost::optional<CollatorInterface*> collator = boost::none) {
        auto plan = makeGroupPlan(
            docs, keyFields, std::move(fillEmptyClusterKeys), accumulatorFn, collator);
        assertSameRows(plan.root.get(), plan.accessors, expected);
    }

    static BSONArray wrapEachElement(const BSONArray& values) {
        BSONArrayBuilder builder;
        for (const auto& value : values) {
            builder.append(BSON_ARRAY(value));
        }
        return builder.arr();
    }

    void runSingleKeyTest(const BSONArray& input, const BSONArray& expected) {
        runMultiKeyTest(1, wrapEachElement(input), wrapEachElement(expected));
    }

    void runMultiKeyTest(int numSlots, const BSONArray& input, const BSONArray& expected) {
        value::TagValueOwned inputValue =
            value::TagValueOwned::fromRaw(stage_builder::makeValue(input));
        value::TagValueOwned expectedValue =
            value::TagValueOwned::fromRaw(stage_builder::makeValue(expected));

        auto makeStageFn = [](value::SlotVector scanSlots, std::unique_ptr<PlanStage> scanStage) {
            return std::make_pair(scanSlots, makeStreamingAgg(std::move(scanStage), scanSlots));
        };

        runTestMulti(numSlots,
                     inputValue.tag(),
                     inputValue.value(),
                     expectedValue.tag(),
                     expectedValue.value(),
                     makeStageFn);
        inputValue.disown();
        expectedValue.disown();
    }

    void assertResults(PlanStage* stage,
                       value::SlotAccessor* accessor,
                       value::TypeTags expectedTag,
                       value::Value expectedVal) {
        value::TagValueOwned results =
            value::TagValueOwned::fromRaw(getAllResults(stage, accessor));
        ASSERT_TRUE(valueEquals(results.tag(), results.value(), expectedTag, expectedVal));
    }
};

TEST_F(StreamingAggStageTest, DeduplicatesSortedInputSingleKey) {
    runSingleKeyTest(BSON_ARRAY(1 << 1 << 2 << 3 << 3 << 3), BSON_ARRAY(1 << 2 << 3));
}

TEST_F(StreamingAggStageTest, PreservesOrderWhenNoDuplicates) {
    runSingleKeyTest(BSON_ARRAY(1 << 2 << 3), BSON_ARRAY(1 << 2 << 3));
}

TEST_F(StreamingAggStageTest, PreservesOrderWithUnsortedUniqueKeys) {
    runSingleKeyTest(BSON_ARRAY(3 << 1 << 2), BSON_ARRAY(3 << 1 << 2));
}

TEST_F(StreamingAggStageTest, CollapsesAllIdenticalKeys) {
    runSingleKeyTest(BSON_ARRAY(5 << 5 << 5), BSON_ARRAY(5));
}

TEST_F(StreamingAggStageTest, HandlesEmptyInput) {
    runSingleKeyTest(BSONArray{}, BSONArray{});
}

TEST_F(StreamingAggStageTest, DeduplicatesMultipleSlotsInKey) {
    runMultiKeyTest(2,
                    BSON_ARRAY(BSON_ARRAY(1 << 2) << BSON_ARRAY(1 << 2) << BSON_ARRAY(1 << 3)),
                    BSON_ARRAY(BSON_ARRAY(1 << 2) << BSON_ARRAY(1 << 3)));
}

TEST_F(StreamingAggStageTest, DoesNotDedupWhenOnlyFirstSlotMatches) {
    runMultiKeyTest(2,
                    BSON_ARRAY(BSON_ARRAY(1 << 2) << BSON_ARRAY(1 << 3)),
                    BSON_ARRAY(BSON_ARRAY(1 << 2) << BSON_ARRAY(1 << 3)));
}

TEST_F(StreamingAggStageTest, DoesNotDedupWhenOnlySecondSlotMatches) {
    runMultiKeyTest(2,
                    BSON_ARRAY(BSON_ARRAY(1 << 2) << BSON_ARRAY(2 << 2)),
                    BSON_ARRAY(BSON_ARRAY(1 << 2) << BSON_ARRAY(2 << 2)));
}

TEST_F(StreamingAggStageTest, RespectsCollation) {
    value::TagValueOwned inputValue =
        value::TagValueOwned::fromRaw(stage_builder::makeValue(BSON_ARRAY("a" << "A" << "b")));
    value::TagValueOwned expectedValue =
        value::TagValueOwned::fromRaw(stage_builder::makeValue(BSON_ARRAY("a" << "b")));

    auto collator =
        std::make_unique<CollatorInterfaceMock>(CollatorInterfaceMock::MockType::kToLowerString);

    auto collatorSlot = generateSlotId();

    auto [scanSlot, scanStage] = generateVirtualScan(std::move(inputValue));

    auto stage = makeStreamingAgg(std::move(scanStage), sbe::makeSV(scanSlot), collatorSlot);

    auto ctx = makeCompileCtx();

    value::OwnedValueAccessor collatorAccessor;
    ctx->pushCorrelated(collatorSlot, &collatorAccessor);
    collatorAccessor.reset(value::TypeTags::collator,
                           value::bitcastFrom<CollatorInterface*>(collator.release()));

    auto resultAccessor = prepareTree(ctx.get(), stage.get(), scanSlot);
    assertResults(stage.get(), resultAccessor, expectedValue.tag(), expectedValue.value());
}

TEST_F(StreamingAggStageTest, ResetsStateAfterReopen) {
    value::TagValueOwned inputValue =
        value::TagValueOwned::fromRaw(stage_builder::makeValue(BSON_ARRAY(1 << 1 << 2 << 3 << 3)));
    value::TagValueOwned expectedValue =
        value::TagValueOwned::fromRaw(stage_builder::makeValue(BSON_ARRAY(1 << 2 << 3)));

    auto [scanSlot, scanStage] = generateVirtualScan(std::move(inputValue));

    auto stage = makeStreamingAgg(std::move(scanStage), sbe::makeSV(scanSlot));
    auto ctx = makeCompileCtx();
    auto resultAccessor = prepareTree(ctx.get(), stage.get(), scanSlot);

    assertResults(stage.get(), resultAccessor, expectedValue.tag(), expectedValue.value());

    stage->close();
    stage->open(true);

    assertResults(stage.get(), resultAccessor, expectedValue.tag(), expectedValue.value());
}

TEST_F(StreamingAggStageTest, SumAccumulatorGroupsByKey) {
    value::TagValueOwned inputValue =
        value::TagValueOwned::fromRaw(stage_builder::makeValue(BSON_ARRAY(
            BSON_ARRAY(1 << 2) << BSON_ARRAY(1 << 3) << BSON_ARRAY(2 << 2) << BSON_ARRAY(2 << 4))));
    value::TagValueOwned expectedValue = value::TagValueOwned::fromRaw(
        stage_builder::makeValue(BSON_ARRAY(BSON_ARRAY(1 << 5) << BSON_ARRAY(2 << 6))));

    auto [scanSlots, scanStage] = generateVirtualScanMulti(2, inputValue.tag(), inputValue.value());
    inputValue.disown();
    auto keySlot = scanSlots[0];
    auto valueSlot = scanSlots[1];

    auto sumSlot = generateSlotId();
    auto spillSlot = generateSlotId();
    std::vector<std::unique_ptr<HashAggAccumulator>> accumulators;
    accumulators.emplace_back(std::make_unique<CompiledHashAggAccumulator>(
        sumSlot,
        spillSlot,
        makeFunction(EFn::kSum, makeVariable(valueSlot)),
        makeE<EConstant>(value::TypeTags::Nothing, 0)));

    auto stage = makeS<StreamingAggStage>(std::move(scanStage),
                                          sbe::makeSV(keySlot),
                                          sbe::makeSV(keySlot),
                                          boost::none,
                                          std::move(accumulators),
                                          kEmptyPlanNodeId);

    auto ctx = makeCompileCtx();
    auto resultAccessors = prepareTree(ctx.get(), stage.get(), sbe::makeSV(keySlot, sumSlot));

    value::TagValueOwned results =
        value::TagValueOwned::fromRaw(getAllResultsMulti(stage.get(), resultAccessors));
    ASSERT_TRUE(
        valueEquals(results.tag(), results.value(), expectedValue.tag(), expectedValue.value()));
}

TEST_F(StreamingAggStageTest, SumAccumulatorWidensAcrossNumericTypes) {
    // A single group accumulates int, long long, double, and decimal values; the accumulator state
    // must widen as each wider type arrives and finalize to a decimal.
    runGroupTest(BSON_ARRAY(BSON("a" << 1 << "v" << 1)                      // int
                            << BSON("a" << 1 << "v" << 2LL)                 // long long
                            << BSON("a" << 1 << "v" << 0.5)                 // double
                            << BSON("a" << 1 << "v" << Decimal128("0.25"))  // decimal
                            ),
                 {"a"},
                 {false},
                 EFn::kSum,
                 BSON_ARRAY(BSON_ARRAY(1 << Decimal128("3.75"))));
}

TEST_F(StreamingAggStageTest, NumericKeysOfDifferentTypesGroupTogether) {
    // int32 1, int64 1, double 1.0, and Decimal128(1) compare equal as group keys, so they form a
    // single group; the emitted key keeps the first row's type.
    runGroupTest(BSON_ARRAY(BSON("a" << 1 << "v" << 1)
                            << BSON("a" << 1LL << "v" << 2) << BSON("a" << 1.0 << "v" << 4)
                            << BSON("a" << Decimal128("1") << "v" << 8)
                            << BSON("a" << 2 << "v" << 16)),
                 {"a"},
                 {false},
                 EFn::kSum,
                 BSON_ARRAY(BSON_ARRAY(1 << 15) << BSON_ARRAY(2 << 16)));
}

TEST_F(StreamingAggStageTest, ClusteredInterleavedNullAndMissing) {
    runGroupTest(BSON_ARRAY(BSON("a" << 1 << "v" << 1)                        // 1,missing
                            << BSON("a" << 1 << "b" << BSONNULL << "v" << 2)  // 1,null
                            << BSON("a" << 1 << "b" << BSONNULL << "v" << 8)  // 1,null
                            << BSON("a" << 1 << "v" << 4)),                   // 1,missing

                 {"a", "b"},
                 {false, true},
                 EFn::kSum,
                 BSON_ARRAY(BSON_ARRAY(1 << "missing" << 5) << BSON_ARRAY(1 << BSONNULL << 10)));
}

TEST_F(StreamingAggStageTest, ClusteredInterleavedNullAndMissingAndOtherValues) {
    // clustered slot b has null, missing, and integers. clusters have different patterns of group
    // count and sequences of values
    runGroupTest(BSON_ARRAY(BSON("a" << 1 << "v" << 1)                         // 1,missing
                            << BSON("a" << 1 << "b" << BSONNULL << "v" << 2)   // 1,null
                            << BSON("a" << 1 << "b" << 3 << "v" << 4)          // 1,3
                            << BSON("a" << 1 << "b" << 3 << "v" << 8)          // 1,3
                            << BSON("a" << 2 << "b" << BSONNULL << "v" << 3)   // 2,null
                            << BSON("a" << 3 << "b" << BSONNULL << "v" << 5)   // 3,null
                            << BSON("a" << 4 << "b" << BSONNULL << "v" << 15)  // 4,null
                            << BSON("a" << 4 << "v" << 9)                      // 4,missing
                            << BSON("a" << 4 << "b" << BSONNULL << "v" << 3)   // 4,null
                            << BSON("a" << 4 << "b" << 3 << "v" << 32)),       // 4,3
                 {"a", "b"},
                 {false, true},
                 EFn::kSum,
                 BSON_ARRAY(BSON_ARRAY(1 << "missing" << 1)
                            << BSON_ARRAY(1 << BSONNULL << 2) << BSON_ARRAY(1 << 3 << 12)
                            << BSON_ARRAY(2 << BSONNULL << 3) << BSON_ARRAY(3 << BSONNULL << 5)
                            << BSON_ARRAY(4 << BSONNULL << 18) << BSON_ARRAY(4 << "missing" << 9)
                            << BSON_ARRAY(4 << 3 << 32)));
}

TEST_F(StreamingAggStageTest, NoClusteringComparesNullAndMissingExactly) {
    // b isn't clustered, null and missing appear out of order and form separate groups
    // this is an invalid input in practice, since the clusters aren't contiguous
    runGroupTest(BSON_ARRAY(BSON("a" << 1 << "v" << 1)                        // 1,missing
                            << BSON("a" << 1 << "v" << 2)                     // 1,missing
                            << BSON("a" << 1 << "b" << BSONNULL << "v" << 4)  // 1,null
                            << BSON("a" << 1 << "v" << 8)),                   // 1,missing
                 {"a", "b"},
                 {false, false},
                 EFn::kSum,
                 BSON_ARRAY(BSON_ARRAY(1 << "missing" << 3)
                            << BSON_ARRAY(1 << BSONNULL << 4) << BSON_ARRAY(1 << "missing" << 8)));
}

TEST_F(StreamingAggStageTest, ClusteredAndUnclusteredKeysTogether) {
    // a and b aren't clustered, c is.
    // the stage must differentiate (a:1, c:missing, b:null) from (a:1, c:null, b:null)
    runGroupTest(BSON_ARRAY(BSON("a" << 1 << "v" << 1)                        // 1,missing,missing
                            << BSON("a" << 1 << "c" << BSONNULL << "v" << 2)  // 1,missing,null
                            << BSON("a" << 1 << "v" << 4)                     // 1,missing,missing
                            << BSON("a" << 1 << "b" << BSONNULL << "v" << 8)  // 1,null,missing
                            << BSON("a" << 1 << "b" << BSONNULL << "c" << BSONNULL  // 1,null,null
                                        << "v" << 16)
                            << BSON("a" << 1 << "b" << BSONNULL << "v" << 32)),  // 1,null,missing
                 {"a", "b", "c"},
                 {false, false, true},
                 EFn::kSum,
                 BSON_ARRAY(BSON_ARRAY(1 << "missing" << "missing" << 5)
                            << BSON_ARRAY(1 << "missing" << BSONNULL << 2)
                            << BSON_ARRAY(1 << BSONNULL << "missing" << 40)
                            << BSON_ARRAY(1 << BSONNULL << BSONNULL << 16)));
}

TEST_F(StreamingAggStageTest, EmptyInputWithSeparateClusterKeys) {
    runGroupTest(BSONArray{}, {"a", "b"}, {false, true}, EFn::kSum, BSONArray{});
}

TEST_F(StreamingAggStageTest, CloneKeepsSeparateClusterKeySlots) {
    auto plan = makeGroupPlan(BSON_ARRAY(BSON("a" << 1 << "v" << 1)
                                         << BSON("a" << 1 << "b" << BSONNULL << "v" << 2)
                                         << BSON("a" << 1 << "v" << 4)),
                              {"a", "b"},
                              {false, true},
                              EFn::kSum);
    auto clone = plan.root->clone();
    auto ctx = makeCompileCtx();
    auto accessors = prepareTree(ctx.get(), clone.get(), plan.outSlots);
    assertSameRows(clone.get(),
                   accessors,
                   BSON_ARRAY(BSON_ARRAY(1 << "missing" << 5) << BSON_ARRAY(1 << BSONNULL << 2)));
}

TEST_F(StreamingAggStageTest, RejectsMismatchedClusterKeySlotCount) {
    // 2 group keys, 1 cluster key
    auto [scanSlots, scanStage] = generateVirtualScanMulti(2, BSONArray{});
    ASSERT_TASSERT_CODE(makeS<StreamingAggStage>(std::move(scanStage),
                                                 scanSlots,
                                                 value::SlotVector{scanSlots[0]},
                                                 boost::none,
                                                 std::vector<std::unique_ptr<HashAggAccumulator>>{},
                                                 kEmptyPlanNodeId),
                        13625700);
}

TEST_F(StreamingAggStageTest, TassertsOnArrayKeyInFirstRow) {
    auto [scanSlot, scanStage] = generateVirtualScan(BSON_ARRAY(BSON_ARRAY(1 << 2) << 3));
    auto stage = makeStreamingAgg(std::move(scanStage), sbe::makeSV(scanSlot));
    auto ctx = makeCompileCtx();
    ASSERT_TASSERT_CODE(prepareTree(ctx.get(), stage.get(), scanSlot), 13857200);
}

TEST_F(StreamingAggStageTest, TassertsOnArrayKeyInLaterRow) {
    auto [scanSlot, scanStage] = generateVirtualScan(BSON_ARRAY(1 << 1 << BSON_ARRAY(2 << 3)));
    auto stage = makeStreamingAgg(std::move(scanStage), sbe::makeSV(scanSlot));
    auto ctx = makeCompileCtx();
    auto resultAccessor = prepareTree(ctx.get(), stage.get(), scanSlot);
    ASSERT_TASSERT_CODE(getAllResults(stage.get(), resultAccessor), 13857200);
}

TEST_F(StreamingAggStageTest, StatsDebugInfoReportsClusterKeySlots) {
    auto plan = makeGroupPlan(BSONArray{}, {"a", "b"}, {false, true}, EFn::kSum);

    // plan.root is the output ProjectStage whose only child is the StreamingAggStage.
    auto stats = plan.root->getStats(true);
    auto aggStats = stats->children[0].get();
    ASSERT_EQ(aggStats->common.stageType, "streaming_group");
    auto clustered = aggStats->debugInfo["clusterKeySlots"];
    ASSERT_TRUE(clustered.type() == BSONType::array);
    auto arr = clustered.Array();
    ASSERT_EQ(arr.size(), 2);
    // The first position reuses the group-by slot id; the second has its own cluster key slot id.
    ASSERT_EQ(arr[0].numberLong(), static_cast<long long>(plan.keySlots[0]));
    ASSERT_EQ(arr[1].numberLong(), static_cast<long long>(plan.clusterKeySlots[1]));
}

TEST_F(StreamingAggStageTest, DebugPrintReportsClusterKeySlots) {
    auto plan = makeGroupPlan(BSONArray{}, {"a", "b"}, {false, true}, EFn::kSum);

    DebugPrintInfo printInfo;
    DebugPrinter printer;
    auto printed = printer.print(*plan.root, printInfo);
    ASSERT_STRING_CONTAINS(printed, "clustered");
    // The cluster key slot list must include both the shared group-by slot and the separate
    // cluster key slot.
    ASSERT_STRING_CONTAINS(printed, str::stream() << "s" << plan.clusterKeySlots[0]);
    ASSERT_STRING_CONTAINS(printed, str::stream() << "s" << plan.clusterKeySlots[1]);
}

TEST_F(StreamingAggStageTest, SeparatesAllCombinationsOfFourInterleavedKeyFields) {
    // For 4 group keys, the docs have every permutation of missing and null for each key, repeated
    // twice. Each of the 32 documents has the same all-null cluster key, but across 16 groups.
    BSONArrayBuilder docs;
    for (unsigned pass = 0; pass < 2; ++pass) {
        for (unsigned group = 0; group < 16; ++group) {
            BSONObjBuilder bob;
            bob.append("a", 1);
            for (unsigned field = 0; field < 4; ++field) {
                const bool missing = ((group >> field) & 1) != pass;
                if (!missing) {
                    bob.appendNull(std::string(1, 'b' + field));
                }
            }
            bob.append("v", static_cast<int>(2 * group + pass + 1));
            docs.append(bob.obj());
        }
    }

    BSONArrayBuilder expected;
    for (unsigned group = 0; group < 16; ++group) {
        BSONArrayBuilder row;
        row.append(1);
        for (unsigned field = 0; field < 4; ++field) {
            if ((group >> field) & 1) {
                row.append("missing");
            } else {
                row.appendNull();
            }
        }
        row.append(BSON_ARRAY(static_cast<int>(2 * group + 1)            // v from pass 0
                              << static_cast<int>(2 * (15 - group) + 2)  // v from pass 1
                              ));
        expected.append(row.arr());
    }

    runGroupTest(docs.arr(),
                 {"a", "b", "c", "d", "e"},
                 {false, true, true, true, true},
                 EFn::kAddToArray,
                 expected.arr());
}

TEST_F(StreamingAggStageTest, RejectsMoreThanFourSeparateClusterKeySlots) {
    // The stage supports at most kMaxClusteredGroupKeys (4) key positions with a separate cluster
    // key slot, so construction fails with five.
    auto [scanSlots, scanStage] = generateVirtualScanMulti(10, BSONArray{});
    ASSERT_TASSERT_CODE(
        makeS<StreamingAggStage>(
            std::move(scanStage),
            value::SlotVector{scanSlots.begin(), scanSlots.begin() + 5},
            value::SlotVector{scanSlots[5], scanSlots[6], scanSlots[7], scanSlots[8], scanSlots[9]},
            boost::none,
            std::vector<std::unique_ptr<HashAggAccumulator>>{},
            kEmptyPlanNodeId),
        13625701);
}

TEST_F(StreamingAggStageTest, MaxSeparateClusterKeysBesideSharedClusterKeys) {
    // kMaxClusteredGroupKeys (4) separate cluster key slots beside two shared ones; the shared
    // fields (a, b) determine the clusters, and the interleaved fields (c-f) split each cluster
    // into up to four groups by null/missing combination.
    runGroupTest(
        BSON_ARRAY(BSON("a" << 1 << "b" << "x" << "v" << 1) /* c-f missing */
                   << BSON("a" << 1 << "b" << "x" << "c" << BSONNULL << "v" << 2)
                   << BSON("a" << 1 << "b" << "x" << "c" << BSONNULL << "d" << BSONNULL << "e"
                               << BSONNULL << "f" << BSONNULL << "v" << 4)
                   << BSON("a" << 1 << "b" << "x" << "v" << 8) /* c-f missing */
                   << BSON("a" << 2 << "b" << "y" << "e" << BSONNULL << "v" << 16)
                   << BSON("a" << 2 << "b" << "y" << "v" << 32)), /* c-f missing */
        {"a", "b", "c", "d", "e", "f"},
        {false, false, true, true, true, true},
        EFn::kSum,
        BSON_ARRAY(
            BSON_ARRAY(1 << "x" << "missing" << "missing" << "missing" << "missing" << 9)
            << BSON_ARRAY(1 << "x" << BSONNULL << "missing" << "missing" << "missing" << 2)
            << BSON_ARRAY(1 << "x" << BSONNULL << BSONNULL << BSONNULL << BSONNULL << 4)
            << BSON_ARRAY(2 << "y" << "missing" << "missing" << BSONNULL << "missing" << 16)
            << BSON_ARRAY(2 << "y" << "missing" << "missing" << "missing" << "missing" << 32)));
}

TEST_F(StreamingAggStageTest, CollatorAndSeparateClusterKeySlotTogether) {
    // "x" and "X" are equal under the lowercase collator, so all four rows form one cluster. b is
    // split into two groups for null/missing, using the first a value ("x" or "X") seen for that
    // group, as in HashAggStage, rather than from the cluster's first row.
    auto collator =
        std::make_unique<CollatorInterfaceMock>(CollatorInterfaceMock::MockType::kToLowerString);
    runGroupTest(BSON_ARRAY(BSON("a" << "x" << "v" << 1)                          // x, missing
                            << BSON("a" << "X" << "b" << BSONNULL << "v" << 2)    // X, null
                            << BSON("a" << "X" << "v" << 4)                       // X, missing
                            << BSON("a" << "x" << "b" << BSONNULL << "v" << 8)),  // x, null
                 {"a", "b"},
                 {false, true},
                 EFn::kSum,
                 BSON_ARRAY(BSON_ARRAY("x" << "missing" << 5) << BSON_ARRAY("X" << BSONNULL << 10)),
                 collator.release());
}

TEST_F(StreamingAggStageTest, MultipleAccumulatorsStayIsolatedAcrossGroupSwitches) {
    auto plan = makeGroupPlanAccs(
        BSON_ARRAY(BSON("a" << 1 << "v" << 1)                          /* 1, missing */
                   << BSON("a" << 1 << "b" << BSONNULL << "v" << 2)    /* 1, null */
                   << BSON("a" << 1 << "v" << 4)                       /* 1, missing */
                   << BSON("a" << 1 << "b" << BSONNULL << "v" << 8)    /* 1, null */
                   << BSON("a" << 1 << "v" << 16)                      /* 1, missing */
                   << BSON("a" << 1 << "b" << BSONNULL << "v" << 32)), /* 1, null */
        {"a", "b"},
        {false, true},
        [&](value::SlotId valueSlot, boost::optional<value::SlotId>) {
            return makeHashAggAccumulatorList(
                std::make_unique<CompiledHashAggAccumulator>(
                    generateSlotId(),
                    generateSlotId(),
                    makeFunction(EFn::kSum, makeVariable(valueSlot)),
                    makeConstant(value::TypeTags::Nothing, 0)),
                std::make_unique<CompiledHashAggAccumulator>(
                    generateSlotId(),
                    generateSlotId(),
                    makeFunction(EFn::kAddToArray, makeVariable(valueSlot)),
                    makeConstant(value::TypeTags::Nothing, 0)));
        });
    assertSameRows(plan.root.get(),
                   plan.accessors,
                   BSON_ARRAY(BSON_ARRAY(1 << "missing" << 21 << BSON_ARRAY(1 << 4 << 16))
                              << BSON_ARRAY(1 << BSONNULL << 42 << BSON_ARRAY(2 << 8 << 32))));
}

TEST_F(StreamingAggStageTest, FinalizesAccumulatorStatePerEmittedGroup) {
    // $avg's row state is an unfinalized [sum, count] array; the stage must call finalize() on
    // each emitted group or the raw state leaks into the output.
    auto plan = makeGroupPlanAccs(
        BSON_ARRAY(BSON("a" << 1 << "v" << 2)                          // 1, missing
                   << BSON("a" << 1 << "b" << BSONNULL << "v" << 4)    // 1, null
                   << BSON("a" << 1 << "v" << 6)                       // 1, missing
                   << BSON("a" << 1 << "b" << BSONNULL << "v" << 8)),  // 1, null
        {"a", "b"},
        {false, true},
        [&](value::SlotId valueSlot, boost::optional<value::SlotId>) {
            return makeHashAggAccumulatorList(
                std::make_unique<ArithmeticAverageHashAggAccumulatorTerminal>(
                    generateSlotId(), generateSlotId(), makeVariable(valueSlot), boost::none));
        });
    assertSameRows(
        plan.root.get(),
        plan.accessors,
        BSON_ARRAY(BSON_ARRAY(1 << "missing" << 4.0) << BSON_ARRAY(1 << BSONNULL << 6.0)));
}

TEST_F(StreamingAggStageTest, OwningAccumulatorSurvivesMultipleClustersAndGroups) {
    // $addToArray's per-group state is an owned array, and the 8-char elements are heap-owned (past
    // the small-string inline limit). Across several clusters with two groups each, emitNext moves
    // earlier clusters' owned state out of the emitted rows while later clusters reinitialize the
    // recycled rows.
    runGroupTest(BSON_ARRAY(BSON("a" << 1 << "v" << "aaaaaaaa")
                            << BSON("a" << 1 << "b" << BSONNULL << "v" << "bbbbbbbb")
                            << BSON("a" << 1 << "v" << "cccccccc")
                            << BSON("a" << 2 << "b" << BSONNULL << "v" << "dddddddd")
                            << BSON("a" << 2 << "v" << "eeeeeeee")
                            << BSON("a" << 3 << "v" << "ffffffff")
                            << BSON("a" << 3 << "b" << BSONNULL << "v" << "gggggggg")),

                 {"a", "b"},
                 {false, true},
                 EFn::kAddToArray,
                 BSON_ARRAY(BSON_ARRAY(1 << "missing" << BSON_ARRAY("aaaaaaaa" << "cccccccc"))
                            << BSON_ARRAY(1 << BSONNULL << BSON_ARRAY("bbbbbbbb"))
                            << BSON_ARRAY(2 << BSONNULL << BSON_ARRAY("dddddddd"))
                            << BSON_ARRAY(2 << "missing" << BSON_ARRAY("eeeeeeee"))
                            << BSON_ARRAY(3 << "missing" << BSON_ARRAY("ffffffff"))
                            << BSON_ARRAY(3 << BSONNULL << BSON_ARRAY("gggggggg"))));
}

TEST_F(StreamingAggStageTest, GrowingClustersReuseAndExtendRows) {
    // Clusters grow from 1 group to 2 to 4 (with one, then two, separate cluster key slots).
    // Cluster a=2 must reuse the row recycled from cluster a=1 and extend the vectors by one;
    // cluster a=3 reuses two rows and extends by two more. Long string cluster keys make the key
    // rows heap-owning, so a recycled row's old buffer must have been released at emission time
    // (a retained buffer would leak or trip the allocator's consistency checks).
    runGroupTest(BSON_ARRAY(BSON("a" << "cluster-key-0001" << "v" << 1)
                            // 2-group cluster: b interleaves missing/null.
                            << BSON("a" << "cluster-key-0002" << "v" << 2)
                            << BSON("a" << "cluster-key-0002" << "b" << BSONNULL << "v" << 4)
                            << BSON("a" << "cluster-key-0002" << "v" << 8)
                            // 4-group cluster: b and c each interleave missing/null.
                            << BSON("a" << "cluster-key-0003" << "v" << 16)
                            << BSON("a" << "cluster-key-0003" << "b" << BSONNULL << "v" << 32)
                            << BSON("a" << "cluster-key-0003" << "c" << BSONNULL << "v" << 64)
                            << BSON("a" << "cluster-key-0003" << "b" << BSONNULL << "c" << BSONNULL
                                        << "v" << 128)),
                 {"a", "b", "c"},
                 {false, true, true},
                 EFn::kSum,
                 BSON_ARRAY(BSON_ARRAY("cluster-key-0001" << "missing" << "missing" << 1)
                            << BSON_ARRAY("cluster-key-0002" << "missing" << "missing" << 10)
                            << BSON_ARRAY("cluster-key-0002" << BSONNULL << "missing" << 4)
                            << BSON_ARRAY("cluster-key-0003" << "missing" << "missing" << 16)
                            << BSON_ARRAY("cluster-key-0003" << BSONNULL << "missing" << 32)
                            << BSON_ARRAY("cluster-key-0003" << "missing" << BSONNULL << 64)
                            << BSON_ARRAY("cluster-key-0003" << BSONNULL << BSONNULL << 128)));
}

TEST_F(StreamingAggStageTest, SeparateClusterKeySlotsWithNoAccumulators) {
    // Splitting a cluster into per-group output rows works with no accumulators at all.
    auto plan = makeGroupPlanAccs(BSON_ARRAY(BSON("a" << 1)
                                             << BSON("a" << 1 << "b" << BSONNULL) << BSON("a" << 1)
                                             << BSON("a" << 2 << "b" << BSONNULL)),
                                  {"a", "b"},
                                  {false, true},
                                  [](value::SlotId, boost::optional<value::SlotId>) {
                                      return std::vector<std::unique_ptr<HashAggAccumulator>>{};
                                  });
    assertSameRows(plan.root.get(),
                   plan.accessors,
                   BSON_ARRAY(BSON_ARRAY(1 << "missing")
                              << BSON_ARRAY(1 << BSONNULL) << BSON_ARRAY(2 << BSONNULL)));
}

TEST_F(StreamingAggStageTest, RecycledStateRowIsFullyReinitialized) {
    // The null group of cluster a=1 dies mid-cluster, so its row is recycled at emission and reused
    // for cluster a=2's group. The stage must reset a recycled row to the empty state, because
    // $push's initialize is not guaranteed to clear leftover state.
    runGroupTest(BSON_ARRAY(BSON("a" << 1 << "b" << BSONNULL << "v" << 1)
                            << BSON("a" << 1 << "b" << BSONNULL << "v" << 2)
                            << BSON("a" << 1 << "v" << 4)    // null group ends, missing group ends
                            << BSON("a" << 2 << "v" << 8)),  // cluster a=2 recycles a state row
                 {"a", "b"},
                 {false, true},
                 EFn::kAddToArray,
                 BSON_ARRAY(BSON_ARRAY(1 << BSONNULL << BSON_ARRAY(1 << 2))
                            << BSON_ARRAY(1 << "missing" << BSON_ARRAY(4))
                            << BSON_ARRAY(2 << "missing" << BSON_ARRAY(8))));
}

TEST_F(StreamingAggStageTest, FirstAccumulatorPreservesStateAcrossGroupSwitches) {
    // The $first values are captured at the start of each group; rows after switching away and back
    // must not overwrite them. The first row's missing 'v' makes the missing group's $first null,
    // following MQL semantics.
    auto plan = makeGroupPlanAccs(
        BSON_ARRAY(BSON("a" << 1) << BSON("a" << 1 << "b" << BSONNULL << "v" << "x")
                                  << BSON("a" << 1 << "v" << 5)
                                  << BSON("a" << 1 << "b" << BSONNULL << "v" << 7)),
        {"a", "b"},
        {false, true},
        [&](value::SlotId valueSlot, boost::optional<value::SlotId>) {
            return makeHashAggAccumulatorList(std::make_unique<FirstHashAggAccumulator>(
                generateSlotId(), generateSlotId(), makeVariable(valueSlot), boost::none));
        });
    assertSameRows(
        plan.root.get(),
        plan.accessors,
        BSON_ARRAY(BSON_ARRAY(1 << "missing" << BSONNULL) << BSON_ARRAY(1 << BSONNULL << "x")));
}

TEST_F(StreamingAggStageTest, ReopenAfterPartiallyDrainingMultiGroupCluster) {
    auto plan = makeGroupPlan(BSON_ARRAY(BSON("a" << 1 << "v" << 1)
                                         << BSON("a" << 1 << "b" << BSONNULL << "v" << 2)
                                         << BSON("a" << 1 << "v" << 4) << BSON("a" << 2 << "v" << 8)
                                         << BSON("a" << 2 << "b" << BSONNULL << "v" << 16)),
                              {"a", "b"},
                              {false, true},
                              EFn::kSum);
    ASSERT_EQ(plan.root->getNext(), PlanState::ADVANCED);  // first group of cluster a=1
    plan.root->close();
    plan.root->open(true);
    assertSameRows(plan.root.get(),
                   plan.accessors,
                   BSON_ARRAY(BSON_ARRAY(1 << "missing" << 5)
                              << BSON_ARRAY(1 << BSONNULL << 2) << BSON_ARRAY(2 << "missing" << 8)
                              << BSON_ARRAY(2 << BSONNULL << 16)));
}
}  // namespace mongo::sbe
