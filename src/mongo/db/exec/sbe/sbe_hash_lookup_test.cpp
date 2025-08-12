/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

/**
 * This file contains tests for sbe::HashLookupStage.
 */

#include "mongo/db/exec/sbe/sbe_hash_lookup_shared_test.h"
#include "mongo/db/exec/sbe/stages/hash_lookup.h"

namespace mongo::sbe {

class HashLookupStageTest : public HashLookupSharedTest {
public:
    void runVariation(const RunVariationParams& params) override {
        auto& stream = params.gctx.outStream();
        if (stream.tellp()) {
            stream << std::endl;
        }

        stream << "==== VARIATION: " << params.name << " ====" << std::endl;
        if (params.collator) {
            stream << "COLLATOR: ";
            value::ValuePrinters::make(stream, printOptions).writeCollatorToStream(params.collator);
            stream << std::endl;
        }
        stream << "-- INPUTS:" << std::endl;

        // Build a scan for the outer loop.
        auto [outerScanSlots, outerScanStage] = generateVirtualScanMulti(2, params.outer);
        stream << "OUTER ";
        cloneAndEvalStage(stream,
                          {{outerScanSlots[0], "value"}, {outerScanSlots[1], "key"}},
                          outerScanStage.get());
        stream << std::endl;

        // Build a scan for the inner loop.
        auto [innerScanSlots, innerScanStage] = generateVirtualScanMulti(2, params.inner);
        stream << "INNER ";
        cloneAndEvalStage(stream,
                          {{innerScanSlots[0], "value"}, {innerScanSlots[1], "key"}},
                          innerScanStage.get());
        stream << std::endl;

        // Prepare and eval stage.
        auto ctx = makeCompileCtx();

        value::ViewOfValueAccessor collatorAccessor;
        boost::optional<value::SlotId> collatorSlot;

        if (params.collator) {
            // Setup collator and insert it into the ctx.
            collatorSlot = generateSlotId();
            ctx->pushCorrelated(collatorSlot.value(), &collatorAccessor);
            collatorAccessor.reset(value::TypeTags::collator,
                                   value::bitcastFrom<CollatorInterface*>(params.collator));
        }

        // Build and prepare for execution loop join of the two scan stages.
        value::SlotId lookupStageOutputSlot = generateSlotId();
        SlotExprPair agg = std::make_pair(
            lookupStageOutputSlot, makeFunction("addToArray", makeE<EVariable>(innerScanSlots[0])));
        auto lookupStage = makeS<HashLookupStage>(std::move(outerScanStage),
                                                  std::move(innerScanStage),
                                                  outerScanSlots[1],
                                                  innerScanSlots[1],
                                                  innerScanSlots[0],
                                                  std::move(agg),
                                                  collatorSlot,
                                                  kEmptyPlanNodeId);

        StageResultsPrinters::SlotNames slotNames;
        if (params.outerKeyOnly) {
            slotNames.emplace_back(outerScanSlots[1], "outer_key");
        } else {
            slotNames.emplace_back(outerScanSlots[0], "outer");
        }
        slotNames.emplace_back(lookupStageOutputSlot, "inner_agg");

        prepareAndEvalStageWithReopen(
            ctx.get(), stream, slotNames, lookupStage.get(), params.expectMemUse);
    }  // runVariation
};  // class HashLookupStageTest

TEST_F(HashLookupStageTest, BasicTests) {
    unittest::GoldenTestContext gctx(&goldenTestConfigSbe);

    runVariation({.gctx = gctx,
                  .name = "simple scalar key",
                  .outer = BSONArray(fromjson(R"""([
                    [{_id: 1}, 1],
                    [{_id: 2}, 2],
                    [{_id: 3}, 2],
                    [{_id: 4}, 7]
                 ])""")),
                  .inner = BSONArray(fromjson(R"""([
                    [{_id: 11}, 1],
                    [{_id: 12}, 2],
                    [{_id: 13}, 2],
                    [{_id: 14}, 7]
                 ])"""))});

    runVariation({.gctx = gctx,
                  .name = "no matches",
                  .outer = BSONArray(fromjson(R"""([
                  [{_id: 1}, 1],
                  [{_id: 2}, 4]
               ])""")),
                  .inner = BSONArray(fromjson(R"""([
                  [{_id: 11}, 2],
                  [{_id: 12}, 3]
               ])"""))});

    runVariation({.gctx = gctx,
                  .name = "simple array key",
                  .outer = BSONArray(fromjson(R"""([
                  [{_id: 1}, 1],
                  [{_id: 2}, [2, 3]],
                  [{_id: 3}, [2, 4]],
                  [{_id: 4}, []]
               ])""")),
                  .inner = BSONArray(fromjson(R"""([
                  [{_id: 11}, 2],
                  [{_id: 12}, 4],
                  [{_id: 13}, [1, 2, 3]],
                  [{_id: 14}, []]
               ])"""))});

    runVariation({.gctx = gctx,
                  .name = "nested array key",
                  .outer = BSONArray(fromjson(R"""([
                [{_id: 1}, 1],
                [{_id: 2}, 2],
                [{_id: 3}, 3],
                [{_id: 4}, [1]],
                [{_id: 5}, [3]],
                [{_id: 6}, [[3]]],
                [{_id: 7}, [2, [3]]]
             ])""")),
                  .inner = BSONArray(fromjson(R"""([
                [{_id: 11}, 1],
                [{_id: 12}, 2],
                [{_id: 13}, 3],
                [{_id: 14}, [1]],
                [{_id: 15}, [3]],
                [{_id: 16}, [[3]]],
                [{_id: 17}, [2, [3]]]
             ])"""))});

    runVariation({.gctx = gctx,
                  .name = "nested object key",
                  .outer = BSONArray(fromjson(R"""([
                [{_id: 1}, {a: 1}],
                [{_id: 2}, {b: 1}],
                [{_id: 3}, {a: 1, b: 1}],
                [{_id: 4}, {b: 1, a: 1}],
                [{_id: 5}, [{a: 1}]],
                [{_id: 6}, [{a: 1}, {b: 1}]],
                [{_id: 7}, [{a: 1, b: 1}]],
                [{_id: 8}, [{b: 1, a: 1}]]
             ])""")),
                  .inner = BSONArray(fromjson(R"""([
                [{_id: 11}, {a: 1}],
                [{_id: 12}, {b: 1}],
                [{_id: 13}, {a: 1, b: 1}],
                [{_id: 14}, {b: 1, a: 1}],
                [{_id: 15}, [{a: 1}]],
                [{_id: 16}, [{a: 1}, {b: 1}]],
                [{_id: 17}, [{a: 1, b: 1}]],
                [{_id: 18}, [{b: 1, a: 1}]]
             ])"""))});


    runVariation({.gctx = gctx,
                  .name = "mixed key",
                  .outer = BSONArray(fromjson(R"""([
              [{_id: 1}, null],
              [{_id: 2}, 1],
              [{_id: 3}, "abc"],
              [{_id: 4}, NumberDecimal("1")],
              [{_id: 5}, [1]],
              [{_id: 6}, ["abc"]],
              [{_id: 7}, [null]],
              [{_id: 8}, [null, "1", "abc", NumberDecimal("1")]],
              [{_id: 9}, [{x:1, y: "abc"}]]
           ])""")),
                  .inner = BSONArray(fromjson(R"""([
              [{_id: 11}, null],
              [{_id: 12}, 1],
              [{_id: 13}, "abc"],
              [{_id: 14}, NumberDecimal("1")],
              [{_id: 15}, [1]],
              [{_id: 16}, ["abc"]],
              [{_id: 17}, [null]],
              [{_id: 18}, [null, "1", "abc", NumberDecimal("1")]],
              [{_id: 19}, [{x:1, y: "abc"}]]
           ])"""))});

    auto toLowerCollator =
        std::make_unique<CollatorInterfaceMock>(CollatorInterfaceMock::MockType::kToLowerString);

    runVariation({.gctx = gctx,
                  .name = "with toLower collator",
                  .outer = BSONArray(fromjson(R"""([
              [{_id: 1}, null],
              [{_id: 2}, "abc"],
              [{_id: 3}, "ABC"],
              [{_id: 4}, "Abc"],
              [{_id: 5}, "def"]
           ])""")),
                  .inner = BSONArray(fromjson(R"""([
              [{_id: 11}, null],
              [{_id: 12}, "abc"],
              [{_id: 13}, "ABC"],
              [{_id: 14}, "Abc"],
              [{_id: 15}, "def"]
           ])""")),
                  .collator = toLowerCollator.get()});

    runVariation({.gctx = gctx,
                  .name = "empty",
                  .outer = BSONArray(fromjson(R"""([
           ])""")),
                  .inner = BSONArray(fromjson(R"""([
           ])""")),
                  .expectMemUse = false /*expectMemUse*/});

    runVariation({.gctx = gctx,
                  .name = "empty outer",
                  .outer = BSONArray(fromjson(R"""([
           ])""")),
                  .inner = BSONArray(fromjson(R"""([
                [{_id: 11}, 2],
                [{_id: 12}, 3]
           ])"""))});

    // We don't expect a memory footprint to be recorded because the hash table never needs to be
    // constructed-- there are no "inner" side documents.
    runVariation({.gctx = gctx,
                  .name = "empty inner",
                  .outer = BSONArray(fromjson(R"""([
              [{_id: 1}, 1],
              [{_id: 2}, 2]
         ])""")),
                  .inner = BSONArray(fromjson(R"""([
         ])""")),
                  .expectMemUse = false});
}

TEST_F(HashLookupStageTest, ForceSpillTest) {

    const BSONArray outer{fromjson(R"""([
      [{_id: 1}, 1],
      [{_id: 2}, 2],
      [{_id: 3}, 2],
      [{_id: 4}, 7]
   ])""")};
    const BSONArray inner{fromjson(R"""([
      [{_id: 11}, 1],
      [{_id: 12}, 2],
      [{_id: 13}, 7],
      [{_id: 14}, 2]
   ])""")};

    // Build a scan for the outer loop.
    auto [outerScanSlots, outerScanStage] = generateVirtualScanMulti(2, outer);
    // Build a scan for the inner loop.
    auto [innerScanSlots, innerScanStage] = generateVirtualScanMulti(2, inner);

    auto ctx = makeCompileCtx();

    auto collatorSlot = generateSlotId();
    // Setup collator and insert it into the ctx.
    auto collator =
        std::make_unique<CollatorInterfaceMock>(CollatorInterfaceMock::MockType::kAlwaysEqual);
    value::OwnedValueAccessor collatorAccessor;
    ctx->pushCorrelated(collatorSlot, &collatorAccessor);
    collatorAccessor.reset(value::TypeTags::collator,
                           value::bitcastFrom<CollatorInterface*>(collator.release()));

    // Build and prepare for execution loop join of the two scan stages.
    value::SlotId lookupStageOutputSlot = generateSlotId();
    SlotExprPair agg = std::make_pair(
        lookupStageOutputSlot, makeFunction("addToArray", makeE<EVariable>(innerScanSlots[0])));
    auto lookupStage = makeS<HashLookupStage>(std::move(outerScanStage),
                                              std::move(innerScanStage),
                                              outerScanSlots[1],
                                              innerScanSlots[1],
                                              innerScanSlots[0],
                                              std::move(agg),
                                              collatorSlot,
                                              kEmptyPlanNodeId);

    value::SlotVector lookupSlots;
    lookupSlots.push_back(outerScanSlots[0]);
    lookupSlots.push_back(lookupStageOutputSlot);
    auto resultAccessors = prepareTree(ctx.get(), lookupStage.get(), lookupSlots);

    std::vector<std::vector<std::pair<value::TypeTags, value::Value>>> expectedResults;
    std::vector<std::pair<value::TypeTags, value::Value>> flatValues;
    while (lookupStage->getNext() == PlanState::ADVANCED) {
        std::vector<std::pair<value::TypeTags, value::Value>> results{};
        for (size_t i = 0; i < resultAccessors.size(); ++i) {
            flatValues.emplace_back(resultAccessors[i]->getCopyOfValue());
            results.emplace_back(flatValues.back());
        }
        expectedResults.emplace_back(std::move(results));
    }

    // This is used to release the values when the test is done.
    ValueVectorGuard resultsGuard{flatValues};

    // Close the stage and execute again with spilling.
    lookupStage->close();

    lookupStage->open(false);
    int idx = 0;
    while (lookupStage->getNext() == PlanState::ADVANCED) {
        for (size_t i = 0; i < resultAccessors.size(); ++i) {
            const auto [resTag, resValue] = resultAccessors[i]->getViewOfValue();
            const auto [expectedTag, exprectedValue] = expectedResults[idx][i];

            auto [compTag, compVal] =
                value::compareValue(expectedTag, exprectedValue, resTag, resValue);

            ASSERT_EQ(value::TypeTags::NumberInt32, compTag);
            ASSERT_EQ(0, compVal);
        }

        if (idx == 1) {
            // Make sure it has not spilled already.
            auto* stats = static_cast<const HashLookupStats*>(lookupStage->getSpecificStats());
            auto totalSpillingStats = stats->getTotalSpillingStats();
            ASSERT_FALSE(stats->usedDisk);
            ASSERT_EQ(0, totalSpillingStats.getSpills());
            ASSERT_EQ(0, totalSpillingStats.getSpilledRecords());
            ASSERT_GT(lookupStage->getMemoryTracker()->inUseTrackedMemoryBytes(), 0);
            ASSERT_GT(stats->peakTrackedMemBytes, 0);

            // Get ready to yield.
            lookupStage->saveState();

            // Force spill.
            lookupStage->forceSpill(nullptr /*yieldPolicy*/);

            // Check stats to make sure it spilled
            stats = static_cast<const HashLookupStats*>(lookupStage->getSpecificStats());
            totalSpillingStats = stats->getTotalSpillingStats();
            ASSERT_TRUE(stats->usedDisk);
            ASSERT_EQ(7, totalSpillingStats.getSpills());
            ASSERT_EQ(7, totalSpillingStats.getSpilledRecords());
            ASSERT_EQ(0, lookupStage->getMemoryTracker()->inUseTrackedMemoryBytes());
            ASSERT_GT(stats->peakTrackedMemBytes, 0);

            // Get ready to retrieve more records.
            lookupStage->restoreState();
        }
        ++idx;
    }

    lookupStage->close();
}
}  // namespace mongo::sbe
