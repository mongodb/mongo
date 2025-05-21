/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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
#include "mongo/base/string_data.h"
#include "mongo/bson/json.h"
#include "mongo/db/exec/sbe/sbe_unittest.h"
#include "mongo/db/exec/sbe/values/slot.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_set_window_fields.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/pipeline/window_function/window_function_concat_arrays.h"
#include "mongo/db/pipeline/window_function/window_function_set_union.h"
#include "mongo/db/query/collation/collator_interface_mock.h"
#include "mongo/db/query/stage_builder/sbe/gen_helpers.h"
#include "mongo/db/query/stage_builder/sbe/tests/sbe_builder_test_fixture.h"
#include "mongo/unittest/unittest.h"

namespace mongo {


class SBESetWindowFieldsTest : public GoldenSbeStageBuilderTestFixture {
public:
    boost::intrusive_ptr<DocumentSource> createSetWindowFieldsDocumentSource(
        boost::intrusive_ptr<ExpressionContext> expCtx, const BSONObj& windowSpec) {
        ASSERT(expCtx) << "expCtx must not be null";
        BSONObj spec = BSON("$_internalSetWindowFields" << windowSpec);

        auto docSrc =
            DocumentSourceInternalSetWindowFields::createFromBson(spec.firstElement(), expCtx);
        docSrc->optimize();

        return docSrc;
    }

    std::pair<std::unique_ptr<QuerySolution>,
              boost::intrusive_ptr<DocumentSourceInternalSetWindowFields>>
    makeSetWindowFieldsQuerySolution(boost::intrusive_ptr<ExpressionContext> expCtx,
                                     BSONObj spec,
                                     std::vector<BSONArray> inputDocs) {

        auto docSrcA = createSetWindowFieldsDocumentSource(expCtx, spec);
        auto docSrc = dynamic_cast<DocumentSourceInternalSetWindowFields*>(docSrcA.get());
        ASSERT(docSrc != nullptr);

        // Constructs a QuerySolution consisting of a WindowNode on top of a VirtualScanNode.
        auto virtScanNode = std::make_unique<VirtualScanNode>(
            inputDocs, VirtualScanNode::ScanType::kCollScan, false /*hasRecordId*/);

        auto windowNode = std::make_unique<WindowNode>(std::move(virtScanNode),
                                                       docSrc->getPartitionBy(),
                                                       docSrc->getSortBy(),
                                                       docSrc->getOutputFields());

        // Makes a QuerySolution from the root window node.
        return {makeQuerySolution(std::move(windowNode)), docSrc};
    }

    std::pair<sbe::value::TypeTags, sbe::value::Value> getSetWindowFieldsResults(
        BSONObj windowSpec,
        std::vector<BSONArray> inputDocs,
        std::unique_ptr<CollatorInterface> collator = nullptr) {
        // Makes a QuerySolution for SetWindowFieldsNode over VirtualScanNode.
        auto [querySolution, windowNode] =
            makeSetWindowFieldsQuerySolution(make_intrusive<ExpressionContextForTest>(),
                                             std::move(windowSpec),
                                             std::move(inputDocs));

        // Translates the QuerySolution tree to a sbe::PlanStage tree.
        auto [resultSlots, stage, data, _] = buildPlanStage(
            std::move(querySolution), false /*hasRecordId*/, nullptr, std::move(collator));
        ASSERT_EQ(resultSlots.size(), 1);

        // Print the stage explain output and verify.
        _gctx->printTestHeader(GoldenTestContext::HeaderFormat::Text);
        _gctx->outStream() << sbe::DebugPrinter().print(*stage.get());
        _gctx->outStream() << std::endl;
        _gctx->verifyOutput();

        auto resultAccessors = prepareTree(&data.env.ctx, stage.get(), resultSlots[0]);
        return getAllResults(stage.get(), &resultAccessors[0]);
    }

    void runSetWindowFieldsTest(StringData windowSpec,
                                std::vector<BSONArray> inputDocs,
                                const mongo::BSONArray& expectedValue,
                                std::unique_ptr<CollatorInterface> collator = nullptr) {
        auto [resultsTag, resultsVal] =
            getSetWindowFieldsResults(fromjson(windowSpec.data()), inputDocs, std::move(collator));
        sbe::value::ValueGuard resultGuard{resultsTag, resultsVal};

        auto [expectedTag, expectedVal] = stage_builder::makeValue(expectedValue);
        sbe::value::ValueGuard expectedGuard{expectedTag, expectedVal};

        ASSERT_TRUE(
            PlanStageTestFixture::valueEquals(resultsTag, resultsVal, expectedTag, expectedVal))
            << "expected: " << std::make_pair(expectedTag, expectedVal)
            << " but got: " << std::make_pair(resultsTag, resultsVal);
    }

    // This function expects that the output has a field 'result' which contains the elements we
    // need to compare.
    enum class ArrayAccumType { kSetUnion, kConcatArrays };
    void runArrayAccumulatorTest(ArrayAccumType accumType,
                                 StringData windowSpec,
                                 std::vector<BSONArray> inputDocs,
                                 const mongo::BSONArray& expectedResult,
                                 std::unique_ptr<CollatorInterface> collator = nullptr) {
        using namespace mongo::sbe::value;

        // Run the accumulator.
        auto [resultsTag, resultsVal] =
            getSetWindowFieldsResults(fromjson(windowSpec.data()), inputDocs, std::move(collator));
        ValueGuard resultGuard{resultsTag, resultsVal};
        ASSERT_EQ(resultsTag, TypeTags::Array);
        auto resultArr = getArrayView(resultsVal);

        // Get an Array view of the expectedResult.
        auto [expectedTag, expectedVal] = sbe::makeArray(expectedResult);
        ValueGuard expectedGuard{expectedTag, expectedVal};
        ASSERT_EQ(expectedTag, TypeTags::Array);
        auto expectedArr = getArrayView(expectedVal);

        ASSERT_EQ(resultArr->size(), expectedArr->size());

        // Returns whatever is at the 'result' field in the object given by 'nextVal'.
        auto extractResultArray = [accumType](TypeTags nextTag, sbe::value::Value nextVal) {
            TypeTags resultTag;
            sbe::value::Value resultVal;
            ObjectEnumerator objEnum{nextTag, nextVal};

            ASSERT(sbe::value::isObject(nextTag) && !objEnum.atEnd())
                << "Expected a non-empty result object but got: "
                << std::make_pair(nextTag, nextVal);
            while (!objEnum.atEnd()) {
                if (objEnum.getFieldName() == "result"_sd) {
                    auto [arrTag, arrVal] = objEnum.getViewOfValue();
                    ASSERT_EQ(arrTag, TypeTags::bsonArray)
                        << "Expected an array for field 'result' but got: "
                        << std::make_pair(arrTag, arrVal);

                    auto [tmpTag, tmpVal] = copyValue(arrTag, arrVal);
                    ValueGuard tmpGuard{tmpTag, tmpVal};
                    switch (accumType) {
                        case ArrayAccumType::kSetUnion: {
                            std::tie(resultTag, resultVal) = arrayToSet(tmpTag, tmpVal);
                            break;
                        }
                        case ArrayAccumType::kConcatArrays: {
                            // Need to convert the bsonArray to an Array.
                            auto [arrTag, arrVal] = sbe::value::makeNewArray();
                            sbe::value::ValueGuard guard{arrTag, arrVal};
                            auto arrView = sbe::value::getArrayView(arrVal);

                            sbe::value::ArrayEnumerator enumerator{tmpTag, tmpVal};
                            while (!enumerator.atEnd()) {
                                auto [tag, val] = enumerator.getViewOfValue();
                                enumerator.advance();

                                auto [copyTag, copyVal] = sbe::value::copyValue(tag, val);
                                arrView->push_back(copyTag, copyVal);
                            }
                            std::tie(resultTag, resultVal) = std::tie(arrTag, arrVal);
                            guard.reset();
                            break;
                        }
                        default:
                            MONGO_UNREACHABLE;
                    }
                }
                objEnum.advance();
            }
            return std::make_pair(resultTag, resultVal);
        };

        sbe::value::ArrayEnumerator arrEnumExpected{expectedTag, expectedVal};
        sbe::value::ArrayEnumerator arrEnumActual{resultsTag, resultsVal};
        while (!arrEnumExpected.atEnd()) {
            ASSERT_FALSE(arrEnumActual.atEnd());

            auto [nextActualTag, nextActualVal] = arrEnumActual.getViewOfValue();
            auto [actualTag, actualResult] = extractResultArray(nextActualTag, nextActualVal);
            ValueGuard actualValueGuard{actualTag, actualResult};

            auto [nextExpectedTag, nextExpectedVal] = arrEnumExpected.getViewOfValue();
            auto [expectedTag, expectedResult] =
                extractResultArray(nextExpectedTag, nextExpectedVal);
            ValueGuard expectedValueGuard{expectedTag, expectedResult};

            ASSERT(valueEquals(expectedTag, expectedResult, actualTag, actualResult))
                << "expected result: " << std::make_pair(expectedTag, expectedResult)
                << " but got result: " << std::make_pair(actualTag, actualResult);

            arrEnumExpected.advance();
            arrEnumActual.advance();
        }
    }
};

TEST_F(SBESetWindowFieldsTest, FirstTestPositiveWindow) {
    auto docs = std::vector<BSONArray>{BSON_ARRAY(BSON("a" << 1 << "b" << 1)),
                                       BSON_ARRAY(BSON("a" << 2 << "b" << 3)),
                                       BSON_ARRAY(BSON("a" << 3 << "b" << 5)),
                                       BSON_ARRAY(BSON("a" << 4 << "b" << 7))};
    runSetWindowFieldsTest(
        R"({sortBy: {a: 1}, output: {first: {$first: '$b', window: {documents: [1, 2]} }}})",
        docs,
        BSON_ARRAY(BSON("a" << 1 << "b" << 1 << "first" << 3)
                   << BSON("a" << 2 << "b" << 3 << "first" << 5)
                   << BSON("a" << 3 << "b" << 5 << "first" << 7)
                   << BSON("a" << 4 << "b" << 7 << "first" << BSONNULL)));
}

TEST_F(SBESetWindowFieldsTest, FirstTestConstantValuePositiveWindow) {
    auto docs = std::vector<BSONArray>{BSON_ARRAY(BSON("a" << 1 << "b" << 1)),
                                       BSON_ARRAY(BSON("a" << 2 << "b" << 3)),
                                       BSON_ARRAY(BSON("a" << 3 << "b" << 5)),
                                       BSON_ARRAY(BSON("a" << 4 << "b" << 7))};
    runSetWindowFieldsTest(
        R"({sortBy: {a: 1}, output: {first: {$first: 1000, window: {documents: [1, 2]} }}})",
        docs,
        BSON_ARRAY(BSON("a" << 1 << "b" << 1 << "first" << 1000)
                   << BSON("a" << 2 << "b" << 3 << "first" << 1000)
                   << BSON("a" << 3 << "b" << 5 << "first" << 1000)
                   << BSON("a" << 4 << "b" << 7 << "first" << BSONNULL)));
}

TEST_F(SBESetWindowFieldsTest, FirstTestNegativeWindow) {
    auto docs = std::vector<BSONArray>{BSON_ARRAY(BSON("a" << 1 << "b" << 1)),
                                       BSON_ARRAY(BSON("a" << 2 << "b" << 3)),
                                       BSON_ARRAY(BSON("a" << 3 << "b" << 5)),
                                       BSON_ARRAY(BSON("a" << 4 << "b" << 7))};
    runSetWindowFieldsTest(
        R"({sortBy: {a: 1}, output: {first: {$first: '$b', window: {documents: [-2, -1]} }}})",
        docs,
        BSON_ARRAY(BSON("a" << 1 << "b" << 1 << "first" << BSONNULL)
                   << BSON("a" << 2 << "b" << 3 << "first" << 1)
                   << BSON("a" << 3 << "b" << 5 << "first" << 1)
                   << BSON("a" << 4 << "b" << 7 << "first" << 3)));
}

TEST_F(SBESetWindowFieldsTest, FirstTestConstantValueNegativeWindow) {
    auto docs = std::vector<BSONArray>{BSON_ARRAY(BSON("a" << 1 << "b" << 1)),
                                       BSON_ARRAY(BSON("a" << 2 << "b" << 3)),
                                       BSON_ARRAY(BSON("a" << 3 << "b" << 5)),
                                       BSON_ARRAY(BSON("a" << 4 << "b" << 7))};
    runSetWindowFieldsTest(
        R"({sortBy: {a: 1}, output: {first: {$first: 1000, window: {documents: [-2, -1]} }}})",
        docs,
        BSON_ARRAY(BSON("a" << 1 << "b" << 1 << "first" << BSONNULL)
                   << BSON("a" << 2 << "b" << 3 << "first" << 1000)
                   << BSON("a" << 3 << "b" << 5 << "first" << 1000)
                   << BSON("a" << 4 << "b" << 7 << "first" << 1000)));
}

TEST_F(SBESetWindowFieldsTest, LastTestPositiveWindow) {
    auto docs = std::vector<BSONArray>{BSON_ARRAY(BSON("a" << 1 << "b" << 1)),
                                       BSON_ARRAY(BSON("a" << 2 << "b" << 3)),
                                       BSON_ARRAY(BSON("a" << 3 << "b" << 5)),
                                       BSON_ARRAY(BSON("a" << 4 << "b" << 7))};
    runSetWindowFieldsTest(
        R"({sortBy: {a: 1}, output: {last: {$last: '$b', window: {documents: [1, 2]} }}})",
        docs,
        BSON_ARRAY(BSON("a" << 1 << "b" << 1 << "last" << 5)
                   << BSON("a" << 2 << "b" << 3 << "last" << 7)
                   << BSON("a" << 3 << "b" << 5 << "last" << 7)
                   << BSON("a" << 4 << "b" << 7 << "last" << BSONNULL)));
}

TEST_F(SBESetWindowFieldsTest, LastTestConstantValuePositiveWindow) {
    auto docs = std::vector<BSONArray>{BSON_ARRAY(BSON("a" << 1 << "b" << 1)),
                                       BSON_ARRAY(BSON("a" << 2 << "b" << 3)),
                                       BSON_ARRAY(BSON("a" << 3 << "b" << 5)),
                                       BSON_ARRAY(BSON("a" << 4 << "b" << 7))};
    runSetWindowFieldsTest(
        R"({sortBy: {a: 1}, output: {last: {$last: 1000, window: {documents: [1, 2]} }}})",
        docs,
        BSON_ARRAY(BSON("a" << 1 << "b" << 1 << "last" << 1000)
                   << BSON("a" << 2 << "b" << 3 << "last" << 1000)
                   << BSON("a" << 3 << "b" << 5 << "last" << 1000)
                   << BSON("a" << 4 << "b" << 7 << "last" << BSONNULL)));
}

TEST_F(SBESetWindowFieldsTest, LastTestNegativeWindow) {
    auto docs = std::vector<BSONArray>{BSON_ARRAY(BSON("a" << 1 << "b" << 1)),
                                       BSON_ARRAY(BSON("a" << 2 << "b" << 3)),
                                       BSON_ARRAY(BSON("a" << 3 << "b" << 5)),
                                       BSON_ARRAY(BSON("a" << 4 << "b" << 7))};
    runSetWindowFieldsTest(
        R"({sortBy: {a: 1}, output: {last: {$last: '$b', window: {documents: [-2, -1]} }}})",
        docs,
        BSON_ARRAY(BSON("a" << 1 << "b" << 1 << "last" << BSONNULL)
                   << BSON("a" << 2 << "b" << 3 << "last" << 1)
                   << BSON("a" << 3 << "b" << 5 << "last" << 3)
                   << BSON("a" << 4 << "b" << 7 << "last" << 5)));
}

TEST_F(SBESetWindowFieldsTest, LastTestConstantValueNegativeWindow) {
    auto docs = std::vector<BSONArray>{BSON_ARRAY(BSON("a" << 1 << "b" << 1)),
                                       BSON_ARRAY(BSON("a" << 2 << "b" << 3)),
                                       BSON_ARRAY(BSON("a" << 3 << "b" << 5)),
                                       BSON_ARRAY(BSON("a" << 4 << "b" << 7))};
    runSetWindowFieldsTest(
        R"({sortBy: {a: 1}, output: {last: {$last: 1000, window: {documents: [-2, -1]} }}})",
        docs,
        BSON_ARRAY(BSON("a" << 1 << "b" << 1 << "last" << BSONNULL)
                   << BSON("a" << 2 << "b" << 3 << "last" << 1000)
                   << BSON("a" << 3 << "b" << 5 << "last" << 1000)
                   << BSON("a" << 4 << "b" << 7 << "last" << 1000)));
}

TEST_F(SBESetWindowFieldsTest, PartitionedWithRangeAvg) {
    auto ts = 1736467200000LL;
    auto oneDay = 86400000LL;
    auto docs = std::vector<BSONArray>{
        BSON_ARRAY(BSON("a" << Date_t::fromMillisSinceEpoch(ts) << "b" << 1 << "c" << 1)),
        BSON_ARRAY(BSON("a" << Date_t::fromMillisSinceEpoch(ts + oneDay) << "b" << 3 << "c" << 1)),
        BSON_ARRAY(BSON("a" << Date_t::fromMillisSinceEpoch(ts) << "b" << 5 << "c" << 2)),
        BSON_ARRAY(
            BSON("a" << Date_t::fromMillisSinceEpoch(ts + oneDay * 2) << "b" << 7 << "c" << 2))};

    runSetWindowFieldsTest(
        R"({partitionBy: '$c', sortBy: {a: 1}, output: {avg: {$avg: '$b', window: {range: [-1, 0], unit: 'day'} }}})",
        docs,
        BSON_ARRAY(
            BSON("a" << Date_t::fromMillisSinceEpoch(ts) << "b" << 1 << "c" << 1 << "avg" << 1)
            << BSON("a" << Date_t::fromMillisSinceEpoch(ts + oneDay) << "b" << 3 << "c" << 1
                        << "avg" << 2)
            << BSON("a" << Date_t::fromMillisSinceEpoch(ts) << "b" << 5 << "c" << 2 << "avg" << 5)
            << BSON("a" << Date_t::fromMillisSinceEpoch(ts + oneDay * 2) << "b" << 7 << "c" << 2
                        << "avg" << 7)));
}

TEST_F(SBESetWindowFieldsTest, SumNegativeToPositiveRange) {
    auto docs = std::vector<BSONArray>{BSON_ARRAY(BSON("a" << 1 << "b" << 1)),
                                       BSON_ARRAY(BSON("a" << 2 << "b" << 3)),
                                       BSON_ARRAY(BSON("a" << 3 << "b" << 5)),
                                       BSON_ARRAY(BSON("a" << 4 << "b" << 7))};
    runSetWindowFieldsTest(
        R"({sortBy: {a: 1}, output: {sum: {$sum: '$b', window: {range: [-2, 1]} }}})",
        docs,
        BSON_ARRAY(BSON("a" << 1 << "b" << 1 << "sum" << 4)
                   << BSON("a" << 2 << "b" << 3 << "sum" << 9)
                   << BSON("a" << 3 << "b" << 5 << "sum" << 16)
                   << BSON("a" << 4 << "b" << 7 << "sum" << 15)));
}

TEST_F(SBESetWindowFieldsTest, TopNUnboundedToCurrent) {
    auto docs = std::vector<BSONArray>{BSON_ARRAY(BSON("a" << 1 << "b" << 1)),
                                       BSON_ARRAY(BSON("a" << 2 << "b" << 7)),
                                       BSON_ARRAY(BSON("a" << 3 << "b" << 5)),
                                       BSON_ARRAY(BSON("a" << 4 << "b" << 3))};
    runSetWindowFieldsTest(
        R"({sortBy: {a: 1}, output: {top2: {$topN: {output: '$a', n: 2, sortBy: {b: -1}}, window: {documents: ['unbounded', 'current']} }}})",
        docs,
        BSON_ARRAY(BSON("a" << 1 << "b" << 1 << "top2" << BSON_ARRAY(1))
                   << BSON("a" << 2 << "b" << 7 << "top2" << BSON_ARRAY(2 << 1))
                   << BSON("a" << 3 << "b" << 5 << "top2" << BSON_ARRAY(2 << 3))
                   << BSON("a" << 4 << "b" << 3 << "top2" << BSON_ARRAY(2 << 3))));
}

TEST_F(SBESetWindowFieldsTest, ShiftWithoutWindow) {
    auto docs = std::vector<BSONArray>{BSON_ARRAY(BSON("a" << 1 << "b" << 1)),
                                       BSON_ARRAY(BSON("a" << 2 << "b" << 7)),
                                       BSON_ARRAY(BSON("a" << 3 << "b" << 5)),
                                       BSON_ARRAY(BSON("a" << 4 << "b" << 3))};
    runSetWindowFieldsTest(
        R"({sortBy: {a: 1}, output: {lastOne: {$shift: {output: '$b', by: -1, default: 0}}}})",
        docs,
        BSON_ARRAY(BSON("a" << 1 << "b" << 1 << "lastOne" << 0)
                   << BSON("a" << 2 << "b" << 7 << "lastOne" << 1)
                   << BSON("a" << 3 << "b" << 5 << "lastOne" << 7)
                   << BSON("a" << 4 << "b" << 3 << "lastOne" << 5)));
}

// TODO SERVER-99529 re-enable this test.
// TEST_F(SBESetWindowFieldsTest, RankUnboundedToUnbounded) {
//     auto docs = std::vector<BSONArray>{BSON_ARRAY(BSON("a" << 1 << "b" << 1)),
//                                        BSON_ARRAY(BSON("a" << 2 << "b" << 7)),
//                                        BSON_ARRAY(BSON("a" << 3 << "b" << 5)),
//                                        BSON_ARRAY(BSON("a" << 4 << "b" << 3))};
//     runSetWindowFieldsTest(R"({sortBy: {a: 1}, output: {rank: {$rank: {}}}})",
//                            docs,
//                            BSON_ARRAY(BSON("a" << 1 << "b" << 1 << "rank" << 1)
//                                       << BSON("a" << 2 << "b" << 7 << "rank" << 2)
//                                       << BSON("a" << 3 << "b" << 5 << "rank" << 3)
//                                       << BSON("a" << 4 << "b" << 3 << "rank" << 4)));
// }

TEST_F(SBESetWindowFieldsTest, DerivativeRangeNegativeToCurrent) {
    auto ts = 1736467200000LL;
    auto thirtySec = 30000LL;
    auto docs = std::vector<BSONArray>{
        BSON_ARRAY(BSON("a" << Date_t::fromMillisSinceEpoch(ts) << "b" << 100)),
        BSON_ARRAY(BSON("a" << Date_t::fromMillisSinceEpoch(ts + thirtySec) << "b" << 101)),
        BSON_ARRAY(BSON("a" << Date_t::fromMillisSinceEpoch(ts + thirtySec * 2) << "b" << 102.5)),
        BSON_ARRAY(BSON("a" << Date_t::fromMillisSinceEpoch(ts + thirtySec * 3) << "b" << 103))};

    runSetWindowFieldsTest(
        R"({sortBy: {a: 1}, output: {speed: {$derivative: {input: '$b', unit: 'hour'}, window: {range: [-30, 0], unit: 'second'} }}})",
        docs,
        BSON_ARRAY(
            BSON("a" << Date_t::fromMillisSinceEpoch(ts) << "b" << 100 << "speed" << BSONNULL)
            << BSON("a" << Date_t::fromMillisSinceEpoch(ts + thirtySec) << "b" << 101 << "speed"
                        << 120)
            << BSON("a" << Date_t::fromMillisSinceEpoch(ts + thirtySec * 2) << "b" << 102.5
                        << "speed" << 180)
            << BSON("a" << Date_t::fromMillisSinceEpoch(ts + thirtySec * 3) << "b" << 103 << "speed"
                        << 60)));
}

TEST_F(SBESetWindowFieldsTest, SetUnionWindowNoRemoval) {
    auto docs = std::vector<BSONArray>{BSON_ARRAY(fromjson("{a: 1, b: [1, 2]}")),
                                       BSON_ARRAY(fromjson("{a: 2, b: [3, 4]}")),
                                       BSON_ARRAY(fromjson("{a: 3, b: [5, 6]}"))};

    runArrayAccumulatorTest(
        ArrayAccumType::kSetUnion,
        R"({sortBy: {a: 1}, output: {result: {$setUnion: '$b'}}})",
        docs,
        BSON_ARRAY(fromjson("{a: 1, b: [1, 2], result: [1, 2, 3, 4, 5, 6]}")
                   << fromjson("{a: 2, b: [3, 4], result: [1, 2, 3, 4, 5, 6]}")
                   << fromjson("{a: 3, b: [5, 6], result: [1, 2, 3, 4, 5, 6]}")));
}

TEST_F(SBESetWindowFieldsTest, SetUnionWindowDoesNotAllowDuplicates) {
    auto docs = std::vector<BSONArray>{BSON_ARRAY(fromjson("{a: 1, b: [1, 2]}")),
                                       BSON_ARRAY(fromjson("{a: 2, b: [2, 3]}")),
                                       BSON_ARRAY(fromjson("{a: 3, b: [2, 3]}"))};

    runArrayAccumulatorTest(ArrayAccumType::kSetUnion,
                            R"({sortBy: {a: 1}, output: {result: {$setUnion: '$b'}}})",
                            docs,
                            BSON_ARRAY(fromjson("{a: 1, b: [1, 2], result: [1, 2, 3]}")
                                       << fromjson("{a: 2, b: [3, 4], result: [1, 2, 3]}")
                                       << fromjson("{a: 3, b: [5, 6], result: [1, 2, 3]}")));
}

TEST_F(SBESetWindowFieldsTest, SetUnionWindowWithRemovalsAndDuplicates) {
    auto docs = std::vector<BSONArray>{BSON_ARRAY(fromjson("{a: 1, b: [1, 2]}")),
                                       BSON_ARRAY(fromjson("{a: 2, b: [2, 3, 4]}")),
                                       BSON_ARRAY(fromjson("{a: 3, b: [4, 5, 6]}"))};

    runArrayAccumulatorTest(
        ArrayAccumType::kSetUnion,
        R"({sortBy: {a: 1}, output: {result: {$setUnion: '$b', window: {documents: [-1, 0]}}}})",
        docs,
        BSON_ARRAY(fromjson("{a: 1, b: [1, 2], result: [1, 2]}")
                   << fromjson("{a: 2, b: [2, 3, 4], result: [1, 2, 3, 4]}")
                   << fromjson("{a: 3, b: [4, 5, 6], result: [2, 3, 4, 5, 6]}")));
}

TEST_F(SBESetWindowFieldsTest, SetUnionWindowIgnoresMissingFieldOnAddition) {
    auto docs = std::vector<BSONArray>{BSON_ARRAY(fromjson("{a: 1, b: [1, 2]}")),
                                       BSON_ARRAY(fromjson("{a: 2}")),
                                       BSON_ARRAY(fromjson("{a: 3, b: [5, 6]}"))};

    runArrayAccumulatorTest(ArrayAccumType::kSetUnion,
                            R"({sortBy: {a: 1}, output: {result: {$setUnion: '$b'}}})",
                            docs,
                            BSON_ARRAY(fromjson("{a: 1, b: [1, 2], result: [1, 2, 5, 6]}")
                                       << fromjson("{a: 2, b: [3, 4], result: [1, 2, 5, 6]}")
                                       << fromjson("{a: 3, b: [5, 6], result: [1, 2, 5, 6]}")));
}

TEST_F(SBESetWindowFieldsTest, SetUnionWindowIgnoresMissingFieldWithRemoval) {
    auto docs = std::vector<BSONArray>{BSON_ARRAY(fromjson("{a: 1}")),
                                       BSON_ARRAY(fromjson("{a: 2, b: [1, 2]}")),
                                       BSON_ARRAY(fromjson("{a: 3, b: [5, 6]}"))};
    runArrayAccumulatorTest(
        ArrayAccumType::kSetUnion,
        R"({sortBy: {a: 1}, output: {result: {$setUnion: '$b', window: {documents: [-1, 0]}}}})",
        docs,
        BSON_ARRAY(fromjson("{a: 1, b: [1, 2], result: []}")
                   << fromjson("{a: 2, b: [3, 4], result: [1, 2]}")
                   << fromjson("{a: 3, b: [5, 6], result: [1, 2, 5, 6]}")));
}

TEST_F(SBESetWindowFieldsTest, SetUnionWindowSubfiedMissingFieldWithRemoval) {
    auto docs = std::vector<BSONArray>{BSON_ARRAY(fromjson("{a: 1, b: {c: [1, 2, 3]}}")),
                                       BSON_ARRAY(fromjson("{a: 2}")),
                                       BSON_ARRAY(fromjson("{a: 3, b: 1}")),
                                       BSON_ARRAY(fromjson("{a: 4, b: {c: [4, 5, 6]}}")),
                                       BSON_ARRAY(fromjson("{a: 5, b: {c: [7, 8, 9]}}"))};

    runArrayAccumulatorTest(
        ArrayAccumType::kSetUnion,
        R"({sortBy: {a: 1}, output: {result: {$setUnion: '$b.c', window: {documents: [-1, 0]}}}})",
        docs,
        BSON_ARRAY(fromjson("{a: 1, b: {c: [1, 2, 3]}, result: [1, 2, 3]}")
                   << fromjson("{a: 2, result: [1, 2, 3]}") << fromjson("{a: 3, b: 1, result: []}")
                   << fromjson("{a: 4, b: {c: [4, 5, 6]}, result: [4, 5, 6]}")
                   << fromjson("{a: 5, b: {c: [7, 8, 9]}, result: [4, 5, 6, 7, 8, 9]}")));
}

TEST_F(SBESetWindowFieldsTest, SetUnionWindowSubfiedArrayMissingFieldWithRemoval) {
    auto docs = std::vector<BSONArray>{
        BSON_ARRAY(fromjson("{a: 1, b: [{c: [1, 2, 3]}, {c: [4, 5, 6]}]}")),
        BSON_ARRAY(fromjson("{a: 2}")),
        BSON_ARRAY(fromjson("{a: 3, b: [{c: [1, 2, 3]}, {c: [4, 5, 6]}, {c: [13, 14, 15]}]}")),
        BSON_ARRAY(fromjson("{a: 4, b: [{c: [7, 8, 9]}, {c: [10, 11, 12]}]}"))};

    runArrayAccumulatorTest(
        ArrayAccumType::kSetUnion,
        R"({sortBy: {a: 1}, output: {result: {$setUnion: '$b.c', window: {documents: [-1, 0]}}}})",
        docs,
        BSON_ARRAY(
            fromjson("{a: 1, b: [{c: [1, 2, 3]}, {c: [4, 5, 6]}], result: [[1, 2, 3], [4, 5, 6]]}")
            << fromjson("{a: 2, result: [[1, 2, 3], [4, 5, 6]]}")
            << fromjson(
                   "{a: 3, b: [{c: [1, 2, 3]}, {c: [4, 5, 6]}, {c: [13, 14, 15]}], result: [[1, "
                   "2, 3], [4, 5, 6], [13, 14, 15]]}")
            << fromjson("{a: 4, b: [{c: [7, 8, 9]}, {c: [10, 11, 12]}], result: [[1, 2, 3], [4, 5, "
                        "6], [13, 14, 15], [7,8, 9], [10, 11, 12]]}")));
}


TEST_F(SBESetWindowFieldsTest, SetUnionWindowRespectsCollation) {
    auto docs = std::vector<BSONArray>{BSON_ARRAY(fromjson("{a: 1, b: [\"a\", \"b\"]}")),
                                       BSON_ARRAY(fromjson("{a: 2, b: [\"a\", \"c\"]}")),
                                       BSON_ARRAY(fromjson("{a: 3, b: [\"c\"]}"))};

    runArrayAccumulatorTest(
        ArrayAccumType::kSetUnion,
        R"({sortBy: {a: 1}, output: {result: {$setUnion: '$b'}}})",
        docs,
        BSON_ARRAY(fromjson("{a: 1, b: [\"a\", \"b\"], result: [\"a\"]}")
                   << fromjson("{a: 2, b: [\"a\", \"c\"], result: [\"a\"]}")
                   << fromjson("{a: 3, b: [\"c\"], result: [\"a\"]}")),
        std::make_unique<CollatorInterfaceMock>(CollatorInterfaceMock::MockType::kAlwaysEqual));
}

TEST_F(SBESetWindowFieldsTest, SetUnionWindowArrayOfArrays) {
    auto docs = std::vector<BSONArray>{BSON_ARRAY(fromjson("{a: 1, b: [[1, 2, 3], [4, 5, 6]]}")),
                                       BSON_ARRAY(fromjson("{a: 2, b: [[1, 2, 3]]}")),
                                       BSON_ARRAY(fromjson("{a: 3, b: [[7, 8, 9], [4, 5, 6]]}"))};

    runArrayAccumulatorTest(
        ArrayAccumType::kSetUnion,
        R"({sortBy: {a: 1}, output: {result: {$setUnion: '$b', window: {documents: [-1, 0]}}}})",
        docs,
        BSON_ARRAY(fromjson("{a: 1, b: [1, 2], result: [[1, 2, 3], [4, 5, 6]]}")
                   << fromjson("{a: 2, b: [3, 4], result: [[1, 2, 3], [4, 5, 6]]}")
                   << fromjson("{a: 3, b: [5, 6], result: [[1, 2, 3], [7, 8, 9], [4, 5, 6]]}")));
}
TEST_F(SBESetWindowFieldsTest, SetUnionWindowNegOneToOne) {
    auto docs = std::vector<BSONArray>{BSON_ARRAY(fromjson("{a: 1, b: [1, 2]}")),
                                       BSON_ARRAY(fromjson("{a: 2, b: [3, 4]}")),
                                       BSON_ARRAY(fromjson("{a: 2, b: [5, 6]}")),
                                       BSON_ARRAY(fromjson("{a: 2}")),
                                       BSON_ARRAY(fromjson("{a: 1, b: [5, 6]}"))};

    runArrayAccumulatorTest(
        ArrayAccumType::kSetUnion,
        R"({sortBy: {a: 1}, output: {result: {$setUnion: '$b', window: {documents: [-1, 1]}}}})",
        docs,
        BSON_ARRAY(fromjson("{a: 1, b: [1, 2], result: [1, 2, 3, 4]}")
                   << fromjson("{a: 2, b: [3, 4], result: [1, 2, 3, 4, 5, 6]}")
                   << fromjson("{a: 2, b: [3, 4], result: [3, 4, 5, 6]}")
                   << fromjson("{a: 2, b: [3, 4], result: [5, 6]}")
                   << fromjson("{a: 3, b: [5, 6], result: [5, 6]}")));
}

TEST_F(SBESetWindowFieldsTest, SetUnionWindowZeroToZero) {
    auto docs = std::vector<BSONArray>{BSON_ARRAY(fromjson("{a: 1, b: [1, 2]}")),
                                       BSON_ARRAY(fromjson("{a: 2, b: [3, 4]}")),
                                       BSON_ARRAY(fromjson("{a: 2, b: [5, 6]}")),
                                       BSON_ARRAY(fromjson("{a: 2}")),
                                       BSON_ARRAY(fromjson("{a: 1, b: [5, 6]}"))};

    runArrayAccumulatorTest(
        ArrayAccumType::kSetUnion,
        R"({sortBy: {a: 1}, output: {result: {$setUnion: '$b', window: {documents: [0, 0]}}}})",
        docs,
        BSON_ARRAY(fromjson("{a: 1, b: [1, 2], result: [1, 2]}")
                   << fromjson("{a: 2, b: [3, 4], result: [3, 4]}")
                   << fromjson("{a: 2, b: [3, 4], result: [5, 6]}")
                   << fromjson("{a: 2, b: [3, 4], result: []}")
                   << fromjson("{a: 3, b: [5, 6], result: [5, 6]}")));
}


TEST_F(SBESetWindowFieldsTest, SetUnionWindowZeroToOne) {
    auto docs = std::vector<BSONArray>{BSON_ARRAY(fromjson("{a: 1, b: [1, 2]}")),
                                       BSON_ARRAY(fromjson("{a: 2, b: [3, 4]}")),
                                       BSON_ARRAY(fromjson("{a: 2, b: [5, 6]}")),
                                       BSON_ARRAY(fromjson("{a: 2}")),
                                       BSON_ARRAY(fromjson("{a: 1, b: [5, 6]}"))};

    runArrayAccumulatorTest(
        ArrayAccumType::kSetUnion,
        R"({sortBy: {a: 1}, output: {result: {$setUnion: '$b', window: {documents: [0, 1]}}}})",
        docs,
        BSON_ARRAY(fromjson("{a: 1, b: [1, 2], result: [1, 2, 3, 4]}")
                   << fromjson("{a: 2, b: [3, 4], result: [3, 4, 5, 6]}")
                   << fromjson("{a: 2, b: [3, 4], result: [5, 6]}")
                   << fromjson("{a: 2, b: [3, 4], result: [5, 6]}")
                   << fromjson("{a: 3, b: [5, 6], result: [5, 6]}")));
}


TEST_F(SBESetWindowFieldsTest, SetUnionWindowCurrentToCurrent) {
    auto docs = std::vector<BSONArray>{BSON_ARRAY(fromjson("{a: 1, b: [1, 2]}")),
                                       BSON_ARRAY(fromjson("{a: 2, b: [3, 4]}")),
                                       BSON_ARRAY(fromjson("{a: 2, b: [5, 6]}")),
                                       BSON_ARRAY(fromjson("{a: 2}")),
                                       BSON_ARRAY(fromjson("{a: 1, b: [5, 6]}"))};

    runArrayAccumulatorTest(
        ArrayAccumType::kSetUnion,
        R"({sortBy: {a: 1}, output: {result: {$setUnion: '$b', window: {documents: ["current", "current"]}}}})",
        docs,
        BSON_ARRAY(fromjson("{a: 1, b: [1, 2], result: [1, 2]}")
                   << fromjson("{a: 2, b: [3, 4], result: [3, 4]}")
                   << fromjson("{a: 2, b: [3, 4], result: [5, 6]}")
                   << fromjson("{a: 2, b: [3, 4], result: []}")
                   << fromjson("{a: 3, b: [5, 6], result: [5, 6]}")));
}

TEST_F(SBESetWindowFieldsTest, SetUnionWindowCurrentToUnbounded) {
    auto docs = std::vector<BSONArray>{BSON_ARRAY(fromjson("{a: 1, b: [1, 2]}")),
                                       BSON_ARRAY(fromjson("{a: 2, b: [3, 4]}")),
                                       BSON_ARRAY(fromjson("{a: 2, b: [5, 6]}")),
                                       BSON_ARRAY(fromjson("{a: 2}")),
                                       BSON_ARRAY(fromjson("{a: 1, b: [5, 6]}"))};

    runArrayAccumulatorTest(
        ArrayAccumType::kSetUnion,
        R"({sortBy: {a: 1}, output: {result: {$setUnion: '$b', window: {documents: ["current", "unbounded"]}}}})",
        docs,
        BSON_ARRAY(fromjson("{a: 1, b: [1, 2], result: [1, 2, 3, 4, 5, 6]}")
                   << fromjson("{a: 2, b: [3, 4], result: [3, 4, 5, 6]}")
                   << fromjson("{a: 2, b: [3, 4], result: [5, 6]}")
                   << fromjson("{a: 2, b: [3, 4], result: [5, 6]}")
                   << fromjson("{a: 3, b: [5, 6], result: [5, 6]}")));
}

TEST_F(SBESetWindowFieldsTest, SetUnionWindowUnboundedToCurrent) {
    auto docs = std::vector<BSONArray>{BSON_ARRAY(fromjson("{a: 1, b: [1, 2]}")),
                                       BSON_ARRAY(fromjson("{a: 2, b: [3, 4]}")),
                                       BSON_ARRAY(fromjson("{a: 2, b: [5, 6]}")),
                                       BSON_ARRAY(fromjson("{a: 2}")),
                                       BSON_ARRAY(fromjson("{a: 1, b: [5, 6]}"))};

    runArrayAccumulatorTest(
        ArrayAccumType::kSetUnion,
        R"({sortBy: {a: 1}, output: {result: {$setUnion: '$b', window: {documents: ["unbounded", "current"]}}}})",
        docs,
        BSON_ARRAY(fromjson("{a: 1, b: [1, 2], result: [1, 2]}")
                   << fromjson("{a: 2, b: [3, 4], result: [1, 2, 3, 4]}")
                   << fromjson("{a: 2, b: [3, 4], result: [1, 2, 3, 4, 5, 6]}")
                   << fromjson("{a: 2, b: [3, 4], result: [1, 2, 3, 4, 5, 6]}")
                   << fromjson("{a: 3, b: [5, 6], result: [1, 2, 3, 4, 5, 6]}")));
}

TEST_F(SBESetWindowFieldsTest, SetUnionWindowUnboundedToUnbounded) {
    auto docs = std::vector<BSONArray>{BSON_ARRAY(fromjson("{a: 1, b: [1, 2]}")),
                                       BSON_ARRAY(fromjson("{a: 2, b: [3, 4]}")),
                                       BSON_ARRAY(fromjson("{a: 2, b: [5, 6]}")),
                                       BSON_ARRAY(fromjson("{a: 2}")),
                                       BSON_ARRAY(fromjson("{a: 1, b: [5, 6]}"))};

    runArrayAccumulatorTest(
        ArrayAccumType::kSetUnion,
        R"({sortBy: {a: 1}, output: {result: {$setUnion: '$b', window: {documents: ["unbounded", "unbounded"]}}}})",
        docs,
        BSON_ARRAY(fromjson("{a: 1, b: [1, 2], result: [1, 2, 3, 4, 5, 6]}")
                   << fromjson("{a: 2, b: [3, 4], result: [1, 2, 3, 4, 5, 6]}")
                   << fromjson("{a: 2, b: [3, 4], result: [1, 2, 3, 4, 5, 6]}")
                   << fromjson("{a: 2, b: [3, 4], result: [1, 2, 3, 4, 5, 6]}")
                   << fromjson("{a: 3, b: [5, 6], result: [1, 2, 3, 4, 5, 6]}")));
}

TEST_F(SBESetWindowFieldsTest, SetUnionWindowDifferentTypes) {
    // We want to test the memory accounting for different types, so we need a second document so we
    // remove values from the window.
    auto docs = std::vector<BSONArray>{
        BSON_ARRAY(fromjson(
            "{a: 1, b: [\"a\", \"Beauty\", \"Certainly this is very large string that we will use "
            "SBE's StringBig type for\", \"MinKey\", \"MaxKey\", null, 1, Infinity, "
            "NumberDecimal(\"Infinity\"), {foo: 1, bar: 2}, [4, 5, 6], true, Date(1), /^ABC/i]}")),
        BSON_ARRAY(fromjson("{a: 2}"))};

    runArrayAccumulatorTest(
        ArrayAccumType::kSetUnion,
        R"({sortBy: {a: 1}, output: {result: {$setUnion: '$b', window: {documents: ["current", 0]}}}})",
        docs,
        BSON_ARRAY(
            fromjson("{a: 1, b: [1, 2], result: [\"a\", \"Beauty\", \"Certainly this is very large "
                     "string that we will use SBE's StringBig type for\", \"MinKey\", \"MaxKey\", "
                     "null, 1, Infinity, NumberDecimal(\"Infinity\"), {foo: 1, bar: 2}, [4, "
                     "5, 6], true, Date(1), /^ABC/i]}")
            << fromjson("{a: 2, result: []}")));
}

TEST_F(SBESetWindowFieldsTest, ConcatArraysWindowNoRemoval) {
    auto docs = std::vector<BSONArray>{BSON_ARRAY(fromjson("{a: 1, b: [1, 2]}")),
                                       BSON_ARRAY(fromjson("{a: 2, b: [3, 4]}")),
                                       BSON_ARRAY(fromjson("{a: 3, b: [5, 6]}"))};

    runArrayAccumulatorTest(
        ArrayAccumType::kConcatArrays,
        R"({sortBy: {a: 1}, output: {result: {$concatArrays: '$b'}}})",
        docs,
        BSON_ARRAY(fromjson("{a: 1, b: [1, 2], result: [1, 2, 3, 4, 5, 6]}")
                   << fromjson("{a: 2, b: [3, 4], result: [1, 2, 3, 4, 5, 6]}")
                   << fromjson("{a: 3, b: [5, 6], result: [1, 2, 3, 4, 5, 6]}")));
}

TEST_F(SBESetWindowFieldsTest, ConcatArraysWindowAllowsDuplicates) {
    auto docs = std::vector<BSONArray>{BSON_ARRAY(fromjson("{a: 1, b: [1, 2]}")),
                                       BSON_ARRAY(fromjson("{a: 2, b: [2, 3]}")),
                                       BSON_ARRAY(fromjson("{a: 3, b: [2, 3]}"))};

    runArrayAccumulatorTest(
        ArrayAccumType::kConcatArrays,
        R"({sortBy: {a: 1}, output: {result: {$concatArrays: '$b'}}})",
        docs,
        BSON_ARRAY(fromjson("{a: 1, b: [1, 2], result: [1, 2, 2, 3, 2, 3]}")
                   << fromjson("{a: 2, b: [3, 4], result: [1, 2, 2, 3, 2, 3]}")
                   << fromjson("{a: 3, b: [5, 6], result: [1, 2, 2, 3, 2, 3]}")));
}

TEST_F(SBESetWindowFieldsTest, ConcatArraysWindowWithRemovalsAndDuplicates) {
    auto docs = std::vector<BSONArray>{BSON_ARRAY(fromjson("{a: 1, b: [1, 2]}")),
                                       BSON_ARRAY(fromjson("{a: 1, b: [2, 3, 4]}")),
                                       BSON_ARRAY(fromjson("{a: 1, b: [4, 5, 6]}"))};

    runArrayAccumulatorTest(
        ArrayAccumType::kConcatArrays,
        R"({sortBy: {a: 1}, output: {result: {$concatArrays: '$b', window: {documents: [-1, 0]}}}})",
        docs,
        BSON_ARRAY(fromjson("{a: 1, b: [1, 2], result: [1, 2]}")
                   << fromjson("{a: 2, b: [3, 4], result: [1, 2, 2, 3, 4]}")
                   << fromjson("{a: 3, b: [5, 6], result: [2, 3, 4, 4, 5, 6]}")));
}

TEST_F(SBESetWindowFieldsTest, ConcatArraysWindowIgnoresMissingFieldOnAddition) {
    auto docs = std::vector<BSONArray>{BSON_ARRAY(fromjson("{a: 1, b: [1, 2]}")),
                                       BSON_ARRAY(fromjson("{a: 2}")),
                                       BSON_ARRAY(fromjson("{a: 1, b: [5, 6]}"))};

    runArrayAccumulatorTest(ArrayAccumType::kConcatArrays,
                            R"({sortBy: {a: 1}, output: {result: {$concatArrays: '$b'}}})",
                            docs,
                            BSON_ARRAY(fromjson("{a: 1, b: [1, 2], result: [1, 2, 5, 6]}")
                                       << fromjson("{a: 2, result: [1, 2, 5, 6]}")
                                       << fromjson("{a: 3, b: [5, 6], result: [1, 2, 5, 6]}")));
}

TEST_F(SBESetWindowFieldsTest, ConcatArraysWindowIgnoresMissingFieldWithRemoval) {
    auto docs = std::vector<BSONArray>{BSON_ARRAY(fromjson("{a: 1}")),
                                       BSON_ARRAY(fromjson("{a: 2, b: [1, 2]}")),
                                       BSON_ARRAY(fromjson("{a: 3, b: [5, 6]}"))};

    runArrayAccumulatorTest(
        ArrayAccumType::kConcatArrays,
        R"({sortBy: {a: 1}, output: {result: {$concatArrays: '$b', window: {documents: [-1, 0]}}}})",
        docs,
        BSON_ARRAY(fromjson("{a: 1, b: [1, 2], result: []}")
                   << fromjson("{a: 2, b: [3, 4], result: [1, 2]}")
                   << fromjson("{a: 3, b: [5, 6], result: [1, 2, 5, 6]}")));
}

TEST_F(SBESetWindowFieldsTest, ConcatArraysWindowSubfiedMissingFieldWithRemoval) {
    auto docs = std::vector<BSONArray>{BSON_ARRAY(fromjson("{a: 1, b: {c: [1, 2, 3]}}")),
                                       BSON_ARRAY(fromjson("{a: 2}")),
                                       BSON_ARRAY(fromjson("{a: 3, b: 1}")),
                                       BSON_ARRAY(fromjson("{a: 4, b: {c: [4, 5, 6]}}")),
                                       BSON_ARRAY(fromjson("{a: 5, b: {c: [7, 8, 9]}}"))};

    runArrayAccumulatorTest(
        ArrayAccumType::kConcatArrays,
        R"({sortBy: {a: 1}, output: {result: {$concatArrays: '$b.c', window: {documents: [-1, 0]}}}})",
        docs,
        BSON_ARRAY(fromjson("{a: 1, b: {c: [1, 2, 3]}, result: [1, 2, 3]}")
                   << fromjson("{a: 2, result: [1, 2, 3]}") << fromjson("{a: 3, b: 1, result: []}")
                   << fromjson("{a: 4, b: {c: [4, 5, 6]}, result: [4, 5, 6]}")
                   << fromjson("{a: 5, b: {c: [7, 8, 9]}, result: [4, 5, 6, 7, 8, 9]}")));
}

TEST_F(SBESetWindowFieldsTest, ConcatArraysWindowSubfiedArrayMissingFieldWithRemoval) {
    auto docs = std::vector<BSONArray>{
        BSON_ARRAY(fromjson("{a: 1, b: [{c: [1, 2, 3]}, {c: [4, 5, 6]}]}")),
        BSON_ARRAY(fromjson("{a: 2}")),
        BSON_ARRAY(fromjson("{a: 3, b: [{c: [1, 2, 3]}, {c: [4, 5, 6]}, {c: [13, 14, 15]}]}")),
        BSON_ARRAY(fromjson("{a: 4, b: [{c: [7, 8, 9]}, {c: [10, 11, 12]}]}"))};

    runArrayAccumulatorTest(
        ArrayAccumType::kConcatArrays,
        R"({sortBy: {a: 1}, output: {result: {$concatArrays: '$b.c', window: {documents: [-1, 0]}}}})",
        docs,
        BSON_ARRAY(
            fromjson("{a: 1, b: [{c: [1, 2, 3]}, {c: [4, 5, 6]}], result: [[1, 2, 3], [4, 5, 6]]}")
            << fromjson("{a: 2, result: [[1, 2, 3], [4, 5, 6]]}")
            << fromjson(
                   "{a: 3, b: [{c: [1, 2, 3]}, {c: [4, 5, 6]}, {c: [13, 14, 15]}], result: [[1, "
                   "2, 3], [4, 5, 6], [13, 14, 15]]}")
            << fromjson("{a: 4, b: [{c: [7, 8, 9]}, {c: [10, 11, 12]}], result: [[1, 2, 3], [4, 5, "
                        "6], [13, 14, 15], [7,8, 9], [10, 11, 12]]}")));
}

TEST_F(SBESetWindowFieldsTest, ConcatArraysWindowNotAffectedByCollation) {
    auto docs = std::vector<BSONArray>{BSON_ARRAY(fromjson("{a: 1, b: [\"a\", \"b\"]}")),
                                       BSON_ARRAY(fromjson("{a: 2, b: [\"a\", \"c\"]}")),
                                       BSON_ARRAY(fromjson("{a: 3, b: [\"c\"]}"))};

    runArrayAccumulatorTest(
        ArrayAccumType::kConcatArrays,
        R"({sortBy: {a: 1}, output: {result: {$concatArrays: '$b'}}})",
        docs,
        BSON_ARRAY(
            fromjson("{a: 1, b: [\"a\", \"b\"], result: [\"a\", \"b\", \"a\", \"c\", \"c\"]}")
            << fromjson("{a: 2, b: [\"a\", \"c\"], result: [\"a\", \"b\", \"a\", \"c\", \"c\"]}")
            << fromjson("{a: 3, b: [\"c\"], result: [\"a\", \"b\", \"a\", \"c\", \"c\"]}")),
        std::make_unique<CollatorInterfaceMock>(CollatorInterfaceMock::MockType::kAlwaysEqual));
}

TEST_F(SBESetWindowFieldsTest, ConcatArraysWindowArrayOfArrays) {
    auto docs = std::vector<BSONArray>{BSON_ARRAY(fromjson("{a: 1, b: [[1, 2, 3], [4, 5, 6]]}")),
                                       BSON_ARRAY(fromjson("{a: 2, b: [[1, 2, 3]]}")),
                                       BSON_ARRAY(fromjson("{a: 3, b: [[7, 8, 9], [4, 5, 6]]}"))};

    runArrayAccumulatorTest(
        ArrayAccumType::kConcatArrays,
        R"({sortBy: {a: 1}, output: {result: {$concatArrays: '$b', window: {documents: [-1, 0]}}}})",
        docs,
        BSON_ARRAY(fromjson("{a: 1, b: [1, 2], result: [[1, 2, 3], [4, 5, 6]]}")
                   << fromjson("{a: 2, b: [3, 4], result: [[1, 2, 3], [4, 5, 6], [1, 2, 3]]}")
                   << fromjson("{a: 3, b: [5, 6], result: [[1, 2, 3], [7, 8, 9], [4, 5, 6]]}")));
}

TEST_F(SBESetWindowFieldsTest, ConcatArraysWindowNegOneToOne) {
    auto docs = std::vector<BSONArray>{BSON_ARRAY(fromjson("{a: 1, b: [1, 2]}")),
                                       BSON_ARRAY(fromjson("{a: 2, b: [3, 4]}")),
                                       BSON_ARRAY(fromjson("{a: 2, b: [5, 6]}")),
                                       BSON_ARRAY(fromjson("{a: 2}")),
                                       BSON_ARRAY(fromjson("{a: 1, b: [5, 6]}"))};

    runArrayAccumulatorTest(
        ArrayAccumType::kConcatArrays,
        R"({sortBy: {a: 1}, output: {result: {$concatArrays: '$b', window: {documents: [-1, 1]}}}})",
        docs,
        BSON_ARRAY(fromjson("{a: 1, b: [1, 2], result: [1, 2, 3, 4]}")
                   << fromjson("{a: 2, b: [3, 4], result: [1, 2, 3, 4, 5, 6]}")
                   << fromjson("{a: 2, b: [3, 4], result: [3, 4, 5, 6]}")
                   << fromjson("{a: 2, b: [3, 4], result: [5, 6, 5, 6]}")
                   << fromjson("{a: 3, b: [5, 6], result: [5, 6]}")));
}

TEST_F(SBESetWindowFieldsTest, ConcatArraysWindowZeroToZero) {
    auto docs = std::vector<BSONArray>{BSON_ARRAY(fromjson("{a: 1, b: [1, 2]}")),
                                       BSON_ARRAY(fromjson("{a: 2, b: [3, 4]}")),
                                       BSON_ARRAY(fromjson("{a: 2, b: [5, 6]}")),
                                       BSON_ARRAY(fromjson("{a: 2}")),
                                       BSON_ARRAY(fromjson("{a: 1, b: [5, 6]}"))};

    runArrayAccumulatorTest(
        ArrayAccumType::kConcatArrays,
        R"({sortBy: {a: 1}, output: {result: {$concatArrays: '$b', window: {documents: [0, 0]}}}})",
        docs,
        BSON_ARRAY(fromjson("{a: 1, b: [1, 2], result: [1, 2]}")
                   << fromjson("{a: 2, b: [3, 4], result: [3, 4]}")
                   << fromjson("{a: 2, b: [3, 4], result: [5, 6]}")
                   << fromjson("{a: 2, b: [3, 4], result: []}")
                   << fromjson("{a: 3, b: [5, 6], result: [5, 6]}")));
}


TEST_F(SBESetWindowFieldsTest, ConcatArraysWindowZeroToOne) {
    auto docs = std::vector<BSONArray>{BSON_ARRAY(fromjson("{a: 1, b: [1, 2]}")),
                                       BSON_ARRAY(fromjson("{a: 2, b: [3, 4]}")),
                                       BSON_ARRAY(fromjson("{a: 2, b: [5, 6]}")),
                                       BSON_ARRAY(fromjson("{a: 2}")),
                                       BSON_ARRAY(fromjson("{a: 1, b: [5, 6]}"))};

    runArrayAccumulatorTest(
        ArrayAccumType::kConcatArrays,
        R"({sortBy: {a: 1}, output: {result: {$concatArrays: '$b', window: {documents: [0, 1]}}}})",
        docs,
        BSON_ARRAY(fromjson("{a: 1, b: [1, 2], result: [1, 2, 3, 4]}")
                   << fromjson("{a: 2, b: [3, 4], result: [3, 4, 5, 6]}")
                   << fromjson("{a: 2, b: [3, 4], result: [5, 6]}")
                   << fromjson("{a: 2, b: [3, 4], result: [5, 6]}")
                   << fromjson("{a: 3, b: [5, 6], result: [5, 6]}")));
}


TEST_F(SBESetWindowFieldsTest, ConcatArraysWindowCurrentToCurrent) {
    auto docs = std::vector<BSONArray>{BSON_ARRAY(fromjson("{a: 1, b: [1, 2]}")),
                                       BSON_ARRAY(fromjson("{a: 2, b: [3, 4]}")),
                                       BSON_ARRAY(fromjson("{a: 2, b: [5, 6]}")),
                                       BSON_ARRAY(fromjson("{a: 2}")),
                                       BSON_ARRAY(fromjson("{a: 1, b: [5, 6]}"))};

    runArrayAccumulatorTest(
        ArrayAccumType::kConcatArrays,
        R"({sortBy: {a: 1}, output: {result: {$concatArrays: '$b', window: {documents: ["current", "current"]}}}})",
        docs,
        BSON_ARRAY(fromjson("{a: 1, b: [1, 2], result: [1, 2]}")
                   << fromjson("{a: 2, b: [3, 4], result: [3, 4]}")
                   << fromjson("{a: 2, b: [3, 4], result: [5, 6]}")
                   << fromjson("{a: 2, b: [3, 4], result: []}")
                   << fromjson("{a: 3, b: [5, 6], result: [5, 6]}")));
}

TEST_F(SBESetWindowFieldsTest, ConcatArraysWindowCurrentToUnbounded) {
    auto docs = std::vector<BSONArray>{BSON_ARRAY(fromjson("{a: 1, b: [1, 2]}")),
                                       BSON_ARRAY(fromjson("{a: 2, b: [3, 4]}")),
                                       BSON_ARRAY(fromjson("{a: 2, b: [5, 6]}")),
                                       BSON_ARRAY(fromjson("{a: 2}")),
                                       BSON_ARRAY(fromjson("{a: 1, b: [5, 6]}"))};

    runArrayAccumulatorTest(
        ArrayAccumType::kConcatArrays,
        R"({sortBy: {a: 1}, output: {result: {$concatArrays: '$b', window: {documents: ["current", "unbounded"]}}}})",
        docs,
        BSON_ARRAY(fromjson("{a: 1, b: [1, 2], result: [1, 2, 3, 4, 5, 6, 5, 6]}")
                   << fromjson("{a: 2, b: [3, 4], result: [3, 4, 5, 6, 5, 6]}")
                   << fromjson("{a: 2, b: [3, 4], result: [5, 6, 5, 6]}")
                   << fromjson("{a: 2, b: [3, 4], result: [5, 6]}")
                   << fromjson("{a: 3, b: [5, 6], result: [5, 6]}")));
}

TEST_F(SBESetWindowFieldsTest, ConcatArraysWindowUnboundedToCurrent) {
    auto docs = std::vector<BSONArray>{BSON_ARRAY(fromjson("{a: 1, b: [1, 2]}")),
                                       BSON_ARRAY(fromjson("{a: 2, b: [3, 4]}")),
                                       BSON_ARRAY(fromjson("{a: 2, b: [5, 6]}")),
                                       BSON_ARRAY(fromjson("{a: 2}")),
                                       BSON_ARRAY(fromjson("{a: 1, b: [5, 6]}"))};

    runArrayAccumulatorTest(
        ArrayAccumType::kConcatArrays,
        R"({sortBy: {a: 1}, output: {result: {$concatArrays: '$b', window: {documents: ["unbounded", "current"]}}}})",
        docs,
        BSON_ARRAY(fromjson("{a: 1, b: [1, 2], result: [1, 2]}")
                   << fromjson("{a: 2, b: [3, 4], result: [1, 2, 3, 4]}")
                   << fromjson("{a: 2, b: [3, 4], result: [1, 2, 3, 4, 5, 6]}")
                   << fromjson("{a: 2, b: [3, 4], result: [1, 2, 3, 4, 5, 6]}")
                   << fromjson("{a: 3, b: [5, 6], result: [1, 2, 3, 4, 5, 6, 5, 6]}")));
}

TEST_F(SBESetWindowFieldsTest, ConcatArraysWindowUnboundedToUnbounded) {
    auto docs = std::vector<BSONArray>{BSON_ARRAY(fromjson("{a: 1, b: [1, 2]}")),
                                       BSON_ARRAY(fromjson("{a: 2, b: [3, 4]}")),
                                       BSON_ARRAY(fromjson("{a: 2, b: [5, 6]}")),
                                       BSON_ARRAY(fromjson("{a: 2}")),
                                       BSON_ARRAY(fromjson("{a: 1, b: [5, 6]}"))};

    runArrayAccumulatorTest(
        ArrayAccumType::kConcatArrays,
        R"({sortBy: {a: 1}, output: {result: {$concatArrays: '$b', window: {documents: ["unbounded", "unbounded"]}}}})",
        docs,
        BSON_ARRAY(fromjson("{a: 1, b: [1, 2], result: [1, 2, 3, 4, 5, 6, 5, 6]}")
                   << fromjson("{a: 2, b: [3, 4], result: [1, 2, 3, 4, 5, 6, 5, 6]}")
                   << fromjson("{a: 2, b: [3, 4], result: [1, 2, 3, 4, 5, 6, 5, 6]}")
                   << fromjson("{a: 2, b: [3, 4], result: [1, 2, 3, 4, 5, 6, 5, 6]}")
                   << fromjson("{a: 3, b: [5, 6], result: [1, 2, 3, 4, 5, 6, 5, 6]}")));
}

TEST_F(SBESetWindowFieldsTest, ConcatArraysWindowDifferentTypes) {
    // We want to test the memory accounting for different types, so we need a second document so we
    // remove values from the window.
    auto docs = std::vector<BSONArray>{
        BSON_ARRAY(fromjson(
            "{a: 1, b: [\"a\", \"Beauty\", \"Certainly this is very large string that we will use "
            "SBE's StringBig type for\", \"MinKey\", \"MaxKey\", null, 1, Infinity, "
            "NumberDecimal(\"Infinity\"), {foo: 1, bar: 2}, [4, 5, 6], true, Date(1), /^ABC/i]}")),
        BSON_ARRAY(fromjson("{a: 2}"))};

    runArrayAccumulatorTest(
        ArrayAccumType::kConcatArrays,
        R"({sortBy: {a: 1}, output: {result: {$concatArrays: '$b', window: {documents: ["current", 0]}}}})",
        docs,
        BSON_ARRAY(
            fromjson("{a: 1, b: [1, 2], result: [\"a\", \"Beauty\", \"Certainly this is very large "
                     "string that we will use SBE's StringBig type for\", \"MinKey\", \"MaxKey\", "
                     "null, 1, Infinity, NumberDecimal(\"Infinity\"), {foo: 1, bar: 2}, [4, "
                     "5, 6], true, Date(1), /^ABC/i]}")
            << fromjson("{a: 2, result: []}")));
}

}  // namespace mongo
