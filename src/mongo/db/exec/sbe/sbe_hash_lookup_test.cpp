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
 * This file contains tests for sbe::LoopJoinStage.
 */

#include "mongo/bson/json.h"
#include "mongo/db/exec/sbe/expressions/expression.h"
#include "mongo/db/exec/sbe/sbe_plan_stage_test.h"
#include "mongo/db/exec/sbe/stages/hash_lookup.h"
#include "mongo/db/exec/sbe/util/stage_results_printer.h"
#include "mongo/db/exec/sbe/values/value_printer.h"
#include "mongo/db/query/collation/collator_interface_mock.h"
#include "mongo/unittest/golden_test.h"
#include "mongo/util/assert_util.h"
namespace mongo::sbe {

unittest::GoldenTestConfig goldenTestConfig{"src/mongo/db/test_output/exec/sbe"};

class HashLookupStageTest : public PlanStageTestFixture {
public:
    void runVariation(unittest::GoldenTestContext& gctx,
                      const std::string& name,
                      const BSONArray& outer,
                      const BSONArray& inner,
                      bool outerKeyOnly = true,
                      CollatorInterface* collator = nullptr) {
        auto& stream = gctx.outStream();
        if (stream.tellp()) {
            stream << std::endl;
        }

        stream << "==== VARIATION: " << name << " ====" << std::endl;
        if (collator) {
            stream << "COLLATOR: ";
            value::ValuePrinters::make(stream, printOptions).writeCollatorToStream(collator);
            stream << std::endl;
        }
        stream << "-- INPUTS:" << std::endl;

        // Build a scan for the outer loop.
        auto [outerScanSlots, outerScanStage] = generateVirtualScanMulti(2, outer);
        stream << "OUTER ";
        cloneAndEvalStage(stream,
                          {{outerScanSlots[0], "value"}, {outerScanSlots[1], "key"}},
                          outerScanStage.get());
        stream << std::endl;

        // Build a scan for the inner loop.
        auto [innerScanSlots, innerScanStage] = generateVirtualScanMulti(2, inner);
        stream << "INNER ";
        cloneAndEvalStage(stream,
                          {{innerScanSlots[0], "value"}, {innerScanSlots[1], "key"}},
                          innerScanStage.get());
        stream << std::endl;

        // Prepare and eval stage
        auto ctx = makeCompileCtx();

        value::ViewOfValueAccessor collatorAccessor;
        boost::optional<value::SlotId> collatorSlot;

        if (collator) {
            // Setup collator and insert it into the ctx.
            collatorSlot = generateSlotId();
            ctx->pushCorrelated(collatorSlot.get(), &collatorAccessor);
            collatorAccessor.reset(value::TypeTags::collator,
                                   value::bitcastFrom<CollatorInterface*>(collator));
        }

        // Build and prepare for execution loop join of the two scan stages.
        auto lookupAggSlot = generateSlotId();
        auto aggs =
            makeEM(lookupAggSlot,
                   stage_builder::makeFunction("addToArray", makeE<EVariable>(innerScanSlots[0])));
        auto lookupStage = makeS<HashLookupStage>(std::move(outerScanStage),
                                                  std::move(innerScanStage),
                                                  outerScanSlots[1],
                                                  innerScanSlots[1],
                                                  makeSV(innerScanSlots[0]),
                                                  std::move(aggs),
                                                  collatorSlot,
                                                  kEmptyPlanNodeId);

        StageResultsPrinters::SlotNames slotNames;
        if (outerKeyOnly) {
            slotNames.emplace_back(outerScanSlots[1], "outer_key");
        } else {
            slotNames.emplace_back(outerScanSlots[0], "outer");
        }
        slotNames.emplace_back(lookupAggSlot, "inner_agg");

        prepareAndEvalStageWithReopen(ctx.get(), stream, slotNames, lookupStage.get());
    }

    void cloneAndEvalStage(std::ostream& stream,
                           const StageResultsPrinters::SlotNames& slotNames,
                           PlanStage* stage) {
        auto ctx = makeCompileCtx();
        auto clone = stage->clone();
        prepareAndEvalStage(ctx.get(), stream, slotNames, clone.get());
    }

    void prepareAndEvalStage(CompileCtx* ctx,
                             std::ostream& stream,
                             const StageResultsPrinters::SlotNames& slotNames,
                             PlanStage* stage) {
        prepareTree(ctx, stage);
        StageResultsPrinters::make(stream, printOptions).printStageResults(ctx, slotNames, stage);
        stage->close();
    }

    /**
     * Evaluate stage with reopens, but only write output once to the output stream.
     */
    void prepareAndEvalStageWithReopen(CompileCtx* ctx,
                                       std::ostream& stream,
                                       const StageResultsPrinters::SlotNames& slotNames,
                                       PlanStage* stage) {
        prepareTree(ctx, stage);

        // Execute the stage normally.
        std::stringstream firstStream;
        StageResultsPrinters::make(firstStream, printOptions)
            .printStageResults(ctx, slotNames, stage);
        std::string firstStr = firstStream.str();
        stream << "--- First Stats" << std::endl;
        printHashLookupStats(stream, stage);

        // Execute the stage after reopen and verify that output is the same.
        stage->open(true);
        std::stringstream secondStream;
        StageResultsPrinters::make(secondStream, printOptions)
            .printStageResults(ctx, slotNames, stage);
        std::string secondStr = secondStream.str();
        ASSERT_EQ(firstStr, secondStr);
        stream << "--- Second Stats" << std::endl;
        printHashLookupStats(stream, stage);

        // Execute the stage after close and open and verify that output is the same.
        stage->close();
        stage->open(false);
        std::stringstream thirdStream;
        StageResultsPrinters::make(thirdStream, printOptions)
            .printStageResults(ctx, slotNames, stage);
        std::string thirdStr = thirdStream.str();
        ASSERT_EQ(firstStr, thirdStr);
        stream << "--- Third Stats" << std::endl;
        printHashLookupStats(stream, stage);

        stage->close();

        // Execute the stage with spilling to disk.
        auto defaultInternalQuerySBELookupApproxMemoryUseInBytesBeforeSpill =
            internalQuerySBELookupApproxMemoryUseInBytesBeforeSpill.load();
        internalQuerySBELookupApproxMemoryUseInBytesBeforeSpill.store(10);
        ON_BLOCK_EXIT([&] {
            internalQuerySBELookupApproxMemoryUseInBytesBeforeSpill.store(
                defaultInternalQuerySBELookupApproxMemoryUseInBytesBeforeSpill);
        });

        // Run the stage after the knob is set and spill to disk. We need to hold a global IS lock
        // to read from WT.
        Lock::GlobalLock lk(operationContext(), MODE_IS);
        stage->open(true);
        std::stringstream fourthStream;
        StageResultsPrinters::make(fourthStream, printOptions)
            .printStageResults(ctx, slotNames, stage);
        std::string fourthStr = fourthStream.str();
        ASSERT_EQ(firstStr, fourthStr);
        stream << "--- Fourth Stats" << std::endl;
        printHashLookupStats(stream, stage);
        stream << std::endl;

        // Execute the stage after close and open and verify that output is the same.
        stage->close();
        stage->open(false);
        std::stringstream fifthStream;
        StageResultsPrinters::make(fifthStream, printOptions)
            .printStageResults(ctx, slotNames, stage);
        std::string fifthStr = fifthStream.str();
        ASSERT_EQ(firstStr, fifthStr);
        stream << "--- Fifth Stats" << std::endl;
        printHashLookupStats(stream, stage);
        stream << std::endl;

        // Execute the stage after reopen and we have spilled to disk and verify that output is the
        // same.
        stage->open(true);
        std::stringstream sixthStream;
        StageResultsPrinters::make(sixthStream, printOptions)
            .printStageResults(ctx, slotNames, stage);
        std::string sixthStr = sixthStream.str();
        ASSERT_EQ(firstStr, sixthStr);
        stream << "--- Sixth Stats" << std::endl;
        printHashLookupStats(stream, stage);
        stream << std::endl;

        stage->close();

        stream << "-- OUTPUT ";
        stream << firstStr;
    }

public:
    PrintOptions printOptions =
        PrintOptions().arrayObjectOrNestingMaxDepth(SIZE_MAX).useTagForAmbiguousValues(true);

private:
    static void printHashLookupStats(std::ostream& stream, const PlanStage* stage) {
        auto stats = static_cast<const HashLookupStats*>(stage->getSpecificStats());
        printHashLookupStats(stream, stats);
    }
    static void printHashLookupStats(std::ostream& stream, const HashLookupStats* stats) {
        stream << "dsk:" << stats->usedDisk << std::endl;
        stream << "htRecs:" << stats->spilledHtRecords << std::endl;
        stream << "htIndices:" << stats->spilledHtBytesOverAllRecords << std::endl;
        stream << "buffRecs:" << stats->spilledBuffRecords << std::endl;
        stream << "buffBytes:" << stats->spilledBuffBytesOverAllRecords << std::endl;
    }
};

TEST_F(HashLookupStageTest, BasicTests) {
    unittest::GoldenTestContext gctx(&goldenTestConfig);

    runVariation(gctx,
                 "simple scalar key",
                 BSONArray(fromjson(R"""([
                    [{_id: 1}, 1],
                    [{_id: 2}, 2],
                    [{_id: 3}, 2],
                    [{_id: 4}, 7]
                 ])""")),
                 BSONArray(fromjson(R"""([
                    [{_id: 11}, 1],
                    [{_id: 12}, 2],
                    [{_id: 13}, 2],
                    [{_id: 14}, 7]
                 ])""")));

    runVariation(gctx,
                 "no matches",
                 BSONArray(fromjson(R"""([
                  [{_id: 1}, 1],
                  [{_id: 2}, 4]
               ])""")),
                 BSONArray(fromjson(R"""([
                  [{_id: 11}, 2],
                  [{_id: 12}, 3]
               ])""")));

    runVariation(gctx,
                 "simple array key",
                 BSONArray(fromjson(R"""([
                  [{_id: 1}, 1],
                  [{_id: 2}, [2, 3]],
                  [{_id: 3}, [2, 4]],
                  [{_id: 4}, []]
               ])""")),
                 BSONArray(fromjson(R"""([
                  [{_id: 11}, 2],
                  [{_id: 12}, 4],
                  [{_id: 13}, [1, 2, 3]],
                  [{_id: 14}, []]
               ])""")));

    runVariation(gctx,
                 "nested array key",
                 BSONArray(fromjson(R"""([
                [{_id: 1}, 1],
                [{_id: 2}, 2],
                [{_id: 3}, 3],
                [{_id: 4}, [1]],
                [{_id: 5}, [3]],
                [{_id: 6}, [[3]]],
                [{_id: 7}, [2, [3]]]
             ])""")),
                 BSONArray(fromjson(R"""([
                [{_id: 11}, 1],
                [{_id: 12}, 2],
                [{_id: 13}, 3],
                [{_id: 14}, [1]],
                [{_id: 15}, [3]],
                [{_id: 16}, [[3]]],
                [{_id: 17}, [2, [3]]]
             ])""")));

    runVariation(gctx,
                 "nested object key",
                 BSONArray(fromjson(R"""([
                [{_id: 1}, {a: 1}],
                [{_id: 2}, {b: 1}],
                [{_id: 3}, {a: 1, b: 1}],
                [{_id: 4}, {b: 1, a: 1}],
                [{_id: 5}, [{a: 1}]],
                [{_id: 6}, [{a: 1}, {b: 1}]],
                [{_id: 7}, [{a: 1, b: 1}]],
                [{_id: 8}, [{b: 1, a: 1}]]
             ])""")),
                 BSONArray(fromjson(R"""([
                [{_id: 11}, {a: 1}],
                [{_id: 12}, {b: 1}],
                [{_id: 13}, {a: 1, b: 1}],
                [{_id: 14}, {b: 1, a: 1}],
                [{_id: 15}, [{a: 1}]],
                [{_id: 16}, [{a: 1}, {b: 1}]],
                [{_id: 17}, [{a: 1, b: 1}]],
                [{_id: 18}, [{b: 1, a: 1}]]
             ])""")));

    runVariation(gctx,
                 "mixed key",
                 BSONArray(fromjson(R"""([
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
                 BSONArray(fromjson(R"""([
              [{_id: 11}, null],
              [{_id: 12}, 1],
              [{_id: 13}, "abc"],
              [{_id: 14}, NumberDecimal("1")],
              [{_id: 15}, [1]],
              [{_id: 16}, ["abc"]],
              [{_id: 17}, [null]],
              [{_id: 18}, [null, "1", "abc", NumberDecimal("1")]],
              [{_id: 19}, [{x:1, y: "abc"}]]
           ])""")));

    auto toLowerCollator =
        std::make_unique<CollatorInterfaceMock>(CollatorInterfaceMock::MockType::kToLowerString);
    runVariation(gctx,
                 "with toLower collator",
                 BSONArray(fromjson(R"""([
              [{_id: 1}, null],
              [{_id: 2}, "abc"],
              [{_id: 3}, "ABC"],
              [{_id: 4}, "Abc"],
              [{_id: 5}, "def"]
           ])""")),
                 BSONArray(fromjson(R"""([
              [{_id: 11}, null],
              [{_id: 12}, "abc"],
              [{_id: 13}, "ABC"],
              [{_id: 14}, "Abc"],
              [{_id: 15}, "def"]
           ])""")),
                 true,
                 toLowerCollator.get());

    runVariation(gctx,
                 "empty",
                 BSONArray(fromjson(R"""([
           ])""")),
                 BSONArray(fromjson(R"""([
           ])""")));

    runVariation(gctx,
                 "empty outer",
                 BSONArray(fromjson(R"""([
           ])""")),
                 BSONArray(fromjson(R"""([
                [{_id: 11}, 2],
                [{_id: 12}, 3]
           ])""")));

    runVariation(gctx,
                 "empty inner",
                 BSONArray(fromjson(R"""([
              [{_id: 1}, 1],
              [{_id: 2}, 2]
         ])""")),
                 BSONArray(fromjson(R"""([
         ])""")));
}
}  // namespace mongo::sbe
