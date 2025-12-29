/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/bson/json.h"
#include "mongo/db/exec/sbe/sbe_hash_lookup_shared_test.h"
#include "mongo/db/exec/sbe/stages/hash_lookup.h"
#include "mongo/unittest/test_info.h"
#include "mongo/unittest/unittest.h"

#include <benchmark/benchmark.h>

namespace mongo::sbe {

class HashLookupDummyTest : public HashLookupSharedTest {
public:
    void runVariation(const RunVariationParams& params) override {
        return;
    }
    void TestBody() override {
        return;
    }
};

class DummyBenchmarkTestFixture : public ::testing::Test {
public:
    void TestBody() override {}
};

// Register a dummy test for GoldenTestContext initialization. This test is never run.
TEST_F(DummyBenchmarkTestFixture, DummyBenchmarkTest) {}

class HashLookupBenchmarkFixture : public benchmark::Fixture {
public:
    void runVariation(benchmark::State& state,
                      const HashLookupSharedTest::RunVariationParams& params) {
        HashLookupDummyTest sharedTest;
        sharedTest.setUp();

        auto& stream = std::cout;

        // Avoid using 'std::endl' here because it unnecessarily flushes the stream, which can lead
        // to poor performance.
        if (stream.tellp()) {
            stream << '\n';
        }

        stream << "==== VARIATION: " << params.name << " ====\n";
        if (params.collator) {
            stream << "COLLATOR: ";
            value::ValuePrinters::make(stream, sharedTest.printOptions)
                .writeCollatorToStream(params.collator);
            stream << '\n';
        }
        stream << "-- INPUTS:\n";
        for (auto _ : state) {
            state.PauseTiming();
            // Build a scan for the outer loop.
            auto [outerScanSlots, outerScanStage] =
                sharedTest.generateVirtualScanMulti(2, params.outer);
            stream << "OUTER ";
            sharedTest.cloneAndEvalStage(stream,
                                         {{outerScanSlots[0], "value"}, {outerScanSlots[1], "key"}},
                                         outerScanStage.get());
            stream << '\n';

            // Build a scan for the inner loop.
            auto [innerScanSlots, innerScanStage] =
                sharedTest.generateVirtualScanMulti(2, params.inner);
            stream << "INNER ";
            sharedTest.cloneAndEvalStage(stream,
                                         {{innerScanSlots[0], "value"}, {innerScanSlots[1], "key"}},
                                         innerScanStage.get());
            stream << '\n';

            // Prepare and eval stage.
            auto ctx = sharedTest.makeCompileCtx();

            value::ViewOfValueAccessor collatorAccessor;
            boost::optional<value::SlotId> collatorSlot;

            if (params.collator) {
                // Setup collator and insert it into the ctx.
                collatorSlot = sharedTest.generateSlotId();
                ctx->pushCorrelated(collatorSlot.value(), &collatorAccessor);
                collatorAccessor.reset(value::TypeTags::collator,
                                       value::bitcastFrom<CollatorInterface*>(params.collator));
            }

            // Build and prepare for execution loop join of the two scan stages.
            value::SlotId lookupStageOutputSlot = sharedTest.generateSlotId();
            SlotExprPair agg =
                std::make_pair(lookupStageOutputSlot,
                               makeFunction("addToArray", makeE<EVariable>(innerScanSlots[0])));
            auto lookupStage = makeS<HashLookupStage>(std::move(outerScanStage),
                                                      std::move(innerScanStage),
                                                      outerScanSlots[1],
                                                      innerScanSlots[1],
                                                      innerScanSlots[0],
                                                      std::move(agg),
                                                      collatorSlot,
                                                      kEmptyPlanNodeId);

            StageResultsPrinters::SlotNames slotNames;
            slotNames.reserve(2);
            if (params.outerKeyOnly) {
                slotNames.emplace_back(outerScanSlots[1], "outer_key");
            } else {
                slotNames.emplace_back(outerScanSlots[0], "outer");
            }
            slotNames.emplace_back(lookupStageOutputSlot, "inner_agg");

            state.ResumeTiming();

            sharedTest.prepareAndEvalStageWithReopen(ctx.get(),
                                                     stream,
                                                     slotNames,
                                                     lookupStage.get(),
                                                     params.expectMemUse,
                                                     false /* getStats */,
                                                     false /* executeWithSpill */);
        }

        sharedTest.tearDown();
    }  // runVariation
    void TestBody() {
        return;
    }
    unittest::GoldenTestContext getGoldenTestContext() {
        // GoldenTestContext not used in this benchmark, create a dummy GoldenTestContext.
        static const ::testing::TestInfo* dummyTestInfo =
            ::testing::UnitTest::GetInstance()->GetTestSuite(0)->GetTestInfo(0);
        return unittest::GoldenTestContext(
            &goldenTestConfigSbe, dummyTestInfo, false /* validateOnClose */);
    }
};

// Explicit benchmark registrations for HashLookupBenchmarkFixture.
BENCHMARK_F(HashLookupBenchmarkFixture, simpleScalarKey)(benchmark::State& state) {
    unittest::GoldenTestContext gctx = getGoldenTestContext();
    runVariation(state,
                 {.gctx = gctx,
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
}

BENCHMARK_F(HashLookupBenchmarkFixture, noMatches)(benchmark::State& state) {
    unittest::GoldenTestContext gctx = getGoldenTestContext();
    runVariation(state,
                 {.gctx = gctx,
                  .name = "no matches",
                  .outer = BSONArray(fromjson(R"""([
                                        [{_id: 1}, 1],
                                        [{_id: 2}, 4]
                                    ])""")),
                  .inner = BSONArray(fromjson(R"""([
                                        [{_id: 11}, 2],
                                        [{_id: 12}, 3]
                                    ])"""))});
}

BENCHMARK_F(HashLookupBenchmarkFixture, simpleArrayKey)(benchmark::State& state) {
    unittest::GoldenTestContext gctx = getGoldenTestContext();
    runVariation(state,
                 {.gctx = gctx,
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
}

BENCHMARK_F(HashLookupBenchmarkFixture, nestedArrayKey)(benchmark::State& state) {
    unittest::GoldenTestContext gctx = getGoldenTestContext();
    runVariation(state,
                 {.gctx = gctx,
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
}

BENCHMARK_F(HashLookupBenchmarkFixture, nestedObjectKey)(benchmark::State& state) {
    unittest::GoldenTestContext gctx = getGoldenTestContext();
    runVariation(state,
                 {.gctx = gctx,
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
}

BENCHMARK_F(HashLookupBenchmarkFixture, mixedKey)(benchmark::State& state) {
    unittest::GoldenTestContext gctx = getGoldenTestContext();
    runVariation(state,
                 {.gctx = gctx,
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
}

BENCHMARK_F(HashLookupBenchmarkFixture, withToLowerCollator)(benchmark::State& state) {
    auto toLowerCollator =
        std::make_unique<CollatorInterfaceMock>(CollatorInterfaceMock::MockType::kToLowerString);
    unittest::GoldenTestContext gctx = getGoldenTestContext();
    runVariation(state,
                 {.gctx = gctx,
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
}

BENCHMARK_F(HashLookupBenchmarkFixture, empty)(benchmark::State& state) {
    unittest::GoldenTestContext gctx = getGoldenTestContext();
    runVariation(state,
                 {.gctx = gctx,
                  .name = "empty",
                  .outer = BSONArray(fromjson(R"""([
                                    ])""")),
                  .inner = BSONArray(fromjson(R"""([
                                            ])""")),
                  .expectMemUse = false});
}

BENCHMARK_F(HashLookupBenchmarkFixture, emptyOuter)(benchmark::State& state) {
    unittest::GoldenTestContext gctx = getGoldenTestContext();
    runVariation(state,
                 {.gctx = gctx,
                  .name = "empty outer",
                  .outer = BSONArray(fromjson(R"""([
                                    ])""")),
                  .inner = BSONArray(fromjson(R"""([
                                        [{_id: 11}, 2],
                                        [{_id: 12}, 3]
                                    ])"""))});
}

BENCHMARK_F(HashLookupBenchmarkFixture, emptyInner)(benchmark::State& state) {
    unittest::GoldenTestContext gctx = getGoldenTestContext();
    runVariation(state,
                 {.gctx = gctx,
                  .name = "empty inner",
                  .outer = BSONArray(fromjson(R"""([
                                        [{_id: 1}, 1],
                                        [{_id: 2}, 2]
                                            ])""")),
                  .inner = BSONArray(fromjson(R"""([
                                            ])""")),
                  .expectMemUse = false});
}

}  // namespace mongo::sbe
