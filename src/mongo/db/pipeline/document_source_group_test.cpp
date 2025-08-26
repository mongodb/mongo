/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/db/pipeline/document_source_group.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/json.h"
#include "mongo/db/exec/agg/document_source_to_stage_registry.h"
#include "mongo/db/exec/agg/group_base_stage.h"
#include "mongo/db/exec/agg/mock_stage.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/document_metadata_fields.h"
#include "mongo/db/exec/document_value/document_value_test_util.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/exec/document_value/value_comparator.h"
#include "mongo/db/exec/plan_stats.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/aggregate_command_gen.h"
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/document_source_streaming_group.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/pipeline/variables.h"
#include "mongo/db/query/compiler/dependency_analysis/dependencies.h"
#include "mongo/db/query/query_test_service_context.h"
#include "mongo/db/service_context.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/dbtests/dbtests.h"  // IWYU pragma: keep
#include "mongo/idl/server_parameter_test_controller.h"
#include "mongo/stdx/unordered_set.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/temp_dir.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/string_map.h"

#include <cstdint>
#include <deque>
#include <functional>
#include <list>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "src/mongo/db/exec/agg/document_source_to_stage_registry.h"
#include <absl/container/node_hash_map.h>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>
#include <fmt/format.h>
#include <fmt/ranges.h>

namespace mongo {
namespace {

static const char* const ns = "unittests.document_source_group_tests";

/**
 * Test helper which owns the pipeline and shard keys required for a DistributedPlanContext.
 *
 * DistributedPlanContext takes references to the noted values, as it is designed to encapsulate
 * a group of arguments passed from elsewhere, but does not require ownership in production code.
 *
 * For tests, this means each test needs to maintain ownership of the relevant fields. This either
 * leads to repetition, or use of members on the fixture.
 *
 * To separate concerns but allow a helper method to construct and return a DistributedPlanContext,
 * this helper handles that ownership.
 */
class OwningDistributedPlanContext : public DocumentSourceGroup::DistributedPlanContext {
public:
    OwningDistributedPlanContext(std::unique_ptr<Pipeline> pipelinePrefix,
                                 std::unique_ptr<Pipeline> pipelineSuffix,
                                 boost::optional<OrderedPathSet> shardKeys)
        : DocumentSourceGroup::DistributedPlanContext{*pipelinePrefix,
                                                      *pipelineSuffix,
                                                      this->shardKeys},
          pipelinePrefix(std::move(pipelinePrefix)),
          pipelineSuffix(std::move(pipelineSuffix)),
          shardKeys(std::move(shardKeys)) {}
    std::unique_ptr<Pipeline> pipelinePrefix;
    std::unique_ptr<Pipeline> pipelineSuffix;
    boost::optional<OrderedPathSet> shardKeys;
};

// This provides access to getExpCtx(), but we'll use a different name for this test suite.
class DocumentSourceGroupTest : public AggregationContextFixture {
public:
    auto makePlanCtx(StringData pipelineJson, OrderedPathSet shardKeys) {
        auto bson = fromjson(pipelineJson);
        std::vector<BSONObj> rawPipeline;
        for (const auto& element : bson) {
            rawPipeline.push_back(element.Obj());
        }
        auto pipeline =
            Pipeline::makePipeline(rawPipeline, getExpCtx(), {.attachCursorSource = false});

        auto pipelineSuffix =
            Pipeline::makePipeline({}, getExpCtx(), {.attachCursorSource = false});
        return OwningDistributedPlanContext(
            std::move(pipeline), std::move(pipelineSuffix), std::move(shardKeys));
    }
    auto makePlanCtx(OrderedPathSet shardKeys) {
        return makePlanCtx("[]", std::move(shardKeys));
    }

    std::pair<int64_t, int64_t> getCurOpMemoryStats() {
        OperationContext* opCtx = getExpCtx()->getOperationContext();
        CurOp* curOp = CurOp::get(opCtx);
        return {curOp->getInUseTrackedMemoryBytes(), curOp->getPeakTrackedMemoryBytes()};
    }
};

TEST_F(DocumentSourceGroupTest, ShouldBeAbleToPauseLoading) {
    auto expCtx = getExpCtx();
    expCtx->setInRouter(true);  // Disallow external sort.
                                // This is the only way to do this in a debug build.
    auto&& [parser, _1, _2, _3] = AccumulationStatement::getParser("$sum");
    auto accumulatorArg = BSON("" << 1);
    auto accExpr = parser(expCtx.get(), accumulatorArg.firstElement(), expCtx->variablesParseState);
    AccumulationStatement countStatement{"count", accExpr};
    auto group = DocumentSourceGroup::create(
        expCtx, ExpressionConstant::create(expCtx.get(), Value(BSONNULL)), {countStatement}, false);
    auto mock =
        exec::agg::MockStage::createForTest({DocumentSource::GetNextResult::makePauseExecution(),
                                             Document(),
                                             DocumentSource::GetNextResult::makePauseExecution(),
                                             Document(),
                                             Document(),
                                             DocumentSource::GetNextResult::makePauseExecution(),
                                             Document()},
                                            expCtx);
    auto groupStage = exec::agg::buildStage(group);

    groupStage->setSource(mock.get());

    // There were 3 pauses, so we should expect 3 paused results before any results can be returned.
    ASSERT_TRUE(groupStage->getNext().isPaused());
    ASSERT_TRUE(groupStage->getNext().isPaused());
    ASSERT_TRUE(groupStage->getNext().isPaused());

    // There were 4 documents, so we expect a count of 4.
    auto result = groupStage->getNext();
    ASSERT_TRUE(result.isAdvanced());
    ASSERT_DOCUMENT_EQ(result.releaseDocument(), (Document{{"_id", BSONNULL}, {"count", 4}}));
}

TEST_F(DocumentSourceGroupTest, ShouldBeAbleToPauseLoadingWhileSpilled) {
    auto expCtx = getExpCtx();

    // Allow the $group stage to spill to disk.
    unittest::TempDir tempDir("DocumentSourceGroupTest");
    expCtx->setTempDir(tempDir.path());
    expCtx->setAllowDiskUse(true);
    const size_t maxMemoryUsageBytes = 1000;

    auto&& [parser, _1, _2, _3] = AccumulationStatement::getParser("$push");
    auto accumulatorArg = BSON("" << "$largeStr");
    auto accExpr = parser(expCtx.get(), accumulatorArg.firstElement(), expCtx->variablesParseState);
    AccumulationStatement pushStatement{"spaceHog", accExpr};
    auto groupByExpression =
        ExpressionFieldPath::parse(expCtx.get(), "$_id", expCtx->variablesParseState);
    auto group = DocumentSourceGroup::create(
        expCtx, groupByExpression, {pushStatement}, maxMemoryUsageBytes);

    std::string largeStr(maxMemoryUsageBytes, 'x');
    auto mock =
        exec::agg::MockStage::createForTest({Document{{"_id", 0}, {"largeStr", largeStr}},
                                             DocumentSource::GetNextResult::makePauseExecution(),
                                             Document{{"_id", 1}, {"largeStr", largeStr}},
                                             DocumentSource::GetNextResult::makePauseExecution(),
                                             Document{{"_id", 2}, {"largeStr", largeStr}}},
                                            expCtx);
    auto groupStage = exec::agg::buildStage(group);

    groupStage->setSource(mock.get());

    // There were 2 pauses, so we should expect 2 paused results before any results can be returned.
    ASSERT_TRUE(groupStage->getNext().isPaused());
    ASSERT_TRUE(groupStage->getNext().isPaused());

    // Now we expect to get the results back, although in no particular order.
    stdx::unordered_set<int> idSet;
    for (auto result = groupStage->getNext(); result.isAdvanced(); result = groupStage->getNext()) {
        idSet.insert(result.releaseDocument()["_id"].coerceToInt());
    }
    ASSERT_TRUE(groupStage->getNext().isEOF());

    ASSERT_EQ(idSet.size(), 3UL);
    ASSERT_EQ(idSet.count(0), 1UL);
    ASSERT_EQ(idSet.count(1), 1UL);
    ASSERT_EQ(idSet.count(2), 1UL);
}

TEST_F(DocumentSourceGroupTest, ShouldErrorIfNotAllowedToSpillToDiskAndResultSetIsTooLarge) {
    auto expCtx = getExpCtx();
    const size_t maxMemoryUsageBytes = 1000;
    expCtx->setInRouter(true);  // Disallow external sort.
                                // This is the only way to do this in a debug build.

    auto&& [parser, _1, _2, _3] = AccumulationStatement::getParser("$push");
    auto accumulatorArg = BSON("" << "$largeStr");
    auto accExpr = parser(expCtx.get(), accumulatorArg.firstElement(), expCtx->variablesParseState);
    AccumulationStatement pushStatement{"spaceHog", accExpr};
    auto groupByExpression =
        ExpressionFieldPath::parse(expCtx.get(), "$_id", expCtx->variablesParseState);
    auto group = DocumentSourceGroup::create(
        expCtx, groupByExpression, {pushStatement}, false, maxMemoryUsageBytes);

    std::string largeStr(maxMemoryUsageBytes, 'x');
    auto mock = exec::agg::MockStage::createForTest({Document{{"_id", 0}, {"largeStr", largeStr}},
                                                     Document{{"_id", 1}, {"largeStr", largeStr}}},
                                                    expCtx);
    auto groupStage = exec::agg::buildStage(group);
    groupStage->setSource(mock.get());

    ASSERT_THROWS_CODE(groupStage->getNext(),
                       AssertionException,
                       ErrorCodes::QueryExceededMemoryLimitNoDiskUseAllowed);
}

TEST_F(DocumentSourceGroupTest, ShouldBeAbleToForceSpillAfterReturningResults) {
    auto expCtx = getExpCtx();

    unittest::TempDir tempDir("DocumentSourceGroupTest");
    expCtx->setTempDir(tempDir.path());
    expCtx->setAllowDiskUse(true);
    const size_t maxMemoryUsageBytes = 1024 * 1024;

    auto&& [parser, _1, _2, _3] = AccumulationStatement::getParser("$push");
    auto accumulatorArg = BSON("" << "$largeStr");
    auto accExpr = parser(expCtx.get(), accumulatorArg.firstElement(), expCtx->variablesParseState);
    AccumulationStatement pushStatement{"spaceHog", accExpr};
    auto groupByExpression =
        ExpressionFieldPath::parse(expCtx.get(), "$_id", expCtx->variablesParseState);
    auto group = DocumentSourceGroup::create(
        expCtx, groupByExpression, {pushStatement}, maxMemoryUsageBytes);

    std::string largeStr(maxMemoryUsageBytes / 16, 'x');
    auto mock = exec::agg::MockStage::createForTest({Document{{"_id", 0}, {"largeStr", largeStr}},
                                                     Document{{"_id", 1}, {"largeStr", largeStr}},
                                                     Document{{"_id", 2}, {"largeStr", largeStr}}},
                                                    expCtx);
    auto groupStage = exec::agg::buildStage(group);
    groupStage->setSource(mock.get());

    auto next = groupStage->getNext();
    ASSERT_TRUE(next.isAdvanced());
    stdx::unordered_set<int> idSet;
    idSet.insert(next.releaseDocument()["_id"].coerceToInt());

    const auto* stats = static_cast<const GroupStats*>(groupStage->getSpecificStats());
    ASSERT_EQ(stats->spillingStats.getSpills(), 0);

    groupStage->forceSpill();

    ASSERT_EQ(stats->spillingStats.getSpills(), 1);
    ASSERT_EQ(stats->spillingStats.getSpilledRecords(), 2);

    for (auto result = groupStage->getNext(); result.isAdvanced(); result = groupStage->getNext()) {
        idSet.insert(result.releaseDocument()["_id"].coerceToInt());
    }
    ASSERT_TRUE(groupStage->getNext().isEOF());

    ASSERT_EQ(idSet.size(), 3UL);
    ASSERT_EQ(idSet.count(0), 1UL);
    ASSERT_EQ(idSet.count(1), 1UL);
    ASSERT_EQ(idSet.count(2), 1UL);
}

TEST_F(DocumentSourceGroupTest, ShouldCorrectlyTrackMemoryUsageBetweenPauses) {
    auto expCtx = getExpCtx();
    const size_t maxMemoryUsageBytes = 1000;
    expCtx->setInRouter(true);  // Disallow external sort.
                                // This is the only way to do this in a debug build.

    auto&& [parser, _1, _2, _3] = AccumulationStatement::getParser("$push");
    auto accumulatorArg = BSON("" << "$largeStr");
    auto accExpr = parser(expCtx.get(), accumulatorArg.firstElement(), expCtx->variablesParseState);
    AccumulationStatement pushStatement{"spaceHog", accExpr};
    auto groupByExpression =
        ExpressionFieldPath::parse(expCtx.get(), "$_id", expCtx->variablesParseState);
    auto group = DocumentSourceGroup::create(
        expCtx, groupByExpression, {pushStatement}, false, maxMemoryUsageBytes);

    std::string largeStr(maxMemoryUsageBytes / 2, 'x');
    auto mock =
        exec::agg::MockStage::createForTest({Document{{"_id", 0}, {"largeStr", largeStr}},
                                             DocumentSource::GetNextResult::makePauseExecution(),
                                             Document{{"_id", 1}, {"largeStr", largeStr}},
                                             Document{{"_id", 2}, {"largeStr", largeStr}}},
                                            expCtx);
    auto groupStage = exec::agg::buildStage(group);
    groupStage->setSource(mock.get());

    // The first getNext() should pause.
    ASSERT_TRUE(groupStage->getNext().isPaused());

    // The next should realize it's used too much memory.
    ASSERT_THROWS_CODE(groupStage->getNext(),
                       AssertionException,
                       ErrorCodes::QueryExceededMemoryLimitNoDiskUseAllowed);
}

DEATH_TEST_REGEX_F(DocumentSourceGroupTest,
                   CannotHandleControlEvent,
                   "Tripwire assertion.*10358900") {
    auto expCtx = getExpCtx();
    expCtx->setInRouter(true);  // Disallow external sort.
                                // This is the only way to do this in a debug build.
    auto&& [parser, _1, _2, _3] = AccumulationStatement::getParser("$sum");
    auto accumulatorArg = BSON("" << 1);
    auto accExpr = parser(expCtx.get(), accumulatorArg.firstElement(), expCtx->variablesParseState);
    AccumulationStatement countStatement{"count", accExpr};
    auto group = DocumentSourceGroup::create(
        expCtx, ExpressionConstant::create(expCtx.get(), Value(BSONNULL)), {countStatement}, false);

    // Create a control event.
    MutableDocument doc(Document{{"_id", 0}});
    doc.metadata().setChangeStreamControlEvent();
    auto mock = exec::agg::MockStage::createForTest({doc.freeze()}, expCtx);
    auto groupStage = exec::agg::buildStage(group);

    groupStage->setSource(mock.get());

    ASSERT_THROWS_CODE(groupStage->getNext(), AssertionException, 10358900);
}

DEATH_TEST_REGEX_F(DocumentSourceGroupTest,
                   StreamingGroupCannotHandleControlEvent,
                   "Tripwire assertion.*10358903") {
    auto expCtx = getExpCtx();
    expCtx->setInRouter(true);  // Disallow external sort.
                                // This is the only way to do this in a debug build.

    auto spec = fromjson(R"({
    $_internalStreamingGroup: {
        _id: {
            a: "$a",
            b: "$b"
        },
        a: {
            $first: '$b'
        },
        $monotonicIdFields: [ "a", "b" ]
    }
})");
    auto group = DocumentSourceStreamingGroup::createFromBson(spec.firstElement(), expCtx);
    auto stage = exec::agg::buildStage(group);

    // Create a control event.
    MutableDocument doc(Document{{"_id", 0}});
    doc.metadata().setChangeStreamControlEvent();

    auto mock = exec::agg::MockStage::createForTest({doc.freeze()}, expCtx);
    stage->setSource(mock.get());

    ASSERT_THROWS_CODE(stage->getNext(), AssertionException, 10358903);
}

TEST_F(DocumentSourceGroupTest, ShouldReportSingleFieldGroupKeyAsARename) {
    auto expCtx = getExpCtx();
    VariablesParseState vps = expCtx->variablesParseState;
    auto groupByExpression = ExpressionFieldPath::parse(expCtx.get(), "$x", vps);
    auto group = DocumentSourceGroup::create(expCtx, groupByExpression, {}, false);
    auto modifiedPathsRet = group->getModifiedPaths();
    ASSERT(modifiedPathsRet.type == DocumentSource::GetModPathsReturn::Type::kAllExcept);
    ASSERT_EQ(modifiedPathsRet.paths.size(), 0UL);
    ASSERT_EQ(modifiedPathsRet.renames.size(), 1UL);
    ASSERT_EQ(modifiedPathsRet.renames["_id"], "x");
}

TEST_F(DocumentSourceGroupTest, ShouldReportMultipleFieldGroupKeysAsARename) {
    auto expCtx = getExpCtx();
    VariablesParseState vps = expCtx->variablesParseState;
    auto x = ExpressionFieldPath::parse(expCtx.get(), "$x", vps);
    auto y = ExpressionFieldPath::parse(expCtx.get(), "$y", vps);
    auto groupByExpression = ExpressionObject::create(expCtx.get(), {{"x", x}, {"y", y}});
    auto group = DocumentSourceGroup::create(expCtx, groupByExpression, {}, false);
    auto modifiedPathsRet = group->getModifiedPaths();
    ASSERT(modifiedPathsRet.type == DocumentSource::GetModPathsReturn::Type::kAllExcept);
    ASSERT_EQ(modifiedPathsRet.paths.size(), 0UL);
    ASSERT_EQ(modifiedPathsRet.renames.size(), 2UL);
    ASSERT_EQ(modifiedPathsRet.renames["_id.x"], "x");
    ASSERT_EQ(modifiedPathsRet.renames["_id.y"], "y");
}

TEST_F(DocumentSourceGroupTest, ShouldNotReportDottedGroupKeyAsARename) {
    auto expCtx = getExpCtx();
    VariablesParseState vps = expCtx->variablesParseState;
    auto xDotY = ExpressionFieldPath::parse(expCtx.get(), "$x.y", vps);
    auto group = DocumentSourceGroup::create(expCtx, xDotY, {}, false);
    auto modifiedPathsRet = group->getModifiedPaths();
    ASSERT(modifiedPathsRet.type == DocumentSource::GetModPathsReturn::Type::kAllExcept);
    ASSERT_EQ(modifiedPathsRet.paths.size(), 0UL);
    ASSERT_EQ(modifiedPathsRet.renames.size(), 0UL);
}

TEST_F(DocumentSourceGroupTest, GroupRedactsCorrectWithIdNull) {
    auto spec = fromjson(R"({
        $group: {
            _id: null,
            foo: { $count: {} }
        }
    })");
    auto docSource = DocumentSourceGroup::createFromBson(spec.firstElement(), getExpCtx());
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({"$group":{"_id":"?null","HASH<foo>":{"$sum":"?number"}}})",
        redact(*docSource));
}

TEST_F(DocumentSourceGroupTest, GroupRedactsCorrectWithIdSingleField) {
    auto spec = fromjson(R"({
        $group: {
            _id: '$foo'
        }
    })");
    auto docSource = DocumentSourceGroup::createFromBson(spec.firstElement(), getExpCtx());
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({"$group":{"_id":"$HASH<foo>"}})",
        redact(*docSource));
}

TEST_F(DocumentSourceGroupTest, GroupRedactsCorrectWithIdDocument) {
    auto spec = fromjson(R"({
        $group: {
            _id: {
                x: '$x',
                y: '$z'
            },
            foo: {
                $sum: {
                    $multiply: ['$a.b', '$c', '$d']
                }
            },
            bar: {
                $first: '$baz'
            }
        }
    })");
    auto docSource = DocumentSourceGroup::createFromBson(spec.firstElement(), getExpCtx());
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "$group": {
                "_id": {
                    "HASH<x>": "$HASH<x>",
                    "HASH<y>": "$HASH<z>"
                },
                "HASH<foo>": {
                    "$sum": {
                        "$multiply": [
                            "$HASH<a>.HASH<b>",
                            "$HASH<c>",
                            "$HASH<d>"
                        ]
                    }
                },
                "HASH<bar>": {
                    "$first": "$HASH<baz>"
                }
            }
        })",
        redact(*docSource));
}

TEST_F(DocumentSourceGroupTest, StreamingGroupRedactsCorrectly) {
    auto spec = fromjson(R"({
        $_internalStreamingGroup: {
            _id: {
                a: "$a",
                b: "$b"
            },
            a: {
                $first: '$b'
            },
            $monotonicIdFields: [ "a", "b" ]
        }
    })");
    auto docSource = DocumentSourceStreamingGroup::createFromBson(spec.firstElement(), getExpCtx());
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "$_internalStreamingGroup": {
                "_id": {
                    "HASH<a>": "$HASH<a>",
                    "HASH<b>": "$HASH<b>"
                },
                "HASH<a>": {
                    "$first": "$HASH<b>"
                },
                "$monotonicIdFields": [
                    "HASH<a>",
                    "HASH<b>"
                ]
            }
        })",
        redact(*docSource));
}

TEST_F(DocumentSourceGroupTest, CanHandleEmptyExpressionObject) {
    // Test the case where the _id expression is an empty object. This is handled as a special case,
    // but is not directly reachable from the parser, so we will manually instantiate an
    // ExpressionObject here.
    auto idExpression = ExpressionObject::create(getExpCtx().get(), {});
    std::vector<AccumulationStatement> accumulationStatements;
    auto group =
        DocumentSourceGroup::create(getExpCtx(), idExpression, accumulationStatements, false);
    auto mock = exec::agg::MockStage::createForTest({Document{{"_id"_sd, 0}}}, getExpCtx());
    auto groupStage = exec::agg::buildStage(group);

    groupStage->setSource(mock.get());
    auto next = groupStage->getNext();
    ASSERT(next.isAdvanced());
    // The constant _id value from the $group spec is passed through.
    ASSERT_DOCUMENT_EQ((Document{{"_id", Document{}}}), next.getDocument());
}

TEST_F(DocumentSourceGroupTest, CanOutputExecutionStatsExplainWithoutProcessingDocuments) {
    for (bool flagStatus : {false, true}) {
        RAIIServerParameterControllerForTest featureFlagController("featureFlagQueryMemoryTracking",
                                                                   flagStatus);

        auto expCtx = getExpCtx();
        expCtx->setExplain(ExplainOptions::Verbosity::kExecStats);

        auto&& [parser, _1, _2, _3] = AccumulationStatement::getParser("$sum");
        auto accumulatorArg = BSON("" << 1);
        auto accExpr =
            parser(expCtx.get(), accumulatorArg.firstElement(), expCtx->variablesParseState);
        AccumulationStatement countStatement{"count", accExpr};

        auto group =
            DocumentSourceGroup::create(expCtx,
                                        ExpressionConstant::create(expCtx.get(), Value(BSONNULL)),
                                        {countStatement},
                                        false);
        auto groupStage = exec::agg::buildStage(group);
        groupStage->dispose();

        SerializationOptions explainOpts;
        explainOpts.verbosity = expCtx->getExplain();

        BSONObjBuilder bob;
        bob.appendElements(fromjson(R"({
            $group: {
                _id: {$const: null},
                count: {$sum: {$const: 1}},
                $willBeMerged: false
            },
            maxAccumulatorMemoryUsageBytes: {count: 0},
            totalOutputDataSizeBytes: 0,
            usedDisk: false,
            spills: 0,
            spilledDataStorageSize: 0,
            spilledBytes: 0,
            spilledRecords: 0
        })"));

        if (flagStatus) {
            bob.append("peakTrackedMemBytes", 0);
        }

        ASSERT_DOCUMENT_EQ(Document(bob.obj()), group->serialize(explainOpts).getDocument());
    }
}

TEST_F(DocumentSourceGroupTest, CreateCorrectlyInheritsNeedsMergeValueFromExpCtx) {
    for (auto needsMerge : {true, false}) {
        auto expCtx = getExpCtx();

        auto&& [parser, _1, _2, _3] = AccumulationStatement::getParser("$sum");
        auto accumulatorArg = BSON("" << 1);
        auto accExpr =
            parser(expCtx.get(), accumulatorArg.firstElement(), expCtx->variablesParseState);
        AccumulationStatement countStatement{"count", accExpr};

        auto group =
            DocumentSourceGroup::create(expCtx,
                                        ExpressionConstant::create(expCtx.get(), Value(BSONNULL)),
                                        {countStatement},
                                        needsMerge);

        if (needsMerge) {
            ASSERT_TRUE(group->getGroupProcessor()->willBeMerged());
        } else {
            ASSERT_FALSE(group->getGroupProcessor()->willBeMerged());
        }
    }
}

TEST_F(DocumentSourceGroupTest, CorrectlyReportsTriviallyReferencedExprsFromID) {
    // Verify that DocumentSourceGroupBase::getTriviallyReferencedPaths identifies paths which are
    // used directly, without further computation - "trivially" referenced.
    const auto getTriviallyReferenced = [&](StringData idJsonStr) {
        auto idExpr = fromjson(idJsonStr);
        auto spec = BSON("$group" << BSON("_id" << idExpr));
        auto group = boost::dynamic_pointer_cast<DocumentSourceGroup>(
            DocumentSourceGroup::createFromBson(spec.firstElement(), getExpCtx()));
        return group->getTriviallyReferencedPaths();
    };
    const auto expect = [&](StringData idJsonStr, OrderedPathSet expected) {
        auto actual = getTriviallyReferenced(idJsonStr);
        ASSERT_EQ(actual, expected) << fmt::format(
            "_id:{}, [{}] != [{}]", idJsonStr, fmt::join(actual, ", "), fmt::join(expected, ", "));
    };
    expect(R"(["$a"])", {"a"});
    expect(R"(["$a", "$a"])", {"a"});
    expect(R"({"foo":"$a"})", {"a"});
    expect(R"({"foo":"$a", "bar":"$a"})", {"a"});

    expect(R"(["$a", "$b"])", {"a", "b"});
    expect(R"({"foo":"$a", "bar":"$b"})", {"a", "b"});

    expect(R"({"foo":["$a", "$b"]})", {"a", "b"});
    expect(R"([{"foo":["$a", "$b"]}])", {"a", "b"});
}


TEST_F(DocumentSourceGroupTest, DistributedLogicRequiresMergeIfWithoutShardKey) {
    auto spec = fromjson(R"({$group: {_id: "$x.y", $willBeMerged: true}})");
    auto group = DocumentSourceGroup::createFromBson(spec.firstElement(), getExpCtx());
    auto distributedPlanLogic = group->distributedPlanLogic();
    ASSERT_EQ(distributedPlanLogic->mergingStages.size(), 1UL);
    ASSERT_BSONOBJ_EQ(distributedPlanLogic->shardsStage->serializeToBSONForDebug(),
                      group->serializeToBSONForDebug());
}

TEST_F(DocumentSourceGroupTest, DistributedLogicRequiresMergeIfIdNotSupersetOfShardKey) {
    auto spec = fromjson(R"({$group: {_id: {a: "$a", b: "$b", c: "$c"}, $willBeMerged: true}})");
    boost::intrusive_ptr<DocumentSourceGroup> group = dynamic_cast<DocumentSourceGroup*>(
        DocumentSourceGroup::createFromBson(spec.firstElement(), getExpCtx()).get());
    auto distributedPlanLogic =
        group->pipelineDependentDistributedPlanLogic(makePlanCtx({"a", "b", "d"}));
    ASSERT_EQ(distributedPlanLogic->mergingStages.size(), 1UL);
    ASSERT_BSONOBJ_EQ(distributedPlanLogic->shardsStage->serializeToBSONForDebug(),
                      group->serializeToBSONForDebug());
}

TEST_F(DocumentSourceGroupTest, DistributedLogicDoesNotRequireMergeIfIdEqualToShardKey) {
    RAIIServerParameterControllerForTest controller("featureFlagShardFilteringDistinctScan", true);
    auto spec = fromjson(R"({$group: {_id: {a: "$a", b: "$b", c: "$c"}}})");
    boost::intrusive_ptr<DocumentSourceGroup> group = dynamic_cast<DocumentSourceGroup*>(
        DocumentSourceGroup::createFromBson(spec.firstElement(), getExpCtx()).get());
    auto distributedPlanLogic =
        group->pipelineDependentDistributedPlanLogic(makePlanCtx({"a", "b", "c"}));
    ASSERT_FALSE(distributedPlanLogic);
}

TEST_F(DocumentSourceGroupTest, DistributedLogicDoesRequireMergeIfIdEqualToShardKeyButFFDisabled) {
    RAIIServerParameterControllerForTest controller("featureFlagShardFilteringDistinctScan", false);
    auto spec = fromjson(R"({$group: {_id: {a: "$a", b: "$b", c: "$c"}}})");
    boost::intrusive_ptr<DocumentSourceGroup> group = dynamic_cast<DocumentSourceGroup*>(
        DocumentSourceGroup::createFromBson(spec.firstElement(), getExpCtx()).get());
    auto distributedPlanLogic =
        group->pipelineDependentDistributedPlanLogic(makePlanCtx({"a", "b", "c"}));
    ASSERT_TRUE(distributedPlanLogic);
}

TEST_F(DocumentSourceGroupTest, DistributedLogicDoesNotRequireMergeIfIdSupersetOfShardKey) {
    RAIIServerParameterControllerForTest controller("featureFlagShardFilteringDistinctScan", true);
    auto spec = fromjson(R"({$group: {_id: {a: "$a", b: "$b", c: "$c"}}})");
    boost::intrusive_ptr<DocumentSourceGroup> group = dynamic_cast<DocumentSourceGroup*>(
        DocumentSourceGroup::createFromBson(spec.firstElement(), getExpCtx()).get());
    auto distributedPlanLogic =
        group->pipelineDependentDistributedPlanLogic(makePlanCtx({"a", "c"}));
    ASSERT_FALSE(distributedPlanLogic);
}

TEST_F(DocumentSourceGroupTest,
       DistributedLogicDoesRequireMergeIfIdSupersetOfShardKeyButFFDisabled) {
    RAIIServerParameterControllerForTest controller("featureFlagShardFilteringDistinctScan", false);
    auto spec = fromjson(R"({$group: {_id: {a: "$a", b: "$b", c: "$c"}}})");
    boost::intrusive_ptr<DocumentSourceGroup> group = dynamic_cast<DocumentSourceGroup*>(
        DocumentSourceGroup::createFromBson(spec.firstElement(), getExpCtx()).get());
    auto distributedPlanLogic =
        group->pipelineDependentDistributedPlanLogic(makePlanCtx({"a", "c"}));
    ASSERT_TRUE(distributedPlanLogic);
}

TEST_F(DocumentSourceGroupTest, ShouldUpdateMemoryUsageTrackerDuringGroup) {
    auto expCtx = getExpCtx();

    for (bool flagStatus : {true, false}) {
        RAIIServerParameterControllerForTest featureFlagController("featureFlagQueryMemoryTracking",
                                                                   flagStatus);

        // Pause between input docs so we have a chance to check memory tracking.
        auto mock = exec::agg::MockStage::createForTest(
            {
                Document{{"_id", 0}, {"k", 10}, {"arr", BSON_ARRAY("foo"_sd << "bar"_sd)}},
                DocumentSource::GetNextResult::makePauseExecution(),
                Document{{"_id", 1}, {"k", 10}, {"arr", BSON_ARRAY("baz"_sd << "mongo"_sd)}},
                DocumentSource::GetNextResult::makePauseExecution(),
                Document{{"_id", 2}, {"k", 20}, {"arr", BSON_ARRAY("bird"_sd << "elephant"_sd)}},
                DocumentSource::GetNextResult::makePauseExecution(),
                Document{{"_id", 3}, {"k", 20}, {"arr", BSON_ARRAY("dog"_sd << "giraffe"_sd)}},
            },
            expCtx);

        auto group = [&expCtx, &mock]() {
            auto spec = fromjson(R"({
                $group: {
                    _id: "$k",
                    concatted: {$concatArrays: "$arr"}
                }
            })");
            auto src = DocumentSourceGroup::createFromBson(spec.firstElement(), expCtx.get());
            auto group = boost::dynamic_pointer_cast<DocumentSourceGroup>(src);
            return group;
        }();

        GroupProcessor* groupProcessor = group->getGroupProcessor();
        const MemoryUsageTracker& memTracker = groupProcessor->getMemoryTracker();
        ASSERT_EQUALS(memTracker.inUseTrackedMemoryBytes(), 0);

        auto groupStage = exec::agg::buildStage(group);
        groupStage->setSource(mock.get());

        // Tracked memory increases as rows are processed. Different platforms have different
        // amounts of memory here, so just show that the amount is increasing.
        ASSERT_TRUE(groupStage->getNext().isPaused());
        int64_t curBytes1 = memTracker.inUseTrackedMemoryBytes();
        ASSERT_GREATER_THAN(curBytes1, 0);

        ASSERT_TRUE(groupStage->getNext().isPaused());
        int64_t curBytes2 = memTracker.inUseTrackedMemoryBytes();
        ASSERT_GREATER_THAN(curBytes2, curBytes1);

        ASSERT_TRUE(groupStage->getNext().isPaused());
        int64_t curBytes3 = memTracker.inUseTrackedMemoryBytes();
        ASSERT_GREATER_THAN(curBytes3, curBytes2);

        std::vector<Document> outDocs;
        {
            auto result = groupStage->getNext();
            ASSERT_TRUE(result.isAdvanced());
            int64_t curBytes4 = memTracker.inUseTrackedMemoryBytes();
            ASSERT_GREATER_THAN(curBytes4, curBytes3);
            outDocs.push_back(result.releaseDocument());

            // There are no more input documents, so memory usage stays the same here.
            result = groupStage->getNext();
            ASSERT_TRUE(result.isAdvanced());
            int64_t curBytes5 = memTracker.inUseTrackedMemoryBytes();
            ASSERT_EQUALS(curBytes4, curBytes5);
            outDocs.push_back(result.releaseDocument());
        }

        // Output order of documents will not be deterministic, so sort them.
        std::sort(outDocs.begin(), outDocs.end(), [](const Document& d0, const Document& d1) {
            return Value::compare(d0["_id"], d1["_id"], nullptr) < 0;
        });
        ASSERT_DOCUMENT_EQ(
            outDocs[0],
            Document{fromjson(R"({_id: 10, concatted: ["foo", "bar", "baz", "mongo"]})")});
        ASSERT_DOCUMENT_EQ(
            outDocs[1],
            Document{fromjson(R"({_id: 20, concatted: ["bird", "elephant", "dog", "giraffe"]})")});

        ASSERT_TRUE(groupStage->getNext().isEOF());
        // Tracked memory goes back to zero once all output has been produced.
        ASSERT_EQUALS(memTracker.inUseTrackedMemoryBytes(), 0);
    }
}

/**
 * Check that when the memory tracking feature flag is on, the memory tracking not only tracks
 * memory as expected, but also reports these memory stats upstream to the operation's CurOp
 * instance.
 */
TEST_F(DocumentSourceGroupTest, ShouldUpdateCurOpStatsDuringGroup) {
    auto expCtx = getExpCtx();
    RAIIServerParameterControllerForTest featureFlagController("featureFlagQueryMemoryTracking",
                                                               true);

    // Pause between input docs so we have a chance to check memory tracking.
    auto mock = exec::agg::MockStage::createForTest(
        {
            Document{{"_id", 0}, {"k", 10}, {"arr", BSON_ARRAY("foo"_sd << "bar"_sd)}},
            DocumentSource::GetNextResult::makePauseExecution(),
            Document{{"_id", 1}, {"k", 10}, {"arr", BSON_ARRAY("baz"_sd << "mongo"_sd)}},
            DocumentSource::GetNextResult::makePauseExecution(),
            Document{{"_id", 2}, {"k", 20}, {"arr", BSON_ARRAY("bird"_sd << "elephant"_sd)}},
            DocumentSource::GetNextResult::makePauseExecution(),
            Document{{"_id", 3}, {"k", 20}, {"arr", BSON_ARRAY("dog"_sd << "giraffe"_sd)}},
        },
        expCtx);

    auto group = [&expCtx, &mock]() {
        auto spec = fromjson(R"({
                $group: {
                    _id: "$k",
                    concatted: {$concatArrays: "$arr"}
                }
            })");
        auto src = DocumentSourceGroup::createFromBson(spec.firstElement(), expCtx.get());
        auto group = boost::dynamic_pointer_cast<DocumentSourceGroup>(src);
        return group;
    }();

    auto groupStage = exec::agg::buildStage(group);
    groupStage->setSource(mock.get());

    int64_t inUseTrackedMemoryBytes, peakTrackedMemoryBytes;
    std::tie(inUseTrackedMemoryBytes, peakTrackedMemoryBytes) = getCurOpMemoryStats();
    ASSERT_EQ(inUseTrackedMemoryBytes, 0);
    ASSERT_EQ(peakTrackedMemoryBytes, 0);

    // Tracked memory increases as rows are processed. Different platforms have different
    // amounts of memory here, so just show that the amount is increasing.
    ASSERT_TRUE(groupStage->getNext().isPaused());
    std::tie(inUseTrackedMemoryBytes, peakTrackedMemoryBytes) = getCurOpMemoryStats();
    ASSERT_GREATER_THAN(inUseTrackedMemoryBytes, 0);
    ASSERT_GREATER_THAN(peakTrackedMemoryBytes, 0);

    ASSERT_TRUE(groupStage->getNext().isPaused());
    int64_t prevTrackedMemBytes, prevMaxUsedMemoryBytes;
    prevTrackedMemBytes = inUseTrackedMemoryBytes;
    prevMaxUsedMemoryBytes = peakTrackedMemoryBytes;
    std::tie(inUseTrackedMemoryBytes, peakTrackedMemoryBytes) = getCurOpMemoryStats();
    ASSERT_GREATER_THAN(inUseTrackedMemoryBytes, prevTrackedMemBytes);
    ASSERT_GREATER_THAN_OR_EQUALS(peakTrackedMemoryBytes, prevMaxUsedMemoryBytes);

    ASSERT_TRUE(groupStage->getNext().isPaused());
    prevTrackedMemBytes = inUseTrackedMemoryBytes;
    prevMaxUsedMemoryBytes = peakTrackedMemoryBytes;
    std::tie(inUseTrackedMemoryBytes, peakTrackedMemoryBytes) = getCurOpMemoryStats();
    ASSERT_GREATER_THAN(inUseTrackedMemoryBytes, prevTrackedMemBytes);
    ASSERT_GREATER_THAN(peakTrackedMemoryBytes, prevMaxUsedMemoryBytes);

    std::vector<Document> outDocs;
    {
        auto result = groupStage->getNext();
        ASSERT_TRUE(result.isAdvanced());
        prevTrackedMemBytes = inUseTrackedMemoryBytes;
        prevMaxUsedMemoryBytes = peakTrackedMemoryBytes;
        std::tie(inUseTrackedMemoryBytes, peakTrackedMemoryBytes) = getCurOpMemoryStats();
        ASSERT_GREATER_THAN(inUseTrackedMemoryBytes, prevTrackedMemBytes);
        ASSERT_GREATER_THAN(peakTrackedMemoryBytes, prevMaxUsedMemoryBytes);

        outDocs.push_back(result.releaseDocument());

        // There are no more input documents, so memory usage stays the same here.
        result = groupStage->getNext();
        ASSERT_TRUE(result.isAdvanced());
        prevTrackedMemBytes = inUseTrackedMemoryBytes;
        prevMaxUsedMemoryBytes = peakTrackedMemoryBytes;
        std::tie(inUseTrackedMemoryBytes, peakTrackedMemoryBytes) = getCurOpMemoryStats();
        ASSERT_EQ(inUseTrackedMemoryBytes, prevTrackedMemBytes);
        ASSERT_EQ(peakTrackedMemoryBytes, prevMaxUsedMemoryBytes);
        outDocs.push_back(result.releaseDocument());
    }

    // Output order of documents will not be deterministic, so sort them.
    std::sort(outDocs.begin(), outDocs.end(), [](const Document& d0, const Document& d1) {
        return Value::compare(d0["_id"], d1["_id"], nullptr) < 0;
    });
    ASSERT_DOCUMENT_EQ(
        outDocs[0], Document{fromjson(R"({_id: 10, concatted: ["foo", "bar", "baz", "mongo"]})")});
    ASSERT_DOCUMENT_EQ(
        outDocs[1],
        Document{fromjson(R"({_id: 20, concatted: ["bird", "elephant", "dog", "giraffe"]})")});

    // When we reach the end of the documents, current memory goes to zero, while the max used
    // remains the same.
    ASSERT_TRUE(groupStage->getNext().isEOF());
    std::tie(inUseTrackedMemoryBytes, peakTrackedMemoryBytes) = getCurOpMemoryStats();
    ASSERT_EQUALS(inUseTrackedMemoryBytes, 0);
    ASSERT_EQ(peakTrackedMemoryBytes, prevMaxUsedMemoryBytes);
}

/**
 * Even when $group is consuming memory, show that we don't aggregate memory stats in CurOp when the
 * feature flag is off. In this case, CurOp's memory stats should stay at zero.
 */
TEST_F(DocumentSourceGroupTest, CurOpStatsAreNotUpdatedIfFeatureFlagOff) {
    auto expCtx = getExpCtx();
    RAIIServerParameterControllerForTest featureFlagController("featureFlagQueryMemoryTracking",
                                                               false);

    // Pause between input docs so we have a chance to check memory tracking.
    auto mock = exec::agg::MockStage::createForTest(
        {
            Document{{"_id", 0}, {"k", 10}, {"arr", BSON_ARRAY("foo"_sd << "bar"_sd)}},
            Document{{"_id", 1}, {"k", 10}, {"arr", BSON_ARRAY("baz"_sd << "mongo"_sd)}},
            Document{{"_id", 2}, {"k", 20}, {"arr", BSON_ARRAY("bird"_sd << "elephant"_sd)}},
            Document{{"_id", 3}, {"k", 20}, {"arr", BSON_ARRAY("dog"_sd << "giraffe"_sd)}},
        },
        expCtx);

    auto group = [&expCtx, &mock]() {
        auto spec = fromjson(R"({
                $group: {
                    _id: "$k",
                    concatted: {$concatArrays: "$arr"}
                }
            })");
        auto src = DocumentSourceGroup::createFromBson(spec.firstElement(), expCtx.get());
        auto group = boost::dynamic_pointer_cast<DocumentSourceGroup>(src);
        return group;
    }();

    auto groupStage = exec::agg::buildStage(group);
    groupStage->setSource(mock.get());

    int64_t inUseTrackedMemoryBytes, peakTrackedMemoryBytes;
    std::tie(inUseTrackedMemoryBytes, peakTrackedMemoryBytes) = getCurOpMemoryStats();
    ASSERT_EQ(inUseTrackedMemoryBytes, 0);
    ASSERT_EQ(peakTrackedMemoryBytes, 0);

    std::vector<Document> outDocs;
    {
        auto result = groupStage->getNext();
        ASSERT_TRUE(result.isAdvanced());
        std::tie(inUseTrackedMemoryBytes, peakTrackedMemoryBytes) = getCurOpMemoryStats();
        ASSERT_EQ(inUseTrackedMemoryBytes, 0);
        ASSERT_EQ(peakTrackedMemoryBytes, 0);

        outDocs.push_back(result.releaseDocument());

        // There are no more input documents, so memory usage stays the same here.
        result = groupStage->getNext();
        ASSERT_TRUE(result.isAdvanced());
        std::tie(inUseTrackedMemoryBytes, peakTrackedMemoryBytes) = getCurOpMemoryStats();
        ASSERT_EQ(inUseTrackedMemoryBytes, 0);
        ASSERT_EQ(peakTrackedMemoryBytes, 0);
        outDocs.push_back(result.releaseDocument());
    }

    // Output order of documents will not be deterministic, so sort them.
    std::sort(outDocs.begin(), outDocs.end(), [](const Document& d0, const Document& d1) {
        return Value::compare(d0["_id"], d1["_id"], nullptr) < 0;
    });
    ASSERT_DOCUMENT_EQ(
        outDocs[0], Document{fromjson(R"({_id: 10, concatted: ["foo", "bar", "baz", "mongo"]})")});
    ASSERT_DOCUMENT_EQ(
        outDocs[1],
        Document{fromjson(R"({_id: 20, concatted: ["bird", "elephant", "dog", "giraffe"]})")});

    // All output has been produced.
    ASSERT_TRUE(groupStage->getNext().isEOF());
    std::tie(inUseTrackedMemoryBytes, peakTrackedMemoryBytes) = getCurOpMemoryStats();
    ASSERT_EQ(inUseTrackedMemoryBytes, 0);
    ASSERT_EQ(peakTrackedMemoryBytes, 0);
}

BSONObj toBson(const boost::intrusive_ptr<DocumentSource>& source) {
    std::vector<Value> arr;
    source->serializeToArray(arr);
    ASSERT_EQUALS(arr.size(), 1UL);
    return arr[0].getDocument().toBson();
}

enum class GroupStageType { Default, Streaming };

class Base : public ServiceContextTest {
public:
    Base(GroupStageType groupStageType = GroupStageType::Default)
        : _opCtx(makeOperationContext()),
          _ctx(new ExpressionContextForTest(
              _opCtx.get(),
              AggregateCommandRequest(NamespaceString::createNamespaceString_forTest(ns),
                                      std::vector<mongo::BSONObj>()))),
          _tempDir("DocumentSourceGroupTest"),
          _groupStageType(groupStageType) {}

protected:
    StringData getStageName() const {
        switch (_groupStageType) {
            case GroupStageType::Default:
                return DocumentSourceGroup::kStageName;
            case GroupStageType::Streaming:
                return DocumentSourceStreamingGroup::kStageName;
            default:
                MONGO_UNREACHABLE;
        }
    }

    virtual boost::optional<int64_t> getMaxMemoryUsageBytes() {
        return boost::none;
    }

    boost::intrusive_ptr<DocumentSource> createFromBson(
        BSONElement specElement, boost::intrusive_ptr<ExpressionContext> expressionContext) {
        switch (_groupStageType) {
            case GroupStageType::Default:
                return DocumentSourceGroup::createFromBsonWithMaxMemoryUsage(
                    std::move(specElement), expressionContext, getMaxMemoryUsageBytes());
            case GroupStageType::Streaming:
                return DocumentSourceStreamingGroup::createFromBsonWithMaxMemoryUsage(
                    std::move(specElement), expressionContext, getMaxMemoryUsageBytes());
            default:
                MONGO_UNREACHABLE;
        }
    }

    exec::agg::StagePtr createGroupStage() {
        switch (_groupStageType) {
            case GroupStageType::Default:
                return exec::agg::buildStage(
                    boost::dynamic_pointer_cast<DocumentSourceGroup>(_group));
            case GroupStageType::Streaming:
                return exec::agg::buildStage(
                    boost::dynamic_pointer_cast<DocumentSourceStreamingGroup>(_group));
            default:
                MONGO_UNREACHABLE;
        }
    }

    void createGroup(const BSONObj& spec, bool inShard = false, bool inRouter = false) {
        BSONObjBuilder bob;
        for (auto&& elem : spec) {
            bob << elem;
        }
        if (!inShard) {
            bob << "$willBeMerged" << false;
        } else {
            bob << "$willBeMerged" << true;
        }

        BSONObj namedSpec = BSON(getStageName() << bob.obj());
        BSONElement specElement = namedSpec.firstElement();

        boost::intrusive_ptr<ExpressionContextForTest> expressionContext =
            new ExpressionContextForTest(
                _opCtx.get(),
                AggregateCommandRequest(NamespaceString::createNamespaceString_forTest(ns),
                                        std::vector<mongo::BSONObj>()));
        expressionContext->setAllowDiskUse(true);
        // For $group, 'inShard' implies 'fromRouter' and 'needsMerge'.
        expressionContext->setFromRouter(inShard);
        expressionContext->setNeedsMerge(inShard);
        expressionContext->setInRouter(inRouter);
        // Won't spill to disk properly if it needs to.
        expressionContext->setTempDir(_tempDir.path());

        _group = createFromBson(specElement, expressionContext);
        _groupStage = createGroupStage();
        assertRoundTrips(_group, expressionContext);
    }
    DocumentSourceGroupBase* group() {
        return static_cast<DocumentSourceGroupBase*>(_group.get());
    }
    exec::agg::StagePtr groupStage() {
        return _groupStage;
    }
    /** Assert that iterator state accessors consistently report the source is exhausted. */
    void assertEOF(const boost::intrusive_ptr<exec::agg::Stage>& stage) const {
        // It should be safe to check doneness multiple times
        ASSERT(stage->getNext().isEOF());
        ASSERT(stage->getNext().isEOF());
        ASSERT(stage->getNext().isEOF());
    }

    boost::intrusive_ptr<ExpressionContextForTest> ctx() const {
        return _ctx;
    }

private:
    /** Check that the group's spec round trips. */
    void assertRoundTrips(const boost::intrusive_ptr<DocumentSource>& group,
                          const boost::intrusive_ptr<ExpressionContext>& expCtx) {
        // We don't check against the spec that generated 'group' originally, because
        // $const operators may be introduced in the first serialization.
        BSONObj spec = toBson(group);
        BSONElement specElement = spec.firstElement();
        boost::intrusive_ptr<DocumentSource> generated = createFromBson(specElement, expCtx);
        ASSERT_BSONOBJ_EQ(spec, toBson(generated));
    }
    std::unique_ptr<QueryTestServiceContext> _queryServiceContext;
    ServiceContext::UniqueOperationContext _opCtx;
    boost::intrusive_ptr<ExpressionContextForTest> _ctx;
    boost::intrusive_ptr<DocumentSource> _group;
    exec::agg::StagePtr _groupStage;
    unittest::TempDir _tempDir;
    GroupStageType _groupStageType;
};

class ParseErrorBase : public Base {
public:
    ~ParseErrorBase() override {}
    void _doTest() final {
        ASSERT_THROWS(createGroup(spec()), AssertionException);
    }

protected:
    virtual BSONObj spec() = 0;
};

class ExpressionBase : public Base {
public:
    ~ExpressionBase() override {}
    void _doTest() final {
        createGroup(spec());
        auto mockStage = exec::agg::MockStage::createForTest(Document(doc()), ctx());
        groupStage()->setSource(mockStage.get());
        // A group result is available.
        auto next = groupStage()->getNext();
        ASSERT(next.isAdvanced());
        // The constant _id value from the $group spec is passed through.
        ASSERT_BSONOBJ_EQ(expected(), next.getDocument().toBson());
    }

protected:
    virtual BSONObj doc() = 0;
    virtual BSONObj spec() = 0;
    virtual BSONObj expected() = 0;
};

class IdConstantBase : public ExpressionBase {
    BSONObj doc() override {
        return BSONObj();
    }
    BSONObj expected() override {
        // Since spec() specifies a constant _id, its value will be passed through.
        return spec();
    }
};

/** $group spec is not an object. */
class NonObject : public Base {
public:
    void _doTest() final {
        BSONObj spec = BSON(getStageName() << "foo");
        BSONElement specElement = spec.firstElement();
        ASSERT_THROWS(createFromBson(specElement, ctx()), AssertionException);
    }
};

/** $group spec is an empty object. */
class EmptySpec : public ParseErrorBase {
    BSONObj spec() override {
        return BSONObj();
    }
};

/** $group _id is an empty object. */
class IdEmptyObject : public IdConstantBase {
    BSONObj spec() override {
        return BSON("_id" << BSONObj());
    }
};

/** $group _id is an empty object surrounded by a no-op $expr */
class IdEmptyObjectWithExpr : public IdConstantBase {
    BSONObj spec() override {
        return BSON("_id" << BSON("$expr" << BSONObj()));
    }
    BSONObj expected() override {
        return BSON("_id" << BSONObj());
    }
};

/** $group _id is an empty object surrounded by multiple no-op $exprs */
class IdEmptyObjectWithManyExpr : public IdConstantBase {
    BSONObj spec() override {
        return BSON("_id" << BSON("$expr" << BSON("$expr" << BSON("$expr" << BSONObj()))));
    }
    BSONObj expected() override {
        return BSON("_id" << BSONObj());
    }
};

/** $group _id is computed from an object expression. */
class IdObjectExpression : public ExpressionBase {
    BSONObj doc() override {
        return BSON("a" << 6);
    }
    BSONObj spec() override {
        return BSON("_id" << BSON("z" << "$a"));
    }
    BSONObj expected() override {
        return BSON("_id" << BSON("z" << 6));
    }
};

/** $group _id is specified as an invalid object expression. */
class IdInvalidObjectExpression : public ParseErrorBase {
    BSONObj spec() override {
        return BSON("_id" << BSON("$add" << 1 << "$and" << 1));
    }
};

/** $group with two _id specs. */
class TwoIdSpecs : public ParseErrorBase {
    BSONObj spec() override {
        return BSON("_id" << 1 << "_id" << 2);
    }
};

/** $group _id is the empty string. */
class IdEmptyString : public IdConstantBase {
    BSONObj spec() override {
        return BSON("_id" << "");
    }
};

/** $group _id is a std::string constant. */
class IdStringConstant : public IdConstantBase {
    BSONObj spec() override {
        return BSON("_id" << "abc");
    }
};

/** $group _id is a field path expression. */
class IdFieldPath : public ExpressionBase {
    BSONObj doc() override {
        return BSON("a" << 5);
    }
    BSONObj spec() override {
        return BSON("_id" << "$a");
    }
    BSONObj expected() override {
        return BSON("_id" << 5);
    }
};

/** $group with _id set to an invalid field path. */
class IdInvalidFieldPath : public ParseErrorBase {
    BSONObj spec() override {
        return BSON("_id" << "$a..");
    }
};

/** $group _id is a numeric constant. */
class IdNumericConstant : public IdConstantBase {
    BSONObj spec() override {
        return BSON("_id" << 2);
    }
};

/** $group _id is an array constant. */
class IdArrayConstant : public IdConstantBase {
    BSONObj spec() override {
        return BSON("_id" << BSON_ARRAY(1 << 2));
    }
};

/** $group _id is a regular expression (not supported). */
class IdRegularExpression : public IdConstantBase {
    BSONObj spec() override {
        return fromjson("{_id:/a/}");
    }
};

/** The name of an aggregate field is specified with a $ prefix. */
class DollarAggregateFieldName : public ParseErrorBase {
    BSONObj spec() override {
        return BSON("_id" << 1 << "$foo" << BSON("$sum" << 1));
    }
};

/** An aggregate field spec that is not an object. */
class NonObjectAggregateSpec : public ParseErrorBase {
    BSONObj spec() override {
        return BSON("_id" << 1 << "a" << 1);
    }
};

/** An aggregate field spec that is not an object. */
class EmptyObjectAggregateSpec : public ParseErrorBase {
    BSONObj spec() override {
        return BSON("_id" << 1 << "a" << BSONObj());
    }
};

/** An aggregate field spec with an invalid accumulator operator. */
class BadAccumulator : public ParseErrorBase {
    BSONObj spec() override {
        return BSON("_id" << 1 << "a" << BSON("$bad" << 1));
    }
};

/** An aggregate field spec with an array argument. */
class SumArray : public ParseErrorBase {
    BSONObj spec() override {
        return BSON("_id" << 1 << "a" << BSON("$sum" << BSONArray()));
    }
};

/** Multiple accumulator operators for a field. */
class MultipleAccumulatorsForAField : public ParseErrorBase {
    BSONObj spec() override {
        return BSON("_id" << 1 << "a" << BSON("$sum" << 1 << "$push" << 1));
    }
};

/** Aggregation using duplicate field names is allowed currently. */
class DuplicateAggregateFieldNames : public ExpressionBase {
    BSONObj doc() override {
        return BSONObj();
    }
    BSONObj spec() override {
        return BSON("_id" << 0 << "z" << BSON("$sum" << 1) << "z" << BSON("$push" << 1));
    }
    BSONObj expected() override {
        return BSON("_id" << 0 << "z" << 1 << "z" << BSON_ARRAY(1));
    }
};

/** Aggregate the value of an object expression. */
class AggregateObjectExpression : public ExpressionBase {
    BSONObj doc() override {
        return BSON("a" << 6);
    }
    BSONObj spec() override {
        return BSON("_id" << 0 << "z" << BSON("$first" << BSON("x" << "$a")));
    }
    BSONObj expected() override {
        return BSON("_id" << 0 << "z" << BSON("x" << 6));
    }
};

/** Aggregate the value of an operator expression. */
class AggregateOperatorExpression : public ExpressionBase {
    BSONObj doc() override {
        return BSON("a" << 6);
    }
    BSONObj spec() override {
        return BSON("_id" << 0 << "z" << BSON("$first" << "$a"));
    }
    BSONObj expected() override {
        return BSON("_id" << 0 << "z" << 6);
    }
};

struct ValueCmp {
    bool operator()(const Value& a, const Value& b) const {
        return ValueComparator().evaluate(a < b);
    }
};
typedef std::map<Value, Document, ValueCmp> IdMap;

class CheckResultsBase : public Base {
public:
    CheckResultsBase(GroupStageType groupStageType = GroupStageType::Default)
        : Base(groupStageType) {}
    ~CheckResultsBase() override {}
    void _doTest() override {
        runSharded(false);
        runSharded(true);
    }
    void runSharded(bool sharded) {
        createGroup(groupSpec());
        auto mockStage = exec::agg::MockStage::createForTest(inputData(), ctx());
        groupStage()->setSource(mockStage.get());

        boost::intrusive_ptr<exec::agg::Stage> sink = groupStage();
        if (sharded) {
            sink = createMerger();
            // Serialize and re-parse the shard stage.
            createGroup(toBson(group())[group()->getSourceName()].Obj(), true);
            groupStage()->setSource(mockStage.get());
            sink->setSource(groupStage().get());
        }

        checkResultSet(sink);
    }

protected:
    virtual std::deque<DocumentSource::GetNextResult> inputData() {
        return {};
    }
    virtual BSONObj groupSpec() {
        return BSON("_id" << 0);
    }
    /** Expected results.  Must be sorted by _id to ensure consistent ordering. */
    virtual BSONObj expectedResultSet() {
        BSONObj wrappedResult =
            // fromjson cannot parse an array, so place the array within an object.
            fromjson(std::string("{'':") + expectedResultSetString() + "}");
        return wrappedResult[""].embeddedObject().getOwned();
    }
    /** Expected results.  Must be sorted by _id to ensure consistent ordering. */
    virtual std::string expectedResultSetString() {
        return "[]";
    }
    boost::intrusive_ptr<exec::agg::Stage> createMerger() {
        // Set up a group merger to simulate merging results in the router.  In this
        // case only one shard is in use.
        auto distributedPlanLogic = group()->distributedPlanLogic();
        ASSERT(distributedPlanLogic);
        ASSERT_EQ(distributedPlanLogic->mergingStages.size(), 1)
            << distributedPlanLogic->mergingStages.size();
        auto mergingStage = *distributedPlanLogic->mergingStages.begin();
        ASSERT_NOT_EQUALS(group(), mergingStage);
        ASSERT_FALSE(static_cast<bool>(distributedPlanLogic->mergeSortPattern));
        return exec::agg::buildStage(mergingStage);
    }
    void checkResultSet(const boost::intrusive_ptr<exec::agg::Stage>& sink) {
        // Load the results from the DocumentSourceGroup and sort them by _id.
        IdMap resultSet;
        for (auto output = sink->getNext(); output.isAdvanced(); output = sink->getNext()) {
            // Save the current result.
            Value id = output.getDocument().getField("_id");
            resultSet[id] = output.releaseDocument();
        }
        // Verify the DocumentSourceGroup is exhausted.
        assertEOF(sink);

        // Convert results to BSON once they all have been retrieved (to detect any errors
        // resulting from incorrectly shared sub objects).
        BSONArrayBuilder bsonResultSet;
        for (IdMap::const_iterator i = resultSet.begin(); i != resultSet.end(); ++i) {
            bsonResultSet << i->second;
        }
        // Check the result set.
        ASSERT_BSONOBJ_EQ(expectedResultSet(), bsonResultSet.arr());
    }
};

/** An empty collection generates no results. */
class EmptyCollection : public CheckResultsBase {};

/** A $group performed on a single document. */
class SingleDocument : public CheckResultsBase {
    std::deque<DocumentSource::GetNextResult> inputData() override {
        return {DOC("a" << 1)};
    }
    BSONObj groupSpec() override {
        return BSON("_id" << 0 << "a" << BSON("$sum" << "$a"));
    }
    std::string expectedResultSetString() override {
        return "[{_id:0,a:1}]";
    }
};

/** A $group performed on two values for a single key. */
class TwoValuesSingleKey : public CheckResultsBase {
    std::deque<DocumentSource::GetNextResult> inputData() override {
        return {DOC("a" << 1), DOC("a" << 2)};
    }
    BSONObj groupSpec() override {
        return BSON("_id" << 0 << "a" << BSON("$push" << "$a"));
    }
    std::string expectedResultSetString() override {
        return "[{_id:0,a:[1,2]}]";
    }
};

/** A $group performed on two values with one key each. */
class TwoValuesTwoKeys : public CheckResultsBase {
    std::deque<DocumentSource::GetNextResult> inputData() override {
        return {DOC("_id" << 0 << "a" << 1), DOC("_id" << 1 << "a" << 2)};
    }
    BSONObj groupSpec() override {
        return BSON("_id" << "$_id"
                          << "a" << BSON("$push" << "$a"));
    }
    std::string expectedResultSetString() override {
        return "[{_id:0,a:[1]},{_id:1,a:[2]}]";
    }
};

/** A $group performed on two values with two keys each. */
class FourValuesTwoKeys : public CheckResultsBase {
    std::deque<DocumentSource::GetNextResult> inputData() override {
        return {DOC("id" << 0 << "a" << 1),
                DOC("id" << 1 << "a" << 2),
                DOC("id" << 0 << "a" << 3),
                DOC("id" << 1 << "a" << 4)};
    }
    BSONObj groupSpec() override {
        return BSON("_id" << "$id"
                          << "a" << BSON("$push" << "$a"));
    }
    std::string expectedResultSetString() override {
        return "[{_id:0,a:[1,3]},{_id:1,a:[2,4]}]";
    }
};

/** A $group performed on two values with two keys each and two accumulator operations. */
class FourValuesTwoKeysTwoAccumulators : public CheckResultsBase {
    std::deque<DocumentSource::GetNextResult> inputData() override {
        return {DOC("id" << 0 << "a" << 1),
                DOC("id" << 1 << "a" << 2),
                DOC("id" << 0 << "a" << 3),
                DOC("id" << 1 << "a" << 4)};
    }
    BSONObj groupSpec() override {
        return BSON("_id" << "$id"
                          << "list" << BSON("$push" << "$a") << "sum"
                          << BSON("$sum" << BSON("$divide" << BSON_ARRAY("$a" << 2))));
    }
    std::string expectedResultSetString() override {
        return "[{_id:0,list:[1,3],sum:2},{_id:1,list:[2,4],sum:3}]";
    }
};

/** Null and undefined _id values are grouped together. */
class GroupNullUndefinedIds : public CheckResultsBase {
    std::deque<DocumentSource::GetNextResult> inputData() override {
        return {DOC("a" << BSONNULL << "b" << 100), DOC("b" << 10)};
    }
    BSONObj groupSpec() override {
        return BSON("_id" << "$a"
                          << "sum" << BSON("$sum" << "$b"));
    }
    std::string expectedResultSetString() override {
        return "[{_id:null,sum:110}]";
    }
};

/** A complex _id expression. */
class ComplexId : public CheckResultsBase {
    std::deque<DocumentSource::GetNextResult> inputData() override {
        return {DOC("a" << "de"_sd
                        << "b"
                        << "ad"_sd
                        << "c"
                        << "beef"_sd
                        << "d"
                        << ""_sd),
                DOC("a" << "d"_sd
                        << "b"
                        << "eadbe"_sd
                        << "c"
                        << ""_sd
                        << "d"
                        << "ef"_sd)};
    }
    BSONObj groupSpec() override {
        return BSON("_id" << BSON("$concat" << BSON_ARRAY("$a" << "$b"
                                                               << "$c"
                                                               << "$d")));
    }
    std::string expectedResultSetString() override {
        return "[{_id:'deadbeef'}]";
    }
};

/** An undefined accumulator value is dropped. */
class UndefinedAccumulatorValue : public CheckResultsBase {
    std::deque<DocumentSource::GetNextResult> inputData() override {
        return {Document()};
    }
    BSONObj groupSpec() override {
        return BSON("_id" << 0 << "first" << BSON("$first" << "$missing"));
    }
    std::string expectedResultSetString() override {
        return "[{_id:0, first:null}]";
    }
};

/** Simulate merging sharded results in the router. */
class RouterMerger : public CheckResultsBase {
public:
    void _doTest() final {
        auto mockStage = exec::agg::MockStage::createForTest({"{_id:0,list:[1,2]}",
                                                              "{_id:1,list:[3,4]}",
                                                              "{_id:0,list:[10,20]}",
                                                              "{_id:1,list:[30,40]}"},
                                                             ctx());

        // Create a group stage.
        createGroup(BSON("_id" << "$x"
                               << "list" << BSON("$push" << "$y")));
        // Create a merger version of the stage.
        boost::intrusive_ptr<exec::agg::Stage> group = createMerger();
        // Attach the merger to the synthetic shard results.
        group->setSource(mockStage.get());
        // Check the merger's output.
        checkResultSet(group);
    }

private:
    std::string expectedResultSetString() override {
        return "[{_id:0,list:[1,2,10,20]},{_id:1,list:[3,4,30,40]}]";
    }
};

/** Dependant field paths. */
class Dependencies : public Base {
public:
    void _doTest() final {
        createGroup(fromjson("{_id:'$x',a:{$sum:'$y.z'},b:{$avg:{$add:['$u','$v']}}}"));
        DepsTracker dependencies;
        ASSERT_EQUALS(DepsTracker::State::EXHAUSTIVE_ALL, group()->getDependencies(&dependencies));
        ASSERT_EQUALS(4U, dependencies.fields.size());
        // Dependency from _id expression.
        ASSERT_EQUALS(1U, dependencies.fields.count("x"));
        // Dependencies from accumulator expressions.
        ASSERT_EQUALS(1U, dependencies.fields.count("y.z"));
        ASSERT_EQUALS(1U, dependencies.fields.count("u"));
        ASSERT_EQUALS(1U, dependencies.fields.count("v"));
        ASSERT_EQUALS(false, dependencies.needWholeDocument);
        ASSERT_EQUALS(false, dependencies.getNeedsMetadata(DocumentMetadataFields::kTextScore));
    }
};

/**
 * A std::string constant (not a field path) as an _id expression and passed to an accumulator.
 * SERVER-6766
 */
class StringConstantIdAndAccumulatorExpressions : public CheckResultsBase {
    std::deque<DocumentSource::GetNextResult> inputData() override {
        return {Document()};
    }
    BSONObj groupSpec() override {
        return fromjson("{_id:{$const:'$_id...'},a:{$push:{$const:'$a...'}}}");
    }
    std::string expectedResultSetString() override {
        return "[{_id:'$_id...',a:['$a...']}]";
    }
};

/** An array constant passed to an accumulator. */
class ArrayConstantAccumulatorExpression : public CheckResultsBase {
public:
    void _doTest() final {
        // A parse exception is thrown when a raw array is provided to an accumulator.
        ASSERT_THROWS(createGroup(fromjson("{_id:1,a:{$push:[4,5,6]}}")), AssertionException);
        // Run standard base tests.
        CheckResultsBase::_doTest();
    }
    std::deque<DocumentSource::GetNextResult> inputData() override {
        return {Document()};
    }
    BSONObj groupSpec() override {
        // An array can be specified using $const.
        return fromjson("{_id:[1,2,3],a:{$push:{$const:[4,5,6]}}}");
    }
    std::string expectedResultSetString() override {
        return "[{_id:[1,2,3],a:[[4,5,6]]}]";
    }
};

class StreamingSimple final : public CheckResultsBase {
public:
    StreamingSimple() : CheckResultsBase(GroupStageType::Streaming) {}

private:
    std::deque<DocumentSource::GetNextResult> inputData() final {
        return {Document(BSON("a" << 1 << "b" << 1)),
                Document(BSON("a" << 1 << "b" << 2)),
                Document(BSON("a" << 2 << "b" << 3)),
                Document(BSON("a" << 2 << "b" << 1))};
    }
    BSONObj groupSpec() final {
        return BSON("_id" << "$a"
                          << "sum" << BSON("$sum" << "$b") << "$monotonicIdFields"
                          << BSON_ARRAY("_id"));
    }
    std::string expectedResultSetString() final {
        return "[{_id:1,sum:3},{_id:2,sum:4}]";
    }
};

constexpr size_t kBigStringSize = 1024;
const std::string kBigString(kBigStringSize, 'a');

class CheckResultsAndSpills : public CheckResultsBase {
public:
    CheckResultsAndSpills(GroupStageType groupStageType, uint64_t expectedSpills)
        : CheckResultsBase(groupStageType), _expectedSpills(expectedSpills) {}

    void _doTest() final {
        for (int sharded = 0; sharded < 2; ++sharded) {
            runSharded(sharded);
            const auto* groupStats =
                static_cast<const GroupStats*>(groupStage()->getSpecificStats());
            ASSERT_EQ(groupStats->spillingStats.getSpills(), _expectedSpills);
        }
    }

private:
    uint64_t _expectedSpills;
};

template <GroupStageType groupStageType, uint64_t expectedSpills>
class StreamingSpillTest : public CheckResultsAndSpills {
public:
    StreamingSpillTest() : CheckResultsAndSpills(groupStageType, expectedSpills) {}

private:
    static constexpr int kCount = 11;

    std::deque<DocumentSource::GetNextResult> inputData() final {
        std::deque<DocumentSource::GetNextResult> queue;
        for (int i = 0; i < kCount; ++i) {
            queue.emplace_back(Document(BSON("a" << i << "b" << kBigString)));
        }
        return queue;
    }

    BSONObj groupSpec() final {
        if constexpr (groupStageType == GroupStageType::Streaming) {
            return fromjson("{_id: '$a', big_array: {$push: '$b'}, $monotonicIdFields: ['_id']}");
        } else {
            return fromjson("{_id: '$a', big_array: {$push: '$b'}}");
        }
    }

    boost::optional<int64_t> getMaxMemoryUsageBytes() final {
        return 10 * kBigStringSize;
    }

    BSONObj expectedResultSet() final {
        BSONArrayBuilder result;
        for (int i = 0; i < kCount; ++i) {
            result << BSON("_id" << i << "big_array" << BSON_ARRAY(kBigString));
        }
        return result.arr();
    }
};

class WithoutStreamingSpills final
    : public StreamingSpillTest<GroupStageType::Default, 2 /*expectedSpills*/> {};
class StreamingDoesNotSpill final
    : public StreamingSpillTest<GroupStageType::Streaming, 0 /*expectedSpills*/> {};

class StreamingCanSpill final : public CheckResultsAndSpills {
public:
    StreamingCanSpill() : CheckResultsAndSpills(GroupStageType::Streaming, 2 /*expectedSpills*/) {}

private:
    static constexpr int kCount = 11;

    std::deque<DocumentSource::GetNextResult> inputData() final {
        std::deque<DocumentSource::GetNextResult> queue;
        for (int i = 0; i < kCount; ++i) {
            queue.emplace_back(Document(BSON("x" << 0 << "y" << i << "b" << kBigString)));
        }
        return queue;
    }

    BSONObj groupSpec() final {
        auto id = BSON("x" << "$x"
                           << "y"
                           << "$y");
        return BSON("_id" << id << "big_array" << BSON("$push" << "$b") << "$monotonicIdFields"
                          << BSON_ARRAY("x"));
    }

    boost::optional<int64_t> getMaxMemoryUsageBytes() final {
        return 10 * kBigStringSize;
    }

    BSONObj expectedResultSet() final {
        BSONArrayBuilder result;
        for (int i = 0; i < kCount; ++i) {
            auto id = BSON("x" << 0 << "y" << i);
            result << BSON("_id" << id << "big_array" << BSON_ARRAY(kBigString));
        }
        return result.arr();
    }
};

class StreamingAlternatingSpillAndNoSpillBatches : public CheckResultsAndSpills {
public:
    StreamingAlternatingSpillAndNoSpillBatches()
        : CheckResultsAndSpills(GroupStageType::Streaming, expectedSpills()) {}

private:
    static constexpr int kCount = 12;

    int expectedSpills() const {
        // 'DocumentSourceGroup' has the knob called 'internalQueryEnableAggressiveSpillsInGroup'
        // used to spill more aggressively when turned on.
        return internalQueryEnableAggressiveSpillsInGroup.loadRelaxed() ? kCount : 4;
    }

    std::deque<DocumentSource::GetNextResult> inputData() final {
        std::deque<DocumentSource::GetNextResult> queue;
        for (int i = 0; i < kCount; ++i) {
            // For groups with i % 3 == 0 and i % 3 == 1 there should be no spilling, but groups
            // with i % 3 == 2 should spill.
            for (int j = 0; j < (i % 3) + 1; ++j) {
                queue.emplace_back(Document(BSON("a" << i << "b" << kBigString)));
            }
        }
        return queue;
    }

    BSONObj groupSpec() final {
        return BSON("_id" << "$a"
                          << "big_array" << BSON("$push" << "$b") << "$monotonicIdFields"
                          << BSON_ARRAY("_id"));
    }

    boost::optional<int64_t> getMaxMemoryUsageBytes() final {
        return (25 * kBigStringSize) / 10;
    }

    BSONObj expectedResultSet() final {
        BSONArrayBuilder result;
        for (int i = 0; i < kCount; ++i) {
            BSONArrayBuilder bigArrayBuilder;
            for (int j = 0; j < (i % 3) + 1; ++j) {
                bigArrayBuilder << kBigString;
            }
            result << BSON("_id" << i << "big_array" << bigArrayBuilder.arr());
        }
        return result.arr();
    }
};

class StreamingComplex final : public CheckResultsBase {
public:
    StreamingComplex() : CheckResultsBase(GroupStageType::Streaming) {}

private:
    static constexpr int kCount = 3;

    std::deque<DocumentSource::GetNextResult> inputData() final {
        std::deque<DocumentSource::GetNextResult> queue;
        for (int i = 0; i < kCount; ++i) {
            for (int j = 0; j < kCount; ++j) {
                for (int k = 0; k < kCount; ++k) {
                    queue.emplace_back(Document(BSON("x" << i << "y" << j << "z" << k)));
                }
            }
        }
        return queue;
    }

    BSONObj groupSpec() final {
        BSONObj id = BSON("x" << "$x"
                              << "y"
                              << "$y");
        return BSON("_id" << id << "sum" << BSON("$sum" << "$z") << "$monotonicIdFields"
                          << BSON_ARRAY("x"));
    }

    boost::optional<int64_t> getMaxMemoryUsageBytes() final {
        return 10 * kBigStringSize;
    }

    BSONObj expectedResultSet() final {
        BSONArrayBuilder result;
        for (int i = 0; i < kCount; ++i) {
            for (int j = 0; j < kCount; ++j) {
                result << BSON("_id" << BSON("x" << i << "y" << j) << "sum"
                                     << (kCount * (kCount - 1)) / 2);
            }
        }
        return result.arr();
    }
};

class StreamingMultipleMonotonicFields final : public CheckResultsBase {
public:
    StreamingMultipleMonotonicFields() : CheckResultsBase(GroupStageType::Streaming) {}

private:
    static constexpr int kCount = 6;
    std::deque<DocumentSource::GetNextResult> inputData() final {
        std::deque<DocumentSource::GetNextResult> queue;
        generateInputOutput([&queue](int x, int y) {
            for (int i = 0; i < kCount; ++i) {
                queue.emplace_back(Document(BSON("x" << x << "y" << y << "z" << i)));
            }
        });
        return queue;
    }

    BSONObj groupSpec() final {
        return fromjson(
            "{_id: {x: '$x', y: '$y'}, sum: {$sum: '$z'}, $monotonicIdFields: ['x', 'y']}");
    }

    boost::optional<int64_t> getMaxMemoryUsageBytes() final {
        return 10 * kBigStringSize;
    }

    BSONObj expectedResultSet() final {
        BSONArrayBuilder result;
        const int sum = (kCount * (kCount - 1)) / 2;
        generateInputOutput([&](int x, int y) {
            result << BSON("_id" << BSON("x" << x << "y" << y) << "sum" << sum);
        });
        return result.arr();
    }

    template <typename Callback>
    void generateInputOutput(const Callback& callback) {
        int x = 0;
        int y = 0;
        for (int i = 0; i < kCount; ++i) {
            callback(x, y);
            int state = i % 3;
            if (state == 0) {
                x++;
            } else if (state == 1) {
                y++;
            } else {
                x++;
                y++;
            }
        }
    }
};

class All : public unittest::OldStyleSuiteSpecification {
public:
    All() : OldStyleSuiteSpecification("DocumentSourceGroupTests") {}

    void setupTests() override {
        add<NonObject>();
        add<EmptySpec>();
        add<IdEmptyObject>();
        add<IdEmptyObjectWithExpr>();
        add<IdEmptyObjectWithManyExpr>();
        add<IdObjectExpression>();
        add<IdInvalidObjectExpression>();
        add<TwoIdSpecs>();
        add<IdEmptyString>();
        add<IdStringConstant>();
        add<IdFieldPath>();
        add<IdInvalidFieldPath>();
        add<IdNumericConstant>();
        add<IdArrayConstant>();
        add<IdRegularExpression>();
        add<DollarAggregateFieldName>();
        add<NonObjectAggregateSpec>();
        add<EmptyObjectAggregateSpec>();
        add<BadAccumulator>();
        add<SumArray>();
        add<MultipleAccumulatorsForAField>();
        add<DuplicateAggregateFieldNames>();
        add<AggregateObjectExpression>();
        add<AggregateOperatorExpression>();
        add<EmptyCollection>();
        add<SingleDocument>();
        add<TwoValuesSingleKey>();
        add<TwoValuesTwoKeys>();
        add<FourValuesTwoKeys>();
        add<FourValuesTwoKeysTwoAccumulators>();
        add<GroupNullUndefinedIds>();
        add<ComplexId>();
        add<UndefinedAccumulatorValue>();
        add<RouterMerger>();
        add<Dependencies>();
        add<StringConstantIdAndAccumulatorExpressions>();
        add<ArrayConstantAccumulatorExpression>();

        add<StreamingSimple>();
        add<WithoutStreamingSpills>();
        add<StreamingDoesNotSpill>();
        add<StreamingCanSpill>();
        add<StreamingAlternatingSpillAndNoSpillBatches>();
        add<StreamingComplex>();
        add<StreamingMultipleMonotonicFields>();
#if 0
        // TODO (SERVER-23318): Enable tests
        add<StreamingOptimization>();
        add<StreamingWithMultipleIdFields>();
        add<NoOptimizationIfMissingDoubleSort>();
        add<NoOptimizationWithRawRoot>();
        add<NoOptimizationIfUsingExpressions>();
        add<StreamingWithMultipleLevels>();
        add<StreamingWithConstant>();
        add<StreamingWithEmptyId>();
        add<StreamingWithRootSubfield>();
        add<StreamingWithConstantAndFieldPath>();
        add<StreamingWithFieldRepeated>();
#endif
    }
};

unittest::OldStyleSuiteInitializer<All> myall;

}  // namespace
}  // namespace mongo
