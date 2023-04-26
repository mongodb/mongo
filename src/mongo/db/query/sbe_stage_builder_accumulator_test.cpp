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

#include "mongo/bson/bsonobj.h"
#include "mongo/db/exec/sbe/expression_test_base.h"
#include "mongo/db/pipeline/document_source_group.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/query/collation/collator_interface_mock.h"
#include "mongo/db/query/query_solution.h"
#include "mongo/db/query/sbe_stage_builder_accumulator.h"
#include "mongo/db/query/sbe_stage_builder_test_fixture.h"
#include "mongo/idl/server_parameter_test_util.h"
#include "mongo/logv2/log.h"
#include "mongo/unittest/unittest.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

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
                                                     docSrcGroup->getIdExpression(),
                                                     docSrcGroup->getAccumulatedFields(),
                                                     false /*doingMerge*/,
                                                     true /*shouldProduceBson*/);

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

    void runGroupAggregationToFail(StringData groupSpec,
                                   std::vector<BSONArray> inputDocs,
                                   ErrorCodes::Error expectedError,
                                   std::unique_ptr<CollatorInterface> collator = nullptr) {
        try {
            getResultsForAggregation(fromjson(groupSpec), inputDocs, std::move(collator));
            ASSERT(false) << "Expected error: " << expectedError << " for " << groupSpec
                          << " but succeeded";
        } catch (const DBException& e) {
            ASSERT_TRUE(e.code() == expectedError) << "group spec: " << groupSpec;
        }
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
        expCtx->sbeCompatibility = SbeCompatibility::fullyCompatible;
        // When we parse and optimize the 'groupSpec' to build a DocumentSourceGroup, those
        // accumulation expressions or '_id' expression that are not supported by SBE will flip the
        // 'sbeCompatible()' flag in the 'groupStage' to false.
        auto docSrc = createDocumentSourceGroup(expCtx, groupSpec);
        auto groupStage = dynamic_cast<DocumentSourceGroup*>(docSrc.get());
        ASSERT(groupStage != nullptr);

        auto sbeCompatible = groupStage->sbeCompatibility() != SbeCompatibility::notCompatible;
        ASSERT_EQ(false, sbeCompatible) << "group spec: " << groupSpec;
    }

    void runSbeGroupCompatibleFlagTest(const std::vector<BSONObj>& groupSpecs,
                                       boost::intrusive_ptr<ExpressionContext>& expCtx) {
        expCtx->sbeCompatibility = SbeCompatibility::fullyCompatible;
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
            ASSERT_EQ(sbeGroupCompatible,
                      groupStage->sbeCompatibility() != SbeCompatibility::notCompatible)
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

TEST_F(SbeStageBuilderGroupTest, TestIdEmptyObject) {
    auto docs = std::vector<BSONArray>{BSON_ARRAY(BSON("a" << 1 << "b" << 1)),
                                       BSON_ARRAY(BSON("a" << 1 << "b" << 2)),
                                       BSON_ARRAY(BSON("a" << 2 << "b" << 3))};

    runGroupAggregationTest(R"({_id: {}})", docs, BSON_ARRAY(BSON("_id" << BSONObj{})));
}

TEST_F(SbeStageBuilderGroupTest, TestIdEmptyString) {
    auto docs = std::vector<BSONArray>{BSON_ARRAY(BSON("a" << 1 << "b" << 1)),
                                       BSON_ARRAY(BSON("a" << 1 << "b" << 2)),
                                       BSON_ARRAY(BSON("a" << 2 << "b" << 3))};

    runGroupAggregationTest(R"({_id: ""})",
                            docs,
                            BSON_ARRAY(BSON("_id"
                                            << "")));
}

TEST_F(SbeStageBuilderGroupTest, TestIdConstString) {
    auto docs = std::vector<BSONArray>{BSON_ARRAY(BSON("a" << 1 << "b" << 1)),
                                       BSON_ARRAY(BSON("a" << 1 << "b" << 2)),
                                       BSON_ARRAY(BSON("a" << 2 << "b" << 3))};

    runGroupAggregationTest(R"({_id: "abc"})",
                            docs,
                            BSON_ARRAY(BSON("_id"
                                            << "abc")));
}

TEST_F(SbeStageBuilderGroupTest, TestIdNumericConst) {
    auto docs = std::vector<BSONArray>{BSON_ARRAY(BSON("a" << 1 << "b" << 1)),
                                       BSON_ARRAY(BSON("a" << 1 << "b" << 2)),
                                       BSON_ARRAY(BSON("a" << 2 << "b" << 3))};

    runGroupAggregationTest(R"({_id: 1})", docs, BSON_ARRAY(BSON("_id" << 1)));
}

TEST_F(SbeStageBuilderGroupTest, TestIdNumericExpression) {
    auto docs = std::vector<BSONArray>{BSON_ARRAY(BSON("a" << 1 << "b" << 1)),
                                       BSON_ARRAY(BSON("a" << 1 << "b" << 2)),
                                       BSON_ARRAY(BSON("a" << 1 << "b" << 2)),
                                       BSON_ARRAY(BSON("a" << 2 << "b" << 1)),
                                       BSON_ARRAY(BSON("a" << 2 << "b" << 3))};

    runGroupAggregationTest(R"({_id: {"$add": ["$a", "$b"]}})",
                            docs,
                            BSON_ARRAY(BSON("_id" << 2) << BSON("_id" << 3) << BSON("_id" << 5)));

    runGroupAggregationTest(
        R"({_id: {"$multiply": ["$b", 1000]}})",
        docs,
        BSON_ARRAY(BSON("_id" << 1000) << BSON("_id" << 2000) << BSON("_id" << 3000)));

    runGroupAggregationTest(R"({_id: {"$divide": [{"$multiply": ["$b", 1000]}, 500]}})",
                            docs,
                            BSON_ARRAY(BSON("_id" << 2) << BSON("_id" << 4) << BSON("_id" << 6)));
}

TEST_F(SbeStageBuilderGroupTest, TestIdNumericExprOnNonNumericData) {
    auto docs = std::vector<BSONArray>{BSON_ARRAY(BSON("a" << 1 << "b" << 1)),
                                       BSON_ARRAY(BSON("a" << 1 << "b"
                                                           << "2"))};

    runGroupAggregationToFail(
        R"({_id: {"$add": ["$a", "$b"]}})", docs, static_cast<ErrorCodes::Error>(7157723));

    runGroupAggregationToFail(
        R"({_id: {"$multiply": ["$b", 1000]}})", docs, static_cast<ErrorCodes::Error>(7157721));

    runGroupAggregationToFail(R"({_id: {"$divide": [{"$multiply": ["$a", 1000]}, "$b"]}})",
                              docs,
                              static_cast<ErrorCodes::Error>(7157719));
}

TEST_F(SbeStageBuilderGroupTest, TestIdObjectExpression) {
    auto docs = std::vector<BSONArray>{BSON_ARRAY(BSON("b" << 1)),
                                       BSON_ARRAY(BSON("a" << BSONNULL << "b" << 2)),
                                       BSON_ARRAY(BSON("a" << 1 << "b" << 2)),
                                       BSON_ARRAY(BSON("a" << 1 << "b" << 2)),
                                       BSON_ARRAY(BSON("a" << 2 << "b" << 2)),
                                       BSON_ARRAY(BSON("a" << 2 << "b" << 3))};

    // Document id with a special field name such as "_id".
    // 'Nothing' is converted to 'Null' when there's only one field and both {_id: Nothing} / {_id:
    // null} becomes {_id: null}.
    runGroupAggregationTest(R"({_id: {_id: "$a"}})",
                            docs,
                            BSON_ARRAY(BSON("_id" << BSON("_id" << BSONNULL))
                                       << BSON("_id" << BSON("_id" << 1))
                                       << BSON("_id" << BSON("_id" << 2))));

    runGroupAggregationTest(R"({_id: {b: "$b"}})",
                            docs,
                            BSON_ARRAY(BSON("_id" << BSON("b" << 1))
                                       << BSON("_id" << BSON("b" << 2))
                                       << BSON("_id" << BSON("b" << 3))));

    // Duplicated field expressions.
    // When there are multiple fields, 'Nothing' is not converted to 'Null' and {_id: Nothing, b:
    // Nothing} becomes {}.
    runGroupAggregationTest(R"({_id: {_id: "$a", a: "$a"}})",
                            docs,
                            BSON_ARRAY(BSON("_id" << BSONObj{})
                                       << BSON("_id" << BSON("_id" << BSONNULL << "a" << BSONNULL))
                                       << BSON("_id" << BSON("_id" << 1 << "a" << 1))
                                       << BSON("_id" << BSON("_id" << 2 << "a" << 2))));

    // A missing field for a group-by key must generate an _id document without such field when
    // there are multiple fields. {a: Nothing, c: Nothing} becomes {}.
    runGroupAggregationTest(R"({_id: {a: "$a", c: "$c"}})",
                            docs,
                            BSON_ARRAY(BSON("_id" << BSONObj{})
                                       << BSON("_id" << BSON("a" << BSONNULL))
                                       << BSON("_id" << BSON("a" << 1))
                                       << BSON("_id" << BSON("a" << 2))));

    // A missing field for a group-by key must generate an _id document without such field when
    // there are multiple fields. {a: Nothing, b: 1} becomes {b: 1}.
    runGroupAggregationTest(R"({_id: {a: "$a", b: "$b"}})",
                            docs,
                            BSON_ARRAY(BSON("_id" << BSON("a" << BSONNULL << "b" << 2))
                                       << BSON("_id" << BSON("a" << 1 << "b" << 2))
                                       << BSON("_id" << BSON("a" << 2 << "b" << 2))
                                       << BSON("_id" << BSON("a" << 2 << "b" << 3))
                                       << BSON("_id" << BSON("b" << 1))));
}

TEST_F(SbeStageBuilderGroupTest, TestIdObjectSubFieldExpression) {
    auto docs = std::vector<BSONArray>{BSON_ARRAY(BSON("d" << BSON("a" << 1 << "b" << 1))),
                                       BSON_ARRAY(BSON("d" << BSON("a" << 1 << "b" << 2))),
                                       BSON_ARRAY(BSON("d" << BSON("a" << 1 << "b" << 2))),
                                       BSON_ARRAY(BSON("d" << BSON("a" << 2 << "b" << 2))),
                                       BSON_ARRAY(BSON("d" << BSON("a" << 2 << "b" << 3)))};

    runGroupAggregationTest(
        R"({_id: {da: "$d.a"}})",
        docs,
        BSON_ARRAY(BSON("_id" << BSON("da" << 1)) << BSON("_id" << BSON("da" << 2))));

    // 'Nothing' is converted to 'Null' when there's only one field and both {dc: Nothing} becomes
    // {_id: null}.
    runGroupAggregationTest(
        R"({_id: {dc: "$d.c"}})", docs, BSON_ARRAY(BSON("_id" << BSON("dc" << BSONNULL))));
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

TEST_F(SbeStageBuilderGroupTest, FirstNAccumulatorSingleGroup) {
    auto docs = std::vector<BSONArray>{BSON_ARRAY(BSON("a" << 11 << "b" << 1)),
                                       BSON_ARRAY(BSON("a" << 22 << "b" << 2)),
                                       BSON_ARRAY(BSON("a" << 33 << "b" << 3)),
                                       BSON_ARRAY(BSON("a" << 44 << "b" << 4))};
    runGroupAggregationTest(
        "{_id: null, x: {$firstN: {input: '$a', n: 3}}}",
        docs,
        BSON_ARRAY(BSON("_id" << BSONNULL << "x" << BSON_ARRAY(11 << 22 << 33))));
}

TEST_F(SbeStageBuilderGroupTest, FirstNAccumulatorNotEnoughElement) {
    auto docs = std::vector<BSONArray>{BSON_ARRAY(BSON("a" << 22 << "b" << 2)),
                                       BSON_ARRAY(BSON("a" << 11 << "b" << 1))};
    runGroupAggregationTest("{_id: null, x: {$firstN: {input: '$a', n: 3}}}",
                            docs,
                            BSON_ARRAY(BSON("_id" << BSONNULL << "x" << BSON_ARRAY(22 << 11))));
}

TEST_F(SbeStageBuilderGroupTest, FirstNAccumulatorMultiGroup) {
    auto docs = std::vector<BSONArray>{BSON_ARRAY(BSON("a" << 44 << "b" << 4 << "n" << 2)),
                                       BSON_ARRAY(BSON("a" << 77 << "b" << 7 << "n" << 4)),
                                       BSON_ARRAY(BSON("a" << 33 << "b" << 3 << "n" << 2)),
                                       BSON_ARRAY(BSON("a" << 88 << "b" << 8 << "n" << 4)),
                                       BSON_ARRAY(BSON("a" << 22 << "b" << 2 << "n" << 2)),
                                       BSON_ARRAY(BSON("a" << 66 << "b" << 6 << "n" << 4)),
                                       BSON_ARRAY(BSON("a" << 11 << "b" << 1 << "n" << 2)),
                                       BSON_ARRAY(BSON("a" << 55 << "b" << 5 << "n" << 4))};
    runGroupAggregationTest("{_id: '$n', x: {$firstN: {input: '$a', n: 3}}}",
                            docs,
                            BSON_ARRAY(BSON("_id" << 2 << "x" << BSON_ARRAY(44 << 33 << 22))
                                       << BSON("_id" << 4 << "x" << BSON_ARRAY(77 << 88 << 66))));
}

TEST_F(SbeStageBuilderGroupTest, FirstNAccumulatorDynamicN) {
    auto docs = std::vector<BSONArray>{BSON_ARRAY(BSON("a" << 44 << "b" << 4 << "n" << 2)),
                                       BSON_ARRAY(BSON("a" << 33 << "b" << 3 << "n" << 2)),
                                       BSON_ARRAY(BSON("a" << 22 << "b" << 2 << "n" << 2)),
                                       BSON_ARRAY(BSON("a" << 11 << "b" << 1 << "n" << 2)),
                                       BSON_ARRAY(BSON("a" << 88 << "b" << 8 << "n" << 4)),
                                       BSON_ARRAY(BSON("a" << 77 << "b" << 7 << "n" << 4)),
                                       BSON_ARRAY(BSON("a" << 66 << "b" << 6 << "n" << 4)),
                                       BSON_ARRAY(BSON("a" << 55 << "b" << 5 << "n" << 4))};
    runGroupAggregationTest(
        "{_id: {k: '$n'}, x: {$firstN: {input: '$a', n: '$k'}}}",
        docs,
        BSON_ARRAY(BSON("_id" << BSON("k" << 2) << "x" << BSON_ARRAY(44 << 33))
                   << BSON("_id" << BSON("k" << 4) << "x" << BSON_ARRAY(88 << 77 << 66 << 55))));
}

TEST_F(SbeStageBuilderGroupTest, FirstNAccumulatorInvalidConstantN) {
    const std::vector<std::string> testCases{"'string'", "4.2", "-1", "0"};
    auto docs = std::vector<BSONArray>{BSON_ARRAY(BSON("a" << 11 << "b" << 1))};
    for (const auto& testCase : testCases) {
        runGroupAggregationToFail(
            str::stream() << "{_id: null, x: {$firstN: {input: '$a', n: " << testCase << "}}}",
            docs,
            static_cast<ErrorCodes::Error>(7548606));
    }
}

TEST_F(SbeStageBuilderGroupTest, FirstNAccumulatorInvalidDynamicN) {
    auto docs = std::vector<BSONArray>{BSON_ARRAY(BSON("a" << 11 << "n" << 1))};
    runGroupAggregationToFail("{_id: null, x: {$firstN: {input: '$a', n: '$n'}}}",
                              docs,
                              static_cast<ErrorCodes::Error>(7548607));

    runGroupAggregationToFail("{_id: {k: '$n'}, x: {$firstN: {input: '$a', n: '$v'}}}",
                              docs,
                              static_cast<ErrorCodes::Error>(7548607));
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
        R"(_id: {a: "$a", b: "$b"})",
        // All supported case.
        R"(_id: null, agg1: {$sum: "$item"}, agg2: {$stdDevPop: "$price"}, agg3: {$stdDevSamp: "$quantity"})",
        // Mix of supported/unsupported accumulators.
        R"(_id: null, agg1: {$sum: "$item"}, agg2: {$incompatible: "$item"}, agg3: {$avg: "$a"})",
        R"(_id: null, agg1: {$incompatible: "$item"}, agg2: {$min: "$item"}, agg3: {$avg: "$quantity"})",
        // No accumulator case
        R"(_id: "$a")",
    };

    boost::intrusive_ptr<ExpressionContext> expCtx(new ExpressionContextForTest());
    std::vector<BSONObj> groupSpecs;
    groupSpecs.reserve(testCases.size());
    for (const auto& testCase : testCases) {
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
    for (const auto& testCase : testCases) {
        auto groupSpec = fromjson(fmt::sprintf("{%s}", testCase));
        runSbeIncompatibleGroupSpecTest({groupSpec}, expCtx);
    }
}

/**
 * A test fixture designed to test that the expressions generated to combine partial aggregates
 * that have been spilled to disk work correctly. We use 'EExpressionTestFixture' rather than
 * something like 'SbeStageBuilderTestFixture' so that the expressions can be tested in isolation,
 * without actually requiring a hash agg stage or without actually spilling any data to disk.
 */
class SbeStageBuilderGroupAggCombinerTest : public sbe::EExpressionTestFixture {
public:
    explicit SbeStageBuilderGroupAggCombinerTest()
        : _expCtx{make_intrusive<ExpressionContextForTest>()},
          _inputSlotId{bindAccessor(&_inputAccessor)},
          _collatorSlotId{bindAccessor(&_collatorAccessor)} {}

    AccumulationStatement makeAccumulationStatement(StringData accumName) {
        return makeAccumulationStatement(BSON("unused" << BSON(accumName << "unused")));
    }

    AccumulationStatement makeAccumulationStatement(BSONObj accumulationStmt) {
        _accumulationStmtBson = std::move(accumulationStmt);
        VariablesParseState vps = _expCtx->variablesParseState;
        return AccumulationStatement::parseAccumulationStatement(
            _expCtx.get(), _accumulationStmtBson.firstElement(), vps);
    }

    /**
     * Convenience method for producing bytecode which combines partial aggregates for the given
     * 'AccumulationStatement'.
     *
     * Requires that accumulation statement results in a single aggregate with one input and one
     * output. Furthermore, cannot be used when the test case involves a non-simple collation.
     */
    std::unique_ptr<sbe::vm::CodeFragment> compileSingleInputNoCollator(
        const AccumulationStatement& accStatement) {
        auto exprs = stage_builder::buildCombinePartialAggregates(
            accStatement, {_inputSlotId}, boost::none, _frameIdGenerator);
        ASSERT_EQ(exprs.size(), 1u);
        _expr = std::move(exprs[0]);

        return compileAggExpression(*_expr, &_aggAccessor);
    }

    /**
     * Verifies that executing the bytecode ('code') for combining partial aggregates for $group
     * spilling produces the 'expected' outputs given 'inputs'.
     *
     * The inputs and expected outputs are expressed as BSON arrays as a convenience to the caller,
     * and should have the same length. The bytecode is executed over each element of 'inputs'
     * one-by-one, with the result stored into a slot holding the aggregate value. At each step,
     * this function asserts that the current aggregate value is equal to the matching element in
     * 'expected'.
     *
     * The string "MISSING" can be used as a sentinel in either 'inputs' or 'outputs' in order to
     * represent the Nothing value (since nothingness cannot literally be stored in a BSON array).
     */
    void aggregateAndAssertResults(BSONArray inputs,
                                   BSONArray expected,
                                   const sbe::vm::CodeFragment* code) {
        auto [inputTag, inputVal] = makeArray(inputs);
        auto [expectedTag, expectedVal] = makeArray(expected);
        return aggregateAndAssertResults(inputTag, inputVal, expectedTag, expectedVal, code);
    }

    /**
     * Verifies that executing the bytecode ('code') for combining partial aggregates for $group
     * spilling produces the 'expectedVal' outputs given 'inputsVal'. Assumes ownership of both
     * 'expectedVal' and 'inputsVal'.
     *
     * Identical to the overload above, except the inputs and expected outputs are provided as SBE
     * arrays rather than BSON arrays. This is useful if the caller needs to construct input and
     * output ways in a special way that cannot be achieved by trivial conversion from BSON.
     */
    void aggregateAndAssertResults(sbe::value::TypeTags inputTag,
                                   sbe::value::Value inputVal,
                                   sbe::value::TypeTags expectedTag,
                                   sbe::value::Value expectedVal,
                                   const sbe::vm::CodeFragment* code) {
        // Make sure we are starting from a clean state.
        _inputAccessor.reset();
        _aggAccessor.reset();

        sbe::value::ValueGuard inputGuard{inputTag, inputVal};
        sbe::value::ValueGuard expectedGuard{expectedTag, expectedVal};

        sbe::value::ArrayEnumerator inputEnumerator{inputTag, inputVal};
        sbe::value::ArrayEnumerator expectedEnumerator{expectedTag, expectedVal};

        // Aggregate the inputs one-by-one, and at each step validate that the resulting accumulator
        // state is as expected.
        int index = 0;
        while (!inputEnumerator.atEnd()) {
            ASSERT_FALSE(expectedEnumerator.atEnd());
            auto [nextInputTag, nextInputVal] = inputEnumerator.getViewOfValue();

            // Feed in the input value, treating "MISSING" as a special sentinel to indicate the
            // Nothing value.
            if (sbe::value::isString(nextInputTag) &&
                sbe::value::getStringView(nextInputTag, nextInputVal) == "MISSING"_sd) {
                _inputAccessor.reset();
            } else {
                auto [copyTag, copyVal] = sbe::value::copyValue(nextInputTag, nextInputVal);
                _inputAccessor.reset(true, copyTag, copyVal);
            }

            auto [outputTag, outputVal] = runCompiledExpression(code);

            // Validate that the output value equals the expected value, and then put the output
            // value into the slot that holds the accumulation state.
            auto [expectedOutputTag, expectedOutputValue] = expectedEnumerator.getViewOfValue();
            if (sbe::value::isString(expectedOutputTag) &&
                sbe::value::getStringView(expectedOutputTag, expectedOutputValue) == "MISSING"_sd) {
                expectedOutputTag = sbe::value::TypeTags::Nothing;
                expectedOutputValue = 0;
            }
            auto [compareTag, compareValue] = sbe::value::compareValue(
                outputTag, outputVal, expectedOutputTag, expectedOutputValue);
            if (compareTag != sbe::value::TypeTags::NumberInt32 || compareValue != 0) {
                // The test failed, but dump the actual and expected values to the logs for ease of
                // debugging.
                str::stream actualBuilder;
                auto actualPrinter = makeValuePrinter(actualBuilder);
                actualPrinter.writeValueToStream(outputTag, outputVal);

                str::stream expectedBuilder;
                auto expectedPrinter = makeValuePrinter(expectedBuilder);
                expectedPrinter.writeValueToStream(expectedOutputTag, expectedOutputValue);

                LOGV2(7039529,
                      "Actual value not equal to expected value",
                      "actual"_attr = actualBuilder,
                      "expected"_attr = expectedBuilder,
                      "index"_attr = index);
                FAIL("accumulator did not have expected value");
            }

            _aggAccessor.reset(true, outputTag, outputVal);

            inputEnumerator.advance();
            expectedEnumerator.advance();
            ++index;
        }
    }

    /**
     * A helper for converting a sequence of accumulator states for $push or $addToSet into the
     * corresponding SBE value.
     */
    enum class Accumulator { kPush, kAddToSet };
    std::pair<sbe::value::TypeTags, sbe::value::Value> makeArrayAccumVal(BSONArray bsonArray,
                                                                         Accumulator accumType) {
        auto [resultTag, resultVal] = sbe::value::makeNewArray();
        sbe::value::ValueGuard resultGuard{resultTag, resultVal};
        auto resultArr = sbe::value::getArrayView(resultVal);

        for (auto&& elt : bsonArray) {
            ASSERT(elt.type() == BSONType::Array);

            BSONObjIterator arrayIt{elt.embeddedObject()};
            ASSERT_TRUE(arrayIt.more());
            auto firstElt = arrayIt.next();
            ASSERT(firstElt.type() == BSONType::Array);
            BSONArray partialBsonArr{firstElt.embeddedObject()};

            ASSERT_TRUE(arrayIt.more());
            auto secondElt = arrayIt.next();
            ASSERT(secondElt.isNumber());
            int64_t size = secondElt.safeNumberLong();

            ASSERT_FALSE(arrayIt.more());

            // Each partial aggregate is a two-element array whose first element is the partial
            // $push result (itself an array) and whose second element is the size.
            auto [partialAggTag, partialAggVal] = sbe::value::makeNewArray();
            auto partialAggArr = sbe::value::getArrayView(partialAggVal);

            auto [pushedValsTag, pushedValsVal] = accumType == Accumulator::kPush
                ? makeArray(partialBsonArr)
                : makeArraySet(partialBsonArr);
            partialAggArr->push_back(pushedValsTag, pushedValsVal);

            partialAggArr->push_back(sbe::value::TypeTags::NumberInt64,
                                     sbe::value::bitcastFrom<int64_t>(size));

            resultArr->push_back(partialAggTag, partialAggVal);
        }

        resultGuard.reset();
        return {resultTag, resultVal};
    }

    /**
     * Given the name of an SBE agg function ('aggFuncName') and an array of values expressed as a
     * BSON array, aggregates the values inside the array and returns the resulting SBE value.
     */
    std::pair<sbe::value::TypeTags, sbe::value::Value> makeOnePartialAggregate(
        StringData aggFuncName, BSONArray valuesToAgg) {
        // Make sure we are starting from a clean state.
        _inputAccessor.reset();
        _aggAccessor.reset();

        // Construct an expression which calls the given agg function, aggregating the values in
        // '_inputSlotId'.
        auto expr =
            stage_builder::makeFunction(aggFuncName, stage_builder::makeVariable(_inputSlotId));
        auto code = compileAggExpression(*expr, &_aggAccessor);

        // Find the first element by skipping the length.
        const char* bsonElt = valuesToAgg.objdata() + 4;
        const char* bsonEnd = bsonElt + valuesToAgg.objsize();
        while (*bsonElt != 0) {
            auto fieldName = sbe::bson::fieldNameAndLength(bsonElt);

            // Convert the BSON value to an SBE value and put it inside the input slot.
            auto [tag, val] = sbe::bson::convertFrom<false>(bsonElt, bsonEnd, fieldName.size());
            _inputAccessor.reset(true, tag, val);

            // Run the agg function, and put the result in the slot holding the aggregate value.
            auto [outputTag, outputVal] = runCompiledExpression(code.get());
            _aggAccessor.reset(true, outputTag, outputVal);

            bsonElt = sbe::bson::advance(bsonElt, fieldName.size());
        }

        return _aggAccessor.copyOrMoveValue();
    }

    /**
     * Returns an SBE array which contains a sequence of partial aggregate values. Useful for
     * constructing a sequence of partial aggregates when those partial aggregates are not trivial
     * to describe using BSON. The input to this function is a BSON array of BSON arrays; each of
     * the inner arrays is aggregated using the given 'aggFuncName' in order to produce the output
     * SBE array.
     *
     * As an example, suppose the agg function is a simple sum. Given the input
     *
     *   [[8, 1, 5], [6], [2,3]]
     *
     * the output will be the SBE array [14, 6, 5].
     */
    std::pair<sbe::value::TypeTags, sbe::value::Value> makePartialAggArray(
        StringData aggFuncName, BSONArray arrayOfArrays) {
        auto [arrTag, arrVal] = sbe::value::makeNewArray();
        sbe::value::ValueGuard guard{arrTag, arrVal};

        auto arr = sbe::value::getArrayView(arrVal);

        for (auto&& element : arrayOfArrays) {
            ASSERT(element.type() == BSONType::Array);
            auto [tag, val] =
                makeOnePartialAggregate(aggFuncName, BSONArray{element.embeddedObject()});
            arr->push_back(tag, val);
        }

        guard.reset();
        return {arrTag, arrVal};
    }

    std::pair<sbe::value::TypeTags, sbe::value::Value> convertFromBSONArray(BSONArray arr) {
        auto [arrTag, arrVal] = sbe::value::makeNewArray();
        auto arrView = sbe::value::getArrayView(arrVal);

        for (auto elem : arr) {
            auto [tag, val] = sbe::bson::convertFrom<false>(elem);
            arrView->push_back(tag, val);
        }
        return {arrTag, arrVal};
    }

protected:
    sbe::value::FrameIdGenerator _frameIdGenerator;
    boost::intrusive_ptr<ExpressionContextForTest> _expCtx;

    // Accessor and corresponding slot id that holds the input to the agg expression. Each time we
    // "turn the crank" this will hold the next partial aggregate to be aggregated into
    // '_aggAccessor'.
    sbe::value::OwnedValueAccessor _inputAccessor;
    sbe::value::SlotId _inputSlotId;

    // The accessor which holds the final output resulting from combining all partial outputs. We
    // check that the intermediate value is as expected after every turn of the crank.
    sbe::value::OwnedValueAccessor _aggAccessor;

    sbe::value::OwnedValueAccessor _collatorAccessor;
    sbe::value::SlotId _collatorSlotId;

private:
    BSONObj _accumulationStmtBson;
    std::unique_ptr<sbe::EExpression> _expr;
};

TEST_F(SbeStageBuilderGroupAggCombinerTest, CombinePartialAggsMin) {
    auto accStatement = makeAccumulationStatement("$min"_sd);
    auto compiledExpr = compileSingleInputNoCollator(accStatement);

    auto inputValues = BSON_ARRAY(8 << 7 << 9 << BSONNULL << 6);
    auto expectedAggStates = BSON_ARRAY(8 << 7 << 7 << 7 << 6);
    aggregateAndAssertResults(inputValues, expectedAggStates, compiledExpr.get());

    // Test that Nothing values are treated as expected.
    inputValues = BSON_ARRAY("MISSING" << 9 << 7 << "MISSING" << 6);
    expectedAggStates = BSON_ARRAY("MISSING" << 9 << 7 << 7 << 6);
    aggregateAndAssertResults(inputValues, expectedAggStates, compiledExpr.get());
}

TEST_F(SbeStageBuilderGroupAggCombinerTest, CombinePartialAggsMinWithCollation) {
    auto accStatement = makeAccumulationStatement("$min"_sd);

    auto exprs = stage_builder::buildCombinePartialAggregates(
        accStatement, {_inputSlotId}, {_collatorSlotId}, _frameIdGenerator);
    ASSERT_EQ(exprs.size(), 1u);
    auto expr = std::move(exprs[0]);

    CollatorInterfaceMock collator{CollatorInterfaceMock::MockType::kReverseString};
    _collatorAccessor.reset(false,
                            sbe::value::TypeTags::collator,
                            sbe::value::bitcastFrom<const CollatorInterface*>(&collator));

    auto compiledExpr = compileAggExpression(*expr, &_aggAccessor);

    // The strings in reverse have the opposite ordering as compared to forwards.
    auto inputValues = BSON_ARRAY("az"
                                  << "by"
                                  << "cx");
    auto expectedAggStates = BSON_ARRAY("az"
                                        << "by"
                                        << "cx");
    aggregateAndAssertResults(inputValues, expectedAggStates, compiledExpr.get());
}

TEST_F(SbeStageBuilderGroupAggCombinerTest, CombinePartialAggsMax) {
    auto accStatement = makeAccumulationStatement("$max"_sd);
    auto compiledExpr = compileSingleInputNoCollator(accStatement);

    auto inputValues = BSON_ARRAY(3 << 1 << 4 << BSONNULL << 8);
    auto expectedAggStates = BSON_ARRAY(3 << 3 << 4 << 4 << 8);
    aggregateAndAssertResults(inputValues, expectedAggStates, compiledExpr.get());

    // Test that Nothing values are treated as expected.
    inputValues = BSON_ARRAY("MISSING" << 7 << 9 << "MISSING" << 10);
    expectedAggStates = BSON_ARRAY("MISSING" << 7 << 9 << 9 << 10);
    aggregateAndAssertResults(inputValues, expectedAggStates, compiledExpr.get());
}

TEST_F(SbeStageBuilderGroupAggCombinerTest, CombinePartialAggsMaxWithCollation) {
    auto accStatement = makeAccumulationStatement("$max"_sd);

    auto exprs = stage_builder::buildCombinePartialAggregates(
        accStatement, {_inputSlotId}, {_collatorSlotId}, _frameIdGenerator);
    ASSERT_EQ(exprs.size(), 1u);
    auto expr = std::move(exprs[0]);

    CollatorInterfaceMock collator{CollatorInterfaceMock::MockType::kReverseString};
    _collatorAccessor.reset(false,
                            sbe::value::TypeTags::collator,
                            sbe::value::bitcastFrom<const CollatorInterface*>(&collator));

    auto compiledExpr = compileAggExpression(*expr, &_aggAccessor);

    // The strings in reverse have the opposite ordering as compared to forwards.
    auto inputValues = BSON_ARRAY("cx"
                                  << "by"
                                  << "az");
    auto expectedAggStates = BSON_ARRAY("cx"
                                        << "by"
                                        << "az");
    aggregateAndAssertResults(inputValues, expectedAggStates, compiledExpr.get());
}

TEST_F(SbeStageBuilderGroupAggCombinerTest, CombinePartialAggsFirst) {
    auto accStatement = makeAccumulationStatement("$first"_sd);
    auto compiledExpr = compileSingleInputNoCollator(accStatement);

    auto inputValues = BSON_ARRAY(3 << 1 << BSONNULL << "MISSING" << 8);
    auto expectedAggStates = BSON_ARRAY(3 << 3 << 3 << 3 << 3);
    aggregateAndAssertResults(inputValues, expectedAggStates, compiledExpr.get());

    // When the first value is missing, the resulting value is a literal null.
    inputValues = BSON_ARRAY("MISSING" << 1 << BSONNULL << "MISSING" << 8);
    expectedAggStates = BSON_ARRAY(BSONNULL << BSONNULL << BSONNULL << BSONNULL << BSONNULL);
    aggregateAndAssertResults(inputValues, expectedAggStates, compiledExpr.get());
}

TEST_F(SbeStageBuilderGroupAggCombinerTest, CombinePartialAggsLast) {
    auto accStatement = makeAccumulationStatement("$last"_sd);
    auto compiledExpr = compileSingleInputNoCollator(accStatement);

    auto inputValues = BSON_ARRAY(3 << 1 << BSONNULL << "MISSING" << 8);
    auto expectedAggStates = BSON_ARRAY(3 << 1 << BSONNULL << BSONNULL << 8);
    aggregateAndAssertResults(inputValues, expectedAggStates, compiledExpr.get());
}

TEST_F(SbeStageBuilderGroupAggCombinerTest, CombinePartialAggsPush) {
    auto accStatement = makeAccumulationStatement("$push"_sd);
    auto compiledExpr = compileSingleInputNoCollator(accStatement);

    auto [inputValuesTag, inputValuesVal] = makeArrayAccumVal(
        BSON_ARRAY(BSON_ARRAY(BSON_ARRAY(5 << 4 << 3) << 10)
                   << BSON_ARRAY(BSON_ARRAY(2 << 1) << 20) << BSON_ARRAY(BSONArray{} << 0)),
        Accumulator::kPush);
    auto [expectedTag, expectedVal] =
        makeArrayAccumVal(BSON_ARRAY(BSON_ARRAY(BSON_ARRAY(5 << 4 << 3) << 10)
                                     << BSON_ARRAY(BSON_ARRAY(5 << 4 << 3 << 2 << 1) << 30)
                                     << BSON_ARRAY(BSON_ARRAY(5 << 4 << 3 << 2 << 1) << 30)),
                          Accumulator::kPush);
    aggregateAndAssertResults(
        inputValuesTag, inputValuesVal, expectedTag, expectedVal, compiledExpr.get());
}

TEST_F(SbeStageBuilderGroupAggCombinerTest, CombinePartialAggsPushThrowsWhenExceedingSizeLimit) {
    auto accStatement = makeAccumulationStatement("$push"_sd);
    auto compiledExpr = compileSingleInputNoCollator(accStatement);

    // If we inject a very large size, we expect the accumulator to throw. This cap prevents the
    // accumulator from consuming too much memory.
    const int64_t largeSize = 1000 * 1000 * 1000;

    auto input = makeArrayAccumVal(BSON_ARRAY(BSON_ARRAY(BSON_ARRAY(5 << 4) << 3)
                                              << BSON_ARRAY(BSON_ARRAY(2 << 1) << largeSize)),
                                   Accumulator::kPush);
    auto expected = makeArrayAccumVal(
        BSON_ARRAY(BSON_ARRAY(BSON_ARRAY(5 << 4) << 3) << BSON_ARRAY(BSON_ARRAY("unused") << -1)),
        Accumulator::kPush);
    ASSERT_THROWS_CODE(
        aggregateAndAssertResults(
            input.first, input.second, expected.first, expected.second, compiledExpr.get()),
        DBException,
        ErrorCodes::ExceededMemoryLimit);
}

TEST_F(SbeStageBuilderGroupAggCombinerTest, CombinePartialAggsAddToSet) {
    auto accStatement = makeAccumulationStatement("$addToSet"_sd);
    auto compiledExpr = compileSingleInputNoCollator(accStatement);

    auto [inputValuesTag, inputValuesVal] =
        makeArrayAccumVal(BSON_ARRAY(BSON_ARRAY(BSON_ARRAY(3 << 4 << 5) << 10)
                                     << BSON_ARRAY(BSON_ARRAY(1 << 3 << 5 << 8) << 20)
                                     << BSON_ARRAY(BSONArray{} << 0)),
                          Accumulator::kAddToSet);

    // Each SBE value is 8 bytes and its tag is 1 byte. So we expect each unique element's size to
    // be calculated as 9 bytes. The sizes from the partial aggregates end up getting ignored, and
    // the total size is recalculated, since we cannot predict the size of the set union in advance.
    auto [expectedTag, expectedVal] =
        makeArrayAccumVal(BSON_ARRAY(BSON_ARRAY(BSON_ARRAY(3 << 4 << 5) << 27)
                                     << BSON_ARRAY(BSON_ARRAY(1 << 3 << 4 << 5 << 8) << 45)
                                     << BSON_ARRAY(BSON_ARRAY(1 << 3 << 4 << 5 << 8) << 45)),
                          Accumulator::kAddToSet);
    aggregateAndAssertResults(
        inputValuesTag, inputValuesVal, expectedTag, expectedVal, compiledExpr.get());
}

TEST_F(SbeStageBuilderGroupAggCombinerTest, CombinePartialAggsAddToSetWithCollation) {
    auto accStatement = makeAccumulationStatement("$addToSet"_sd);

    auto exprs = stage_builder::buildCombinePartialAggregates(
        accStatement, {_inputSlotId}, {_collatorSlotId}, _frameIdGenerator);
    ASSERT_EQ(exprs.size(), 1u);
    auto expr = std::move(exprs[0]);

    CollatorInterfaceMock collator{CollatorInterfaceMock::MockType::kToLowerString};
    _collatorAccessor.reset(false,
                            sbe::value::TypeTags::collator,
                            sbe::value::bitcastFrom<const CollatorInterface*>(&collator));

    auto compiledExpr = compileAggExpression(*expr, &_aggAccessor);

    auto [inputValuesTag, inputValuesVal] =
        makeArrayAccumVal(BSON_ARRAY(BSON_ARRAY(BSON_ARRAY("foo"
                                                           << "bar")
                                                << 10)
                                     << BSON_ARRAY(BSON_ARRAY("FOO"
                                                              << "BAR"
                                                              << "baz")
                                                   << 20)),
                          Accumulator::kAddToSet);

    // These strings end up as big strings copied out of the BSON array, so the size accounts for
    // the value itself, the type tag, the 4-byte size of the string, and the string itself.
    auto [expectedTag, expectedVal] =
        makeArrayAccumVal(BSON_ARRAY(BSON_ARRAY(BSON_ARRAY("bar"
                                                           << "foo")
                                                << 34)
                                     << BSON_ARRAY(BSON_ARRAY("bar"
                                                              << "baz"
                                                              << "foo")
                                                   << 51)),
                          Accumulator::kAddToSet);
    aggregateAndAssertResults(
        inputValuesTag, inputValuesVal, expectedTag, expectedVal, compiledExpr.get());
}

TEST_F(SbeStageBuilderGroupAggCombinerTest,
       CombinePartialAggsAddToSetThrowsWhenExceedingSizeLimit) {
    RAIIServerParameterControllerForTest queryKnobController("internalQueryMaxAddToSetBytes", 50);

    auto accStatement = makeAccumulationStatement("$addToSet"_sd);
    auto compiledExpr = compileSingleInputNoCollator(accStatement);

    auto input = makeArrayAccumVal(BSON_ARRAY(BSON_ARRAY(BSON_ARRAY(1 << 2) << 0)
                                              << BSON_ARRAY(BSON_ARRAY(3 << 4 << 5) << 0)
                                              << BSON_ARRAY(BSON_ARRAY(6) << 0)),
                                   Accumulator::kAddToSet);

    auto expected =
        makeArrayAccumVal(BSON_ARRAY(BSON_ARRAY(BSON_ARRAY(1 << 2) << 18)
                                     << BSON_ARRAY(BSON_ARRAY(1 << 2 << 3 << 4 << 5) << 45)
                                     << BSON_ARRAY(BSON_ARRAY("unused") << -1)),
                          Accumulator::kAddToSet);

    ASSERT_THROWS_CODE(
        aggregateAndAssertResults(
            input.first, input.second, expected.first, expected.second, compiledExpr.get()),
        DBException,
        ErrorCodes::ExceededMemoryLimit);
}

TEST_F(SbeStageBuilderGroupAggCombinerTest, CombinePartialAggsMergeObjects) {
    auto accStatement = makeAccumulationStatement("$mergeObjects"_sd);
    auto compiledExpr = compileSingleInputNoCollator(accStatement);

    auto inputValues = BSON_ARRAY(BSONNULL << BSONObj{} << BSON("a" << 1) << BSONNULL << "MISSING"
                                           << BSON("a" << 2 << "b" << 3 << "c" << 4) << BSONObj{});
    auto expectedAggStates =
        BSON_ARRAY(BSONObj{} << BSONObj{} << BSON("a" << 1) << BSON("a" << 1) << BSON("a" << 1)
                             << BSON("a" << 2 << "b" << 3 << "c" << 4)
                             << BSON("a" << 2 << "b" << 3 << "c" << 4));
    aggregateAndAssertResults(inputValues, expectedAggStates, compiledExpr.get());
}

TEST_F(SbeStageBuilderGroupAggCombinerTest, CombinePartialAggsSimpleCount) {
    auto accStatement = makeAccumulationStatement(BSON("unused" << BSON("$sum" << 1)));
    auto compiledExpr = compileSingleInputNoCollator(accStatement);

    // $sum:1 is a simple count of the incoming documents, and therefore does not require the
    // DoubleDouble summation algorithm. In order to combine partial counts, we use a simple
    // summation.
    auto inputValues = BSON_ARRAY(5 << 8 << "MISSING" << 4);
    auto expectedAggStates = BSON_ARRAY(5 << 13 << 13 << 17);
    aggregateAndAssertResults(inputValues, expectedAggStates, compiledExpr.get());
}

TEST_F(SbeStageBuilderGroupAggCombinerTest, CombinePartialAggsDoubleDoubleSum) {
    auto [inputTag, inputVal] = makePartialAggArray(
        "aggDoubleDoubleSum"_sd,
        BSON_ARRAY(BSON_ARRAY(1 << 2 << 3) << BSON_ARRAY(4 << 6) << BSON_ARRAY(1 << 1 << 1)));
    auto [expectedTag, expectedVal] = makePartialAggArray(
        "aggDoubleDoubleSum"_sd, BSON_ARRAY(BSON_ARRAY(6) << BSON_ARRAY(16) << BSON_ARRAY(19)));

    // A field path expression is needed so that the merging expression is constructed to combine
    // DoubleDouble summations rather than doing a simple sum. The actual field name "foo" is
    // irrelevant because the values are fed into the merging expression by the test fixture.
    auto accStatement = makeAccumulationStatement(BSON("unused" << BSON("$sum"
                                                                        << "$foo")));
    auto compiledExpr = compileSingleInputNoCollator(accStatement);

    aggregateAndAssertResults(inputTag, inputVal, expectedTag, expectedVal, compiledExpr.get());
}

TEST_F(SbeStageBuilderGroupAggCombinerTest, CombinePartialAggsDoubleDoubleSumInfAndNan) {
    auto [inputTag, inputVal] =
        makePartialAggArray("aggDoubleDoubleSum"_sd,
                            BSON_ARRAY(BSON_ARRAY(1 << 2 << 3)
                                       << BSON_ARRAY(4 << std::numeric_limits<double>::infinity())
                                       << BSON_ARRAY(1 << 1 << 1)
                                       << BSON_ARRAY(std::numeric_limits<double>::quiet_NaN())));
    auto [expectedTag, expectedVal] = makePartialAggArray(
        "aggDoubleDoubleSum"_sd,
        BSON_ARRAY(BSON_ARRAY(6) << BSON_ARRAY(10 << std::numeric_limits<double>::infinity())
                                 << BSON_ARRAY(10 << std::numeric_limits<double>::infinity())
                                 << BSON_ARRAY(std::numeric_limits<double>::quiet_NaN())));

    // A field path expression is needed so that the merging expression is constructed to combine
    // DoubleDouble summations rather than doing a simple sum. The actual field name "foo" is
    // irrelevant because the values are fed into the merging expression by the test fixture.
    auto accStatement = makeAccumulationStatement(BSON("unused" << BSON("$sum"
                                                                        << "$foo")));
    auto compiledExpr = compileSingleInputNoCollator(accStatement);

    aggregateAndAssertResults(inputTag, inputVal, expectedTag, expectedVal, compiledExpr.get());
}

TEST_F(SbeStageBuilderGroupAggCombinerTest, CombinePartialAggsDoubleDoubleSumMixedTypes) {
    auto [inputTag, inputVal] = makePartialAggArray(
        "aggDoubleDoubleSum"_sd,
        BSON_ARRAY(BSON_ARRAY(1 << 2) << BSON_ARRAY(3ll << 4ll) << BSON_ARRAY(5.5 << 6.6)
                                      << BSON_ARRAY(Decimal128(7) << Decimal128(8))));
    auto [expectedTag, expectedVal] = makePartialAggArray(
        "aggDoubleDoubleSum"_sd,
        BSON_ARRAY(BSON_ARRAY(1 << 2) << BSON_ARRAY(1 << 2 << 3ll << 4ll)
                                      << BSON_ARRAY(1 << 2 << 3ll << 4ll << 5.5 << 6.6)
                                      << BSON_ARRAY(1 << 2 << 3ll << 4ll << 5.5 << 6.6
                                                      << Decimal128(7) << Decimal128(8))));

    // A field path expression is needed so that the merging expression is constructed to combine
    // DoubleDouble summations rather than doing a simple sum. The actual field name "foo" is
    // irrelevant because the values are fed into the merging expression by the test fixture.
    auto accStatement = makeAccumulationStatement(BSON("unused" << BSON("$sum"
                                                                        << "$foo")));
    auto compiledExpr = compileSingleInputNoCollator(accStatement);

    aggregateAndAssertResults(inputTag, inputVal, expectedTag, expectedVal, compiledExpr.get());
}

TEST_F(SbeStageBuilderGroupAggCombinerTest, CombinePartialAggsDoubleDoubleSumLargeInts) {
    // Large 64-bit ints can't be represented precisely as doubles. This test demonstrates that when
    // summing such large longs, the sum is returned as a long and no precision is lost.
    const int64_t largeLong = std::numeric_limits<int64_t>::max() - 10;

    auto [inputTag, inputVal] = makePartialAggArray(
        "aggDoubleDoubleSum"_sd,
        BSON_ARRAY(BSON_ARRAY(largeLong << 1 << 1) << BSON_ARRAY(1ll << 1ll << 1ll)));
    auto [expectedTag, expectedVal] =
        makePartialAggArray("aggDoubleDoubleSum"_sd,
                            BSON_ARRAY(BSON_ARRAY(largeLong + 2ll) << BSON_ARRAY(largeLong + 5ll)));

    // A field path expression is needed so that the merging expression is constructed to combine
    // DoubleDouble summations rather than doing a simple sum. The actual field name "foo" is
    // irrelevant because the values are fed into the merging expression by the test fixture.
    auto accStatement = makeAccumulationStatement(BSON("unused" << BSON("$sum"
                                                                        << "$foo")));
    auto compiledExpr = compileSingleInputNoCollator(accStatement);

    aggregateAndAssertResults(inputTag, inputVal, expectedTag, expectedVal, compiledExpr.get());

    // Feed the result back into the input accessor. We finalize the resulting aggregate in order
    // to make sure that the resulting sum is mathematically correct.
    auto [resTag, resVal] = _aggAccessor.copyOrMoveValue();
    _inputAccessor.reset(true, resTag, resVal);
    auto finalizeExpr = stage_builder::makeFunction("doubleDoubleSumFinalize",
                                                    stage_builder::makeVariable(_inputSlotId));
    auto finalizeCode = compileExpression(*finalizeExpr);
    auto [finalizedTag, finalizedRes] = runCompiledExpression(finalizeCode.get());
    ASSERT_EQ(finalizedTag, sbe::value::TypeTags::NumberInt64);
    ASSERT_EQ(sbe::value::bitcastTo<int64_t>(finalizedRes), largeLong + 5ll);
}

TEST_F(SbeStageBuilderGroupAggCombinerTest, CombinePartialAggsAvg) {
    auto accStatement = makeAccumulationStatement("$avg"_sd);

    // We expect $avg to result in two separate agg expressions: one for computing the sum and the
    // other for computing the count. Both agg expressions read from the same input slot.
    auto exprs = stage_builder::buildCombinePartialAggregates(
        accStatement, {_inputSlotId, _inputSlotId}, boost::none, _frameIdGenerator);
    ASSERT_EQ(exprs.size(), 2u);

    // Compile the first expression and make sure it can combine DoubleDouble summations as
    // expected.
    auto [inputTag, inputVal] = makePartialAggArray(
        "aggDoubleDoubleSum"_sd,
        BSON_ARRAY(BSON_ARRAY(1 << 2) << BSON_ARRAY(3ll << 4ll) << BSON_ARRAY(5.5 << 6.6)
                                      << BSON_ARRAY(Decimal128(7) << Decimal128(8))));
    auto [expectedTag, expectedVal] = makePartialAggArray(
        "aggDoubleDoubleSum"_sd,
        BSON_ARRAY(BSON_ARRAY(1 << 2) << BSON_ARRAY(1 << 2 << 3ll << 4ll)
                                      << BSON_ARRAY(1 << 2 << 3ll << 4ll << 5.5 << 6.6)
                                      << BSON_ARRAY(1 << 2 << 3ll << 4ll << 5.5 << 6.6
                                                      << Decimal128(7) << Decimal128(8))));
    auto doubleDoubleSumExpr = compileAggExpression(*exprs[0], &_aggAccessor);
    aggregateAndAssertResults(
        inputTag, inputVal, expectedTag, expectedVal, doubleDoubleSumExpr.get());

    // Now compile the second expression and make sure it computes a simple sum.
    auto simpleSumExpr = compileAggExpression(*exprs[1], &_aggAccessor);

    auto inputValues = BSON_ARRAY(5 << 8 << 0 << 4);
    auto expectedAggStates = BSON_ARRAY(5 << 13 << 13 << 17);
    aggregateAndAssertResults(inputValues, expectedAggStates, simpleSumExpr.get());
}

TEST_F(SbeStageBuilderGroupAggCombinerTest, CombinePartialAggsStdDevPop) {
    auto [inputTag, inputVal] = makePartialAggArray(
        "aggStdDev"_sd,
        BSON_ARRAY(BSON_ARRAY(5 << 10)
                   << BSON_ARRAY(6 << 8) << BSON_ARRAY("MISSING") << BSON_ARRAY(1 << 9 << 10)));
    auto [expectedTag, expectedVal] = makePartialAggArray(
        "aggStdDev"_sd,
        BSON_ARRAY(BSON_ARRAY(5 << 10)
                   << BSON_ARRAY(5 << 10 << 6 << 8) << BSON_ARRAY(5 << 10 << 6 << 8)
                   << BSON_ARRAY(5 << 10 << 6 << 8 << 1 << 9 << 10)));

    auto accStatement = makeAccumulationStatement("$stdDevPop"_sd);
    auto compiledExpr = compileSingleInputNoCollator(accStatement);
    aggregateAndAssertResults(inputTag, inputVal, expectedTag, expectedVal, compiledExpr.get());

    // Feed the result back into the input accessor.
    auto [resTag, resVal] = _aggAccessor.copyOrMoveValue();
    _inputAccessor.reset(true, resTag, resVal);
    auto finalizeExpr =
        stage_builder::makeFunction("stdDevPopFinalize", stage_builder::makeVariable(_inputSlotId));
    auto finalizeCode = compileExpression(*finalizeExpr);
    auto [finalizedTag, finalizedRes] = runCompiledExpression(finalizeCode.get());
    ASSERT_EQ(finalizedTag, sbe::value::TypeTags::NumberDouble);
    ASSERT_APPROX_EQUAL(sbe::value::bitcastTo<double>(finalizedRes), 3.0237, 0.0001);
}

TEST_F(SbeStageBuilderGroupAggCombinerTest, CombinePartialAggsStdDevSamp) {
    auto [inputTag, inputVal] = makePartialAggArray(
        "aggStdDev"_sd,
        BSON_ARRAY(BSON_ARRAY(5 << 10)
                   << BSON_ARRAY(6 << 8) << BSON_ARRAY("MISSING") << BSON_ARRAY(1 << 9 << 10)));
    auto [expectedTag, expectedVal] = makePartialAggArray(
        "aggStdDev"_sd,
        BSON_ARRAY(BSON_ARRAY(5 << 10)
                   << BSON_ARRAY(5 << 10 << 6 << 8) << BSON_ARRAY(5 << 10 << 6 << 8)
                   << BSON_ARRAY(5 << 10 << 6 << 8 << 1 << 9 << 10)));

    auto accStatement = makeAccumulationStatement("$stdDevSamp"_sd);
    auto compiledExpr = compileSingleInputNoCollator(accStatement);
    aggregateAndAssertResults(inputTag, inputVal, expectedTag, expectedVal, compiledExpr.get());

    // Feed the result back into the input accessor.
    auto [resTag, resVal] = _aggAccessor.copyOrMoveValue();
    _inputAccessor.reset(true, resTag, resVal);
    auto finalizeExpr = stage_builder::makeFunction("stdDevSampFinalize",
                                                    stage_builder::makeVariable(_inputSlotId));
    auto finalizeCode = compileExpression(*finalizeExpr);
    auto [finalizedTag, finalizedRes] = runCompiledExpression(finalizeCode.get());
    ASSERT_EQ(finalizedTag, sbe::value::TypeTags::NumberDouble);
    ASSERT_APPROX_EQUAL(sbe::value::bitcastTo<double>(finalizedRes), 3.2660, 0.0001);
}

TEST_F(SbeStageBuilderGroupAggCombinerTest, CombinePartialAggsFirstNMergeBothArray) {
    auto expr =
        stage_builder::makeFunction("aggFirstNMerge", stage_builder::makeVariable(_inputSlotId));
    auto compiledExpr = compileAggExpression(*expr, &_aggAccessor);

    auto aggSlot = bindAccessor(&_aggAccessor);
    auto finalizeExpr =
        stage_builder::makeFunction("aggFirstNFinalize", stage_builder::makeVariable(aggSlot));
    auto finalizeCompiledExpr = compileExpression(*finalizeExpr);

    // Merge both arrays
    auto bsonAccArr = BSON_ARRAY(BSON_ARRAY(1 << 2) << 3ll << 16 << 1024);
    auto [accArrTag, accArrVal] = convertFromBSONArray(bsonAccArr);
    _aggAccessor.reset(true, accArrTag, accArrVal);

    auto bsonInputArr = BSON_ARRAY(BSON_ARRAY(3 << 4 << 5) << 3ll << 24 << 1024);
    auto [inputArrTag, inputArrVal] = convertFromBSONArray(bsonInputArr);
    _inputAccessor.reset(true, inputArrTag, inputArrVal);

    auto [resultTag, resultVal] = runCompiledExpression(compiledExpr.get());
    _aggAccessor.reset(true, resultTag, resultVal);
    std::tie(resultTag, resultVal) = runCompiledExpression(finalizeCompiledExpr.get());

    auto expectedArray = BSON_ARRAY(1 << 2 << 3);
    auto [compareTag, compareVal] =
        sbe::value::compareValue(resultTag,
                                 resultVal,
                                 sbe::value::TypeTags::bsonArray,
                                 sbe::value::bitcastFrom<const char*>(expectedArray.objdata()));

    ASSERT_EQ(resultTag, sbe::value::TypeTags::Array);
    ASSERT_EQ(compareTag, sbe::value::TypeTags::NumberInt32);
    ASSERT_EQ(compareVal, 0);
    sbe::value::releaseValue(resultTag, resultVal);
}

TEST_F(SbeStageBuilderGroupAggCombinerTest, CombinePartialAggsFirstNNoMerge) {
    auto expr =
        stage_builder::makeFunction("aggFirstNMerge", stage_builder::makeVariable(_inputSlotId));
    auto compiledExpr = compileAggExpression(*expr, &_aggAccessor);

    auto aggSlot = bindAccessor(&_aggAccessor);
    auto finalizeExpr =
        stage_builder::makeFunction("aggFirstNFinalize", stage_builder::makeVariable(aggSlot));
    auto finalizeCompiledExpr = compileExpression(*finalizeExpr);

    // No merge
    auto bsonAccArr = BSON_ARRAY(BSON_ARRAY(1 << 2 << 6) << 3ll << 24 << 1024);
    auto [accArrTag, accArrVal] = convertFromBSONArray(bsonAccArr);
    _aggAccessor.reset(true, accArrTag, accArrVal);

    auto bsonInputArr = BSON_ARRAY(BSON_ARRAY(3 << 4 << 5) << 3ll << 24 << 1024);
    auto [inputArrTag, inputArrVal] = convertFromBSONArray(bsonInputArr);
    _inputAccessor.reset(true, inputArrTag, inputArrVal);

    auto [resultTag, resultVal] = runCompiledExpression(compiledExpr.get());
    _aggAccessor.reset(true, resultTag, resultVal);
    std::tie(resultTag, resultVal) = runCompiledExpression(finalizeCompiledExpr.get());

    auto expectedArray = BSON_ARRAY(1 << 2 << 6);
    auto [compareTag, compareVal] =
        sbe::value::compareValue(resultTag,
                                 resultVal,
                                 sbe::value::TypeTags::bsonArray,
                                 sbe::value::bitcastFrom<const char*>(expectedArray.objdata()));

    ASSERT_EQ(resultTag, sbe::value::TypeTags::Array);
    ASSERT_EQ(compareTag, sbe::value::TypeTags::NumberInt32);
    ASSERT_EQ(compareVal, 0);
    sbe::value::releaseValue(resultTag, resultVal);
}

TEST_F(SbeStageBuilderGroupAggCombinerTest, CombinePartialAggsFirstNMergeArrayEmpty) {
    auto expr =
        stage_builder::makeFunction("aggFirstNMerge", stage_builder::makeVariable(_inputSlotId));
    auto compiledExpr = compileAggExpression(*expr, &_aggAccessor);

    auto aggSlot = bindAccessor(&_aggAccessor);
    auto finalizeExpr =
        stage_builder::makeFunction("aggFirstNFinalize", stage_builder::makeVariable(aggSlot));
    auto finalizeCompiledExpr = compileExpression(*finalizeExpr);

    // merge array empty
    auto bsonAccArr = BSON_ARRAY(BSONArrayBuilder().arr() << 3ll << 0 << 1024);
    auto [accArrTag, accArrVal] = convertFromBSONArray(bsonAccArr);
    _aggAccessor.reset(true, accArrTag, accArrVal);

    auto bsonInputArr = BSON_ARRAY(BSON_ARRAY(3 << 4 << 5) << 3ll << 24 << 1024);
    auto [inputArrTag, inputArrVal] = convertFromBSONArray(bsonInputArr);
    _inputAccessor.reset(true, inputArrTag, inputArrVal);

    auto [resultTag, resultVal] = runCompiledExpression(compiledExpr.get());
    _aggAccessor.reset(true, resultTag, resultVal);
    std::tie(resultTag, resultVal) = runCompiledExpression(finalizeCompiledExpr.get());

    auto expectedArray = BSON_ARRAY(3 << 4 << 5);
    auto [compareTag, compareVal] =
        sbe::value::compareValue(resultTag,
                                 resultVal,
                                 sbe::value::TypeTags::bsonArray,
                                 sbe::value::bitcastFrom<const char*>(expectedArray.objdata()));

    ASSERT_EQ(resultTag, sbe::value::TypeTags::Array);
    ASSERT_EQ(compareTag, sbe::value::TypeTags::NumberInt32);
    ASSERT_EQ(compareVal, 0);
    sbe::value::releaseValue(resultTag, resultVal);
}

TEST_F(SbeStageBuilderGroupAggCombinerTest, CombinePartialAggsFirstNInputArrayEmpty) {
    auto expr =
        stage_builder::makeFunction("aggFirstNMerge", stage_builder::makeVariable(_inputSlotId));
    auto compiledExpr = compileAggExpression(*expr, &_aggAccessor);

    auto aggSlot = bindAccessor(&_aggAccessor);
    auto finalizeExpr =
        stage_builder::makeFunction("aggFirstNFinalize", stage_builder::makeVariable(aggSlot));
    auto finalizeCompiledExpr = compileExpression(*finalizeExpr);

    // input array empty
    auto bsonAccArr = BSON_ARRAY(BSON_ARRAY(3 << 4 << 5) << 3ll << 24 << 1024);
    auto [accArrTag, accArrVal] = convertFromBSONArray(bsonAccArr);
    _aggAccessor.reset(true, accArrTag, accArrVal);

    auto bsonInputArr = BSON_ARRAY(BSONArrayBuilder().arr() << 3ll << 0 << 1024);
    auto [inputArrTag, inputArrVal] = convertFromBSONArray(bsonInputArr);
    _inputAccessor.reset(true, inputArrTag, inputArrVal);

    auto [resultTag, resultVal] = runCompiledExpression(compiledExpr.get());
    _aggAccessor.reset(true, resultTag, resultVal);
    std::tie(resultTag, resultVal) = runCompiledExpression(finalizeCompiledExpr.get());

    auto expectedArray = BSON_ARRAY(3 << 4 << 5);
    auto [compareTag, compareVal] =
        sbe::value::compareValue(resultTag,
                                 resultVal,
                                 sbe::value::TypeTags::bsonArray,
                                 sbe::value::bitcastFrom<const char*>(expectedArray.objdata()));

    ASSERT_EQ(resultTag, sbe::value::TypeTags::Array);
    ASSERT_EQ(compareTag, sbe::value::TypeTags::NumberInt32);
    ASSERT_EQ(compareVal, 0);
    sbe::value::releaseValue(resultTag, resultVal);
}
}  // namespace mongo
