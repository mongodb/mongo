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

#include "mongo/db/exec/agg/exec_pipeline.h"

#include "mongo/db/exec/agg/document_source_to_stage_registry.h"
#include "mongo/db/exec/agg/internal_inhibit_optimization_stage.h"
#include "mongo/db/exec/agg/limit_stage.h"
#include "mongo/db/exec/agg/match_stage.h"
#include "mongo/db/exec/agg/mock_stage.h"
#include "mongo/db/exec/agg/single_document_transformation_stage.h"
#include "mongo/db/exec/agg/skip_stage.h"
#include "mongo/db/exec/agg/sort_stage.h"
#include "mongo/db/exec/agg/unwind_stage.h"
#include "mongo/db/exec/document_value/document_value_test_util.h"
#include "mongo/db/matcher/expression_always_boolean.h"
#include "mongo/db/pipeline/accumulation_statement.h"
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/document_source_add_fields.h"
#include "mongo/db/pipeline/document_source_group.h"
#include "mongo/db/pipeline/document_source_sort.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/pipeline/match_processor.h"
#include "mongo/db/query/compiler/dependency_analysis/dependencies.h"
#include "mongo/idl/server_parameter_test_controller.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/temp_dir.h"
#include "mongo/unittest/unittest.h"

namespace mongo::test {

using Pipeline = mongo::exec::agg::Pipeline;
using Stage = mongo::exec::agg::Stage;
using StagePtr = mongo::exec::agg::StagePtr;
using StageContainer = mongo::exec::agg::Pipeline::StageContainer;
using GetNextResult = mongo::exec::agg::GetNextResult;

class FakeStage : public Stage {
public:
    FakeStage(const boost::intrusive_ptr<ExpressionContext>& expCtx) : Stage("$fake", expCtx) {}

    GetNextResult doGetNext() final {
        return GetNextResult::makeEOF();
    }

    Stage* getSource() {
        return pSource;
    }
};

TEST(PipelineTest, OneStagePipeline) {
    auto expCtx = make_intrusive<ExpressionContextForTest>();

    StageContainer stages{make_intrusive<FakeStage>(expCtx)};

    // Assert that the source stage is initially set to nullptr.
    ASSERT_EQ(nullptr, dynamic_cast<FakeStage*>(stages.back().get())->getSource());

    Pipeline pl(std::move(stages), expCtx);

    // Assert that the source stage is still set to nullptr.
    ASSERT_EQ(nullptr, dynamic_cast<FakeStage*>(pl.getStages().back().get())->getSource());
}

TEST(PipelineTest, ThreeStagePipeline) {
    auto expCtx = make_intrusive<ExpressionContextForTest>();

    auto stage0 = make_intrusive<FakeStage>(expCtx);
    auto stage1 = make_intrusive<FakeStage>(expCtx);
    auto stage2 = make_intrusive<FakeStage>(expCtx);

    StageContainer stages{stage0, stage1, stage2};

    Pipeline pl(std::move(stages), expCtx);

    // Assert that the stage order does not change.
    ASSERT_EQ(stage0.get(), pl.getStages()[0].get());
    ASSERT_EQ(stage1.get(), pl.getStages()[1].get());
    ASSERT_EQ(stage2.get(), pl.getStages()[2].get());
}

DEATH_TEST_REGEX(PipelineTestDeathTest,
                 GetNextResultOnEmptyPipelineThrows,
                 "Tripwire assertion.*10549300") {
    StageContainer stages;
    ASSERT_THROWS_CODE(Pipeline(std::move(stages), make_intrusive<ExpressionContextForTest>()),
                       AssertionException,
                       10549300);
}

TEST(PipelineTest, DefaultStageIsEOFReturnsFalse) {
    auto expCtx = make_intrusive<ExpressionContextForTest>();
    FakeStage stage(expCtx);
    ASSERT_FALSE(stage.isEOF());
}

TEST(PipelineTest, PipelineIsEOFDelegatesToLastStage) {
    auto expCtx = make_intrusive<ExpressionContextForTest>();

    // MockStage with no documents reports EOF.
    auto emptyMock = exec::agg::MockStage::createForTest(std::deque<GetNextResult>{}, expCtx);
    StageContainer stages{emptyMock};
    Pipeline pl(std::move(stages), expCtx);
    ASSERT_TRUE(pl.isEOF());
}

TEST(PipelineTest, PipelineIsEOFReturnsFalseWhenDocumentsRemain) {
    auto expCtx = make_intrusive<ExpressionContextForTest>();

    auto mock = exec::agg::MockStage::createForTest(std::vector<BSONObj>{BSON("a" << 1)}, expCtx);
    StageContainer stages{mock};
    Pipeline pl(std::move(stages), expCtx);
    ASSERT_FALSE(pl.isEOF());
}

TEST(PipelineTest, PipelineIsEOFOnlyChecksLastStage) {
    auto expCtx = make_intrusive<ExpressionContextForTest>();

    // First stage is empty (would report EOF), but last stage has documents.
    auto emptyMock = exec::agg::MockStage::createForTest(std::deque<GetNextResult>{}, expCtx);
    auto fullMock =
        exec::agg::MockStage::createForTest(std::vector<BSONObj>{BSON("a" << 1)}, expCtx);
    StageContainer stages{emptyMock, fullMock};
    Pipeline pl(std::move(stages), expCtx);
    ASSERT_FALSE(pl.isEOF());
}

using PipelineIsEOFTest = AggregationContextFixture;

TEST_F(PipelineIsEOFTest, LimitStageReportsEOFAfterLimitReached) {
    auto expCtx = getExpCtx();

    auto mock = exec::agg::MockStage::createForTest(
        std::vector<BSONObj>{BSON("a" << 1), BSON("a" << 2), BSON("a" << 3)}, expCtx);
    auto limit = make_intrusive<exec::agg::LimitStage>("$limit", expCtx, 2);
    exec::agg::MockStage::setSource_forTest(limit, mock.get());

    // Before any getNext() calls, isEOF() should be false.
    ASSERT_FALSE(limit->isEOF());

    // After first getNext(), still not at limit.
    auto result1 = limit->getNext();
    ASSERT_TRUE(result1.isAdvanced());
    ASSERT_FALSE(limit->isEOF());

    // After second getNext(), limit is reached.
    auto result2 = limit->getNext();
    ASSERT_TRUE(result2.isAdvanced());
    ASSERT_TRUE(limit->isEOF());

    // Further getNext() returns EOF.
    auto result3 = limit->getNext();
    ASSERT_TRUE(result3.isEOF());
    ASSERT_TRUE(limit->isEOF());
}

TEST_F(PipelineIsEOFTest, PipelineWithLimitStageReportsEOFAfterLimitReached) {
    auto expCtx = getExpCtx();

    auto mock = exec::agg::MockStage::createForTest(
        std::vector<BSONObj>{BSON("a" << 1), BSON("a" << 2), BSON("a" << 3)}, expCtx);
    auto limit = make_intrusive<exec::agg::LimitStage>("$limit", expCtx, 2);
    // Stitch stages: limit reads from mock.
    exec::agg::MockStage::setSource_forTest(limit, mock.get());

    StageContainer stages{mock, limit};
    Pipeline pl(std::move(stages), expCtx);

    ASSERT_FALSE(pl.isEOF());

    // Consume all documents up to the limit.
    ASSERT_TRUE(pl.getNext().has_value());
    ASSERT_FALSE(pl.isEOF());

    ASSERT_TRUE(pl.getNext().has_value());
    ASSERT_TRUE(pl.isEOF());

    // getNext() returns none (EOF).
    ASSERT_FALSE(pl.getNext().has_value());
    ASSERT_TRUE(pl.isEOF());
}

TEST_F(PipelineIsEOFTest, AllStreamingStagesDelegateIsEOFToSource) {
    auto expCtx = getExpCtx();

    // Use docs with single-element arrays so UnwindStage produces 1:1 output.
    const auto docs = std::vector<BSONObj>{BSON("a" << BSON_ARRAY(1)), BSON("a" << BSON_ARRAY(2))};

    using StageFactory =
        std::function<boost::intrusive_ptr<Stage>(const boost::intrusive_ptr<ExpressionContext>&)>;

    const std::vector<std::pair<std::string, StageFactory>> stageFactories = {
        {"$skip",
         [](const auto& ctx) -> boost::intrusive_ptr<Stage> {
             return make_intrusive<exec::agg::SkipStage>("$skip", ctx, 0);
         }},
        {"$limit",
         [](const auto& ctx) -> boost::intrusive_ptr<Stage> {
             return make_intrusive<exec::agg::LimitStage>("$limit", ctx, 100);
         }},
        {"$_internalInhibitOptimization",
         [](const auto& ctx) -> boost::intrusive_ptr<Stage> {
             return make_intrusive<exec::agg::InternalInhibitOptimizationStage>(
                 "$_internalInhibitOptimization", ctx);
         }},
        {"$match",
         [](const auto& ctx) -> boost::intrusive_ptr<Stage> {
             auto predicate = BSON("$alwaysTrue" << 1);
             auto matchProcessor =
                 std::make_shared<MatchProcessor>(std::make_unique<AlwaysTrueMatchExpression>(),
                                                  DepsTracker{},
                                                  std::move(predicate));
             return make_intrusive<exec::agg::MatchStage>("$match", ctx, matchProcessor, false);
         }},
        {"$addFields (SingleDocumentTransformation)",
         [](const auto& ctx) -> boost::intrusive_ptr<Stage> {
             auto addFields = DocumentSourceAddFields::create(BSON("b" << 1), ctx);
             return exec::agg::buildStage(addFields);
         }},
        {"$unwind",
         [](const auto& ctx) -> boost::intrusive_ptr<Stage> {
             auto processor = std::make_unique<exec::agg::UnwindProcessor>(
                 FieldPath("a"), false, boost::none, false);
             return make_intrusive<exec::agg::UnwindStage>("$unwind", ctx, std::move(processor));
         }},
    };

    for (const auto& [name, factory] : stageFactories) {
        auto mock = exec::agg::MockStage::createForTest(docs, expCtx);
        auto stage = factory(expCtx);
        exec::agg::MockStage::setSource_forTest(stage, mock.get());

        StageContainer stages{mock, stage};
        Pipeline pl(std::move(stages), expCtx);

        ASSERT_FALSE(pl.isEOF()) << name;
        ASSERT_TRUE(pl.getNext().has_value()) << name;
        ASSERT_FALSE(pl.isEOF()) << name;
        ASSERT_TRUE(pl.getNext().has_value()) << name;
        ASSERT_TRUE(pl.isEOF()) << name;
        ASSERT_FALSE(pl.getNext().has_value()) << name;
    }
}

// Helper to create a $group stage that groups by $_id with $sum: 1.
boost::intrusive_ptr<Stage> createGroupStage(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    boost::optional<int64_t> maxMemoryUsageBytes = boost::none) {
    auto&& [parser, _1, _2, _3] = AccumulationStatement::getParser("$sum");
    auto accArg = BSON("" << 1);
    auto accExpr = parser(expCtx.get(), accArg.firstElement(), expCtx->variablesParseState);
    AccumulationStatement countStatement{"count", accExpr};
    auto groupByExpr =
        ExpressionFieldPath::parse(expCtx.get(), "$_id", expCtx->variablesParseState);
    auto group = DocumentSourceGroup::create(
        expCtx, groupByExpr, {countStatement}, false /*willBeMerged*/, maxMemoryUsageBytes);
    return exec::agg::buildStage(group);
}

// Helper to create a $group stage that groups by $_id with $push: "$largeStr".
boost::intrusive_ptr<Stage> createPushGroupStage(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    boost::optional<int64_t> maxMemoryUsageBytes = boost::none) {
    auto&& [parser, _1, _2, _3] = AccumulationStatement::getParser("$push");
    auto accArg = BSON("" << "$largeStr");
    auto accExpr = parser(expCtx.get(), accArg.firstElement(), expCtx->variablesParseState);
    AccumulationStatement pushStatement{"spaceHog", accExpr};
    auto groupByExpr =
        ExpressionFieldPath::parse(expCtx.get(), "$_id", expCtx->variablesParseState);
    auto group = DocumentSourceGroup::create(
        expCtx, groupByExpr, {pushStatement}, false /*willBeMerged*/, maxMemoryUsageBytes);
    return exec::agg::buildStage(group);
}

TEST_F(PipelineIsEOFTest, GroupStageReportsEOF) {
    auto expCtx = getExpCtx();

    auto mock = exec::agg::MockStage::createForTest(
        std::vector<BSONObj>{BSON("_id" << 1), BSON("_id" << 2)}, expCtx);
    auto groupStage = createGroupStage(expCtx);
    exec::agg::MockStage::setSource_forTest(groupStage, mock.get());

    StageContainer stages{mock, groupStage};
    Pipeline pl(std::move(stages), expCtx);

    // Before any getNext(), group hasn't consumed input yet.
    ASSERT_FALSE(pl.isEOF());

    // First getNext() triggers consuming all input, returns first group.
    ASSERT_TRUE(pl.getNext().has_value());
    ASSERT_FALSE(pl.isEOF());

    // Second group result.
    ASSERT_TRUE(pl.getNext().has_value());

    // Group eagerly knows it's exhausted.
    ASSERT_TRUE(pl.isEOF());
    ASSERT_FALSE(pl.getNext().has_value());
}

TEST_F(PipelineIsEOFTest, SortWithLimitEagerlyReportsEOF) {
    auto expCtx = getExpCtx();

    auto mock = exec::agg::MockStage::createForTest(
        std::vector<BSONObj>{BSON("a" << 3), BSON("a" << 1), BSON("a" << 2)}, expCtx);
    auto sort = DocumentSourceSort::create(expCtx, {BSON("a" << 1), expCtx}, {.limit = 2});
    auto sortStage = exec::agg::buildStage(sort);
    exec::agg::MockStage::setSource_forTest(sortStage, mock.get());

    StageContainer stages{mock, sortStage};
    Pipeline pl(std::move(stages), expCtx);

    ASSERT_FALSE(pl.isEOF());

    // First sorted doc.
    auto next = pl.getNext();
    ASSERT_TRUE(next.has_value());
    ASSERT_FALSE(pl.isEOF());

    // Second sorted doc - limit reached, sort eagerly sets EOF.
    next = pl.getNext();
    ASSERT_TRUE(next.has_value());
    ASSERT_TRUE(pl.isEOF());

    ASSERT_FALSE(pl.getNext().has_value());
}

TEST_F(PipelineIsEOFTest, SortWithoutLimitDoesNotEagerlyReportEOF) {
    auto expCtx = getExpCtx();

    auto mock = exec::agg::MockStage::createForTest(
        std::vector<BSONObj>{BSON("a" << 2), BSON("a" << 1)}, expCtx);
    auto sort = DocumentSourceSort::create(expCtx, BSON("a" << 1));
    auto sortStage = exec::agg::buildStage(sort);
    exec::agg::MockStage::setSource_forTest(sortStage, mock.get());

    StageContainer stages{mock, sortStage};
    Pipeline pl(std::move(stages), expCtx);

    ASSERT_FALSE(pl.isEOF());

    ASSERT_TRUE(pl.getNext().has_value());
    ASSERT_TRUE(pl.getNext().has_value());

    // Without a limit, sort does NOT eagerly check hasNext() to avoid overhead.
    ASSERT_FALSE(pl.isEOF());

    // The next getNext() discovers EOF and sets the flag.
    ASSERT_FALSE(pl.getNext().has_value());
    ASSERT_TRUE(pl.isEOF());
}

// Helper to create time-series documents with bucket metadata for bounded sort tests.
std::vector<Document> createTimeDocs(std::vector<long long> millisValues) {
    std::vector<Document> data;
    for (auto ms : millisValues) {
        Document doc{{"time", Date_t::fromMillisSinceEpoch(ms)}};
        MutableDocument mdoc{doc};
        DocumentMetadataFields metadata;
        metadata.setTimeseriesBucketMinTime(doc.getField("time").getDate());
        mdoc.setMetadata(std::move(metadata));
        data.push_back(mdoc.freeze());
    }
    return data;
}

TEST_F(PipelineIsEOFTest, BoundedSortWithoutLimitDoesNotEagerlyReportEOF) {
    auto expCtx = getExpCtx();

    auto mock = exec::agg::MockStage::createForTest(createTimeDocs({1, 0}), expCtx);
    auto sort = DocumentSourceSort::createBoundedSort(
        {BSON("time" << 1), expCtx}, DocumentSourceSort::kMin, -1, boost::none, false, expCtx);
    auto sortStage = exec::agg::buildStage(sort);
    exec::agg::MockStage::setSource_forTest(sortStage, mock.get());

    StageContainer stages{mock, sortStage};
    Pipeline pl(std::move(stages), expCtx);

    ASSERT_FALSE(pl.isEOF());

    ASSERT_TRUE(pl.getNext().has_value());
    ASSERT_TRUE(pl.getNext().has_value());

    // Without a limit, bounded sort does not eagerly set _eof.
    ASSERT_FALSE(pl.isEOF());
    ASSERT_FALSE(pl.getNext().has_value());
    ASSERT_TRUE(pl.isEOF());
}

TEST_F(PipelineIsEOFTest, BoundedSortWithLimitEagerlyReportsEOF) {
    auto expCtx = getExpCtx();

    auto mock = exec::agg::MockStage::createForTest(createTimeDocs({0, 1, 2}), expCtx);
    auto sort = DocumentSourceSort::createBoundedSort(
        {BSON("time" << 1), expCtx}, DocumentSourceSort::kMin, -1, 2 /*limit*/, false, expCtx);
    auto sortStage = exec::agg::buildStage(sort);
    exec::agg::MockStage::setSource_forTest(sortStage, mock.get());

    StageContainer stages{mock, sortStage};
    Pipeline pl(std::move(stages), expCtx);

    ASSERT_FALSE(pl.isEOF());

    ASSERT_TRUE(pl.getNext().has_value());
    ASSERT_FALSE(pl.isEOF());

    // Second doc - limit reached, bounded sort eagerly sets EOF.
    ASSERT_TRUE(pl.getNext().has_value());
    ASSERT_TRUE(pl.isEOF());

    ASSERT_FALSE(pl.getNext().has_value());
}

TEST_F(PipelineIsEOFTest, SpillingSortReportsEOF) {
    RAIIServerParameterControllerForTest sortMemoryLimit{
        "internalQueryMaxBlockingSortMemoryUsageBytes", 3 * 1024};

    unittest::TempDir tempDir("ExecPipelineSpillingSortTest");
    auto expCtx = getExpCtx();
    expCtx->setTempDir(tempDir.path());
    expCtx->setAllowDiskUse(true);

    std::string largeStr(2 * 1024, 'x');
    auto mock = exec::agg::MockStage::createForTest(
        std::vector<BSONObj>{BSON("a" << 3 << "pad" << largeStr),
                             BSON("a" << 2 << "pad" << largeStr),
                             BSON("a" << 1 << "pad" << largeStr)},
        expCtx);
    auto sort = DocumentSourceSort::create(expCtx, BSON("a" << 1));
    auto sortStage = boost::dynamic_pointer_cast<exec::agg::SortStage>(exec::agg::buildStage(sort));
    exec::agg::MockStage::setSource_forTest(sortStage, mock.get());

    StageContainer stages{mock, sortStage};
    Pipeline pl(std::move(stages), expCtx);

    ASSERT_FALSE(pl.isEOF());

    ASSERT_TRUE(pl.getNext().has_value());
    ASSERT_TRUE(pl.getNext().has_value());
    ASSERT_TRUE(pl.getNext().has_value());

    // Without a limit, sort does not eagerly report EOF even when spilling.
    ASSERT_FALSE(pl.isEOF());
    ASSERT_FALSE(pl.getNext().has_value());
    ASSERT_TRUE(pl.isEOF());

    ASSERT_TRUE(sortStage->usedDisk());
}

TEST_F(PipelineIsEOFTest, SpillingGroupReportsEOF) {
    unittest::TempDir tempDir("ExecPipelineSpillingGroupTest");
    auto expCtx = getExpCtx();
    expCtx->setTempDir(tempDir.path());
    expCtx->setAllowDiskUse(true);

    std::string largeStr(1000, 'x');
    auto mock = exec::agg::MockStage::createForTest(
        std::vector<BSONObj>{BSON("_id" << 1 << "largeStr" << largeStr),
                             BSON("_id" << 2 << "largeStr" << largeStr)},
        expCtx);
    auto groupStage = createPushGroupStage(expCtx, 1000 /*maxMemoryUsageBytes*/);
    exec::agg::MockStage::setSource_forTest(groupStage, mock.get());

    StageContainer stages{mock, groupStage};
    Pipeline pl(std::move(stages), expCtx);

    ASSERT_FALSE(pl.isEOF());

    ASSERT_TRUE(pl.getNext().has_value());
    ASSERT_FALSE(pl.isEOF());
    ASSERT_TRUE(pl.getNext().has_value());

    // Group eagerly knows it's exhausted, even after spilling.
    ASSERT_TRUE(pl.isEOF());
    ASSERT_FALSE(pl.getNext().has_value());

    ASSERT_TRUE(groupStage->usedDisk());
}

}  // namespace mongo::test
