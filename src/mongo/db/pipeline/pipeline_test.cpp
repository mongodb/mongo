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

#include <absl/container/flat_hash_map.h>
#include <absl/container/node_hash_set.h>
#include <bitset>
#include <boost/optional.hpp>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/json.h"
#include "mongo/db/client.h"
#include "mongo/db/database_name.h"
#include "mongo/db/exec/document_value/document_value_test_util.h"
#include "mongo/db/pipeline/aggregate_command_gen.h"
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/dependencies.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_change_stream.h"
#include "mongo/db/pipeline/document_source_change_stream_add_post_image.h"
#include "mongo/db/pipeline/document_source_change_stream_add_pre_image.h"
#include "mongo/db/pipeline/document_source_change_stream_gen.h"
#include "mongo/db/pipeline/document_source_change_stream_handle_topology_change.h"
#include "mongo/db/pipeline/document_source_facet.h"
#include "mongo/db/pipeline/document_source_graph_lookup.h"
#include "mongo/db/pipeline/document_source_internal_split_pipeline.h"
#include "mongo/db/pipeline/document_source_lookup.h"
#include "mongo/db/pipeline/document_source_match.h"
#include "mongo/db/pipeline/document_source_mock.h"
#include "mongo/db/pipeline/document_source_out.h"
#include "mongo/db/pipeline/document_source_project.h"
#include "mongo/db/pipeline/document_source_redact.h"
#include "mongo/db/pipeline/document_source_sort.h"
#include "mongo/db/pipeline/document_source_test_optimizations.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/pipeline/pipeline_test_util.h"
#include "mongo/db/pipeline/process_interface/common_process_interface.h"
#include "mongo/db/pipeline/process_interface/stub_mongo_process_interface.h"
#include "mongo/db/pipeline/semantic_analysis.h"
#include "mongo/db/pipeline/sharded_agg_helpers.h"
#include "mongo/db/pipeline/stage_constraints.h"
#include "mongo/db/query/explain_options.h"
#include "mongo/db/query/query_test_service_context.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/s/shard_server_test_fixture.h"
#include "mongo/db/tenant_id.h"
#include "mongo/dbtests/dbtests.h"  // IWYU pragma: keep
#include "mongo/s/sharding_state.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/bson_test_util.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/framework.h"
#include "mongo/unittest/temp_dir.h"
#include "mongo/util/assert_util.h"

namespace mongo {
namespace {

const NamespaceString kTestNss = NamespaceString::createNamespaceString_forTest("a.collection");
const NamespaceString kAdminCollectionlessNss =
    NamespaceString::createNamespaceString_forTest("admin.$cmd.aggregate");
const auto kExplain = SerializationOptions{
    .verbosity = boost::make_optional(ExplainOptions::Verbosity::kQueryPlanner)};

constexpr size_t getChangeStreamStageSize() {
    return 6;
}

void setMockReplicationCoordinatorOnOpCtx(OperationContext* opCtx) {
    repl::ReplicationCoordinator::set(
        opCtx->getServiceContext(),
        std::make_unique<repl::ReplicationCoordinatorMock>(opCtx->getServiceContext()));
}

namespace Optimizations {
namespace Local {

BSONObj pipelineFromJsonArray(const std::string& jsonArray) {
    return fromjson("{pipeline: " + jsonArray + "}");
}

class StubExplainInterface : public StubMongoProcessInterface {
    BSONObj preparePipelineAndExplain(Pipeline* ownedPipeline,
                                      ExplainOptions::Verbosity verbosity) override {
        std::unique_ptr<Pipeline, PipelineDeleter> pipeline(
            ownedPipeline, PipelineDeleter(ownedPipeline->getContext()->opCtx));
        BSONArrayBuilder bab;
        auto opts = SerializationOptions{.verbosity = boost::make_optional(verbosity)};
        auto pipelineVec = pipeline->writeExplainOps(opts);
        for (auto&& stage : pipelineVec) {
            bab << stage;
        }
        return BSON("pipeline" << bab.arr());
    }
    std::unique_ptr<Pipeline, PipelineDeleter> attachCursorSourceToPipelineForLocalRead(
        Pipeline* ownedPipeline, boost::optional<const AggregateCommandRequest&> aggRequest) {
        std::unique_ptr<Pipeline, PipelineDeleter> pipeline(
            ownedPipeline, PipelineDeleter(ownedPipeline->getContext()->opCtx));
        return pipeline;
    }
};
void assertPipelineOptimizesAndSerializesTo(std::string inputPipeJson,
                                            std::string outputPipeJson,
                                            std::string serializedPipeJson,
                                            NamespaceString aggNss = kTestNss) {
    QueryTestServiceContext testServiceContext;
    auto opCtx = testServiceContext.makeOperationContext();

    const BSONObj inputBson = pipelineFromJsonArray(inputPipeJson);
    const BSONObj outputPipeExpected = pipelineFromJsonArray(outputPipeJson);
    const BSONObj serializePipeExpected = pipelineFromJsonArray(serializedPipeJson);

    ASSERT_EQUALS(inputBson["pipeline"].type(), BSONType::Array);
    std::vector<BSONObj> rawPipeline;
    for (auto&& stageElem : inputBson["pipeline"].Array()) {
        ASSERT_EQUALS(stageElem.type(), BSONType::Object);
        rawPipeline.push_back(stageElem.embeddedObject());
    }
    AggregateCommandRequest request(aggNss, rawPipeline);
    boost::intrusive_ptr<ExpressionContextForTest> ctx =
        new ExpressionContextForTest(opCtx.get(), request);
    ctx->mongoProcessInterface = std::make_shared<StubExplainInterface>();
    unittest::TempDir tempDir("PipelineTest");
    ctx->tempDir = tempDir.path();

    // For $graphLookup and $lookup, we have to populate the resolvedNamespaces so that the
    // operations will be able to have a resolved view definition.
    NamespaceString lookupCollNs =
        NamespaceString::createNamespaceString_forTest("a", "lookupColl");
    NamespaceString unionCollNs = NamespaceString::createNamespaceString_forTest("b", "unionColl");
    ctx->setResolvedNamespace(lookupCollNs, {lookupCollNs, std::vector<BSONObj>{}});
    ctx->setResolvedNamespace(unionCollNs, {unionCollNs, std::vector<BSONObj>{}});

    auto outputPipe = Pipeline::parse(request.getPipeline(), ctx);
    outputPipe->optimizePipeline();

    // We normalize match expressions in the pipeline here to ensure the stability of the predicate
    // order after optimizations.
    outputPipe = normalizeMatchStageInPipeline(std::move(outputPipe));

    ASSERT_VALUE_EQ(
        Value(outputPipe->writeExplainOps(SerializationOptions{
            .verbosity = boost::make_optional(ExplainOptions::Verbosity::kQueryPlanner)})),
        Value(outputPipeExpected["pipeline"]));
    ASSERT_VALUE_EQ(Value(outputPipe->serialize()), Value(serializePipeExpected["pipeline"]));
}

void assertPipelineOptimizesTo(std::string inputPipeJson, std::string outputPipeJson) {
    assertPipelineOptimizesAndSerializesTo(inputPipeJson, outputPipeJson, outputPipeJson);
}

TEST(PipelineOptimizationTest, MoveSkipBeforeProject) {
    assertPipelineOptimizesTo("[{$project: {a : 1}}, {$skip : 5}]",
                              "[{$skip : 5}, {$project: {_id: true, a : true}}]");
}

TEST(PipelineOptimizationTest, LimitDoesNotMoveBeforeProject) {
    assertPipelineOptimizesTo("[{$project: {a : 1}}, {$limit : 5}]",
                              "[{$project: {_id: true, a : true}}, {$limit : 5}]");
}

TEST(PipelineOptimizationTest, SampleLegallyPushedBefore) {
    std::string inputPipe =
        "[{$replaceRoot: { newRoot: \"$a\" }}, "
        "{$project: { b: 1 }}, "
        "{$addFields: { c: 1 }}, "
        "{$sample: { size: 4 }}]";

    std::string outputPipe =
        "[{$sample: {size: 4}}, "
        "{$replaceRoot: {newRoot: \"$a\"}}, "
        "{$project: {_id: true, b : true}}, "
        "{$addFields: {c : {$const : 1}}}]";

    assertPipelineOptimizesTo(inputPipe, outputPipe);
}

TEST(PipelineOptimizationTest, SampleNotIllegallyPushedBefore) {
    std::string inputPipe =
        "[{$project: { a : 1 }}, "
        "{$match: { a: 1 }}, "
        "{$sample: { size: 4 }}]";

    std::string outputPipe =
        "[{$match: {a: {$eq: 1}}}, "
        "{$sample : {size: 4}}, "
        "{$project: {_id: true, a : true}}]";

    std::string serializedPipe =
        "[{$match: {a: 1}}, "
        "{$sample : {size: 4}}, "
        "{$project: {_id: true, a : true}}]";

    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe, serializedPipe);
}

TEST(PipelineOptimizationTest, MoveMatchBeforeAddFieldsIfInvolvedFieldsNotRelated) {
    std::string inputPipe = "[{$addFields : {a : 1}}, {$match : {b : 1}}]";

    std::string outputPipe = "[{$match : {b : {$eq : 1}}}, {$addFields : {a : {$const : 1}}}]";

    std::string serializedPipe = "[{$match: {b : 1}}, {$addFields: {a : {$const : 1}}}]";

    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe, serializedPipe);
}

TEST(PipelineOptimizationTest, MoveMatchWithExprBeforeAddFieldsIfInvolvedFieldsNotRelated) {
    std::string inputPipe = "[{$addFields : {a : 1}}, {$match : {$expr: {$eq: ['$b', 1]}}}]";

    std::string outputPipe =
        "[{$match: {$and: [{$expr: {$eq: ['$b', {$const: 1}]}},"
        "                  {b: {$_internalExprEq: 1}}]}},"
        " {$addFields : {a : {$const : 1}}}]";

    std::string serializedPipe =
        "[{$match : {$expr: {$eq: ['$b', 1]}}},"
        " {$addFields : {a : {$const : 1}}}]";

    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe, serializedPipe);
}

TEST(PipelineOptimizationTest, MatchDoesNotMoveBeforeAddFieldsIfInvolvedFieldsAreRelated) {
    std::string inputPipe = "[{$addFields : {a : 1}}, {$match : {a : 1}}]";

    std::string outputPipe = "[{$addFields : {a : {$const : 1}}}, {$match : {a : {$eq : 1}}}]";

    std::string serializedPipe = "[{$addFields : {a : {$const : 1}}}, {$match: {a : 1}}]";

    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe, serializedPipe);
}

TEST(PipelineOptimizationTest, MatchWithExprDoesNotMoveBeforeAddFieldsIfInvolvedFieldsAreRelated) {
    std::string inputPipe = "[{$addFields : {a : 1}}, {$match : {$expr: {$eq: ['$a', 1]}}}]";

    std::string outputPipe =
        "[{$addFields : {a : {$const : 1}}},"
        " {$match: {$and: [{$expr: {$eq: ['$a', {$const: 1}]}},"
        "                  {a: {$_internalExprEq: 1}}]}}]";

    std::string serializedPipe =
        "[{$addFields : {a : {$const : 1}}},"
        " {$match : {$expr: {$eq: ['$a', 1]}}}]";

    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe, serializedPipe);
}

TEST(PipelineOptimizationTest, MatchOnTopLevelFieldDoesNotMoveBeforeAddFieldsOfNestedPath) {
    std::string inputPipe = "[{$addFields : {'a.b' : 1}}, {$match : {a : 1}}]";

    std::string outputPipe =
        "[{$addFields : {a : {b : {$const : 1}}}}, {$match : {a : {$eq : 1}}}]";

    std::string serializedPipe = "[{$addFields: {a: {b: {$const: 1}}}}, {$match: {a: 1}}]";

    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe, serializedPipe);
}

TEST(PipelineOptimizationTest, MatchWithExprOnTopLevelFieldDoesNotMoveBeforeAddFieldsOfNestedPath) {
    std::string inputPipe = "[{$addFields : {'a.b' : 1}}, {$match : {$expr: {$eq: ['$a', 1]}}}]";

    std::string outputPipe =
        "[{$addFields : {a : {b : {$const : 1}}}},"
        " {$match: {$and: [{$expr: {$eq: ['$a', {$const: 1}]}},"
        "                  {a: {$_internalExprEq: 1}}]}}]";

    std::string serializedPipe =
        "[{$addFields: {a: {b: {$const: 1}}}},"
        " {$match : {$expr: {$eq: ['$a', 1]}}}]";

    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe, serializedPipe);
}

TEST(PipelineOptimizationTest, MatchOnNestedFieldDoesNotMoveBeforeAddFieldsOfPrefixOfPath) {
    std::string inputPipe = "[{$addFields : {a : 1}}, {$match : {'a.b' : 1}}]";

    std::string outputPipe = "[{$addFields : {a : {$const : 1}}}, {$match : {'a.b' : {$eq : 1}}}]";

    std::string serializedPipe = "[{$addFields : {a : {$const : 1}}}, {$match : {'a.b' : 1}}]";

    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe, serializedPipe);
}

TEST(PipelineOptimizationTest, MatchWithExprOnNestedFieldDoesNotMoveBeforeAddFieldsOfPrefixOfPath) {
    std::string inputPipe = "[{$addFields : {a : 1}}, {$match : {$expr: {$eq: ['$a.b', 1]}}}]";

    std::string outputPipe =
        "[{$addFields : {a : {$const : 1}}},"
        " {$match: {$and: [{$expr: {$eq: ['$a.b', {$const: 1}]}},"
        "                  {'a.b': {$_internalExprEq: 1}}]}}]";

    std::string serializedPipe =
        "[{$addFields : {a : {$const : 1}}},"
        " {$match : {$expr: {$eq: ['$a.b', 1]}}}]";

    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe, serializedPipe);
}

TEST(PipelineOptimizationTest, MoveMatchOnNestedFieldBeforeAddFieldsOfDifferentNestedField) {
    std::string inputPipe = "[{$addFields : {'a.b' : 1}}, {$match : {'a.c' : 1}}]";

    std::string outputPipe =
        "[{$match : {'a.c' : {$eq : 1}}}, {$addFields : {a : {b : {$const : 1}}}}]";

    std::string serializedPipe = "[{$match : {'a.c' : 1}}, {$addFields : {a : {b: {$const : 1}}}}]";

    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe, serializedPipe);
}

TEST(PipelineOptimizationTest,
     MoveMatchWithExprOnNestedFieldBeforeAddFieldsOfDifferentNestedField) {
    std::string inputPipe = "[{$addFields : {'a.b' : 1}}, {$match : {$expr: {$eq: ['$a.c', 1]}}}]";

    std::string outputPipe =
        "[{$match: {$and: [{$expr: {$eq: ['$a.c', {$const: 1}]}},"
        "                  {'a.c': {$_internalExprEq: 1}}]}},"
        " {$addFields : {a : {b : {$const : 1}}}}]";

    std::string serializedPipe =
        "[{$match : {$expr: {$eq: ['$a.c', 1]}}},"
        " {$addFields : {a : {b: {$const : 1}}}}]";

    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe, serializedPipe);
}

TEST(PipelineOptimizationTest, MoveMatchBeforeAddFieldsWhenMatchedFieldIsPrefixOfAddedFieldName) {
    std::string inputPipe = "[{$addFields : {abcd : 1}}, {$match : {abc : 1}}]";

    std::string outputPipe = "[{$match : {abc : {$eq : 1}}}, {$addFields : {abcd: {$const: 1}}}]";

    std::string serializedPipe = "[{$match : {abc : 1}}, {$addFields : {abcd : {$const : 1}}}]";

    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe, serializedPipe);
}

TEST(PipelineOptimizationTest,
     MoveMatchWithExprBeforeAddFieldsWhenMatchedFieldIsPrefixOfAddedFieldName) {
    std::string inputPipe = "[{$addFields : {abcd : 1}}, {$match : {$expr: {$eq: ['$abc', 1]}}}]";

    std::string outputPipe =
        "[{$match: {$and: [{$expr: {$eq: ['$abc', {$const: 1}]}},"
        "                  {abc: {$_internalExprEq: 1}}]}},"
        " {$addFields : {abcd: {$const: 1}}}]";

    std::string serializedPipe =
        "[{$match : {$expr: {$eq: ['$abc', 1]}}},"
        " {$addFields : {abcd : {$const : 1}}}]";

    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe, serializedPipe);
}

TEST(PipelineOptimizationTest, LimitDoesNotSwapBeforeSkipWithoutSort) {
    std::string inputPipe =
        "[{$skip : 3}"
        ",{$skip : 5}"
        ",{$limit: 5}"
        "]";
    std::string outputPipe =
        "[{$skip : 8}"
        ",{$limit: 5}"
        "]";
    assertPipelineOptimizesTo(inputPipe, outputPipe);
}

TEST(PipelineOptimizationTest, SortSwapsBeforeUnwind) {
    std::string inputPipe =
        "[{$unwind : {path: '$a'}}"
        ",{$sort : {b: 1}}"
        "]";
    std::string outputPipe =
        "[{$sort : {sortKey: {b: 1}}}"
        ",{$unwind : {path: '$a'}}"
        "]";
    std::string serializedPipe =
        "[{$sort : {b: 1}}"
        ",{$unwind : {path: '$a'}}"
        "]";
    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe, serializedPipe);
}

TEST(PipelineOptimizationTest, SortSwapsBeforeUnwindMultipleSorts) {
    std::string inputPipe =
        "[{$unwind : {path: '$a'}}"
        ",{$sort : {b: 1}}"
        ",{$sort : {c: 1}}"
        "]";
    std::string outputPipe =
        "[{$sort : {sortKey: {c: 1}}}"
        ",{$unwind : {path: '$a'}}"
        "]";
    std::string serializedPipe =
        "[{$sort : {c: 1}}"
        ",{$unwind : {path: '$a'}}"
        "]";
    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe, serializedPipe);
}

TEST(PipelineOptimizationTest, SortSwapsBeforeUnwindDifferentDotPaths) {
    std::string inputPipe =
        "[{$unwind : {path: '$a.b'}}"
        ",{$sort : {'a.c': 1}}"
        "]";
    std::string outputPipe =
        "[{$sort : {sortKey: {'a.c': 1}}}"
        ",{$unwind : {path: '$a.b'}}"
        "]";
    std::string serializedPipe =
        "[{$sort : {'a.c': 1}}"
        ",{$unwind : {path: '$a.b'}}"
        "]";
    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe, serializedPipe);
}

TEST(PipelineOptimizationTest, SortSwapsBeforeUnwindMultipleSortPaths) {
    std::string inputPipe =
        "[{$unwind : {path: '$a'}}"
        ",{$sort : {b: 1, c: 1}}"
        "]";
    std::string outputPipe =
        "[{$sort : {sortKey: {b: 1, c: 1}}}"
        ",{$unwind : {path: '$a'}}"
        "]";
    std::string serializedPipe =
        "[{$sort : {b: 1, c: 1}}"
        ",{$unwind : {path: '$a'}}"
        "]";
    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe, serializedPipe);
}

TEST(PipelineOptimizationTest, SortDoesNotSwapBeforeUnwindMultipleSortPaths) {
    std::string inputPipe =
        "[{$unwind : {path: '$a'}}"
        ",{$sort : {b: 1, a: 1}}"
        "]";
    std::string outputPipe =
        "[{$unwind : {path: '$a'}}"
        ",{$sort : {sortKey: {b: 1, a: 1}}}"
        "]";
    std::string serializedPipe =
        "[{$unwind : {path: '$a'}}"
        ",{$sort : {b: 1, a: 1}}"
        "]";
    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe, serializedPipe);
}

TEST(PipelineOptimizationTest, SortDoesNotSwapBeforeUnwindBecauseSortPathPrefixOfUnwindPath) {
    std::string inputPipe =
        "[{$unwind : {path: '$b.a'}}"
        ",{$sort : {b: 1}}"
        "]";
    std::string outputPipe =
        "[{$unwind : {path: '$b.a'}}"
        ",{$sort : {sortKey: {b: 1}}}"
        "]";
    std::string serializedPipe =
        "[{$unwind : {path: '$b.a'}}"
        ",{$sort : {b: 1}}"
        "]";
    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe, serializedPipe);
}

TEST(PipelineOptimizationTest, SortDoesNotSwapBeforeUnwindBecauseUnwindPathPrefixOfSortPath) {
    std::string inputPipe =
        "[{$unwind : {path: '$b'}}"
        ",{$sort : {'b.a': 1}}"
        "]";
    std::string outputPipe =
        "[{$unwind : {path: '$b'}}"
        ",{$sort : {sortKey: {'b.a': 1}}}"
        "]";
    std::string serializedPipe =
        "[{$unwind : {path: '$b'}}"
        ",{$sort : {'b.a': 1}}"
        "]";
    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe, serializedPipe);
}

TEST(PipelineOptimizationTest, SortDoesNotSwapBeforeUnwindBecauseUnwindPathEqualToSortPath) {
    std::string inputPipe =
        "[{$unwind : {path: '$a.b'}}"
        ",{$sort : {'a.b': 1}}"
        "]";
    std::string outputPipe =
        "[{$unwind : {path: '$a.b'}}"
        ",{$sort : {sortKey: {'a.b': 1}}}"
        "]";
    std::string serializedPipe =
        "[{$unwind : {path: '$a.b'}}"
        ",{$sort : {'a.b': 1}}"
        "]";
    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe, serializedPipe);
}

TEST(PipelineOptimizationTest, LookupShouldCoalesceWithUnwindOnAsSortDoesNotInterfere) {
    std::string inputPipe =
        "[{$lookup: {from : 'lookupColl', as : 'same', localField: 'left', foreignField: "
        "'right'}}"
        ",{$unwind: {path: '$same'}}"
        ",{$sort : {'a.b': 1}}"
        "]";
    std::string outputPipe =
        "[{$lookup: {from : 'lookupColl', as : 'same', localField: 'left', foreignField: "
        "'right', unwinding: {preserveNullAndEmptyArrays: false}}}"
        ",{$sort : {sortKey: {'a.b': 1}}}]";
    std::string serializedPipe =
        "[{$lookup: {from : 'lookupColl', as : 'same', localField: 'left', foreignField: "
        "'right'}}"
        ",{$unwind: {path: '$same'}}"
        ",{$sort : {'a.b': 1}}"
        "]";
    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe, serializedPipe);
}

TEST(PipelineOptimizationTest, SortSwapsBeforeUnwindMetaWithFieldPath) {
    std::string inputPipe =
        "[{ $match: { $text: { $search: \"operating\" } }}"
        ",{$unwind : {path: '$a'}}"
        ",{$sort : {score: {$meta: \"textScore\"}, c: 1}}"
        "]";
    std::string outputPipe =
        "[{$match: {$text: {$search: \"operating\", $language: \"\", $caseSensitive: false, "
        "$diacriticSensitive: false}}}"
        ",{$sort: {sortKey: {$computed0: {$meta: \"textScore\"}, c: 1}}}"
        ",{$unwind : {path: '$a'}}"
        "]";
    std::string serializedPipe =
        "[{ $match: { $text: { $search: \"operating\" } }}"
        ",{$sort: {$computed0: {$meta: \"textScore\"}, c: 1}}"
        ",{$unwind : {path: '$a'}}"
        "]";
    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe, serializedPipe);
}

TEST(PipelineOptimizationTest, SortSwapsBeforeUnwindMetaWithoutFieldPath) {
    std::string inputPipe =
        "[{ $match: { $text: { $search: \"operating\" } }}"
        ",{$unwind : {path: '$a'}}"
        ",{$sort : {score: {$meta: \"textScore\"}}}"
        "]";
    std::string outputPipe =
        "[{$match: {$text: {$search: \"operating\", $language: \"\", $caseSensitive: false, "
        "$diacriticSensitive: false}}}"
        ",{$sort: {sortKey: {$computed0: {$meta: \"textScore\"}}}}"
        ",{$unwind : {path: '$a'}}"
        "]";
    std::string serializedPipe =
        "[{ $match: { $text: { $search: \"operating\" } }}"
        ",{$sort: {$computed0: {$meta: \"textScore\"}}}"
        ",{$unwind : {path: '$a'}}"
        "]";
    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe, serializedPipe);
}

TEST(PipelineOptimizationTest, LimitDuplicatesBeforeUnwindWithPreserveNull) {
    std::string inputPipe =
        "[{$unwind : {path: '$a', preserveNullAndEmptyArrays: true}}"
        ",{$limit : 100}"
        "]";
    std::string outputPipe =
        "[{$limit : 100}"
        ",{$unwind : {path: '$a', preserveNullAndEmptyArrays: true}}"
        ",{$limit : 100}"
        "]";
    std::string serializedPipe =
        "[{$limit : 100}"
        ",{$unwind : {path: '$a', preserveNullAndEmptyArrays: true}}"
        ",{$limit : 100}"
        "]";
    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe, serializedPipe);
}

TEST(PipelineOptimizationTest, LimitDoesNotDuplicatesBeforeUnwindWithoutPreserveNull) {
    std::string inputPipe =
        "[{$unwind : {path: '$a'}}"
        ",{$limit : 100}"
        "]";
    std::string outputPipe =
        "[{$unwind : {path: '$a'}}"
        ",{$limit : 100}"
        "]";
    std::string serializedPipe =
        "[{$unwind : {path: '$a'}}"
        ",{$limit : 100}"
        "]";
    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe, serializedPipe);
}

TEST(PipelineOptimizationTest, LimitDuplicatesBeforeSortUnwindAndIsMergedWithSort) {
    std::string inputPipe =
        "[{$sort: {b: 1}}"
        ",{$unwind : {path: '$a', preserveNullAndEmptyArrays: true}}"
        ",{$limit : 100}"
        "]";
    std::string outputPipe =
        "[{$sort: {sortKey: {b: 1}, limit: 100}}"
        ",{$unwind : {path: '$a', preserveNullAndEmptyArrays: true}}"
        ",{$limit : 100}"
        "]";
    std::string serializedPipe =
        "[{$sort: {b: 1}}"
        ",{$limit: 100}"
        ",{$unwind: {path: \"$a\", preserveNullAndEmptyArrays: true}}"
        ",{$limit: 100}"
        "]";
    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe, serializedPipe);
}

TEST(PipelineOptimizationTest, SortAndLimitSwapsBeforeUnwindAndMerges) {
    std::string inputPipe =
        "[{$unwind : {path: '$a', preserveNullAndEmptyArrays: true}}"
        ",{$sort : {b: 1}}"
        ",{$limit : 5}"
        "]";
    std::string outputPipe =
        "[{$sort : {sortKey: {b: 1}, limit: 5}}"
        ",{$unwind : {path: '$a', preserveNullAndEmptyArrays: true}}"
        ",{$limit : 5}"
        "]";
    std::string serializedPipe =
        "[{$sort: {b: 1}}"
        ",{$limit: 5}"
        ",{$unwind: {path: \"$a\", preserveNullAndEmptyArrays: true}}"
        ",{$limit: 5}"
        "]";
    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe, serializedPipe);
}

TEST(PipelineOptimizationTest, UnwindLimitLimitPushesSmallestLimitBack) {
    std::string inputPipe =
        "[{$unwind : {path: '$a', preserveNullAndEmptyArrays: true}}"
        ",{$limit : 500}"
        ",{$limit : 50}"
        ",{$limit : 5}"
        "]";
    std::string outputPipe =
        "[{$limit : 5}"
        ",{$unwind : {path: '$a', preserveNullAndEmptyArrays: true}}"
        ",{$limit : 5}"
        "]";
    std::string serializedPipe =
        "[{$limit : 5}"
        ",{$unwind : {path: '$a', preserveNullAndEmptyArrays: true}}"
        ",{$limit : 5}"
        "]";
    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe, serializedPipe);
}

TEST(PipelineOptimizationTest, SortMatchProjSkipLimBecomesMatchTopKSortSkipProj) {
    std::string inputPipe =
        "[{$sort: {a: 1}}"
        ",{$match: {a: 1}}"
        ",{$project : {a: 1}}"
        ",{$skip : 3}"
        ",{$limit: 5}"
        "]";

    std::string outputPipe =
        "[{$match: {a: {$eq: 1}}}"
        ",{$sort: {sortKey: {a: 1}, limit: 8}}"
        ",{$skip: 3}"
        ",{$project: {_id: true, a: true}}"
        "]";

    std::string serializedPipe =
        "[{$match: {a: 1}}"
        ",{$sort: {a: 1}}"
        ",{$limit: 8}"
        ",{$skip : 3}"
        ",{$project : {_id: true, a: true}}"
        "]";

    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe, serializedPipe);
}

TEST(PipelineOptimizationTest, SortMatchWithExprProjSkipLimBecomesMatchTopKSortSkipProj) {
    std::string inputPipe =
        "[{$sort: {a: 1}}"
        ",{$match: {$expr: {$eq: ['$a', 1]}}}"
        ",{$project : {a: 1}}"
        ",{$skip : 3}"
        ",{$limit: 5}"
        "]";

    std::string outputPipe =
        "[{$match: {$and: [{$expr: {$eq: ['$a', {$const: 1}]}}, {a: {$_internalExprEq: 1}}]}}"
        ",{$sort: {sortKey: {a: 1}, limit: 8}}"
        ",{$skip: 3}"
        ",{$project: {_id: true, a: true}}"
        "]";

    std::string serializedPipe =
        "[{$match: {$expr: {$eq: ['$a', 1]}}}"
        ",{$sort: {a: 1}}"
        ",{$limit: 8}"
        ",{$skip : 3}"
        ",{$project : {_id: true, a: true}}"
        "]";

    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe, serializedPipe);
}

TEST(PipelineOptimizationTest, IdenticalSortSortBecomesSort) {
    std::string inputPipe =
        "[{$sort: {a: 1}}"
        ",{$sort: {a: 1}}"
        "]";

    std::string outputPipe =
        "[{$sort: {sortKey: {a: 1}}}"
        "]";

    std::string serializedPipe =
        "[{$sort: {a: 1}}"
        "]";

    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe, serializedPipe);
}

TEST(PipelineOptimizationTest, IdenticalSortSortSortBecomesSort) {
    std::string inputPipe =
        "[{$sort: {a: 1}}"
        ",{$sort: {a: 1}}"
        ",{$sort: {a: 1}}"
        "]";

    std::string outputPipe =
        "[{$sort: {sortKey: {a: 1}}}"
        "]";

    std::string serializedPipe =
        "[{$sort: {a: 1}}"
        "]";

    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe, serializedPipe);
}

TEST(PipelineOptimizationTest, NonIdenticalSortsOnlySortOnFinalKey) {
    std::string inputPipe =
        "[{$sort: {a: -1}}"
        ",{$sort: {a: 1}}"
        ",{$sort: {a: -1}}"
        "]";

    std::string outputPipe =
        "[{$sort: {sortKey: {a: -1}}}"
        "]";

    std::string serializedPipe =
        "[{$sort: {a: -1}}"
        "]";

    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe, serializedPipe);
}

TEST(PipelineOptimizationTest, SortSortLimitBecomesFinalKeyTopKSort) {
    std::string inputPipe =
        "[{$sort: {a: -1}}"
        ",{$sort: {a: 1}}"
        ",{$limit: 5}"
        "]";

    std::string outputPipe =
        "[{$sort: {sortKey: {a: 1}, limit: 5}}"
        "]";

    std::string serializedPipe =
        "[{$sort: {a: 1}}"
        ",{$limit: 5}"
        "]";

    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe, serializedPipe);
}

TEST(PipelineOptimizationTest, SortSortSkipLimitBecomesTopKSortSkip) {
    std::string inputPipe =
        "[{$sort: {b: 1}}"
        ",{$sort: {a: 1}}"
        ",{$skip : 3}"
        ",{$limit: 5}"
        "]";

    std::string outputPipe =
        "[{$sort: {sortKey: {a: 1}, limit: 8}}"
        ",{$skip: 3}"
        "]";

    std::string serializedPipe =
        "[{$sort: {a: 1}}"
        ",{$limit: 8}"
        ",{$skip : 3}"
        "]";

    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe, serializedPipe);
}

TEST(PipelineOptimizationTest, SortLimitSortLimitBecomesTopKSort) {
    std::string inputPipe =
        "[{$sort: {a: 1}}"
        ",{$limit: 12}"
        ",{$sort: {a: 1}}"
        ",{$limit: 20}"
        "]";

    std::string outputPipe =
        "[{$sort: {sortKey: {a: 1}, limit: 12}}"
        "]";

    std::string serializedPipe =
        "[{$sort: {a: 1}}"
        ",{$limit: 12}"
        "]";

    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe, serializedPipe);
}

TEST(PipelineOptimizationTest, SortLimitSortRetainsLimit) {
    std::string inputPipe =
        "[{$sort: {a: 1}}"
        ",{$limit: 12}"
        ",{$sort: {a: 1}}"
        "]";

    std::string outputPipe =
        "[{$sort: {sortKey: {a: 1}, limit: 12}}"
        "]";

    std::string serializedPipe =
        "[{$sort: {a: 1}}"
        ",{$limit: 12}"
        "]";

    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe, serializedPipe);
}

TEST(PipelineOptimizationTest, SortLimitSortWithDifferentSortPatterns) {
    std::string inputPipe =
        "[{$sort: {a: 1}}"
        ",{$limit: 12}"
        ",{$sort: {b: 1}}"
        "]";

    std::string outputPipe =
        "[{$sort: {sortKey: {a: 1}, limit: 12}}"
        ",{$sort: {sortKey: {b: 1}}}"
        "]";

    std::string serializedPipe = inputPipe;

    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe, serializedPipe);
}
TEST(PipelineOptimizationTest, SortSortLimitRetainsLimit) {
    std::string inputPipe =
        "[{$sort: {a: 1}}"
        ",{$sort: {a: 1}}"
        ",{$limit: 20}"
        "]";

    std::string outputPipe =
        "[{$sort: {sortKey: {a: 1}, limit: 20}}"
        "]";

    std::string serializedPipe =
        "[{$sort: {a: 1}}"
        ",{$limit: 20}"
        "]";

    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe, serializedPipe);
}

TEST(PipelineOptimizationTest, SortSortSortMatchProjSkipLimBecomesMatchTopKSortSkipProj) {
    std::string inputPipe =
        "[{$sort: {a: 1}}"
        ",{$sort: {a: 1}}"
        ",{$sort: {a: 1}}"
        ",{$match: {a: 1}}"
        ",{$project : {a: 1}}"
        ",{$skip : 3}"
        ",{$limit: 5}"
        "]";

    std::string outputPipe =
        "[{$match: {a: {$eq: 1}}}"
        ",{$sort: {sortKey: {a: 1}, limit: 8}}"
        ",{$skip: 3}"
        ",{$project: {_id: true, a: true}}"
        "]";

    std::string serializedPipe =
        "[{$match: {a: 1}}"
        ",{$sort: {a: 1}}"
        ",{$limit: 8}"
        ",{$skip : 3}"
        ",{$project : {_id: true, a: true}}"
        "]";

    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe, serializedPipe);
}

TEST(PipelineOptimizationTest, SortSortSortMatchOnExprProjSkipLimBecomesMatchTopKSortSkipProj) {
    std::string inputPipe =
        "[{$sort: {a: 1}}"
        ",{$sort: {a: 1}}"
        ",{$sort: {a: 1}}"
        ",{$match: {$expr: {$eq: ['$a', 1]}}}"
        ",{$project : {a: 1}}"
        ",{$skip : 3}"
        ",{$limit: 5}"
        "]";

    std::string outputPipe =
        "[{$match: {$and: [{$expr: {$eq: ['$a', {$const: 1}]}}, {a: {$_internalExprEq: 1}}]}}"
        ",{$sort: {sortKey: {a: 1}, limit: 8}}"
        ",{$skip: 3}"
        ",{$project: {_id: true, a: true}}"
        "]";

    std::string serializedPipe =
        "[{$match: {$expr: {$eq: ['$a', 1]}}}"
        ",{$sort: {a: 1}}"
        ",{$limit: 8}"
        ",{$skip : 3}"
        ",{$project : {_id: true, a: true}}"
        "]";

    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe, serializedPipe);
}

TEST(PipelineOptimizationTest, NonIdenticalSortsBecomeFinalKeyTopKSort) {
    std::string inputPipe =
        "[{$sort: {a: -1}}"
        ",{$sort: {b: -1}}"
        ",{$sort: {b: 1}}"
        ",{$sort: {a: 1}}"
        ",{$limit: 7}"
        ",{$project : {a: 1}}"
        ",{$limit: 5}"
        "]";

    std::string outputPipe =
        "[{$sort: {sortKey: {a: 1}, limit: 5}}"
        ",{$project: {_id: true, a: true}}"
        "]";

    std::string serializedPipe =
        "[{$sort: {a: 1}}"
        ",{$limit: 5}"
        ",{$project : {_id: true, a: true}}"
        "]";

    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe, serializedPipe);
}

TEST(PipelineOptimizationTest, SubsequentSortsMergeAndBecomeTopKSortWithFinalKeyAndLowestLimit) {
    std::string inputPipe =
        "[{$sort: {a: 1}}"
        ",{$sort: {a: -1}}"
        ",{$limit: 8}"
        ",{$limit: 7}"
        ",{$project : {a: 1}}"
        ",{$unwind: {path: '$a'}}"
        "]";

    std::string outputPipe =
        "[{$sort: {sortKey: {a: -1}, limit: 7}}"
        ",{$project: {_id: true, a: true}}"
        ",{$unwind: {path: '$a'}}"
        "]";

    std::string serializedPipe =
        "[{$sort: {a: -1}}"
        ",{$limit: 7}"
        ",{$project : {_id: true, a: true}}"
        ",{$unwind: {path: '$a'}}"
        "]";

    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe, serializedPipe);
}

TEST(PipelineOptimizationTest, RemoveSkipZero) {
    assertPipelineOptimizesTo("[{$skip: 0}]", "[]");
}

TEST(PipelineOptimizationTest, DoNotRemoveSkipOne) {
    assertPipelineOptimizesTo("[{$skip: 1}]", "[{$skip: 1}]");
}

TEST(PipelineOptimizationTest, RemoveEmptyMatch) {
    assertPipelineOptimizesTo("[{$match: {}}]", "[]");
}

TEST(PipelineOptimizationTest, RemoveMultipleEmptyMatches) {
    std::string inputPipe = "[{$match: {}}, {$match: {}}]";

    std::string outputPipe = "[{$match: {}}]";

    std::string serializedPipe = "[{$match: {$and: [{}, {}]}}]";

    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe, serializedPipe);
}

TEST(PipelineOptimizationTest, RemoveEmptyMatchesAndKeepNonEmptyMatches) {
    std::string inputPipe = "[{$match: {}}, {$match: {}}, {$match: {a: 1}}]";
    std::string outputPipe = "[{$match: {a: {$eq: 1}}}]";
    std::string serializedPipe = "[{$match: {$and: [{}, {}, {a: 1}]}}]";
    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe, serializedPipe);
}

TEST(PipelineOptimizationTest, RemoveEmptyMatchesAndKeepOtherStages) {
    assertPipelineOptimizesTo("[{$match: {}}, {$skip: 1}, {$match: {}}]", "[{$skip: 1}]");
}

TEST(PipelineOptimizationTest, KeepEmptyMatchWithComment) {
    std::string inputPipe = "[{$match: {$comment: 'foo'}}]";
    std::string outputPipe = "[{$match: {}}]";
    std::string serializedPipe = "[{$match: {$comment: 'foo'}}]";
    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe, serializedPipe);
}

TEST(PipelineOptimizationTest, DoNotRemoveNonEmptyMatch) {
    std::string inputPipe = "[{$match: {_id: 1}}]";

    std::string outputPipe = "[{$match: {_id: {$eq : 1}}}]";

    std::string serializedPipe = "[{$match: {_id: 1}}]";

    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe, serializedPipe);
}

TEST(PipelineOptimizationTest, RemoveMatchWithTrueConstExpr) {
    std::string inputPipe = "[{$match: {$expr: true}}]";
    std::string outputPipe = "[{$match: {}}]";
    std::string serializedPipe = "[{$match: {$expr: true}}]";
    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe, serializedPipe);
}

TEST(PipelineOptimizationTest, RemoveMultipleMatchesWithTrueConstExpr) {
    std::string inputPipe = "[{$match: {$expr: true}}, {$match: {$expr: true}}]";
    std::string outputPipe = "[{$match: {}}]";
    std::string serializedPipe = "[{$match: {$and: [{$expr: true}, {$expr: true}]}}]";
    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe, serializedPipe);
}

TEST(PipelineOptimizationTest, RemoveMatchWithTruthyConstExpr) {
    std::string inputPipe = "[{$match: {$expr: {$concat: ['a', 'b']}}}]";
    std::string outputPipe = "[{$match: {}}]";
    std::string serializedPipe = "[{$match: {$expr: {$concat: ['a', 'b']}}}]";
    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe, serializedPipe);
}

TEST(PipelineOptimizationTest, KeepMatchWithNonConstExpr) {
    assertPipelineOptimizesTo("[{$match: {$expr: {$concat: ['$a', '$b']}}}]",
                              "[{$match: {$expr: {$concat: ['$a', '$b']}}}]");
}

TEST(PipelineOptimizationTest, MoveMatchBeforeSort) {
    std::string inputPipe = "[{$sort: {b: 1}}, {$match: {a: 2}}]";
    std::string outputPipe = "[{$match: {a: {$eq : 2}}}, {$sort: {sortKey: {b: 1}}}]";
    std::string serializedPipe = "[{$match: {a: 2}}, {$sort: {b: 1}}]";
    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe, serializedPipe);
}

TEST(PipelineOptimizationTest, LookupMoveSortNotOnAsBefore) {
    std::string inputPipe =
        "[{$lookup: {from : 'lookupColl', as : 'new', localField: 'left', foreignField: "
        "'right'}}"
        ",{$sort: {left: 1}}"
        "]";
    std::string outputPipe =
        "[{$sort: {sortKey: {left: 1}}}"
        ",{$lookup: {from : 'lookupColl', as : 'new', localField: 'left', foreignField: "
        "'right'}}"
        "]";
    std::string serializedPipe =
        "[{$sort: {left: 1}}"
        ",{$lookup: {from : 'lookupColl', as : 'new', localField: 'left', foreignField: "
        "'right'}}"
        "]";
    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe, serializedPipe);
}

TEST(PipelineOptimizationTest, LookupMoveSortOnPrefixStringOfAsBefore) {
    std::string inputPipe =
        "[{$lookup: {from : 'lookupColl', as : 'leftNew', localField: 'left', foreignField: "
        "'right'}}"
        ",{$sort: {left: 1}}"
        "]";
    std::string outputPipe =
        "[{$sort: {sortKey: {left: 1}}}"
        ",{$lookup: {from : 'lookupColl', as : 'leftNew', localField: 'left', foreignField: "
        "'right'}}"
        "]";
    std::string serializedPipe =
        "[{$sort: {left: 1}}"
        ",{$lookup: {from : 'lookupColl', as : 'leftNew', localField: 'left', foreignField: "
        "'right'}}"
        "]";
    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe, serializedPipe);
}

TEST(PipelineOptimizationTest, LookupShouldNotMoveSortOnAsBefore) {
    std::string inputPipe =
        "[{$lookup: {from : 'lookupColl', as : 'same', localField: 'left', foreignField: "
        "'right'}}"
        ",{$sort: {same: 1, left: 1}}"
        "]";
    std::string outputPipe =
        "[{$lookup: {from : 'lookupColl', as : 'same', localField: 'left', foreignField: "
        "'right'}}"
        ",{$sort: {sortKey: {same: 1, left: 1}}}"
        "]";
    std::string serializedPipe =
        "[{$lookup: {from : 'lookupColl', as : 'same', localField: 'left', foreignField: "
        "'right'}}"
        ",{$sort: {same: 1, left: 1}}"
        "]";
    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe, serializedPipe);
}

TEST(PipelineOptimizationTest, LookupShouldNotMoveSortOnPathPrefixOfAsBefore) {
    std::string inputPipe =
        "[{$lookup: {from : 'lookupColl', as : 'same.new', localField: 'left', foreignField: "
        "'right'}}"
        ",{$sort: {same: 1}}"
        "]";
    std::string outputPipe =
        "[{$lookup: {from : 'lookupColl', as : 'same.new', localField: 'left', foreignField: "
        "'right'}}"
        ",{$sort: {sortKey: {same: 1}}}"
        "]";
    std::string serializedPipe =
        "[{$lookup: {from : 'lookupColl', as : 'same.new', localField: 'left', foreignField: "
        "'right'}}"
        ",{$sort: {same: 1}}"
        "]";
    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe, serializedPipe);
}

TEST(PipelineOptimizationTest, LookupUnwindShouldNotMoveSortBefore) {
    std::string inputPipe =
        "[{$lookup: {from : 'lookupColl', as : 'same', localField: 'left', foreignField: "
        "'right'}}"
        ",{$unwind: {path: '$same'}}"
        ",{$sort: {left: 1}}"
        "]";
    std::string outputPipe =
        "[{$lookup: {from : 'lookupColl', as : 'same', localField: 'left', foreignField: "
        "'right', unwinding: {preserveNullAndEmptyArrays: false}}}"
        ",{$sort: {sortKey: {left: 1}}}"
        "]";
    std::string serializedPipe =
        "[{$lookup: {from : 'lookupColl', as : 'same', localField: 'left', foreignField: "
        "'right'}}"
        ",{$unwind: {path: '$same'}}"
        ",{$sort: {left: 1}}"
        "]";
    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe, serializedPipe);
}

TEST(PipelineOptimizationTest, MoveMatchOnExprBeforeSort) {
    std::string inputPipe = "[{$sort: {b: 1}}, {$match: {$expr: {$eq: ['$a', 2]}}}]";
    std::string outputPipe =
        "[{$match: {$and: [{$expr: {$eq: ['$a', {$const: 2}]}},"
        "                  {a: {$_internalExprEq: 2}}]}},"
        " {$sort: {sortKey: {b: 1}}}]";
    std::string serializedPipe = "[{$match: {$expr: {$eq: ['$a', 2]}}}, {$sort: {b: 1}}]";
    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe, serializedPipe);
}

TEST(PipelineOptimizationTest, LookupShouldCoalesceWithUnwindOnAs) {
    std::string inputPipe =
        "[{$lookup: {from : 'lookupColl', as : 'same', localField: 'left', foreignField: "
        "'right'}}"
        ",{$unwind: {path: '$same'}}"
        "]";
    std::string outputPipe =
        "[{$lookup: {from : 'lookupColl', as : 'same', localField: 'left', foreignField: "
        "'right', unwinding: {preserveNullAndEmptyArrays: false}}}]";
    std::string serializedPipe =
        "[{$lookup: {from : 'lookupColl', as : 'same', localField: 'left', foreignField: "
        "'right'}}"
        ",{$unwind: {path: '$same'}}"
        "]";
    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe, serializedPipe);
}

TEST(PipelineOptimizationTest, LookupWithPipelineSyntaxShouldCoalesceWithUnwindOnAs) {
    std::string inputPipe =
        "[{$lookup: {from : 'lookupColl', as : 'same', let: {}, pipeline: []}}"
        ",{$unwind: {path: '$same'}}"
        "]";
    std::string outputPipe =
        "[{$lookup: {from : 'lookupColl', as : 'same', let: {}, pipeline: [], "
        "unwinding: {preserveNullAndEmptyArrays: false}}}]";
    std::string serializedPipe =
        "[{$lookup: {from : 'lookupColl', as : 'same', let: {}, pipeline: []}}"
        ",{$unwind: {path: '$same'}}"
        "]";
    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe, serializedPipe);
}

TEST(PipelineOptimizationTest, LookupShouldCoalesceWithUnwindOnAsWithPreserveEmpty) {
    std::string inputPipe =
        "[{$lookup: {from : 'lookupColl', as : 'same', localField: 'left', foreignField: "
        "'right'}}"
        ",{$unwind: {path: '$same', preserveNullAndEmptyArrays: true}}"
        "]";
    std::string outputPipe =
        "[{$lookup: {from : 'lookupColl', as : 'same', localField: 'left', foreignField: "
        "'right', unwinding: {preserveNullAndEmptyArrays: true}}}]";
    std::string serializedPipe =
        "[{$lookup: {from : 'lookupColl', as : 'same', localField: 'left', foreignField: "
        "'right'}}"
        ",{$unwind: {path: '$same', preserveNullAndEmptyArrays: true}}"
        "]";
    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe, serializedPipe);
}

TEST(PipelineOptimizationTest, LookupShouldCoalesceWithUnwindOnAsWithIncludeArrayIndex) {
    std::string inputPipe =
        "[{$lookup: {from : 'lookupColl', as : 'same', localField: 'left', foreignField: "
        "'right'}}"
        ",{$unwind: {path: '$same', includeArrayIndex: 'index'}}"
        "]";
    std::string outputPipe =
        "[{$lookup: {from : 'lookupColl', as : 'same', localField: 'left', foreignField: "
        "'right', unwinding: {preserveNullAndEmptyArrays: false, includeArrayIndex: "
        "'index'}}}]";
    std::string serializedPipe =
        "[{$lookup: {from : 'lookupColl', as : 'same', localField: 'left', foreignField: "
        "'right'}}"
        ",{$unwind: {path: '$same', includeArrayIndex: 'index'}}"
        "]";
    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe, serializedPipe);
}

TEST(PipelineOptimizationTest, LookupShouldNotCoalesceWithUnwindNotOnAs) {
    std::string inputPipe =
        "[{$lookup: {from : 'lookupColl', as : 'same', localField: 'left', foreignField: "
        "'right'}}"
        ",{$unwind: {path: '$from'}}"
        "]";
    std::string outputPipe =
        "[{$lookup: {from : 'lookupColl', as : 'same', localField: 'left', foreignField: "
        "'right'}}"
        ",{$unwind: {path: '$from'}}"
        "]";
    assertPipelineOptimizesTo(inputPipe, outputPipe);
}

TEST(PipelineOptimizationTest, LookupWithPipelineSyntaxShouldNotCoalesceWithUnwindNotOnAs) {
    std::string inputPipe =
        "[{$lookup: {from : 'lookupColl', as : 'same', pipeline: []}}"
        ",{$unwind: {path: '$from'}}"
        "]";
    std::string outputPipe =
        "[{$lookup: {from : 'lookupColl', as : 'same', let: {}, pipeline: []}}"
        ",{$unwind: {path: '$from'}}"
        "]";
    assertPipelineOptimizesTo(inputPipe, outputPipe);
}

TEST(PipelineOptimizationTest, LookupShouldSwapWithMatch) {
    std::string inputPipe =
        "[{$lookup: {from: 'lookupColl', as: 'asField', localField: 'y', foreignField: "
        "'z'}}, "
        " {$match: {'independent': 0}}]";
    std::string outputPipe =
        "[{$match: {independent: {$eq : 0}}}, "
        " {$lookup: {from: 'lookupColl', as: 'asField', localField: 'y', foreignField: "
        "'z'}}]";
    std::string serializedPipe =
        "[{$match: {independent: 0}}, "
        "{$lookup: {from: 'lookupColl', as: 'asField', localField: 'y', foreignField: 'z'}}]";

    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe, serializedPipe);
}

TEST(PipelineOptimizationTest, LookupShouldSwapWithMatchOnExpr) {
    std::string inputPipe =
        "[{$lookup: {from: 'lookupColl', as: 'asField', localField: 'y', foreignField: "
        "'z'}}, "
        " {$match: {$expr: {$eq: ['$independent', 1]}}}]";
    std::string outputPipe =
        "[{$match: {$and: [{$expr: {$eq: ['$independent', {$const: 1}]}},"
        "                  {independent: {$_internalExprEq: 1}}]}},"
        " {$lookup: {from: 'lookupColl', as: 'asField', localField: 'y', foreignField: 'z'}}]";
    std::string serializedPipe =
        "[{$match: {$expr: {$eq: ['$independent', 1]}}}, "
        "{$lookup: {from: 'lookupColl', as: 'asField', localField: 'y', foreignField: 'z'}}]";

    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe, serializedPipe);
}

TEST(PipelineOptimizationTest, LookupWithPipelineSyntaxShouldSwapWithMatch) {
    std::string inputPipe =
        "[{$lookup: {from: 'lookupColl', as: 'asField', pipeline: []}}, "
        " {$match: {'independent': 0}}]";
    std::string outputPipe =
        "[{$match: {independent: {$eq : 0}}}, "
        " {$lookup: {from: 'lookupColl', as: 'asField', let: {}, pipeline: []}}]";
    std::string serializedPipe =
        "[{$match: {independent: 0}}, "
        "{$lookup: {from: 'lookupColl', as: 'asField', let: {}, pipeline: []}}]";

    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe, serializedPipe);
}

TEST(PipelineOptimizationTest, LookupWithPipelineSyntaxShouldSwapWithMatchOnExpr) {
    std::string inputPipe =
        "[{$lookup: {from: 'lookupColl', as: 'asField', pipeline: []}}, "
        " {$match: {$expr: {$eq: ['$independent', 1]}}}]";
    std::string outputPipe =
        "[{$match: {$and: [{$expr: {$eq: ['$independent', {$const: 1}]}},"
        "                  {independent: {$_internalExprEq: 1}}]}},"
        " {$lookup: {from: 'lookupColl', as: 'asField', let: {}, pipeline: []}}]";
    std::string serializedPipe =
        "[{$match: {$expr: {$eq: ['$independent', 1]}}}, "
        "{$lookup: {from: 'lookupColl', as: 'asField', let: {}, pipeline: []}}]";

    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe, serializedPipe);
}

TEST(PipelineOptimizationTest, LookupShouldSplitMatch) {
    std::string inputPipe =
        "[{$lookup: {from: 'lookupColl', as: 'asField', localField: 'y', foreignField: "
        "'z'}}, "
        " {$match: {'independent': 0, asField: {$eq: 3}}}]";
    std::string outputPipe =
        "[{$match: {independent: {$eq: 0}}}, "
        " {$lookup: {from: 'lookupColl', as: 'asField', localField: 'y', foreignField: "
        "'z'}}, "
        " {$match: {asField: {$eq: 3}}}]";
    assertPipelineOptimizesTo(inputPipe, outputPipe);
}

TEST(PipelineOptimizationTest, LookupShouldNotAbsorbMatchOnAs) {
    std::string inputPipe =
        "[{$lookup: {from: 'lookupColl', as: 'asField', localField: 'y', foreignField: "
        "'z'}}, "
        " {$match: {'asField.subfield': 0}}]";
    std::string outputPipe =
        "[{$lookup: {from: 'lookupColl', as: 'asField', localField: 'y', foreignField: "
        "'z'}}, "
        " {$match: {'asField.subfield': {$eq : 0}}}]";
    std::string serializedPipe =
        "[{$lookup: {from: 'lookupColl', as: 'asField', localField: 'y', foreignField: "
        "'z'}}, "
        " {$match: {'asField.subfield': 0}}]";

    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe, serializedPipe);
}

TEST(PipelineOptimizationTest, LookupShouldNotAbsorbMatchWithExprOnAs) {
    std::string inputPipe =
        "[{$lookup: {from: 'lookupColl', as: 'asField', localField: 'y', foreignField: 'z'}},"
        " {$match: {$expr: {$eq: ['$asField.subfield', 0]}}}]";
    std::string outputPipe =
        "[{$lookup: {from: 'lookupColl', as: 'asField', localField: 'y', foreignField: 'z'}},"
        "{$match: {$and: [{$expr: {$eq: ['$asField.subfield', {$const: 0}]}},"
        "                 {'asField.subfield': {$_internalExprEq: 0}}]}}]";
    std::string serializedPipe =
        "[{$lookup: {from: 'lookupColl', as: 'asField', localField: 'y', foreignField: 'z'}},"
        " {$match: {$expr: {$eq: ['$asField.subfield', 0]}}}]";

    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe, serializedPipe);
}

TEST(PipelineOptimizationTest, LookupShouldAbsorbUnwindMatch) {
    std::string inputPipe =
        "[{$lookup: {from: 'lookupColl', as: 'asField', localField: 'y', foreignField: "
        "'z'}}, "
        "{$unwind: '$asField'}, "
        "{$match: {'asField.subfield': {$eq: 1}}}]";
    std::string outputPipe =
        "[{$lookup: {from: 'lookupColl', as: 'asField', localField: 'y', foreignField: 'z', "
        "            let: {}, pipeline: [{$match: {subfield: {$eq: 1}}}],"
        "            unwinding: {preserveNullAndEmptyArrays: false}}}]";
    std::string serializedPipe =
        "[{$lookup: {from: 'lookupColl', as: 'asField', localField: 'y', foreignField: "
        "'z',  let: {}, pipeline: [{$match: {subfield: {$eq: 1}}}]}},"
        "{$unwind: {path: '$asField'}}]";
    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe, serializedPipe);
}

TEST(PipelineOptimizationTest, LookupShouldAbsorbUnwindAndTypeMatch) {
    std::string inputPipe =
        "[{$lookup: {from: 'lookupColl', as: 'asField', localField: 'y', foreignField: "
        "'z'}}, "
        "{$unwind: '$asField'}, "
        "{$match: {'asField.subfield': {$type: [2]}}}]";
    std::string outputPipe =
        "[{$lookup: {from: 'lookupColl', as: 'asField', localField: 'y', foreignField: 'z', "
        "            let: {}, pipeline: [{$match: {subfield: {$type: [2]}}}],"
        "            unwinding: {preserveNullAndEmptyArrays: false}}}]";
    std::string serializedPipe =
        "[{$lookup: {from: 'lookupColl', as: 'asField', localField: 'y', foreignField: "
        "'z', let: {}, pipeline: [{$match: {subfield: {$type: [2]}}}]}},"
        "{$unwind: {path: '$asField'}}]";
    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe, serializedPipe);
}

TEST(PipelineOptimizationTest, LookupWithPipelineSyntaxShouldAbsorbUnwindMatch) {
    std::string inputPipe =
        "[{$lookup: {from: 'lookupColl', as: 'asField', pipeline: []}}, "
        "{$unwind: '$asField'}, "
        "{$match: {'asField.subfield': {$eq: 1}}}]";
    std::string outputPipe =
        "[{$lookup: {from: 'lookupColl', as: 'asField', let: {}, "
        "pipeline: [{$match: {subfield: {$eq: 1}}}], "
        "unwinding: {preserveNullAndEmptyArrays: false} } } ]";
    std::string serializedPipe =
        "[{$lookup: {from: 'lookupColl', as: 'asField', let: {}, "
        "pipeline: [{$match: {subfield: {$eq: 1}}}]}}, "
        "{$unwind: {path: '$asField'}}]";
    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe, serializedPipe);
}

TEST(PipelineOptimizationTest, LookupShouldAbsorbUnwindAndSplitAndAbsorbMatch) {
    std::string inputPipe =
        "[{$lookup: {from: 'lookupColl', as: 'asField', localField: 'y', foreignField: "
        "'z'}}, "
        " {$unwind: '$asField'}, "
        " {$match: {'asField.subfield': {$eq: 1}, independentField: {$gt: 2}}}]";
    std::string outputPipe =
        "[{$match: {independentField: {$gt: 2}}}, "
        " {$lookup: { "
        "      from: 'lookupColl', "
        "      as: 'asField', "
        "      localField: 'y', "
        "      foreignField: 'z', "
        "      let: {}, "
        "      pipeline: [{$match: {subfield: {$eq: 1}}}], "
        "      unwinding: { "
        "          preserveNullAndEmptyArrays: false"
        "      } "
        " }}]";
    std::string serializedPipe =
        "[{$match: {independentField: {$gt: 2}}}, "
        " {$lookup: {from: 'lookupColl', as: 'asField', localField: 'y', foreignField: "
        "'z', let: {}, pipeline: [{$match: {subfield: {$eq: 1}}}]}}, "
        " {$unwind: {path: '$asField'}}]";
    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe, serializedPipe);
}

TEST(PipelineOptimizationTest, LookupShouldNotSplitIndependentAndDependentOrClauses) {
    // If any child of the $or is dependent on the 'asField', then the $match cannot be moved above
    // the $lookup, and if any child of the $or is independent of the 'asField', then the $match
    // cannot be absorbed by the $lookup.
    std::string inputPipe =
        "[{$lookup: {from: 'lookupColl', as: 'asField', localField: 'y', foreignField: "
        "'z'}}, "
        " {$unwind: '$asField'}, "
        " {$match: {$or: [{'independent': {$gt: 4}}, "
        "                 {'asField.dependent': {$elemMatch: {a: {$eq: 1}}}}]}}]";
    std::string outputPipe =
        "[{$lookup: {from: 'lookupColl', as: 'asField', localField: 'y', foreignField: 'z', "
        "            unwinding: {preserveNullAndEmptyArrays: false}}}, "
        " {$match: {$or: [{'asField.dependent': {$elemMatch: {a: {$eq: 1}}}}, "
        "                 {'independent': {$gt: 4}}]}}]";
    std::string serializedPipe =
        "[{$lookup: {from: 'lookupColl', as: 'asField', localField: 'y', foreignField: "
        "'z'}}, "
        " {$unwind: {path: '$asField'}}, "
        " {$match: {$or: [{'independent': {$gt: 4}}, "
        "                 {'asField.dependent': {$elemMatch: {a: {$eq: 1}}}}]}}]";
    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe, serializedPipe);
}

TEST(PipelineOptimizationTest, LookupWithMatchOnArrayIndexFieldShouldNotCoalesce) {
    std::string inputPipe =
        "[{$lookup: {from: 'lookupColl', as: 'asField', localField: 'y', foreignField: "
        "'z'}}, "
        " {$unwind: {path: '$asField', includeArrayIndex: 'index'}}, "
        " {$match: {index: 0, 'asField.value': {$gt: 0}, independent: 1}}]";
    std::string outputPipe =
        "[{$match: {independent: {$eq: 1}}}, "
        " {$lookup: { "
        "      from: 'lookupColl', "
        "      as: 'asField', "
        "      localField: 'y', "
        "      foreignField: 'z', "
        "      unwinding: { "
        "          preserveNullAndEmptyArrays: false, "
        "          includeArrayIndex: 'index' "
        "      } "
        " }}, "
        " {$match: {$and: [{index: {$eq: 0}}, {'asField.value': {$gt: 0}}]}}]";
    std::string serializedPipe =
        "[{$match: {independent: {$eq: 1}}}, "
        " {$lookup: {from: 'lookupColl', as: 'asField', localField: 'y', foreignField: "
        "'z'}}, "
        " {$unwind: {path: '$asField', includeArrayIndex: 'index'}}, "
        " {$match: {$and: [{index: {$eq: 0}}, {'asField.value': {$gt: 0}}]}}]";
    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe, serializedPipe);
}

TEST(PipelineOptimizationTest, LookupWithUnwindPreservingNullAndEmptyArraysShouldNotCoalesce) {
    std::string inputPipe =
        "[{$lookup: {from: 'lookupColl', as: 'asField', localField: 'y', foreignField: "
        "'z'}}, "
        " {$unwind: {path: '$asField', preserveNullAndEmptyArrays: true}}, "
        " {$match: {'asField.value': {$gt: 0}, independent: 1}}]";
    std::string outputPipe =
        "[{$match: {independent: {$eq: 1}}}, "
        " {$lookup: { "
        "      from: 'lookupColl', "
        "      as: 'asField', "
        "      localField: 'y', "
        "      foreignField: 'z', "
        "      unwinding: { "
        "          preserveNullAndEmptyArrays: true"
        "      } "
        " }}, "
        " {$match: {'asField.value': {$gt: 0}}}]";
    std::string serializedPipe =
        "[{$match: {independent: {$eq: 1}}}, "
        " {$lookup: {from: 'lookupColl', as: 'asField', localField: 'y', foreignField: "
        "'z'}}, "
        " {$unwind: {path: '$asField', preserveNullAndEmptyArrays: true}}, "
        " {$match: {'asField.value': {$gt: 0}}}]";
    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe, serializedPipe);
}

TEST(PipelineOptimizationTest, LookupDoesNotAbsorbElemMatch) {
    std::string inputPipe =
        "[{$lookup: {from: 'lookupColl', as: 'x', localField: 'y', foreignField: 'z'}}, "
        " {$unwind: '$x'}, "
        " {$match: {x: {$elemMatch: {a: 1}}}}]";
    std::string outputPipe =
        "[{$lookup: { "
        "             from: 'lookupColl', "
        "             as: 'x', "
        "             localField: 'y', "
        "             foreignField: 'z', "
        "             unwinding: { "
        "                          preserveNullAndEmptyArrays: false "
        "             } "
        "           } "
        " }, "
        " {$match: {x: {$elemMatch: {a: {$eq: 1}}}}}]";
    std::string serializedPipe =
        "[{$lookup: {from: 'lookupColl', as: 'x', localField: 'y', foreignField: 'z'}}, "
        " {$unwind: {path: '$x'}}, "
        " {$match: {x: {$elemMatch: {a: 1}}}}]";
    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe, serializedPipe);
}

TEST(PipelineOptimizationTest, LookupDoesSwapWithMatchOnLocalField) {
    std::string inputPipe =
        "[{$lookup: {from: 'lookupColl', as: 'x', localField: 'y', foreignField: 'z'}}, "
        " {$match: {y: {$eq: 3}}}]";
    std::string outputPipe =
        "[{$match: {y: {$eq: 3}}}, "
        " {$lookup: {from: 'lookupColl', as: 'x', localField: 'y', foreignField: 'z'}}]";
    assertPipelineOptimizesTo(inputPipe, outputPipe);
}

TEST(PipelineOptimizationTest, LookupDoesSwapWithMatchOnFieldWithSameNameAsForeignField) {
    std::string inputPipe =
        "[{$lookup: {from: 'lookupColl', as: 'x', localField: 'y', foreignField: 'z'}}, "
        " {$match: {z: {$eq: 3}}}]";
    std::string outputPipe =
        "[{$match: {z: {$eq: 3}}}, "
        " {$lookup: {from: 'lookupColl', as: 'x', localField: 'y', foreignField: 'z'}}]";
    assertPipelineOptimizesTo(inputPipe, outputPipe);
}

TEST(PipelineOptimizationTest, LookupDoesNotAbsorbUnwindOnSubfieldOfAsButStillMovesMatch) {
    std::string inputPipe =
        "[{$lookup: {from: 'lookupColl', as: 'x', localField: 'y', foreignField: 'z'}}, "
        " {$unwind: {path: '$x.subfield'}}, "
        " {$match: {'independent': 2, 'x.dependent': 2}}]";
    std::string outputPipe =
        "[{$match: {'independent': {$eq: 2}}}, "
        " {$lookup: {from: 'lookupColl', as: 'x', localField: 'y', foreignField: 'z'}}, "
        " {$match: {'x.dependent': {$eq: 2}}}, "
        " {$unwind: {path: '$x.subfield'}}]";
    assertPipelineOptimizesTo(inputPipe, outputPipe);
}

TEST(PipelineOptimizationTest, GroupShouldSwapWithMatchIfFilteringOnID) {
    std::string inputPipe =
        "[{$group : {_id:'$a'}}, "
        " {$match: {_id : 4}}]";
    std::string outputPipe =
        "[{$match: {a:{$eq : 4}}}, "
        " {$group:{_id:'$a'}}]";
    std::string serializedPipe =
        "[{$match: {a:{$eq :4}}}, "
        " {$group:{_id:'$a'}}]";

    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe, serializedPipe);
}

TEST(PipelineOptimizationTest, GroupShouldSwapWithMatchOnExprIfFilteringOnID) {
    std::string inputPipe =
        "[{$group: {_id: '$a'}}, "
        " {$match: {$expr: {$eq: ['$_id', 4]}}}]";
    std::string outputPipe =
        "[{$match: {$and: [{$expr: {$eq: ['$a', {$const: 4}]}}, {a: {$_internalExprEq: 4}}]}},"
        " {$group: {_id: '$a'}}]";
    std::string serializedPipe =
        "[{$match: {$expr: {$eq: ['$a', {$const: 4}]}}}, "
        " {$group: {_id: '$a'}}]";

    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe, serializedPipe);
}

TEST(PipelineOptimizationTest, GroupShouldNotSwapWithMatchOnExprIfNotFilteringOnID) {
    std::string inputPipe =
        "[{$group : {_id:'$a'}}, "
        " {$match: {$expr: {$eq: ['$b', 4]}}}]";
    std::string outputPipe =
        "[{$group : {_id:'$a'}}, "
        " {$match: {$and: [{$expr: {$eq: ['$b', {$const: 4}]}}, {b: {$_internalExprEq: 4}}]}}]";
    std::string serializedPipe =
        "[{$group : {_id:'$a'}}, "
        " {$match: {$expr: {$eq: ['$b', 4]}}}]";

    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe, serializedPipe);
}

TEST(PipelineOptimizationTest, GroupShouldNotSwapWithMatchIfNotFilteringOnID) {
    std::string inputPipe =
        "[{$group : {_id:'$a'}}, "
        " {$match: {b : 4}}]";
    std::string outputPipe =
        "[{$group : {_id:'$a'}}, "
        " {$match: {b : {$eq: 4}}}]";
    std::string serializedPipe =
        "[{$group : {_id:'$a'}}, "
        " {$match: {b : 4}}]";

    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe, serializedPipe);
}

TEST(PipelineOptimizationTest, GroupShouldNotSwapWithMatchIfExistsPredicateOnID) {
    std::string inputPipe =
        "[{$group : {_id:'$x'}}, "
        " {$match: {_id : {$exists: true}}}]";
    std::string outputPipe =
        "[{$group : {_id:'$x'}}, "
        " {$match: {_id : {$exists: true}}}]";
    std::string serializedPipe =
        "[{$group : {_id:'$x'}}, "
        " {$match: {_id : {$exists: true}}}]";

    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe, serializedPipe);
}

TEST(PipelineOptimizationTest, GroupShouldNotSwapWithCompoundMatchIfExistsPredicateOnID) {
    std::string inputPipe =
        "[{$group : {_id:'$x'}}, "
        " {$match: {$or : [ {_id : {$exists: true}}, {_id : {$gt : 70}}]}}]";
    std::string outputPipe =
        "[{$group : {_id:'$x'}}, "
        " {$match: {$or : [ {_id : {$gt : 70}}, {_id : {$exists: true}}]}}]";
    std::string serializedPipe =
        "[{$group : {_id:'$x'}}, "
        " {$match: {$or : [ {_id : {$exists: true}}, {_id : {$gt : 70}}]}}]";

    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe, serializedPipe);
}

TEST(PipelineOptimizationTest, GroupShouldSwapWithCompoundMatchIfFilteringOnID) {
    std::string inputPipe =
        "[{$group : {_id:'$x'}}, "
        " {$match: {$or : [ {_id : {$lte : 50}}, {_id : {$gt : 70}}]}}]";
    std::string outputPipe =
        "[{$match: {$or : [  {x : {$lte : 50}}, {x : {$gt : 70}}]}},"
        "{$group : {_id:'$x'}}]";
    std::string serializedPipe =
        "[{$match: {$or : [  {x : {$lte : 50}}, {x : {$gt : 70}}]}},"
        "{$group : {_id:'$x'}}]";

    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe, serializedPipe);
}

TEST(PipelineOptimizationTest, MatchShouldDuplicateItselfBeforeRedact) {
    std::string inputPipe = "[{$redact: '$$PRUNE'}, {$match: {a: 1, b:12}}]";
    std::string outputPipe =
        "[{$match: {$and: [{a: {$eq: 1}}, {b: {$eq: 12}}]}}, {$redact: '$$PRUNE'}, "
        "{$match: {$and: [{a: {$eq: 1}}, {b: {$eq: 12}}]}}]";
    std::string serializedPipe =
        "[{$match: {a: 1, b: 12}}, {$redact: '$$PRUNE'}, {$match: {a: 1, b: 12}}]";
    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe, serializedPipe);
}

TEST(PipelineOptimizationTest, MatchShouldSwapWithUnwind) {
    std::string inputPipe =
        "[{$unwind: '$a.b.c'}, "
        "{$match: {'b': 1}}]";
    std::string outputPipe =
        "[{$match: {'b': {$eq : 1}}}, "
        "{$unwind: {path: '$a.b.c'}}]";
    std::string serializedPipe = "[{$match: {b: 1}}, {$unwind: {path: '$a.b.c'}}]";
    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe, serializedPipe);
}

TEST(PipelineOptimizationTest, MatchOnExprShouldSwapWithUnwind) {
    std::string inputPipe =
        "[{$unwind: '$a.b.c'}, "
        "{$match: {$expr: {$eq: ['$b', 1]}}}]";
    std::string outputPipe =
        "[{$match: {$and: [{$expr: {$eq: ['$b', {$const: 1}]}}, {b: {$_internalExprEq: 1}}]}}, "
        "{$unwind: {path: '$a.b.c'}}]";
    std::string serializedPipe =
        "[{$match: {$expr: {$eq: ['$b', 1]}}}, {$unwind: {path: '$a.b.c'}}]";
    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe, serializedPipe);
}

TEST(PipelineOptimizationTest, MatchOnPrefixShouldNotSwapOnUnwind) {
    std::string inputPipe =
        "[{$unwind: {path: '$a.b.c'}}, "
        "{$match: {'a.b': 1}}]";
    std::string outputPipe =
        "[{$unwind: {path: '$a.b.c'}}, "
        "{$match: {'a.b': {$eq : 1}}}]";
    std::string serializedPipe = "[{$unwind: {path: '$a.b.c'}}, {$match: {'a.b': 1}}]";
    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe, serializedPipe);
}

TEST(PipelineOptimizationTest, MatchShouldSplitOnUnwind) {
    std::string inputPipe =
        "[{$unwind: '$a.b'}, "
        "{$match: {$and: [{f: {$eq: 5}}, "
        "                 {$nor: [{'a.d': 1, c: 5}, {'a.b': 3, c: 5}]}]}}]";
    std::string outputPipe =
        "[{$match: {$and: [{f: {$eq: 5}},"
        "                  {$nor: [{$and: [{'a.d': {$eq: 1}}, {c: {$eq: 5}}]}]}]}},"
        "{$unwind: {path: '$a.b'}}, "
        "{$match: {$nor: [{$and: [{'a.b': {$eq: 3}}, {c: {$eq: 5}}]}]}}]";
    assertPipelineOptimizesTo(inputPipe, outputPipe);
}

// The 'a.b' path is a modified one by $unwind and $elemMatch is dependent on it and so we can't
// swap $elemMatch in this case.
TEST(PipelineOptimizationTest, MatchShouldNotOptimizeWithElemMatchOnModifiedPathByUnwind) {
    std::string inputPipe =
        "[{$unwind: {path: '$a.b'}}, "
        "{$match: {a: {$elemMatch: {b: {d: 1}}}}}]";
    std::string outputPipe =
        "[{$unwind: {path: '$a.b'}}, "
        "{$match: {a: {$elemMatch: {b: {$eq : {d: 1}}}}}}]";
    std::string serializedPipe =
        "[{$unwind : {path : '$a.b'}}, {$match : {a : {$elemMatch : {b : {d : 1}}}}}]";
    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe, serializedPipe);
}

// The 'a.b' path is a modified one by $project and $elemMatch is dependent on it and so we can't
// swap $elemMatch in this case.
TEST(PipelineOptimizationTest, MatchShouldNotOptimizeWithElemMatchOnModifiedPathByProject1) {
    std::string inputPipe =
        "[{$project: {x: '$a.b', _id: false}}, "
        "{$match: {x: {$elemMatch: {d: 1}}}}]";
    std::string outputPipe =
        "[{$project: {x: '$a.b', _id: false}}, "
        "{$match: {x: {$elemMatch: {d: {$eq: 1}}}}}]";
    std::string serializedPipe =
        "[{$project: {x: '$a.b', _id: false}}, "
        "{$match: {x: {$elemMatch: {d: 1}}}}]";
    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe, serializedPipe);
}

// The 'a.b' path is a modified one by $project and $elemMatch is dependent on it and so we can't
// swap $elemMatch in this case.
TEST(PipelineOptimizationTest, MatchShouldNotOptimizeWithElemMatchOnModifiedPathByProject2) {
    std::string inputPipe =
        "[{$project: {x: {y: '$a.b'}, _id: false}}, "
        "{$match: {'x.y': {$elemMatch: {d: 1}}}}]";
    std::string outputPipe =
        "[{$project: {x: {y: '$a.b'}, _id: false}}, "
        "{$match: {'x.y': {$elemMatch: {d: {$eq: 1}}}}}]";
    std::string serializedPipe =
        "[{$project: {x: {y: '$a.b'}, _id: false}}, "
        "{$match: {'x.y': {$elemMatch: {d: 1}}}}]";
    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe, serializedPipe);
}

// The 'a.b' path is a modified one by $project and $elemMatch is dependent on it and so we can't
// swap $elemMatch in this case.
TEST(PipelineOptimizationTest, MatchShouldNotOptimizeWithElemMatchOnModifiedPathByProject3) {
    std::string inputPipe =
        "[{$project: {x: {y: {z: '$a.b'}}, _id: false}}, "
        "{$match: {'x.y.z': {$elemMatch: {d: 1}}}}]";
    std::string outputPipe =
        "[{$project: {x: {y: {z: '$a.b'}}, _id: false}}, "
        "{$match: {'x.y.z': {$elemMatch: {d: {$eq: 1}}}}}]";
    std::string serializedPipe =
        "[{$project: {x: {y: {z: '$a.b'}}, _id: false}}, "
        "{$match: {'x.y.z': {$elemMatch: {d: 1}}}}]";
    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe, serializedPipe);
}

TEST(PipelineOptimizationTest, MatchShouldNotOptimizeWhenMatchingOnIndexField) {
    std::string inputPipe =
        "[{$unwind: {path: '$a', includeArrayIndex: 'foo'}}, "
        " {$match: {foo: 0, b: 1}}]";
    std::string outputPipe =
        "[{$match: {b: {$eq: 1}}}, "
        " {$unwind: {path: '$a', includeArrayIndex: 'foo'}}, "
        " {$match: {foo: {$eq: 0}}}]";
    assertPipelineOptimizesTo(inputPipe, outputPipe);
}

TEST(PipelineOptimizationTest, MatchWithNorOnlySplitsIndependentChildren) {
    std::string inputPipe =
        "[{$unwind: {path: '$a'}}, "
        "{$match: {$nor: [{$and: [{a: {$eq: 1}}, {b: {$eq: 1}}]}, {b: {$eq: 2}} ]}}]";
    std::string outputPipe =
        R"(
        [{$match: {b: {$not: {$eq: 2}}}},
         {$unwind: {path: '$a'}},
         {$match: {$nor: [{$and: [{a: {$eq: 1}}, {b: {$eq: 1}}]}]}}])";
    std::string serializedPipe = R"(
        [{$match: {$nor: [{b: {$eq: 2}}]}},
         {$unwind: {path: '$a'}},
         {$match: {$nor: [{$and: [{a: {$eq: 1}}, {b: {$eq: 1}}]}]}}])";
    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe, serializedPipe);
}

TEST(PipelineOptimizationTest, MatchWithOrDoesNotSplit) {
    std::string inputPipe =
        "[{$unwind: {path: '$a'}}, "
        "{$match: {$or: [{a: {$eq: 'dependent'}}, {b: {$eq: 'independent'}}]}}]";
    std::string outputPipe =
        "[{$unwind: {path: '$a'}}, "
        "{$match: {$or: [{a: {$eq: 'dependent'}}, {b: {$eq: 'independent'}}]}}]";
    assertPipelineOptimizesTo(inputPipe, outputPipe);
}

TEST(PipelineOptimizationTest, MatchOnExprWithOrDoesNotSplit) {
    std::string inputPipe =
        "[{$unwind: {path: '$a'}}, "
        " {$match: {$or: [{$expr: {$eq: ['$a', 'dependent']}}, {b: {$eq: 'independent'}}]}}]";
    std::string outputPipe =
        "[{$unwind: {path: '$a'}}, "
        " {$match: {$or: [{$and: [{$expr: {$eq: ['$a', {$const: 'dependent'}]}},"
        "                         {a: {$_internalExprEq: 'dependent'}}]},"
        "                 {b: {$eq: 'independent'}}]}}]";
    std::string serializedPipe =
        "[{$unwind: {path: '$a'}}, "
        " {$match: {$or: [{$expr: {$eq: ['$a', 'dependent']}}, {b: {$eq: 'independent'}}]}}]";
    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe, serializedPipe);
}

TEST(PipelineOptimizationTest, UnwindBeforeDoubleMatchShouldRepeatedlyOptimize) {
    std::string inputPipe =
        "[{$unwind: '$a'}, "
        "{$match: {b: {$gt: 0}}}, "
        "{$match: {a: 1, c: 1}}]";
    std::string outputPipe =
        "[{$match: {$and: [{c: {$eq: 1}}, {b: {$gt: 0}}]}},"
        "{$unwind: {path: '$a'}}, "
        "{$match: {a: {$eq: 1}}}]";
    std::string serializedPipe =
        "[{$match: {$and: [{b: {$gt: 0}}, {c: {$eq: 1}}]}},"
        "{$unwind: {path: '$a'}}, "
        "{$match: {a: {$eq: 1}}}]";
    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe, serializedPipe);
}

TEST(PipelineOptimizationTest, GraphLookupShouldCoalesceWithUnwindOnAs) {
    std::string inputPipe =
        "[{$graphLookup: {from: 'lookupColl', as: 'out', connectToField: 'b', "
        "                 connectFromField: 'c', startWith: '$d'}}, "
        " {$unwind: '$out'}]";

    std::string outputPipe =
        "[{$graphLookup: {from: 'lookupColl', as: 'out', connectToField: 'b', "
        "                 connectFromField: 'c', startWith: '$d', "
        "                 unwinding: {preserveNullAndEmptyArrays: false}}}]";

    std::string serializedPipe =
        "[{$graphLookup: {from: 'lookupColl', as: 'out', connectToField: 'b', "
        "                 connectFromField: 'c', startWith: '$d'}}, "
        " {$unwind: {path: '$out'}}]";
    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe, serializedPipe);
}

TEST(PipelineOptimizationTest, GraphLookupShouldCoalesceWithUnwindOnAsWithPreserveEmpty) {
    std::string inputPipe =
        "[{$graphLookup: {from: 'lookupColl', as: 'out', connectToField: 'b', "
        "                 connectFromField: 'c', startWith: '$d'}}, "
        " {$unwind: {path: '$out', preserveNullAndEmptyArrays: true}}]";

    std::string outputPipe =
        "[{$graphLookup: {from: 'lookupColl', as: 'out', connectToField: 'b', "
        "                 connectFromField: 'c', startWith: '$d', "
        "                 unwinding: {preserveNullAndEmptyArrays: true}}}]";

    std::string serializedPipe =
        "[{$graphLookup: {from: 'lookupColl', as: 'out', connectToField: 'b', "
        "                 connectFromField: 'c', startWith: '$d'}}, "
        " {$unwind: {path: '$out', preserveNullAndEmptyArrays: true}}]";
    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe, serializedPipe);
}

TEST(PipelineOptimizationTest, GraphLookupShouldCoalesceWithUnwindOnAsWithIncludeArrayIndex) {
    std::string inputPipe =
        "[{$graphLookup: {from: 'lookupColl', as: 'out', connectToField: 'b', "
        "                 connectFromField: 'c', startWith: '$d'}}, "
        " {$unwind: {path: '$out', includeArrayIndex: 'index'}}]";

    std::string outputPipe =
        "[{$graphLookup: {from: 'lookupColl', as: 'out', connectToField: 'b', "
        "                 connectFromField: 'c', startWith: '$d', "
        "                 unwinding: {preserveNullAndEmptyArrays: false, "
        "                             includeArrayIndex: 'index'}}}]";

    std::string serializedPipe =
        "[{$graphLookup: {from: 'lookupColl', as: 'out', connectToField: 'b', "
        "                 connectFromField: 'c', "
        "                 startWith: '$d'}}, "
        " {$unwind: {path: '$out', includeArrayIndex: 'index'}}]";
    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe, serializedPipe);
}

TEST(PipelineOptimizationTest, GraphLookupShouldNotCoalesceWithUnwindNotOnAs) {
    std::string inputPipe =
        "[{$graphLookup: {from: 'lookupColl', as: 'out', connectToField: 'b', "
        "                 connectFromField: 'c', startWith: '$d'}}, "
        " {$unwind: '$nottherightthing'}]";

    std::string outputPipe =
        "[{$graphLookup: {from: 'lookupColl', as: 'out', connectToField: 'b', "
        "                 connectFromField: 'c', startWith: '$d'}}, "
        " {$unwind: {path: '$nottherightthing'}}]";
    assertPipelineOptimizesTo(inputPipe, outputPipe);
}

TEST(PipelineOptimizationTest, GraphLookupShouldSwapWithMatch) {
    std::string inputPipe =
        "[{$graphLookup: {"
        "    from: 'lookupColl',"
        "    as: 'results',"
        "    connectToField: 'to',"
        "    connectFromField: 'from',"
        "    startWith: '$startVal'"
        " }},"
        " {$match: {independent: 'x'}}"
        "]";
    std::string outputPipe =
        "[{$match: {independent: {$eq : 'x'}}},"
        " {$graphLookup: {"
        "    from: 'lookupColl',"
        "    as: 'results',"
        "    connectToField: 'to',"
        "    connectFromField: 'from',"
        "    startWith: '$startVal'"
        " }}]";
    std::string serializedPipe =
        "[{$match: {independent: 'x'}}, "
        " {$graphLookup: {"
        "   from: 'lookupColl',"
        "   as: 'results',"
        "   connectToField: 'to',"
        "   connectFromField: 'from',"
        "   startWith: '$startVal'"
        " }}]";
    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe, serializedPipe);
}

TEST(PipelineOptimizationTest, GraphLookupShouldSwapWithSortNotOnAs) {
    std::string inputPipe =
        "["
        "   {$graphLookup: {"
        "       from: 'lookupColl',"
        "       as: 'out',"
        "       connectToField: 'to',"
        "       connectFromField: 'from',"
        "       startWith: '$start'"
        "   }},"
        "   {$sort: {from: 1}}"
        "]";
    std::string outputPipe =
        "["
        "   {$sort: {sortKey: {from: 1}}},"
        "   {$graphLookup: {"
        "       from: 'lookupColl',"
        "       as: 'out',"
        "       connectToField: 'to',"
        "       connectFromField: 'from',"
        "       startWith: '$start'"
        "   }}"
        "]";
    std::string serializedPipe =
        "["
        "   {$sort: {from: 1}},"
        "   {$graphLookup: {"
        "       from: 'lookupColl',"
        "       as: 'out',"
        "       connectToField: 'to',"
        "       connectFromField: 'from',"
        "       startWith: '$start'"
        "   }}"
        "]";
    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe, serializedPipe);
}

TEST(PipelineOptimizationTest, GraphLookupWithInternalUnwindShouldNotSwapWithSortNotOnAs) {
    std::string inputPipe =
        "["
        "   {$graphLookup: {"
        "       from: 'lookupColl',"
        "       as: 'out',"
        "       connectToField: 'to',"
        "       connectFromField: 'from',"
        "       startWith: '$start'"
        "   }},"
        "   {$unwind: {path: '$out', includeArrayIndex: 'index'}},"
        "   {$sort: {from: 1}}"
        "]";
    std::string outputPipe =
        "["
        "   {$graphLookup: {"
        "       from: 'lookupColl',"
        "       as: 'out',"
        "       connectToField: 'to',"
        "       connectFromField: 'from',"
        "       startWith: '$start',"
        "       unwinding: {preserveNullAndEmptyArrays: false, includeArrayIndex: 'index'}"
        "   }},"
        "   {$sort: {sortKey: {from: 1}}}"
        "]";
    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe, inputPipe);
}

TEST(PipelineOptimizationTest, GraphLookupShouldNotSwapWithSortOnAs) {
    std::string inputPipe =
        "["
        "   {$graphLookup: {"
        "       from: 'lookupColl',"
        "       as: 'out',"
        "       connectToField: 'to',"
        "       connectFromField: 'from',"
        "       startWith: '$start'"
        "   }},"
        "   {$sort: {out: 1}}"
        "]";
    std::string outputPipe =
        "["
        "   {$graphLookup: {"
        "       from: 'lookupColl',"
        "       as: 'out',"
        "       connectToField: 'to',"
        "       connectFromField: 'from',"
        "       startWith: '$start'"
        "   }},"
        "   {$sort: {sortKey: {out: 1}}}"
        "]";
    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe, inputPipe);
}

TEST(PipelineOptimizationTest, ExclusionProjectShouldSwapWithIndependentMatch) {
    std::string inputPipe = "[{$project: {redacted: 0}}, {$match: {unrelated: 4}}]";
    std::string outputPipe =
        "[{$match: {unrelated: {$eq : 4}}}, {$project: {redacted: false, _id: true}}]";
    std::string serializedPipe =
        "[{$match : {unrelated : 4}}, {$project : {redacted : false, _id: true}}]";
    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe, serializedPipe);
}

TEST(PipelineOptimizationTest, ExclusionProjectShouldNotSwapWithMatchOnExcludedFields) {
    std::string pipeline =
        "[{$project: {subdoc: {redacted: false}, _id: true}}, {$match: {'subdoc.redacted': {$eq : "
        "4}}}]";
    assertPipelineOptimizesTo(pipeline, pipeline);
}

TEST(PipelineOptimizationTest, MatchShouldSplitIfPartIsIndependentOfExclusionProjection) {
    std::string inputPipe =
        "[{$project: {redacted: 0}},"
        " {$match: {redacted: 'x', unrelated: 4}}]";
    std::string outputPipe =
        "[{$match: {unrelated: {$eq: 4}}},"
        " {$project: {redacted: false, _id: true}},"
        " {$match: {redacted: {$eq: 'x'}}}]";
    assertPipelineOptimizesTo(inputPipe, outputPipe);
}

TEST(PipelineOptimizationTest, MatchOnExprShouldSplitIfPartIsIndependentOfExclusionProjection) {
    std::string inputPipe =
        "[{$project: {redacted: 0}},"
        " {$match: {$and: [{$expr: {$eq: ['$redacted', 'x']}},"
        "                  {$expr: {$eq: ['$unrelated', 4]}}]}}]";
    std::string outputPipe =
        "[{$match: {$and: [{$expr: {$eq: ['$unrelated', {$const: 4}]}},"
        "                  {unrelated: {$_internalExprEq: 4}}]}},"
        " {$project: {redacted: false, _id: true}},"
        " {$match: {$and: [{$expr: {$eq: ['$redacted', {$const: 'x'}]}},"
        "                  {redacted: {$_internalExprEq: 'x'}}]}}]";
    std::string serializedPipe =
        "[{$match: {$expr: {$eq: ['$unrelated', {$const: 4}]}}},"
        " {$project: {redacted: false, _id: true}},"
        " {$match: {$expr: {$eq: ['$redacted', {$const: 'x'}]}}}]";
    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe, serializedPipe);
}

TEST(PipelineOptimizationTest, InclusionProjectShouldSwapWithIndependentMatch) {
    std::string inputPipe = "[{$project: {included: 1}}, {$match: {included: 4}}]";
    std::string outputPipe =
        "[{$match: {included: {$eq : 4}}}, {$project: {_id: true, included: true}}]";
    std::string serializedPipe =
        "[{$match : {included : 4}}, {$project : {_id: true, included : true}}]";
    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe, serializedPipe);
}

TEST(PipelineOptimizationTest, InclusionProjectShouldNotSwapWithMatchOnFieldsNotIncluded) {
    std::string inputPipe =
        "[{$project: {_id: true, included: true, subdoc: {included: true}}},"
        " {$match: {notIncluded: 'x', unrelated: 4}}]";
    std::string outputPipe =
        "[{$project: {_id: true, included: true, subdoc: {included: true}}},"
        " {$match: {$and: [{notIncluded: {$eq: 'x'}}, {unrelated: {$eq: 4}}]}}]";
    std::string serializedPipe =
        "[{$project: {_id: true, included: true, subdoc: {included: true}}},"
        " {$match: {notIncluded: 'x', unrelated: 4}}]";
    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe, serializedPipe);
}

TEST(PipelineOptimizationTest, MatchShouldSplitIfPartIsIndependentOfInclusionProjection) {
    std::string inputPipe =
        "[{$project: {_id: true, included: true}},"
        " {$match: {included: 'x', unrelated: 4}}]";
    std::string outputPipe =
        "[{$match: {included: {$eq: 'x'}}},"
        " {$project: {_id: true, included: true}},"
        " {$match: {unrelated: {$eq: 4}}}]";
    assertPipelineOptimizesTo(inputPipe, outputPipe);
}

TEST(PipelineOptimizationTest, MatchOnExprShouldNotSplitIfDependentOnInclusionProjection) {
    std::string inputPipe =
        "[{$project: {_id: true, included: true}},"
        " {$match: {$expr: {$eq: ['$redacted', 'x']}}}]";
    std::string outputPipe =
        "[{$project: {_id: true, included: true}},"
        " {$match: {$and: [{$expr: {$eq: ['$redacted', {$const: 'x'}]}},"
        "                  {redacted: {$_internalExprEq: 'x'}}]}}]";
    std::string serializedPipe =
        "[{$project: {_id: true, included: true}},"
        " {$match: {$expr: {$eq: ['$redacted', 'x']}}}]";
    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe, serializedPipe);
}

TEST(PipelineOptimizationTest, TwoMatchStagesShouldBothPushIndependentPartsBeforeProjection) {
    std::string inputPipe =
        "[{$project: {_id: true, included: true}},"
        " {$match: {included: 'x', unrelated: 4}},"
        " {$match: {included: 'y', unrelated: 5}}]";
    std::string outputPipe =
        "[{$match: {$and: [{included: {$eq: 'x'}}, {included: {$eq: 'y'}}]}},"
        " {$project: {_id: true, included: true}},"
        " {$match: {$and: [{unrelated: {$eq: 4}}, {unrelated: {$eq: 5}}]}}]";
    assertPipelineOptimizesTo(inputPipe, outputPipe);
}

TEST(PipelineOptimizationTest, NeighboringMatchesShouldCoalesce) {
    std::string inputPipe =
        "[{$match: {x: 'x'}},"
        " {$match: {y: 'y'}}]";
    std::string outputPipe = "[{$match: {$and: [{x: {$eq: 'x'}}, {y: {$eq : 'y'}}]}}]";
    std::string serializedPipe = "[{$match: {$and: [{x: 'x'}, {y: 'y'}]}}]";
    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe, serializedPipe);
}

TEST(PipelineOptimizationTest, MatchShouldNotSwapBeforeLimit) {
    std::string inputPipe = "[{$limit: 3}, {$match: {y: 'y'}}]";
    std::string outputPipe = "[{$limit: 3}, {$match: {y: {$eq : 'y'}}}]";
    std::string serializedPipe = "[{$limit: 3}, {$match: {y: 'y'}}]";
    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe, serializedPipe);
}

TEST(PipelineOptimizationTest, MatchOnExprShouldNotSwapBeforeLimit) {
    std::string inputPipe = "[{$limit: 3}, {$match : {$expr: {$eq: ['$y', 'y']}}}]";
    std::string outputPipe =
        "[{$limit: 3}, {$match: {$and: [{$expr: {$eq: ['$y', {$const: 'y'}]}},"
        "                               {y: {$_internalExprEq: 'y'}}]}}]";
    std::string serializedPipe = "[{$limit: 3}, {$match : {$expr: {$eq: ['$y', 'y']}}}]";
    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe, serializedPipe);
}

TEST(PipelineOptimizationTest, MatchShouldNotSwapBeforeSkip) {
    std::string inputPipe = "[{$skip: 3}, {$match: {y: 'y'}}]";
    std::string outputPipe = "[{$skip: 3}, {$match: {y: {$eq : 'y'}}}]";
    std::string serializedPipe = "[{$skip: 3}, {$match: {y: 'y'}}]";
    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe, serializedPipe);
}

TEST(PipelineOptimizationTest, MatchOnExprShouldNotSwapBeforeSkip) {
    std::string inputPipe = "[{$skip: 3}, {$match : {$expr: {$eq: ['$y', 'y']}}}]";
    std::string outputPipe =
        "[{$skip: 3}, {$match: {$and: [{$expr: {$eq: ['$y', {$const: 'y'}]}},"
        "                              {y: {$_internalExprEq: 'y'}}]}}]";
    std::string serializedPipe = "[{$skip: 3}, {$match : {$expr: {$eq: ['$y', 'y']}}}]";
    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe, serializedPipe);
}

TEST(PipelineOptimizationTest, MatchShouldMoveAcrossProjectRename) {
    std::string inputPipe = "[{$project: {_id: true, a: '$b'}}, {$match: {a: {$eq: 1}}}]";
    std::string outputPipe = "[{$match: {b: {$eq: 1}}}, {$project: {_id: true, a: '$b'}}]";
    assertPipelineOptimizesTo(inputPipe, outputPipe);
}

TEST(PipelineOptimizationTest, MatchShouldMoveAcrossAddFieldsRename) {
    std::string inputPipe = "[{$addFields: {a: '$b'}}, {$match: {a: {$eq: 1}}}]";
    std::string outputPipe = "[{$match: {b: {$eq: 1}}}, {$addFields: {a: '$b'}}]";
    assertPipelineOptimizesTo(inputPipe, outputPipe);
}

TEST(PipelineOptimizationTest, MatchShouldMoveAcrossProjectRenameWithExplicitROOT) {
    std::string inputPipe = "[{$project: {_id: true, a: '$$ROOT.b'}}, {$match: {a: {$eq: 1}}}]";
    std::string outputPipe = "[{$match: {b: {$eq: 1}}}, {$project: {_id: true, a: '$$ROOT.b'}}]";
    assertPipelineOptimizesTo(inputPipe, outputPipe);
}

TEST(PipelineOptimizationTest, MatchShouldMoveAcrossAddFieldsRenameWithExplicitCURRENT) {
    std::string inputPipe = "[{$addFields: {a: '$$CURRENT.b'}}, {$match: {a: {$eq: 1}}}]";
    std::string outputPipe = "[{$match: {b: {$eq: 1}}}, {$addFields: {a: '$b'}}]";
    assertPipelineOptimizesTo(inputPipe, outputPipe);
}

TEST(PipelineOptimizationTest, PartiallyDependentMatchWithRenameShouldSplitAcrossAddFields) {
    std::string inputPipe =
        "[{$addFields: {'a.b': '$c', d: {$add: ['$e', '$f']}}},"
        "{$match: {$and: [{$or: [{'a.b': 1}, {x: 2}]}, {d: 3}]}}]";
    std::string outputPipe =
        "[{$match: {$or: [{c: {$eq: 1}}, {x: {$eq: 2}}]}},"
        "{$addFields: {a: {b: '$c'}, d: {$add: ['$e', '$f']}}},"
        "{$match: {d: {$eq: 3}}}]";
    assertPipelineOptimizesTo(inputPipe, outputPipe);
}

TEST(PipelineOptimizationTest, NorCanSplitAcrossProjectWithRename) {
    std::string inputPipe =
        "[{$project: {x: true, y: '$z', _id: false}},"
        "{$match: {$nor: [{w: {$eq: 1}}, {y: {$eq: 1}}]}}]";
    std::string outputPipe =
        R"([{$match: {z : {$not: {$eq: 1}}}},
             {$project: {x: true, y: "$z", _id: false}},
             {$match: {w: {$not: {$eq: 1}}}}])";
    std::string serializedPipe = R"(
        [{$match: {$nor: [ {z : {$eq: 1}}]}},
         {$project: {x: true, y: "$z", _id: false}},
         {$match: {$nor: [ {w: {$eq: 1}}]}}]
        )";
    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe, serializedPipe);
}

TEST(PipelineOptimizationTest, MatchCanMoveAcrossDottedRenameOnGrouping) {
    std::string inputPipeline =
        "[{$group: { _id: { c: '$d' }, c: { $sum: {$const: 1} } } },"
        "{$project: { m: '$_id.c' } },"
        "{$match: { m: {$eq: 2} } }]";
    std::string outputPipeline =
        "[{$match: { d: {$eq: 2} } },"
        "{$group: { _id: { c: '$d' }, c: { $sum: {$const: 1} } } },"
        "{$project: { _id: true, m: '$_id.c' } }]";
    assertPipelineOptimizesTo(inputPipeline, outputPipeline);
}

TEST(PipelineOptimizationTest, MatchCanMoveAcrossDottedRenameOnGroupingMixedPredicates) {
    std::string inputPipeline =
        "[{$group: { _id: { c: '$d' }, c: { $sum: { $const: 1} } } },"
        "{$project: { m: '$_id.c' } },"
        "{$match: { $and: [ {m: {$eq: 2} }, {_id: {$eq: 3} } ] } }]";
    std::string outputPipeline =
        "[{$group: { _id: { c: '$d' }, c: { $sum: { $const: 1} } } },"
        "{$match: { $and: [{_id: {$eq: 3} }, {'_id.c': {$eq: 2} } ] } },"
        "{$project: { _id: true, m: '$_id.c' } } ]";
    std::string serializedPipe =
        "[{$group: { _id: { c: '$d' }, c: { $sum: { $const: 1} } } },"
        "{$match: { $and: [ {'_id.c': {$eq: 2} }, {_id: {$eq: 3} } ] } },"
        "{$project: { _id: true, m: '$_id.c' } } ]";

    assertPipelineOptimizesAndSerializesTo(inputPipeline, outputPipeline, serializedPipe);
}

TEST(PipelineOptimizationTest, AvoidPushingMatchOverGroupWithLongDottedRename) {
    std::string inputPipeline =
        "[{$group: {_id: {a: {b: '$a'}}}},"
        "{$project: {renamed: '$_id.a.b'}},"
        "{$match: {renamed: {$eq: 5}}}]";
    std::string outputPipeline =
        "[{$group: {_id: {a: {b: '$a'}}}},"
        "{$project: {_id: true, renamed: '$_id.a.b'}},"
        "{$match: {renamed: {$eq: 5 }}}]";
    assertPipelineOptimizesTo(inputPipeline, outputPipeline);
}

TEST(PipelineOptimizationTest, MatchCanMoveAcrossDottedRenameOnNestedGrouping) {
    std::string inputPipeline =
        "[{$group: { _id: { c: '$d', s: '$k' }, c: { $sum: {$const: 1} } } },"
        "{$project: { m: '$_id.c' } },"
        "{$match: { m: {$eq: 2} } }]";
    std::string outputPipeline =
        "[{$match: { d: {$eq: 2} } },"
        "{$group: { _id: { c: '$d', s: '$k' }, c: { $sum: {$const: 1} } } },"
        "{$project: { _id: true, m: '$_id.c' } }]";
    assertPipelineOptimizesTo(inputPipeline, outputPipeline);
}

TEST(PipelineOptimizationTest, MatchLeavingSecondAfterPushingOverProjection) {
    std::string inputPipeline =
        "[{$group: { _id: { c: '$d' }, c: { '$sum': {$const: 1} } } },"
        "{$project: { m1: '$_id.c' } },"
        "{$match: { m1: {$eq: 2}, k: {$eq: 5} } }]";

    std::string outputPipeline =
        "[{$match: { d: {$eq: 2} } },"
        "{$group: { _id: { c: '$d' }, c: { '$sum': {$const: 1} } } },"
        "{$project: { _id: true, m1: '$_id.c' } },"
        "{$match: { k: {$eq: 5} } }]";
    assertPipelineOptimizesTo(inputPipeline, outputPipeline);
}

TEST(PipelineOptimizationTest, PushingOverProjectionWithTail) {
    std::string inputPipeline =
        "[{$group: { _id: { c: '$d' }, c: { '$sum': {$const: 1} } } },"
        "{$project: { m1: '$_id.c' } },"
        "{$match: { m1: {$eq: 2}, k: {$eq: 5} } },"
        "{$project: { m2: '$_id' } } ]";

    std::string outputPipeline =
        "[{$match: { d: {$eq: 2} } },"
        "{$group: { _id: { c: '$d' }, c: { '$sum': {$const: 1} } } },"
        "{$project: { _id: true, m1: '$_id.c' } },"
        "{$match: { k: {$eq: 5} } },"
        "{$project: { _id: true, m2: '$_id' } }]";
    assertPipelineOptimizesTo(inputPipeline, outputPipeline);
}

TEST(PipelineOptimizationTest, PushingDottedMatchOverGrouping) {
    std::string inputPipeline =
        "[{$group: {_id: {a: '$l', b: '$b'}}},"
        "{$match: {'_id.a': 5}}]";

    std::string outputPipeline =
        "[{ $match: { l: { $eq: 5 } } },"
        "{ $group: { _id: { a: '$l', b: '$b' } } }]";
    assertPipelineOptimizesTo(inputPipeline, outputPipeline);
}

TEST(PipelineOptimizationTest, MatchCanMoveAcrossSeveralRenames) {
    std::string inputPipe =
        "[{$project: {c: '$d', _id: false}},"
        "{$addFields: {b: '$c'}},"
        "{$project: {a: '$b', z: 1}},"
        "{$match: {a: 1, z: 2}}]";
    std::string outputPipe =
        "[{$match: {d: {$eq: 1}}},"
        "{$project: {c: '$d', _id: false}},"
        "{$match: {z: {$eq: 2}}},"
        "{$addFields: {b: '$c'}},"
        "{$project: {_id: true, z: true, a: '$b'}}]";
    std::string serializedPipe = R"(
        [{$match: {d : {$eq: 1}}},
         {$project: {c: "$d", _id: false}},
         {$match: {z : {$eq: 2}}},
         {$addFields: {b: "$c"}},
         {$project: {_id: true, z: true, a: "$b"}}])";
    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe, serializedPipe);
}

TEST(PipelineOptimizationTest, RenameShouldNotBeAppliedToDependentMatch) {
    std::string pipeline =
        "[{$project: {x: {$add: ['$foo', '$bar']}, y: '$z', _id: false}},"
        "{$match: {$or: [{x: {$eq: 1}}, {y: {$eq: 1}}]}}]";
    assertPipelineOptimizesTo(pipeline, pipeline);
}

TEST(PipelineOptimizationTest, MatchCannotMoveAcrossAddFieldsRenameOfDottedPath) {
    std::string pipeline = "[{$addFields: {a: '$b.c'}}, {$match: {a: {$eq: 1}}}]";
    assertPipelineOptimizesTo(pipeline, pipeline);
}

TEST(PipelineOptimizationTest, MatchCannotMoveAcrossProjectRenameOfDottedPath) {
    std::string inputPipe =
        "[{$project: {a: '$$CURRENT.b.c', _id: false}}, {$match: {a: {$eq: 1}}}]";
    std::string outputPipe = "[{$project: {a: '$b.c', _id: false}}, {$match: {a: {$eq: 1}}}]";
    assertPipelineOptimizesTo(inputPipe, outputPipe);
}

TEST(PipelineOptimizationTest, MatchWithTypeShouldMoveAcrossRename) {
    std::string inputPipe = "[{$addFields: {a: '$b'}}, {$match: {a: {$type: 4}}}]";
    std::string outputPipe = "[{$match: {b: {$type: [4]}}}, {$addFields: {a: '$b'}}]";
    assertPipelineOptimizesTo(inputPipe, outputPipe);
}

TEST(PipelineOptimizationTest, MatchOnArrayFieldCanSplitAcrossRenameWithMapAndProject) {
    std::string inputPipe =
        "[{$project: {d: {$map: {input: '$a', as: 'iter', in: {e: '$$iter.b', f: {$add: "
        "['$$iter.c', 1]}}}}}}, {$match: {'d.e': 1, 'd.f': 1}}]";
    std::string outputPipe =
        "[{$match: {'a.b': {$eq: 1}}}, {$project: {_id: true, d: {$map: {input: '$a', as: 'iter', "
        "in: {e: '$$iter.b', f: {$add: ['$$iter.c', {$const: 1}]}}}}}}, {$match: {'d.f': {$eq: "
        "1}}}]";
    assertPipelineOptimizesTo(inputPipe, outputPipe);
}

TEST(PipelineOptimizationTest,
     MatchElemMatchValueOnArrayFieldCanSplitAcrossRenameWithMapAndProject) {
    // The $project simply renames 'a.b' & 'a.c' to 'd.e' & 'd.f' and the $match with $elemMatch on
    // the leaf value can be swapped with $project.
    std::string inputPipe = R"(
[
    {
        $project: {
            d: {
                $map: {input: '$a', as: 'iter', in : {e: '$$iter.b', f: '$$iter.c'}}
            }
        }
    },
    {$match: {"d.e": {$elemMatch: {$eq: 1}}, "d.f": {$elemMatch: {$eq: 1}}}}
]
        )";
    std::string outputPipe = R"(
[
    {
        $match: {$and: [{"a.b": {$elemMatch: {$eq: 1}}}, {"a.c": {$elemMatch: {$eq: 1}}}]}
    },
    {
        $project: {
            _id: true,
            d: {
                $map: {input: '$a', as: 'iter', in : {e: '$$iter.b', f: '$$iter.c'}}
            }
        }
    }
]
        )";

    assertPipelineOptimizesTo(inputPipe, outputPipe);
}

// TODO SERVER-74298 The $match can be swapped with $project after renaming.
TEST(PipelineOptimizationTest,
     MatchElemMatchObjectOnArrayFieldCanNotSplitAcrossRenameWithMapAndProject) {
    // The $project simply renames 'a.b' & 'a.c' to 'd.e' & 'd.f' but the dependency tracker reports
    // the 'd' for $elemMatch as a modified dependency and so $match cannot be swapped with
    // $project.
    std::string inputPipe = R"(
[
    {
        $project: {
            d: {
                $map: {input: '$a', as: 'iter', in : {e: '$$iter.b', f: '$$iter.c'}}
            }
        }
    },
    {$match: {d: {$elemMatch: {e: 1, f: 1}}}}
]
        )";
    std::string outputPipe = R"(
[
    {
        $project: {
            _id: true,
            d: {
                $map: {input: "$a", as: "iter", in : {e: "$$iter.b", f: "$$iter.c"}}
            }
        }
    },
    {$match: {d: {$elemMatch: {$and: [{e: {$eq: 1}}, {f: {$eq: 1}}]}}}}
]
        )";
    std::string serializedPipe = R"(
[
    {
        $project: {
            _id: true,
            d: {
                $map: {input: '$a', as: 'iter', in : {e: '$$iter.b', f: '$$iter.c'}}
            }
        }
    },
    {$match: {d: {$elemMatch: {e: 1, f: 1}}}}
]
        )";

    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe, serializedPipe);
}

// TODO SERVER-74298 The $match can be swapped with $project after renaming.
TEST(PipelineOptimizationTest, MatchEqObjectCanNotSplitAcrossRenameWithMapAndProject) {
    // The $project simply renames 'a.b' & 'a.c' to 'd.e' & 'd.f' but the dependency tracker reports
    // the 'd' for $eq as a modified dependency and so $match cannot be swapped with $project.
    std::string inputPipe = R"(
[
    {
        $project: {
            d: {
                $map: {input: '$a', as: 'i', in : {e: '$$i.b', f: '$$i.c'}}
            }
        }
    },
    {$match: {d: {$eq: {e: 1, f: 1}}}}
]
        )";
    std::string outputPipe = R"(
[
    {
        $project: {
            _id: true,
            d: {
                $map: {input: "$a", as: "i", in : {e: "$$i.b", f: "$$i.c"}}
            }
        }
    },
    {$match: {d: {$eq: {e: 1, f: 1}}}}
]
        )";
    std::string serializedPipe = R"(
[
    {
        $project: {
            _id: true,
            d: {
                $map: {input: '$a', as: 'i', in : {e: '$$i.b', f: '$$i.c'}}
            }
        }
    },
    {$match: {d: {$eq: {e: 1, f: 1}}}}
]
        )";

    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe, serializedPipe);
}

TEST(PipelineOptimizationTest, MatchOnArrayFieldCanSplitAcrossRenameWithMapAndAddFields) {
    std::string inputPipe =
        "[{$addFields: {d: {$map: {input: '$a', as: 'iter', in: {e: '$$iter.b', f: {$add: "
        "['$$iter.c', 1]}}}}}}, {$match: {'d.e': 1, 'd.f': 1}}]";
    std::string outputPipe =
        "[{$match: {'a.b': {$eq: 1}}}, {$addFields: {d: {$map: {input: '$a', as: 'iter', in: {e: "
        "'$$iter.b', f: {$add: ['$$iter.c', {$const: 1}]}}}}}}, {$match: {'d.f': {$eq: 1}}}]";
    assertPipelineOptimizesTo(inputPipe, outputPipe);
}

TEST(PipelineOptimizationTest, MatchCannotSwapWithLimit) {
    std::string pipeline = "[{$limit: 3}, {$match: {x: {$gt: 0}}}]";
    assertPipelineOptimizesTo(pipeline, pipeline);
}

TEST(PipelineOptimizationTest, MatchCannotSwapWithSortLimit) {
    std::string inputPipe = "[{$sort: {x: -1}}, {$limit: 3}, {$match: {x: {$gt: 0}}}]";
    std::string outputPipe = "[{$sort: {sortKey: {x: -1}, limit: 3}}, {$match: {x: {$gt: 0}}}]";
    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe, inputPipe);
}

TEST(PipelineOptimizationTest, MatchOnMinItemsShouldSwapSinceCategoryIsArrayMatching) {
    std::string inputPipe =
        "[{$project: {_id: true, a: '$b'}}, "
        "{$match: {a: {$_internalSchemaMinItems: 1}}}]";
    std::string outputPipe =
        "[{$match: {b: {$_internalSchemaMinItems: 1}}}, "
        "{$project: {_id: true, a: '$b'}}]";
    assertPipelineOptimizesTo(inputPipe, outputPipe);

    inputPipe =
        "[{$project: {redacted: false, _id: true}}, "
        "{$match: {a: {$_internalSchemaMinItems: 1}}}]";
    outputPipe =
        "[{$match: {a: {$_internalSchemaMinItems: 1}}}, "
        "{$project: {redacted: false, _id: true}}]";
    assertPipelineOptimizesTo(inputPipe, outputPipe);

    inputPipe =
        "[{$addFields : {a : {$const: 1}}}, "
        "{$match: {b: {$_internalSchemaMinItems: 1}}}]";
    outputPipe =
        "[{$match: {b: {$_internalSchemaMinItems: 1}}}, "
        "{$addFields : {a : {$const: 1}}}]";
    assertPipelineOptimizesTo(inputPipe, outputPipe);
}

TEST(PipelineOptimizationTest, MatchOnMaxItemsShouldSwapSinceCategoryIsArrayMatching) {
    std::string inputPipe =
        "[{$project: {_id: true, a: '$b'}}, "
        "{$match: {a: {$_internalSchemaMaxItems: 1}}}]";
    std::string outputPipe =
        "[{$match: {b: {$_internalSchemaMaxItems: 1}}}, "
        "{$project: {_id: true, a: '$b'}}]";
    assertPipelineOptimizesTo(inputPipe, outputPipe);

    inputPipe =
        "[{$project: {redacted: false, _id: true}}, "
        "{$match: {a: {$_internalSchemaMaxItems: 1}}}]";
    outputPipe =
        "[{$match: {a: {$_internalSchemaMaxItems: 1}}}, "
        "{$project: {redacted: false, _id: true}}]";
    assertPipelineOptimizesTo(inputPipe, outputPipe);

    inputPipe =
        "[{$addFields : {a : {$const: 1}}}, "
        "{$match: {b: {$_internalSchemaMaxItems: 1}}}]";
    outputPipe =
        "[{$match: {b: {$_internalSchemaMaxItems: 1}}}, "
        "{$addFields : {a : {$const: 1}}}]";
    assertPipelineOptimizesTo(inputPipe, outputPipe);
}

TEST(PipelineOptimizationTest, MatchOnAllElemMatchFromIndexShouldNotSwapBecauseOfNamePlaceHolder) {
    std::string inputPipe =
        "[{$project: {_id: true, a: '$b'}}, "
        "{$match: {a: {$_internalSchemaAllElemMatchFromIndex: [1, {b: {$gt: 0}}]}}}]";
    assertPipelineOptimizesTo(inputPipe, inputPipe);

    inputPipe =
        "[{$project: {redacted: false, _id: true}}, "
        "{$match: {a: {$_internalSchemaAllElemMatchFromIndex: [1, {b: {$gt: 0}}]}}}]";
    std::string outputPipe =
        "[{$match: {a: {$_internalSchemaAllElemMatchFromIndex: [1, {b: {$gt: 0}}]}}}, "
        "{$project: {redacted: false, _id: true}}]";
    assertPipelineOptimizesTo(inputPipe, outputPipe);

    inputPipe =
        "[{$addFields : {a : {$const: 1}}}, "
        "{$match: {b: {$_internalSchemaAllElemMatchFromIndex: [1, {b: {$gt: 0}}]}}}]";
    outputPipe =
        "[{$match: {b: {$_internalSchemaAllElemMatchFromIndex: [1, {b: {$gt: 0}}]}}}, "
        "{$addFields : {a : {$const: 1}}}]";
    assertPipelineOptimizesTo(inputPipe, outputPipe);
}

TEST(PipelineOptimizationTest, MatchOnArrayIndexShouldNotSwapBecauseOfNamePlaceHolder) {
    std::string inputPipe = R"(
        [{$project: {_id: true, a: '$b'}},
        {$match: {a: {$_internalSchemaMatchArrayIndex:
           {index: 0, namePlaceholder: 'i', expression: {i: {$lt: 0}}}}}}])";
    assertPipelineOptimizesTo(inputPipe, inputPipe);

    inputPipe = R"(
        [{$project: {redacted: false, _id: true}},
        {$match: {a: {$_internalSchemaMatchArrayIndex:
           {index: 0, namePlaceholder: 'i', expression: {i: {$lt: 0}}}}}}])";
    std::string outputPipe = R"(
        [{$match: {a: {$_internalSchemaMatchArrayIndex:
           {index: 0, namePlaceholder: 'i', expression: {i: {$lt: 0}}}}}},
        {$project: {redacted: false, _id: true}}])";
    assertPipelineOptimizesTo(inputPipe, outputPipe);

    inputPipe = R"(
        [{$addFields : {a : {$const: 1}}},
        {$match: {b: {$_internalSchemaMatchArrayIndex:
           {index: 0, namePlaceholder: 'i', expression: {i: {$lt: 0}}}}}}])";
    outputPipe = R"(
        [{$match: {b: {$_internalSchemaMatchArrayIndex:
           {index: 0, namePlaceholder: 'i', expression: {i: {$lt: 0}}}}}},
        {$addFields : {a : {$const: 1}}}])";
    assertPipelineOptimizesTo(inputPipe, outputPipe);
}

TEST(PipelineOptimizationTest, MatchOnUniqueItemsShouldSwapSinceCategoryIsArrayMatching) {
    std::string inputPipe =
        "[{$project: {_id: true, a: '$b'}}, "
        "{$match: {a: {$_internalSchemaUniqueItems: true}}}]";
    std::string outputPipe =
        "[{$match: {b: {$_internalSchemaUniqueItems: true}}}, "
        "{$project: {_id: true, a: '$b'}}]";
    assertPipelineOptimizesTo(inputPipe, outputPipe);

    inputPipe =
        "[{$project: {redacted: false, _id: true}}, "
        "{$match: {a: {$_internalSchemaUniqueItems: true}}}]";
    outputPipe =
        "[{$match: {a: {$_internalSchemaUniqueItems: true}}}, "
        "{$project: {redacted: false, _id: true}}]";
    assertPipelineOptimizesTo(inputPipe, outputPipe);

    inputPipe =
        "[{$addFields : {a : {$const: 1}}}, "
        "{$match: {b: {$_internalSchemaUniqueItems: true}}}]";
    outputPipe =
        "[{$match: {b: {$_internalSchemaUniqueItems: true}}}, "
        "{$addFields : {a : {$const: 1}}}]";
    assertPipelineOptimizesTo(inputPipe, outputPipe);
}

// Descriptive test. The following internal match expression *could* participate in pipeline
// optimizations, but it currently does not.
TEST(PipelineOptimizationTest, MatchOnObjectMatchShouldNotSwapSinceCategoryIsOther) {
    std::string inputPipe =
        "[{$project: {_id: true, a: '$b'}}, "
        "{$match: {a: {$_internalSchemaObjectMatch: {b: 1}}}}]";
    std::string outputPipe =
        "[{$project: {_id: true, a: '$b'}}, "
        "{$match: {a: {$_internalSchemaObjectMatch: {b: {$eq: 1}}}}}]";
    std::string serializedPipe =
        "[{$project: {_id: true, a: '$b'}},"
        "{$match: {a: {$_internalSchemaObjectMatch: {b: 1}}}}]";

    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe, serializedPipe);

    inputPipe =
        "[{$project: {redacted: false}}, "
        "{$match: {a: {$_internalSchemaObjectMatch: {b: 1}}}}]";
    outputPipe =
        "[{$project: {redacted: false, _id: true}},"
        "{$match: {a: {$_internalSchemaObjectMatch: {b: {$eq: 1}}}}}]";
    serializedPipe =
        "[{$project: {redacted: false, _id: true}},"
        "{$match: {a: {$_internalSchemaObjectMatch: {b: 1}}}}]";

    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe, serializedPipe);

    inputPipe =
        "[{$addFields : {a : {$const: 1}}}, "
        "{$match: {a: {$_internalSchemaObjectMatch: {b: 1}}}}]";
    outputPipe =
        "[{$addFields : {a : {$const: 1}}}, "
        "{$match: {a: {$_internalSchemaObjectMatch: {b: {$eq: 1}}}}}]";
    serializedPipe =
        "[{$addFields : {a : {$const: 1}}}, "
        "{$match: {a: {$_internalSchemaObjectMatch: {b: 1}}}}]";

    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe, serializedPipe);
}

// Descriptive test. The following internal match expression *could* participate in pipeline
// optimizations, but it currently does not.
TEST(PipelineOptimizationTest, MatchOnMinPropertiesShouldNotSwapSinceCategoryIsOther) {
    std::string inputPipe =
        "[{$project: {_id: true, a: '$b'}}, "
        "{$match: {$_internalSchemaMinProperties: 2}}]";
    assertPipelineOptimizesTo(inputPipe, inputPipe);

    inputPipe =
        "[{$project: {redacted: false, _id: true}}, "
        "{$match: {$_internalSchemaMinProperties: 2}}]";
    assertPipelineOptimizesTo(inputPipe, inputPipe);

    inputPipe =
        "[{$addFields : {a : {$const: 1}}}, "
        "{$match: {$_internalSchemaMinProperties: 2}}]";
    assertPipelineOptimizesTo(inputPipe, inputPipe);
}

// Descriptive test. The following internal match expression *could* participate in pipeline
// optimizations, but it currently does not.
TEST(PipelineOptimizationTest, MatchOnMaxPropertiesShouldNotSwapSinceCategoryIsOther) {
    std::string inputPipe =
        "[{$project: {_id: true, a: '$b'}}, "
        "{$match: {$_internalSchemaMaxProperties: 2}}]";
    assertPipelineOptimizesTo(inputPipe, inputPipe);

    inputPipe =
        "[{$project: {redacted: false, _id: true}}, "
        "{$match: {$_internalSchemaMaxProperties: 2}}]";
    assertPipelineOptimizesTo(inputPipe, inputPipe);

    inputPipe =
        "[{$addFields : {a : {$const: 1}}}, "
        "{$match: {$_internalSchemaMaxProperties: 2}}]";
    assertPipelineOptimizesTo(inputPipe, inputPipe);
}

// Descriptive test. The following internal match expression *could* participate in pipeline
// optimizations, but it currently does not.
TEST(PipelineOptimizationTest, MatchOnAllowedPropertiesShouldNotSwapSinceCategoryIsOther) {
    std::string inputPipe = R"(
        [{$project: {_id: true, a: '$b'}},
        {$match: {$_internalSchemaAllowedProperties: {
            properties: ['b'],
            namePlaceholder: 'i',
            patternProperties: [],
            otherwise: {i: 1}
        }}}])";
    std::string outputPipe = R"(
        [{$project: {_id: true, a: '$b'}},
        {$match: {$_internalSchemaAllowedProperties: {
            properties: ['b'],
            namePlaceholder: 'i',
            patternProperties: [],
            otherwise: {i: {$eq : 1}}
        }}}])";
    std::string serializedPipe = R"(
        [{$project: {_id: true, a: '$b'}},
        {$match: {$_internalSchemaAllowedProperties: {
            properties: ['b'],
            namePlaceholder: 'i',
            patternProperties: [],
            otherwise: {i : 1}}
        }}])";
    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe, serializedPipe);

    inputPipe = R"(
        [{$project: {redacted: false}},
        {$match: {$_internalSchemaAllowedProperties: {
            properties: ['b'],
            namePlaceholder: 'i',
            patternProperties: [],
            otherwise: {i: 1}
        }}}])";
    outputPipe = R"(
        [{$project: {redacted: false, _id: true}},
        {$match: {$_internalSchemaAllowedProperties: {
            properties: ['b'],
            namePlaceholder: 'i',
            patternProperties: [],
            otherwise: {i: {$eq: 1}
        }}}}])";
    serializedPipe = R"(
        [{$project: {redacted: false, _id: true}},
        {$match: {$_internalSchemaAllowedProperties: {
            properties: ['b'],
            namePlaceholder: 'i',
            patternProperties: [],
            otherwise: {i: 1}
        }}}])";
    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe, serializedPipe);

    inputPipe = R"(
        [{$addFields : {a : {$const: 1}}},
        {$match: {$_internalSchemaAllowedProperties: {
            properties: ['b'],
            namePlaceholder: 'i',
            patternProperties: [],
            otherwise: {i: 1}
        }}}])";
    outputPipe = R"(
        [{$addFields: {a: {$const: 1}}},
        {$match: {$_internalSchemaAllowedProperties: {
            properties: ["b"],
            namePlaceholder: "i",
            patternProperties: [],
            otherwise: {i: {$eq: 1}
        }}}}])";
    serializedPipe = R"(
        [{$addFields : {a : {$const: 1}}},
        {$match: {$_internalSchemaAllowedProperties: {
            properties: ['b'],
            namePlaceholder: 'i',
            patternProperties: [],
            otherwise: {i: 1}
        }}}])";
    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe, serializedPipe);
}

// Descriptive test. The following internal match expression *could* participate in pipeline
// optimizations, but it currently does not.
TEST(PipelineOptimizationTest, MatchOnCondShouldNotSwapSinceCategoryIsOther) {
    std::string inputPipe =
        "[{$project: {_id: true, a: '$b'}}, "
        "{$match: {$_internalSchemaCond: [{a: 1}, {b: 1}, {c: 1}]}}]";
    std::string outputPipe =
        "[{$project: {_id: true, a: '$b'}}, "
        "{$match: {$_internalSchemaCond: [{a: {$eq : 1}}, {b: {$eq : 1}}, {c: {$eq : 1}}]}}]";
    std::string serializedPipe =
        "[{$project: {_id: true, a: '$b'}}, "
        "{$match: {$_internalSchemaCond: [{a: 1}, {b: 1}, {c: 1}]}}]";
    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe, serializedPipe);

    inputPipe =
        "[{$project: {redacted: false}}, "
        "{$match: {$_internalSchemaCond: [{a: 1}, {b: 1}, {c: 1}]}}]";
    outputPipe =
        "[{$project: {redacted: false, _id: true}}, "
        "{$match: {$_internalSchemaCond: [{a: {$eq : 1}}, {b: {$eq: 1}}, {c: {$eq: 1}}]}}]";
    serializedPipe =
        "[{$project: {redacted: false, _id: true}}, "
        "{$match: {$_internalSchemaCond: [{a: 1}, {b: 1}, {c: 1}]}}]";
    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe, serializedPipe);

    inputPipe =
        "[{$addFields : {a : {$const: 1}}}, "
        "{$match: {$_internalSchemaCond: [{a: 1}, {b: 1}, {c: 1}]}}]";
    outputPipe =
        "[{$addFields : {a : {$const: 1}}}, "
        "{$match: {$_internalSchemaCond: [{a: {$eq : 1}}, {b: {$eq: 1}}, {c: {$eq : 1}}]}}]";
    serializedPipe =
        "[{$addFields : {a : {$const: 1}}}, "
        "{$match: {$_internalSchemaCond: [{a: 1}, {b: 1}, {c: 1}]}}]";
    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe, serializedPipe);
}

// Descriptive test. The following internal match expression *could* participate in pipeline
// optimizations, but it currently does not.
TEST(PipelineOptimizationTest, MatchOnRootDocEqShouldNotSwapSinceCategoryIsOther) {
    std::string inputPipe =
        "[{$project: {_id: true, a: '$b'}}, "
        "{$match: {$_internalSchemaRootDocEq: {a: 1}}}]";
    assertPipelineOptimizesTo(inputPipe, inputPipe);

    inputPipe =
        "[{$project: {redacted: false, _id: true}}, "
        "{$match: {$_internalSchemaRootDocEq: {a: 1}}}]";
    assertPipelineOptimizesTo(inputPipe, inputPipe);

    inputPipe =
        "[{$addFields : {a : {$const: 1}}}, "
        "{$match: {$_internalSchemaRootDocEq: {a: 1}}}]";
    assertPipelineOptimizesTo(inputPipe, inputPipe);
}

// Descriptive test. The following internal match expression can participate in pipeline
// optimizations.
TEST(PipelineOptimizationTest, MatchOnInternalSchemaTypeShouldSwap) {
    std::string inputPipe =
        "[{$project: {_id: true, a: '$b'}}, "
        "{$match: {a: {$_internalSchemaType: 1}}}]";
    std::string outputPipe =
        "[{$match: {b: {$_internalSchemaType: [1]}}}, "
        "{$project: {_id: true, a: '$b'}}]";
    std::string serializedPipe =
        "[{$match: {b: {$_internalSchemaType: [1]}}}, "
        "{$project: {_id: true, a: '$b'}}]";
    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe, serializedPipe);

    inputPipe =
        "[{$project: {redacted: false}}, "
        "{$match: {a: {$_internalSchemaType: 1}}}]";
    outputPipe =
        "[{$match: {a: {$_internalSchemaType: [1]}}}, "
        "{$project: {redacted: false, _id: true}}]";
    serializedPipe =
        "[{$match: {a: {$_internalSchemaType: 1}}}, "
        "{$project: {redacted: false, _id: true}}]";
    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe, serializedPipe);

    inputPipe =
        "[{$addFields : {a : {$const: 1}}}, "
        "{$match: {b: {$_internalSchemaType: 1}}}]";
    outputPipe =
        "[{$match: {b: {$_internalSchemaType: [1]}}}, "
        "{$addFields : {a : {$const: 1}}}]";
    serializedPipe =
        "[{$match: {b: {$_internalSchemaType: 1}}}, "
        "{$addFields : {a : {$const: 1}}}]";
    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe, serializedPipe);
}

TEST(PipelineOptimizationTest, MatchOnMinLengthShouldSwapWithAdjacentStage) {
    std::string inputPipe =
        "[{$project: {_id: true, a: '$b'}}, "
        "{$match: {a: {$_internalSchemaMinLength: 1}}}]";
    std::string outputPipe =
        "[{$match: {b: {$_internalSchemaMinLength: 1}}},"
        "{$project: {_id: true, a: '$b'}}]";
    assertPipelineOptimizesTo(inputPipe, outputPipe);

    inputPipe =
        "[{$project: {redacted: false}}, "
        "{$match: {a: {$_internalSchemaMinLength: 1}}}]";
    outputPipe =
        "[{$match: {a: {$_internalSchemaMinLength: 1}}},"
        "{$project: {redacted: false, _id: true}}]";
    assertPipelineOptimizesTo(inputPipe, outputPipe);

    inputPipe =
        "[{$addFields : {a : {$const: 1}}}, "
        "{$match: {b: {$_internalSchemaMinLength: 1}}}]";
    outputPipe =
        "[{$match: {b: {$_internalSchemaMinLength: 1}}},"
        "{$addFields: {a: {$const: 1}}}]";
    assertPipelineOptimizesTo(inputPipe, outputPipe);
}

TEST(PipelineOptimizationTest, MatchOnMaxLengthShouldSwapWithAdjacentStage) {
    std::string inputPipe =
        "[{$project: {_id: true, a: '$b'}}, "
        "{$match: {a: {$_internalSchemaMaxLength: 1}}}]";
    std::string outputPipe =
        "[{$match: {b: {$_internalSchemaMaxLength: 1}}},"
        "{$project: {_id: true, a: '$b'}}]";
    assertPipelineOptimizesTo(inputPipe, outputPipe);

    inputPipe =
        "[{$project: {redacted: false}}, "
        "{$match: {a: {$_internalSchemaMaxLength: 1}}}]";
    outputPipe =
        "[{$match: {a: {$_internalSchemaMaxLength: 1}}}, "
        "{$project: {redacted: false, _id: true}}]";
    assertPipelineOptimizesTo(inputPipe, outputPipe);

    inputPipe =
        "[{$addFields : {a : {$const: 1}}}, "
        "{$match: {b: {$_internalSchemaMaxLength: 1}}}]";
    outputPipe =
        "[{$match: {b: {$_internalSchemaMaxLength: 1}}}, "
        "{$addFields: {a: {$const: 1}}}]";
    assertPipelineOptimizesTo(inputPipe, outputPipe);
}

TEST(PipelineOptimizationTest, MatchOnInternalEqShouldSwapWithAdjacentStage) {
    std::string inputPipe =
        "[{$project: {_id: true, a: '$b'}}, "
        "{$match: {a: {$_internalSchemaEq: {c: 1}}}}]";
    std::string outputPipe =
        "[{$match: {b: {$_internalSchemaEq: {c: 1}}}}, "
        "{$project: {_id: true, a: '$b'}}]";
    assertPipelineOptimizesTo(inputPipe, outputPipe);

    inputPipe =
        "[{$project: {redacted: false, _id: true}}, "
        "{$match: {a: {$_internalSchemaEq: {c: 1}}}}]";
    outputPipe =
        "[{$match: {a: {$_internalSchemaEq: {c: 1}}}}, "
        "{$project: {redacted: false, _id: true}}]";
    assertPipelineOptimizesTo(inputPipe, outputPipe);

    inputPipe =
        "[{$addFields : {a : {$const: 1}}}, "
        "{$match: {b: {$_internalSchemaEq: {c: 1}}}}]";
    outputPipe =
        "[{$match: {b: {$_internalSchemaEq: {c: 1}}}}, "
        "{$addFields: {a: {$const: 1}}}]";
    assertPipelineOptimizesTo(inputPipe, outputPipe);
}

TEST(PipelineOptimizationTest, MatchOnXorShouldSwapIfEverySubExpressionIsEligible) {
    std::string inputPipe =
        "[{$project: {_id: true, a: '$b', c: '$d'}}, "
        "{$match: {$_internalSchemaXor: [{a: 1}, {c: 1}]}}]";
    std::string outputPipe =
        "[{$match: {$_internalSchemaXor: [{b: {$eq: 1}}, {d: {$eq: 1}}]}}, "
        "{$project: {_id: true, a: '$b', c: '$d'}}]";
    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe, outputPipe);

    inputPipe =
        "[{$project: {redacted: false}}, "
        "{$match: {$_internalSchemaXor: [{a: 1}, {b: 1}]}}]";
    outputPipe =
        "[{$match: {$_internalSchemaXor: [{a: {$eq : 1}}, {b: {$eq : 1}}]}}, "
        "{$project: {redacted: false, _id: true}}]";
    std::string serializedPipe =
        "[{$match: {$_internalSchemaXor: [{a: 1}, {b: 1}]}}, "
        " {$project: {redacted: false, _id: true}}]";
    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe, serializedPipe);

    inputPipe =
        "[{$addFields : {a : {$const: 1}}}, "
        "{$match: {$_internalSchemaXor: [{b: 1}, {c: 1}]}}]";
    outputPipe =
        "[{$match: {$_internalSchemaXor: [{b: {$eq: 1}}, {c: {$eq: 1}}]}}, "
        "{$addFields: {a: {$const: 1}}}]";
    serializedPipe =
        "[{$match: {$_internalSchemaXor: [{b: 1}, {c: 1}]}}, "
        "{$addFields : {a : {$const: 1}}}]";
    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe, serializedPipe);

    inputPipe =
        "[{$addFields : {a : {$const: 1}}}, "
        "{$match: {$_internalSchemaXor: [{b: 1}, {a: 1}]}}]";
    outputPipe =
        "[{$addFields: {a: {$const: 1}}}, "
        "{$match: {$_internalSchemaXor: [{a: {$eq: 1}}, {b: {$eq: 1}}]}}]";
    serializedPipe =
        "[{$addFields: {a: {$const: 1}}}, "
        "{$match: {$_internalSchemaXor: [{b: 1}, {a: 1}]}}]";
    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe, serializedPipe);
}

TEST(PipelineOptimizationTest, MatchOnFmodShouldSwapWithAdjacentStage) {
    std::string inputPipe =
        "[{$project: {_id: true, a: '$b'}}, "
        "{$match: {a: {$_internalSchemaFmod: [5, 0]}}}]";
    std::string outputPipe =
        "[{$match: {b: {$_internalSchemaFmod: [5, 0]}}}, "
        "{$project: {_id: true, a: '$b'}}]";
    assertPipelineOptimizesTo(inputPipe, outputPipe);

    inputPipe =
        "[{$project: {redacted: false, _id: true}}, "
        "{$match: {a: {$_internalSchemaFmod: [5, 0]}}}]";
    outputPipe =
        "[{$match: {a: {$_internalSchemaFmod: [5, 0]}}}, "
        "{$project: {redacted: false, _id: true}}]";
    assertPipelineOptimizesTo(inputPipe, outputPipe);

    inputPipe =
        "[{$addFields : {a : {$const: 1}}}, "
        "{$match: {b: {$_internalSchemaFmod: [5, 0]}}}]";
    outputPipe =
        "[{$match: {b: {$_internalSchemaFmod: [5, 0]}}}, "
        "{$addFields: {a: {$const: 1}}}]";
    assertPipelineOptimizesTo(inputPipe, outputPipe);
}

TEST(PipelineOptimizationTest, ChangeStreamLookupSwapsWithIndependentMatch) {
    QueryTestServiceContext testServiceContext;
    auto opCtx = testServiceContext.makeOperationContext();

    boost::intrusive_ptr<ExpressionContext> expCtx(new ExpressionContextForTest(kTestNss));
    expCtx->opCtx = opCtx.get();
    expCtx->uuid = UUID::gen();
    setMockReplicationCoordinatorOnOpCtx(expCtx->opCtx);

    // We enable the 'showExpandedEvents' flag to avoid injecting an additional $match stage which
    // filters out newly added events.
    auto spec = BSON("$changeStream" << BSON(
                         "fullDocument"
                         << "updateLookup"
                         << DocumentSourceChangeStreamSpec::kShowExpandedEventsFieldName << true));
    auto stages = DocumentSourceChangeStream::createFromBson(spec.firstElement(), expCtx);
    ASSERT_EQ(stages.size(), getChangeStreamStageSize());
    // Make sure the change lookup is at the end.
    ASSERT(dynamic_cast<DocumentSourceChangeStreamAddPostImage*>(stages.back().get()));

    auto matchPredicate = BSON("extra"
                               << "predicate");
    stages.push_back(DocumentSourceMatch::create(matchPredicate, expCtx));
    auto pipeline = Pipeline::create(stages, expCtx);
    pipeline->optimizePipeline();

    // Make sure the $match stage has swapped before the change look up.
    ASSERT(
        dynamic_cast<DocumentSourceChangeStreamAddPostImage*>(pipeline->getSources().back().get()));
}

TEST(PipelineOptimizationTest, ChangeStreamLookupDoesNotSwapWithMatchOnPostImage) {
    QueryTestServiceContext testServiceContext;
    auto opCtx = testServiceContext.makeOperationContext();

    boost::intrusive_ptr<ExpressionContext> expCtx(new ExpressionContextForTest(kTestNss));
    expCtx->opCtx = opCtx.get();
    expCtx->uuid = UUID::gen();
    setMockReplicationCoordinatorOnOpCtx(expCtx->opCtx);

    // We enable the 'showExpandedEvents' flag to avoid injecting an additional $match stage which
    // filters out newly added events.
    auto spec = BSON("$changeStream" << BSON(
                         "fullDocument"
                         << "updateLookup"
                         << DocumentSourceChangeStreamSpec::kShowExpandedEventsFieldName << true));
    auto stages = DocumentSourceChangeStream::createFromBson(spec.firstElement(), expCtx);
    ASSERT_EQ(stages.size(), getChangeStreamStageSize());
    // Make sure the change lookup is at the end.
    ASSERT(dynamic_cast<DocumentSourceChangeStreamAddPostImage*>(stages.back().get()));

    stages.push_back(DocumentSourceMatch::create(
        BSON(DocumentSourceChangeStreamAddPostImage::kFullDocumentFieldName << BSONNULL), expCtx));
    auto pipeline = Pipeline::create(stages, expCtx);
    pipeline->optimizePipeline();

    // Make sure the $match stage stays at the end.
    ASSERT(dynamic_cast<DocumentSourceMatch*>(pipeline->getSources().back().get()));
}

TEST(PipelineOptimizationTest, FullDocumentBeforeChangeLookupSwapsWithIndependentMatch) {
    QueryTestServiceContext testServiceContext;
    auto opCtx = testServiceContext.makeOperationContext();

    boost::intrusive_ptr<ExpressionContext> expCtx(new ExpressionContextForTest(kTestNss));
    expCtx->opCtx = opCtx.get();
    expCtx->uuid = UUID::gen();
    setMockReplicationCoordinatorOnOpCtx(expCtx->opCtx);

    // We enable the 'showExpandedEvents' flag to avoid injecting an additional $match stage which
    // filters out newly added events.
    auto spec = BSON("$changeStream" << BSON(
                         "fullDocumentBeforeChange"
                         << "required"
                         << DocumentSourceChangeStreamSpec::kShowExpandedEventsFieldName << true));
    auto stages = DocumentSourceChangeStream::createFromBson(spec.firstElement(), expCtx);
    ASSERT_EQ(stages.size(), getChangeStreamStageSize());
    // Make sure the pre-image lookup is at the end.
    ASSERT(dynamic_cast<DocumentSourceChangeStreamAddPreImage*>(stages.back().get()));

    auto matchPredicate = BSON("extra"
                               << "predicate");
    stages.push_back(DocumentSourceMatch::create(matchPredicate, expCtx));
    auto pipeline = Pipeline::create(stages, expCtx);
    pipeline->optimizePipeline();

    // Make sure the $match stage has swapped before the change look up.
    ASSERT(
        dynamic_cast<DocumentSourceChangeStreamAddPreImage*>(pipeline->getSources().back().get()));
}

TEST(PipelineOptimizationTest, FullDocumentBeforeChangeDoesNotSwapWithMatchOnPreImage) {
    QueryTestServiceContext testServiceContext;
    auto opCtx = testServiceContext.makeOperationContext();

    boost::intrusive_ptr<ExpressionContext> expCtx(new ExpressionContextForTest(kTestNss));
    expCtx->opCtx = opCtx.get();
    expCtx->uuid = UUID::gen();
    setMockReplicationCoordinatorOnOpCtx(expCtx->opCtx);

    // We enable the 'showExpandedEvents' flag to avoid injecting an additional $match stage which
    // filters out newly added events.
    auto spec = BSON("$changeStream" << BSON(
                         "fullDocumentBeforeChange"
                         << "required"
                         << DocumentSourceChangeStreamSpec::kShowExpandedEventsFieldName << true));
    auto stages = DocumentSourceChangeStream::createFromBson(spec.firstElement(), expCtx);
    ASSERT_EQ(stages.size(), getChangeStreamStageSize());
    // Make sure the pre-image lookup is at the end.
    ASSERT(dynamic_cast<DocumentSourceChangeStreamAddPreImage*>(stages.back().get()));

    stages.push_back(DocumentSourceMatch::create(
        BSON(DocumentSourceChangeStreamAddPreImage::kFullDocumentBeforeChangeFieldName << BSONNULL),
        expCtx));
    auto pipeline = Pipeline::create(stages, expCtx);
    pipeline->optimizePipeline();

    // Make sure the $match stage stays at the end.
    ASSERT(dynamic_cast<DocumentSourceMatch*>(pipeline->getSources().back().get()));
}

TEST(PipelineOptimizationTest, ChangeStreamHandleTopologyChangeSwapsWithRedact) {
    QueryTestServiceContext testServiceContext;
    auto opCtx = testServiceContext.makeOperationContext();

    boost::intrusive_ptr<ExpressionContext> expCtx(new ExpressionContextForTest(kTestNss));
    expCtx->opCtx = opCtx.get();
    expCtx->uuid = UUID::gen();
    expCtx->inMongos = true;  // To enforce the $_internalChangeStreamHandleTopologyChange stage.
    setMockReplicationCoordinatorOnOpCtx(expCtx->opCtx);

    auto stages = DocumentSourceChangeStream::createFromBson(
        fromjson("{$changeStream: {showExpandedEvents: true}}").firstElement(), expCtx);

    // Assert that the last stage is $_internalChangeStreamHandleTopologyChange.
    ASSERT(dynamic_cast<DocumentSourceChangeStreamHandleTopologyChange*>(stages.back().get()));

    // Add $redact as the last stage.
    stages.push_back(DocumentSourceRedact::createFromBson(
        fromjson("{$redact: '$$PRUNE'}").firstElement(), expCtx));

    auto pipeline = Pipeline::create(stages, expCtx);
    pipeline->optimizePipeline();

    // Assert that $redact swaps with $_internalChangeStreamHandleTopologyChange after optimization.
    ASSERT(dynamic_cast<DocumentSourceRedact*>(
        std::prev(std::prev(pipeline->getSources().end()))->get()));
    ASSERT(dynamic_cast<DocumentSourceChangeStreamHandleTopologyChange*>(
        pipeline->getSources().back().get()));
}

TEST(PipelineOptimizationTest, SortLimProjLimBecomesTopKSortProj) {
    std::string inputPipe =
        "[{$sort: {a: 1}}"
        ",{$limit: 7}"
        ",{$project : {a: 1}}"
        ",{$limit: 5}"
        "]";

    std::string outputPipe =
        "[{$sort: {sortKey: {a: 1}, limit: 5}}"
        ",{$project: {_id: true, a: true}}"
        "]";

    std::string serializedPipe =
        "[{$sort: {a: 1}}"
        ",{$limit: 5}"
        ",{$project : {_id: true, a: true}}"
        "]";

    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe, serializedPipe);
}

TEST(PipelineOptimizationTest, SortProjUnwindLimLimBecomesSortProjUnwindLim) {
    std::string inputPipe =
        "[{$sort: {a: 1}}"
        ",{$project : {a: 1}}"
        ",{$unwind: {path: '$a'}}"
        ",{$limit: 7}"
        ",{$limit: 5}"
        "]";

    std::string outputPipe =
        "[{$sort: {sortKey: {a: 1}}}"
        ",{$project: {_id: true, a: true}}"
        ",{$unwind: {path: '$a'}}"
        ",{$limit: 5}"
        "]";

    std::string serializedPipe =
        "[{$sort: {a: 1}}"
        ",{$project : {_id: true, a: true}}"
        ",{$unwind: {path: '$a'}}"
        ",{$limit: 5}"
        "]";

    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe, serializedPipe);
}

TEST(PipelineOptimizationTest, SortSkipLimBecomesTopKSortSkip) {
    std::string inputPipe =
        "[{$sort: {a: 1}}"
        ",{$skip: 2}"
        ",{$limit: 5}"
        "]";

    std::string outputPipe =
        "[{$sort: {sortKey: {a: 1}, limit: 7}}"
        ",{$skip: 2}"
        "]";

    std::string serializedPipe =
        "[{$sort: {a: 1}}"
        ",{$limit: 7}"
        ",{$skip: 2}"
        "]";

    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe, serializedPipe);
}

TEST(PipelineOptimizationTest, LimDoesNotCoalesceWithSortInSortProjGroupLim) {
    std::string inputPipe =
        "[{$sort: {a: 1}}"
        ",{$project : {a: 1}}"
        ",{$group: {_id: '$a'}}"
        ",{$limit: 5}"
        "]";

    std::string outputPipe =
        "[{$sort: {sortKey: {a: 1}}}"
        ",{$project: {_id: true, a: true}}"
        ",{$group: {_id: '$a'}}"
        ",{$limit: 5}"
        "]";

    std::string serializedPipe =
        "[{$sort: {a: 1}}"
        ",{$project : {_id: true, a: true}}"
        ",{$group: {_id: '$a'}}"
        ",{$limit: 5}"
        "]";

    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe, serializedPipe);
}

TEST(PipelineOptimizationTest, SortProjSkipLimBecomesTopKSortSkipProj) {
    std::string inputPipe =
        "[{$sort: {a: 1}}"
        ",{$project : {a: 1}}"
        ",{$skip: 3}"
        ",{$limit: 5}"
        "]";

    std::string outputPipe =
        "[{$sort: {sortKey: {a: 1}, limit: 8}}"
        ",{$skip: 3}"
        ",{$project: {_id: true, a: true}}"
        "]";

    std::string serializedPipe =
        "[{$sort: {a: 1}}"
        ",{$limit: 8}"
        ",{$skip: 3}"
        ",{$project : {_id: true, a: true}}"
        "]";

    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe, serializedPipe);
}

TEST(PipelineOptimizationTest, SortSkipProjSkipLimSkipLimBecomesTopKSortSkipProj) {
    std::string inputPipe =
        "[{$sort: {a: 1}}"
        ",{$skip: 2}"
        ",{$project : {a: 1}}"
        ",{$skip: 4}"
        ",{$limit: 25}"
        ",{$skip: 6}"
        ",{$limit: 3}"
        "]";

    std::string outputPipe =
        "[{$sort: {sortKey: {a: 1}, limit: 15}}"
        ",{$skip: 12}"
        ",{$project: {_id: true, a: true}}"
        "]";

    std::string serializedPipe =
        "[{$sort: {a: 1}}"
        ",{$limit: 15}"
        ",{$skip: 12}"
        ",{$project : {_id: true, a: true}}"
        "]";

    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe, serializedPipe);
}

TEST(PipelineOptimizationTest, MatchGetsPushedIntoBothChildrenOfUnion) {
    assertPipelineOptimizesTo(
        "["
        " {$unionWith: 'unionColl'},"
        " {$match: {x: {$eq: 2}}}"
        "]",
        "[{$match: {x: {$eq: 2}}},"
        " {$unionWith: {"
        "   coll: 'unionColl',"
        "   pipeline: [{$match: {x: {$eq: 2}}}]"
        " }}]");

    // Test that the $match can get pulled forward through other stages.
    assertPipelineOptimizesAndSerializesTo(
        "["
        " {$unionWith: 'unionColl'},"
        " {$lookup: {from: 'lookupColl', as: 'y', localField: 'z', foreignField: 'z'}},"
        " {$sort: {score: 1}},"
        " {$match: {x: {$eq: 2}}}"
        "]",
        "["
        " {$match: {x: {$eq: 2}}},"
        " {$unionWith: {"
        "   coll: 'unionColl',"
        "   pipeline: [{$match: {x: {$eq: 2}}}]"
        " }},"
        " {$sort: {sortKey: {score: 1}}},"
        " {$lookup: {from: 'lookupColl', as: 'y', localField: 'z', foreignField: 'z'}}"
        "]",
        "["
        " {$match: {x: {$eq: 2}}},"
        " {$unionWith: {"
        "   coll: 'unionColl',"
        "   pipeline: [{$match: {x: {$eq: 2}}}]"
        " }},"
        " {$sort: {score: 1}},"
        " {$lookup: {from: 'lookupColl', as: 'y', localField: 'z', foreignField: 'z'}}"
        "]");

    // Test that the $match can get pulled forward from after the $unionWith to inside, then to the
    // beginning of a $unionWith subpipeline.
    assertPipelineOptimizesAndSerializesTo(
        "["
        " {$unionWith: {"
        "    coll: 'unionColl',"
        "    pipeline: ["
        "      {$project: {y: false}},"
        "      {$sort: {score: 1}}"
        "    ]"
        " }},"
        " {$match: {x: {$eq: 2}}}"
        "]",
        "["
        " {$match: {x: {$eq: 2}}},"
        " {$unionWith: {"
        "    coll: 'unionColl',"
        "    pipeline: ["
        "      {$match: {x: {$eq: 2}}},"
        "      {$project: {y: false, _id: true}},"
        "      {$sort: {sortKey: {score: 1}}}"
        "    ]"
        " }}"
        "]",
        "["
        " {$match: {x: {$eq: 2}}},"
        " {$unionWith: {"
        "    coll: 'unionColl',"
        "    pipeline: ["
        "      {$match: {x: {$eq: 2}}},"
        "      {$project: {y: false, _id: true}},"
        "      {$sort: {score: 1}}"
        "    ]"
        " }}"
        "]");
}

TEST(PipelineOptimizationTest, internalAllCollectionStatsAbsorbsMatchOnNs) {
    std::string inputPipe =
        "["
        " {$_internalAllCollectionStats: {}},"
        " {$match: {ns: 'test.foo', a: 10}}"
        "]";
    std::string outputPipe =
        "["
        " {$_internalAllCollectionStats: {match: {ns: {$eq: 'test.foo'}}}},"
        " {$match: {a: {$eq: 10}}}"
        "]";
    std::string serializedPipe =
        "["
        " {$_internalAllCollectionStats: {}},"
        " {$match: {ns: {$eq: 'test.foo'}}},"
        " {$match: {a: {$eq: 10}}}"
        "]";
    assertPipelineOptimizesAndSerializesTo(
        inputPipe, outputPipe, serializedPipe, kAdminCollectionlessNss);
}

TEST(PipelineOptimizationTest, internalAllCollectionStatsAbsorbsSeveralMatchesOnNs) {
    std::string inputPipe =
        "["
        " {$_internalAllCollectionStats: {}},"
        " {$match: {ns: {$gt: 0}}},"
        " {$match: {a: 10}},"
        " {$match: {ns: {$ne: 5}}}"
        "]";
    std::string outputPipe =
        "["
        " {$_internalAllCollectionStats: {match: {$and: [{ns: {$gt: 0}}, {ns: {$not: {$eq: "
        "5}}}]}}},"
        " {$match: {a: {$eq: 10}}}"
        "]";
    std::string serializedPipe =
        "["
        " {$_internalAllCollectionStats: {}},"
        " {$match: {$and: [{ns: {$gt: 0}}, {ns: {$not: {$eq: 5}}}]}},"
        " {$match: {a: {$eq: 10}}}"
        "]";
    assertPipelineOptimizesAndSerializesTo(
        inputPipe, outputPipe, serializedPipe, kAdminCollectionlessNss);
}

TEST(PipelineOptimizationTest, internalAllCollectionStatsDoesNotAbsorbMatchNotOnNs) {
    std::string inputPipe =
        "["
        " {$_internalAllCollectionStats: {}},"
        " {$match: {a: 10}}"
        "]";
    std::string outputPipe =
        "["
        " {$_internalAllCollectionStats: {}},"
        " {$match: {a: {$eq: 10}}}"
        "]";
    std::string serializedPipe =
        "["
        " {$_internalAllCollectionStats: {}},"
        " {$match: {a: 10}}"
        "]";
    assertPipelineOptimizesAndSerializesTo(
        inputPipe, outputPipe, serializedPipe, kAdminCollectionlessNss);
}

TEST(PipelineOptimizationTest, ProjectGetsPushedIntoBothChildrenOfUnion) {
    assertPipelineOptimizesTo(
        "["
        " {$unionWith: 'unionColl'},"
        " {$project: {x: false}}"
        "]",
        "[{$project: {x: false, _id: true}},"
        " {$unionWith: {"
        "   coll: 'unionColl',"
        "   pipeline: [{$project: {x: false, _id: true}}]"
        " }}]");

    // Test an inclusion projection.
    assertPipelineOptimizesTo(
        "["
        " {$unionWith: 'unionColl'},"
        " {$project: {x: true}}"
        "]",
        "[{$project: {_id: true, x: true}},"
        " {$unionWith: {"
        "   coll: 'unionColl',"
        "   pipeline: [{$project: {_id: true, x: true}}]"
        " }}]");

    // Test a $set.
    assertPipelineOptimizesTo(
        "["
        " {$unionWith: 'unionColl'},"
        " {$set: {x: 'new value'}}"
        "]",
        "[{$set: {x: {$const: 'new value'}}},"
        " {$unionWith: {"
        "   coll: 'unionColl',"
        "   pipeline: [{$set: {x: {$const: 'new value'}}}]"
        " }}]");
}

TEST(PipelineOptimizationTest, UnionWithViewsSampleUseCase) {
    // Test that if someone uses $unionWith to query one logical collection from four physical
    // collections then the query and projection can get pushed down to next to each collection
    // access.
    assertPipelineOptimizesTo(
        "["
        " {$unionWith: 'unionColl'},"
        " {$unionWith: 'unionColl'},"
        " {$unionWith: 'unionColl'},"
        " {$match: {business: {$eq: 'good'}}},"
        " {$project: {_id: true, x: true}}"
        "]",
        "[{$match: {business: {$eq: 'good'}}},"
        " {$project: {_id: true, x: true}},"
        " {$unionWith: {"
        "   coll: 'unionColl',"
        "   pipeline: ["
        "     {$match: {business: {$eq: 'good'}}},"
        "     {$project: {_id: true, x: true}}"
        "   ]"
        " }},"
        " {$unionWith: {"
        "   coll: 'unionColl',"
        "   pipeline: ["
        "     {$match: {business: {$eq: 'good'}}},"
        "     {$project: {_id: true, x: true}}"
        "   ]"
        " }},"
        " {$unionWith: {"
        "   coll: 'unionColl',"
        "   pipeline: ["
        "     {$match: {business: {$eq: 'good'}}},"
        "     {$project: {_id: true, x: true}}"
        "   ]"
        " }}"
        "]");
}


std::unique_ptr<Pipeline, PipelineDeleter> getOptimizedPipeline(const BSONObj inputBson) {
    QueryTestServiceContext testServiceContext;
    auto opCtx = testServiceContext.makeOperationContext();

    ASSERT_EQUALS(inputBson["pipeline"].type(), BSONType::Array);
    std::vector<BSONObj> rawPipeline;
    for (auto&& stageElem : inputBson["pipeline"].Array()) {
        ASSERT_EQUALS(stageElem.type(), BSONType::Object);
        rawPipeline.push_back(stageElem.embeddedObject());
    }
    AggregateCommandRequest request(kTestNss, rawPipeline);
    boost::intrusive_ptr<ExpressionContextForTest> ctx =
        new ExpressionContextForTest(opCtx.get(), request);
    ctx->mongoProcessInterface = std::make_shared<StubExplainInterface>();
    unittest::TempDir tempDir("PipelineTest");
    ctx->tempDir = tempDir.path();

    auto outputPipe = Pipeline::parse(request.getPipeline(), ctx);
    outputPipe->optimizePipeline();
    return outputPipe;
}

void assertTwoPipelinesOptimizeAndMergeTo(const std::string inputPipe1,
                                          const std::string inputPipe2,
                                          const std::string outputPipe) {
    const BSONObj input1Bson = pipelineFromJsonArray(inputPipe1);
    const BSONObj input2Bson = pipelineFromJsonArray(inputPipe2);
    const BSONObj outputBson = pipelineFromJsonArray(outputPipe);

    auto pipeline1 = getOptimizedPipeline(input1Bson);
    auto pipeline2 = getOptimizedPipeline(input2Bson);

    // Merge the pipelines
    for (const auto& source : pipeline2->getSources()) {
        pipeline1->pushBack(source);
    }
    pipeline1->optimizePipeline();

    ASSERT_VALUE_EQ(
        Value(pipeline1->writeExplainOps(SerializationOptions{
            .verbosity = boost::make_optional(ExplainOptions::Verbosity::kQueryPlanner)})),
        Value(outputBson["pipeline"]));
}

TEST(PipelineOptimizationTest, MergeUnwindPipelineWithSortLimitPipelineDoesNotSwapIfNoPreserve) {
    std::string inputPipe1 =
        "[{$unwind : {path: '$a'}}"
        "]";
    std::string inputPipe2 =
        "[{$sort: {b: 1}}"
        ",{$limit: 5}"
        "]";
    std::string outputPipe =
        "[{$unwind: {path: \"$a\"}}"
        ",{$sort: {sortKey: {b: 1}, limit: 5}}"
        "]";

    assertTwoPipelinesOptimizeAndMergeTo(inputPipe1, inputPipe2, outputPipe);
}

TEST(PipelineOptimizationTest, MergeUnwindPipelineWithSortLimitPipelineDoesSwapWithPreserve) {
    std::string inputPipe1 =
        "[{$unwind : {path: '$a', preserveNullAndEmptyArrays: true}}"
        "]";
    std::string inputPipe2 =
        "[{$sort: {b: 1}}"
        ",{$limit: 5}"
        "]";
    std::string outputPipe =
        "[{$sort: {sortKey: {b: 1}, limit: 5}}"
        ",{$unwind: {path: \"$a\", preserveNullAndEmptyArrays: true}}"
        ",{$limit: 5}"
        "]";

    assertTwoPipelinesOptimizeAndMergeTo(inputPipe1, inputPipe2, outputPipe);
}

TEST(PipelineOptimizationTest,
     MergeUnwindPipelineWithSortLimitPipelineDoesNotSwapWithOverlapPaths) {
    std::string inputPipe1 =
        "[{$unwind : {path: '$b', preserveNullAndEmptyArrays: true}}"
        "]";
    std::string inputPipe2 =
        "[{$sort: {b: 1}}"
        ",{$limit: 5}"
        "]";
    std::string outputPipe =
        "[{$unwind: {path: \"$b\", preserveNullAndEmptyArrays: true}}"
        ",{$sort: {sortKey: {b: 1}, limit: 5}}"
        "]";

    assertTwoPipelinesOptimizeAndMergeTo(inputPipe1, inputPipe2, outputPipe);
}

TEST(PipelineOptimizationTest, MergeUnwindPipelineWithSortLimitPipelinePlacesLimitProperly) {
    std::string inputPipe1 =
        "[{$unwind : {path: '$a', preserveNullAndEmptyArrays: true}}"
        "]";
    std::string inputPipe2 =
        "[{$sort: {b: 1}}"
        ",{$limit: 5}"
        ",{$skip: 4}"
        "]";
    std::string outputPipe =
        "[{$sort: {sortKey: {b: 1}, limit: 5}}"
        ",{$unwind: {path: \"$a\", preserveNullAndEmptyArrays: true}}"
        ",{$limit: 5}"
        ",{$skip: 4}"
        "]";

    assertTwoPipelinesOptimizeAndMergeTo(inputPipe1, inputPipe2, outputPipe);
}

}  // namespace Local

namespace Sharded {

/**
 * Stub process interface used to allow accessing the CatalogCache for those tests which involve
 * selecting a specific shard merger.
 */
class ShardMergerMongoProcessInterface : public StubMongoProcessInterface {
public:
    ShardMergerMongoProcessInterface(CatalogCacheMock* catalogCache)
        : StubMongoProcessInterface(), _catalogCache(catalogCache) {}

    boost::optional<ShardId> determineSpecificMergeShard(OperationContext* opCtx,
                                                         const NamespaceString& ns) const override {
        if (_catalogCache) {
            return CommonProcessInterface::findOwningShard(opCtx, _catalogCache, ns);
        }
        return boost::none;
    }

private:
    CatalogCacheMock* _catalogCache;
};

class PipelineOptimizations : public ShardServerTestFixtureWithCatalogCacheMock {
public:
    // Allows tests to override the default resolvedNamespaces.
    virtual NamespaceString getLookupCollNs() {
        return NamespaceString::createNamespaceString_forTest("a", "lookupColl");
    }

    BSONObj pipelineFromJsonArray(const std::string& array) {
        return fromjson("{pipeline: " + array + "}");
    }

    void doTest(std::string inputPipeJson, std::string shardPipeJson, std::string mergePipeJson) {
        const BSONObj inputBson = pipelineFromJsonArray(inputPipeJson);
        const BSONObj shardPipeExpected = pipelineFromJsonArray(shardPipeJson);
        const BSONObj mergePipeExpected = pipelineFromJsonArray(mergePipeJson);

        ASSERT_EQUALS(inputBson["pipeline"].type(), BSONType::Array);
        std::vector<BSONObj> rawPipeline;
        for (auto&& stageElem : inputBson["pipeline"].Array()) {
            ASSERT_EQUALS(stageElem.type(), BSONType::Object);
            rawPipeline.push_back(stageElem.embeddedObject());
        }
        AggregateCommandRequest request(kTestNss, rawPipeline);
        boost::intrusive_ptr<ExpressionContextForTest> ctx = createExpressionContext(request);
        unittest::TempDir tempDir("PipelineTest");
        ctx->tempDir = tempDir.path();
        ctx->mongoProcessInterface =
            std::make_shared<Sharded::ShardMergerMongoProcessInterface>(getCatalogCacheMock());

        // For $graphLookup and $lookup, we have to populate the resolvedNamespaces so that the
        // operations will be able to have a resolved view definition.
        auto lookupCollNs = getLookupCollNs();
        ctx->setResolvedNamespace(lookupCollNs, {lookupCollNs, std::vector<BSONObj>{}});

        // Test that we can both split the pipeline and reassemble it into its original form.
        mergePipe = Pipeline::parse(request.getPipeline(), ctx);
        mergePipe->optimizePipeline();

        auto splitPipeline = sharded_agg_helpers::splitPipeline(std::move(mergePipe));
        const auto explain = SerializationOptions{
            .verbosity = boost::make_optional(ExplainOptions::Verbosity::kQueryPlanner)};
        ASSERT_VALUE_EQ(Value(splitPipeline.shardsPipeline->writeExplainOps(explain)),
                        Value(shardPipeExpected["pipeline"]));
        ASSERT_VALUE_EQ(Value(splitPipeline.mergePipeline->writeExplainOps(explain)),
                        Value(mergePipeExpected["pipeline"]));

        shardPipe = std::move(splitPipeline.shardsPipeline);
        mergePipe = std::move(splitPipeline.mergePipeline);
    }

    virtual boost::intrusive_ptr<ExpressionContextForTest> createExpressionContext(
        const AggregateCommandRequest& request) {
        return new ExpressionContextForTest(operationContext(), request);
    }

protected:
    std::unique_ptr<Pipeline, PipelineDeleter> mergePipe;
    std::unique_ptr<Pipeline, PipelineDeleter> shardPipe;
};

TEST_F(PipelineOptimizations, Empty) {
    doTest("[]" /*inputPipeJson*/, "[]" /*shardPipeJson*/, "[]" /*mergePipeJson*/);
}


// Since each shard has an identical copy of config.cache.chunks.* namespaces, $lookup from
// config.cache.chunks.* should run on each shard in parallel.
class PipelineOptimizationsLookupFromShardsInParallel : public PipelineOptimizations {
public:
    NamespaceString getLookupCollNs() override {
        return _fromLookupColl;
    }

    void doTest(std::string inputPipeJson,
                std::string shardPipeJson,
                NamespaceString fromLookupColl) {
        _fromLookupColl = fromLookupColl;
        PipelineOptimizations::doTest(inputPipeJson, shardPipeJson, "[]");
    }

private:
    NamespaceString _fromLookupColl;
};

TEST_F(PipelineOptimizationsLookupFromShardsInParallel, LookupWithDBAndColl) {
    static const std::string kInputPipeJson =
        "[{$lookup: {from: {db: 'config', coll: 'cache.chunks.test.foo'}, as: 'results', "
        "localField: 'x', foreignField: '_id'}}]";
    doTest(kInputPipeJson,
           kInputPipeJson /*shardPipeJson*/,
           NamespaceString::createNamespaceString_forTest("config", "cache.chunks.test.foo"));
};

TEST_F(PipelineOptimizationsLookupFromShardsInParallel, LookupWithLetWithDBAndColl) {
    static const std::string kInputPipeJson =
        "[{$lookup: {from: {db: 'config', coll: 'cache.chunks.test.foo'}, as: 'results', "
        "let: {x_field: '$x'}, pipeline: []}}]";
    doTest(kInputPipeJson,
           kInputPipeJson /*shardPipeJson*/,
           NamespaceString::createNamespaceString_forTest("config", "cache.chunks.test.foo"));
};

TEST_F(PipelineOptimizationsLookupFromShardsInParallel, CollectionCloningPipeline) {
    static const std::string kInputPipeJson =
        "[{$match: {$expr: {$gte: ['$_id', {$literal: 1}]}}}"
        ",{$sort: {_id: 1}}"
        ",{$replaceWith: {original: '$$ROOT'}}"
        ",{$lookup: {from: {db: 'config', coll: 'cache.chunks.test'},"
        "pipeline: [], as: 'intersectingChunk'}}"
        ",{$match: {intersectingChunk: {$ne: []}}}"
        ",{$replaceWith: '$original'}"
        "]";
    static const std::string kShardPipeJson =
        "[{$match: {$and: [{_id: {$_internalExprGte: 1}}, {$expr: {$gte: ['$_id', "
        "{$const: 1}]}}]}}"
        ", {$sort: {sortKey: {_id: 1}}}"
        ", {$replaceRoot: {newRoot: {original: '$$ROOT'}}}"
        ", {$lookup: {from: {db: 'config', coll: 'cache.chunks.test'}, as: "
        "'intersectingChunk', let: {}, pipeline: []}}"
        ", {$match: {intersectingChunk: {$not: {$eq: []}}}}"
        ", {$replaceRoot: {newRoot: '$original'}}"
        "]";
    doTest(kInputPipeJson,
           kShardPipeJson,
           NamespaceString::createNamespaceString_forTest("config", "cache.chunks.test"));
};

namespace moveFinalUnwindFromShardsToMerger {

TEST_F(PipelineOptimizations, MoveFinalUnwindFromShardsToMerger) {
    doTest("[{$unwind: {path: '$a'}}]" /*inputPipeJson*/,
           "[]" /*shardPipeJson*/,
           "[{$unwind: {path: '$a'}}]" /*mergePipeJson*/);
};

TEST_F(PipelineOptimizations, MoveFinalUnwindTwoFromShardsToMerger) {
    doTest("[{$unwind: {path: '$a'}}, {$unwind: {path: '$b'}}]" /*inputPipeJson*/,
           "[]" /*shardPipeJson*/,
           "[{$unwind: {path: '$a'}}, {$unwind: {path: '$b'}}]" /*mergePipeJson*/);
};

TEST_F(PipelineOptimizations, DontMoveNonFinalUnwindTwoFromShardsToMerger) {
    doTest("[{$unwind: {path: '$a'}}, {$match: {a:1}}]" /*inputPipeJson*/,
           "[{$unwind: {path: '$a'}}, {$match: {a:{$eq:1}}}]" /*shardPipeJson*/,
           "[]" /*mergePipeJson*/);
};

TEST_F(PipelineOptimizations, MoveFinalUnwindWithOtherShardsToMerger) {
    doTest("[{$match: {a:1}}, {$unwind: {path: '$a'}}]" /*inputPipeJson*/,
           "[{$match: {a: {$eq: 1}}}]" /*shardPipeJson*/,
           "[{$unwind: {path: '$a'}}]" /*mergePipeJson*/);
};

}  // namespace moveFinalUnwindFromShardsToMerger

namespace propagateDocLimitToShards {

/**
 * The $skip stage splits the pipeline into a shard pipeline and merge pipeline. Because the $limit
 * stage in the merge pipeline creates an upper bound on how many documents are necessary from any
 * of the shards, we can add a $limit to the shard pipeline to prevent it from sending more
 * documents than necessary. See the comment for propagateDocLimitToShard in
 * sharded_agg_helpers.cpp and the explanation in SERVER-36881.
 */

TEST_F(PipelineOptimizations, MatchWithSkipAndLimit) {
    doTest("[{$match: {x: 4}}, {$skip: 10}, {$limit: 5}]" /*inputPipeJson*/,
           "[{$match: {x: {$eq: 4}}}, {$limit: 15}]" /*shardPipeJson*/,
           "[{$skip: 10}, {$limit: 5}]" /*mergePipeJson*/);
};

/**
 * When computing an upper bound on how many documents we need from each shard, make sure to count
 * all $skip stages in any pipeline that has more than one.
 */
TEST_F(PipelineOptimizations, MatchWithMultipleSkipsAndLimit) {
    doTest("[{$match: {x: 4}}, {$skip: 7}, {$skip: 3}, {$limit: 5}]" /*inputPipeJson*/,
           "[{$match: {x: {$eq: 4}}}, {$limit: 15}]" /*shardPipeJson*/,
           "[{$skip: 10}, {$limit: 5}]" /*mergePipeJson*/);
};

/**
 * A $limit stage splits the pipeline with the $limit in place on both the shard and merge
 * pipelines. Make sure that the propagateDocLimitToShards() optimization does not add another
 * $limit to the shard pipeline.
 */
TEST_F(PipelineOptimizations, MatchWithLimitAndSkip) {
    doTest("[{$match: {x: 4}}, {$limit: 10}, {$skip: 5}]" /*inputPipeJson*/,
           "[{$match: {x: {$eq: 4}}}, {$limit: 10}]" /*shardPipeJson*/,
           "[{$limit: 10}, {$skip: 5}]" /*mergePipeJson*/);
};

/**
 * The addition of an $addFields stage between the $skip and $limit stages does not prevent us from
 * propagating the limit to the shards.
 */
TEST_F(PipelineOptimizations, MatchWithSkipAddFieldsAndLimit) {
    doTest("[{$match: {x: 4}}, {$skip: 10}, {$addFields: {y: 1}}, {$limit: 5}]" /*inputPipeJson*/,
           "[{$match: {x: {$eq: 4}}}, {$limit: 15}]" /*shardPipeJson*/,
           "[{$skip: 10}, {$addFields: {y: {$const: 1}}}, {$limit: 5}]" /*mergePipeJson*/);
};

/**
 * The addition of a $group stage between the $skip and $limit stages _does_ prevent us from
 * propagating the limit to the shards. The merger will need to see all the documents from each
 * shard before it can apply the $limit.
 */
TEST_F(PipelineOptimizations, MatchWithSkipGroupAndLimit) {
    doTest("[{$match: {x: 4}}, {$skip: 10}, {$group: {_id: '$y'}}, {$limit: 5}]" /*inputPipeJson*/,
           "[{$match: {x: {$eq: 4}}}, {$project: {y: true, _id: false}}]" /*shardPipeJson*/,
           "[{$skip: 10}, {$group: {_id: '$y'}}, {$limit: 5}]" /*mergePipeJson*/);
};

/**
 * The addition of a $match stage between the $skip and $limit stages also prevents us from
 * propagating the limit to the shards. We don't know in advance how many documents will pass the
 * filter in the second $match, so we also don't know how many documents we'll need from the shards.
 */
TEST_F(PipelineOptimizations, MatchWithSkipSecondMatchAndLimit) {
    doTest(
        "[{$match: {x: 4}}, {$skip: 10}, {$match: {y: {$gt: 10}}}, {$limit: 5}]" /*inputPipeJson*/,
        "[{$match: {x: {$eq: 4}}}]" /*shardPipeJson*/,
        "[{$skip: 10}, {$match: {y: {$gt: 10}}}, {$limit: 5}]" /*mergePipeJson*/);
};
}  // namespace propagateDocLimitToShards

namespace limitFieldsSentFromShardsToMerger {
// These tests use $limit to split the pipelines between shards and merger as it is
// always a split point and neutral in terms of needed fields.
TEST_F(PipelineOptimizations, LimitFieldsSentFromShardsToMergerNeedWholeDoc) {
    doTest("[{$limit:1}]" /*inputPipeJson*/,
           "[{$limit:1}]" /*shardPipeJson*/,
           "[{$limit:1}]" /*mergePipeJson*/);
};

TEST_F(PipelineOptimizations, LimitFieldsSentFromShardsToMergerJustNeedsId) {
    doTest("[{$limit:1}, {$group: {_id: '$_id'}}]" /*inputPipeJson*/,
           "[{$limit:1}, {$project: {_id:true}}]" /*shardPipeJson*/,
           "[{$limit:1}, {$group: {_id: '$_id'}}]" /*mergePipeJson*/);
};

TEST_F(PipelineOptimizations, LimitFieldsSentFromShardsToMergerJustNeedsNonId) {
    doTest("[{$limit:1}, {$group: {_id: '$a.b'}}]" /*inputPipeJson*/,
           "[{$limit:1}, {$project: {a: {b: true}, _id: false}}]" /*shardPipeJson*/,
           "[{$limit:1}, {$group: {_id: '$a.b'}}]" /*mergePipeJson*/);
};

TEST_F(PipelineOptimizations, LimitFieldsSentFromShardsToMergerNothingNeeded) {
    static const std::string kInputPipeJson =
        "[{$limit:1},"
        "{$group: {_id: {$const: null}, count: {$sum: {$const: 1}}}}]";
    doTest(kInputPipeJson,
           "[{$limit:1}, {$project: {_id: true}}]" /*shardPipeJson*/,
           kInputPipeJson /*mergePipeJson*/);
};

// No new project should be added. This test reflects current behavior where the
// 'a' field is still sent because it is explicitly asked for, even though it
// isn't actually needed. If this changes in the future, this test will need to
// change.
TEST_F(PipelineOptimizations, LimitFieldsSentFromShardsToMergerShardAlreadyExhaustive) {
    static const std::string kInputPipeJson =
        "[{$project: {_id:true, a:true}},"
        "{$group: {_id: '$_id'}}]";
    doTest(kInputPipeJson,
           kInputPipeJson /*shardPipeJson*/,
           "[{$group: {_id: '$$ROOT._id', $doingMerge: true}}]" /*mergePipeJson*/);
};

TEST_F(PipelineOptimizations,
       LimitFieldsSentFromShardsToMergerShardedSortMatchProjSkipLimBecomesMatchTopKSortSkipProj) {
    static const std::string kInputPipeJson =
        "[{$sort: {a : 1}}"
        ",{$match: {a: 1}}"
        ",{$project : {a: 1}}"
        ",{$skip : 3}"
        ",{$limit: 5}"
        "]";
    static const std::string kShardPipeJson =
        "[{$match: {a: {$eq : 1}}}"
        ",{$sort: {sortKey: {a: 1}, limit: 8}}"
        ",{$project: {_id: true, a: true}}"
        "]";
    static const std::string kMergePipeJson =
        "[{$limit: 8}"
        ",{$skip: 3}"
        ",{$project: {_id: true, a: true}}"
        "]";
    doTest(kInputPipeJson, kShardPipeJson, kMergePipeJson);
};

TEST_F(PipelineOptimizations,
       LimitFieldsSentFromShardsToMergerShardedMatchProjLimDoesNotBecomeMatchLimProj) {
    doTest(
        "[{$match: {a: 1}}, {$project : {a: 1}}, {$limit: 5}]" /*inputPipeJson*/,
        "[{$match: {a: {$eq : 1}}},{$project: {_id: true, a: true}},{$limit: 5}]" /*shardPipeJson*/,
        "[{$limit: 5}]" /*mergePipeJson*/);
};

TEST_F(PipelineOptimizations,
       LimitFieldsSentFromShardsToMergerShardedSortProjLimBecomesTopKSortProj) {
    doTest(
        "[{$sort: {a: 1}}, {$project : {a: 1}}, {$limit: 5}]" /*inputPipeJson*/,
        "[{$sort: {sortKey:{a: 1}, limit:5}},{$project: {_id: true, a: true}}]" /*shardPipeJson*/,
        "[{$limit: 5}, {$project: {_id: true, a: true}}]" /*mergePipeJson*/);
};

TEST_F(PipelineOptimizations,
       LimitFieldsSentFromShardsToMergerShardedSortGroupProjLimDoesNotBecomeTopKSortProjGroup) {
    doTest(
        "[{$sort:{a: 1}},{$group:{_id:{a:'$a'}}},{$project:{a: 1}},{$limit:5}]" /*inputPipeJson*/,
        "[{$sort: {sortKey: {a: 1}}},{$project : {a: true, _id: false}}]" /*shardPipeJson*/,
        "[{$group: {_id:{a: '$a'}}},{$project:{_id: true, a: true}},{$limit: 5}]" /*mergePipeJson*/);
};

TEST_F(PipelineOptimizations,
       LimitFieldsSentFromShardsToMergerShardedMatchSortProjLimBecomesMatchTopKSortProj) {
    doTest(
        "[{$match:{a:{$eq: 1}}},{$sort:{a: -1}},{$project:{a: 1}},{$limit: 6}]" /*inputPipeJson*/,
        "[{$match:{a:{$eq: 1}}},{$sort:{sortKey: {a: -1}, limit: 6}},{$project:{_id: true, a: "
        "true}}]" /*shardPipeJson*/,
        "[{$limit: 6},{$project: {_id: true, a: true}}]" /*mergePipeJson*/);
};

}  // namespace limitFieldsSentFromShardsToMerger

namespace coalesceLookUpAndUnwind {

TEST_F(PipelineOptimizations, ShouldCoalesceUnwindOnAs) {
    doTest(
        "[{$lookup: {from : 'lookupColl', as : 'same', localField: 'left', foreignField: "
        "'right'}},{$unwind: {path: '$same'}}]" /*inputPipeJson*/,
        "[]" /*shardPipeJson*/,
        "[{$lookup: {from : 'lookupColl', as : 'same', localField: 'left', foreignField: 'right', "
        "unwinding: {preserveNullAndEmptyArrays: false}}}]" /*mergePipeJson*/);
};

TEST_F(PipelineOptimizations, ShouldCoalesceUnwindOnAsWithPreserveEmpty) {
    doTest(
        "[{$lookup: {from : 'lookupColl', as : 'same', localField: 'left', foreignField: "
        "'right'}},{$unwind: {path: '$same', preserveNullAndEmptyArrays: true}}]" /*inputPipeJson*/,
        "[]" /*shardPipeJson*/,
        "[{$lookup: {from : 'lookupColl', as : 'same', localField: 'left', foreignField: 'right', "
        "unwinding: {preserveNullAndEmptyArrays: true}}}]" /*mergePipeJson*/);
};

TEST_F(PipelineOptimizations, ShouldCoalesceUnwindOnAsWithIncludeArrayIndex) {
    doTest(
        "[{$lookup: {from : 'lookupColl', as : 'same', localField: 'left', foreignField: "
        "'right'}},{$unwind: {path: '$same', includeArrayIndex: 'index'}}]" /*inputPipeJson*/,
        "[]" /*shardPipeJson*/,
        "[{$lookup: {from : 'lookupColl', as : 'same', localField: 'left', foreignField: 'right', "
        "unwinding: {preserveNullAndEmptyArrays: false, includeArrayIndex: 'index'}}}]" /*mergePipeJson*/);
};

TEST_F(PipelineOptimizations, ShouldNotCoalesceUnwindNotOnAs) {
    doTest(
        "[{$lookup: {from : 'lookupColl', as : 'same', localField: 'left', foreignField: "
        "'right'}},{$unwind: {path: '$from'}}]" /*inputPipeJson*/,
        "[]" /*shardPipeJson*/,
        "[{$lookup: {from : 'lookupColl', as : 'same', localField: 'left', foreignField: "
        "'right'}},{$unwind: {path: '$from'}}]" /*mergePipeJson*/);
};

}  // namespace coalesceLookUpAndUnwind

namespace needsSpecificShardMerger {

class PipelineOptimizationsShardMerger : public PipelineOptimizations {
public:
    void setUp() override {
        PipelineOptimizations::setUp();
        getCatalogCacheLoaderMock()->setDatabaseRefreshReturnValue(
            DatabaseType{DatabaseName::createDatabaseName_forTest(boost::none, "a"),
                         kMyShardName,
                         DatabaseVersion{}});
    }

    void doTest(std::string inputPipeJson,
                std::string shardPipeJson,
                std::string mergePipeJson,
                boost::optional<ShardId> needsSpecificShardMerger = boost::none) {
        PipelineOptimizations::doTest(
            std::move(inputPipeJson), std::move(shardPipeJson), std::move(mergePipeJson));
        ASSERT_EQUALS(mergePipe->needsSpecificShardMerger(), needsSpecificShardMerger);
        ASSERT(!shardPipe->needsSpecificShardMerger());
    }

    void doMergeWithCollectionWithRoutingTableTest(bool unsplittable) {
        const ChunkRange range = ChunkRange{BSON("_id" << MINKEY), BSON("_id" << MAXKEY)};
        const UUID uuid = UUID::gen();
        const OID epoch = OID::gen();
        const Timestamp timestamp{1, 1};

        auto rt = RoutingTableHistory::makeNew(
            NamespaceString::createNamespaceString_forTest("a", "outColl"),
            uuid,
            KeyPattern{BSON("_id" << 1)},
            unsplittable,
            nullptr /* defaultCollator */,
            false /* unique */,
            epoch,
            Timestamp(1, 1),
            boost::none /* timeseriesFields */,
            boost::none /* reshardingFields */,
            true,
            {ChunkType{uuid, range, ChunkVersion({epoch, timestamp}, {1, 0}), kMyShardName}});

        getCatalogCacheMock()->setCollectionReturnValue(
            NamespaceString::createNamespaceString_forTest("a.outColl"),
            CollectionRoutingInfo{ChunkManager{kMyShardName,
                                               DatabaseVersion{UUID::gen(), timestamp},
                                               makeStandaloneRoutingTableHistory(std::move(rt)),
                                               timestamp},
                                  boost::none});

        static const std::string kSentPipeJson =
            "[{$merge: {into: {db: 'a', coll: 'outColl'}, on: '_id', "
            "whenMatched: 'merge', whenNotMatched: 'insert'}}]";

        std::string shardPipeJson = unsplittable ? "[]" : kSentPipeJson;
        std::string mergePipeJson = unsplittable ? kSentPipeJson : "[]";
        boost::optional<ShardId> mergeShardId{unsplittable, kMyShardName};

        doTest("[{$merge: 'outColl'}]" /*inputPipeJson*/,
               std::move(shardPipeJson),
               std::move(mergePipeJson),
               mergeShardId);
    }
};

TEST_F(PipelineOptimizationsShardMerger, Out) {
    const Timestamp timestamp{1, 1};
    const auto nss = NamespaceString::createNamespaceString_forTest("a", "outColl");

    getCatalogCacheMock()->setCollectionReturnValue(
        nss,
        CatalogCacheMock::makeCollectionRoutingInfoUnsplittable(
            nss, ShardId("dbPrimary"), DatabaseVersion{UUID::gen(), timestamp}, kMyShardName));

    doTest("[{$out: 'outColl'}]" /*inputPipeJson*/,
           "[]" /*shardPipeJson*/,
           "[{$out: {coll: 'outColl', db: 'a'}}]" /*mergePipeJson*/,
           kMyShardName /* mergeShardId */);
};

TEST_F(PipelineOptimizationsShardMerger, MergeWithUntrackedCollection) {
    const Timestamp timestamp{1, 1};
    getCatalogCacheMock()->setCollectionReturnValue(
        NamespaceString::createNamespaceString_forTest("a.outColl"),
        CollectionRoutingInfo{
            ChunkManager{kMyShardName,
                         DatabaseVersion{UUID::gen(), timestamp},
                         RoutingTableHistoryValueHandle{OptionalRoutingTableHistory{}},
                         timestamp},
            boost::none});
    doTest("[{$merge: 'outColl'}]" /*inputPipeJson*/,
           "[]" /*shardPipeJson*/,
           "[{$merge: {into: {db: 'a', coll: 'outColl'}, on: '_id', "
           "whenMatched: 'merge', whenNotMatched: 'insert'}}]" /*mergePipeJson*/,
           kMyShardName /*needsSpecificShardMerger*/);
};

TEST_F(PipelineOptimizationsShardMerger, MergeWithShardedCollection) {
    doMergeWithCollectionWithRoutingTableTest(false /*unsplittable*/);
};

TEST_F(PipelineOptimizationsShardMerger, MergeWithUnsplittableCollection) {
    doMergeWithCollectionWithRoutingTableTest(true /*unsplittable*/);
};

TEST_F(PipelineOptimizationsShardMerger, Project) {
    doTest("[{$project: {a : 1}}]" /*inputPipeJson*/,
           "[{$project: {_id: true, a: true}}]" /*shardPipeJson*/,
           "[]" /*mergePipeJson*/);
};

TEST_F(PipelineOptimizationsShardMerger, LookUpUnsplittableFromCollection) {
    const ChunkRange range = ChunkRange{BSON("_id" << MINKEY), BSON("_id" << MAXKEY)};
    const UUID uuid = UUID::gen();
    const OID epoch = OID::gen();
    const Timestamp timestamp{1, 1};
    auto fromCollNs = getLookupCollNs();
    auto rt = RoutingTableHistory::makeNew(
        fromCollNs,
        uuid,
        KeyPattern{BSON("right" << 1)},
        true /* unsplittable */,
        nullptr /* defaultCollator */,
        false /* unique */,
        epoch,
        Timestamp(1, 1),
        boost::none /* timeseriesFields */,
        boost::none /* reshardingFields */,
        true /* allowMigrations */,
        {ChunkType{uuid, range, ChunkVersion({epoch, timestamp}, {1, 0}), kMyShardName}});

    getCatalogCacheMock()->setCollectionReturnValue(
        fromCollNs,
        CollectionRoutingInfo{ChunkManager{kMyShardName,
                                           DatabaseVersion{UUID::gen(), timestamp},
                                           makeStandaloneRoutingTableHistory(std::move(rt)),
                                           timestamp},
                              boost::none});
    doTest(
        "[{$lookup: {from : 'lookupColl', as : 'same', localField: 'left', foreignField: 'right'}}]" /* inputPipeJson */
        ,
        "[]" /* shardPipeJson */,
        "[{$lookup: {from : 'lookupColl', as : 'same', localField: 'left', foreignField: 'right'}}]" /* mergePipeJson */
        ,
        kMyShardName /* needsSpecificShardMerger */);
};

TEST_F(PipelineOptimizationsShardMerger, LookUpShardedFromCollection) {
    const ChunkRange range = ChunkRange{BSON("_id" << MINKEY), BSON("_id" << MAXKEY)};
    const UUID uuid = UUID::gen();
    const OID epoch = OID::gen();
    const Timestamp timestamp{1, 1};
    auto fromCollNs = getLookupCollNs();
    auto rt = RoutingTableHistory::makeNew(
        fromCollNs,
        uuid,
        KeyPattern{BSON("right" << 1)},
        false /* unsplittable */,
        nullptr /* defaultCollator */,
        false /* unique */,
        epoch,
        Timestamp(1, 1),
        boost::none /* timeseriesFields */,
        boost::none /* reshardingFields */,
        true /* allowMigrations */,
        {ChunkType{uuid, range, ChunkVersion({epoch, timestamp}, {1, 0}), kMyShardName}});

    getCatalogCacheMock()->setCollectionReturnValue(
        fromCollNs,
        CollectionRoutingInfo{ChunkManager{kMyShardName,
                                           DatabaseVersion{UUID::gen(), timestamp},
                                           makeStandaloneRoutingTableHistory(std::move(rt)),
                                           timestamp},
                              boost::none});
    doTest(
        "[{$lookup: {from : 'lookupColl', as : 'same', localField: 'left', foreignField: 'right'}}]" /* inputPipeJson */
        ,
        "[]" /* shardPipeJson */,
        "[{$lookup: {from : 'lookupColl', as : 'same', localField: 'left', foreignField: 'right'}}]" /* mergePipeJson */
        ,
        kMyShardName /* needsSpecificShardMerger */);
};

}  // namespace needsSpecificShardMerger

namespace mustRunOnMongoS {

// Like a DocumentSourceMock, but must run on mongoS and can be used anywhere in the pipeline.
class DocumentSourceMustRunOnMongoS : public DocumentSourceMock {
public:
    DocumentSourceMustRunOnMongoS(const boost::intrusive_ptr<ExpressionContext>& expCtx)
        : DocumentSourceMock({}, expCtx) {}

    StageConstraints constraints(Pipeline::SplitState pipeState) const final {
        // Overrides DocumentSourceMock's required position.
        return {StreamType::kStreaming,
                PositionRequirement::kNone,
                HostTypeRequirement::kMongoS,
                DiskUseRequirement::kNoDiskUse,
                FacetRequirement::kNotAllowed,
                TransactionRequirement::kAllowed,
                LookupRequirement::kNotAllowed,
                UnionRequirement::kAllowed};
    }

    static boost::intrusive_ptr<DocumentSourceMustRunOnMongoS> create(
        const boost::intrusive_ptr<ExpressionContext>& expCtx) {
        return new DocumentSourceMustRunOnMongoS(expCtx);
    }
};

using HostTypeRequirement = StageConstraints::HostTypeRequirement;
using PipelineMustRunOnMongoSTest = AggregationContextFixture;

TEST_F(PipelineMustRunOnMongoSTest, UnsplittablePipelineMustRunOnMongoS) {
    auto expCtx = getExpCtx();
    expCtx->inMongos = true;

    auto match = DocumentSourceMatch::create(fromjson("{x: 5}"), expCtx);
    auto runOnMongoS = DocumentSourceMustRunOnMongoS::create(expCtx);

    auto pipeline = Pipeline::create({match, runOnMongoS}, expCtx);
    ASSERT_TRUE(pipeline->requiredToRunOnMongos());

    pipeline->optimizePipeline();
    ASSERT_TRUE(pipeline->requiredToRunOnMongos());
}

TEST_F(PipelineMustRunOnMongoSTest, UnsplittableMongoSPipelineAssertsIfDisallowedStagePresent) {
    auto expCtx = getExpCtx();

    expCtx->allowDiskUse = true;
    expCtx->inMongos = true;

    auto match = DocumentSourceMatch::create(fromjson("{x: 5}"), expCtx);
    auto runOnMongoS = DocumentSourceMustRunOnMongoS::create(expCtx);
    auto sort = DocumentSourceSort::create(expCtx, fromjson("{x: 1}"));

    auto pipeline = Pipeline::create({match, runOnMongoS, sort}, expCtx);
    pipeline->optimizePipeline();

    // The entire pipeline must run on mongoS, but $sort cannot do so when 'allowDiskUse' is true.
    ASSERT_TRUE(pipeline->requiredToRunOnMongos());
    ASSERT_NOT_OK(pipeline->canRunOnMongos());
}

DEATH_TEST_F(PipelineMustRunOnMongoSTest,
             SplittablePipelineMustMergeOnMongoSAfterSplit,
             "invariant") {
    auto expCtx = getExpCtx();
    expCtx->inMongos = true;

    auto match = DocumentSourceMatch::create(fromjson("{x: 5}"), expCtx);
    auto split = DocumentSourceInternalSplitPipeline::create(expCtx, HostTypeRequirement::kNone);
    auto runOnMongoS = DocumentSourceMustRunOnMongoS::create(expCtx);

    auto pipeline = Pipeline::create({match, split, runOnMongoS}, expCtx);

    // We don't need to run the entire pipeline on mongoS because we can split at
    // $_internalSplitPipeline.
    ASSERT_FALSE(pipeline->requiredToRunOnMongos());

    auto splitPipeline = sharded_agg_helpers::splitPipeline(std::move(pipeline));
    ASSERT(splitPipeline.shardsPipeline);
    ASSERT(splitPipeline.mergePipeline);

    ASSERT_TRUE(splitPipeline.mergePipeline->requiredToRunOnMongos());

    // Calling 'requiredToRunOnMongos' on the shard pipeline will hit an invariant.
    splitPipeline.shardsPipeline->requiredToRunOnMongos();
}

/**
 * For the purposes of this test, assume every collection is unsharded. Stages may ask this during
 * setup. For example, to compute its constraints, the $merge stage needs to know if the output
 * collection is sharded.
 */
class FakeMongoProcessInterface : public StubMongoProcessInterface {
public:
    bool isSharded(OperationContext* opCtx, const NamespaceString& ns) override {
        return false;
    }
};

TEST_F(PipelineMustRunOnMongoSTest, SplitMongoSMergePipelineAssertsIfShardStagePresent) {
    auto expCtx = getExpCtx();

    expCtx->allowDiskUse = true;
    expCtx->inMongos = true;
    expCtx->mongoProcessInterface = std::make_shared<FakeMongoProcessInterface>();

    auto match = DocumentSourceMatch::create(fromjson("{x: 5}"), expCtx);
    auto split = DocumentSourceInternalSplitPipeline::create(expCtx, HostTypeRequirement::kNone);
    auto runOnMongoS = DocumentSourceMustRunOnMongoS::create(expCtx);
    auto outSpec = fromjson("{$out: 'outcoll'}");
    auto out = DocumentSourceOut::createFromBson(outSpec["$out"], expCtx);

    auto pipeline = Pipeline::create({match, split, runOnMongoS, out}, expCtx);

    // We don't need to run the entire pipeline on mongoS because we can split at
    // $_internalSplitPipeline.
    ASSERT_FALSE(pipeline->requiredToRunOnMongos());

    auto splitPipeline = sharded_agg_helpers::splitPipeline(std::move(pipeline));

    // The merge pipeline must run on mongoS, but $out needs to run on  the primary shard.
    ASSERT_TRUE(splitPipeline.mergePipeline->requiredToRunOnMongos());
    ASSERT_NOT_OK(splitPipeline.mergePipeline->canRunOnMongos());
}

TEST_F(PipelineMustRunOnMongoSTest, SplittablePipelineAssertsIfMongoSStageOnShardSideOfSplit) {
    auto expCtx = getExpCtx();
    expCtx->inMongos = true;

    auto match = DocumentSourceMatch::create(fromjson("{x: 5}"), expCtx);
    auto runOnMongoS = DocumentSourceMustRunOnMongoS::create(expCtx);
    auto split =
        DocumentSourceInternalSplitPipeline::create(expCtx, HostTypeRequirement::kAnyShard);

    auto pipeline = Pipeline::create({match, runOnMongoS, split}, expCtx);
    pipeline->optimizePipeline();

    // The 'runOnMongoS' stage comes before any splitpoint, so this entire pipeline must run on
    // mongoS. However, the pipeline *cannot* run on mongoS and *must* split at
    // $_internalSplitPipeline due to the latter's 'anyShard' requirement. The mongoS stage would
    // end up on the shard side of this split, and so it asserts.
    ASSERT_TRUE(pipeline->requiredToRunOnMongos());
    ASSERT_NOT_OK(pipeline->canRunOnMongos());
}

TEST_F(PipelineMustRunOnMongoSTest, SplittablePipelineRunsUnsplitOnMongoSIfSplitpointIsEligible) {
    auto expCtx = getExpCtx();
    expCtx->inMongos = true;

    auto match = DocumentSourceMatch::create(fromjson("{x: 5}"), expCtx);
    auto runOnMongoS = DocumentSourceMustRunOnMongoS::create(expCtx);
    auto split = DocumentSourceInternalSplitPipeline::create(expCtx, HostTypeRequirement::kNone);

    auto pipeline = Pipeline::create({match, runOnMongoS, split}, expCtx);
    pipeline->optimizePipeline();

    // The 'runOnMongoS' stage is before the splitpoint, so this entire pipeline must run on mongoS.
    // In this case, the splitpoint is itself eligible to run on mongoS, and so we are able to
    // return true.
    ASSERT_TRUE(pipeline->requiredToRunOnMongos());
}

}  // namespace mustRunOnMongoS

namespace DeferredSort {

// Like a DocumentSourceMock, but has a deferrable merge sort.
class DocumentSourceDeferredMergeSort : public DocumentSourceMock {
public:
    DocumentSourceDeferredMergeSort(const boost::intrusive_ptr<ExpressionContext>& expCtx)
        : DocumentSourceMock({}, expCtx) {}

    static boost::intrusive_ptr<DocumentSourceDeferredMergeSort> create(
        const boost::intrusive_ptr<ExpressionContext>& expCtx) {
        return new DocumentSourceDeferredMergeSort(expCtx);
    }


    static bool canMovePastDuringSplit(const DocumentSource& ds) {
        return ds.constraints().preservesOrderAndMetadata;
    }

    boost::optional<DistributedPlanLogic> distributedPlanLogic() override {
        DistributedPlanLogic logic;

        logic.shardsStage = this;
        logic.mergingStages = {};
        logic.mergeSortPattern = BSON("a" << 1);
        logic.needsSplit = false;
        logic.canMovePast = canMovePastDuringSplit;

        return logic;
    }
};

class DocumentSourceCanSwapWithSort : public DocumentSourceMock {
public:
    DocumentSourceCanSwapWithSort(const boost::intrusive_ptr<ExpressionContext>& expCtx)
        : DocumentSourceMock({}, expCtx) {}

    static boost::intrusive_ptr<DocumentSourceCanSwapWithSort> create(
        const boost::intrusive_ptr<ExpressionContext>& expCtx) {
        return new DocumentSourceCanSwapWithSort(expCtx);
    }

    boost::optional<DistributedPlanLogic> distributedPlanLogic() override {
        return boost::none;
    }

    // This is just to test splitting logic, doGetNext should not be called.
    GetNextResult doGetNext() override {
        MONGO_UNREACHABLE;
    }

    StageConstraints constraints(Pipeline::SplitState pipeState) const override {
        StageConstraints constraints(StreamType::kStreaming,
                                     PositionRequirement::kNone,
                                     HostTypeRequirement::kNone,
                                     DiskUseRequirement::kNoDiskUse,
                                     FacetRequirement::kNotAllowed,
                                     TransactionRequirement::kNotAllowed,
                                     LookupRequirement::kAllowed,
                                     UnionRequirement::kAllowed,
                                     ChangeStreamRequirement::kDenylist);
        constraints.preservesOrderAndMetadata = true;

        return constraints;
    }
};

using PipelineDeferredMergeSortTest = AggregationContextFixture;
using HostTypeRequirement = StageConstraints::HostTypeRequirement;

TEST_F(PipelineDeferredMergeSortTest, StageWithDeferredSortDoesNotSplit) {
    auto expCtx = getExpCtx();

    expCtx->inMongos = true;

    auto mock = DocumentSourceDeferredMergeSort::create(expCtx);
    auto swappable = DocumentSourceCanSwapWithSort::create(expCtx);
    auto split = DocumentSourceInternalSplitPipeline::create(expCtx, HostTypeRequirement::kNone);
    auto runOnMongoS = DocumentSourceMatch::create(fromjson("{b: 5}"), expCtx);

    auto pipeline = Pipeline::create({mock, swappable, split, runOnMongoS}, expCtx);

    auto splitPipeline = sharded_agg_helpers::splitPipeline(std::move(pipeline));

    // Verify that we've split the pipeline at the SplitPipeline stage, not on the deferred sort.
    ASSERT_EQ(splitPipeline.shardsPipeline->getSources().size(), 2);
    ASSERT_EQ(splitPipeline.mergePipeline->getSources().size(), 2);

    // Verify the sort is correct.
    ASSERT(splitPipeline.shardCursorsSortSpec);
    ASSERT_BSONOBJ_EQ(splitPipeline.shardCursorsSortSpec.value(), BSON("a" << 1));
}

TEST_F(PipelineDeferredMergeSortTest, EarliestSortIsSelectedIfDeferred) {
    auto expCtx = getExpCtx();

    expCtx->inMongos = true;

    auto mock = DocumentSourceDeferredMergeSort::create(expCtx);
    auto swappable = DocumentSourceCanSwapWithSort::create(expCtx);
    auto sort = DocumentSourceSort::create(expCtx, fromjson("{NO: 1}"));
    auto split = DocumentSourceInternalSplitPipeline::create(expCtx, HostTypeRequirement::kNone);
    auto runOnMongoS = DocumentSourceMatch::create(fromjson("{b: 5}"), expCtx);

    auto pipeline = Pipeline::create({mock, swappable, sort, split, runOnMongoS}, expCtx);

    auto splitPipeline = sharded_agg_helpers::splitPipeline(std::move(pipeline));

    // Verify that we've split the pipeline at the non-deferred sort.
    ASSERT_EQ(splitPipeline.shardsPipeline->getSources().size(), 2);
    ASSERT_EQ(splitPipeline.mergePipeline->getSources().size(), 3);

    // Verify the sort is correct.
    ASSERT(splitPipeline.shardCursorsSortSpec);
    ASSERT_BSONOBJ_EQ(splitPipeline.shardCursorsSortSpec.value(), BSON("a" << 1));
}

TEST_F(PipelineDeferredMergeSortTest, StageThatCantSwapGoesToMergingHalf) {
    auto expCtx = getExpCtx();

    expCtx->inMongos = true;

    auto mock = DocumentSourceDeferredMergeSort::create(expCtx);
    auto match = DocumentSourceMatch::create(fromjson("{a: 5}"), expCtx);
    auto split = DocumentSourceInternalSplitPipeline::create(expCtx, HostTypeRequirement::kNone);
    auto runOnMongoS = DocumentSourceMatch::create(fromjson("{b: 5}"), expCtx);

    auto pipeline = Pipeline::create({mock, match, split, runOnMongoS}, expCtx);

    auto splitPipeline = sharded_agg_helpers::splitPipeline(std::move(pipeline));

    // Verify that we've split the pipeline at the stage that can't be swapped.
    ASSERT_EQ(splitPipeline.shardsPipeline->getSources().size(), 1);
    ASSERT_EQ(splitPipeline.mergePipeline->getSources().size(), 3);

    // Verify the sort is correct.
    ASSERT(splitPipeline.shardCursorsSortSpec);
    ASSERT_BSONOBJ_EQ(splitPipeline.shardCursorsSortSpec.value(), BSON("a" << 1));
}
}  // namespace DeferredSort
}  // namespace Sharded
}  // namespace Optimizations

class PipelineInitialSource : public ServiceContextTest {};

TEST_F(PipelineInitialSource, GeoNearInitialQuery) {
    auto opCtx = makeOperationContext();
    const std::vector<BSONObj> rawPipeline = {
        fromjson("{$geoNear: {distanceField: 'd', near: [0, 0], query: {a: 1}}}")};
    boost::intrusive_ptr<ExpressionContextForTest> ctx = new ExpressionContextForTest(
        opCtx.get(),
        AggregateCommandRequest(NamespaceString::createNamespaceString_forTest("a.collection"),
                                rawPipeline));
    auto pipe = Pipeline::parse(rawPipeline, ctx);
    ASSERT_BSONOBJ_EQ(pipe->getInitialQuery(), BSON("a" << 1));
}

TEST_F(PipelineInitialSource, MatchInitialQuery) {
    auto opCtx = makeOperationContext();
    const std::vector<BSONObj> rawPipeline = {fromjson("{$match: {'a': 4}}")};
    boost::intrusive_ptr<ExpressionContextForTest> ctx = new ExpressionContextForTest(
        opCtx.get(),
        AggregateCommandRequest(NamespaceString::createNamespaceString_forTest("a.collection"),
                                rawPipeline));

    auto pipe = Pipeline::parse(rawPipeline, ctx);
    ASSERT_BSONOBJ_EQ(pipe->getInitialQuery(), BSON("a" << 4));
}

// Contains test cases for validation done on pipeline creation.
namespace pipeline_validate {

using PipelineValidateTest = AggregationContextFixture;

class DocumentSourceCollectionlessMock : public DocumentSourceMock {
public:
    DocumentSourceCollectionlessMock(const boost::intrusive_ptr<ExpressionContext>& expCtx)
        : DocumentSourceMock({}, expCtx) {}

    StageConstraints constraints(Pipeline::SplitState pipeState) const final {
        StageConstraints constraints(StreamType::kStreaming,
                                     PositionRequirement::kFirst,
                                     HostTypeRequirement::kNone,
                                     DiskUseRequirement::kNoDiskUse,
                                     FacetRequirement::kNotAllowed,
                                     TransactionRequirement::kAllowed,
                                     LookupRequirement::kAllowed,
                                     UnionRequirement::kAllowed);
        constraints.isIndependentOfAnyCollection = true;
        constraints.requiresInputDocSource = false;
        return constraints;
    }

    static boost::intrusive_ptr<DocumentSourceCollectionlessMock> create(
        const boost::intrusive_ptr<ExpressionContext>& expCtx) {
        return new DocumentSourceCollectionlessMock(expCtx);
    }
};

TEST_F(PipelineValidateTest, AggregateOneNSNotValidForEmptyPipeline) {
    const std::vector<BSONObj> rawPipeline = {};
    auto ctx = getExpCtx();

    ctx->ns = NamespaceString::makeCollectionlessAggregateNSS(
        DatabaseName::createDatabaseName_forTest(boost::none, "a"));

    ASSERT_THROWS_CODE(
        Pipeline::parse(rawPipeline, ctx), AssertionException, ErrorCodes::InvalidNamespace);
}

TEST_F(PipelineValidateTest, AggregateOneNSNotValidIfInitialStageRequiresCollection) {
    const std::vector<BSONObj> rawPipeline = {fromjson("{$match: {}}")};
    auto ctx = getExpCtx();

    ctx->ns = NamespaceString::makeCollectionlessAggregateNSS(
        DatabaseName::createDatabaseName_forTest(boost::none, "a"));

    ASSERT_THROWS_CODE(
        Pipeline::parse(rawPipeline, ctx), AssertionException, ErrorCodes::InvalidNamespace);
}

TEST_F(PipelineValidateTest, AggregateOneNSValidIfInitialStageIsCollectionless) {
    auto ctx = getExpCtx();
    auto collectionlessSource = DocumentSourceCollectionlessMock::create(ctx);

    ctx->ns = NamespaceString::makeCollectionlessAggregateNSS(
        DatabaseName::createDatabaseName_forTest(boost::none, "a"));

    Pipeline::create({collectionlessSource}, ctx);
}

TEST_F(PipelineValidateTest, CollectionNSNotValidIfInitialStageIsCollectionless) {
    auto ctx = getExpCtx();
    auto collectionlessSource = DocumentSourceCollectionlessMock::create(ctx);

    ctx->ns = kTestNss;

    ASSERT_THROWS_CODE(Pipeline::parse({fromjson("{$listLocalSessions: {}}")},
                                       ctx),  // Pipeline::create({collectionlessSource}, ctx),
                       AssertionException,
                       ErrorCodes::InvalidNamespace);
}

TEST_F(PipelineValidateTest, AggregateOneNSValidForFacetPipelineRegardlessOfInitialStage) {
    const std::vector<BSONObj> rawPipeline = {fromjson("{$facet: {subPipe: [{$match: {}}]}}")};
    auto ctx = getExpCtx();

    ctx->ns = NamespaceString::makeCollectionlessAggregateNSS(
        DatabaseName::createDatabaseName_forTest(boost::none, "unittests"));

    ASSERT_THROWS_CODE(
        Pipeline::parse(rawPipeline, ctx), AssertionException, ErrorCodes::InvalidNamespace);
}

TEST_F(PipelineValidateTest, ChangeStreamIsValidAsFirstStage) {
    const std::vector<BSONObj> rawPipeline = {fromjson("{$changeStream: {}}")};
    auto ctx = getExpCtx();
    setMockReplicationCoordinatorOnOpCtx(ctx->opCtx);
    ctx->ns = NamespaceString::createNamespaceString_forTest("a.collection");
    Pipeline::parse(rawPipeline, ctx);
}

TEST_F(PipelineValidateTest, ChangeStreamIsNotValidIfNotFirstStage) {
    const std::vector<BSONObj> rawPipeline = {fromjson("{$match: {custom: 'filter'}}"),
                                              fromjson("{$changeStream: {}}")};
    auto ctx = getExpCtx();
    setMockReplicationCoordinatorOnOpCtx(ctx->opCtx);
    ctx->ns = NamespaceString::createNamespaceString_forTest("a.collection");
    ASSERT_THROWS_CODE(Pipeline::parse(rawPipeline, ctx), AssertionException, 40602);
}

TEST_F(PipelineValidateTest, ChangeStreamIsNotValidIfNotFirstStageInFacet) {
    const std::vector<BSONObj> rawPipeline = {
        fromjson("{$facet: {subPipe: [{$match: {}}, {$changeStream: {}}]}}")};

    auto ctx = getExpCtx();
    setMockReplicationCoordinatorOnOpCtx(ctx->opCtx);
    ctx->ns = NamespaceString::createNamespaceString_forTest("a.collection");
    ASSERT_THROWS_CODE(Pipeline::parse(rawPipeline, ctx), AssertionException, 40600);
}

TEST_F(PipelineValidateTest, ChangeStreamSplitLargeEventIsValid) {
    const std::vector<BSONObj> rawPipeline = {fromjson("{$changeStream: {}}"),
                                              fromjson("{$changeStreamSplitLargeEvent: {}}")};
    auto ctx = getExpCtx();
    setMockReplicationCoordinatorOnOpCtx(ctx->opCtx);
    ctx->ns = NamespaceString::createNamespaceString_forTest("a.collection");
    Pipeline::parse(rawPipeline, ctx);
}

TEST_F(PipelineValidateTest, ChangeStreamSplitLargeEventIsNotValidWithoutChangeStream) {
    const std::vector<BSONObj> rawPipeline = {fromjson("{$changeStreamSplitLargeEvent: {}}")};
    auto ctx = getExpCtx();
    ctx->changeStreamSpec = boost::none;
    setMockReplicationCoordinatorOnOpCtx(ctx->opCtx);
    ctx->ns = NamespaceString::createNamespaceString_forTest("a.collection");
    ASSERT_THROWS_CODE(
        Pipeline::parse(rawPipeline, ctx), DBException, ErrorCodes::IllegalOperation);
}

TEST_F(PipelineValidateTest, ChangeStreamSplitLargeEventIsNotLastStage) {
    const std::vector<BSONObj> rawPipeline = {fromjson("{$changeStream: {}}"),
                                              fromjson("{$changeStreamSplitLargeEvent: {}}"),
                                              fromjson("{$match: {}}")};
    auto ctx = getExpCtx();
    setMockReplicationCoordinatorOnOpCtx(ctx->opCtx);
    ctx->ns = NamespaceString::createNamespaceString_forTest("a.collection");
    ASSERT_THROWS_CODE(Pipeline::parse(rawPipeline, ctx), DBException, 7182802);
}

TEST_F(PipelineValidateTest, ChangeStreamSplitLargeEventIsValidAfterMatch) {
    const std::vector<BSONObj> rawPipeline = {fromjson("{$changeStream: {}}"),
                                              fromjson("{$match: {custom: 'filter'}}"),
                                              fromjson("{$changeStreamSplitLargeEvent: {}}")};
    auto ctx = getExpCtx();
    setMockReplicationCoordinatorOnOpCtx(ctx->opCtx);
    ctx->ns = NamespaceString::createNamespaceString_forTest("a.collection");
    Pipeline::parse(rawPipeline, ctx);
}

TEST_F(PipelineValidateTest, ChangeStreamSplitLargeEventIsValidAfterRedact) {
    const std::vector<BSONObj> rawPipeline = {fromjson("{$changeStream: {}}"),
                                              fromjson("{$redact: '$$PRUNE'}"),
                                              fromjson("{$changeStreamSplitLargeEvent: {}}")};
    auto ctx = getExpCtx();
    setMockReplicationCoordinatorOnOpCtx(ctx->opCtx);
    ctx->ns = NamespaceString::createNamespaceString_forTest("a.collection");
    Pipeline::parse(rawPipeline, ctx);
}

class DocumentSourceDisallowedInTransactions : public DocumentSourceMock {
public:
    DocumentSourceDisallowedInTransactions(const boost::intrusive_ptr<ExpressionContext>& expCtx)
        : DocumentSourceMock({}, expCtx) {}

    StageConstraints constraints(Pipeline::SplitState pipeState) const final {
        return StageConstraints{StreamType::kStreaming,
                                PositionRequirement::kNone,
                                HostTypeRequirement::kNone,
                                DiskUseRequirement::kNoDiskUse,
                                FacetRequirement::kAllowed,
                                TransactionRequirement::kNotAllowed,
                                LookupRequirement::kAllowed,
                                UnionRequirement::kAllowed};
    }

    static boost::intrusive_ptr<DocumentSourceDisallowedInTransactions> create(
        const boost::intrusive_ptr<ExpressionContext>& expCtx) {
        return new DocumentSourceDisallowedInTransactions(expCtx);
    }
};

TEST_F(PipelineValidateTest, TopLevelPipelineValidatedForStagesIllegalInTransactions) {
    auto ctx = getExpCtx();
    ctx->opCtx->setInMultiDocumentTransaction();

    // Make a pipeline with a legal $match, and then an illegal mock stage, and verify that pipeline
    // creation fails with the expected error code.
    auto matchStage = DocumentSourceMatch::create(BSON("_id" << 3), ctx);
    auto illegalStage = DocumentSourceDisallowedInTransactions::create(ctx);
    ASSERT_THROWS_CODE(Pipeline::create({matchStage, illegalStage}, ctx),
                       AssertionException,
                       ErrorCodes::OperationNotSupportedInTransaction);
}

TEST_F(PipelineValidateTest, FacetPipelineValidatedForStagesIllegalInTransactions) {
    auto ctx = getExpCtx();
    ctx->opCtx->setInMultiDocumentTransaction();

    const std::vector<BSONObj> rawPipeline = {
        fromjson("{$facet: {subPipe: [{$match: {}}, {$out: 'outColl'}]}}")};
    ASSERT_THROWS_CODE(Pipeline::parse(rawPipeline, ctx),
                       AssertionException,
                       ErrorCodes::OperationNotSupportedInTransaction);
}

}  // namespace pipeline_validate

namespace Dependencies {

using PipelineDependenciesTest = AggregationContextFixture;

TEST_F(PipelineDependenciesTest, EmptyPipelineShouldRequireWholeDocument) {
    auto pipeline = Pipeline::create({}, getExpCtx());

    auto depsTracker = pipeline->getDependencies(DepsTracker::kAllMetadata);
    ASSERT_TRUE(depsTracker.needWholeDocument);
    ASSERT_FALSE(depsTracker.getNeedsMetadata(DocumentMetadataFields::kTextScore));

    depsTracker =
        pipeline->getDependencies(DepsTracker::kAllMetadata & ~DepsTracker::kOnlyTextScore);
    ASSERT_TRUE(depsTracker.needWholeDocument);
}

//
// Some dummy DocumentSources with different dependencies.
//

// Like a DocumentSourceMock, but can be used anywhere in the pipeline.
class DocumentSourceDependencyDummy : public DocumentSourceMock {
public:
    DocumentSourceDependencyDummy(const boost::intrusive_ptr<ExpressionContext>& expCtx)
        : DocumentSourceMock({}, expCtx) {}

    StageConstraints constraints(Pipeline::SplitState pipeState) const final {
        // Overrides DocumentSourceMock's required position.
        return {StreamType::kStreaming,
                PositionRequirement::kNone,
                HostTypeRequirement::kNone,
                DiskUseRequirement::kNoDiskUse,
                FacetRequirement::kAllowed,
                TransactionRequirement::kAllowed,
                LookupRequirement::kAllowed,
                UnionRequirement::kAllowed};
    }
};

class DocumentSourceDependenciesNotSupported : public DocumentSourceDependencyDummy {
public:
    DocumentSourceDependenciesNotSupported(const boost::intrusive_ptr<ExpressionContext>& expCtx)
        : DocumentSourceDependencyDummy(expCtx) {}
    DepsTracker::State getDependencies(DepsTracker* deps) const final {
        return DepsTracker::State::NOT_SUPPORTED;
    }

    static boost::intrusive_ptr<DocumentSourceDependenciesNotSupported> create(
        const boost::intrusive_ptr<ExpressionContext>& expCtx) {
        return new DocumentSourceDependenciesNotSupported(expCtx);
    }
};

class DocumentSourceNeedsASeeNext : public DocumentSourceDependencyDummy {
public:
    DocumentSourceNeedsASeeNext(const boost::intrusive_ptr<ExpressionContext>& expCtx)
        : DocumentSourceDependencyDummy(expCtx) {}
    DepsTracker::State getDependencies(DepsTracker* deps) const final {
        deps->fields.insert("a");
        return DepsTracker::State::SEE_NEXT;
    }

    static boost::intrusive_ptr<DocumentSourceNeedsASeeNext> create(
        const boost::intrusive_ptr<ExpressionContext>& expCtx) {
        return new DocumentSourceNeedsASeeNext(expCtx);
    }
};

class DocumentSourceNeedsOnlyB : public DocumentSourceDependencyDummy {
public:
    DocumentSourceNeedsOnlyB(const boost::intrusive_ptr<ExpressionContext>& expCtx)
        : DocumentSourceDependencyDummy(expCtx) {}
    DepsTracker::State getDependencies(DepsTracker* deps) const final {
        deps->fields.insert("b");
        return DepsTracker::State::EXHAUSTIVE_FIELDS;
    }

    static boost::intrusive_ptr<DocumentSourceNeedsOnlyB> create(
        const boost::intrusive_ptr<ExpressionContext>& expCtx) {
        return new DocumentSourceNeedsOnlyB(expCtx);
    }
};

class DocumentSourceNeedsOnlyTextScore : public DocumentSourceDependencyDummy {
public:
    DocumentSourceNeedsOnlyTextScore(const boost::intrusive_ptr<ExpressionContext>& expCtx)
        : DocumentSourceDependencyDummy(expCtx) {}
    DepsTracker::State getDependencies(DepsTracker* deps) const final {
        deps->setNeedsMetadata(DocumentMetadataFields::kTextScore, true);
        return DepsTracker::State::EXHAUSTIVE_META;
    }

    static boost::intrusive_ptr<DocumentSourceNeedsOnlyTextScore> create(
        const boost::intrusive_ptr<ExpressionContext>& expCtx) {
        return new DocumentSourceNeedsOnlyTextScore(expCtx);
    }
};

class DocumentSourceStripsTextScore : public DocumentSourceDependencyDummy {
public:
    DocumentSourceStripsTextScore(const boost::intrusive_ptr<ExpressionContext>& expCtx)
        : DocumentSourceDependencyDummy(expCtx) {}
    DepsTracker::State getDependencies(DepsTracker* deps) const final {
        return DepsTracker::State::EXHAUSTIVE_META;
    }

    static boost::intrusive_ptr<DocumentSourceStripsTextScore> create(
        const boost::intrusive_ptr<ExpressionContext>& expCtx) {
        return new DocumentSourceStripsTextScore(expCtx);
    }
};

TEST_F(PipelineDependenciesTest, ShouldRequireWholeDocumentIfAnyStageDoesNotSupportDeps) {
    auto ctx = getExpCtx();
    auto needsASeeNext = DocumentSourceNeedsASeeNext::create(ctx);
    auto notSupported = DocumentSourceDependenciesNotSupported::create(ctx);
    auto pipeline = Pipeline::create({needsASeeNext, notSupported}, ctx);

    auto depsTracker = pipeline->getDependencies(DepsTracker::kAllMetadata);
    ASSERT_TRUE(depsTracker.needWholeDocument);
    // The inputs did not have a text score available, so we should not require a text score.
    ASSERT_FALSE(depsTracker.getNeedsMetadata(DocumentMetadataFields::kTextScore));

    // Now in the other order.
    pipeline = Pipeline::create({notSupported, needsASeeNext}, ctx);

    depsTracker = pipeline->getDependencies(DepsTracker::kAllMetadata);
    ASSERT_TRUE(depsTracker.needWholeDocument);
}

TEST_F(PipelineDependenciesTest, ShouldRequireWholeDocumentIfNoStageReturnsExhaustiveFields) {
    auto ctx = getExpCtx();
    auto needsASeeNext = DocumentSourceNeedsASeeNext::create(ctx);
    auto pipeline = Pipeline::create({needsASeeNext}, ctx);

    auto depsTracker = pipeline->getDependencies(DepsTracker::kNoMetadata);
    ASSERT_TRUE(depsTracker.needWholeDocument);
}

TEST_F(PipelineDependenciesTest, ShouldNotRequireWholeDocumentIfAnyStageReturnsExhaustiveFields) {
    auto ctx = getExpCtx();
    auto needsASeeNext = DocumentSourceNeedsASeeNext::create(ctx);
    auto needsOnlyB = DocumentSourceNeedsOnlyB::create(ctx);
    auto pipeline = Pipeline::create({needsASeeNext, needsOnlyB}, ctx);

    auto depsTracker = pipeline->getDependencies(DepsTracker::kNoMetadata);
    ASSERT_FALSE(depsTracker.needWholeDocument);
    ASSERT_EQ(depsTracker.fields.size(), 2UL);
    ASSERT_EQ(depsTracker.fields.count("a"), 1UL);
    ASSERT_EQ(depsTracker.fields.count("b"), 1UL);
}

TEST_F(PipelineDependenciesTest, ShouldNotAddAnyRequiredFieldsAfterFirstStageWithExhaustiveFields) {
    auto ctx = getExpCtx();
    auto needsOnlyB = DocumentSourceNeedsOnlyB::create(ctx);
    auto needsASeeNext = DocumentSourceNeedsASeeNext::create(ctx);
    auto pipeline = Pipeline::create({needsOnlyB, needsASeeNext}, ctx);

    auto depsTracker = pipeline->getDependencies(DepsTracker::kAllMetadata);
    ASSERT_FALSE(depsTracker.needWholeDocument);
    ASSERT_FALSE(depsTracker.getNeedsMetadata(DocumentMetadataFields::kTextScore));

    // 'needsOnlyB' claims to know all its field dependencies, so we shouldn't add any from
    // 'needsASeeNext'.
    ASSERT_EQ(depsTracker.fields.size(), 1UL);
    ASSERT_EQ(depsTracker.fields.count("b"), 1UL);
}

TEST_F(PipelineDependenciesTest, ShouldNotRequireTextScoreIfThereIsNoScoreAvailable) {
    auto ctx = getExpCtx();
    auto pipeline = Pipeline::create({}, ctx);

    auto depsTracker = pipeline->getDependencies(DepsTracker::kAllMetadata);
    ASSERT_FALSE(depsTracker.getNeedsMetadata(DocumentMetadataFields::kTextScore));
}

TEST_F(PipelineDependenciesTest, ShouldThrowIfTextScoreIsNeededButNotPresent) {
    auto ctx = getExpCtx();
    auto needsText = DocumentSourceNeedsOnlyTextScore::create(ctx);
    auto pipeline = Pipeline::create({needsText}, ctx);

    ASSERT_THROWS(pipeline->getDependencies(DepsTracker::kAllMetadata), AssertionException);
}

TEST_F(PipelineDependenciesTest,
       ShouldRequireTextScoreIfAvailableAndNoStageReturnsExhaustiveMetaAndNeedsMerge) {
    auto ctx = getExpCtx();

    // When needsMerge is true, the consumer might implicitly use textScore, if it's available.
    ctx->needsMerge = true;

    auto pipeline = Pipeline::create({}, ctx);
    auto deps = pipeline->getDependencies(DepsTracker::kAllMetadata & ~DepsTracker::kOnlyTextScore);
    ASSERT_TRUE(deps.getNeedsMetadata(DocumentMetadataFields::kTextScore));

    pipeline = Pipeline::create({DocumentSourceNeedsASeeNext::create(ctx)}, ctx);
    deps = pipeline->getDependencies(DepsTracker::kAllMetadata & ~DepsTracker::kOnlyTextScore);
    ASSERT_TRUE(deps.getNeedsMetadata(DocumentMetadataFields::kTextScore));

    // When needsMerge is false, if no stage explicitly uses textScore then we know it isn't needed.
    ctx->needsMerge = false;

    pipeline = Pipeline::create({}, ctx);
    deps = pipeline->getDependencies(DepsTracker::kAllMetadata & ~DepsTracker::kOnlyTextScore);
    ASSERT_FALSE(deps.getNeedsMetadata(DocumentMetadataFields::kTextScore));

    pipeline = Pipeline::create({DocumentSourceNeedsASeeNext::create(ctx)}, ctx);
    deps = pipeline->getDependencies(DepsTracker::kAllMetadata & ~DepsTracker::kOnlyTextScore);
    ASSERT_FALSE(deps.getNeedsMetadata(DocumentMetadataFields::kTextScore));
}

TEST_F(PipelineDependenciesTest, ShouldNotRequireTextScoreIfAvailableButDefinitelyNotNeeded) {
    auto ctx = getExpCtx();
    auto stripsTextScore = DocumentSourceStripsTextScore::create(ctx);
    auto needsText = DocumentSourceNeedsOnlyTextScore::create(ctx);
    auto pipeline = Pipeline::create({stripsTextScore, needsText}, ctx);

    auto depsTracker =
        pipeline->getDependencies(DepsTracker::kAllMetadata & ~DepsTracker::kOnlyTextScore);

    // 'stripsTextScore' claims that no further stage will need metadata information, so we
    // shouldn't have the text score as a dependency.
    ASSERT_FALSE(depsTracker.getNeedsMetadata(DocumentMetadataFields::kTextScore));
}


class DocumentSourceProducerConsumer : public DocumentSourceDependencyDummy {
public:
    DocumentSourceProducerConsumer(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                   OrderedPathSet&& dependencies,
                                   OrderedPathSet&& generated,
                                   DepsTracker::State depsState)
        : DocumentSourceDependencyDummy(expCtx),
          _dependencies(std::move(dependencies)),
          _generated(std::move(generated)),
          _depsState(depsState) {}
    DepsTracker::State getDependencies(DepsTracker* deps) const final {
        deps->fields = _dependencies;
        return _depsState;
    }

    GetModPathsReturn getModifiedPaths() const final {
        auto generated = _generated;
        return {GetModPathsReturn::Type::kFiniteSet, std::move(generated), {}};
    }

    static boost::intrusive_ptr<DocumentSourceProducerConsumer> create(
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        OrderedPathSet&& dependencies,
        OrderedPathSet&& generated,
        DepsTracker::State depsState = DepsTracker::State::SEE_NEXT) {
        return new DocumentSourceProducerConsumer(
            expCtx, std::move(dependencies), std::move(generated), depsState);
    }

private:
    OrderedPathSet _dependencies;
    OrderedPathSet _generated;
    DepsTracker::State _depsState;
};

TEST_F(PipelineDependenciesTest, ShouldNotReturnDependenciesOnGeneratedPaths) {
    auto ctx = getExpCtx();
    auto needsAProducesBC = DocumentSourceProducerConsumer::create(ctx, {"a"}, {"b", "c"});
    auto needsCDProducesE = DocumentSourceProducerConsumer::create(ctx, {"c", "d"}, {"e"});
    auto needsBE = DocumentSourceProducerConsumer::create(
        ctx, {"b", "e"}, {}, DepsTracker::State::EXHAUSTIVE_ALL);
    auto pipeline = Pipeline::create({needsAProducesBC, needsCDProducesE, needsBE}, ctx);

    auto depsTracker = pipeline->getDependencies(DepsTracker::kAllMetadata);
    ASSERT_FALSE(depsTracker.needWholeDocument);
    ASSERT_FALSE(depsTracker.getNeedsMetadata(DocumentMetadataFields::kTextScore));

    // b, c, and e are generated within the pipeline so we should not request any of them. a and d
    // are non-generated dependencies.
    ASSERT_EQ(depsTracker.fields.size(), 2UL);
    ASSERT_EQ(depsTracker.fields.count("a"), 1UL);
    ASSERT_EQ(depsTracker.fields.count("d"), 1UL);
}

TEST_F(PipelineDependenciesTest, ShouldNotReturnDependenciesOnGeneratedPathsWithSubPathReferences) {
    auto ctx = getExpCtx();
    auto producer = DocumentSourceProducerConsumer::create(ctx, {}, {"a", "b", "c"});
    auto consumer = DocumentSourceProducerConsumer::create(
        ctx, {"aa", "b.b.b", "c.b", "d.b"}, {}, DepsTracker::State::EXHAUSTIVE_ALL);
    auto pipeline = Pipeline::create({producer, consumer}, ctx);

    auto depsTracker = pipeline->getDependencies(DepsTracker::kAllMetadata);
    ASSERT_FALSE(depsTracker.needWholeDocument);
    ASSERT_FALSE(depsTracker.getNeedsMetadata(DocumentMetadataFields::kTextScore));

    // 'a', 'b', and 'c' are generated within the pipeline so we should not request any of them.
    // 'aa' and 'd.b' are non-generated dependencies.
    ASSERT_EQ(depsTracker.fields.size(), 2UL);
    ASSERT_EQ(depsTracker.fields.count("aa"), 1UL);
    ASSERT_EQ(depsTracker.fields.count("d.b"), 1UL);
}

TEST_F(PipelineDependenciesTest, PathModifiedWithoutNameChangeShouldStillBeADependency) {
    auto ctx = getExpCtx();
    auto producer = DocumentSourceProducerConsumer::create(ctx, {"a"}, {"a"});
    auto consumer =
        DocumentSourceProducerConsumer::create(ctx, {"a"}, {}, DepsTracker::State::EXHAUSTIVE_ALL);
    auto pipeline = Pipeline::create({producer, consumer}, ctx);

    auto depsTracker = pipeline->getDependencies(DepsTracker::kAllMetadata);
    ASSERT_FALSE(depsTracker.needWholeDocument);
    ASSERT_FALSE(depsTracker.getNeedsMetadata(DocumentMetadataFields::kTextScore));

    // 'a' is both consumed by and modified within the same stage in the pipeline, so we need to
    // request it.
    ASSERT_EQ(depsTracker.fields.size(), 1UL);
    ASSERT_EQ(depsTracker.fields.count("a"), 1UL);
}
}  // namespace Dependencies

using PipelineRenameTracking = AggregationContextFixture;

TEST_F(PipelineRenameTracking, ReportsIdentityMapWhenEmpty) {
    auto expCtx = getExpCtx();
    auto pipeline = Pipeline::create({DocumentSourceMock::createForTest(expCtx)}, expCtx);
    {
        // Tracking renames backwards.
        auto renames = semantic_analysis::renamedPaths(
            pipeline->getSources().crbegin(), pipeline->getSources().crend(), {"a", "b", "c.d"});
        ASSERT(static_cast<bool>(renames));
        auto nameMap = *renames;
        ASSERT_EQ(nameMap.size(), 3UL);
        ASSERT_EQ(nameMap["a"], "a");
        ASSERT_EQ(nameMap["b"], "b");
        ASSERT_EQ(nameMap["c.d"], "c.d");
    }
    {
        // Tracking renames forwards.
        auto renames = semantic_analysis::renamedPaths(
            pipeline->getSources().cbegin(), pipeline->getSources().cend(), {"a", "b", "c.d"});
        ASSERT(static_cast<bool>(renames));
        auto nameMap = *renames;
        ASSERT_EQ(nameMap.size(), 3UL);
        ASSERT_EQ(nameMap["a"], "a");
        ASSERT_EQ(nameMap["b"], "b");
        ASSERT_EQ(nameMap["c.d"], "c.d");
    }
}

class NoModifications : public DocumentSourceTestOptimizations {
public:
    NoModifications(const boost::intrusive_ptr<ExpressionContext>& expCtx)
        : DocumentSourceTestOptimizations(expCtx) {}
    static boost::intrusive_ptr<NoModifications> create(
        const boost::intrusive_ptr<ExpressionContext>& expCtx) {
        return new NoModifications(expCtx);
    }

    /**
     * Returns a description which communicate that this stage modifies nothing.
     */
    GetModPathsReturn getModifiedPaths() const final {
        return {GetModPathsReturn::Type::kFiniteSet, OrderedPathSet(), {}};
    }
};

TEST_F(PipelineRenameTracking, ReportsIdentityWhenNoStageModifiesAnything) {
    auto expCtx = getExpCtx();
    {
        // Tracking renames backwards.
        auto pipeline = Pipeline::create(
            {DocumentSourceMock::createForTest(expCtx), NoModifications::create(expCtx)}, expCtx);
        auto renames = semantic_analysis::renamedPaths(
            pipeline->getSources().crbegin(), pipeline->getSources().crend(), {"a", "b", "c.d"});
        ASSERT(static_cast<bool>(renames));
        auto nameMap = *renames;
        ASSERT_EQ(nameMap.size(), 3UL);
        ASSERT_EQ(nameMap["a"], "a");
        ASSERT_EQ(nameMap["b"], "b");
        ASSERT_EQ(nameMap["c.d"], "c.d");
    }
    {
        // Tracking renames forwards.
        auto pipeline = Pipeline::create(
            {DocumentSourceMock::createForTest(expCtx), NoModifications::create(expCtx)}, expCtx);
        auto renames = semantic_analysis::renamedPaths(
            pipeline->getSources().cbegin(), pipeline->getSources().cend(), {"a", "b", "c.d"});
        ASSERT(static_cast<bool>(renames));
        auto nameMap = *renames;
        ASSERT_EQ(nameMap.size(), 3UL);
        ASSERT_EQ(nameMap["a"], "a");
        ASSERT_EQ(nameMap["b"], "b");
        ASSERT_EQ(nameMap["c.d"], "c.d");
    }
    {
        // Tracking renames backwards.
        auto pipeline = Pipeline::create({DocumentSourceMock::createForTest(expCtx),
                                          NoModifications::create(expCtx),
                                          NoModifications::create(expCtx),
                                          NoModifications::create(expCtx)},
                                         expCtx);
        auto renames = semantic_analysis::renamedPaths(
            pipeline->getSources().crbegin(), pipeline->getSources().crend(), {"a", "b", "c.d"});
        auto nameMap = *renames;
        ASSERT_EQ(nameMap.size(), 3UL);
        ASSERT_EQ(nameMap["a"], "a");
        ASSERT_EQ(nameMap["b"], "b");
        ASSERT_EQ(nameMap["c.d"], "c.d");
    }
    {
        // Tracking renames forwards.
        auto pipeline = Pipeline::create({DocumentSourceMock::createForTest(expCtx),
                                          NoModifications::create(expCtx),
                                          NoModifications::create(expCtx),
                                          NoModifications::create(expCtx)},
                                         expCtx);
        auto renames = semantic_analysis::renamedPaths(
            pipeline->getSources().cbegin(), pipeline->getSources().cend(), {"a", "b", "c.d"});
        auto nameMap = *renames;
        ASSERT_EQ(nameMap.size(), 3UL);
        ASSERT_EQ(nameMap["a"], "a");
        ASSERT_EQ(nameMap["b"], "b");
        ASSERT_EQ(nameMap["c.d"], "c.d");
    }
}

class NotSupported : public DocumentSourceTestOptimizations {
public:
    NotSupported(const boost::intrusive_ptr<ExpressionContext>& expCtx)
        : DocumentSourceTestOptimizations(expCtx) {}
    static boost::intrusive_ptr<NotSupported> create(
        const boost::intrusive_ptr<ExpressionContext>& expCtx) {
        return new NotSupported(expCtx);
    }

    /**
     * Returns a description which communicate that this stage modifies nothing.
     */
    GetModPathsReturn getModifiedPaths() const final {
        return {GetModPathsReturn::Type::kNotSupported, OrderedPathSet(), {}};
    }
};

TEST_F(PipelineRenameTracking, DoesNotReportRenamesIfAStageDoesNotSupportTrackingThem) {
    auto expCtx = getExpCtx();
    auto pipeline = Pipeline::create({DocumentSourceMock::createForTest(expCtx),
                                      NoModifications::create(expCtx),
                                      NotSupported::create(expCtx),
                                      NoModifications::create(expCtx)},
                                     expCtx);
    // Backwards case.
    ASSERT_FALSE(static_cast<bool>(semantic_analysis::renamedPaths(
        pipeline->getSources().crbegin(), pipeline->getSources().crend(), {"a"})));
    ASSERT_FALSE(static_cast<bool>(semantic_analysis::renamedPaths(
        pipeline->getSources().crbegin(), pipeline->getSources().crend(), {"a", "b"})));
    ASSERT_FALSE(static_cast<bool>(semantic_analysis::renamedPaths(
        pipeline->getSources().crbegin(), pipeline->getSources().crend(), {"x", "yahoo", "c.d"})));
    // Forwards case.
    ASSERT_FALSE(static_cast<bool>(semantic_analysis::renamedPaths(
        pipeline->getSources().cbegin(), pipeline->getSources().cend(), {"a"})));
    ASSERT_FALSE(static_cast<bool>(semantic_analysis::renamedPaths(
        pipeline->getSources().cbegin(), pipeline->getSources().cend(), {"a", "b"})));
    ASSERT_FALSE(static_cast<bool>(semantic_analysis::renamedPaths(
        pipeline->getSources().cbegin(), pipeline->getSources().cend(), {"x", "yahoo", "c.d"})));
}

class RenamesAToB : public DocumentSourceTestOptimizations {
public:
    RenamesAToB(const boost::intrusive_ptr<ExpressionContext>& expCtx)
        : DocumentSourceTestOptimizations(expCtx) {}
    static boost::intrusive_ptr<RenamesAToB> create(
        const boost::intrusive_ptr<ExpressionContext>& expCtx) {
        return new RenamesAToB(expCtx);
    }
    GetModPathsReturn getModifiedPaths() const final {
        return {GetModPathsReturn::Type::kFiniteSet, OrderedPathSet{}, {{"b", "a"}}};
    }
};

TEST_F(PipelineRenameTracking, ReportsNewNamesWhenSingleStageRenames) {
    auto expCtx = getExpCtx();
    auto pipeline = Pipeline::create(
        {DocumentSourceMock::createForTest(expCtx), RenamesAToB::create(expCtx)}, expCtx);
    {
        // Tracking backwards.
        auto renames = semantic_analysis::renamedPaths(
            pipeline->getSources().crbegin(), pipeline->getSources().crend(), {"b"});
        ASSERT(static_cast<bool>(renames));
        auto nameMap = *renames;
        ASSERT_EQ(nameMap.size(), 1UL);
        ASSERT_EQ(nameMap["b"], "a");
    }
    {
        // Tracking forwards.
        auto renames = semantic_analysis::renamedPaths(
            pipeline->getSources().cbegin(), pipeline->getSources().cend(), {"a"});
        ASSERT(static_cast<bool>(renames));
        auto nameMap = *renames;
        ASSERT_EQ(nameMap.size(), 1UL);
        ASSERT_EQ(nameMap["a"], "b");
    }
    {
        // Tracking backwards.
        auto renames = semantic_analysis::renamedPaths(
            pipeline->getSources().crbegin(), pipeline->getSources().crend(), {"b", "c.d"});
        ASSERT(static_cast<bool>(renames));
        auto nameMap = *renames;
        ASSERT_EQ(nameMap.size(), 2UL);
        ASSERT_EQ(nameMap["b"], "a");
        ASSERT_EQ(nameMap["c.d"], "c.d");
    }
    {
        // Tracking forwards.
        auto renames = semantic_analysis::renamedPaths(
            pipeline->getSources().cbegin(), pipeline->getSources().cend(), {"a", "c.d"});
        ASSERT(static_cast<bool>(renames));
        auto nameMap = *renames;
        ASSERT_EQ(nameMap.size(), 2UL);
        ASSERT_EQ(nameMap["a"], "b");
        ASSERT_EQ(nameMap["c.d"], "c.d");
    }

    {
        // This is strange; the mock stage reports to essentially duplicate the "a" field into "b".
        // Because of this, both "b" and "a" should map to "a".
        auto renames = semantic_analysis::renamedPaths(
            pipeline->getSources().crbegin(), pipeline->getSources().crend(), {"b", "a"});
        ASSERT(static_cast<bool>(renames));
        auto nameMap = *renames;
        ASSERT_EQ(nameMap.size(), 2UL);
        ASSERT_EQ(nameMap["b"], "a");
        ASSERT_EQ(nameMap["a"], "a");
    }
    {
        // Same strangeness as above, but in the forwards direction.
        auto renames = semantic_analysis::renamedPaths(
            pipeline->getSources().cbegin(), pipeline->getSources().cend(), {"b", "a"});
        ASSERT(static_cast<bool>(renames));
        auto nameMap = *renames;
        ASSERT_EQ(nameMap.size(), 2UL);
        ASSERT_EQ(nameMap["a"], "b");
        ASSERT_EQ(nameMap["b"], "b");
    }
}

TEST_F(PipelineRenameTracking, ReportsIdentityMapWhenGivenEmptyIteratorRange) {
    auto expCtx = getExpCtx();
    auto pipeline = Pipeline::create(
        {DocumentSourceMock::createForTest(expCtx), RenamesAToB::create(expCtx)}, expCtx);
    {
        // Tracking backwards.
        auto renames = semantic_analysis::renamedPaths(
            pipeline->getSources().crbegin(), pipeline->getSources().crbegin(), {"b"});
        ASSERT(static_cast<bool>(renames));
        auto nameMap = *renames;
        ASSERT_EQ(nameMap.size(), 1UL);
        ASSERT_EQ(nameMap["b"], "b");
    }
    {
        // Tracking forwards.
        auto renames = semantic_analysis::renamedPaths(
            pipeline->getSources().cbegin(), pipeline->getSources().cbegin(), {"b"});
        ASSERT(static_cast<bool>(renames));
        auto nameMap = *renames;
        ASSERT_EQ(nameMap.size(), 1UL);
        ASSERT_EQ(nameMap["b"], "b");
    }

    {
        // Tracking backwards.
        auto renames = semantic_analysis::renamedPaths(
            pipeline->getSources().crbegin(), pipeline->getSources().crbegin(), {"b", "c.d"});
        ASSERT(static_cast<bool>(renames));
        auto nameMap = *renames;
        ASSERT_EQ(nameMap.size(), 2UL);
        ASSERT_EQ(nameMap["b"], "b");
        ASSERT_EQ(nameMap["c.d"], "c.d");
    }
    {
        // Tracking forwards.
        auto renames = semantic_analysis::renamedPaths(
            pipeline->getSources().cbegin(), pipeline->getSources().cbegin(), {"b", "c.d"});
        ASSERT(static_cast<bool>(renames));
        auto nameMap = *renames;
        ASSERT_EQ(nameMap.size(), 2UL);
        ASSERT_EQ(nameMap["b"], "b");
        ASSERT_EQ(nameMap["c.d"], "c.d");
    }
}

class RenamesBToC : public DocumentSourceTestOptimizations {
public:
    RenamesBToC(const boost::intrusive_ptr<ExpressionContext>& expCtx)
        : DocumentSourceTestOptimizations(expCtx) {}
    static boost::intrusive_ptr<RenamesBToC> create(
        const boost::intrusive_ptr<ExpressionContext>& expCtx) {
        return new RenamesBToC(expCtx);
    }
    GetModPathsReturn getModifiedPaths() const final {
        return {GetModPathsReturn::Type::kFiniteSet, OrderedPathSet{}, {{"c", "b"}}};
    }
};

TEST_F(PipelineRenameTracking, ReportsNewNameAcrossMultipleRenames) {
    auto expCtx = getExpCtx();
    {
        // Tracking backwards.
        auto pipeline = Pipeline::create({DocumentSourceMock::createForTest(expCtx),
                                          RenamesAToB::create(expCtx),
                                          RenamesBToC::create(expCtx)},
                                         expCtx);
        auto stages = pipeline->getSources();
        auto renames = semantic_analysis::renamedPaths(stages.crbegin(), stages.crend(), {"c"});
        ASSERT(static_cast<bool>(renames));
        auto nameMap = *renames;
        ASSERT_EQ(nameMap.size(), 1UL);
        ASSERT_EQ(nameMap["c"], "a");
    }
    {
        // Tracking forwards.
        auto pipeline = Pipeline::create({DocumentSourceMock::createForTest(expCtx),
                                          RenamesAToB::create(expCtx),
                                          RenamesBToC::create(expCtx)},
                                         expCtx);
        auto stages = pipeline->getSources();
        auto renames = semantic_analysis::renamedPaths(stages.cbegin(), stages.cend(), {"a"});
        ASSERT(static_cast<bool>(renames));
        auto nameMap = *renames;
        ASSERT_EQ(nameMap.size(), 1UL);
        ASSERT_EQ(nameMap["a"], "c");
    }
}

class RenamesBToA : public DocumentSourceTestOptimizations {
public:
    RenamesBToA(const boost::intrusive_ptr<ExpressionContext>& expCtx)
        : DocumentSourceTestOptimizations(expCtx) {}
    static boost::intrusive_ptr<RenamesBToA> create(
        const boost::intrusive_ptr<ExpressionContext>& expCtx) {
        return new RenamesBToA(expCtx);
    }
    GetModPathsReturn getModifiedPaths() const final {
        return {GetModPathsReturn::Type::kFiniteSet, OrderedPathSet{}, {{"a", "b"}}};
    }
};

TEST_F(PipelineRenameTracking, CanHandleBackAndForthRename) {
    auto expCtx = getExpCtx();
    {
        // Tracking backwards.
        auto pipeline = Pipeline::create({DocumentSourceMock::createForTest(expCtx),
                                          RenamesAToB::create(expCtx),
                                          RenamesBToA::create(expCtx)},
                                         expCtx);
        auto stages = pipeline->getSources();
        auto renames = semantic_analysis::renamedPaths(stages.crbegin(), stages.crend(), {"a"});
        ASSERT(static_cast<bool>(renames));
        auto nameMap = *renames;
        ASSERT_EQ(nameMap.size(), 1UL);
        ASSERT_EQ(nameMap["a"], "a");
    }
    {
        // Tracking forwards.
        auto pipeline = Pipeline::create({DocumentSourceMock::createForTest(expCtx),
                                          RenamesAToB::create(expCtx),
                                          RenamesBToA::create(expCtx)},
                                         expCtx);
        auto stages = pipeline->getSources();
        auto renames = semantic_analysis::renamedPaths(stages.cbegin(), stages.cend(), {"a"});
        ASSERT(static_cast<bool>(renames));
        auto nameMap = *renames;
        ASSERT_EQ(nameMap.size(), 1UL);
        ASSERT_EQ(nameMap["a"], "a");
    }
}

class InvolvedNamespacesTest : public AggregationContextFixture {
protected:
    InvolvedNamespacesTest() {
        ShardingState::create(getServiceContext());
    }
};

TEST_F(InvolvedNamespacesTest, NoInvolvedNamespacesForMatchSortProject) {
    boost::intrusive_ptr<ExpressionContext> expCtx(getExpCtx());
    auto pipeline = Pipeline::create(
        {DocumentSourceMock::createForTest(expCtx),
         DocumentSourceMatch::create(BSON("x" << 1), expCtx),
         DocumentSourceSort::create(expCtx, BSON("y" << -1)),
         DocumentSourceProject::create(BSON("x" << 1 << "y" << 1), expCtx, "$project"_sd)},
        expCtx);
    auto involvedNssSet = pipeline->getInvolvedCollections();
    ASSERT(involvedNssSet.empty());
}

TEST_F(InvolvedNamespacesTest, IncludesLookupNamespace) {
    auto expCtx = getExpCtx();
    const NamespaceString lookupNss = NamespaceString::createNamespaceString_forTest("test", "foo");
    const NamespaceString resolvedNss =
        NamespaceString::createNamespaceString_forTest("test", "bar");
    expCtx->setResolvedNamespace(lookupNss, {resolvedNss, std::vector<BSONObj>{}});
    auto lookupSpec =
        fromjson("{$lookup: {from: 'foo', as: 'x', localField: 'foo_id', foreignField: '_id'}}");
    auto pipeline =
        Pipeline::create({DocumentSourceMock::createForTest(expCtx),
                          DocumentSourceLookUp::createFromBson(lookupSpec.firstElement(), expCtx)},
                         expCtx);

    auto involvedNssSet = pipeline->getInvolvedCollections();
    ASSERT_EQ(involvedNssSet.size(), 1UL);
    ASSERT(involvedNssSet.find(resolvedNss) != involvedNssSet.end());
}

TEST_F(InvolvedNamespacesTest, IncludesGraphLookupNamespace) {
    auto expCtx = getExpCtx();
    const NamespaceString lookupNss = NamespaceString::createNamespaceString_forTest("test", "foo");
    const NamespaceString resolvedNss =
        NamespaceString::createNamespaceString_forTest("test", "bar");
    expCtx->setResolvedNamespace(lookupNss, {resolvedNss, std::vector<BSONObj>{}});
    auto graphLookupSpec = fromjson(
        "{$graphLookup: {"
        "  from: 'foo',"
        "  as: 'x',"
        "  connectFromField: 'x',"
        "  connectToField: 'y',"
        "  startWith: '$start'"
        "}}");
    auto pipeline = Pipeline::create(
        {DocumentSourceMock::createForTest(expCtx),
         DocumentSourceGraphLookUp::createFromBson(graphLookupSpec.firstElement(), expCtx)},
        expCtx);

    auto involvedNssSet = pipeline->getInvolvedCollections();
    ASSERT_EQ(involvedNssSet.size(), 1UL);
    ASSERT(involvedNssSet.find(resolvedNss) != involvedNssSet.end());
}

TEST_F(InvolvedNamespacesTest, IncludesLookupSubpipelineNamespaces) {
    auto expCtx = getExpCtx();
    const NamespaceString outerLookupNss =
        NamespaceString::createNamespaceString_forTest("test", "foo_outer");
    const NamespaceString outerResolvedNss =
        NamespaceString::createNamespaceString_forTest("test", "bar_outer");
    const NamespaceString innerLookupNss =
        NamespaceString::createNamespaceString_forTest("test", "foo_inner");
    const NamespaceString innerResolvedNss =
        NamespaceString::createNamespaceString_forTest("test", "bar_inner");
    expCtx->setResolvedNamespace(outerLookupNss, {outerResolvedNss, std::vector<BSONObj>{}});
    expCtx->setResolvedNamespace(innerLookupNss, {innerResolvedNss, std::vector<BSONObj>{}});
    auto lookupSpec = fromjson(
        "{$lookup: {"
        "  from: 'foo_outer', "
        "  as: 'x', "
        "  pipeline: [{$lookup: {from: 'foo_inner', as: 'y', pipeline: []}}]"
        "}}");
    auto pipeline =
        Pipeline::create({DocumentSourceMock::createForTest(expCtx),
                          DocumentSourceLookUp::createFromBson(lookupSpec.firstElement(), expCtx)},
                         expCtx);

    auto involvedNssSet = pipeline->getInvolvedCollections();
    ASSERT_EQ(involvedNssSet.size(), 2UL);
    ASSERT(involvedNssSet.find(outerResolvedNss) != involvedNssSet.end());
    ASSERT(involvedNssSet.find(innerResolvedNss) != involvedNssSet.end());
}

TEST_F(InvolvedNamespacesTest, IncludesGraphLookupSubPipeline) {
    auto expCtx = getExpCtx();
    const NamespaceString outerLookupNss =
        NamespaceString::createNamespaceString_forTest("test", "foo_outer");
    const NamespaceString outerResolvedNss =
        NamespaceString::createNamespaceString_forTest("test", "bar_outer");
    const NamespaceString innerLookupNss =
        NamespaceString::createNamespaceString_forTest("test", "foo_inner");
    const NamespaceString innerResolvedNss =
        NamespaceString::createNamespaceString_forTest("test", "bar_inner");
    expCtx->setResolvedNamespace(outerLookupNss, {outerResolvedNss, std::vector<BSONObj>{}});
    expCtx->setResolvedNamespace(
        outerLookupNss,
        {outerResolvedNss,
         std::vector<BSONObj>{fromjson("{$lookup: {from: 'foo_inner', as: 'x', pipeline: []}}")}});
    expCtx->setResolvedNamespace(innerLookupNss, {innerResolvedNss, std::vector<BSONObj>{}});
    auto graphLookupSpec = fromjson(
        "{$graphLookup: {"
        "  from: 'foo_outer', "
        "  as: 'x', "
        "  connectFromField: 'x',"
        "  connectToField: 'y',"
        "  startWith: '$start'"
        "}}");
    auto pipeline = Pipeline::create(
        {DocumentSourceMock::createForTest(expCtx),
         DocumentSourceGraphLookUp::createFromBson(graphLookupSpec.firstElement(), expCtx)},
        expCtx);

    auto involvedNssSet = pipeline->getInvolvedCollections();
    ASSERT_EQ(involvedNssSet.size(), 2UL);
    ASSERT(involvedNssSet.find(outerResolvedNss) != involvedNssSet.end());
    ASSERT(involvedNssSet.find(innerResolvedNss) != involvedNssSet.end());
}

TEST_F(InvolvedNamespacesTest, IncludesAllCollectionsWhenResolvingViews) {
    auto expCtx = getExpCtx();
    const NamespaceString normalCollectionNss =
        NamespaceString::createNamespaceString_forTest("test", "collection");
    const NamespaceString lookupNss = NamespaceString::createNamespaceString_forTest("test", "foo");
    const NamespaceString resolvedNss =
        NamespaceString::createNamespaceString_forTest("test", "bar");
    const NamespaceString nssIncludedInResolvedView =
        NamespaceString::createNamespaceString_forTest("test", "extra_backer_of_bar");
    expCtx->setResolvedNamespace(
        lookupNss,
        {resolvedNss,
         std::vector<BSONObj>{
             fromjson("{$lookup: {from: 'extra_backer_of_bar', as: 'x', pipeline: []}}")}});
    expCtx->setResolvedNamespace(nssIncludedInResolvedView,
                                 {nssIncludedInResolvedView, std::vector<BSONObj>{}});
    expCtx->setResolvedNamespace(normalCollectionNss,
                                 {normalCollectionNss, std::vector<BSONObj>{}});
    auto facetSpec = fromjson(
        "{$facet: {"
        "  pipe_1: ["
        "    {$lookup: {"
        "      from: 'foo',"
        "      as: 'x',"
        "      localField: 'foo_id',"
        "      foreignField: '_id'"
        "    }}"
        "  ],"
        "  pipe_2: ["
        "    {$lookup: {"
        "       from: 'collection',"
        "       as: 'z',"
        "       pipeline: []"
        "    }}"
        "  ]"
        "}}");
    auto pipeline =
        Pipeline::create({DocumentSourceMock::createForTest(expCtx),
                          DocumentSourceFacet::createFromBson(facetSpec.firstElement(), expCtx)},
                         expCtx);

    auto involvedNssSet = pipeline->getInvolvedCollections();
    ASSERT_EQ(involvedNssSet.size(), 3UL);
    ASSERT(involvedNssSet.find(resolvedNss) != involvedNssSet.end());
    ASSERT(involvedNssSet.find(nssIncludedInResolvedView) != involvedNssSet.end());
    ASSERT(involvedNssSet.find(normalCollectionNss) != involvedNssSet.end());
};

}  // namespace
}  // namespace mongo
