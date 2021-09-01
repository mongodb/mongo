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

#include "mongo/db/exec/sbe/util/debug_print.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/sbe_stage_builder_accumulator.h"
#include "mongo/db/query/sbe_stage_builder_eval_frame.h"
#include "mongo/db/query/sbe_stage_builder_expression.h"
#include "mongo/db/query/sbe_stage_builder_helpers.h"
#include "mongo/db/query/sbe_stage_builder_test_fixture.h"
#include "mongo/platform/basic.h"
#include "mongo/unittest/unittest.h"

namespace mongo {

const NamespaceString kTestNss("TestDB", "TestColl");

class SbeAccumulatorBuilderTest : public SbeStageBuilderTestFixture {
protected:
    std::unique_ptr<QuerySolutionNode> makeVirtualScanTree(std::vector<BSONArray> docs) {
        return std::make_unique<VirtualScanNode>(docs, VirtualScanNode::ScanType::kCollScan, false);
    }

    AccumulationStatement makeAccumulator(ExpressionContext* expCtx, BSONElement elem) {
        auto vps = expCtx->variablesParseState;
        return AccumulationStatement::parseAccumulationStatement(expCtx, elem, vps);
    }

    std::unique_ptr<CanonicalQuery> canonicalize(const char* queryStr) {
        BSONObj queryObj = fromjson(queryStr);
        auto findCommand = std::make_unique<FindCommandRequest>(kTestNss);
        findCommand->setFilter(queryObj);
        boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
        auto statusWithCQ =
            CanonicalQuery::canonicalize(expCtx->opCtx,
                                         std::move(findCommand),
                                         false,
                                         expCtx,
                                         ExtensionsCallbackNoop(),
                                         MatchExpressionParser::kAllowAllSpecialFeatures);

        ASSERT_OK(statusWithCQ.getStatus());
        return std::move(statusWithCQ.getValue());
    }

    stage_builder::StageBuilderState makeStageBuilderState() {
        boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
        auto cq = canonicalize("{dummy: 'query'}");
        auto env =
            stage_builder::makeRuntimeEnvironment(*(cq.get()), expCtx->opCtx, &_slotIdGenerator);
        return stage_builder::StageBuilderState(expCtx->opCtx,
                                                env.get(),
                                                cq->getExpCtxRaw()->variables,
                                                &_slotIdGenerator,
                                                &_frameIdGenerator,
                                                &_spoolIdGenerator);
    }

    std::pair<sbe::value::TypeTags, sbe::value::Value> getResultsForAggregationWithNoGroupByTest(
        StringData queryStatement, std::vector<BSONArray> docs) {
        auto expCtx = ExpressionContextForTest{};
        auto acc = fromjson(queryStatement.rawData());
        auto accStmt = makeAccumulator(&expCtx, acc.firstElement());

        // Build a VirtualScan input sub-tree to feed test docs into the argument expression.
        auto querySolution = makeQuerySolution(makeVirtualScanTree(docs));
        auto [resultSlots, stage, data] =
            buildPlanStage(std::move(querySolution), false /* hasRecordId */, nullptr);

        stage_builder::EvalStage evalStage;
        evalStage.stage = std::move(stage);

        auto state = makeStageBuilderState();
        auto [argExpr, argStage] =
            stage_builder::buildArgument(state,
                                         accStmt,
                                         std::move(evalStage),
                                         resultSlots.front() /* see comment for buildPlanStage */,
                                         kEmptyPlanNodeId);

        auto [aggExprs, accStage] = stage_builder::buildAccumulator(
            state, accStmt, std::move(argStage), std::move(argExpr), kEmptyPlanNodeId);

        sbe::value::SlotMap<std::unique_ptr<sbe::EExpression>> aggs;
        sbe::value::SlotVector aggSlots;
        for (auto& expr : aggExprs) {
            auto slot = state.slotId();
            aggSlots.push_back(slot);
            aggs[slot] = std::move(expr);
        }
        auto groupStage = makeHashAgg(
            std::move(accStage), sbe::makeSV(), std::move(aggs), boost::none, kEmptyPlanNodeId);

        auto [finalExpr, finalStage] = stage_builder::buildFinalize(
            state, accStmt, std::move(aggSlots), std::move(groupStage), kEmptyPlanNodeId);

        auto outSlot = state.slotId();
        auto outStage =
            makeProject(std::move(finalStage), kEmptyPlanNodeId, outSlot, std::move(finalExpr));

        auto resultAccessors = prepareTree(&data.ctx, outStage.stage.get(), outSlot);
        return getAllResults(outStage.stage.get(), &resultAccessors[0]);
    }

    void runAggregationWithNoGroupByTest(StringData queryStatement,
                                         std::vector<BSONArray> docs,
                                         const struct mongo::BSONArray& expectedValue) {
        auto [resultsTag, resultsVal] =
            getResultsForAggregationWithNoGroupByTest(queryStatement, docs);
        sbe::value::ValueGuard resultGuard{resultsTag, resultsVal};
        auto [expectedTag, expectedVal] = stage_builder::makeValue(expectedValue);
        sbe::value::ValueGuard expectedGuard{expectedTag, expectedVal};
        ASSERT_TRUE(valueEquals(resultsTag, resultsVal, expectedTag, expectedVal))
            << "expected: " << std::make_pair(expectedTag, expectedVal)
            << " but got: " << std::make_pair(resultsTag, resultsVal);
    }

    void runAggregationWithGroupByTest(StringData queryStatement,
                                       std::vector<BSONArray> docs,
                                       std::vector<StringData> groupByFields,
                                       const struct mongo::BSONArray& expectedValue) {
        // This test simulates translation a $group with group-by statements on groupByFields.
        auto expCtx = ExpressionContextForTest{};

        // Build the a VirtualScan input sub-tree to feed test docs into the argument expression.
        auto querySolution = makeQuerySolution(makeVirtualScanTree(docs));
        auto [resultSlots, stage, data] = buildPlanStage(std::move(querySolution), false, nullptr);
        stage_builder::EvalStage groupByStage;
        groupByStage.stage = std::move(stage);

        auto state = makeStageBuilderState();
        auto acc = fromjson(queryStatement.rawData());
        auto accStmt = makeAccumulator(&expCtx, acc.firstElement());

        // Translate the the group-by field path and bind it to a slot in a project stage.
        auto vps = expCtx.variablesParseState;
        stage_builder::EvalExpr groupByExpr;
        for (auto&& groupByField : groupByFields) {
            auto groupByExpression =
                ExpressionFieldPath::parse(&expCtx, groupByField.rawData(), vps);
            std::tie(groupByExpr, groupByStage) = stage_builder::generateExpression(
                state,
                groupByExpression.get(),
                std::move(groupByStage),  // NOLINT(bugprone-use-after-move)
                resultSlots.front() /* See comment for buildPlanStage */,
                kEmptyPlanNodeId);
        }

        auto [groupBySlot, projectGroupByStage] = projectEvalExpr(std::move(groupByExpr),
                                                                  std::move(groupByStage),
                                                                  kEmptyPlanNodeId,
                                                                  state.slotIdGenerator);

        auto [argExpr, argStage] =
            stage_builder::buildArgument(state,
                                         accStmt,
                                         std::move(projectGroupByStage),
                                         resultSlots.front(), /* See comment for buildPlanStage*/
                                         kEmptyPlanNodeId);

        auto [aggExprs, accStage] = stage_builder::buildAccumulator(
            state, accStmt, std::move(argStage), std::move(argExpr), kEmptyPlanNodeId);

        sbe::value::SlotMap<std::unique_ptr<sbe::EExpression>> aggs;
        sbe::value::SlotVector aggSlots;
        for (auto& expr : aggExprs) {
            auto slot = state.slotId();
            aggSlots.push_back(slot);
            aggs[slot] = std::move(expr);
        }
        auto groupStage = makeHashAgg(std::move(accStage),
                                      sbe::makeSV(groupBySlot),
                                      std::move(aggs),
                                      boost::none,
                                      kEmptyPlanNodeId);

        // Build the finalize stage over the collected accumulators.
        auto [finalExpr, finalStage] = stage_builder::buildFinalize(
            state, accStmt, aggSlots, std::move(groupStage), kEmptyPlanNodeId);

        auto outSlot = state.slotId();
        auto outStage =
            makeProject(std::move(finalStage), kEmptyPlanNodeId, outSlot, std::move(finalExpr));

        auto resultAccessors = prepareTree(&data.ctx, outStage.stage.get(), outSlot);
        auto [resultsTag, resultsVal] = getAllResults(outStage.stage.get(), &resultAccessors[0]);
        sbe::value::ValueGuard resultGuard{resultsTag, resultsVal};

        auto [sortedResultsTag, sortedResultsVal] = sortResults(resultsTag, resultsVal);
        sbe::value::ValueGuard sortedResultGuard{sortedResultsTag, sortedResultsVal};

        auto [expectedTag, expectedVal] = stage_builder::makeValue(expectedValue);
        sbe::value::ValueGuard expectedGuard{expectedTag, expectedVal};

        ASSERT_TRUE(valueEquals(sortedResultsTag, sortedResultsVal, expectedTag, expectedVal));
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

    /**
     * $addToSet doesn't guarantee any particular order of the accumulated values. To make the
     * result checking deterministic, we convert the array of the expected values into an arraySet
     * as well and compare the sets.
     * Note: Currently, the order agnostic comparison only works for arraySets with
     * non-pointer-based accumulated values.
     */
    void runAddToSetTest(StringData queryStatement,
                         std::vector<BSONArray> docs,
                         const BSONArray& expectedResult) {
        using namespace mongo::sbe::value;

        // Create ArraySet Value from the expectedResult BSON Array.
        auto [tmpTag, tmpVal] =
            copyValue(TypeTags::bsonArray, bitcastFrom<const char*>(expectedResult.objdata()));
        ValueGuard tmpGuard{tmpTag, tmpVal};
        auto [expectedTag, expectedSet] = arrayToSet(tmpTag, tmpVal);
        ValueGuard expectedValueGuard{expectedTag, expectedSet};

        // Run the accumulator.
        auto [resultsTag, resultsVal] =
            getResultsForAggregationWithNoGroupByTest(queryStatement, docs);
        ValueGuard resultGuard{resultsTag, resultsVal};
        ASSERT_EQ(resultsTag, TypeTags::Array);

        // Extract the accumulated ArraySet from the result and compare it to the expected.
        auto arr = getArrayView(resultsVal);
        ASSERT_EQ(1, arr->size());

        auto [aggregatedTag, aggregatedSet] = arr->getAt(0);
        ASSERT_EQ(aggregatedTag, TypeTags::ArraySet);

        // TODO SERVER-59631: replace the lookup loop below with a call to valueEquals on sets.
        // ASSERT(valueEquals(expectedTag, expectedSet, aggregatedTag, aggregatedSet))
        //     << "expected set: " << std::make_pair(expectedTag, expectedSet)
        //     << " but got set: " << std::make_pair(aggregatedTag, aggregatedSet);
        const auto& lhsSet = getArraySetView(expectedSet)->values();
        const auto& rhsSet = getArraySetView(aggregatedSet)->values();
        bool areEqual = (lhsSet.size() == rhsSet.size());
        if (areEqual) {
            for (const auto& e : rhsSet) {
                if (!lhsSet.contains(e)) {
                    areEqual = false;
                    break;
                }
            }
        }
        ASSERT(areEqual) << "expected set: " << std::make_pair(expectedTag, expectedSet)
                         << " but got set: " << std::make_pair(aggregatedTag, aggregatedSet);
    }

private:
    sbe::value::SlotIdGenerator _slotIdGenerator;
    sbe::value::FrameIdGenerator _frameIdGenerator;
    sbe::value::SpoolIdGenerator _spoolIdGenerator;
};

TEST_F(SbeAccumulatorBuilderTest, MinAccumulatorTranslationBasic) {
    auto docs = std::vector<BSONArray>{BSON_ARRAY(BSON("a" << 1 << "b" << 100ll)),
                                       BSON_ARRAY(BSON("a" << 1 << "b" << Decimal128(10.0))),
                                       BSON_ARRAY(BSON("a" << 1 << "b" << 1.0))};
    runAggregationWithNoGroupByTest("{x: {$min: '$b'}}", docs, BSON_ARRAY(1.0));
}

TEST_F(SbeAccumulatorBuilderTest, MinAccumulatorTranslationAllUndefined) {
    auto docs = std::vector<BSONArray>{BSON_ARRAY(BSON("a" << 1 << "b" << BSONUndefined))};
    runAggregationWithNoGroupByTest("{x: {$min: '$b'}}", docs, BSON_ARRAY(BSONNULL));
}

TEST_F(SbeAccumulatorBuilderTest, MinAccumulatorTranslationSomeUndefined) {
    auto docs = std::vector<BSONArray>{BSON_ARRAY(BSON("a" << 1 << "b" << BSONUndefined)),
                                       BSON_ARRAY(BSON("a" << 1 << "b" << 1))};
    runAggregationWithNoGroupByTest("{x: {$min: '$b'}}", docs, BSON_ARRAY(1));
}

TEST_F(SbeAccumulatorBuilderTest, MinAccumulatorTranslationSomeNull) {
    auto docs = std::vector<BSONArray>{BSON_ARRAY(BSON("a" << 1 << "b" << BSONNULL)),
                                       BSON_ARRAY(BSON("a" << 1 << "b" << 10)),
                                       BSON_ARRAY(BSON("a" << 1 << "b" << 1)),
                                       BSON_ARRAY(BSON("a" << 1 << "b" << BSONNULL))};
    runAggregationWithNoGroupByTest("{x: {$min: '$b'}}", docs, BSON_ARRAY(1));
}

TEST_F(SbeAccumulatorBuilderTest, MinAccumulatorTranslationAllNull) {
    auto docs = std::vector<BSONArray>{BSON_ARRAY(BSON("a" << 1 << "b" << BSONNULL)),
                                       BSON_ARRAY(BSON("a" << 1 << "b" << BSONNULL)),
                                       BSON_ARRAY(BSON("a" << 1 << "b" << BSONNULL))};
    runAggregationWithNoGroupByTest("{x: {$min: '$b'}}", docs, BSON_ARRAY(BSONNULL));
}

TEST_F(SbeAccumulatorBuilderTest, MinAccumulatorTranslationAllMissingFields) {
    auto docs = std::vector<BSONArray>{BSON_ARRAY(BSON("a" << 1))};
    runAggregationWithNoGroupByTest("{x: {$min: '$b'}}", docs, BSON_ARRAY(BSONNULL));
}

TEST_F(SbeAccumulatorBuilderTest, MinAccumulatorTranslationSomeMissingFields) {
    auto docs = std::vector<BSONArray>{BSON_ARRAY(BSON("a" << 1)),
                                       BSON_ARRAY(BSON("a" << 1 << "b" << 1)),
                                       BSON_ARRAY(BSON("a" << 1 << "b" << 100))};
    runAggregationWithNoGroupByTest("{x: {$min: '$b'}}", docs, BSON_ARRAY(1));
}

TEST_F(SbeAccumulatorBuilderTest, MinAccumulatorTranslationMixedTypes) {
    auto docs = std::vector<BSONArray>{BSON_ARRAY(BSON("a" << 1 << "b" << BSON_ARRAY(1 << 2))),
                                       BSON_ARRAY(BSON("a" << 1 << "b" << 1)),
                                       BSON_ARRAY(BSON("a" << 1 << "b" << BSON("c" << 1)))};
    runAggregationWithNoGroupByTest("{x: {$min: '$b'}}", docs, BSON_ARRAY(1));
}

TEST_F(SbeAccumulatorBuilderTest, MinAccumulatorTranslationOneGroupBy) {
    auto docs = std::vector<BSONArray>{BSON_ARRAY(BSON("a" << 1 << "b" << 0)),
                                       BSON_ARRAY(BSON("a" << 1 << "b" << 100)),
                                       BSON_ARRAY(BSON("a" << 2 << "b" << 1))};
    runAggregationWithGroupByTest("{x: {$min: '$b'}}", docs, {"$a"}, BSON_ARRAY(0 << 1));
}

TEST_F(SbeAccumulatorBuilderTest, MaxAccumulatorTranslationBasic) {
    auto docs = std::vector<BSONArray>{BSON_ARRAY(BSON("a" << 1 << "b" << 100ll)),
                                       BSON_ARRAY(BSON("a" << 1 << "b" << Decimal128(10.0))),
                                       BSON_ARRAY(BSON("a" << 1 << "b" << 1.0))};
    runAggregationWithNoGroupByTest("{x: {$max: '$b'}}", docs, BSON_ARRAY(100));
}

TEST_F(SbeAccumulatorBuilderTest, MaxAccumulatorTranslationSomeNull) {
    auto docs = std::vector<BSONArray>{BSON_ARRAY(BSON("a" << 1 << "b" << BSONNULL)),
                                       BSON_ARRAY(BSON("a" << 1 << "b" << 10)),
                                       BSON_ARRAY(BSON("a" << 1 << "b" << 1)),
                                       BSON_ARRAY(BSON("a" << 1 << "b" << BSONNULL))};
    runAggregationWithNoGroupByTest("{x: {$max: '$b'}}", docs, BSON_ARRAY(10));
}

TEST_F(SbeAccumulatorBuilderTest, MaxAccumulatorTranslationAllNull) {
    auto docs = std::vector<BSONArray>{BSON_ARRAY(BSON("a" << 1 << "b" << BSONNULL)),
                                       BSON_ARRAY(BSON("a" << 1 << "b" << BSONNULL)),
                                       BSON_ARRAY(BSON("a" << 1 << "b" << BSONNULL))};
    runAggregationWithNoGroupByTest("{x: {$max: '$b'}}", docs, BSON_ARRAY(BSONNULL));
}

TEST_F(SbeAccumulatorBuilderTest, MaxAccumulatorTranslationAllMissingFields) {
    auto docs = std::vector<BSONArray>{BSON_ARRAY(BSON("a" << 1))};
    runAggregationWithNoGroupByTest("{x: {$max: '$b'}}", docs, BSON_ARRAY(BSONNULL));
}

TEST_F(SbeAccumulatorBuilderTest, MaxAccumulatorTranslationSomeMissingFields) {
    auto docs = std::vector<BSONArray>{BSON_ARRAY(BSON("a" << 1)),
                                       BSON_ARRAY(BSON("a" << 1 << "b" << 1)),
                                       BSON_ARRAY(BSON("a" << 1 << "b" << 100))};
    runAggregationWithNoGroupByTest("{x: {$max: '$b'}}", docs, BSON_ARRAY(100));
}

TEST_F(SbeAccumulatorBuilderTest, MaxAccumulatorTranslationMixedTypes) {
    auto docs = std::vector<BSONArray>{BSON_ARRAY(BSON("a" << 1 << "b" << BSON_ARRAY(1 << 2))),
                                       BSON_ARRAY(BSON("a" << 1 << "b" << 1)),
                                       BSON_ARRAY(BSON("a" << 1 << "b" << BSON("c" << 1)))};
    runAggregationWithNoGroupByTest("{x: {$max: '$b'}}", docs, BSON_ARRAY(BSON_ARRAY(1 << 2)));
}

TEST_F(SbeAccumulatorBuilderTest, MaxAccumulatorTranslationOneGroupBy) {
    auto docs = std::vector<BSONArray>{BSON_ARRAY(BSON("a" << 1 << "b" << 0)),
                                       BSON_ARRAY(BSON("a" << 1 << "b" << 100)),
                                       BSON_ARRAY(BSON("a" << 2 << "b" << 1))};
    runAggregationWithGroupByTest("{x: {$max: '$b'}}", docs, {"$a"}, BSON_ARRAY(1 << 100));
}

TEST_F(SbeAccumulatorBuilderTest, FirstAccumulatorTranslationOneDoc) {
    auto docs = std::vector<BSONArray>{BSON_ARRAY(BSON("a" << 1 << "b" << BSON_ARRAY(1 << 2)))};
    runAggregationWithNoGroupByTest("{x: {$first: '$b'}}", docs, BSON_ARRAY(BSON_ARRAY(1 << 2)));
}

TEST_F(SbeAccumulatorBuilderTest, FirstAccumulatorTranslationMissingField) {
    auto docs = std::vector<BSONArray>{BSON_ARRAY(BSON("a" << 1))};
    runAggregationWithNoGroupByTest("{x: {$first: '$b'}}", docs, BSON_ARRAY(BSONNULL));
}

TEST_F(SbeAccumulatorBuilderTest, FirstAccumulatorTranslationBasic) {
    auto docs = std::vector<BSONArray>{BSON_ARRAY(BSON("a" << 1 << "b" << 100)),
                                       BSON_ARRAY(BSON("a" << 1 << "b" << 0))};
    runAggregationWithNoGroupByTest("{x: {$first: '$b'}}", docs, BSON_ARRAY(100));
}

TEST_F(SbeAccumulatorBuilderTest, FirstAccumulatorTranslationFirstDocWithMissingField) {
    auto docs =
        std::vector<BSONArray>{BSON_ARRAY(BSON("a" << 1)), BSON_ARRAY(BSON("a" << 1 << "b" << 0))};
    runAggregationWithNoGroupByTest("{x: {$first: '$b'}}", docs, BSON_ARRAY(BSONNULL));
}

TEST_F(SbeAccumulatorBuilderTest, FirstAccumulatorTranslationOneGroupBy) {
    auto docs = std::vector<BSONArray>{BSON_ARRAY(BSON("a" << 1 << "b" << 0)),
                                       BSON_ARRAY(BSON("a" << 1 << "b" << 100)),
                                       BSON_ARRAY(BSON("a" << 2 << "b" << 2.5))};
    runAggregationWithGroupByTest("{x: {$min: '$b'}}", docs, {"$a"}, BSON_ARRAY(0 << 2.5));
}

TEST_F(SbeAccumulatorBuilderTest, LastAccumulatorTranslationOneDoc) {
    auto docs = std::vector<BSONArray>{BSON_ARRAY(BSON("a" << 1 << "b" << BSON_ARRAY(1 << 2)))};
    runAggregationWithNoGroupByTest("{x: {$last: '$b'}}", docs, BSON_ARRAY(BSON_ARRAY(1 << 2)));
}

TEST_F(SbeAccumulatorBuilderTest, LastAccumulatorTranslationMissingField) {
    auto docs = std::vector<BSONArray>{BSON_ARRAY(BSON("a" << 1))};
    runAggregationWithNoGroupByTest("{x: {$last: '$b'}}", docs, BSON_ARRAY(BSONNULL));
}

TEST_F(SbeAccumulatorBuilderTest, LastAccumulatorTranslationBasic) {
    auto docs = std::vector<BSONArray>{BSON_ARRAY(BSON("a" << 1 << "b" << 100)),
                                       BSON_ARRAY(BSON("a" << 1 << "b" << 0))};
    runAggregationWithNoGroupByTest("{x: {$last: '$b'}}", docs, BSON_ARRAY(0));
}

TEST_F(SbeAccumulatorBuilderTest, LastAccumulatorTranslationLastDocWithMissingField) {
    auto docs =
        std::vector<BSONArray>{BSON_ARRAY(BSON("a" << 1 << "b" << 0)), BSON_ARRAY(BSON("a" << 1))};
    runAggregationWithNoGroupByTest("{x: {$last: '$b'}}", docs, BSON_ARRAY(BSONNULL));
}

TEST_F(SbeAccumulatorBuilderTest, LastAccumulatorTranslationOneGroupBy) {
    auto docs = std::vector<BSONArray>{BSON_ARRAY(BSON("a" << 1 << "b" << 0)),
                                       BSON_ARRAY(BSON("a" << 1 << "b" << 100)),
                                       BSON_ARRAY(BSON("a" << 2 << "b" << 2.5))};
    runAggregationWithGroupByTest("{x: {$last: '$b'}}", docs, {"$a"}, BSON_ARRAY(2.5 << 100));
}

TEST_F(SbeAccumulatorBuilderTest, AvgAccumulatorTranslation) {
    // Parse the test $avg accumulator into an AccumulationStatement.
    auto expCtx = ExpressionContextForTest{};
    auto sumStmt = fromjson("{x: {$avg: '$b'}}");
    auto accStmt = makeAccumulator(&expCtx, sumStmt.firstElement());

    auto docs = std::vector<BSONArray>{BSON_ARRAY(BSON("a" << 1 << "b" << 2)),
                                       BSON_ARRAY(BSON("a" << 2 << "b" << Decimal128(4.0))),
                                       BSON_ARRAY(BSON("a" << 3 << "b" << 6ll))};

    // Build the a VirtualScan input sub-tree to feed test docs into the argument expression.
    auto querySolution = makeQuerySolution(makeVirtualScanTree(docs));
    auto [resultSlots, stage, data] = buildPlanStage(std::move(querySolution), false, nullptr);

    stage_builder::EvalStage evalStage;
    evalStage.stage = std::move(stage);

    auto state = makeStageBuilderState();
    auto [argExpr, argStage] =
        stage_builder::buildArgument(state,
                                     accStmt,
                                     std::move(evalStage),
                                     resultSlots.front() /* See comment for buildPlanStage */,
                                     kEmptyPlanNodeId);

    // The accumulator expression for translation of $avg will have two agg expressions, a
    // sum(..) and a count which is implemented as sum(1).
    auto [aggExprs, accStage] = stage_builder::buildAccumulator(
        state, accStmt, std::move(argStage), std::move(argExpr), kEmptyPlanNodeId);

    // Build a HashAgg stage to implement a group-by with two agg expressions.
    sbe::value::SlotMap<std::unique_ptr<sbe::EExpression>> aggs;
    sbe::value::SlotVector aggSlots;
    for (auto& expr : aggExprs) {
        auto slot = state.slotId();
        aggSlots.push_back(slot);
        aggs[slot] = std::move(expr);
    }
    auto groupStage = makeHashAgg(
        std::move(accStage), sbe::makeSV(), std::move(aggs), boost::none, kEmptyPlanNodeId);

    // The finalization step for $avg translation will produce a divide expression that takes
    // the two group-by slots as input and binds an 'outSlot' that will hold the result of the
    // final result of $avg.
    auto [finalExpr, finalStage] = stage_builder::buildFinalize(
        state, accStmt, aggSlots, std::move(groupStage), kEmptyPlanNodeId);

    auto outSlot = state.slotId();
    auto outStage =
        makeProject(std::move(finalStage), kEmptyPlanNodeId, outSlot, std::move(finalExpr));

    // Prepare the sbe::PlanStage for execution and collect all results in order to assert that
    // sum(2, 4, 6) / 3 == 4.
    auto resultAccessors = prepareTree(&data.ctx, outStage.stage.get(), outSlot);
    auto [resultsTag, resultsVal] = getAllResults(outStage.stage.get(), &resultAccessors[0]);
    sbe::value::ValueGuard resultGuard{resultsTag, resultsVal};

    auto [expectedTag, expectedVal] = stage_builder::makeValue(BSON_ARRAY(4));
    sbe::value::ValueGuard expectedGuard{expectedTag, expectedVal};
    ASSERT_TRUE(valueEquals(resultsTag, resultsVal, expectedTag, expectedVal));
}

TEST_F(SbeAccumulatorBuilderTest, TwoAvgAccumulatorTranslation) {
    // This test simulates translation a $group with two accumulator statements.
    auto expCtx = ExpressionContextForTest{};
    auto docs = std::vector<BSONArray>{BSON_ARRAY(BSON("a" << 10 << "b" << 1ll)),
                                       BSON_ARRAY(BSON("a" << 20 << "b" << Decimal128(2.0))),
                                       BSON_ARRAY(BSON("a" << 30 << "b" << 3))};

    // Build the a VirtualScan input sub-tree to feed test docs into the argument expression.
    auto querySolution = makeQuerySolution(makeVirtualScanTree(docs));
    auto [resultSlots, stage, data] = buildPlanStage(std::move(querySolution), false, nullptr);
    stage_builder::EvalStage evalStage;
    evalStage.stage = std::move(stage);

    auto accs =
        std::vector<BSONObj>{{fromjson("{x: {$avg: '$a'}}")}, {fromjson("{y: {$avg: '$b'}}")}};

    // Translate the two argument Expressions.
    std::unique_ptr<sbe::EExpression> argExpr;
    std::vector<std::unique_ptr<sbe::EExpression>> accExprs;
    sbe::value::SlotMap<std::unique_ptr<sbe::EExpression>> aggs;
    std::vector<sbe::value::SlotVector> accAggSlots;
    std::vector<AccumulationStatement> accStmts;
    auto state = makeStageBuilderState();
    for (auto& acc : accs) {
        auto accStmt = makeAccumulator(&expCtx, acc.firstElement());
        accStmts.push_back(accStmt);

        std::tie(argExpr, evalStage) =
            stage_builder::buildArgument(state,
                                         accStmt,
                                         std::move(evalStage),  // NOLINT(bugprone-use-after-move)
                                         resultSlots.front() /* See comment for buildPlanStage */,
                                         kEmptyPlanNodeId);

        // The accumulator expression for translation of $avg will have two agg expressions, a
        // sum(..) and a count which is implemented as sum(1).
        std::tie(accExprs, evalStage) = stage_builder::buildAccumulator(
            state,
            accStmt,
            std::move(evalStage),  // NOLINT(bugprone-use-after-move)
            std::move(argExpr),
            kEmptyPlanNodeId);

        sbe::value::SlotVector aggSlots;
        for (auto& expr : accExprs) {
            auto slot = state.slotId();
            aggSlots.push_back(slot);
            aggs[slot] = std::move(expr);
        }
        accAggSlots.emplace_back(std::move(aggSlots));
    }

    auto groupStage = makeHashAgg(
        std::move(evalStage), sbe::makeSV(), std::move(aggs), boost::none, kEmptyPlanNodeId);

    // Build the finalize stage over the collected accumulators.
    sbe::value::SlotMap<std::unique_ptr<sbe::EExpression>> projects;
    sbe::value::SlotVector finalSlots;
    std::unique_ptr<sbe::EExpression> finalExpr;
    for (size_t i = 0; i < accs.size(); ++i) {
        std::tie(finalExpr, groupStage) =
            stage_builder::buildFinalize(state,
                                         accStmts[i],
                                         accAggSlots[i],
                                         std::move(groupStage),  // NOLINT(bugprone-use-after-move)
                                         kEmptyPlanNodeId);

        auto outSlot = state.slotId();
        finalSlots.push_back(outSlot);
        projects[outSlot] = std::move(finalExpr);
    }

    auto finalStage = makeProject(std::move(groupStage), std::move(projects), kEmptyPlanNodeId);

    // Prepare the sbe::PlanStage for execution and collect all results in order to assert that
    // the avg of the 'a' fields is 20 and the avg of 'b' fields is 2.
    auto resultAccessors = prepareTree(&data.ctx, finalStage.stage.get(), finalSlots);
    auto [resultsTag, resultsVal] = getAllResultsMulti(finalStage.stage.get(), resultAccessors);
    sbe::value::ValueGuard resultGuard{resultsTag, resultsVal};

    auto [expectedTag, expectedVal] = stage_builder::makeValue(BSON_ARRAY(BSON_ARRAY(20 << 2)));
    sbe::value::ValueGuard expectedGuard{expectedTag, expectedVal};

    ASSERT_TRUE(valueEquals(resultsTag, resultsVal, expectedTag, expectedVal));
}

TEST_F(SbeAccumulatorBuilderTest, AvgAccumulatorOneGroupByTranslation) {
    // This test simulates translation a $group with a group-by statement on '$a'.
    auto expCtx = ExpressionContextForTest{};
    auto docs = std::vector<BSONArray>{BSON_ARRAY(BSON("a" << 1 << "b" << 1.0)),
                                       BSON_ARRAY(BSON("a" << 1 << "b" << 2ll)),
                                       BSON_ARRAY(BSON("a" << 1 << "b" << Decimal128(3.0))),
                                       BSON_ARRAY(BSON("a" << 2 << "b" << 4ll)),
                                       BSON_ARRAY(BSON("a" << 2 << "b" << 5)),
                                       BSON_ARRAY(BSON("a" << 2 << "b" << 6.0))};

    // Build the a VirtualScan input sub-tree to feed test docs into the argument expression.
    auto querySolution = makeQuerySolution(makeVirtualScanTree(docs));
    auto [resultSlots, stage, data] = buildPlanStage(std::move(querySolution), false, nullptr);
    stage_builder::EvalStage evalStage;
    evalStage.stage = std::move(stage);

    auto state = makeStageBuilderState();
    auto acc = fromjson("{x: {$avg: '$b'}}");
    auto accStmt = makeAccumulator(&expCtx, acc.firstElement());

    // Translate the the group-by field path and bind it to a slot in a project stage.
    auto vps = expCtx.variablesParseState;
    auto groupByExpression = ExpressionFieldPath::parse(&expCtx, "$a", vps);
    auto [groupByExpr, groupByStage] =
        stage_builder::generateExpression(state,
                                          groupByExpression.get(),
                                          std::move(evalStage),
                                          resultSlots.front() /* See comment for buildPlanStage */,
                                          kEmptyPlanNodeId);

    auto [groupBySlot, projectGroupByStage] = projectEvalExpr(
        std::move(groupByExpr), std::move(groupByStage), kEmptyPlanNodeId, state.slotIdGenerator);

    auto [argExpr, argStage] =
        stage_builder::buildArgument(state,
                                     accStmt,
                                     std::move(projectGroupByStage),
                                     resultSlots.front() /* See comment for buildPlanStage */,
                                     kEmptyPlanNodeId);

    // The accumulator expression for translation of $avg will have two agg expressions, a
    // sum(..) and a count which is implemented as sum(1).
    auto [aggExprs, accStage] = stage_builder::buildAccumulator(
        state, accStmt, std::move(argStage), std::move(argExpr), kEmptyPlanNodeId);

    sbe::value::SlotMap<std::unique_ptr<sbe::EExpression>> aggs;
    sbe::value::SlotVector aggSlots;
    for (auto& expr : aggExprs) {
        auto slot = state.slotId();
        aggSlots.push_back(slot);
        aggs[slot] = std::move(expr);
    }
    auto groupStage = makeHashAgg(std::move(accStage),
                                  sbe::makeSV(groupBySlot),
                                  std::move(aggs),
                                  boost::none,
                                  kEmptyPlanNodeId);


    // Build the finalize stage over the collected accumulators.
    auto [finalExpr, finalStage] = stage_builder::buildFinalize(
        state, accStmt, aggSlots, std::move(groupStage), kEmptyPlanNodeId);

    auto outSlot = state.slotId();
    auto outStage =
        makeProject(std::move(finalStage), kEmptyPlanNodeId, outSlot, std::move(finalExpr));

    // Prepare the sbe::PlanStage for execution and collect all results in order to assert that The
    // expected averages for each '$a' group are a:1 == 2 and a:2 == 5.
    auto resultAccessors = prepareTree(&data.ctx, outStage.stage.get(), outSlot);
    auto [resultsTag, resultsVal] = getAllResults(outStage.stage.get(), &resultAccessors[0]);
    sbe::value::ValueGuard resultGuard{resultsTag, resultsVal};

    // Sort results for stable compare, since the averages could come out in any order
    auto [sortedResultsTag, sortedResultsVal] = sortResults(resultsTag, resultsVal);
    sbe::value::ValueGuard sortedResultGuard{sortedResultsTag, sortedResultsVal};

    auto [expectedTag, expectedVal] = stage_builder::makeValue(BSON_ARRAY(2.0 << 5));
    sbe::value::ValueGuard expectedGuard{expectedTag, expectedVal};

    ASSERT_TRUE(valueEquals(sortedResultsTag, sortedResultsVal, expectedTag, expectedVal));
}

TEST_F(SbeAccumulatorBuilderTest, SumAccumulatorTranslationBasic) {
    auto docs = std::vector<BSONArray>{BSON_ARRAY(BSON("a" << 1 << "b" << 2)),
                                       BSON_ARRAY(BSON("a" << 1 << "b" << 4)),
                                       BSON_ARRAY(BSON("a" << 1 << "b" << 6))};
    runAggregationWithNoGroupByTest("{x: {$sum: '$b'}}", docs, BSON_ARRAY(12));
}

TEST_F(SbeAccumulatorBuilderTest, SumAccumulatorTranslationOneDocInt) {
    runAggregationWithNoGroupByTest(
        "{x: {$sum: '$b'}}", {BSON_ARRAY(BSON("a" << 1 << "b" << 10))}, BSON_ARRAY(Value(10)));
}

TEST_F(SbeAccumulatorBuilderTest, SumAccumulatorTranslationOneDocLong) {
    runAggregationWithNoGroupByTest(
        "{x: {$sum: '$b'}}",
        std::vector<BSONArray>{BSON_ARRAY(BSON("a" << 3 << "b" << 10ll))},
        BSON_ARRAY(10ll));
}

TEST_F(SbeAccumulatorBuilderTest, SumAccumulatorTranslationOneDocIntRepresentableDouble) {
    runAggregationWithNoGroupByTest(
        "{x: {$sum: '$b'}}",
        std::vector<BSONArray>{BSON_ARRAY(BSON("a" << 4 << "b" << 10.0))},
        BSON_ARRAY(10.0));
}

TEST_F(SbeAccumulatorBuilderTest, SumAccumulatorTranslationOneDocNonIntRepresentableLong) {
    runAggregationWithNoGroupByTest(
        "{x: {$sum: '$b'}}",
        std::vector<BSONArray>{BSON_ARRAY(BSON("a" << 5 << "b" << 60000000000ll))},
        BSON_ARRAY(60000000000ll));
}

TEST_F(SbeAccumulatorBuilderTest, SumAccumulatorTranslationNonIntegerValuedDouble) {
    runAggregationWithNoGroupByTest(
        "{x: {$sum: '$b'}}",
        std::vector<BSONArray>{BSON_ARRAY(BSON("a" << 1 << "b" << 9.5))},
        BSON_ARRAY(9.5));
}

TEST_F(SbeAccumulatorBuilderTest, SumAccumulatorTranslationOneDocNanDouble) {
    runAggregationWithNoGroupByTest(
        "{x: {$sum: '$b'}}",
        std::vector<BSONArray>{
            BSON_ARRAY(BSON("a" << 6 << "b" << std::numeric_limits<double>::quiet_NaN()))},
        BSON_ARRAY(Value(std::numeric_limits<double>::quiet_NaN())));
}

TEST_F(SbeAccumulatorBuilderTest, SumAccumulatorTranslationIntAndLong) {
    auto docs = std::vector<BSONArray>{BSON_ARRAY(BSON("a" << 1 << "b" << 4)),
                                       BSON_ARRAY(BSON("a" << 1 << "b" << 5ll))};
    runAggregationWithNoGroupByTest("{x: {$sum: '$b'}}", docs, BSON_ARRAY(Value(9ll)));
}

TEST_F(SbeAccumulatorBuilderTest, SumAccumulatorTranslationIntAndDouble) {
    auto docs = std::vector<BSONArray>{BSON_ARRAY(BSON("a" << 1 << "b" << 4)),
                                       BSON_ARRAY(BSON("a" << 1 << "b" << 5.5))};
    runAggregationWithNoGroupByTest("{x: {$sum: '$b'}}", docs, BSON_ARRAY(9.5));
}

TEST_F(SbeAccumulatorBuilderTest, SumAccumulatorTranslationLongAndDouble) {
    auto docs = std::vector<BSONArray>{BSON_ARRAY(BSON("a" << 1 << "b" << 4ll)),
                                       BSON_ARRAY(BSON("a" << 1 << "b" << 5.5))};
    runAggregationWithNoGroupByTest("{x: {$sum: '$b'}}", docs, BSON_ARRAY(9.5));
}

TEST_F(SbeAccumulatorBuilderTest, SumAccumulatorTranslationIntLongDouble) {
    auto docs = std::vector<BSONArray>{BSON_ARRAY(BSON("a" << 1 << "b" << 4)),
                                       BSON_ARRAY(BSON("a" << 1 << "b" << 4ll)),
                                       BSON_ARRAY(BSON("a" << 1 << "b" << 5.5))};
    runAggregationWithNoGroupByTest("{x: {$sum: '$b'}}", docs, BSON_ARRAY(13.5));
}

TEST_F(SbeAccumulatorBuilderTest, SumAccumulatorTranslationLongLongDecimal) {
    auto docs = std::vector<BSONArray>{BSON_ARRAY(BSON("a" << 1 << "b" << 4ll)),
                                       BSON_ARRAY(BSON("a" << 1 << "b" << 4ll)),
                                       BSON_ARRAY(BSON("a" << 1 << "b" << Decimal128("5.5")))};
    runAggregationWithNoGroupByTest("{x: {$sum: '$b'}}", docs, BSON_ARRAY(Decimal128("13.5")));
}

TEST_F(SbeAccumulatorBuilderTest, SumAccumulatorTranslationDoubleAndDecimal) {
    const auto doubleVal = 4.2;
    DoubleDoubleSummation doubleDoubleSum;
    doubleDoubleSum.addDouble(doubleVal);
    const auto nonDecimal = doubleDoubleSum.getDecimal();

    const auto decimalVal = Decimal128("5.5");
    const auto expected = decimalVal.add(nonDecimal);

    auto docs = std::vector<BSONArray>{BSON_ARRAY(BSON("a" << 1 << "b" << doubleVal)),
                                       BSON_ARRAY(BSON("a" << 1 << "b" << decimalVal))};
    runAggregationWithNoGroupByTest("{x: {$sum: '$b'}}", docs, BSON_ARRAY(expected));
}

TEST_F(SbeAccumulatorBuilderTest, SumAccumulatorTranslationIntAndMissing) {
    auto docs =
        std::vector<BSONArray>{BSON_ARRAY(BSON("a" << 1 << "b" << 4)), BSON_ARRAY(BSON("a" << 1))};
    runAggregationWithNoGroupByTest("{x: {$sum: '$b'}}", docs, BSON_ARRAY(4));
}

TEST_F(SbeAccumulatorBuilderTest, SumAccumulatorTranslationMixedTypesWithDecimal128) {
    const auto doubleVal1 = 4.8;
    const auto doubleVal2 = -5.0;
    const auto intVal = 6;
    const auto longVal = 3l;

    DoubleDoubleSummation doubleDoubleSum;
    doubleDoubleSum.addDouble(doubleVal1);
    doubleDoubleSum.addInt(intVal);
    doubleDoubleSum.addInt(longVal);
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
    runAggregationWithNoGroupByTest("{x: {$sum: '$b'}}", docs, BSON_ARRAY(expected));
}

TEST_F(SbeAccumulatorBuilderTest, SumAccumulatorTranslationDecimalSum) {
    auto docs = std::vector<BSONArray>{BSON_ARRAY(BSON("a" << 1 << "b" << Decimal128("-10.100"))),
                                       BSON_ARRAY(BSON("a" << 1 << "b" << Decimal128("20.200")))};
    runAggregationWithNoGroupByTest("{x: {$sum: '$b'}}", docs, BSON_ARRAY(Decimal128("10.100")));
}

TEST_F(SbeAccumulatorBuilderTest, SumAccumulatorTranslationAllNull) {
    auto docs = std::vector<BSONArray>{BSON_ARRAY(BSON("a" << 1 << "b" << BSONNULL)),
                                       BSON_ARRAY(BSON("a" << 3 << "b" << BSONNULL)),
                                       BSON_ARRAY(BSON("a" << 3 << "b" << BSONNULL))};
    runAggregationWithNoGroupByTest("{x: {$sum: '$b'}}", docs, BSON_ARRAY(0));
}

TEST_F(SbeAccumulatorBuilderTest, SumAccumulatorTranslationAllMissing) {
    auto docs = std::vector<BSONArray>{BSON_ARRAY(BSON("a" << 1)), BSON_ARRAY(BSON("a" << 1))};
    runAggregationWithNoGroupByTest("{x: {$sum: '$b'}}", docs, BSON_ARRAY(0));
}

TEST_F(SbeAccumulatorBuilderTest, SumAccumulatorTranslationSomeNonNumeric) {
    auto docs = std::vector<BSONArray>{BSON_ARRAY(BSON("a" << 1 << "b"
                                                           << "c")),
                                       BSON_ARRAY(BSON("a" << 1 << "b" << 3)),
                                       BSON_ARRAY(BSON("a" << 1 << "b"
                                                           << "m")),
                                       BSON_ARRAY(BSON("a" << 1 << "b" << 4))};
    runAggregationWithNoGroupByTest("{x: {$sum: '$b'}}", docs, BSON_ARRAY(7));
}

TEST_F(SbeAccumulatorBuilderTest, SumAccumulatorTranslationTwoIntsDoNotOverflow) {
    auto docs = std::vector<BSONArray>{
        BSON_ARRAY(BSON("a" << 1 << "b" << Value(std::numeric_limits<int>::max()))),
        BSON_ARRAY(BSON("a" << 1 << "b" << 10))};
    runAggregationWithNoGroupByTest(
        "{x: {$sum: '$b'}}", docs, BSON_ARRAY(Value(std::numeric_limits<int>::max() + 10LL)));
}

TEST_F(SbeAccumulatorBuilderTest, SumAccumulatorTranslationTwoNegativeIntsDoNotOverflow) {
    auto docs = std::vector<BSONArray>{
        BSON_ARRAY(BSON("a" << 1 << "b" << Value(-std::numeric_limits<int>::max()))),
        BSON_ARRAY(BSON("a" << 1 << "b" << -10))};
    runAggregationWithNoGroupByTest(
        "{x: {$sum: '$b'}}", docs, BSON_ARRAY(Value(-std::numeric_limits<int>::max() - 10LL)));
}

TEST_F(SbeAccumulatorBuilderTest, SumAccumulatorTranslationIntAndLongDoNotTriggerIntOverflow) {
    auto docs = std::vector<BSONArray>{
        BSON_ARRAY(BSON("a" << 1 << "b" << Value(std::numeric_limits<int>::max()))),
        BSON_ARRAY(BSON("a" << 1 << "b" << 1LL))};
    runAggregationWithNoGroupByTest(
        "{x: {$sum: '$b'}}",
        docs,
        BSON_ARRAY(Value(static_cast<long long>(std::numeric_limits<int>::max()) + 1)));
}

TEST_F(SbeAccumulatorBuilderTest, SumAccumulatorTranslationIntAndDoubleDoNotTriggerOverflow) {
    auto docs = std::vector<BSONArray>{
        BSON_ARRAY(BSON("a" << 1 << "b" << Value(std::numeric_limits<int>::max()))),
        BSON_ARRAY(BSON("a" << 1 << "b" << 1.0))};
    runAggregationWithNoGroupByTest(
        "{x: {$sum: '$b'}}",
        docs,
        BSON_ARRAY(Value(static_cast<long long>(std::numeric_limits<int>::max()) + 1.0)));
}

TEST_F(SbeAccumulatorBuilderTest, SumAccumulatorTranslationIntAndLongOverflowIntoDouble) {
    auto docs = std::vector<BSONArray>{
        BSON_ARRAY(BSON("a" << 1 << "b" << Value(std::numeric_limits<long long>::max()))),
        BSON_ARRAY(BSON("a" << 1 << "b" << 1))};
    runAggregationWithNoGroupByTest(
        "{x: {$sum: '$b'}}",
        docs,
        BSON_ARRAY(Value(-static_cast<double>(std::numeric_limits<long long>::min()))));
}

TEST_F(SbeAccumulatorBuilderTest, SumAccumulatorTranslationTwoLongsOverflowIntoDouble) {
    auto docs = std::vector<BSONArray>{
        BSON_ARRAY(BSON("a" << 1 << "b" << Value(std::numeric_limits<long long>::max()))),
        BSON_ARRAY(BSON("a" << 1 << "b" << Value(std::numeric_limits<long long>::max())))};
    runAggregationWithNoGroupByTest(
        "{x: {$sum: '$b'}}",
        docs,
        BSON_ARRAY(Value(static_cast<double>(std::numeric_limits<long long>::max()) * 2)));
}

TEST_F(SbeAccumulatorBuilderTest, SumAccumulatorTranslationLongAndDoubleDoNotTriggerLongOverflow) {
    auto docs = std::vector<BSONArray>{
        BSON_ARRAY(BSON("a" << 1 << "b" << Value(std::numeric_limits<long long>::max()))),
        BSON_ARRAY(BSON("a" << 1 << "b" << 1.0))};
    runAggregationWithNoGroupByTest(
        "{x: {$sum: '$b'}}", docs, BSON_ARRAY(Value(std::numeric_limits<long long>::max() + 1.0)));
}

TEST_F(SbeAccumulatorBuilderTest, SumAccumulatorTranslationTwoDoublesOverflowToInfinity) {
    auto docs = std::vector<BSONArray>{
        BSON_ARRAY(BSON("a" << 1 << "b" << Value(std::numeric_limits<double>::max()))),
        BSON_ARRAY(BSON("a" << 1 << "b" << Value(std::numeric_limits<double>::max())))};
    runAggregationWithNoGroupByTest(
        "{x: {$sum: '$b'}}", docs, BSON_ARRAY(Value(std::numeric_limits<double>::infinity())));
}

TEST_F(SbeAccumulatorBuilderTest, SumAccumulatorTranslationTwoIntegersDoNotOverflowIfDoubleAdded) {
    auto docs = std::vector<BSONArray>{
        BSON_ARRAY(BSON("a" << 1 << "b" << Value(std::numeric_limits<long long>::max()))),
        BSON_ARRAY(BSON("a" << 1 << "b" << Value(std::numeric_limits<long long>::max()))),
        BSON_ARRAY(BSON("a" << 1 << "b" << 1.0))};
    runAggregationWithNoGroupByTest(
        "{x: {$sum: '$b'}}",
        docs,
        BSON_ARRAY(Value(static_cast<double>(std::numeric_limits<long long>::max()) +
                         static_cast<double>(std::numeric_limits<long long>::max()))));
}

TEST_F(SbeAccumulatorBuilderTest, SumAccumulatorTranslationIntAndNanDouble) {
    auto docs = std::vector<BSONArray>{
        BSON_ARRAY(BSON("a" << 1 << "b" << 3)),
        BSON_ARRAY(BSON("a" << 1 << "b" << Value(std::numeric_limits<double>::quiet_NaN())))};
    runAggregationWithNoGroupByTest(
        "{x: {$sum: '$b'}}", docs, BSON_ARRAY(Value(std::numeric_limits<double>::quiet_NaN())));
}

TEST_F(SbeAccumulatorBuilderTest, SumAccumulatorTranslationGroupByTest) {
    auto docs = std::vector<BSONArray>{
        BSON_ARRAY(BSON("a" << 1 << "b" << 3)),
        BSON_ARRAY(BSON("a" << 1 << "b" << 4)),
        BSON_ARRAY(BSON("a" << 2 << "b" << 13)),
        BSON_ARRAY(BSON("a" << 2 << "b"
                            << "a")),
    };
    runAggregationWithGroupByTest("{x: {$sum: '$b'}}", docs, {"$a"}, BSON_ARRAY(7 << 13));
}

TEST_F(SbeAccumulatorBuilderTest, SumAccumulatorTranslationTwoGroupByTest) {
    auto docs = std::vector<BSONArray>{
        BSON_ARRAY(BSON("a" << 1 << "b" << 3 << "c" << 1)),
        BSON_ARRAY(BSON("a" << 1 << "b" << 4 << "c" << 1)),
        BSON_ARRAY(BSON("a" << 2 << "b" << 13 << "c" << 1)),
        BSON_ARRAY(BSON("a" << 2 << "b"
                            << "a"
                            << "c" << 1)),
    };
    runAggregationWithGroupByTest("{x: {$sum: '$b'}}", docs, {"$a", "$c"}, BSON_ARRAY(20));
}
TEST_F(SbeAccumulatorBuilderTest, AddToSetAccumulatorTranslationSingleDoc) {
    auto docs = std::vector<BSONArray>{BSON_ARRAY(BSON("a" << 1 << "b" << 1))};
    runAddToSetTest("{x: {$addToSet: '$b'}}", docs, BSON_ARRAY(1));
}

TEST_F(SbeAccumulatorBuilderTest, AddToSetAccumulatorTranslationBasic) {
    auto docs = std::vector<BSONArray>{BSON_ARRAY(BSON("a" << 1 << "b" << 3)),
                                       BSON_ARRAY(BSON("a" << 2 << "b" << 1)),
                                       BSON_ARRAY(BSON("a" << 3 << "b" << 2))};
    runAddToSetTest("{x: {$addToSet: '$b'}}", docs, BSON_ARRAY(1 << 2 << 3));
}

TEST_F(SbeAccumulatorBuilderTest, AddToSetAccumulatorTranslationSubfield) {
    auto docs = std::vector<BSONArray>{BSON_ARRAY(BSON("a" << 1 << "b" << BSON("c" << 1))),
                                       BSON_ARRAY(BSON("a" << 2 << "b" << BSON("c" << 2))),
                                       BSON_ARRAY(BSON("a" << 3 << "b" << BSON("c" << 3)))};
    runAddToSetTest("{x: {$addToSet: '$b.c'}}", docs, BSON_ARRAY(1 << 2 << 3));
}

TEST_F(SbeAccumulatorBuilderTest, AddToSetAccumulatorTranslationRepeatedValue) {
    auto docs = std::vector<BSONArray>{BSON_ARRAY(BSON("a" << 1 << "b" << 2)),
                                       BSON_ARRAY(BSON("a" << 2 << "b" << 1)),
                                       BSON_ARRAY(BSON("a" << 3 << "b" << 2))};
    runAddToSetTest("{x: {$addToSet: '$b'}}", docs, BSON_ARRAY(1 << 2));
}

TEST_F(SbeAccumulatorBuilderTest, AddToSetAccumulatorTranslationMixedTypes) {
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
    runAddToSetTest("{x: {$addToSet: '$b'}}", docs, expectedSet);
}

TEST_F(SbeAccumulatorBuilderTest, AddToSetAccumulatorTranslationSomeMissingFields) {
    auto docs = std::vector<BSONArray>{BSON_ARRAY(BSON("a" << 1)),
                                       BSON_ARRAY(BSON("a" << 2 << "b" << 2)),
                                       BSON_ARRAY(BSON("a" << 3 << "b" << BSONNULL)),
                                       BSON_ARRAY(BSON("a" << 4 << "c" << 1))};
    runAddToSetTest("{x: {$addToSet: '$b'}}", docs, BSON_ARRAY(BSONNULL << 2));
}

TEST_F(SbeAccumulatorBuilderTest, AddToSetAccumulatorTranslationAllMissingFields) {
    auto docs = std::vector<BSONArray>{
        BSON_ARRAY(BSON("a" << 1)), BSON_ARRAY(BSON("a" << 2)), BSON_ARRAY(BSON("a" << 3))};
    runAddToSetTest("{x: {$addToSet: '$b'}}", docs, BSONArray{});
}

TEST_F(SbeAccumulatorBuilderTest, PushAccumulatorTranslationNoDocs) {
    auto docs = std::vector<BSONArray>{BSONArray{}};
    runAggregationWithNoGroupByTest("{x: {$push: '$b'}}", docs, BSON_ARRAY(BSONArray{}));
}

TEST_F(SbeAccumulatorBuilderTest, PushAccumulatorTranslationBasic) {
    auto docs = std::vector<BSONArray>{BSON_ARRAY(BSON("a" << 1 << "b" << 3)),
                                       BSON_ARRAY(BSON("a" << 2 << "b" << 1)),
                                       BSON_ARRAY(BSON("a" << 3 << "b" << 2)),
                                       BSON_ARRAY(BSON("a" << 1 << "b" << 3))};
    runAggregationWithNoGroupByTest(
        "{x: {$push: '$b'}}", docs, BSON_ARRAY(BSON_ARRAY(3 << 1 << 2 << 3)));
}

TEST_F(SbeAccumulatorBuilderTest, PushAccumulatorTranslationSomeMissing) {
    auto docs = std::vector<BSONArray>{BSON_ARRAY(BSON("a" << 1 << "b" << 3)),
                                       BSON_ARRAY(BSON("a" << 2 << "b" << 1)),
                                       BSON_ARRAY(BSON("a" << 3))};
    runAggregationWithNoGroupByTest("{x: {$push: '$b'}}", docs, BSON_ARRAY(BSON_ARRAY(3 << 1)));
}

TEST_F(SbeAccumulatorBuilderTest, PushAccumulatorTranslationAllMissing) {
    auto docs = std::vector<BSONArray>{
        BSON_ARRAY(BSON("a" << 1)), BSON_ARRAY(BSON("a" << 2)), BSON_ARRAY(BSON("a" << 3))};
    runAggregationWithNoGroupByTest("{x: {$push: '$b'}}", docs, BSON_ARRAY(BSONArray{}));
}

TEST_F(SbeAccumulatorBuilderTest, PushAccumulatorTranslationExpression) {
    auto docs = std::vector<BSONArray>{BSON_ARRAY(BSON("a" << 1 << "b" << 3)),
                                       BSON_ARRAY(BSON("a" << 2 << "b" << 1)),
                                       BSON_ARRAY(BSON("a" << 3 << "b" << 2))};
    runAggregationWithNoGroupByTest(
        "{x: {$push: {$add: ['$a','$b']}}}", docs, BSON_ARRAY(BSON_ARRAY(1 + 3 << 2 + 1 << 3 + 2)));
}

TEST_F(SbeAccumulatorBuilderTest, PushAccumulatorTranslationExpressionMissingField) {
    auto docs = std::vector<BSONArray>{BSON_ARRAY(BSON("a" << 1 << "b" << 3)),
                                       BSON_ARRAY(BSON("a" << 2 << "b" << 1)),
                                       BSON_ARRAY(BSON("a" << 3))};
    runAggregationWithNoGroupByTest(
        "{x: {$push: {$add: ['$a','$b']}}}",
        docs,
        BSON_ARRAY(BSON_ARRAY(1 + 3 << 2 + 1 << BSONNULL /*3 + Nothing*/)));
}

TEST_F(SbeAccumulatorBuilderTest, PushAccumulatorTranslationExpressionInvalid) {
    auto docs = std::vector<BSONArray>{BSON_ARRAY(BSON("a" << 1 << "b" << 3)),
                                       BSON_ARRAY(BSON("a" << 2 << "b" << 1)),
                                       BSON_ARRAY(BSON("a" << 3 << "b"
                                                           << "hello"))};
    ASSERT_THROWS(
        runAggregationWithNoGroupByTest("{x: {$push: {$add: ['$a','$b']}}}", docs, BSONArray{}),
        AssertionException);
}

TEST_F(SbeAccumulatorBuilderTest, PushAccumulatorTranslationVariousTypes) {
    const auto bsonArr = BSON_ARRAY(1 << 2 << 3);
    const auto bsonObj = BSON("c" << 1);
    const auto strVal = "hello"_sd;
    auto docs = std::vector<BSONArray>{BSON_ARRAY(BSON("a" << 1 << "b" << 42)),
                                       BSON_ARRAY(BSON("a" << 2 << "b" << 4.2)),
                                       BSON_ARRAY(BSON("a" << 3 << "b" << true)),
                                       BSON_ARRAY(BSON("a" << 4 << "b" << strVal)),
                                       BSON_ARRAY(BSON("a" << 5 << "b" << bsonObj)),
                                       BSON_ARRAY(BSON("a" << 6 << "b" << bsonArr))};
    runAggregationWithNoGroupByTest(
        "{x: {$push: '$b'}}",
        docs,
        BSON_ARRAY(BSON_ARRAY(42 << 4.2 << true << strVal << bsonObj << bsonArr)));
}

}  // namespace mongo
