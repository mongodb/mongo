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
#include "mongo/idl/server_parameter_test_util.h"

namespace mongo::sbe {

class HashLookupStageTest : public HashLookupSharedTest {
public:
    void runVariation(unittest::GoldenTestContext& gctx,
                      const std::string& name,
                      const BSONArray& outer,
                      const BSONArray& inner,
                      bool outerKeyOnly = true,
                      CollatorInterface* collator = nullptr) override {
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
            ctx->pushCorrelated(collatorSlot.value(), &collatorAccessor);
            collatorAccessor.reset(value::TypeTags::collator,
                                   value::bitcastFrom<CollatorInterface*>(collator));
        }

        // Build and prepare for execution loop join of the two scan stages.
        value::SlotId lookupStageOutputSlot = generateSlotId();
        SlotExprPair agg = std::make_pair(
            lookupStageOutputSlot,
            stage_builder::makeFunction("addToArray", makeE<EVariable>(innerScanSlots[0])));
        auto lookupStage = makeS<HashLookupStage>(std::move(outerScanStage),
                                                  std::move(innerScanStage),
                                                  outerScanSlots[1],
                                                  innerScanSlots[1],
                                                  innerScanSlots[0],
                                                  std::move(agg),
                                                  collatorSlot,
                                                  kEmptyPlanNodeId);

        StageResultsPrinters::SlotNames slotNames;
        if (outerKeyOnly) {
            slotNames.emplace_back(outerScanSlots[1], "outer_key");
        } else {
            slotNames.emplace_back(outerScanSlots[0], "outer");
        }
        slotNames.emplace_back(lookupStageOutputSlot, "inner_agg");

        prepareAndEvalStageWithReopen(ctx.get(), stream, slotNames, lookupStage.get());
    }  // runVariation
};     // class HashLookupStageTest

TEST_F(HashLookupStageTest, BasicTests) {
    unittest::GoldenTestContext gctx(&goldenTestConfigSbe);

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

TEST_F(HashLookupStageTest, DuplicateDocumentKeyCausesSpillTest) {
    constexpr size_t kDupCount = 16;
    const BSONObj kKey = BSON("k" << 1);

    // Set the memory limit so that the initial key fits into memory, but adding more duplicates
    // push memory over the limit.
    RAIIServerParameterControllerForTest maxMemoryLimit(
        "internalQuerySlotBasedExecutionHashLookupApproxMemoryUseInBytesBeforeSpill",
        static_cast<long long>(kKey.objsize() + 8 * kDupCount / 2));

    BSONArrayBuilder innerJoin;
    BSONArrayBuilder lookupOutputBuilder;
    for (size_t i = 0; i < kDupCount; ++i) {
        BSONObj obj = BSON("_id" << static_cast<long long>(i));
        innerJoin.append(BSON_ARRAY(obj << kKey));
        lookupOutputBuilder.append(obj);
    }
    BSONArray lookupOutput = lookupOutputBuilder.arr();
    BSONObj lookupInput = BSON("_id" << 1);

    auto [innerScanSlots, innerScanStage] = generateVirtualScanMulti(2, innerJoin.arr());
    auto [outerScanSlots, outerScanStage] =
        generateVirtualScanMulti(2, BSON_ARRAY(BSON_ARRAY(lookupInput << kKey)));

    auto ctx = makeCompileCtx();

    value::SlotId lookupStageOutputSlot = generateSlotId();
    SlotExprPair agg = std::make_pair(
        lookupStageOutputSlot,
        stage_builder::makeFunction("addToArray", makeE<EVariable>(innerScanSlots[0])));
    auto lookupStage = makeS<HashLookupStage>(std::move(outerScanStage),
                                              std::move(innerScanStage),
                                              outerScanSlots[1],
                                              innerScanSlots[1],
                                              innerScanSlots[0],
                                              std::move(agg),
                                              boost::none,
                                              kEmptyPlanNodeId);

    value::SlotVector lookupSlots;
    lookupSlots.reserve(2);
    lookupSlots.push_back(outerScanSlots[0]);
    lookupSlots.push_back(lookupStageOutputSlot);
    auto resultAccessors = prepareTree(ctx.get(), lookupStage.get(), lookupSlots);

    std::vector<std::vector<std::pair<value::TypeTags, value::Value>>> actualResults;
    std::vector<std::pair<value::TypeTags, value::Value>> flatValues;
    while (lookupStage->getNext() == PlanState::ADVANCED) {
        std::vector<std::pair<value::TypeTags, value::Value>> results{};
        results.reserve(resultAccessors.size());
        for (size_t i = 0; i < resultAccessors.size(); ++i) {
            flatValues.emplace_back(resultAccessors[i]->getCopyOfValue());
            results.emplace_back(flatValues.back());
        }
        actualResults.emplace_back(std::move(results));
    }

    ValueVectorGuard resultsGuard{flatValues};
    lookupStage->close();

    ASSERT_EQ(actualResults.size(), 1);
    ASSERT_EQ(actualResults[0].size(), 2);
    ASSERT_EQ(value::compareValue(actualResults[0][0].first,
                                  actualResults[0][0].second,
                                  value::TypeTags::bsonObject,
                                  value::bitcastFrom<const char*>(lookupInput.objdata())),
              std::make_pair(value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(0)));
    ASSERT_EQ(value::compareValue(actualResults[0][1].first,
                                  actualResults[0][1].second,
                                  value::TypeTags::bsonArray,
                                  value::bitcastFrom<const char*>(lookupOutput.objdata())),
              std::make_pair(value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(0)));
}

TEST_F(HashLookupStageTest, SpillLargeStringWithCollationTest) {
    constexpr size_t kStringLength = 64;
    constexpr size_t kStringCount = 26;

    RAIIServerParameterControllerForTest maxMemoryLimit(
        "internalQuerySlotBasedExecutionHashLookupApproxMemoryUseInBytesBeforeSpill",
        static_cast<long long>(kStringCount * kStringLength / 2));

    std::vector<std::string> innerKeys;
    std::vector<std::string> outerKeys;
    innerKeys.reserve(kStringCount);
    outerKeys.reserve(kStringCount);
    for (size_t i = 0; i < kStringCount; ++i) {
        const std::array<char, 2> chars{static_cast<char>('a' + i), static_cast<char>('A' + i)};
        std::string inner, outer;
        inner.reserve(kStringLength);
        outer.reserve(kStringLength);
        for (size_t j = 0; j < kStringLength; ++j) {
            inner.push_back(chars[j % 2]);
            outer.push_back(chars[1 - j % 2]);
        }
        innerKeys.push_back(std::move(inner));
        outerKeys.push_back(std::move(outer));
    }

    BSONArrayBuilder innerJoin;
    BSONArrayBuilder outerJoin;
    for (size_t i = 0; i < kStringCount; ++i) {
        BSONObj obj = BSON("_id" << static_cast<long long>(i));
        innerJoin.append(BSON_ARRAY(obj << innerKeys[i]));
        outerJoin.append(BSON_ARRAY(obj << outerKeys[i]));
    }

    auto [innerScanSlots, innerScanStage] = generateVirtualScanMulti(2, innerJoin.arr());
    auto [outerScanSlots, outerScanStage] = generateVirtualScanMulti(2, outerJoin.arr());

    auto ctx = makeCompileCtx();

    auto collatorSlot = generateSlotId();
    auto collator =
        std::make_unique<CollatorInterfaceMock>(CollatorInterfaceMock::MockType::kToLowerString);
    value::OwnedValueAccessor collatorAccessor;
    ctx->pushCorrelated(collatorSlot, &collatorAccessor);
    collatorAccessor.reset(value::TypeTags::collator,
                           value::bitcastFrom<CollatorInterface*>(collator.release()));

    value::SlotId lookupStageOutputSlot = generateSlotId();
    SlotExprPair agg = std::make_pair(
        lookupStageOutputSlot,
        stage_builder::makeFunction("addToArray", makeE<EVariable>(innerScanSlots[0])));
    auto lookupStage = makeS<HashLookupStage>(std::move(outerScanStage),
                                              std::move(innerScanStage),
                                              outerScanSlots[1],
                                              innerScanSlots[1],
                                              innerScanSlots[0],
                                              std::move(agg),
                                              collatorSlot,
                                              kEmptyPlanNodeId);

    value::SlotVector lookupSlots;
    lookupSlots.reserve(2);
    lookupSlots.push_back(outerScanSlots[0]);
    lookupSlots.push_back(lookupStageOutputSlot);
    auto resultAccessors = prepareTree(ctx.get(), lookupStage.get(), lookupSlots);

    std::vector<std::vector<std::pair<value::TypeTags, value::Value>>> actualResultView;
    std::vector<std::pair<value::TypeTags, value::Value>> ownedValues;
    while (lookupStage->getNext() == PlanState::ADVANCED) {
        std::vector<std::pair<value::TypeTags, value::Value>> results{};
        results.reserve(resultAccessors.size());
        for (size_t i = 0; i < resultAccessors.size(); ++i) {
            ownedValues.emplace_back(resultAccessors[i]->getCopyOfValue());
            results.emplace_back(ownedValues.back());
        }
        actualResultView.emplace_back(std::move(results));
    }

    ValueVectorGuard resultsGuard{ownedValues};
    lookupStage->close();

    ASSERT_EQ(actualResultView.size(), kStringCount);
    for (size_t i = 0; i < kStringCount; ++i) {
        const auto& result = actualResultView[i];
        ASSERT_EQ(result.size(), 2);
        const BSONObj input = BSON("_id" << static_cast<long long>(i));
        assertValuesEqual(result[0].first,
                          result[0].second,
                          value::TypeTags::bsonObject,
                          value::bitcastFrom<const char*>(input.objdata()));
        const BSONArray output = BSON_ARRAY(input);
        assertValuesEqual(result[1].first,
                          result[1].second,
                          value::TypeTags::bsonArray,
                          value::bitcastFrom<const char*>(output.objdata()));
    }
}

}  // namespace mongo::sbe
