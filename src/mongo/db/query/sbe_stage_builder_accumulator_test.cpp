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

#include "mongo/platform/basic.h"

#include <fmt/printf.h>

#include "mongo/db/pipeline/document_source_group.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/query/collation/collator_interface_mock.h"
#include "mongo/db/query/query_solution.h"
#include "mongo/db/query/sbe_stage_builder_test_fixture.h"
#include "mongo/unittest/unittest.h"

namespace mongo {

class SbeStageBuilderGroupTest : public SbeStageBuilderTestFixture {
protected:
    boost::intrusive_ptr<DocumentSource> createDocumentSourceGroup(
        boost::intrusive_ptr<ExpressionContext> expCtx, const BSONObj& groupSpec) {
        ASSERT(expCtx) << "expCtx must not be null";
        BSONObj namedSpec = BSON("$group" << groupSpec);
        BSONElement specElement = namedSpec.firstElement();

        auto docSrcGrp = DocumentSourceGroup::createFromBson(specElement, expCtx);
        // $group may end up being incompatible with SBE after optimize(). We call 'optimize()' to
        // reveal such cases.
        docSrcGrp->optimize();

        return docSrcGrp;
    }

    std::pair<sbe::value::TypeTags, sbe::value::Value> sortResults(sbe::value::TypeTags tag,
                                                                   sbe::value::Value val) {
        using valuePair = std::pair<sbe::value::TypeTags, sbe::value::Value>;
        std::vector<valuePair> resultsContents;
        auto resultsView = sbe::value::getArrayView(val);
        for (size_t i = 0; i < resultsView->size(); i++) {
            resultsContents.push_back(resultsView->getAt(i));
        }
        std::sort(resultsContents.begin(),
                  resultsContents.end(),
                  [](const valuePair& lhs, const valuePair& rhs) -> bool {
                      auto [lhsTag, lhsVal] = lhs;
                      auto [rhsTag, rhsVal] = rhs;
                      auto [compareTag, compareVal] =
                          sbe::value::compareValue(lhsTag, lhsVal, rhsTag, rhsVal);
                      ASSERT_EQ(compareTag, sbe::value::TypeTags::NumberInt32);
                      return sbe::value::bitcastTo<int32_t>(compareVal) < 0;
                  });

        auto [sortedResultsTag, sortedResultsVal] = sbe::value::makeNewArray();
        sbe::value::ValueGuard sortedResultsGuard{sortedResultsTag, sortedResultsVal};
        auto sortedResultsView = sbe::value::getArrayView(sortedResultsVal);
        for (auto [tag, val] : resultsContents) {
            auto [tagCopy, valCopy] = copyValue(tag, val);
            sortedResultsView->push_back(tagCopy, valCopy);
        }
        sortedResultsGuard.reset();
        return {sortedResultsTag, sortedResultsVal};
    }

    std::pair<std::unique_ptr<QuerySolution>, boost::intrusive_ptr<DocumentSourceGroup>>
    makeGroupQuerySolution(boost::intrusive_ptr<ExpressionContext> expCtx,
                           BSONObj groupSpec,
                           std::vector<BSONArray> inputDocs) {
        auto docSrc = createDocumentSourceGroup(expCtx, groupSpec);
        auto docSrcGroup = dynamic_cast<DocumentSourceGroup*>(docSrc.get());
        ASSERT(docSrcGroup != nullptr);

        // Constructs a QuerySolution consisting of a GroupNode on top of a VirtualScanNode.
        auto virtScanNode = std::make_unique<VirtualScanNode>(
            inputDocs, VirtualScanNode::ScanType::kCollScan, false /*hasRecordId*/);

        auto groupNode = std::make_unique<GroupNode>(std::move(virtScanNode),
                                                     docSrcGroup->getIdFields(),
                                                     docSrcGroup->getAccumulatedFields(),
                                                     false /*doingMerge*/);

        // Makes a QuerySolution from the root group node.
        return {makeQuerySolution(std::move(groupNode)), docSrcGroup};
    }

    std::pair<sbe::value::TypeTags, sbe::value::Value> getResultsForAggregation(
        BSONObj groupSpec,
        std::vector<BSONArray> inputDocs,
        std::unique_ptr<CollatorInterface> collator = nullptr) {
        // Makes a QuerySolution for GroupNode over VirtualScanNode.
        auto [querySolution, groupNode] = makeGroupQuerySolution(
            make_intrusive<ExpressionContextForTest>(), std::move(groupSpec), std::move(inputDocs));

        // Translates the QuerySolution tree to a sbe::PlanStage tree.
        auto [resultSlots, stage, data, _] = buildPlanStage(
            std::move(querySolution), false /*hasRecordId*/, nullptr, std::move(collator));
        ASSERT_EQ(resultSlots.size(), 1);

        auto resultAccessors = prepareTree(&data.ctx, stage.get(), resultSlots[0]);
        return getAllResults(stage.get(), &resultAccessors[0]);
    }

    void runGroupAggregationTest(StringData groupSpec,
                                 std::vector<BSONArray> inputDocs,
                                 const mongo::BSONArray& expectedValue,
                                 std::unique_ptr<CollatorInterface> collator = nullptr) {
        auto [resultsTag, resultsVal] =
            getResultsForAggregation(fromjson(groupSpec.rawData()), inputDocs, std::move(collator));
        sbe::value::ValueGuard resultGuard{resultsTag, resultsVal};

        auto [sortedResultsTag, sortedResultsVal] = sortResults(resultsTag, resultsVal);
        sbe::value::ValueGuard sortedResultGuard{sortedResultsTag, sortedResultsVal};

        auto [expectedTag, expectedVal] = stage_builder::makeValue(expectedValue);
        sbe::value::ValueGuard expectedGuard{expectedTag, expectedVal};

        ASSERT_TRUE(valueEquals(sortedResultsTag, sortedResultsVal, expectedTag, expectedVal))
            << "expected: " << std::make_pair(expectedTag, expectedVal)
            << " but got: " << std::make_pair(sortedResultsTag, sortedResultsVal);
    }

    /**
     * $addToSet doesn't guarantee any particular order of the accumulated values. To make the
     * result checking deterministic, we convert the array of the expected values and the actual
     * values into arraySets and compare the sets.
     *
     * Note: Currently, the order agnostic comparison only works for arraySets with
     * non-pointer-based accumulated values.
     */
    void runAddToSetTest(StringData groupSpec,
                         std::vector<BSONArray> inputDocs,
                         const BSONArray& expectedResult,
                         std::unique_ptr<CollatorInterface> collator = nullptr) {
        using namespace mongo::sbe::value;

        // Create ArraySet Value from the expectedResult.
        auto [tmpTag, tmpVal] =
            copyValue(TypeTags::bsonArray, bitcastFrom<const char*>(expectedResult.objdata()));
        ValueGuard tmpGuard{tmpTag, tmpVal};
        auto [expectedTag, expectedSet] = arrayToSet(tmpTag, tmpVal);
        ValueGuard expectedValueGuard{expectedTag, expectedSet};

        // Run the accumulator.
        auto [resultsTag, resultsVal] =
            getResultsForAggregation(fromjson(groupSpec.rawData()), inputDocs, std::move(collator));
        ValueGuard resultGuard{resultsTag, resultsVal};
        ASSERT_EQ(resultsTag, TypeTags::Array);

        // Extract the accumulated ArraySet from the result and compare it to the expected.
        auto arr = getArrayView(resultsVal);
        ASSERT_EQ(1, arr->size());
        auto [resObjTag, resObjVal] = arr->getAt(0);
        ASSERT_EQ(resObjTag, TypeTags::bsonObject)
            << "Expected a result object but got: " << std::make_pair(resObjTag, resObjVal);

        sbe::value::ObjectEnumerator objEnum{resObjTag, resObjVal};
        ASSERT(!objEnum.atEnd()) << "Expected a result object but got: "
                                 << std::make_pair(resObjTag, resObjVal);

        while (!objEnum.atEnd()) {
            if (objEnum.getFieldName() == "x"_sd) {
                auto [arrTag, arrVal] = objEnum.getViewOfValue();
                ASSERT_EQ(arrTag, TypeTags::bsonArray)
                    << "Expected an array for field x but got: " << std::make_pair(arrTag, arrVal);

                auto [tmpTag2, tmpVal2] = copyValue(TypeTags::bsonArray, arrVal);
                ValueGuard tmpGuard2{tmpTag2, tmpVal2};
                auto [actualTag, actualSet] = arrayToSet(tmpTag2, tmpVal2);
                ValueGuard actualValueGuard{actualTag, actualSet};

                ASSERT(valueEquals(expectedTag, expectedSet, actualTag, actualSet))
                    << "expected set: " << std::make_pair(expectedTag, expectedSet)
                    << " but got set: " << std::make_pair(actualTag, actualSet);
                return;
            }

            objEnum.advance();
        }

        ASSERT(false) << "expected: " << expectedResult
                      << " but got: " << std::make_pair(resObjTag, resObjVal);
    }

    void runSbeIncompatibleGroupSpecTest(const BSONObj& groupSpec,
                                         boost::intrusive_ptr<ExpressionContext>& expCtx) {
        expCtx->sbeCompatible = true;
        // When we parse and optimize the 'groupSpec' to build a DocumentSourceGroup, those
        // accumulation expressions or '_id' expression that are not supported by SBE will flip the
        // 'sbeCompatible()' flag in the 'groupStage' to false.
        auto docSrc = createDocumentSourceGroup(expCtx, groupSpec);
        auto groupStage = dynamic_cast<DocumentSourceGroup*>(docSrc.get());
        ASSERT(groupStage != nullptr);

        ASSERT_EQ(false, groupStage->sbeCompatible()) << "group spec: " << groupSpec;
    }

    void runSbeGroupCompatibleFlagTest(const std::vector<BSONObj>& groupSpecs,
                                       boost::intrusive_ptr<ExpressionContext>& expCtx) {
        expCtx->sbeCompatible = true;
        for (const auto& groupSpec : groupSpecs) {
            // When we parse and optimize the groupSpec to build the DocumentSourceGroup, those
            // AccumulationExpressions or _id expression that are not supported by SBE will flip the
            // sbeGroupCompatible flag in the expCtx to false.
            auto [querySolution, groupStage] =
                makeGroupQuerySolution(expCtx, groupSpec, std::vector<BSONArray>{} /*inputDocs*/);

            // We try to figure out the expected sbeGroupCompatible value here. The
            // sbeGroupCompatible flag should be false if any accumulator being tested does not have
            // a registered SBE accumulator builder function.
            auto sbeGroupCompatible = true;
            try {
                // Tries to translate the QuerySolution tree to a sbe::PlanStage tree.
                (void)buildPlanStage(
                    std::move(querySolution), false /*hasRecordId*/, nullptr, nullptr);

            } catch (const DBException& e) {
                // The accumulator or the _id expression is unsupported in SBE, so we expect that
                // the sbeCompatible flag should be false.
                ASSERT(e.code() == 5754701 || e.code() == 5851602) << "group spec: " << groupSpec;
                sbeGroupCompatible = false;
                break;
            }
            ASSERT_EQ(sbeGroupCompatible, groupStage->sbeCompatible())
                << "group spec: " << groupSpec;
        }
    }
};

double computeStdDev(const std::vector<double>& vec, bool isSamp) {
    double sum = std::accumulate(std::begin(vec), std::end(vec), 0.0);
    double mean = sum / vec.size();

    double accum = 0.0;
    std::for_each(std::begin(vec), std::end(vec), [&](const double d) {
        accum += ((d - mean) * (d - mean));
    });
    if (isSamp) {
        return sqrt(accum / (vec.size() - 1));
    }
    return sqrt(accum / vec.size());
}

TEST_F(SbeStageBuilderGroupTest, TestGroupMultipleAccumulators) {
    auto docs = std::vector<BSONArray>{BSON_ARRAY(BSON("a" << 1 << "b" << 1)),
                                       BSON_ARRAY(BSON("a" << 1 << "b" << 2)),
                                       BSON_ARRAY(BSON("a" << 2 << "b" << 3))};
    runGroupAggregationTest(R"({
                                _id: "$a",
                                sumb: {$sum: "$b"},
                                minb: {$min: "$b"},
                                maxb: {$max: "$b"},
                                firstb: {$first: "$b"},
                                lastb: {$last: "$b"}
                            })",
                            docs,
                            BSON_ARRAY(BSON("_id" << 1 << "sumb" << 3 << "minb" << 1 << "maxb" << 2
                                                  << "firstb" << 1 << "lastb" << 2)
                                       << BSON("_id" << 2 << "sumb" << 3 << "minb" << 3 << "maxb"
                                                     << 3 << "firstb" << 3 << "lastb" << 3)));
}

TEST_F(SbeStageBuilderGroupTest, AccSkippingFinalStepAfterAvg) {
    auto docs = std::vector<BSONArray>{BSON_ARRAY(BSON("a" << 1 << "b" << 1)),
                                       BSON_ARRAY(BSON("a" << 1 << "b" << 2)),
                                       BSON_ARRAY(BSON("a" << 2 << "b" << 3))};
    runGroupAggregationTest(R"({_id: null, x: {$avg: "$b"}, y: {$last: "$b"}})",
                            docs,
                            BSON_ARRAY(BSON("_id" << BSONNULL << "x" << 6 / 3.0 << "y" << 3)));
    // An accumulator skipping the final step after multiple $avg.
    runGroupAggregationTest(
        R"({_id: null, x: {$avg: "$b"}, y: {$avg: "$b"}, z: {$last: "$b"}})",
        docs,
        BSON_ARRAY(BSON("_id" << BSONNULL << "x" << 6 / 3.0 << "y" << 6 / 3.0 << "z" << 3)));
}

TEST_F(SbeStageBuilderGroupTest, NullForMissingGroupBySlot) {
    auto docs = std::vector<BSONArray>{BSON_ARRAY(BSON("a" << 1 << "b" << 1)),
                                       BSON_ARRAY(BSON("a" << 2 << "b" << 3))};
    // _id: null is returned if the group-by field is missing.
    runGroupAggregationTest(
        R"({_id: "$z", x: {$first: "$b"}})", docs, BSON_ARRAY(BSON("_id" << BSONNULL << "x" << 1)));
}

TEST_F(SbeStageBuilderGroupTest, OneNullForMissingAndNullGroupBySlot) {
    auto docs = std::vector<BSONArray>{BSON_ARRAY(BSON("a" << BSONNULL << "b" << 1)),
                                       BSON_ARRAY(BSON("b" << 3))};
    // One _id: null document is returned when there exist null and a value is missing for the _id
    // field path.
    runGroupAggregationTest(
        R"({_id: "$a", x: {$first: "$b"}})", docs, BSON_ARRAY(BSON("_id" << BSONNULL << "x" << 1)));
}

TEST_F(SbeStageBuilderGroupTest, TestGroupNoAccumulators) {
    auto docs = std::vector<BSONArray>{BSON_ARRAY(BSON("a" << 1 << "b" << 1)),
                                       BSON_ARRAY(BSON("a" << 1 << "b" << 2)),
                                       BSON_ARRAY(BSON("a" << 2 << "b" << 3))};
    runGroupAggregationTest(
        R"({_id: "$a"})", docs, BSON_ARRAY(BSON("_id" << 1) << BSON("_id" << 2)));
}

TEST_F(SbeStageBuilderGroupTest, MinAccumulatorTranslationBasic) {
    auto docs = std::vector<BSONArray>{BSON_ARRAY(BSON("a" << 1 << "b" << 100ll)),
                                       BSON_ARRAY(BSON("a" << 1 << "b" << Decimal128(10.0))),
                                       BSON_ARRAY(BSON("a" << 1 << "b" << 1.0))};
    runGroupAggregationTest(
        "{_id: null, x: {$min: '$b'}}", docs, BSON_ARRAY(BSON("_id" << BSONNULL << "x" << 1.0)));
}

TEST_F(SbeStageBuilderGroupTest, MinAccumulatorTranslationAllUndefined) {
    auto docs = std::vector<BSONArray>{BSON_ARRAY(BSON("a" << 1 << "b" << BSONUndefined))};
    runGroupAggregationTest("{_id: null, x: {$min: '$b'}}",
                            docs,
                            BSON_ARRAY(BSON("_id" << BSONNULL << "x" << BSONNULL)));
}

TEST_F(SbeStageBuilderGroupTest, MinAccumulatorTranslationSomeUndefined) {
    auto docs = std::vector<BSONArray>{BSON_ARRAY(BSON("a" << 1 << "b" << BSONUndefined)),
                                       BSON_ARRAY(BSON("a" << 1 << "b" << 1))};
    runGroupAggregationTest(
        "{_id: null, x: {$min: '$b'}}", docs, BSON_ARRAY(BSON("_id" << BSONNULL << "x" << 1)));
}

TEST_F(SbeStageBuilderGroupTest, MinAccumulatorTranslationSomeNull) {
    auto docs = std::vector<BSONArray>{BSON_ARRAY(BSON("a" << 1 << "b" << BSONNULL)),
                                       BSON_ARRAY(BSON("a" << 1 << "b" << 10)),
                                       BSON_ARRAY(BSON("a" << 1 << "b" << 1)),
                                       BSON_ARRAY(BSON("a" << 1 << "b" << BSONNULL))};
    runGroupAggregationTest(
        "{_id: null, x: {$min: '$b'}}", docs, BSON_ARRAY(BSON("_id" << BSONNULL << "x" << 1)));
}

TEST_F(SbeStageBuilderGroupTest, MinAccumulatorTranslationAllNull) {
    auto docs = std::vector<BSONArray>{BSON_ARRAY(BSON("a" << 1 << "b" << BSONNULL)),
                                       BSON_ARRAY(BSON("a" << 1 << "b" << BSONNULL)),
                                       BSON_ARRAY(BSON("a" << 1 << "b" << BSONNULL))};
    runGroupAggregationTest("{_id: null, x: {$min: '$b'}}",
                            docs,
                            BSON_ARRAY(BSON("_id" << BSONNULL << "x" << BSONNULL)));
}

TEST_F(SbeStageBuilderGroupTest, MinAccumulatorTranslationAllMissingFields) {
    auto docs = std::vector<BSONArray>{BSON_ARRAY(BSON("a" << 1))};
    runGroupAggregationTest("{_id: null, x: {$min: '$b'}}",
                            docs,
                            BSON_ARRAY(BSON("_id" << BSONNULL << "x" << BSONNULL)));
}

TEST_F(SbeStageBuilderGroupTest, MinAccumulatorTranslationSomeMissingFields) {
    auto docs = std::vector<BSONArray>{BSON_ARRAY(BSON("a" << 1)),
                                       BSON_ARRAY(BSON("a" << 1 << "b" << 1)),
                                       BSON_ARRAY(BSON("a" << 1 << "b" << 100))};
    runGroupAggregationTest(
        "{_id: null, x: {$min: '$b'}}", docs, BSON_ARRAY(BSON("_id" << BSONNULL << "x" << 1)));
}

TEST_F(SbeStageBuilderGroupTest, MinAccumulatorTranslationMixedTypes) {
    auto docs = std::vector<BSONArray>{BSON_ARRAY(BSON("a" << 1 << "b" << BSON_ARRAY(1 << 2))),
                                       BSON_ARRAY(BSON("a" << 1 << "b" << 1)),
                                       BSON_ARRAY(BSON("a" << 1 << "b" << BSON("c" << 1)))};
    runGroupAggregationTest(
        "{_id: null, x: {$min: '$b'}}", docs, BSON_ARRAY(BSON("_id" << BSONNULL << "x" << 1)));
}

TEST_F(SbeStageBuilderGroupTest, MinAccumulatorTranslationOneGroupBy) {
    auto docs = std::vector<BSONArray>{BSON_ARRAY(BSON("a" << 1 << "b" << 0)),
                                       BSON_ARRAY(BSON("a" << 1 << "b" << 100)),
                                       BSON_ARRAY(BSON("a" << 2 << "b" << 1))};
    runGroupAggregationTest(
        "{_id: '$a', x: {$min: '$b'}}",
        docs,
        BSON_ARRAY(BSON("_id" << 1 << "x" << 0) << BSON("_id" << 2 << "x" << 1)));
}

TEST_F(SbeStageBuilderGroupTest, MinAccumulatorTranslationStringsDefaultCollator) {
    auto docs = std::vector<BSONArray>{BSON_ARRAY(BSON("a" << 1 << "b"
                                                           << "az")),
                                       BSON_ARRAY(BSON("a" << 1 << "b"
                                                           << "za"))};
    runGroupAggregationTest("{_id: null, x: {$min: '$b'}}",
                            docs,
                            BSON_ARRAY(BSON("_id" << BSONNULL << "x"
                                                  << "az")));
}

TEST_F(SbeStageBuilderGroupTest, MinAccumulatorTranslationStringsReverseStringCollator) {
    auto docs = std::vector<BSONArray>{BSON_ARRAY(BSON("a" << 1 << "b"
                                                           << "az")),
                                       BSON_ARRAY(BSON("a" << 1 << "b"
                                                           << "za"))};
    runGroupAggregationTest(
        "{_id: null, x: {$min: '$b'}}",
        docs,
        BSON_ARRAY(BSON("_id" << BSONNULL << "x"
                              << "za")),
        std::make_unique<CollatorInterfaceMock>(CollatorInterfaceMock::MockType::kReverseString));
}

TEST_F(SbeStageBuilderGroupTest, MinAccumulatorTranslationNaN) {
    auto docs = std::vector<BSONArray>{
        BSON_ARRAY(BSON("a" << 1 << "b" << 42ll)),
        BSON_ARRAY(BSON("a" << 1 << "b" << std::numeric_limits<double>::quiet_NaN()))};
    // The collator doesn't affect comparison of numbers but until SERVER-61868 just providing a
    // collator used to trigger a different codepath, so let's throw it in for a good measure.
    runGroupAggregationTest(
        "{_id: null, x: {$min: '$b'}}",
        docs,
        BSON_ARRAY(BSON("_id" << BSONNULL << "x" << std::numeric_limits<double>::quiet_NaN())),
        std::make_unique<CollatorInterfaceMock>(CollatorInterfaceMock::MockType::kReverseString));
}

TEST_F(SbeStageBuilderGroupTest, MaxAccumulatorTranslationBasic) {
    auto docs = std::vector<BSONArray>{BSON_ARRAY(BSON("a" << 1 << "b" << 100ll)),
                                       BSON_ARRAY(BSON("a" << 1 << "b" << Decimal128(10.0))),
                                       BSON_ARRAY(BSON("a" << 1 << "b" << 1.0))};
    runGroupAggregationTest(
        "{_id: null, x: {$max: '$b'}}", docs, BSON_ARRAY(BSON("_id" << BSONNULL << "x" << 100)));
}

TEST_F(SbeStageBuilderGroupTest, MaxAccumulatorTranslationSomeNull) {
    auto docs = std::vector<BSONArray>{BSON_ARRAY(BSON("a" << 1 << "b" << BSONNULL)),
                                       BSON_ARRAY(BSON("a" << 1 << "b" << 10)),
                                       BSON_ARRAY(BSON("a" << 1 << "b" << 1)),
                                       BSON_ARRAY(BSON("a" << 1 << "b" << BSONNULL))};
    runGroupAggregationTest(
        "{_id: null, x: {$max: '$b'}}", docs, BSON_ARRAY(BSON("_id" << BSONNULL << "x" << 10)));
}

TEST_F(SbeStageBuilderGroupTest, MaxAccumulatorTranslationAllNull) {
    auto docs = std::vector<BSONArray>{BSON_ARRAY(BSON("a" << 1 << "b" << BSONNULL)),
                                       BSON_ARRAY(BSON("a" << 1 << "b" << BSONNULL)),
                                       BSON_ARRAY(BSON("a" << 1 << "b" << BSONNULL))};
    runGroupAggregationTest("{_id: null, x: {$max: '$b'}}",
                            docs,
                            BSON_ARRAY(BSON("_id" << BSONNULL << "x" << BSONNULL)));
}

TEST_F(SbeStageBuilderGroupTest, MaxAccumulatorTranslationAllMissingFields) {
    auto docs = std::vector<BSONArray>{BSON_ARRAY(BSON("a" << 1))};
    runGroupAggregationTest("{_id: null, x: {$max: '$b'}}",
                            docs,
                            BSON_ARRAY(BSON("_id" << BSONNULL << "x" << BSONNULL)));
}

TEST_F(SbeStageBuilderGroupTest, MaxAccumulatorTranslationSomeMissingFields) {
    auto docs = std::vector<BSONArray>{BSON_ARRAY(BSON("a" << 1)),
                                       BSON_ARRAY(BSON("a" << 1 << "b" << 1)),
                                       BSON_ARRAY(BSON("a" << 1 << "b" << 100))};
    runGroupAggregationTest(
        "{_id: null, x: {$max: '$b'}}", docs, BSON_ARRAY(BSON("_id" << BSONNULL << "x" << 100)));
}

TEST_F(SbeStageBuilderGroupTest, MaxAccumulatorTranslationMixedTypes) {
    auto docs = std::vector<BSONArray>{BSON_ARRAY(BSON("a" << 1 << "b" << BSON_ARRAY(1 << 2))),
                                       BSON_ARRAY(BSON("a" << 1 << "b" << 1)),
                                       BSON_ARRAY(BSON("a" << 1 << "b" << BSON("c" << 1)))};
    runGroupAggregationTest("{_id: null, x: {$max: '$b'}}",
                            docs,
                            BSON_ARRAY(BSON("_id" << BSONNULL << "x" << BSON_ARRAY(1 << 2))));
}

TEST_F(SbeStageBuilderGroupTest, MaxAccumulatorTranslationOneGroupBy) {
    auto docs = std::vector<BSONArray>{BSON_ARRAY(BSON("a" << 1 << "b" << 0)),
                                       BSON_ARRAY(BSON("a" << 1 << "b" << 100)),
                                       BSON_ARRAY(BSON("a" << 2 << "b" << 1))};
    runGroupAggregationTest(
        "{_id: '$a', x: {$max: '$b'}}",
        docs,
        BSON_ARRAY(BSON("_id" << 1 << "x" << 100) << BSON("_id" << 2 << "x" << 1)));
}

TEST_F(SbeStageBuilderGroupTest, MaxAccumulatorTranslationStringsDefaultCollator) {
    auto docs = std::vector<BSONArray>{BSON_ARRAY(BSON("a" << 1 << "b"
                                                           << "az")),
                                       BSON_ARRAY(BSON("a" << 1 << "b"
                                                           << "za"))};
    runGroupAggregationTest("{_id: null, x: {$max: '$b'}}",
                            docs,
                            BSON_ARRAY(BSON("_id" << BSONNULL << "x"
                                                  << "za")));
}

TEST_F(SbeStageBuilderGroupTest, MaxAccumulatorTranslationStringsReverseStringCollator) {
    auto docs = std::vector<BSONArray>{BSON_ARRAY(BSON("a" << 1 << "b"
                                                           << "az")),
                                       BSON_ARRAY(BSON("a" << 1 << "b"
                                                           << "za"))};
    runGroupAggregationTest(
        "{_id: null, x: {$max: '$b'}}",
        docs,
        BSON_ARRAY(BSON("_id" << BSONNULL << "x"
                              << "az")),
        std::make_unique<CollatorInterfaceMock>(CollatorInterfaceMock::MockType::kReverseString));
}

TEST_F(SbeStageBuilderGroupTest, MaxAccumulatorTranslationNaN) {
    auto docs = std::vector<BSONArray>{
        BSON_ARRAY(BSON("a" << 1 << "b" << 42ll)),
        BSON_ARRAY(BSON("a" << 1 << "b" << std::numeric_limits<double>::quiet_NaN()))};
    // The collator doesn't affect comparison of numbers but until SERVER-61868 just providing a
    // collator used to trigger a different codepath, so let's throw it in for a good measure.
    runGroupAggregationTest(
        "{_id: null, x: {$max: '$b'}}",
        docs,
        BSON_ARRAY(BSON("_id" << BSONNULL << "x" << 42ll)),
        std::make_unique<CollatorInterfaceMock>(CollatorInterfaceMock::MockType::kReverseString));
}

TEST_F(SbeStageBuilderGroupTest, FirstAccumulatorTranslationOneDoc) {
    auto docs = std::vector<BSONArray>{BSON_ARRAY(BSON("a" << 1 << "b" << BSON_ARRAY(1 << 2)))};
    runGroupAggregationTest("{_id: null, x: {$first: '$b'}}",
                            docs,
                            BSON_ARRAY(BSON("_id" << BSONNULL << "x" << BSON_ARRAY(1 << 2))));
}

TEST_F(SbeStageBuilderGroupTest, FirstAccumulatorTranslationMissingField) {
    auto docs = std::vector<BSONArray>{BSON_ARRAY(BSON("a" << 1))};
    runGroupAggregationTest("{_id: null, x: {$first: '$b'}}",
                            docs,
                            BSON_ARRAY(BSON("_id" << BSONNULL << "x" << BSONNULL)));
}

TEST_F(SbeStageBuilderGroupTest, FirstAccumulatorTranslationBasic) {
    auto docs = std::vector<BSONArray>{BSON_ARRAY(BSON("a" << 1 << "b" << 100)),
                                       BSON_ARRAY(BSON("a" << 1 << "b" << 0))};
    runGroupAggregationTest(
        "{_id: null, x: {$first: '$b'}}", docs, BSON_ARRAY(BSON("_id" << BSONNULL << "x" << 100)));
}

TEST_F(SbeStageBuilderGroupTest, FirstAccumulatorTranslationFirstDocWithMissingField) {
    auto docs =
        std::vector<BSONArray>{BSON_ARRAY(BSON("a" << 1)), BSON_ARRAY(BSON("a" << 1 << "b" << 0))};
    runGroupAggregationTest("{_id: null, x: {$first: '$b'}}",
                            docs,
                            BSON_ARRAY(BSON("_id" << BSONNULL << "x" << BSONNULL)));
}

TEST_F(SbeStageBuilderGroupTest, FirstAccumulatorTranslationOneGroupBy) {
    auto docs = std::vector<BSONArray>{BSON_ARRAY(BSON("a" << 1 << "b" << 0)),
                                       BSON_ARRAY(BSON("a" << 1 << "b" << 100)),
                                       BSON_ARRAY(BSON("a" << 2 << "b" << 2.5))};
    runGroupAggregationTest(
        "{_id: '$a', x: {$first: '$b'}}",
        docs,
        BSON_ARRAY(BSON("_id" << 1 << "x" << 0) << BSON("_id" << 2 << "x" << 2.5)));
}

TEST_F(SbeStageBuilderGroupTest, LastAccumulatorTranslationOneDoc) {
    auto docs = std::vector<BSONArray>{BSON_ARRAY(BSON("a" << 1 << "b" << BSON_ARRAY(1 << 2)))};
    runGroupAggregationTest("{_id: null, x: {$last: '$b'}}",
                            docs,
                            BSON_ARRAY(BSON("_id" << BSONNULL << "x" << BSON_ARRAY(1 << 2))));
}

TEST_F(SbeStageBuilderGroupTest, LastAccumulatorTranslationMissingField) {
    auto docs = std::vector<BSONArray>{BSON_ARRAY(BSON("a" << 1))};
    runGroupAggregationTest("{_id: null, x: {$last: '$b'}}",
                            docs,
                            BSON_ARRAY(BSON("_id" << BSONNULL << "x" << BSONNULL)));
}

TEST_F(SbeStageBuilderGroupTest, LastAccumulatorTranslationBasic) {
    auto docs = std::vector<BSONArray>{BSON_ARRAY(BSON("a" << 1 << "b" << 100)),
                                       BSON_ARRAY(BSON("a" << 1 << "b" << 0))};
    runGroupAggregationTest(
        "{_id: null, x: {$last: '$b'}}", docs, BSON_ARRAY(BSON("_id" << BSONNULL << "x" << 0)));
}

TEST_F(SbeStageBuilderGroupTest, LastAccumulatorTranslationLastDocWithMissingField) {
    auto docs =
        std::vector<BSONArray>{BSON_ARRAY(BSON("a" << 1 << "b" << 0)), BSON_ARRAY(BSON("a" << 1))};
    runGroupAggregationTest("{_id: null, x: {$last: '$b'}}",
                            docs,
                            BSON_ARRAY(BSON("_id" << BSONNULL << "x" << BSONNULL)));
}

TEST_F(SbeStageBuilderGroupTest, LastAccumulatorTranslationOneGroupBy) {
    auto docs = std::vector<BSONArray>{BSON_ARRAY(BSON("a" << 1 << "b" << 0)),
                                       BSON_ARRAY(BSON("a" << 1 << "b" << 100)),
                                       BSON_ARRAY(BSON("a" << 2 << "b" << 2.5))};
    runGroupAggregationTest(
        "{_id: '$a', x: {$last: '$b'}}",
        docs,
        BSON_ARRAY(BSON("_id" << 1 << "x" << 100) << BSON("_id" << 2 << "x" << 2.5)));
}

TEST_F(SbeStageBuilderGroupTest, SumAccumulatorTranslationBasic) {
    auto docs = std::vector<BSONArray>{BSON_ARRAY(BSON("a" << 1 << "b" << 2)),
                                       BSON_ARRAY(BSON("a" << 1 << "b" << 4)),
                                       BSON_ARRAY(BSON("a" << 1 << "b" << 6))};
    runGroupAggregationTest(
        "{_id: null, x: {$sum: '$b'}}", docs, BSON_ARRAY(BSON("_id" << BSONNULL << "x" << 12)));
}

TEST_F(SbeStageBuilderGroupTest, SumAccumulatorTranslationOneDocInt) {
    runGroupAggregationTest("{_id: null, x: {$sum: '$b'}}",
                            {BSON_ARRAY(BSON("a" << 1 << "b" << 10))},
                            BSON_ARRAY(BSON("_id" << BSONNULL << "x" << 10)));
}

TEST_F(SbeStageBuilderGroupTest, SumAccumulatorTranslationOneDocLong) {
    runGroupAggregationTest("{_id: null, x: {$sum: '$b'}}",
                            std::vector<BSONArray>{BSON_ARRAY(BSON("a" << 3 << "b" << 10ll))},
                            BSON_ARRAY(BSON("_id" << BSONNULL << "x" << 10ll)));
}

TEST_F(SbeStageBuilderGroupTest, SumAccumulatorTranslationOneDocIntRepresentableDouble) {
    runGroupAggregationTest("{_id: null, x: {$sum: '$b'}}",
                            std::vector<BSONArray>{BSON_ARRAY(BSON("a" << 4 << "b" << 10.0))},
                            BSON_ARRAY(BSON("_id" << BSONNULL << "x" << 10.0)));
}

TEST_F(SbeStageBuilderGroupTest, SumAccumulatorTranslationOneDocNonIntRepresentableLong) {
    runGroupAggregationTest(
        "{_id: null, x: {$sum: '$b'}}",
        std::vector<BSONArray>{BSON_ARRAY(BSON("a" << 5 << "b" << 60000000000ll))},
        BSON_ARRAY(BSON("_id" << BSONNULL << "x" << 60000000000ll)));
}

TEST_F(SbeStageBuilderGroupTest, SumAccumulatorTranslationNonIntegerValuedDouble) {
    runGroupAggregationTest("{_id: null, x: {$sum: '$b'}}",
                            std::vector<BSONArray>{BSON_ARRAY(BSON("a" << 1 << "b" << 9.5))},
                            BSON_ARRAY(BSON("_id" << BSONNULL << "x" << 9.5)));
}

TEST_F(SbeStageBuilderGroupTest, SumAccumulatorTranslationOneDocNanDouble) {
    runGroupAggregationTest(
        "{_id: null, x: {$sum: '$b'}}",
        std::vector<BSONArray>{
            BSON_ARRAY(BSON("a" << 6 << "b" << std::numeric_limits<double>::quiet_NaN()))},
        BSON_ARRAY(BSON("_id" << BSONNULL << "x" << std::numeric_limits<double>::quiet_NaN())));
}

TEST_F(SbeStageBuilderGroupTest, SumAccumulatorTranslationIntAndLong) {
    auto docs = std::vector<BSONArray>{BSON_ARRAY(BSON("a" << 1 << "b" << 4)),
                                       BSON_ARRAY(BSON("a" << 1 << "b" << 5ll))};
    runGroupAggregationTest(
        "{_id: null, x: {$sum: '$b'}}", docs, BSON_ARRAY(BSON("_id" << BSONNULL << "x" << 9ll)));
}

TEST_F(SbeStageBuilderGroupTest, SumAccumulatorTranslationIntAndDouble) {
    auto docs = std::vector<BSONArray>{BSON_ARRAY(BSON("a" << 1 << "b" << 4)),
                                       BSON_ARRAY(BSON("a" << 1 << "b" << 5.5))};
    runGroupAggregationTest(
        "{_id: null, x: {$sum: '$b'}}", docs, BSON_ARRAY(BSON("_id" << BSONNULL << "x" << 9.5)));
}

TEST_F(SbeStageBuilderGroupTest, SumAccumulatorTranslationLongAndDouble) {
    auto docs = std::vector<BSONArray>{BSON_ARRAY(BSON("a" << 1 << "b" << 4ll)),
                                       BSON_ARRAY(BSON("a" << 1 << "b" << 5.5))};
    runGroupAggregationTest(
        "{_id: null, x: {$sum: '$b'}}", docs, BSON_ARRAY(BSON("_id" << BSONNULL << "x" << 9.5)));
}

TEST_F(SbeStageBuilderGroupTest, SumAccumulatorTranslationIntLongDouble) {
    auto docs = std::vector<BSONArray>{BSON_ARRAY(BSON("a" << 1 << "b" << 4)),
                                       BSON_ARRAY(BSON("a" << 1 << "b" << 4ll)),
                                       BSON_ARRAY(BSON("a" << 1 << "b" << 5.5))};
    runGroupAggregationTest(
        "{_id: null, x: {$sum: '$b'}}", docs, BSON_ARRAY(BSON("_id" << BSONNULL << "x" << 13.5)));
}

TEST_F(SbeStageBuilderGroupTest, SumAccumulatorTranslationLongLongDecimal) {
    auto docs = std::vector<BSONArray>{BSON_ARRAY(BSON("a" << 1 << "b" << 4ll)),
                                       BSON_ARRAY(BSON("a" << 1 << "b" << 4ll)),
                                       BSON_ARRAY(BSON("a" << 1 << "b" << Decimal128("5.5")))};
    runGroupAggregationTest("{_id: null, x: {$sum: '$b'}}",
                            docs,
                            BSON_ARRAY(BSON("_id" << BSONNULL << "x" << Decimal128("13.5"))));
}

TEST_F(SbeStageBuilderGroupTest, SumAccumulatorTranslationDoubleAndDecimal) {
    const auto doubleVal = 4.2;
    DoubleDoubleSummation doubleDoubleSum;
    doubleDoubleSum.addDouble(doubleVal);
    const auto nonDecimal = doubleDoubleSum.getDecimal();

    const auto decimalVal = Decimal128("5.5");
    const auto expected = decimalVal.add(nonDecimal);

    auto docs = std::vector<BSONArray>{BSON_ARRAY(BSON("a" << 1 << "b" << doubleVal)),
                                       BSON_ARRAY(BSON("a" << 1 << "b" << decimalVal))};
    runGroupAggregationTest("{_id: null, x: {$sum: '$b'}}",
                            docs,
                            BSON_ARRAY(BSON("_id" << BSONNULL << "x" << expected)));
}

TEST_F(SbeStageBuilderGroupTest, SumAccumulatorTranslationIntAndMissing) {
    auto docs =
        std::vector<BSONArray>{BSON_ARRAY(BSON("a" << 1 << "b" << 4)), BSON_ARRAY(BSON("a" << 1))};
    runGroupAggregationTest(
        "{_id: null, x: {$sum: '$b'}}", docs, BSON_ARRAY(BSON("_id" << BSONNULL << "x" << 4)));
}

TEST_F(SbeStageBuilderGroupTest, SumAccumulatorTranslationMixedTypesWithDecimal128) {
    const auto doubleVal1 = 4.8;
    const auto doubleVal2 = -5.0;
    const auto intVal = 6;
    const auto longVal = 3ll;

    DoubleDoubleSummation doubleDoubleSum;
    doubleDoubleSum.addDouble(doubleVal1);
    doubleDoubleSum.addInt(intVal);
    doubleDoubleSum.addLong(longVal);
    doubleDoubleSum.addDouble(doubleVal2);

    const auto decimalVal = Decimal128(1.0);
    const auto expected = decimalVal.add(doubleDoubleSum.getDecimal());

    auto docs = std::vector<BSONArray>{BSON_ARRAY(BSON("a" << 1 << "b" << BSON_ARRAY(2 << 4 << 6))),
                                       BSON_ARRAY(BSON("a" << 2 << "b" << doubleVal1)),
                                       BSON_ARRAY(BSON("a" << 2)),
                                       BSON_ARRAY(BSON("a" << 3 << "b" << intVal)),
                                       BSON_ARRAY(BSON("a" << 3 << "b" << longVal)),
                                       BSON_ARRAY(BSON("a" << 3 << "b" << decimalVal)),
                                       BSON_ARRAY(BSON("a" << 3 << "b" << doubleVal2)),
                                       BSON_ARRAY(BSON("a" << 3 << "b" << BSONNULL)),
                                       BSON_ARRAY(BSON("a" << 3 << "b" << BSON("c" << 1)))};
    runGroupAggregationTest("{_id: null, x: {$sum: '$b'}}",
                            docs,
                            BSON_ARRAY(BSON("_id" << BSONNULL << "x" << expected)));
}

TEST_F(SbeStageBuilderGroupTest, SumAccumulatorTranslationDecimalSum) {
    auto docs = std::vector<BSONArray>{BSON_ARRAY(BSON("a" << 1 << "b" << Decimal128("-10.100"))),
                                       BSON_ARRAY(BSON("a" << 1 << "b" << Decimal128("20.200")))};
    runGroupAggregationTest("{_id: null, x: {$sum: '$b'}}",
                            docs,
                            BSON_ARRAY(BSON("_id" << BSONNULL << "x" << Decimal128("10.100"))));
}

TEST_F(SbeStageBuilderGroupTest, SumAccumulatorTranslationAllNull) {
    auto docs = std::vector<BSONArray>{BSON_ARRAY(BSON("a" << 1 << "b" << BSONNULL)),
                                       BSON_ARRAY(BSON("a" << 3 << "b" << BSONNULL)),
                                       BSON_ARRAY(BSON("a" << 3 << "b" << BSONNULL))};
    runGroupAggregationTest(
        "{_id: null, x: {$sum: '$b'}}", docs, BSON_ARRAY(BSON("_id" << BSONNULL << "x" << 0)));
}

TEST_F(SbeStageBuilderGroupTest, SumAccumulatorTranslationAllMissing) {
    auto docs = std::vector<BSONArray>{BSON_ARRAY(BSON("a" << 1)), BSON_ARRAY(BSON("a" << 1))};
    runGroupAggregationTest(
        "{_id: null, x: {$sum: '$b'}}", docs, BSON_ARRAY(BSON("_id" << BSONNULL << "x" << 0)));
}

TEST_F(SbeStageBuilderGroupTest, SumAccumulatorTranslationSomeNonNumeric) {
    auto docs = std::vector<BSONArray>{BSON_ARRAY(BSON("a" << 1 << "b"
                                                           << "c")),
                                       BSON_ARRAY(BSON("a" << 1 << "b" << 3)),
                                       BSON_ARRAY(BSON("a" << 1 << "b"
                                                           << "m")),
                                       BSON_ARRAY(BSON("a" << 1 << "b" << 4))};
    runGroupAggregationTest(
        "{_id: null, x: {$sum: '$b'}}", docs, BSON_ARRAY(BSON("_id" << BSONNULL << "x" << 7)));
}

TEST_F(SbeStageBuilderGroupTest, SumAccumulatorTranslationTwoIntsDoNotOverflow) {
    auto docs =
        std::vector<BSONArray>{BSON_ARRAY(BSON("a" << 1 << "b" << std::numeric_limits<int>::max())),
                               BSON_ARRAY(BSON("a" << 1 << "b" << 10))};
    runGroupAggregationTest(
        "{_id: null, x: {$sum: '$b'}}",
        docs,
        BSON_ARRAY(BSON("_id" << BSONNULL << "x" << std::numeric_limits<int>::max() + 10LL)));
}

TEST_F(SbeStageBuilderGroupTest, SumAccumulatorTranslationTwoNegativeIntsDoNotOverflow) {
    auto docs =
        std::vector<BSONArray>{BSON_ARRAY(BSON("a" << 1 << "b" << std::numeric_limits<int>::min())),
                               BSON_ARRAY(BSON("a" << 1 << "b" << -10))};
    // We need -10LL to cast MIN_INT to a long long. This simulates upconversion when int overflows.
    runGroupAggregationTest(
        "{_id: null, x: {$sum: '$b'}}",
        docs,
        BSON_ARRAY(BSON("_id" << BSONNULL << "x" << std::numeric_limits<int>::min() - 10LL)));
}

TEST_F(SbeStageBuilderGroupTest, SumAccumulatorTranslationIntAndLongDoNotTriggerIntOverflow) {
    auto docs =
        std::vector<BSONArray>{BSON_ARRAY(BSON("a" << 1 << "b" << std::numeric_limits<int>::max())),
                               BSON_ARRAY(BSON("a" << 1 << "b" << 1LL))};
    runGroupAggregationTest(
        "{_id: null, x: {$sum: '$b'}}",
        docs,
        BSON_ARRAY(BSON("_id" << BSONNULL << "x" << std::numeric_limits<int>::max() + 1LL)));
}

TEST_F(SbeStageBuilderGroupTest, SumAccumulatorTranslationIntAndDoubleDoNotTriggerOverflow) {
    auto docs =
        std::vector<BSONArray>{BSON_ARRAY(BSON("a" << 1 << "b" << std::numeric_limits<int>::max())),
                               BSON_ARRAY(BSON("a" << 1 << "b" << 1.0))};
    runGroupAggregationTest(
        "{_id: null, x: {$sum: '$b'}}",
        docs,
        BSON_ARRAY(BSON("_id" << BSONNULL << "x" << std::numeric_limits<int>::max() + 1.0)));
}

TEST_F(SbeStageBuilderGroupTest, SumAccumulatorTranslationIntAndLongOverflowIntoDouble) {
    auto docs = std::vector<BSONArray>{
        BSON_ARRAY(BSON("a" << 1 << "b" << std::numeric_limits<long long>::max())),
        BSON_ARRAY(BSON("a" << 1 << "b" << 1))};
    runGroupAggregationTest(
        "{_id: null, x: {$sum: '$b'}}",
        docs,
        BSON_ARRAY(BSON("_id" << BSONNULL << "x"
                              << static_cast<double>(std::numeric_limits<long long>::max()) + 1)));
}

TEST_F(SbeStageBuilderGroupTest, SumAccumulatorTranslationTwoLongsOverflowIntoDouble) {
    auto docs = std::vector<BSONArray>{
        BSON_ARRAY(BSON("a" << 1 << "b" << std::numeric_limits<long long>::max())),
        BSON_ARRAY(BSON("a" << 1 << "b" << std::numeric_limits<long long>::max()))};
    runGroupAggregationTest(
        "{_id: null, x: {$sum: '$b'}}",
        docs,
        BSON_ARRAY(BSON("_id" << BSONNULL << "x"
                              << static_cast<double>(std::numeric_limits<long long>::max()) * 2)));
}

TEST_F(SbeStageBuilderGroupTest, SumAccumulatorTranslationLongAndDoubleDoNotTriggerLongOverflow) {
    auto docs = std::vector<BSONArray>{
        BSON_ARRAY(BSON("a" << 1 << "b" << std::numeric_limits<long long>::max())),
        BSON_ARRAY(BSON("a" << 1 << "b" << 1.0))};
    runGroupAggregationTest(
        "{_id: null, x: {$sum: '$b'}}",
        docs,
        BSON_ARRAY(
            BSON("_id" << BSONNULL << "x"
                       << static_cast<double>(std::numeric_limits<long long>::max()) + 1.0)));
}

TEST_F(SbeStageBuilderGroupTest, SumAccumulatorTranslationTwoDoublesOverflowToInfinity) {
    auto docs = std::vector<BSONArray>{
        BSON_ARRAY(BSON("a" << 1 << "b" << std::numeric_limits<double>::max())),
        BSON_ARRAY(BSON("a" << 1 << "b" << std::numeric_limits<double>::max()))};
    runGroupAggregationTest(
        "{_id: null, x: {$sum: '$b'}}",
        docs,
        BSON_ARRAY(BSON("_id" << BSONNULL << "x" << std::numeric_limits<double>::infinity())));
}

TEST_F(SbeStageBuilderGroupTest, SumAccumulatorTranslationTwoIntegersDoNotOverflowIfDoubleAdded) {
    auto docs = std::vector<BSONArray>{
        BSON_ARRAY(BSON("a" << 1 << "b" << std::numeric_limits<long long>::max())),
        BSON_ARRAY(BSON("a" << 1 << "b" << std::numeric_limits<long long>::max())),
        BSON_ARRAY(BSON("a" << 1 << "b" << 1.0))};
    runGroupAggregationTest(
        "{_id: null, x: {$sum: '$b'}}",
        docs,
        BSON_ARRAY(BSON("_id" << BSONNULL << "x"
                              << static_cast<double>(std::numeric_limits<long long>::max()) +
                            static_cast<double>(std::numeric_limits<long long>::max()) + 1.0)));
}

TEST_F(SbeStageBuilderGroupTest, SumAccumulatorTranslationIntAndNanDouble) {
    auto docs = std::vector<BSONArray>{
        BSON_ARRAY(BSON("a" << 1 << "b" << 3)),
        BSON_ARRAY(BSON("a" << 1 << "b" << std::numeric_limits<double>::quiet_NaN()))};
    runGroupAggregationTest(
        "{_id: null, x: {$sum: '$b'}}",
        docs,
        BSON_ARRAY(BSON("_id" << BSONNULL << "x" << std::numeric_limits<double>::quiet_NaN())));
}

TEST_F(SbeStageBuilderGroupTest, SumAccumulatorTranslationGroupByTest) {
    auto docs = std::vector<BSONArray>{
        BSON_ARRAY(BSON("a" << 1 << "b" << 3)),
        BSON_ARRAY(BSON("a" << 1 << "b" << 4)),
        BSON_ARRAY(BSON("a" << 2 << "b" << 13)),
        BSON_ARRAY(BSON("a" << 2 << "b"
                            << "a")),
    };
    runGroupAggregationTest(
        "{_id: '$a', x: {$sum: '$b'}}",
        docs,
        BSON_ARRAY(BSON("_id" << 1 << "x" << 3 + 4) << BSON("_id" << 2 << "x" << 13)));
}

TEST_F(SbeStageBuilderGroupTest, AvgAccumulatorTranslationSmallIntegers) {
    auto docs = std::vector<BSONArray>{BSON_ARRAY(BSON("a" << 1 << "b" << 2)),
                                       BSON_ARRAY(BSON("a" << 2 << "b" << 4)),
                                       BSON_ARRAY(BSON("a" << 3 << "b" << 6))};
    runGroupAggregationTest(
        "{_id: null, x: {$avg: '$b'}}",
        docs,
        BSON_ARRAY(BSON("_id" << BSONNULL << "x" << static_cast<double>(2 + 4 + 6) / 3)));
}

TEST_F(SbeStageBuilderGroupTest, AvgAccumulatorTranslationIntegerInputsNonIntegerAverage) {
    auto docs = std::vector<BSONArray>{BSON_ARRAY(BSON("a" << 1 << "b" << 1)),
                                       BSON_ARRAY(BSON("a" << 2 << "b" << 2))};
    runGroupAggregationTest(
        "{_id: null, x: {$avg: '$b'}}",
        docs,
        BSON_ARRAY(BSON("_id" << BSONNULL << "x" << static_cast<double>(1 + 2) / 2)));
}

TEST_F(SbeStageBuilderGroupTest, AvgAccumulatorTranslationVariousNumberTypes) {
    auto docs = std::vector<BSONArray>{BSON_ARRAY(BSON("a" << 1 << "b" << 1.0)),
                                       BSON_ARRAY(BSON("a" << 2 << "b" << 2ll)),
                                       BSON_ARRAY(BSON("a" << 3 << "b" << Decimal128(3.0))),
                                       BSON_ARRAY(BSON("a" << 4 << "b" << 4))};
    runGroupAggregationTest(
        "{_id: null, x: {$avg: '$b'}}",
        docs,
        BSON_ARRAY(
            BSON("_id" << BSONNULL << "x" << Decimal128(static_cast<double>(1 + 2 + 3 + 4) / 4))));
}

TEST_F(SbeStageBuilderGroupTest, AvgAccumulatorTranslationNonNumericFields) {
    auto docs = std::vector<BSONArray>{
        BSON_ARRAY(BSON("a" << 1 << "b" << 1)),
        BSON_ARRAY(BSON("a" << 2 << "b"
                            << "hello")),
        BSON_ARRAY(BSON("a" << 3 << "b" << BSONNULL)),
        BSON_ARRAY(BSON("a" << 4 << "b" << BSON("x" << 42))),
        BSON_ARRAY(BSON("a" << 5 << "b" << 6)),
    };
    runGroupAggregationTest(
        "{_id: null, x: {$avg: '$b'}}",
        docs,
        BSON_ARRAY(BSON("_id" << BSONNULL << "x" << static_cast<double>(1 + 6) / 2)));
}

// NaN is a numeric value, so the average of NaN is NaN and not null.
TEST_F(SbeStageBuilderGroupTest, AvgAccumulatorTranslationNan) {
    auto docs = std::vector<BSONArray>{
        BSON_ARRAY(BSON("a" << 5 << "b" << std::numeric_limits<double>::quiet_NaN())),
    };
    runGroupAggregationTest(
        "{_id: null, x: {$avg: '$b'}}",
        docs,
        BSON_ARRAY(BSON("_id" << BSONNULL << "x" << std::numeric_limits<double>::quiet_NaN())));
}

TEST_F(SbeStageBuilderGroupTest, AvgAccumulatorTranslationMissingSomeFields) {
    auto docs = std::vector<BSONArray>{BSON_ARRAY(BSON("a" << 1 << "b" << 1)),
                                       BSON_ARRAY(BSON("a" << 2)),
                                       BSON_ARRAY(BSON("a" << 3 << "b" << 3))};
    runGroupAggregationTest(
        "{_id: null, x: {$avg: '$b'}}",
        docs,
        BSON_ARRAY(BSON("_id" << BSONNULL << "x" << static_cast<double>(1 + 3) / 2)));
}

// Notice, that $sum in the following two cases returns zero, but $avg is undefined and should
// return null.
TEST_F(SbeStageBuilderGroupTest, AvgAccumulatorTranslationMissingAllFields) {
    auto docs = std::vector<BSONArray>{
        BSON_ARRAY(BSON("a" << 1)), BSON_ARRAY(BSON("a" << 2)), BSON_ARRAY(BSON("a" << 3))};
    runGroupAggregationTest("{_id: null, x: {$avg: '$b'}}",
                            docs,
                            BSON_ARRAY(BSON("_id" << BSONNULL << "x" << BSONNULL)));
}

TEST_F(SbeStageBuilderGroupTest, AvgAccumulatorTranslationMissingOrNonNumericFields) {
    auto docs = std::vector<BSONArray>{BSON_ARRAY(BSON("a" << 1)),
                                       BSON_ARRAY(BSON("b" << BSON("x" << 42)))};
    runGroupAggregationTest("{_id: null, x: {$avg: '$b'}}",
                            docs,
                            BSON_ARRAY(BSON("_id" << BSONNULL << "x" << BSONNULL)));
}

TEST_F(SbeStageBuilderGroupTest, AvgAccumulatorTranslationWithGrouping) {
    auto docs = std::vector<BSONArray>{BSON_ARRAY(BSON("a" << 1 << "b" << 2)),
                                       BSON_ARRAY(BSON("a" << 1 << "b" << 4)),
                                       BSON_ARRAY(BSON("a" << 3 << "b" << 6)),
                                       BSON_ARRAY(BSON("a" << 3 << "b" << 8))};
    runGroupAggregationTest("{_id: '$a', x: {$avg: '$b'}}",
                            docs,
                            BSON_ARRAY(BSON("_id" << 1 << "x" << (2 + 4) / 2)
                                       << BSON("_id" << 3 << "x" << (6 + 8) / 2)));
}

TEST_F(SbeStageBuilderGroupTest, AddToSetAccumulatorTranslationSingleDoc) {
    auto docs = std::vector<BSONArray>{BSON_ARRAY(BSON("a" << 1 << "b" << 1))};
    runAddToSetTest("{_id: null, x: {$addToSet: '$b'}}", docs, BSON_ARRAY(1));
}

TEST_F(SbeStageBuilderGroupTest, AddToSetAccumulatorTranslationBasic) {
    auto docs = std::vector<BSONArray>{BSON_ARRAY(BSON("a" << 1 << "b" << 3)),
                                       BSON_ARRAY(BSON("a" << 2 << "b" << 1)),
                                       BSON_ARRAY(BSON("a" << 3 << "b" << 2))};
    runAddToSetTest("{_id: null, x: {$addToSet: '$b'}}", docs, BSON_ARRAY(1 << 2 << 3));
}

TEST_F(SbeStageBuilderGroupTest, AddToSetAccumulatorTranslationSubfield) {
    auto docs = std::vector<BSONArray>{BSON_ARRAY(BSON("a" << 1 << "b" << BSON("c" << 1))),
                                       BSON_ARRAY(BSON("a" << 2 << "b" << BSON("c" << 2))),
                                       BSON_ARRAY(BSON("a" << 3 << "b" << BSON("c" << 3)))};
    runAddToSetTest("{_id: null, x: {$addToSet: '$b.c'}}", docs, BSON_ARRAY(1 << 2 << 3));
}

TEST_F(SbeStageBuilderGroupTest, AddToSetAccumulatorTranslationRepeatedValue) {
    auto docs = std::vector<BSONArray>{BSON_ARRAY(BSON("a" << 1 << "b" << 2)),
                                       BSON_ARRAY(BSON("a" << 2 << "b" << 1)),
                                       BSON_ARRAY(BSON("a" << 3 << "b" << 2))};
    runAddToSetTest("{_id: null, x: {$addToSet: '$b'}}", docs, BSON_ARRAY(1 << 2));
}

TEST_F(SbeStageBuilderGroupTest, AddToSetAccumulatorTranslationMixedTypes) {
    const auto bsonArr = BSON_ARRAY(1 << 2 << 3);
    const auto bsonObj = BSON("c" << 1);
    const auto strVal = "hello"_sd;
    auto docs = std::vector<BSONArray>{BSON_ARRAY(BSON("a" << 1 << "b" << 42)),
                                       BSON_ARRAY(BSON("a" << 2 << "b" << 4.2)),
                                       BSON_ARRAY(BSON("a" << 3 << "b" << true)),
                                       BSON_ARRAY(BSON("a" << 4 << "b" << strVal)),
                                       BSON_ARRAY(BSON("a" << 5 << "b" << bsonObj)),
                                       BSON_ARRAY(BSON("a" << 6 << "b" << bsonArr))};
    auto expectedSet = BSON_ARRAY(42 << 4.2 << true << strVal << bsonObj << bsonArr);
    runAddToSetTest("{_id: null, x: {$addToSet: '$b'}}", docs, expectedSet);
}

TEST_F(SbeStageBuilderGroupTest, AddToSetAccumulatorTranslationSomeMissingFields) {
    auto docs = std::vector<BSONArray>{BSON_ARRAY(BSON("a" << 1)),
                                       BSON_ARRAY(BSON("a" << 2 << "b" << 2)),
                                       BSON_ARRAY(BSON("a" << 3 << "b" << BSONNULL)),
                                       BSON_ARRAY(BSON("a" << 4 << "c" << 1))};
    runAddToSetTest("{_id: null, x: {$addToSet: '$b'}}", docs, BSON_ARRAY(BSONNULL << 2));
}

TEST_F(SbeStageBuilderGroupTest, AddToSetAccumulatorTranslationAllMissingFields) {
    auto docs = std::vector<BSONArray>{
        BSON_ARRAY(BSON("a" << 1)), BSON_ARRAY(BSON("a" << 2)), BSON_ARRAY(BSON("a" << 3))};
    runAddToSetTest("{_id: null, x: {$addToSet: '$b'}}", docs, BSONArray{});
}

TEST_F(SbeStageBuilderGroupTest, AddToSetAccumulatorTranslationWithCollation) {
    auto docs = std::vector<BSONArray>{BSON_ARRAY(BSON("a" << 1 << "b"
                                                           << "x")),
                                       BSON_ARRAY(BSON("a" << 2 << "b"
                                                           << "y"))};
    runAddToSetTest(
        "{_id: null, x: {$addToSet: '$b'}}",
        docs,
        BSON_ARRAY("x"),
        std::make_unique<CollatorInterfaceMock>(CollatorInterfaceMock::MockType::kAlwaysEqual));
}

TEST_F(SbeStageBuilderGroupTest, PushAccumulatorTranslationNoDocs) {
    auto docs = std::vector<BSONArray>{BSONArray{}};
    runGroupAggregationTest("{_id: null, x: {$push: '$b'}}",
                            docs,
                            BSON_ARRAY(BSON("_id" << BSONNULL << "x" << BSONArray{})));
}

TEST_F(SbeStageBuilderGroupTest, PushAccumulatorTranslationBasic) {
    auto docs = std::vector<BSONArray>{BSON_ARRAY(BSON("a" << 1 << "b" << 3)),
                                       BSON_ARRAY(BSON("a" << 2 << "b" << 1)),
                                       BSON_ARRAY(BSON("a" << 3 << "b" << 2)),
                                       BSON_ARRAY(BSON("a" << 1 << "b" << 3))};
    runGroupAggregationTest(
        "{_id: null, x: {$push: '$b'}}",
        docs,
        BSON_ARRAY(BSON("_id" << BSONNULL << "x" << BSON_ARRAY(3 << 1 << 2 << 3))));
}

TEST_F(SbeStageBuilderGroupTest, PushAccumulatorTranslationSomeMissing) {
    auto docs = std::vector<BSONArray>{BSON_ARRAY(BSON("a" << 1 << "b" << 3)),
                                       BSON_ARRAY(BSON("a" << 2 << "b" << 1)),
                                       BSON_ARRAY(BSON("a" << 3))};
    runGroupAggregationTest("{_id: null, x: {$push: '$b'}}",
                            docs,
                            BSON_ARRAY(BSON("_id" << BSONNULL << "x" << BSON_ARRAY(3 << 1))));
}

TEST_F(SbeStageBuilderGroupTest, PushAccumulatorTranslationAllMissing) {
    auto docs = std::vector<BSONArray>{
        BSON_ARRAY(BSON("a" << 1)), BSON_ARRAY(BSON("a" << 2)), BSON_ARRAY(BSON("a" << 3))};
    runGroupAggregationTest("{_id: null, x: {$push: '$b'}}",
                            docs,
                            BSON_ARRAY(BSON("_id" << BSONNULL << "x" << BSONArray{})));
}

TEST_F(SbeStageBuilderGroupTest, PushAccumulatorTranslationVariousTypes) {
    const auto bsonArr = BSON_ARRAY(1 << 2 << 3);
    const auto bsonObj = BSON("c" << 1);
    const auto strVal = "hello"_sd;
    auto docs = std::vector<BSONArray>{BSON_ARRAY(BSON("a" << 1 << "b" << 42)),
                                       BSON_ARRAY(BSON("a" << 2 << "b" << 4.2)),
                                       BSON_ARRAY(BSON("a" << 3 << "b" << true)),
                                       BSON_ARRAY(BSON("a" << 4 << "b" << strVal)),
                                       BSON_ARRAY(BSON("a" << 5 << "b" << bsonObj)),
                                       BSON_ARRAY(BSON("a" << 6 << "b" << bsonArr))};
    runGroupAggregationTest(
        "{_id: null, x: {$push: '$b'}}",
        docs,
        BSON_ARRAY(BSON("_id" << BSONNULL << "x"
                              << BSON_ARRAY(42 << 4.2 << true << strVal << bsonObj << bsonArr))));
}

TEST_F(SbeStageBuilderGroupTest, PushAccumulatorTranslationExpression) {
    auto docs = std::vector<BSONArray>{BSON_ARRAY(BSON("a" << 1 << "b" << 3)),
                                       BSON_ARRAY(BSON("a" << 2 << "b" << 1)),
                                       BSON_ARRAY(BSON("a" << 3 << "b" << 2))};
    runGroupAggregationTest(
        "{_id: null, x: {$push: {$add: ['$a', '$b']}}}",
        docs,
        BSON_ARRAY(BSON("_id" << BSONNULL << "x" << BSON_ARRAY(1 + 3 << 2 + 1 << 3 + 2))));
}

TEST_F(SbeStageBuilderGroupTest, PushAccumulatorTranslationExpressionMissingField) {
    auto docs = std::vector<BSONArray>{BSON_ARRAY(BSON("a" << 1 << "b" << 3)),
                                       BSON_ARRAY(BSON("a" << 2 << "b" << 1)),
                                       BSON_ARRAY(BSON("a" << 3))};
    runGroupAggregationTest(
        "{_id: null, x: {$push: {$add: ['$a', '$b']}}}",
        docs,
        BSON_ARRAY(BSON("_id" << BSONNULL << "x"
                              << BSON_ARRAY(1 + 3 << 2 + 1 << BSONNULL /*3 + Nothing*/))));
}

TEST_F(SbeStageBuilderGroupTest, PushAccumulatorTranslationExpressionInvalid) {
    auto docs = std::vector<BSONArray>{BSON_ARRAY(BSON("a" << 1 << "b" << 3)),
                                       BSON_ARRAY(BSON("a" << 2 << "b" << 1)),
                                       BSON_ARRAY(BSON("a" << 3 << "b"
                                                           << "hello"))};
    ASSERT_THROWS(
        runGroupAggregationTest("{_id: null, x: {$push: {$add: ['$a', '$b']}}}", docs, BSONArray{}),
        AssertionException);
}

TEST_F(SbeStageBuilderGroupTest, MergeObjectsAccumulatorTranslationNoDocs) {
    auto docs = std::vector<BSONArray>{BSONArray{}};
    runGroupAggregationTest("{_id: null, x: {$mergeObjects: '$a'}}",
                            docs,
                            BSON_ARRAY(BSON("_id" << BSONNULL << "x" << BSONObj{})));
}

TEST_F(SbeStageBuilderGroupTest, MergeObjectsAccumulatorTranslationSingleDoc) {
    auto docs = std::vector<BSONArray>{BSON_ARRAY(BSON("a" << BSON("b" << 1)))};
    runGroupAggregationTest("{_id: null, x: {$mergeObjects: '$a'}}",
                            docs,
                            BSON_ARRAY(BSON("_id" << BSONNULL << "x" << BSON("b" << 1))));
}

TEST_F(SbeStageBuilderGroupTest, MergeObjectsAccumulatorTranslationNonExistentField) {
    auto docs = std::vector<BSONArray>{BSON_ARRAY(BSON("_id" << 1 << "a" << BSON("c" << 1)))};
    runGroupAggregationTest("{_id: null, x: {$mergeObjects: '$b'}}",
                            docs,
                            BSON_ARRAY(BSON("_id" << BSONNULL << "x" << BSONObj{})));
}

TEST_F(SbeStageBuilderGroupTest, MergeObjectsAccumulatorTranslationNonObject) {
    auto docs = std::vector<BSONArray>{BSON_ARRAY(BSON("_id" << 1 << "a" << 1))};

    ASSERT_THROWS(runGroupAggregationTest("{_id: null, x: {$mergeObjects: 'a'}", docs, BSONArray{}),
                  AssertionException);
}

TEST_F(SbeStageBuilderGroupTest, MergeObjectsAccumulatorTranslationDisjointDocs) {
    auto docs = std::vector<BSONArray>{BSON_ARRAY(BSON("_id" << 1 << "a" << BSON("c" << 1))),
                                       BSON_ARRAY(BSON("_id" << 1 << "a" << BSON("b" << 1)))};
    runGroupAggregationTest(
        "{_id: null, x: {$mergeObjects: '$a'}}",
        docs,
        BSON_ARRAY(BSON("_id" << BSONNULL << "x" << BSON("c" << 1 << "b" << 1))));
}

TEST_F(SbeStageBuilderGroupTest, MergeObjectsAccumulatorTranslationIntersectingDocs) {
    auto docs =
        std::vector<BSONArray>{BSON_ARRAY(BSON("_id" << 1 << "a" << BSON("c" << 1 << "d" << 1))),
                               BSON_ARRAY(BSON("_id" << 1 << "a" << BSON("b" << 1 << "d" << 4))),
                               BSON_ARRAY(BSON("_id" << 1 << "a"
                                                     << BSON("c"
                                                             << "hello")))};
    runGroupAggregationTest("{_id: null, x: {$mergeObjects: '$a'}}",
                            docs,
                            BSON_ARRAY(BSON("_id" << BSONNULL << "x"
                                                  << BSON("c"
                                                          << "hello"
                                                          << "d" << 4 << "b" << 1))));
}

TEST_F(SbeStageBuilderGroupTest, MergeObjectsAccumulatorTranslationIntersectingEmbeddedDocs) {
    auto docs = std::vector<BSONArray>{
        BSON_ARRAY(BSON("_id" << 1 << "a" << BSON("c" << 1 << "d" << 1 << "e" << BSON("f" << 5)))),
        BSON_ARRAY(BSON("_id" << 1 << "a" << BSON("b" << 1 << "d" << 4 << "e" << BSON("g" << 6))))};
    runGroupAggregationTest(
        "{_id: null, x: {$mergeObjects: '$a'}}",
        docs,
        BSON_ARRAY(BSON("_id" << BSONNULL << "x"
                              << BSON("c" << 1 << "d" << 4 << "e" << BSON("g" << 6) << "b" << 1))));
}

TEST_F(SbeStageBuilderGroupTest, MergeObjectsAccumulatorAccumulatorTranslationMissingField) {
    auto docs = std::vector<BSONArray>{BSON_ARRAY(BSON("_id" << 1 << "a" << BSON("c" << 1))),
                                       BSON_ARRAY(BSON("_id" << 1 << "a" << BSON("b" << 1))),
                                       BSON_ARRAY(BSON("_id" << 1 << "b" << BSON("d" << 1)))};
    runGroupAggregationTest(
        "{_id: null, x: {$mergeObjects: '$a'}}",
        docs,
        BSON_ARRAY(BSON("_id" << BSONNULL << "x" << BSON("c" << 1 << "b" << 1))));
}

TEST_F(SbeStageBuilderGroupTest, MergeObjectsAccumulatorTranslationSomeNull) {
    auto docs = std::vector<BSONArray>{
        BSON_ARRAY(BSON("_id" << 1 << "a" << BSONNULL)),
        BSON_ARRAY(BSON("_id" << 1 << "a" << BSON("b" << BSONNULL << "d" << BSONNULL))),
        BSON_ARRAY(BSON("_id" << 1 << "a" << BSON("d" << 1)))};
    runGroupAggregationTest(
        "{_id: null, x: {$mergeObjects: '$a'}}",
        docs,
        BSON_ARRAY(BSON("_id" << BSONNULL << "x" << BSON("b" << BSONNULL << "d" << 1))));
}

TEST_F(SbeStageBuilderGroupTest, StdDevPopAccumulatorTranslationBasic) {
    std::vector<double> values = {85, 90, 71};
    auto docs = std::vector<BSONArray>{BSON_ARRAY(BSON("a" << values[0])),
                                       BSON_ARRAY(BSON("a" << values[1])),
                                       BSON_ARRAY(BSON("a" << values[2]))};
    runGroupAggregationTest(
        "{_id: null, x: {$stdDevPop: '$a'}}",
        docs,
        BSON_ARRAY(BSON("_id" << BSONNULL << "x" << computeStdDev(values, false))));
}

TEST_F(SbeStageBuilderGroupTest, StdDevSampAccumulatorTranslationBasic) {
    std::vector<double> values = {85, 90, 71};
    auto docs = std::vector<BSONArray>{BSON_ARRAY(BSON("a" << values[0])),
                                       BSON_ARRAY(BSON("a" << values[1])),
                                       BSON_ARRAY(BSON("a" << values[2]))};
    runGroupAggregationTest(
        "{_id: null, x: {$stdDevSamp: '$a'}}",
        docs,
        BSON_ARRAY(BSON("_id" << BSONNULL << "x" << computeStdDev(values, true))));
}

TEST_F(SbeStageBuilderGroupTest, StdDevPopAccumulatorTranslationSomeMissingFields) {
    std::vector<double> values = {85, 71};
    auto docs = std::vector<BSONArray>{BSON_ARRAY(BSON("a" << values[0])),
                                       BSON_ARRAY(BSON("b" << 90)),
                                       BSON_ARRAY(BSON("a" << values[1]))};
    runGroupAggregationTest(
        "{_id: null, x: {$stdDevPop: '$a'}}",
        docs,
        BSON_ARRAY(BSON("_id" << BSONNULL << "x" << computeStdDev(values, false))));
}

TEST_F(SbeStageBuilderGroupTest, StdDevSampAccumulatorTranslationSomeMissingFields) {
    std::vector<double> values = {75, 100};
    auto docs = std::vector<BSONArray>{BSON_ARRAY(BSON("a" << values[0])),
                                       BSON_ARRAY(BSON("b" << 90)),
                                       BSON_ARRAY(BSON("a" << values[1]))};
    runGroupAggregationTest(
        "{_id: null, x: {$stdDevSamp: '$a'}}",
        docs,
        BSON_ARRAY(BSON("_id" << BSONNULL << "x" << computeStdDev(values, true))));
}

TEST_F(SbeStageBuilderGroupTest, StdDevPopAccumulatorTranslationAllMissingFields) {
    auto docs = std::vector<BSONArray>{
        BSON_ARRAY(BSON("b" << 85)), BSON_ARRAY(BSON("b" << 90)), BSON_ARRAY(BSON("b" << 71))};
    runGroupAggregationTest("{_id: null, x: {$stdDevPop: '$a'}}",
                            docs,
                            BSON_ARRAY(BSON("_id" << BSONNULL << "x" << BSONNULL)));
}

TEST_F(SbeStageBuilderGroupTest, StdDevSampAccumulatorTranslationAllMissingFields) {
    auto docs = std::vector<BSONArray>{
        BSON_ARRAY(BSON("b" << 85)), BSON_ARRAY(BSON("b" << 90)), BSON_ARRAY(BSON("b" << 71))};
    runGroupAggregationTest("{_id: null, x: {$stdDevSamp: '$a'}}",
                            docs,
                            BSON_ARRAY(BSON("_id" << BSONNULL << "x" << BSONNULL)));
}

TEST_F(SbeStageBuilderGroupTest, StdDevPopAccumulatorTranslationIncorrectType) {
    std::vector<double> values = {75, 100};
    auto docs = std::vector<BSONArray>{BSON_ARRAY(BSON("a" << values[0])),
                                       BSON_ARRAY(BSON("a" << values[1])),
                                       BSON_ARRAY(BSON("a"
                                                       << "hello"))};
    runGroupAggregationTest(
        "{_id: null, x: {$stdDevPop: '$a'}}",
        docs,
        BSON_ARRAY(BSON("_id" << BSONNULL << "x" << computeStdDev(values, false))));
}

TEST_F(SbeStageBuilderGroupTest, StdDevSampAccumulatorTranslationIncorrectType) {
    std::vector<double> values = {75, 100};
    auto docs = std::vector<BSONArray>{BSON_ARRAY(BSON("a" << values[0])),
                                       BSON_ARRAY(BSON("a" << values[1])),
                                       BSON_ARRAY(BSON("a"
                                                       << "hello"))};
    runGroupAggregationTest(
        "{_id: null, x: {$stdDevSamp: '$a'}}",
        docs,
        BSON_ARRAY(BSON("_id" << BSONNULL << "x" << computeStdDev(values, true))));
}

TEST_F(SbeStageBuilderGroupTest, StdDevPopAccumulatorTranslationDecimalType) {
    std::vector<double> values = {85, 90, 71};
    auto decimalType = Decimal128(90);
    auto docs = std::vector<BSONArray>{BSON_ARRAY(BSON("a" << values[0])),
                                       BSON_ARRAY(BSON("a" << decimalType)),
                                       BSON_ARRAY(BSON("a" << values[2]))};
    runGroupAggregationTest(
        "{_id: null, x: {$stdDevPop: '$a'}}",
        docs,
        BSON_ARRAY(BSON("_id" << BSONNULL << "x" << computeStdDev(values, false))));
}

TEST_F(SbeStageBuilderGroupTest, StdDevSampAccumulatorTranslationDecimalType) {
    std::vector<double> values = {85, 90, 71};
    auto decimalType = Decimal128(90);
    auto docs = std::vector<BSONArray>{BSON_ARRAY(BSON("a" << values[0])),
                                       BSON_ARRAY(BSON("a" << decimalType)),
                                       BSON_ARRAY(BSON("a" << values[2]))};
    runGroupAggregationTest(
        "{_id: null, x: {$stdDevSamp: '$a'}}",
        docs,
        BSON_ARRAY(BSON("_id" << BSONNULL << "x" << computeStdDev(values, true))));
}

TEST_F(SbeStageBuilderGroupTest, StdDevPopAccumulatorTranslationSingleDoc) {
    auto docs = std::vector<BSONArray>{BSON_ARRAY(BSON("a" << 85))};
    runGroupAggregationTest("{_id: null, x: {$stdDevPop: '$a'}}",
                            docs,
                            BSON_ARRAY(BSON("_id" << BSONNULL << "x" << 0)));
}

TEST_F(SbeStageBuilderGroupTest, StdDevSampAccumulatorTranslationSingleDoc) {
    auto docs = std::vector<BSONArray>{BSON_ARRAY(BSON("a" << 85))};
    runGroupAggregationTest("{_id: null, x: {$stdDevSamp: '$a'}}",
                            docs,
                            BSON_ARRAY(BSON("_id" << BSONNULL << "x" << BSONNULL)));
}

TEST_F(SbeStageBuilderGroupTest, StdDevPopAccumulatorTranslationZeroDoc) {
    auto docs = std::vector<BSONArray>{BSONArray{}};
    runGroupAggregationTest("{_id: null, x: {$stdDevPop: '$a'}}",
                            docs,
                            BSON_ARRAY(BSON("_id" << BSONNULL << "x" << BSONNULL)));
}

TEST_F(SbeStageBuilderGroupTest, StdDevSampAccumulatorTranslationZeroDoc) {
    auto docs = std::vector<BSONArray>{BSONArray{}};
    runGroupAggregationTest("{_id: null, x: {$stdDevSamp: '$a'}}",
                            docs,
                            BSON_ARRAY(BSON("_id" << BSONNULL << "x" << BSONNULL)));
}

TEST_F(SbeStageBuilderGroupTest, StdDevPopAccumulatorTranslationDoesNotOverflow) {
    auto docs =
        std::vector<BSONArray>{BSON_ARRAY(BSON("a" << std::numeric_limits<long long>::max())),
                               BSON_ARRAY(BSON("a" << std::numeric_limits<long long>::max()))};
    runGroupAggregationTest("{_id: null, x: {$stdDevPop: '$a'}}",
                            docs,
                            BSON_ARRAY(BSON("_id" << BSONNULL << "x" << 0)));
}

TEST_F(SbeStageBuilderGroupTest, StdDevSampAccumulatorTranslationDoesNotOverflow) {
    auto docs =
        std::vector<BSONArray>{BSON_ARRAY(BSON("a" << std::numeric_limits<long long>::max())),
                               BSON_ARRAY(BSON("a" << std::numeric_limits<long long>::max()))};
    runGroupAggregationTest("{_id: null, x: {$stdDevSamp: '$a'}}",
                            docs,
                            BSON_ARRAY(BSON("_id" << BSONNULL << "x" << 0)));
}

TEST_F(SbeStageBuilderGroupTest, StdDevPopAccumulatorTranslationInfinity) {
    auto docs =
        std::vector<BSONArray>{BSON_ARRAY(BSON("a" << std::numeric_limits<double>::infinity())),
                               BSON_ARRAY(BSON("a" << std::numeric_limits<double>::infinity()))};
    runGroupAggregationTest(
        "{_id: null, x: {$stdDevPop: '$a'}}",
        docs,
        BSON_ARRAY(BSON("_id" << BSONNULL << "x" << std::numeric_limits<double>::quiet_NaN())));
}

TEST_F(SbeStageBuilderGroupTest, StdDevSampAccumulatorTranslationInfinity) {
    auto docs =
        std::vector<BSONArray>{BSON_ARRAY(BSON("a" << std::numeric_limits<double>::infinity())),
                               BSON_ARRAY(BSON("a" << std::numeric_limits<double>::infinity()))};
    runGroupAggregationTest(
        "{_id: null, x: {$stdDevSamp: '$a'}}",
        docs,
        BSON_ARRAY(BSON("_id" << BSONNULL << "x" << std::numeric_limits<double>::quiet_NaN())));
}

TEST_F(SbeStageBuilderGroupTest, StdDevPopAccumulatorTranslationNaN) {
    auto docs =
        std::vector<BSONArray>{BSON_ARRAY(BSON("a" << std::numeric_limits<long long>::max())),
                               BSON_ARRAY(BSON("a" << std::numeric_limits<long long>::max()))};
    runGroupAggregationTest("{_id: null, x: {$stdDevPop: '$a'}}",
                            docs,
                            BSON_ARRAY(BSON("_id" << BSONNULL << "x" << 0)));
}

TEST_F(SbeStageBuilderGroupTest, StdDevSampAccumulatorTranslationNaN) {
    auto docs = std::vector<BSONArray>{
        BSON_ARRAY(BSON("a" << std::numeric_limits<long long>::quiet_NaN())),
        BSON_ARRAY(BSON("a" << std::numeric_limits<long long>::quiet_NaN()))};
    runGroupAggregationTest("{_id: null, x: {$stdDevSamp: '$a'}}",
                            docs,
                            BSON_ARRAY(BSON("_id" << BSONNULL << "x" << 0)));
}

TEST_F(SbeStageBuilderGroupTest, StdDevPopAccumulatorTranslationNonNumber) {
    auto docs = std::vector<BSONArray>{BSON_ARRAY(BSON("a" << 100)),
                                       BSON_ARRAY(BSON("a" << BSON_ARRAY(1 << 2 << 3)))};
    runGroupAggregationTest("{_id: null, x: {$stdDevPop: '$a'}}",
                            docs,
                            BSON_ARRAY(BSON("_id" << BSONNULL << "x" << 0)));
}

TEST_F(SbeStageBuilderGroupTest, StdDevSampAccumulatorTranslationNonNumber) {
    auto docs = std::vector<BSONArray>{BSON_ARRAY(BSON("a" << 100)),
                                       BSON_ARRAY(BSON("a" << 100)),
                                       BSON_ARRAY(BSON("a" << BSON_ARRAY(1 << 2 << 3)))};
    runGroupAggregationTest("{_id: null, x: {$stdDevSamp: '$a'}}",
                            docs,
                            BSON_ARRAY(BSON("_id" << BSONNULL << "x" << 0)));
}

class AccumulatorSBEIncompatible final : public AccumulatorState {
public:
    static constexpr auto kName = "$incompatible"_sd;
    const char* getOpName() const final {
        return kName.rawData();
    }
    explicit AccumulatorSBEIncompatible(ExpressionContext* expCtx) : AccumulatorState(expCtx) {}
    void processInternal(const Value& input, bool merging) final {}
    Value getValue(bool toBeMerged) final {
        return Value(true);
    }
    void reset() final {}
    static boost::intrusive_ptr<AccumulatorState> create(ExpressionContext* expCtx) {
        return new AccumulatorSBEIncompatible(expCtx);
    }
};

REGISTER_ACCUMULATOR(
    incompatible,
    genericParseSBEUnsupportedSingleExpressionAccumulator<AccumulatorSBEIncompatible>);

TEST_F(SbeStageBuilderGroupTest, SbeGroupCompatibleFlag) {
    std::vector<std::string> testCases = {
        R"(_id: null, agg: {$addToSet: "$item"})",
        R"(_id: null, agg: {$avg: "$quantity"})",
        R"(_id: null, agg: {$first: "$item"})",
        R"(_id: null, agg: {$last: "$item"})",
        // TODO (SERVER-51541): Uncomment the following two test cases when $object supported is
        // added to SBE.
        // R"(_id: null, agg: {$_internalJsReduce: {data: {k: "$word", v: "$val"}, eval: "null"}})",
        //
        // R"(_id: null, agg: {$accumulator: {init: "a", accumulate: "b", accumulateArgs:
        // ["$copies"], merge: "c", lang: "js"}})",
        R"(_id: null, agg: {$mergeObjects: "$item"})",
        R"(_id: null, agg: {$min: "$item"})",
        R"(_id: null, agg: {$max: "$item"})",
        R"(_id: null, agg: {$push: "$item"})",
        R"(_id: null, agg: {$max: "$item"})",
        R"(_id: null, agg: {$stdDevPop: "$item"})",
        R"(_id: null, agg: {$stdDevSamp: "$item"})",
        R"(_id: null, agg: {$sum: "$item"})",
        R"(_id: null, agg: {$sum: {$not: "$item"}})",
        // R"(_id: {a: "$a", b: "$b"})",
        // All supported case.
        R"(_id: null, agg1: {$sum: "$item"}, agg2: {$stdDevPop: "$price"}, agg3: {$stdDevSamp: "$quantity"})",
        // Mix of supported/unsupported accumulators.
        R"(_id: null, agg1: {$sum: "$item"}, agg2: {$incompatible: "$item"}, agg3: {$avg: "$a"})",
        R"(_id: null, agg1: {$incompatible: "$item"}, agg2: {$min: "$item"}, agg3: {$avg:
         "$quantity"})",
        // No accumulator case
        R"(_id: "$a")",
    };

    boost::intrusive_ptr<ExpressionContext> expCtx(new ExpressionContextForTest());
    std::vector<BSONObj> groupSpecs;
    groupSpecs.reserve(testCases.size());
    for (auto testCase : testCases) {
        auto groupSpec = fromjson(fmt::sprintf("{%s}", testCase));
        runSbeGroupCompatibleFlagTest({groupSpec}, expCtx);
        groupSpecs.push_back(groupSpec);
    }
    runSbeGroupCompatibleFlagTest(groupSpecs, expCtx);
}

TEST_F(SbeStageBuilderGroupTest, SbeIncompatibleExpressionInGroup) {
    std::vector<std::string> testCases = {
        R"(_id: {$and: ["$a", true]})",
        R"(_id: {$or: ["$a", false]})",
        R"(_id: null, x: {$sum: {$and: ["$a", true]}})",
        R"(_id: null, x: {$sum: {$or: ["$a", false]}})",
        R"(_id: null, x: {$sum: {$add: ["$b", {$and: ["$a", true]}]}})",
        R"(_id: null, x: {$sum: {$add: ["$b", {$or: ["$a", false]}]}})",
    };

    boost::intrusive_ptr<ExpressionContext> expCtx(new ExpressionContextForTest());
    for (auto testCase : testCases) {
        auto groupSpec = fromjson(fmt::sprintf("{%s}", testCase));
        runSbeIncompatibleGroupSpecTest({groupSpec}, expCtx);
    }
}

}  // namespace mongo
