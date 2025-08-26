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

#include "mongo/db/pipeline/pipeline.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/json.h"
#include "mongo/db/client.h"
#include "mongo/db/database_name.h"
#include "mongo/db/exec/document_value/document_value_test_util.h"
#include "mongo/db/exec/exec_shard_filter_policy.h"
#include "mongo/db/global_catalog/router_role_api/router_role.h"
#include "mongo/db/pipeline/aggregate_command_gen.h"
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_change_stream.h"
#include "mongo/db/pipeline/document_source_change_stream_add_post_image.h"
#include "mongo/db/pipeline/document_source_change_stream_add_pre_image.h"
#include "mongo/db/pipeline/document_source_change_stream_ensure_resume_token_present.h"
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
#include "mongo/db/pipeline/pipeline_test_util.h"
#include "mongo/db/pipeline/plan_executor_pipeline.h"
#include "mongo/db/pipeline/process_interface/common_process_interface.h"
#include "mongo/db/pipeline/process_interface/stub_mongo_process_interface.h"
#include "mongo/db/pipeline/semantic_analysis.h"
#include "mongo/db/pipeline/sharded_agg_helpers.h"
#include "mongo/db/pipeline/stage_constraints.h"
#include "mongo/db/query/compiler/dependency_analysis/dependencies.h"
#include "mongo/db/query/explain_options.h"
#include "mongo/db/query/query_test_service_context.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/sharding_environment/shard_server_test_fixture.h"
#include "mongo/db/tenant_id.h"
#include "mongo/db/topology/sharding_state.h"
#include "mongo/dbtests/dbtests.h"  // IWYU pragma: keep
#include "mongo/idl/server_parameter_test_controller.h"
#include "mongo/logv2/log_util.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/temp_dir.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

#include <bitset>
#include <cstddef>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <absl/container/flat_hash_map.h>
#include <absl/container/node_hash_set.h>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {
namespace {

const StringData kDBName = "test";
const NamespaceString kTestNss =
    NamespaceString::createNamespaceString_forTest(kDBName, "collection");
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

DocumentSource* getStageAtPos(const DocumentSourceContainer& stages, int pos) {
    if (pos >= 0) {
        auto it = stages.begin();
        std::advance(it, pos);
        return (*it).get();
    } else {
        auto it = stages.rbegin();
        std::advance(
            it,
            -pos - 1);  // Subtract 1 because rbegin() points to the element before the last one.
        return (*it).get();
    }
}

template <typename T>
void assertStageAtPos(const DocumentSourceContainer& stages, int pos) {
    ASSERT(dynamic_cast<T*>(getStageAtPos(stages, pos)));
}

namespace Optimizations {
namespace Local {

BSONObj pipelineFromJsonArray(const std::string& jsonArray) {
    return fromjson("{pipeline: " + jsonArray + "}");
}

class StubExplainInterface : public StubMongoProcessInterface {
    BSONObj preparePipelineAndExplain(Pipeline* ownedPipeline,
                                      ExplainOptions::Verbosity verbosity) override {
        std::unique_ptr<Pipeline> pipeline(ownedPipeline);
        BSONArrayBuilder bab;
        auto opts = SerializationOptions{.verbosity = boost::make_optional(verbosity)};
        auto pipelineVec = pipeline->writeExplainOps(opts);
        for (auto&& stage : pipelineVec) {
            bab << stage;
        }
        return BSON("pipeline" << bab.arr());
    }
    std::unique_ptr<Pipeline> attachCursorSourceToPipelineForLocalRead(
        Pipeline* ownedPipeline,
        boost::optional<const AggregateCommandRequest&> aggRequest,
        bool shouldUseCollectionDefaultCollator,
        ExecShardFilterPolicy = AutomaticShardFiltering{}) override {
        std::unique_ptr<Pipeline> pipeline(ownedPipeline);
        return pipeline;
    }
};

class PipelineOptimizationTest : public mongo::unittest::Test {
protected:
    std::unique_ptr<Pipeline> assertPipelineOptimizesTo(const std::string& inputPipeJson,
                                                        const std::string& outputPipeJson,
                                                        NamespaceString aggNss = kTestNss) {
        const BSONObj inputBson = pipelineFromJsonArray(inputPipeJson);
        const BSONObj outputPipeExpected = pipelineFromJsonArray(outputPipeJson);

        ASSERT_EQUALS(inputBson["pipeline"].type(), BSONType::array);
        std::vector<BSONObj> rawPipeline;
        for (auto&& stageElem : inputBson["pipeline"].Array()) {
            ASSERT_EQUALS(stageElem.type(), BSONType::object);
            rawPipeline.push_back(stageElem.embeddedObject());
        }
        AggregateCommandRequest request(aggNss, rawPipeline);
        boost::intrusive_ptr<ExpressionContextForTest> ctx =
            new ExpressionContextForTest(opCtx.get(), request);
        ctx->setMongoProcessInterface(std::make_shared<StubExplainInterface>());
        unittest::TempDir tempDir("PipelineTest");
        ctx->setTempDir(tempDir.path());

        // For $graphLookup and $lookup, we have to populate the resolvedNamespaces so that the
        // operations will be able to have a resolved view definition.
        NamespaceString lookupCollNs =
            NamespaceString::createNamespaceString_forTest(kDBName, "lookupColl");
        NamespaceString unionCollNs =
            NamespaceString::createNamespaceString_forTest(kDBName, "unionColl");
        ctx->setResolvedNamespace(lookupCollNs, {lookupCollNs, std::vector<BSONObj>{}});
        ctx->setResolvedNamespace(unionCollNs, {unionCollNs, std::vector<BSONObj>{}});

        auto outputPipe = Pipeline::parse(request.getPipeline(), ctx);
        outputPipe->optimizePipeline();

        // We normalize match expressions in the pipeline here to ensure the stability of the
        // predicate order after optimizations.
        outputPipe = normalizeMatchStageInPipeline(std::move(outputPipe));
        auto opts = SerializationOptions{
            .verbosity = boost::make_optional(ExplainOptions::Verbosity::kQueryPlanner)};
        ASSERT_VALUE_EQ(Value(outputPipe->writeExplainOps(opts)),
                        Value(outputPipeExpected["pipeline"]));
        return outputPipe;
    }

    void assertPipelineSerializesTo(const Pipeline& pipeline,
                                    boost::optional<const SerializationOptions&> opts,
                                    const std::string& serializedPipeJson) {
        const BSONObj serializePipeExpected = pipelineFromJsonArray(serializedPipeJson);
        ASSERT_VALUE_EQ(Value(pipeline.serialize(opts)), Value(serializePipeExpected["pipeline"]));
    }

    void assertPipelineOptimizesAndSerializesTo(const std::string& inputPipeJson,
                                                const std::string& outputPipeJson,
                                                const std::string& serializedPipeJson,
                                                NamespaceString aggNss = kTestNss) {
        auto pipeline = assertPipelineOptimizesTo(inputPipeJson, outputPipeJson, aggNss);
        assertPipelineSerializesTo(*pipeline, boost::none, serializedPipeJson);
    }

    void assertPipelineOptimizesAndSerializesTo(const std::string& inputPipeJson,
                                                const std::string& outputPipeJson) {
        assertPipelineOptimizesAndSerializesTo(inputPipeJson, outputPipeJson, outputPipeJson);
    }

private:
    QueryTestServiceContext testServiceContext;
    ServiceContext::UniqueOperationContext opCtx = testServiceContext.makeOperationContext();
};

TEST_F(PipelineOptimizationTest, MoveSkipBeforeProject) {
    assertPipelineOptimizesAndSerializesTo("[{$project: {a : 1}}, {$skip : 5}]",
                                           "[{$skip : 5}, {$project: {_id: true, a : true}}]");
}

TEST_F(PipelineOptimizationTest, LimitDoesNotMoveBeforeProject) {
    assertPipelineOptimizesAndSerializesTo("[{$project: {a : 1}}, {$limit : 5}]",
                                           "[{$project: {_id: true, a : true}}, {$limit : 5}]");
}

TEST_F(PipelineOptimizationTest, SampleLegallyPushedBefore) {
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

    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe);
}

TEST_F(PipelineOptimizationTest, SampleNotIllegallyPushedBefore) {
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

TEST_F(PipelineOptimizationTest, MoveMatchBeforeAddFieldsIfInvolvedFieldsNotRelated) {
    std::string inputPipe = "[{$addFields : {a : 1}}, {$match : {b : 1}}]";

    std::string outputPipe = "[{$match : {b : {$eq : 1}}}, {$addFields : {a : {$const : 1}}}]";

    std::string serializedPipe = "[{$match: {b : 1}}, {$addFields: {a : {$const : 1}}}]";

    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe, serializedPipe);
}

TEST_F(PipelineOptimizationTest, MoveMatchWithExprBeforeAddFieldsIfInvolvedFieldsNotRelated) {
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

TEST_F(PipelineOptimizationTest, MatchDoesNotMoveBeforeAddFieldsIfInvolvedFieldsAreRelated) {
    std::string inputPipe = "[{$addFields : {a : 1}}, {$match : {a : 1}}]";

    std::string outputPipe = "[{$addFields : {a : {$const : 1}}}, {$match : {a : {$eq : 1}}}]";

    std::string serializedPipe = "[{$addFields : {a : {$const : 1}}}, {$match: {a : 1}}]";

    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe, serializedPipe);
}

TEST_F(PipelineOptimizationTest,
       MatchWithExprDoesNotMoveBeforeAddFieldsIfInvolvedFieldsAreRelated) {
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

TEST_F(PipelineOptimizationTest, MatchOnTopLevelFieldDoesNotMoveBeforeAddFieldsOfNestedPath) {
    std::string inputPipe = "[{$addFields : {'a.b' : 1}}, {$match : {a : 1}}]";

    std::string outputPipe =
        "[{$addFields : {a : {b : {$const : 1}}}}, {$match : {a : {$eq : 1}}}]";

    std::string serializedPipe = "[{$addFields: {a: {b: {$const: 1}}}}, {$match: {a: 1}}]";

    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe, serializedPipe);
}

TEST_F(PipelineOptimizationTest,
       MatchWithExprOnTopLevelFieldDoesNotMoveBeforeAddFieldsOfNestedPath) {
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

TEST_F(PipelineOptimizationTest, MatchOnNestedFieldDoesNotMoveBeforeAddFieldsOfPrefixOfPath) {
    std::string inputPipe = "[{$addFields : {a : 1}}, {$match : {'a.b' : 1}}]";

    std::string outputPipe = "[{$addFields : {a : {$const : 1}}}, {$match : {'a.b' : {$eq : 1}}}]";

    std::string serializedPipe = "[{$addFields : {a : {$const : 1}}}, {$match : {'a.b' : 1}}]";

    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe, serializedPipe);
}

TEST_F(PipelineOptimizationTest,
       MatchWithExprOnNestedFieldDoesNotMoveBeforeAddFieldsOfPrefixOfPath) {
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

TEST_F(PipelineOptimizationTest, MoveMatchOnNestedFieldBeforeAddFieldsOfDifferentNestedField) {
    std::string inputPipe = "[{$addFields : {'a.b' : 1}}, {$match : {'a.c' : 1}}]";

    std::string outputPipe =
        "[{$match : {'a.c' : {$eq : 1}}}, {$addFields : {a : {b : {$const : 1}}}}]";

    std::string serializedPipe = "[{$match : {'a.c' : 1}}, {$addFields : {a : {b: {$const : 1}}}}]";

    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe, serializedPipe);
}

TEST_F(PipelineOptimizationTest,
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

TEST_F(PipelineOptimizationTest, MoveMatchBeforeAddFieldsWhenMatchedFieldIsPrefixOfAddedFieldName) {
    std::string inputPipe = "[{$addFields : {abcd : 1}}, {$match : {abc : 1}}]";

    std::string outputPipe = "[{$match : {abc : {$eq : 1}}}, {$addFields : {abcd: {$const: 1}}}]";

    std::string serializedPipe = "[{$match : {abc : 1}}, {$addFields : {abcd : {$const : 1}}}]";

    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe, serializedPipe);
}

TEST_F(PipelineOptimizationTest,
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

TEST_F(PipelineOptimizationTest, LimitDoesNotSwapBeforeSkipWithoutSort) {
    std::string inputPipe =
        "[{$skip : 3}"
        ",{$skip : 5}"
        ",{$limit: 5}"
        "]";
    std::string outputPipe =
        "[{$skip : 8}"
        ",{$limit: 5}"
        "]";
    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe);
}

TEST_F(PipelineOptimizationTest, SortSwapsBeforeUnwind) {
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

    auto pipeline = assertPipelineOptimizesTo(inputPipe, outputPipe);
    assertPipelineSerializesTo(*pipeline, boost::none, serializedPipe);
    SerializationOptions options{.serializeForCloning = true};
    assertPipelineSerializesTo(*pipeline, options, serializedPipe);
}

TEST_F(PipelineOptimizationTest, SortSwapsBeforeUnwindMultipleSorts) {
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

TEST_F(PipelineOptimizationTest, SortSwapsBeforeUnwindDifferentDotPaths) {
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

TEST_F(PipelineOptimizationTest, SortSwapsBeforeUnwindMultipleSortPaths) {
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

TEST_F(PipelineOptimizationTest, SortDoesNotSwapBeforeUnwindMultipleSortPaths) {
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

TEST_F(PipelineOptimizationTest, SortDoesNotSwapBeforeUnwindBecauseSortPathPrefixOfUnwindPath) {
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

TEST_F(PipelineOptimizationTest, SortDoesNotSwapBeforeUnwindBecauseUnwindPathPrefixOfSortPath) {
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

TEST_F(PipelineOptimizationTest, SortDoesNotSwapBeforeUnwindBecauseUnwindPathEqualToSortPath) {
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

TEST_F(PipelineOptimizationTest, SortDoesNotSwapBeforeUnwindBecauseArrayIndexField) {
    std::string inputPipe =
        "[{$unwind : {path: '$a', includeArrayIndex: 'i'}}"
        ",{$sort : {i: 1}}"
        "]";
    std::string outputPipe =
        "[{$unwind : {path: '$a', includeArrayIndex: 'i'}}"
        ",{$sort : {sortKey: {i: 1}}}"
        "]";
    std::string serializedPipe =
        "[{$unwind : {path: '$a', includeArrayIndex: 'i'}}"
        ",{$sort : {i: 1}}"
        "]";
    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe, serializedPipe);
}

TEST_F(PipelineOptimizationTest, LookupShouldCoalesceWithUnwindOnAsSortDoesNotInterfere) {
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

TEST_F(PipelineOptimizationTest, SortSwapsBeforeUnwindMetaWithFieldPath) {
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

TEST_F(PipelineOptimizationTest, SortSwapsBeforeUnwindMetaWithoutFieldPath) {
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

TEST_F(PipelineOptimizationTest, LimitDuplicatesBeforeUnwindWithPreserveNull) {
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

TEST_F(PipelineOptimizationTest, LimitDoesNotDuplicatesBeforeUnwindWithoutPreserveNull) {
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

TEST_F(PipelineOptimizationTest, LimitDuplicatesBeforeSortUnwindAndIsMergedWithSort) {
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

    auto pipeline = assertPipelineOptimizesTo(inputPipe, outputPipe);
    assertPipelineSerializesTo(*pipeline, boost::none, serializedPipe);

    SerializationOptions options{.serializeForCloning = true};
    serializedPipe =
        "[{$sort: {b: 1, $_internalLimit: 100}}"
        ",{$unwind : {path: '$a', preserveNullAndEmptyArrays: true}}"
        ",{$limit : 100}"
        "]";
    assertPipelineSerializesTo(*pipeline, options, serializedPipe);
}

TEST_F(PipelineOptimizationTest, SortAndLimitSwapsBeforeUnwindAndMerges) {
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

    auto pipeline = assertPipelineOptimizesTo(inputPipe, outputPipe);
    assertPipelineSerializesTo(*pipeline, boost::none, serializedPipe);

    SerializationOptions options{.serializeForCloning = true};
    serializedPipe =
        "[{$sort : {b: 1, $_internalLimit: 5}}"
        ",{$unwind : {path: '$a', preserveNullAndEmptyArrays: true}}"
        ",{$limit : 5}"
        "]";
    assertPipelineSerializesTo(*pipeline, options, serializedPipe);
}

TEST_F(PipelineOptimizationTest, UnwindLimitLimitPushesSmallestLimitBack) {
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

TEST_F(PipelineOptimizationTest, SortMatchProjSkipLimBecomesMatchTopKSortSkipProj) {
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

    auto pipeline = assertPipelineOptimizesTo(inputPipe, outputPipe);
    assertPipelineSerializesTo(*pipeline, boost::none, serializedPipe);

    SerializationOptions options{.serializeForCloning = true};
    serializedPipe =
        "[{$match: {a: 1}}"
        ",{$sort: {a: 1, $_internalLimit: 8}}"
        ",{$skip: 3}"
        ",{$project: {_id: true, a: true}}"
        "]";
    assertPipelineSerializesTo(*pipeline, options, serializedPipe);
}

TEST_F(PipelineOptimizationTest, SortMatchWithExprProjSkipLimBecomesMatchTopKSortSkipProj) {
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

    auto pipeline = assertPipelineOptimizesTo(inputPipe, outputPipe);
    assertPipelineSerializesTo(*pipeline, boost::none, serializedPipe);

    SerializationOptions options{.serializeForCloning = true};
    serializedPipe =
        "[{$match: {$expr: {$eq: ['$a', 1]}}}"
        ",{$sort: {a: 1, $_internalLimit: 8}}"
        ",{$skip : 3}"
        ",{$project : {_id: true, a: true}}"
        "]";
    assertPipelineSerializesTo(*pipeline, options, serializedPipe);
}

TEST_F(PipelineOptimizationTest, IdenticalSortSortBecomesSort) {
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

TEST_F(PipelineOptimizationTest, IdenticalSortSortSortBecomesSort) {
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

TEST_F(PipelineOptimizationTest, NonIdenticalSortsOnlySortOnFinalKey) {
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

TEST_F(PipelineOptimizationTest, SortSortLimitBecomesFinalKeyTopKSort) {
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

    auto pipeline = assertPipelineOptimizesTo(inputPipe, outputPipe);
    assertPipelineSerializesTo(*pipeline, boost::none, serializedPipe);

    SerializationOptions options{.serializeForCloning = true};
    serializedPipe =
        "[{$sort: {a: 1, $_internalLimit: 5}}"
        "]";
    assertPipelineSerializesTo(*pipeline, options, serializedPipe);
}

TEST_F(PipelineOptimizationTest, SortSortSkipLimitBecomesTopKSortSkip) {
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

    auto pipeline = assertPipelineOptimizesTo(inputPipe, outputPipe);
    assertPipelineSerializesTo(*pipeline, boost::none, serializedPipe);

    SerializationOptions options{.serializeForCloning = true};
    serializedPipe =
        "[{$sort: {a: 1, $_internalLimit: 8}}"
        ",{$skip : 3}"
        "]";
    assertPipelineSerializesTo(*pipeline, options, serializedPipe);
}

TEST_F(PipelineOptimizationTest, SortLimitSortLimitBecomesTopKSort) {
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

    auto pipeline = assertPipelineOptimizesTo(inputPipe, outputPipe);
    assertPipelineSerializesTo(*pipeline, boost::none, serializedPipe);

    SerializationOptions options{.serializeForCloning = true};
    serializedPipe =
        "[{$sort: {a: 1, $_internalLimit: 12}}"
        "]";
    assertPipelineSerializesTo(*pipeline, options, serializedPipe);
}

TEST_F(PipelineOptimizationTest, SortLimitSortRetainsLimit) {
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

    auto pipeline = assertPipelineOptimizesTo(inputPipe, outputPipe);
    assertPipelineSerializesTo(*pipeline, boost::none, serializedPipe);

    SerializationOptions options{.serializeForCloning = true};
    serializedPipe =
        "[{$sort: {a: 1, $_internalLimit: 12}}"
        "]";
    assertPipelineSerializesTo(*pipeline, options, serializedPipe);
}

TEST_F(PipelineOptimizationTest, SortLimitSortWithDifferentSortPatterns) {
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

    auto pipeline = assertPipelineOptimizesTo(inputPipe, outputPipe);
    assertPipelineSerializesTo(*pipeline, boost::none, serializedPipe);

    SerializationOptions options{.serializeForCloning = true};
    serializedPipe =
        "[{$sort: {a: 1, $_internalLimit: 12}}"
        ",{$sort: {b: 1}}"
        "]";
    assertPipelineSerializesTo(*pipeline, options, serializedPipe);
}
TEST_F(PipelineOptimizationTest, SortSortLimitRetainsLimit) {
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

    auto pipeline = assertPipelineOptimizesTo(inputPipe, outputPipe);
    assertPipelineSerializesTo(*pipeline, boost::none, serializedPipe);

    SerializationOptions options{.serializeForCloning = true};
    serializedPipe =
        "[{$sort: {a: 1, $_internalLimit: 20}}"
        "]";
    assertPipelineSerializesTo(*pipeline, options, serializedPipe);
}

TEST_F(PipelineOptimizationTest, SortSortSortMatchProjSkipLimBecomesMatchTopKSortSkipProj) {
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

    auto pipeline = assertPipelineOptimizesTo(inputPipe, outputPipe);
    assertPipelineSerializesTo(*pipeline, boost::none, serializedPipe);

    SerializationOptions options{.serializeForCloning = true};
    serializedPipe =
        "[{$match: {a: 1}}"
        ",{$sort: {a: 1, $_internalLimit: 8}}"
        ",{$skip : 3}"
        ",{$project : {_id: true, a: true}}"
        "]";
    assertPipelineSerializesTo(*pipeline, options, serializedPipe);
}

TEST_F(PipelineOptimizationTest, SortSortSortMatchOnExprProjSkipLimBecomesMatchTopKSortSkipProj) {
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

    auto pipeline = assertPipelineOptimizesTo(inputPipe, outputPipe);
    assertPipelineSerializesTo(*pipeline, boost::none, serializedPipe);

    SerializationOptions options{.serializeForCloning = true};
    serializedPipe =
        "[{$match: {$expr: {$eq: ['$a', 1]}}}"
        ",{$sort: {a: 1, $_internalLimit: 8}}"
        ",{$skip : 3}"
        ",{$project : {_id: true, a: true}}"
        "]";

    assertPipelineSerializesTo(*pipeline, options, serializedPipe);
}

TEST_F(PipelineOptimizationTest, NonIdenticalSortsBecomeFinalKeyTopKSort) {
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

    auto pipeline = assertPipelineOptimizesTo(inputPipe, outputPipe);
    assertPipelineSerializesTo(*pipeline, boost::none, serializedPipe);

    SerializationOptions options{.serializeForCloning = true};
    serializedPipe =
        "[{$sort: {a: 1, $_internalLimit: 5}}"
        ",{$project : {_id: true, a: true}}"
        "]";

    assertPipelineSerializesTo(*pipeline, options, serializedPipe);
}

TEST_F(PipelineOptimizationTest, SubsequentSortsMergeAndBecomeTopKSortWithFinalKeyAndLowestLimit) {
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

    auto pipeline = assertPipelineOptimizesTo(inputPipe, outputPipe);
    assertPipelineSerializesTo(*pipeline, boost::none, serializedPipe);

    SerializationOptions options{.serializeForCloning = true};
    serializedPipe =
        "[{$sort: {a: -1, $_internalLimit: 7}}"
        ",{$project : {_id: true, a: true}}"
        ",{$unwind: {path: '$a'}}"
        "]";

    assertPipelineSerializesTo(*pipeline, options, serializedPipe);
}

TEST_F(PipelineOptimizationTest, RemoveSkipZero) {
    assertPipelineOptimizesAndSerializesTo("[{$skip: 0}]", "[]");
}

TEST_F(PipelineOptimizationTest, DoNotRemoveSkipOne) {
    assertPipelineOptimizesAndSerializesTo("[{$skip: 1}]", "[{$skip: 1}]");
}

TEST_F(PipelineOptimizationTest, RemoveReplaceRootWithRoot) {
    assertPipelineOptimizesAndSerializesTo("[{$replaceRoot: {newRoot: '$$ROOT'}}]", "[]");
}

TEST_F(PipelineOptimizationTest, RemoveReplaceRootWithRootAfterExprIsOptimized) {
    assertPipelineOptimizesAndSerializesTo(
        "[{$replaceRoot: {newRoot: {$cond: {if: true, then: '$$ROOT', else: '$$ROOT'}}}}]", "[]");
}

TEST_F(PipelineOptimizationTest, DoNotRemoveReplaceRootWithNonRoot) {
    assertPipelineOptimizesAndSerializesTo("[{$replaceRoot: {newRoot: {notTheRoot: '$$ROOT'}}}]",
                                           "[{$replaceRoot: {newRoot: {notTheRoot: '$$ROOT'}}}]");
}

TEST_F(PipelineOptimizationTest, RemoveReplaceWithRoot) {
    assertPipelineOptimizesAndSerializesTo("[{$replaceWith: '$$ROOT'}]", "[]");
}

TEST_F(PipelineOptimizationTest, OptimizeOutReplaceRootInMultiStagePipeline) {
    std::string inputPipe =
        "["
        " {$replaceWith: '$$ROOT'},"
        " {$match: {x: 2}}"
        "]";
    std::string outputPipe = "[{$match: {x: {$eq: 2}}}]";
    std::string serializedPipe = "[{$match: {x: 2}}]";
    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe, serializedPipe);
}

TEST_F(PipelineOptimizationTest, RemoveEmptyMatch) {
    assertPipelineOptimizesAndSerializesTo("[{$match: {}}]", "[]");
}

TEST_F(PipelineOptimizationTest, RemoveMultipleEmptyMatches) {
    std::string inputPipe = "[{$match: {}}, {$match: {}}]";

    std::string outputPipe = "[{$match: {}}]";

    std::string serializedPipe = "[{$match: {$and: [{}, {}]}}]";

    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe, serializedPipe);
}

TEST_F(PipelineOptimizationTest, RemoveEmptyMatchesAndKeepNonEmptyMatches) {
    std::string inputPipe = "[{$match: {}}, {$match: {}}, {$match: {a: 1}}]";
    std::string outputPipe = "[{$match: {a: {$eq: 1}}}]";
    std::string serializedPipe = "[{$match: {$and: [{}, {}, {a: 1}]}}]";
    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe, serializedPipe);
}

TEST_F(PipelineOptimizationTest, RemoveEmptyMatchesAndKeepOtherStages) {
    assertPipelineOptimizesAndSerializesTo("[{$match: {}}, {$skip: 1}, {$match: {}}]",
                                           "[{$skip: 1}]");
}

TEST_F(PipelineOptimizationTest, KeepEmptyMatchWithComment) {
    std::string inputPipe = "[{$match: {$comment: 'foo'}}]";
    std::string outputPipe = "[{$match: {}}]";
    std::string serializedPipe = "[{$match: {$comment: 'foo'}}]";
    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe, serializedPipe);
}

TEST_F(PipelineOptimizationTest, DoNotRemoveNonEmptyMatch) {
    std::string inputPipe = "[{$match: {_id: 1}}]";

    std::string outputPipe = "[{$match: {_id: {$eq : 1}}}]";

    std::string serializedPipe = "[{$match: {_id: 1}}]";

    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe, serializedPipe);
}

TEST_F(PipelineOptimizationTest, RemoveMatchWithTrueConstExpr) {
    std::string inputPipe = "[{$match: {$expr: true}}]";
    std::string outputPipe = "[{$match: {}}]";
    std::string serializedPipe = "[{$match: {$expr: true}}]";
    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe, serializedPipe);
}

TEST_F(PipelineOptimizationTest, RemoveMultipleMatchesWithTrueConstExpr) {
    std::string inputPipe = "[{$match: {$expr: true}}, {$match: {$expr: true}}]";
    std::string outputPipe = "[{$match: {}}]";
    std::string serializedPipe = "[{$match: {$and: [{$expr: true}, {$expr: true}]}}]";
    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe, serializedPipe);
}

TEST_F(PipelineOptimizationTest, RemoveMatchWithTruthyConstExpr) {
    std::string inputPipe = "[{$match: {$expr: {$concat: ['a', 'b']}}}]";
    std::string outputPipe = "[{$match: {}}]";
    std::string serializedPipe = "[{$match: {$expr: {$concat: ['a', 'b']}}}]";
    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe, serializedPipe);
}

TEST_F(PipelineOptimizationTest, KeepMatchWithNonConstExpr) {
    assertPipelineOptimizesAndSerializesTo("[{$match: {$expr: {$concat: ['$a', '$b']}}}]",
                                           "[{$match: {$expr: {$concat: ['$a', '$b']}}}]");
}

TEST_F(PipelineOptimizationTest, MoveMatchBeforeSort) {
    std::string inputPipe = "[{$sort: {b: 1}}, {$match: {a: 2}}]";
    std::string outputPipe = "[{$match: {a: {$eq : 2}}}, {$sort: {sortKey: {b: 1}}}]";
    std::string serializedPipe = "[{$match: {a: 2}}, {$sort: {b: 1}}]";
    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe, serializedPipe);
}

TEST_F(PipelineOptimizationTest, LookupMoveSortNotOnAsBefore) {
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

TEST_F(PipelineOptimizationTest, LookupMoveSortOnPrefixStringOfAsBefore) {
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

TEST_F(PipelineOptimizationTest, LookupShouldNotMoveSortOnAsBefore) {
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

TEST_F(PipelineOptimizationTest, LookupShouldNotMoveSortOnPathPrefixOfAsBefore) {
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

TEST_F(PipelineOptimizationTest, LookupUnwindShouldNotMoveSortBefore) {
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

TEST_F(PipelineOptimizationTest, MoveMatchOnExprBeforeSort) {
    std::string inputPipe = "[{$sort: {b: 1}}, {$match: {$expr: {$eq: ['$a', 2]}}}]";
    std::string outputPipe =
        "[{$match: {$and: [{$expr: {$eq: ['$a', {$const: 2}]}},"
        "                  {a: {$_internalExprEq: 2}}]}},"
        " {$sort: {sortKey: {b: 1}}}]";
    std::string serializedPipe = "[{$match: {$expr: {$eq: ['$a', 2]}}}, {$sort: {b: 1}}]";
    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe, serializedPipe);
}

TEST_F(PipelineOptimizationTest, LookupShouldCoalesceWithUnwindOnAs) {
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
    auto pipeline = assertPipelineOptimizesTo(inputPipe, outputPipe);
    assertPipelineSerializesTo(*pipeline, boost::none, serializedPipe);
    SerializationOptions options{.serializeForCloning = true};
    serializedPipe =
        "[{$lookup: {from : 'lookupColl', as : 'same', localField: 'left', foreignField: "
        "'right', $_internalUnwind: {$unwind: {path: '$same'}}}}]";
    assertPipelineSerializesTo(*pipeline, options, serializedPipe);
}

TEST_F(PipelineOptimizationTest, LookupWithPipelineSyntaxShouldCoalesceWithUnwindOnAs) {
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

    auto pipeline = assertPipelineOptimizesTo(inputPipe, outputPipe);
    assertPipelineSerializesTo(*pipeline, boost::none, serializedPipe);

    SerializationOptions options{.serializeForCloning = true};
    serializedPipe =
        "[{$lookup: {from : 'lookupColl', as : 'same', let: {}, pipeline: [], "
        "$_internalUnwind: {$unwind: {path: '$same'}}}}"
        "]";
    assertPipelineSerializesTo(*pipeline, options, serializedPipe);
}

TEST_F(PipelineOptimizationTest, LookupShouldCoalesceWithUnwindOnAsWithPreserveEmpty) {
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

    auto pipeline = assertPipelineOptimizesTo(inputPipe, outputPipe);
    assertPipelineSerializesTo(*pipeline, boost::none, serializedPipe);

    SerializationOptions options{.serializeForCloning = true};
    serializedPipe =
        "[{$lookup: {from : 'lookupColl', as : 'same', localField: 'left', foreignField: "
        "'right', "
        "$_internalUnwind: {$unwind: {path: '$same', preserveNullAndEmptyArrays: true}}}}]";
    assertPipelineSerializesTo(*pipeline, options, serializedPipe);
}

TEST_F(PipelineOptimizationTest, LookupShouldCoalesceWithUnwindOnAsWithIncludeArrayIndex) {
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

    auto pipeline = assertPipelineOptimizesTo(inputPipe, outputPipe);
    assertPipelineSerializesTo(*pipeline, boost::none, serializedPipe);

    SerializationOptions options{.serializeForCloning = true};
    serializedPipe =
        "[{$lookup: {from : 'lookupColl', as : 'same', localField: 'left', foreignField: "
        "'right', "
        "$_internalUnwind: {$unwind: {path: '$same', includeArrayIndex: 'index'}}}}]";
    assertPipelineSerializesTo(*pipeline, options, serializedPipe);
}

TEST_F(PipelineOptimizationTest, LookupShouldNotCoalesceWithUnwindNotOnAs) {
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
    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe);
}

TEST_F(PipelineOptimizationTest, LookupWithPipelineSyntaxShouldNotCoalesceWithUnwindNotOnAs) {
    std::string inputPipe =
        "[{$lookup: {from : 'lookupColl', as : 'same', pipeline: []}}"
        ",{$unwind: {path: '$from'}}"
        "]";
    std::string outputPipe =
        "[{$lookup: {from : 'lookupColl', as : 'same', let: {}, pipeline: []}}"
        ",{$unwind: {path: '$from'}}"
        "]";
    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe);
}

TEST_F(PipelineOptimizationTest, LookupShouldSwapWithMatch) {
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

    auto pipeline = assertPipelineOptimizesTo(inputPipe, outputPipe);
    assertPipelineSerializesTo(*pipeline, boost::none, serializedPipe);
    SerializationOptions options{.serializeForCloning = true};
    assertPipelineSerializesTo(*pipeline, options, serializedPipe);
}

TEST_F(PipelineOptimizationTest, LookupShouldSwapWithMatchOnExpr) {
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

    auto pipeline = assertPipelineOptimizesTo(inputPipe, outputPipe);
    assertPipelineSerializesTo(*pipeline, boost::none, serializedPipe);
    SerializationOptions options{.serializeForCloning = true};
    assertPipelineSerializesTo(*pipeline, options, serializedPipe);
}

TEST_F(PipelineOptimizationTest, LookupWithPipelineSyntaxShouldSwapWithMatch) {
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

TEST_F(PipelineOptimizationTest, LookupWithPipelineSyntaxShouldSwapWithMatchOnExpr) {
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

    auto pipeline = assertPipelineOptimizesTo(inputPipe, outputPipe);
    assertPipelineSerializesTo(*pipeline, boost::none, serializedPipe);
    SerializationOptions options{.serializeForCloning = true};
    assertPipelineSerializesTo(*pipeline, options, serializedPipe);
}

TEST_F(PipelineOptimizationTest, LookupShouldSplitMatch) {
    std::string inputPipe =
        "[{$lookup: {from: 'lookupColl', as: 'asField', localField: 'y', foreignField: "
        "'z'}}, "
        " {$match: {'independent': 0, asField: {$eq: 3}}}]";
    std::string outputPipe =
        "[{$match: {independent: {$eq: 0}}}, "
        " {$lookup: {from: 'lookupColl', as: 'asField', localField: 'y', foreignField: "
        "'z'}}, "
        " {$match: {asField: {$eq: 3}}}]";
    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe);
}

TEST_F(PipelineOptimizationTest, LookupShouldNotAbsorbMatchOnAs) {
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

    auto pipeline = assertPipelineOptimizesTo(inputPipe, outputPipe);
    assertPipelineSerializesTo(*pipeline, boost::none, serializedPipe);
    SerializationOptions options{.serializeForCloning = true};
    assertPipelineSerializesTo(*pipeline, options, serializedPipe);
}

TEST_F(PipelineOptimizationTest, LookupShouldNotAbsorbMatchWithExprOnAs) {
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

    auto pipeline = assertPipelineOptimizesTo(inputPipe, outputPipe);
    assertPipelineSerializesTo(*pipeline, boost::none, serializedPipe);
    SerializationOptions options{.serializeForCloning = true};
    assertPipelineSerializesTo(*pipeline, options, serializedPipe);
}

TEST_F(PipelineOptimizationTest, LookupShouldAbsorbUnwindMatch) {
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

    auto pipeline = assertPipelineOptimizesTo(inputPipe, outputPipe);
    assertPipelineSerializesTo(*pipeline, boost::none, serializedPipe);

    SerializationOptions options{.serializeForCloning = true};
    serializedPipe =
        "[{$lookup: {from: 'lookupColl', as: 'asField', localField: 'y', foreignField: "
        "'z',  let: {}, pipeline: [{$match: {subfield: {$eq: 1}}}], "
        "$_internalUnwind: {$unwind: {path: '$asField'}}}}]";
    assertPipelineSerializesTo(*pipeline, options, serializedPipe);
}

TEST_F(PipelineOptimizationTest, LookupShouldAbsorbUnwindAndTypeMatch) {
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

    auto pipeline = assertPipelineOptimizesTo(inputPipe, outputPipe);
    assertPipelineSerializesTo(*pipeline, boost::none, serializedPipe);

    SerializationOptions options{.serializeForCloning = true};
    serializedPipe =
        "[{$lookup: {from: 'lookupColl', as: 'asField', localField: 'y', foreignField: "
        "'z', let: {}, pipeline: [{$match: {subfield: {$type: [2]}}}], "
        "$_internalUnwind: {$unwind: {path: '$asField'}}}}]";
    assertPipelineSerializesTo(*pipeline, options, serializedPipe);
}

TEST_F(PipelineOptimizationTest, LookupWithPipelineSyntaxShouldAbsorbUnwindMatch) {
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

    auto pipeline = assertPipelineOptimizesTo(inputPipe, outputPipe);
    assertPipelineSerializesTo(*pipeline, boost::none, serializedPipe);

    SerializationOptions options{.serializeForCloning = true};
    serializedPipe =
        "[{$lookup: {from: 'lookupColl', as: 'asField', let: {}, "
        "pipeline: [{$match: {subfield: {$eq: 1}}}], "
        "$_internalUnwind: {$unwind: {path: '$asField'}}}}]";
    assertPipelineSerializesTo(*pipeline, options, serializedPipe);
}

TEST_F(PipelineOptimizationTest, LookupWithPipelineSyntaxShouldAbsorbUnwindAndTwoMatch) {
    std::string inputPipe =
        "[{$lookup: {from: 'lookupColl', as: 'asField', pipeline: [{$match: {subfield1: {$eq: "
        "1}}}]}}, "
        "{$unwind: '$asField'}, "
        "{$match: {'asField.subfield2': {$eq: 1}}}, "
        "{$match: {'asField.subfield3': {$eq: 1}}}]";
    std::string outputPipe =
        "[{$lookup: {from: 'lookupColl', as: 'asField', let: {}, "
        "pipeline: [{$match: {subfield1: {$eq: 1}}}, {$match: {$and: [{subfield2: {$eq: 1}}, "
        "{subfield3: {$eq: 1}}]}}], "
        "unwinding: {preserveNullAndEmptyArrays: false} } } ]";
    std::string serializedPipe =
        "[{$lookup: {from: 'lookupColl', as: 'asField', let: {}, "
        "pipeline: [{$match: {subfield1: {$eq: 1}}}, {$match: {$and: [{subfield2: {$eq: 1}}, "
        "{subfield3: {$eq: 1}}]}}]}}, "
        "{$unwind: {path: '$asField'}}]";

    auto pipeline = assertPipelineOptimizesTo(inputPipe, outputPipe);
    assertPipelineSerializesTo(*pipeline, boost::none, serializedPipe);

    SerializationOptions options{.serializeForCloning = true};
    serializedPipe =
        "[{$lookup: {from: 'lookupColl', as: 'asField', let: {}, "
        "pipeline: [{$match: {subfield1: {$eq: 1}}}, {$match: {$and: [{subfield2: {$eq: 1}}, "
        "{subfield3: {$eq: 1}}]}}], "
        "$_internalUnwind: {$unwind: {path: '$asField'}}}}]";
    assertPipelineSerializesTo(*pipeline, options, serializedPipe);
}

TEST_F(PipelineOptimizationTest, LookupShouldAbsorbUnwindAndSplitAndAbsorbMatch) {
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

    auto pipeline = assertPipelineOptimizesTo(inputPipe, outputPipe);
    assertPipelineSerializesTo(*pipeline, boost::none, serializedPipe);

    SerializationOptions options{.serializeForCloning = true};
    serializedPipe =
        "[{$match: {independentField: {$gt: 2}}}, "
        " {$lookup: {from: 'lookupColl', as: 'asField', localField: 'y', foreignField: "
        "'z', let: {}, pipeline: [{$match: {subfield: {$eq: 1}}}], "
        "$_internalUnwind: {$unwind: {path: '$asField'}}}}]";
    assertPipelineSerializesTo(*pipeline, options, serializedPipe);
}

TEST_F(PipelineOptimizationTest, LookupShouldNotSplitIndependentAndDependentOrClauses) {
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

    auto pipeline = assertPipelineOptimizesTo(inputPipe, outputPipe);
    assertPipelineSerializesTo(*pipeline, boost::none, serializedPipe);

    SerializationOptions options{.serializeForCloning = true};
    serializedPipe =
        "[{$lookup: {from: 'lookupColl', as: 'asField', localField: 'y', foreignField: "
        "'z', $_internalUnwind: {$unwind: {path: '$asField'}}}},"
        " {$match: {$or: [{'independent': {$gt: 4}}, "
        "                 {'asField.dependent': {$elemMatch: {a: {$eq: 1}}}}]}}]";
    assertPipelineSerializesTo(*pipeline, options, serializedPipe);
}

TEST_F(PipelineOptimizationTest, LookupWithMatchOnArrayIndexFieldShouldNotCoalesce) {
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

    auto pipeline = assertPipelineOptimizesTo(inputPipe, outputPipe);
    assertPipelineSerializesTo(*pipeline, boost::none, serializedPipe);

    SerializationOptions options{.serializeForCloning = true};
    serializedPipe =
        "[{$match: {independent: {$eq: 1}}}, "
        " {$lookup: {from: 'lookupColl', as: 'asField', localField: 'y', foreignField: "
        "'z', $_internalUnwind: {$unwind: {path: '$asField', includeArrayIndex: 'index'}}}}, "
        " {$match: {$and: [{index: {$eq: 0}}, {'asField.value': {$gt: 0}}]}}]";
    assertPipelineSerializesTo(*pipeline, options, serializedPipe);
}

TEST_F(PipelineOptimizationTest, LookupWithUnwindPreservingNullAndEmptyArraysShouldNotCoalesce) {
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

    auto pipeline = assertPipelineOptimizesTo(inputPipe, outputPipe);
    assertPipelineSerializesTo(*pipeline, boost::none, serializedPipe);

    SerializationOptions options{.serializeForCloning = true};
    serializedPipe =
        "[{$match: {independent: {$eq: 1}}}, "
        " {$lookup: {from: 'lookupColl', as: 'asField', localField: 'y', foreignField: "
        "'z', $_internalUnwind: {$unwind: {path: '$asField', preserveNullAndEmptyArrays: true}}}}, "
        " {$match: {'asField.value': {$gt: 0}}}]";
    assertPipelineSerializesTo(*pipeline, options, serializedPipe);
}

TEST_F(PipelineOptimizationTest, LookupDoesNotAbsorbElemMatch) {
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

    auto pipeline = assertPipelineOptimizesTo(inputPipe, outputPipe);
    assertPipelineSerializesTo(*pipeline, boost::none, serializedPipe);

    SerializationOptions options{.serializeForCloning = true};
    serializedPipe =
        "[{$lookup: {from: 'lookupColl', as: 'x', localField: 'y', foreignField: 'z', "
        " $_internalUnwind: {$unwind: {path: '$x'}}}}, "
        " {$match: {x: {$elemMatch: {a: 1}}}}]";
    assertPipelineSerializesTo(*pipeline, options, serializedPipe);
}

TEST_F(PipelineOptimizationTest, LookupDoesSwapWithMatchOnLocalField) {
    std::string inputPipe =
        "[{$lookup: {from: 'lookupColl', as: 'x', localField: 'y', foreignField: 'z'}}, "
        " {$match: {y: {$eq: 3}}}]";
    std::string outputPipe =
        "[{$match: {y: {$eq: 3}}}, "
        " {$lookup: {from: 'lookupColl', as: 'x', localField: 'y', foreignField: 'z'}}]";
    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe);
}

TEST_F(PipelineOptimizationTest, LookupDoesSwapWithMatchOnFieldWithSameNameAsForeignField) {
    std::string inputPipe =
        "[{$lookup: {from: 'lookupColl', as: 'x', localField: 'y', foreignField: 'z'}}, "
        " {$match: {z: {$eq: 3}}}]";
    std::string outputPipe =
        "[{$match: {z: {$eq: 3}}}, "
        " {$lookup: {from: 'lookupColl', as: 'x', localField: 'y', foreignField: 'z'}}]";
    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe);
}

TEST_F(PipelineOptimizationTest, LookupDoesNotAbsorbUnwindOnSubfieldOfAsButStillMovesMatch) {
    std::string inputPipe =
        "[{$lookup: {from: 'lookupColl', as: 'x', localField: 'y', foreignField: 'z'}}, "
        " {$unwind: {path: '$x.subfield'}}, "
        " {$match: {'independent': 2, 'x.dependent': 2}}]";
    std::string outputPipe =
        "[{$match: {'independent': {$eq: 2}}}, "
        " {$lookup: {from: 'lookupColl', as: 'x', localField: 'y', foreignField: 'z'}}, "
        " {$match: {'x.dependent': {$eq: 2}}}, "
        " {$unwind: {path: '$x.subfield'}}]";
    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe);
}

TEST_F(PipelineOptimizationTest, GroupShouldSwapWithMatchIfFilteringOnID) {
    std::string inputPipe =
        "[{$group : {_id:'$a'}}, "
        " {$match: {_id : 4}}]";
    std::string outputPipe =
        "[{$match: {a:{$eq : 4}}}, "
        " {$group:{_id:'$a', $willBeMerged: false}}]";
    std::string serializedPipe =
        "[{$match: {a:{$eq :4}}}, "
        " {$group:{_id:'$a', $willBeMerged: false}}]";

    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe, serializedPipe);
}

/* ----- GROUP & MATCH ----- */
/* ----- GROUP & MATCH : Nonexistence/nontype queries ----- */
TEST_F(PipelineOptimizationTest, GroupShouldSwapWithMatchOnExprIfFilteringOnID) {
    std::string inputPipe =
        "[{$group: {_id: '$a'}}, "
        " {$match: {$expr: {$eq: ['$_id', 4]}}}]";
    std::string outputPipe =
        "[{$match: {$and: [{$expr: {$eq: ['$a', {$const: 4}]}}, {a: {$_internalExprEq: 4}}]}},"
        " {$group: {_id: '$a', $willBeMerged: false}}]";
    std::string serializedPipe =
        "[{$match: {$expr: {$eq: ['$a', {$const: 4}]}}}, "
        " {$group: {_id: '$a', $willBeMerged: false}}]";

    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe, serializedPipe);
}

TEST_F(PipelineOptimizationTest, GroupShouldNotSwapWithMatchOnExprIfNotFilteringOnID) {
    std::string inputPipe =
        "[{$group : {_id:'$a'}}, "
        " {$match: {$expr: {$eq: ['$b', 4]}}}]";
    std::string outputPipe =
        "[{$group : {_id:'$a', $willBeMerged: false}}, "
        " {$match: {$and: [{$expr: {$eq: ['$b', {$const: 4}]}}, {b: {$_internalExprEq: 4}}]}}]";
    std::string serializedPipe =
        "[{$group : {_id:'$a', $willBeMerged: false}}, "
        " {$match: {$expr: {$eq: ['$b', 4]}}}]";

    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe, serializedPipe);
}

TEST_F(PipelineOptimizationTest, GroupShouldSwapWithCompoundMatchIfFilteringOnID) {
    std::string inputPipe =
        "[{$group : {_id:'$x'}}, "
        " {$match: {$or : [ {_id : {$lte : 50}}, {_id : {$gt : 70}}]}}]";
    std::string outputPipe =
        "[{$match: {$or : [  {x : {$lte : 50}}, {x : {$gt : 70}}]}},"
        "{$group : {_id:'$x', $willBeMerged: false}}]";
    std::string serializedPipe =
        "[{$match: {$or : [  {x : {$lte : 50}}, {x : {$gt : 70}}]}},"
        "{$group : {_id:'$x', $willBeMerged: false}}]";

    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe, serializedPipe);
}

TEST_F(PipelineOptimizationTest, GroupShouldNotSwapWithMatchIfNotFilteringOnID) {
    std::string inputPipe =
        "[{$group : {_id:'$a'}}, "
        " {$match: {b : 4}}]";
    std::string outputPipe =
        "[{$group : {_id:'$a', $willBeMerged: false}}, "
        " {$match: {b : {$eq: 4}}}]";
    std::string serializedPipe =
        "[{$group : {_id:'$a', $willBeMerged: false}}, "
        " {$match: {b : 4}}]";

    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe, serializedPipe);
}

/* ----- GROUP & MATCH : Exsistence queries ----- */
TEST_F(PipelineOptimizationTest, GroupShouldntSwapWithMatchIfFilteringByExistenceOnSemiCompoundID) {
    std::string inputPipe =
        "[{$group : {_id: {a: '$a'}}}, "
        " {$match: {'_id.a': {$exists: true}}}]";
    std::string outputPipe =
        "[{$group : {_id: {a: '$a'}, $willBeMerged: false}}, "
        " {$match: {'_id.a': {$exists: true}}}]";
    std::string serializedPipe =
        "[{$group : {_id: {a: '$a'}, $willBeMerged: false}}, "
        " {$match: {'_id.a': {$exists: true}}}]";

    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe, serializedPipe);
}

TEST_F(PipelineOptimizationTest, GroupShouldSwapWithMatchIfFilteringByExistenceOnCompoundID) {
    std::string inputPipe =
        "[{$group : {_id: {a: '$a', b: '$b'}}}, "
        " {$match: {'_id.b': {$exists: true}}}]";
    std::string outputPipe =
        "[{$match: {'b': {$exists: true}}}, "
        " {$group : {_id: {a: '$a', b: '$b'}, $willBeMerged: false}}]";
    std::string serializedPipe =
        "[{$match: {'b': {$exists: true}}}, "
        " {$group : {_id: {a: '$a', b: '$b'}, $willBeMerged: false}}]";

    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe, serializedPipe);
}

TEST_F(PipelineOptimizationTest, GroupShouldNotSwapWithMatchIfExistsPredicateOnID) {
    std::string inputPipe =
        "[{$group : {_id:'$x'}}, "
        " {$match: {_id : {$exists: true}}}]";
    std::string outputPipe =
        "[{$group : {_id:'$x', $willBeMerged: false}}, "
        " {$match: {_id : {$exists: true}}}]";
    std::string serializedPipe =
        "[{$group : {_id:'$x', $willBeMerged: false}}, "
        " {$match: {_id : {$exists: true}}}]";

    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe, serializedPipe);
}

TEST_F(PipelineOptimizationTest, GroupShouldNotSwapWithCompoundMatchIfExistsPredicateOnID) {
    std::string inputPipe =
        "[{$group : {_id:'$x'}}, "
        " {$match: {$or : [ {_id : {$exists: true}}, {_id : {$gt : 70}}]}}]";
    std::string outputPipe =
        "[{$group : {_id:'$x', $willBeMerged: false}}, "
        " {$match: {$or : [ {_id : {$gt : 70}}, {_id : {$exists: true}}]}}]";
    std::string serializedPipe =
        "[{$group : {_id:'$x', $willBeMerged: false}}, "
        " {$match: {$or : [ {_id : {$exists: true}}, {_id : {$gt : 70}}]}}]";

    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe, serializedPipe);
}

/* ----- GROUP & MATCH : Type queries ----- */
TEST_F(PipelineOptimizationTest, GroupShouldNotSwapWithMatchIfFilteringByTypeOnCompoundID) {
    std::string inputPipe =
        "[{$group : {_id: {a: '$a', b: '$b'}}}, "
        " {$match: {$or: [{'_id.a' : 4}, {'_id.b': {$type: [5]}}]}}]";
    std::string outputPipe =
        "[{$group : {_id: {a: '$a', b: '$b'}, $willBeMerged: false}}, "
        " {$match: {$or: [{'_id.a' : {$eq: 4}}, {'_id.b': {$type: [5]}}]}}]";
    std::string serializedPipe =
        "[{$group : {_id: {a: '$a', b: '$b'}, $willBeMerged: false}}, "
        " {$match: {$or: [{'_id.a' : 4}, {'_id.b': {$type: [5]}}]}}]";

    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe, serializedPipe);
}

TEST_F(PipelineOptimizationTest, GroupShouldNotSwapWithMatchIfFilteringByTypeOnCompoundIDSimple) {
    std::string inputPipe =
        "[{$group : {_id: {a: '$a', b: '$b'}}}, "
        " {$match: {'_id.b': {$type: [5]}}}]";
    std::string outputPipe =
        "[{$group : {_id: {a: '$a', b: '$b'}, $willBeMerged: false}}, "
        " {$match: {'_id.b': {$type: [5]}}}]";
    std::string serializedPipe =
        "[{$group : {_id: {a: '$a', b: '$b'}, $willBeMerged: false}}, "
        " {$match: {'_id.b': {$type: [5]}}}]";

    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe, serializedPipe);
}

TEST_F(PipelineOptimizationTest, GroupShouldNotSwapWithMatchIfTypePredicateOnID) {
    std::string inputPipe =
        "[{$group : {_id:'$x'}}, "
        " {$match: {_id : {$type: [1]}}}]";
    std::string outputPipe =
        "[{$group : {_id:'$x', $willBeMerged: false}}, "
        " {$match: {_id : {$type: [1]}}}]";
    std::string serializedPipe =
        "[{$group : {_id:'$x', $willBeMerged: false}}, "
        " {$match: {_id : {$type: [1]}}}]";

    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe, serializedPipe);
}

TEST_F(PipelineOptimizationTest, GroupShouldNotSwapWithCompoundMatchIfTypePredicateOnID) {
    std::string inputPipe =
        "[{$group : {_id:'$x'}}, "
        " {$match: {$or : [ {_id : {$type: [18]}}, {_id : {$gt : 70}}]}}]";
    std::string outputPipe =
        "[{$group : {_id:'$x', $willBeMerged: false}}, "
        " {$match: {$or : [ {_id : {$gt : 70}}, {_id : {$type: [18]}}]}}]";
    std::string serializedPipe =
        "[{$group : {_id:'$x', $willBeMerged: false}}, "
        " {$match: {$or : [ {_id : {$type: [18]}}, {_id : {$gt : 70}}]}}]";

    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe, serializedPipe);
}

TEST_F(PipelineOptimizationTest, MatchShouldDuplicateItselfBeforeRedact) {
    std::string inputPipe = "[{$redact: '$$PRUNE'}, {$match: {a: 1, b:12}}]";
    std::string outputPipe =
        "[{$match: {$and: [{a: {$eq: 1}}, {b: {$eq: 12}}]}}, {$redact: '$$PRUNE'}, "
        "{$match: {$and: [{a: {$eq: 1}}, {b: {$eq: 12}}]}}]";
    std::string serializedPipe =
        "[{$match: {a: 1, b: 12}}, {$redact: '$$PRUNE'}, {$match: {a: 1, b: 12}}]";
    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe, serializedPipe);
}

TEST_F(PipelineOptimizationTest, MatchShouldSwapWithUnwind) {
    std::string inputPipe =
        "[{$unwind: '$a.b.c'}, "
        "{$match: {'b': 1}}]";
    std::string outputPipe =
        "[{$match: {'b': {$eq : 1}}}, "
        "{$unwind: {path: '$a.b.c'}}]";
    std::string serializedPipe = "[{$match: {b: 1}}, {$unwind: {path: '$a.b.c'}}]";
    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe, serializedPipe);
}

TEST_F(PipelineOptimizationTest, MatchOnExprShouldSwapWithUnwind) {
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

TEST_F(PipelineOptimizationTest, MatchOnPrefixShouldNotSwapOnUnwind) {
    std::string inputPipe =
        "[{$unwind: {path: '$a.b.c'}}, "
        "{$match: {'a.b': 1}}]";
    std::string outputPipe =
        "[{$unwind: {path: '$a.b.c'}}, "
        "{$match: {'a.b': {$eq : 1}}}]";
    std::string serializedPipe = "[{$unwind: {path: '$a.b.c'}}, {$match: {'a.b': 1}}]";
    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe, serializedPipe);
}

TEST_F(PipelineOptimizationTest, MatchShouldSplitOnUnwind) {
    std::string inputPipe =
        "[{$unwind: '$a.b'}, "
        "{$match: {$and: [{f: {$eq: 5}}, "
        "                 {$nor: [{'a.d': 1, c: 5}, {'a.b': 3, c: 5}]}]}}]";
    std::string outputPipe =
        "[{$match: {$and: [{f: {$eq: 5}},"
        "                  {$nor: [{$and: [{'a.d': {$eq: 1}}, {c: {$eq: 5}}]}]}]}},"
        "{$unwind: {path: '$a.b'}}, "
        "{$match: {$nor: [{$and: [{'a.b': {$eq: 3}}, {c: {$eq: 5}}]}]}}]";
    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe);
}

// The 'a.b' path is a modified one by $unwind and $elemMatch is dependent on it and so we can't
// swap $elemMatch in this case.
TEST_F(PipelineOptimizationTest, MatchShouldNotOptimizeWithElemMatchOnModifiedPathByUnwind) {
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
TEST_F(PipelineOptimizationTest, MatchShouldNotOptimizeWithElemMatchOnModifiedPathByProject1) {
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
TEST_F(PipelineOptimizationTest, MatchShouldNotOptimizeWithElemMatchOnModifiedPathByProject2) {
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
TEST_F(PipelineOptimizationTest, MatchShouldNotOptimizeWithElemMatchOnModifiedPathByProject3) {
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

TEST_F(PipelineOptimizationTest, MatchShouldNotOptimizeWhenMatchingOnIndexField) {
    std::string inputPipe =
        "[{$unwind: {path: '$a', includeArrayIndex: 'foo'}}, "
        " {$match: {foo: 0, b: 1}}]";
    std::string outputPipe =
        "[{$match: {b: {$eq: 1}}}, "
        " {$unwind: {path: '$a', includeArrayIndex: 'foo'}}, "
        " {$match: {foo: {$eq: 0}}}]";
    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe);
}

TEST_F(PipelineOptimizationTest, MatchWithNorOnlySplitsIndependentChildren) {
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

TEST_F(PipelineOptimizationTest, MatchWithOrDoesNotSplit) {
    std::string inputPipe =
        "[{$unwind: {path: '$a'}}, "
        "{$match: {$or: [{a: {$eq: 'dependent'}}, {b: {$eq: 'independent'}}]}}]";
    std::string outputPipe =
        "[{$unwind: {path: '$a'}}, "
        "{$match: {$or: [{a: {$eq: 'dependent'}}, {b: {$eq: 'independent'}}]}}]";
    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe);
}

TEST_F(PipelineOptimizationTest, MatchOnExprWithOrDoesNotSplit) {
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

TEST_F(PipelineOptimizationTest, UnwindBeforeDoubleMatchShouldRepeatedlyOptimize) {
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

TEST_F(PipelineOptimizationTest, GraphLookupShouldCoalesceWithUnwindOnAs) {
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

TEST_F(PipelineOptimizationTest, GraphLookupShouldCoalesceWithUnwindOnAsWithPreserveEmpty) {
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

TEST_F(PipelineOptimizationTest, GraphLookupShouldCoalesceWithUnwindOnAsWithIncludeArrayIndex) {
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

TEST_F(PipelineOptimizationTest, GraphLookupShouldNotCoalesceWithUnwindNotOnAs) {
    std::string inputPipe =
        "[{$graphLookup: {from: 'lookupColl', as: 'out', connectToField: 'b', "
        "                 connectFromField: 'c', startWith: '$d'}}, "
        " {$unwind: '$nottherightthing'}]";

    std::string outputPipe =
        "[{$graphLookup: {from: 'lookupColl', as: 'out', connectToField: 'b', "
        "                 connectFromField: 'c', startWith: '$d'}}, "
        " {$unwind: {path: '$nottherightthing'}}]";
    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe);
}

TEST_F(PipelineOptimizationTest, GraphLookupShouldSwapWithMatch) {
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

TEST_F(PipelineOptimizationTest, GraphLookupShouldSwapWithSortNotOnAs) {
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

TEST_F(PipelineOptimizationTest, GraphLookupWithInternalUnwindShouldNotSwapWithSortNotOnAs) {
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

TEST_F(PipelineOptimizationTest, GraphLookupShouldNotSwapWithSortOnAs) {
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

TEST_F(PipelineOptimizationTest, ExclusionProjectShouldSwapWithIndependentMatch) {
    std::string inputPipe = "[{$project: {redacted: 0}}, {$match: {unrelated: 4}}]";
    std::string outputPipe =
        "[{$match: {unrelated: {$eq : 4}}}, {$project: {redacted: false, _id: true}}]";
    std::string serializedPipe =
        "[{$match : {unrelated : 4}}, {$project : {redacted : false, _id: true}}]";
    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe, serializedPipe);
}

TEST_F(PipelineOptimizationTest, ExclusionProjectShouldNotSwapWithMatchOnExcludedFields) {
    std::string pipeline =
        "[{$project: {subdoc: {redacted: false}, _id: true}}, {$match: {'subdoc.redacted': {$eq : "
        "4}}}]";
    assertPipelineOptimizesAndSerializesTo(pipeline, pipeline);
}

TEST_F(PipelineOptimizationTest, MatchShouldSplitIfPartIsIndependentOfExclusionProjection) {
    std::string inputPipe =
        "[{$project: {redacted: 0}},"
        " {$match: {redacted: 'x', unrelated: 4}}]";
    std::string outputPipe =
        "[{$match: {unrelated: {$eq: 4}}},"
        " {$project: {redacted: false, _id: true}},"
        " {$match: {redacted: {$eq: 'x'}}}]";
    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe);
}

TEST_F(PipelineOptimizationTest, MatchOnExprShouldSplitIfPartIsIndependentOfExclusionProjection) {
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

TEST_F(PipelineOptimizationTest, InclusionProjectShouldSwapWithIndependentMatch) {
    std::string inputPipe = "[{$project: {included: 1}}, {$match: {included: 4}}]";
    std::string outputPipe =
        "[{$match: {included: {$eq : 4}}}, {$project: {_id: true, included: true}}]";
    std::string serializedPipe =
        "[{$match : {included : 4}}, {$project : {_id: true, included : true}}]";
    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe, serializedPipe);
}

TEST_F(PipelineOptimizationTest, InclusionProjectShouldNotSwapWithMatchOnFieldsNotIncluded) {
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

TEST_F(PipelineOptimizationTest, MatchShouldSplitIfPartIsIndependentOfInclusionProjection) {
    std::string inputPipe =
        "[{$project: {_id: true, included: true}},"
        " {$match: {included: 'x', unrelated: 4}}]";
    std::string outputPipe =
        "[{$match: {included: {$eq: 'x'}}},"
        " {$project: {_id: true, included: true}},"
        " {$match: {unrelated: {$eq: 4}}}]";
    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe);
}

TEST_F(PipelineOptimizationTest, MatchOnExprShouldNotSplitIfDependentOnInclusionProjection) {
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

TEST_F(PipelineOptimizationTest, TwoMatchStagesShouldBothPushIndependentPartsBeforeProjection) {
    std::string inputPipe =
        "[{$project: {_id: true, included: true}},"
        " {$match: {included: 'x', unrelated: 4}},"
        " {$match: {included: 'y', unrelated: 5}}]";
    std::string outputPipe =
        "[{$match: {$and: [{included: {$eq: 'x'}}, {included: {$eq: 'y'}}]}},"
        " {$project: {_id: true, included: true}},"
        " {$match: {$and: [{unrelated: {$eq: 4}}, {unrelated: {$eq: 5}}]}}]";
    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe);
}

TEST_F(PipelineOptimizationTest, NeighboringMatchesShouldCoalesce) {
    std::string inputPipe =
        "[{$match: {x: 'x'}},"
        " {$match: {y: 'y'}}]";
    std::string outputPipe = "[{$match: {$and: [{x: {$eq: 'x'}}, {y: {$eq : 'y'}}]}}]";
    std::string serializedPipe = "[{$match: {$and: [{x: 'x'}, {y: 'y'}]}}]";
    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe, serializedPipe);
}

TEST_F(PipelineOptimizationTest, MatchShouldNotSwapBeforeLimit) {
    std::string inputPipe = "[{$limit: 3}, {$match: {y: 'y'}}]";
    std::string outputPipe = "[{$limit: 3}, {$match: {y: {$eq : 'y'}}}]";
    std::string serializedPipe = "[{$limit: 3}, {$match: {y: 'y'}}]";
    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe, serializedPipe);
}

TEST_F(PipelineOptimizationTest, MatchOnExprShouldNotSwapBeforeLimit) {
    std::string inputPipe = "[{$limit: 3}, {$match : {$expr: {$eq: ['$y', 'y']}}}]";
    std::string outputPipe =
        "[{$limit: 3}, {$match: {$and: [{$expr: {$eq: ['$y', {$const: 'y'}]}},"
        "                               {y: {$_internalExprEq: 'y'}}]}}]";
    std::string serializedPipe = "[{$limit: 3}, {$match : {$expr: {$eq: ['$y', 'y']}}}]";
    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe, serializedPipe);
}

TEST_F(PipelineOptimizationTest, MatchShouldNotSwapBeforeSkip) {
    std::string inputPipe = "[{$skip: 3}, {$match: {y: 'y'}}]";
    std::string outputPipe = "[{$skip: 3}, {$match: {y: {$eq : 'y'}}}]";
    std::string serializedPipe = "[{$skip: 3}, {$match: {y: 'y'}}]";
    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe, serializedPipe);
}

TEST_F(PipelineOptimizationTest, MatchOnExprShouldNotSwapBeforeSkip) {
    std::string inputPipe = "[{$skip: 3}, {$match : {$expr: {$eq: ['$y', 'y']}}}]";
    std::string outputPipe =
        "[{$skip: 3}, {$match: {$and: [{$expr: {$eq: ['$y', {$const: 'y'}]}},"
        "                              {y: {$_internalExprEq: 'y'}}]}}]";
    std::string serializedPipe = "[{$skip: 3}, {$match : {$expr: {$eq: ['$y', 'y']}}}]";
    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe, serializedPipe);
}

TEST_F(PipelineOptimizationTest, MatchShouldMoveAcrossProjectRename) {
    std::string inputPipe = "[{$project: {_id: true, a: '$b'}}, {$match: {a: {$eq: 1}}}]";
    std::string outputPipe = "[{$match: {b: {$eq: 1}}}, {$project: {_id: true, a: '$b'}}]";
    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe);
}

TEST_F(PipelineOptimizationTest, MatchShouldMoveAcrossAddFieldsRename) {
    std::string inputPipe = "[{$addFields: {a: '$b'}}, {$match: {a: {$eq: 1}}}]";
    std::string outputPipe = "[{$match: {b: {$eq: 1}}}, {$addFields: {a: '$b'}}]";
    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe);
}

TEST_F(PipelineOptimizationTest, MatchShouldMoveAcrossProjectRenameWithExplicitROOT) {
    std::string inputPipe = "[{$project: {_id: true, a: '$$ROOT.b'}}, {$match: {a: {$eq: 1}}}]";
    std::string outputPipe = "[{$match: {b: {$eq: 1}}}, {$project: {_id: true, a: '$$ROOT.b'}}]";
    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe);
}

TEST_F(PipelineOptimizationTest, MatchShouldMoveAcrossAddFieldsRenameWithExplicitCURRENT) {
    std::string inputPipe = "[{$addFields: {a: '$$CURRENT.b'}}, {$match: {a: {$eq: 1}}}]";
    std::string outputPipe = "[{$match: {b: {$eq: 1}}}, {$addFields: {a: '$b'}}]";
    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe);
}

TEST_F(PipelineOptimizationTest, PartiallyDependentMatchWithRenameShouldSplitAcrossAddFields) {
    // The $match predicate has two parts:
    //   1. A simple predicate on 'd'. This cannot be pushed down because 'd' is computed.
    //   2. An $or on two fields: 'a' and 'x'.
    // The $or can be pushed down, because:
    //   - 'x' is preserved by the $addFields.
    //   - 'a' is computed, but its value is available in field 'c' before the $addFields.
    std::string inputPipe =
        "[{$addFields: {'a': '$c', d: {$add: ['$e', '$f']}}},"
        "{$match: {$and: [{$or: [{'a': 1}, {x: 2}]}, {d: 3}]}}]";
    std::string outputPipe =
        "[{$match: {$or: [{c: {$eq: 1}}, {x: {$eq: 2}}]}},"
        "{$addFields: {a: '$c', d: {$add: ['$e', '$f']}}},"
        "{$match: {d: {$eq: 3}}}]";
    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe);
}

TEST_F(PipelineOptimizationTest, NorCanSplitAcrossProjectWithRename) {
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

TEST_F(PipelineOptimizationTest, MatchCanMoveAcrossDottedRenameOnGrouping) {
    std::string inputPipeline =
        "[{$group: { _id: { c: '$d' }, c: { $sum: {$const: 1} } } },"
        "{$project: { m: '$_id.c' } },"
        "{$match: { m: {$eq: 2} } }]";
    std::string outputPipeline =
        "[{$match: { d: {$eq: 2} } },"
        "{$group: { _id: { c: '$d' }, c: { $sum: {$const: 1} }, $willBeMerged: false } },"
        "{$project: { _id: true, m: '$_id.c' } }]";
    assertPipelineOptimizesAndSerializesTo(inputPipeline, outputPipeline);
}

TEST_F(PipelineOptimizationTest, MatchCanMoveAcrossDottedRenameOnGroupingMixedPredicates) {
    std::string inputPipeline =
        "[{$group: { _id: { c: '$d' }, c: { $sum: { $const: 1} } } },"
        "{$project: { m: '$_id.c' } },"
        "{$match: { $and: [ {m: {$eq: 2} }, {_id: {$eq: 3} } ] } }]";
    std::string outputPipeline =
        "[{$group: { _id: { c: '$d' }, c: { $sum: { $const: 1} }, $willBeMerged: false } },"
        "{$match: { $and: [{_id: {$eq: 3} }, {'_id.c': {$eq: 2} } ] } },"
        "{$project: { _id: true, m: '$_id.c' } } ]";
    std::string serializedPipe =
        "[{$group: { _id: { c: '$d' }, c: { $sum: { $const: 1} }, $willBeMerged: false } },"
        "{$match: { $and: [ {'_id.c': {$eq: 2} }, {_id: {$eq: 3} } ] } },"
        "{$project: { _id: true, m: '$_id.c' } } ]";

    assertPipelineOptimizesAndSerializesTo(inputPipeline, outputPipeline, serializedPipe);
}

TEST_F(PipelineOptimizationTest, AvoidPushingMatchOverGroupWithLongDottedRename) {
    std::string inputPipeline =
        "[{$group: {_id: {a: {b: '$a'}}}},"
        "{$project: {renamed: '$_id.a.b'}},"
        "{$match: {renamed: {$eq: 5}}}]";
    std::string outputPipeline =
        "[{$group: {_id: {a: {b: '$a'}}, $willBeMerged: false}},"
        "{$project: {_id: true, renamed: '$_id.a.b'}},"
        "{$match: {renamed: {$eq: 5 }}}]";
    assertPipelineOptimizesAndSerializesTo(inputPipeline, outputPipeline);
}

TEST_F(PipelineOptimizationTest, MatchCanMoveAcrossDottedRenameOnNestedGrouping) {
    std::string inputPipeline =
        "[{$group: { _id: { c: '$d', s: '$k' }, c: { $sum: {$const: 1} }} },"
        "{$project: { m: '$_id.c' } },"
        "{$match: { m: {$eq: 2} } }]";
    std::string outputPipeline =
        "[{$match: { d: {$eq: 2} } },"
        "{$group: { _id: { c: '$d', s: '$k' }, c: { $sum: {$const: 1} }, $willBeMerged: false } },"
        "{$project: { _id: true, m: '$_id.c' } }]";
    assertPipelineOptimizesAndSerializesTo(inputPipeline, outputPipeline);
}

TEST_F(PipelineOptimizationTest, MatchLeavingSecondAfterPushingOverProjection) {
    std::string inputPipeline =
        "[{$group: { _id: { c: '$d' }, c: { '$sum': {$const: 1} } } },"
        "{$project: { m1: '$_id.c' } },"
        "{$match: { m1: {$eq: 2}, k: {$eq: 5} } }]";

    std::string outputPipeline =
        "[{$match: { d: {$eq: 2} } },"
        "{$group: { _id: { c: '$d' }, c: { '$sum': {$const: 1} }, $willBeMerged: false } },"
        "{$project: { _id: true, m1: '$_id.c' } },"
        "{$match: { k: {$eq: 5} } }]";
    assertPipelineOptimizesAndSerializesTo(inputPipeline, outputPipeline);
}

TEST_F(PipelineOptimizationTest, PushingOverProjectionWithTail) {
    std::string inputPipeline =
        "[{$group: { _id: { c: '$d' }, c: { '$sum': {$const: 1} } } },"
        "{$project: { m1: '$_id.c' } },"
        "{$match: { m1: {$eq: 2}, k: {$eq: 5} } },"
        "{$project: { m2: '$_id' } } ]";

    std::string outputPipeline =
        "[{$match: { d: {$eq: 2} } },"
        "{$group: { _id: { c: '$d' }, c: { '$sum': {$const: 1} }, $willBeMerged: false } },"
        "{$project: { _id: true, m1: '$_id.c' } },"
        "{$match: { k: {$eq: 5} } },"
        "{$project: { _id: true, m2: '$_id' } }]";
    assertPipelineOptimizesAndSerializesTo(inputPipeline, outputPipeline);
}

TEST_F(PipelineOptimizationTest, PushingDottedMatchOverGrouping) {
    std::string inputPipeline =
        "[{$group: {_id: {a: '$l', b: '$b'}, $willBeMerged: false}},"
        "{$match: {'_id.a': 5}}]";

    std::string outputPipeline =
        "[{ $match: { l: { $eq: 5 } } },"
        "{ $group: { _id: { a: '$l', b: '$b' }, $willBeMerged: false } }]";
    assertPipelineOptimizesAndSerializesTo(inputPipeline, outputPipeline);
}

TEST_F(PipelineOptimizationTest, MatchCanMoveAcrossSeveralRenames) {
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

TEST_F(PipelineOptimizationTest, RenameShouldNotBeAppliedToDependentMatch) {
    std::string pipeline =
        "[{$project: {x: {$add: ['$foo', '$bar']}, y: '$z', _id: false}},"
        "{$match: {$or: [{x: {$eq: 1}}, {y: {$eq: 1}}]}}]";
    assertPipelineOptimizesAndSerializesTo(pipeline, pipeline);
}

TEST_F(PipelineOptimizationTest, MatchCannotMoveAcrossAddFieldsRenameOfDottedPathOnRight) {
    // This one is illegal because the $b.c expression can reshape arrays.
    std::string pipeline = "[{$addFields: {a: '$b.c'}}, {$match: {a: {$eq: 1}}}]";
    assertPipelineOptimizesAndSerializesTo(pipeline, pipeline);
}

TEST_F(PipelineOptimizationTest, MatchCannotMoveAcrossAddFieldsRenameOfDottedPathOnLeft) {
    // This one is illegal because the 'a.b' path can write through doubly-nested arrays,
    // and it can be a noop when 'a' is empty-array.
    std::string inputPipe = "[{$addFields: {'a.b': '$x'}}, {$match: {'a.b': {$eq: 1}}}]";
    // It serializes using this equivalent nested syntax.
    std::string outputPipe = "[{$addFields: {a: {b: '$x'}}}, {$match: {'a.b': {$eq: 1}}}]";
    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe);
}

TEST_F(PipelineOptimizationTest, MatchCannotMoveAcrossProjectRenameOfDottedPath) {
    std::string inputPipe =
        "[{$project: {a: '$$CURRENT.b.c', _id: false}}, {$match: {a: {$eq: 1}}}]";
    std::string outputPipe = "[{$project: {a: '$b.c', _id: false}}, {$match: {a: {$eq: 1}}}]";
    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe);
}

TEST_F(PipelineOptimizationTest, MatchWithTypeShouldMoveAcrossRename) {
    std::string inputPipe = "[{$addFields: {a: '$b'}}, {$match: {a: {$type: 4}}}]";
    std::string outputPipe = "[{$match: {b: {$type: [4]}}}, {$addFields: {a: '$b'}}]";
    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe);
}

TEST_F(PipelineOptimizationTest,
       MatchElemMatchValueOnArrayFieldCanSplitAcrossRenameWithSimpleProject) {
    // The $project simply renames 'a' to 'b', and the $match with $elemMatch on
    // the value can be swapped with $project.
    std::string inputPipe =
        "[{$project: {b: '$a', _id: false}},{$match: {b: {$elemMatch: {$eq: 1}}}}]";
    std::string outputPipe =
        "[{$match: {a: {$elemMatch: {$eq: 1}}}},{$project: {b: '$a', _id: false}}]";
    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe);
}

TEST_F(PipelineOptimizationTest,
       MatchElemMatchValueOnArrayFieldCanNotSplitAcrossRenameWithMapAndAddFields) {
    // The $addFields simply maps an array of objects to one containing their inner 'elementField'
    // scalar values . The $match stage on the reshaped array should not be swapped with $project to
    // preserve the original $elemMatch semantics.
    std::string pipeline = R"(
[
    {
        $addFields: {
            "reshapedArray": {
                $map: {input: '$arrayField', as: 'iter', in : "$$iter.elementField"}
            },
            _id: { "$const": false }
        }
    },
    {$match: {"reshapedArray": {$elemMatch: {$eq: 1}}}}
]
        )";
    assertPipelineOptimizesAndSerializesTo(pipeline, pipeline);
}

TEST_F(PipelineOptimizationTest,
       MatchElemMatchValueOnArrayFieldCanNotSplitAcrossRenameWithDottedProject) {
    // The $project stage maps a dotted field path to a simple non-dotted one which is then matched
    // upon. Expect no swap to be happen as it might affect the result of the query due to
    // $elemMatch.
    std::string pipeline =
        "[{$project: {reshaped: '$document.array.element.deeply.nested.field', _id: false}},"
        "{$match: {reshaped: {$elemMatch: {$eq: 1}}}}]";
    assertPipelineOptimizesAndSerializesTo(pipeline, pipeline);
}

// TODO SERVER-74298 The $match can be swapped with $project after renaming.
TEST_F(PipelineOptimizationTest,
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
TEST_F(PipelineOptimizationTest, MatchEqObjectCanNotSplitAcrossRenameWithMapAndProject) {
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

TEST_F(PipelineOptimizationTest, MatchCannotSwapWithLimit) {
    std::string pipeline = "[{$limit: 3}, {$match: {x: {$gt: 0}}}]";
    assertPipelineOptimizesAndSerializesTo(pipeline, pipeline);
}

TEST_F(PipelineOptimizationTest, MatchCannotSwapWithSortLimit) {
    std::string inputPipe = "[{$sort: {x: -1}}, {$limit: 3}, {$match: {x: {$gt: 0}}}]";
    std::string outputPipe = "[{$sort: {sortKey: {x: -1}, limit: 3}}, {$match: {x: {$gt: 0}}}]";
    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe, inputPipe);
}

TEST_F(PipelineOptimizationTest, MatchOnMinItemsShouldSwapSinceCategoryIsArrayMatching) {
    std::string inputPipe =
        "[{$project: {_id: true, a: '$b'}}, "
        "{$match: {a: {$_internalSchemaMinItems: 1}}}]";
    std::string outputPipe =
        "[{$match: {b: {$_internalSchemaMinItems: 1}}}, "
        "{$project: {_id: true, a: '$b'}}]";
    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe);

    inputPipe =
        "[{$project: {redacted: false, _id: true}}, "
        "{$match: {a: {$_internalSchemaMinItems: 1}}}]";
    outputPipe =
        "[{$match: {a: {$_internalSchemaMinItems: 1}}}, "
        "{$project: {redacted: false, _id: true}}]";
    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe);

    inputPipe =
        "[{$addFields : {a : {$const: 1}}}, "
        "{$match: {b: {$_internalSchemaMinItems: 1}}}]";
    outputPipe =
        "[{$match: {b: {$_internalSchemaMinItems: 1}}}, "
        "{$addFields : {a : {$const: 1}}}]";
    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe);
}

TEST_F(PipelineOptimizationTest, MatchOnMaxItemsShouldSwapSinceCategoryIsArrayMatching) {
    std::string inputPipe =
        "[{$project: {_id: true, a: '$b'}}, "
        "{$match: {a: {$_internalSchemaMaxItems: 1}}}]";
    std::string outputPipe =
        "[{$match: {b: {$_internalSchemaMaxItems: 1}}}, "
        "{$project: {_id: true, a: '$b'}}]";
    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe);

    inputPipe =
        "[{$project: {redacted: false, _id: true}}, "
        "{$match: {a: {$_internalSchemaMaxItems: 1}}}]";
    outputPipe =
        "[{$match: {a: {$_internalSchemaMaxItems: 1}}}, "
        "{$project: {redacted: false, _id: true}}]";
    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe);

    inputPipe =
        "[{$addFields : {a : {$const: 1}}}, "
        "{$match: {b: {$_internalSchemaMaxItems: 1}}}]";
    outputPipe =
        "[{$match: {b: {$_internalSchemaMaxItems: 1}}}, "
        "{$addFields : {a : {$const: 1}}}]";
    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe);
}

TEST_F(PipelineOptimizationTest,
       MatchOnAllElemMatchFromIndexShouldNotSwapBecauseOfNamePlaceHolder) {
    std::string inputPipe =
        "[{$project: {_id: true, a: '$b'}}, "
        "{$match: {a: {$_internalSchemaAllElemMatchFromIndex: [1, {b: {$gt: 0}}]}}}]";
    assertPipelineOptimizesAndSerializesTo(inputPipe, inputPipe);

    inputPipe =
        "[{$project: {redacted: false, _id: true}}, "
        "{$match: {a: {$_internalSchemaAllElemMatchFromIndex: [1, {b: {$gt: 0}}]}}}]";
    std::string outputPipe =
        "[{$match: {a: {$_internalSchemaAllElemMatchFromIndex: [1, {b: {$gt: 0}}]}}}, "
        "{$project: {redacted: false, _id: true}}]";
    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe);

    inputPipe =
        "[{$addFields : {a : {$const: 1}}}, "
        "{$match: {b: {$_internalSchemaAllElemMatchFromIndex: [1, {b: {$gt: 0}}]}}}]";
    outputPipe =
        "[{$match: {b: {$_internalSchemaAllElemMatchFromIndex: [1, {b: {$gt: 0}}]}}}, "
        "{$addFields : {a : {$const: 1}}}]";
    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe);
}

TEST_F(PipelineOptimizationTest, MatchOnArrayIndexShouldNotSwapBecauseOfNamePlaceHolder) {
    std::string inputPipe = R"(
        [{$project: {_id: true, a: '$b'}},
        {$match: {a: {$_internalSchemaMatchArrayIndex:
           {index: 0, namePlaceholder: 'i', expression: {i: {$lt: 0}}}}}}])";
    assertPipelineOptimizesAndSerializesTo(inputPipe, inputPipe);

    inputPipe = R"(
        [{$project: {redacted: false, _id: true}},
        {$match: {a: {$_internalSchemaMatchArrayIndex:
           {index: 0, namePlaceholder: 'i', expression: {i: {$lt: 0}}}}}}])";
    std::string outputPipe = R"(
        [{$match: {a: {$_internalSchemaMatchArrayIndex:
           {index: 0, namePlaceholder: 'i', expression: {i: {$lt: 0}}}}}},
        {$project: {redacted: false, _id: true}}])";
    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe);

    inputPipe = R"(
        [{$addFields : {a : {$const: 1}}},
        {$match: {b: {$_internalSchemaMatchArrayIndex:
           {index: 0, namePlaceholder: 'i', expression: {i: {$lt: 0}}}}}}])";
    outputPipe = R"(
        [{$match: {b: {$_internalSchemaMatchArrayIndex:
           {index: 0, namePlaceholder: 'i', expression: {i: {$lt: 0}}}}}},
        {$addFields : {a : {$const: 1}}}])";
    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe);
}

TEST_F(PipelineOptimizationTest, MatchOnUniqueItemsShouldSwapSinceCategoryIsArrayMatching) {
    std::string inputPipe =
        "[{$project: {_id: true, a: '$b'}}, "
        "{$match: {a: {$_internalSchemaUniqueItems: true}}}]";
    std::string outputPipe =
        "[{$match: {b: {$_internalSchemaUniqueItems: true}}}, "
        "{$project: {_id: true, a: '$b'}}]";
    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe);

    inputPipe =
        "[{$project: {redacted: false, _id: true}}, "
        "{$match: {a: {$_internalSchemaUniqueItems: true}}}]";
    outputPipe =
        "[{$match: {a: {$_internalSchemaUniqueItems: true}}}, "
        "{$project: {redacted: false, _id: true}}]";
    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe);

    inputPipe =
        "[{$addFields : {a : {$const: 1}}}, "
        "{$match: {b: {$_internalSchemaUniqueItems: true}}}]";
    outputPipe =
        "[{$match: {b: {$_internalSchemaUniqueItems: true}}}, "
        "{$addFields : {a : {$const: 1}}}]";
    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe);
}

// Descriptive test. The following internal match expression *could* participate in pipeline
// optimizations, but it currently does not.
TEST_F(PipelineOptimizationTest, MatchOnObjectMatchShouldNotSwapSinceCategoryIsOther) {
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
TEST_F(PipelineOptimizationTest, MatchOnMinPropertiesShouldNotSwapSinceCategoryIsOther) {
    std::string inputPipe =
        "[{$project: {_id: true, a: '$b'}}, "
        "{$match: {$_internalSchemaMinProperties: 2}}]";
    assertPipelineOptimizesAndSerializesTo(inputPipe, inputPipe);

    inputPipe =
        "[{$project: {redacted: false, _id: true}}, "
        "{$match: {$_internalSchemaMinProperties: 2}}]";
    assertPipelineOptimizesAndSerializesTo(inputPipe, inputPipe);

    inputPipe =
        "[{$addFields : {a : {$const: 1}}}, "
        "{$match: {$_internalSchemaMinProperties: 2}}]";
    assertPipelineOptimizesAndSerializesTo(inputPipe, inputPipe);
}

// Descriptive test. The following internal match expression *could* participate in pipeline
// optimizations, but it currently does not.
TEST_F(PipelineOptimizationTest, MatchOnMaxPropertiesShouldNotSwapSinceCategoryIsOther) {
    std::string inputPipe =
        "[{$project: {_id: true, a: '$b'}}, "
        "{$match: {$_internalSchemaMaxProperties: 2}}]";
    assertPipelineOptimizesAndSerializesTo(inputPipe, inputPipe);

    inputPipe =
        "[{$project: {redacted: false, _id: true}}, "
        "{$match: {$_internalSchemaMaxProperties: 2}}]";
    assertPipelineOptimizesAndSerializesTo(inputPipe, inputPipe);

    inputPipe =
        "[{$addFields : {a : {$const: 1}}}, "
        "{$match: {$_internalSchemaMaxProperties: 2}}]";
    assertPipelineOptimizesAndSerializesTo(inputPipe, inputPipe);
}

// Descriptive test. The following internal match expression *could* participate in pipeline
// optimizations, but it currently does not.
TEST_F(PipelineOptimizationTest, MatchOnAllowedPropertiesShouldNotSwapSinceCategoryIsOther) {
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
TEST_F(PipelineOptimizationTest, MatchOnCondShouldNotSwapSinceCategoryIsOther) {
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
TEST_F(PipelineOptimizationTest, MatchOnRootDocEqShouldNotSwapSinceCategoryIsOther) {
    std::string inputPipe =
        "[{$project: {_id: true, a: '$b'}}, "
        "{$match: {$_internalSchemaRootDocEq: {a: 1}}}]";
    assertPipelineOptimizesAndSerializesTo(inputPipe, inputPipe);

    inputPipe =
        "[{$project: {redacted: false, _id: true}}, "
        "{$match: {$_internalSchemaRootDocEq: {a: 1}}}]";
    assertPipelineOptimizesAndSerializesTo(inputPipe, inputPipe);

    inputPipe =
        "[{$addFields : {a : {$const: 1}}}, "
        "{$match: {$_internalSchemaRootDocEq: {a: 1}}}]";
    assertPipelineOptimizesAndSerializesTo(inputPipe, inputPipe);
}

// Descriptive test. The following internal match expression can participate in pipeline
// optimizations.
TEST_F(PipelineOptimizationTest, MatchOnInternalSchemaTypeShouldSwap) {
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

TEST_F(PipelineOptimizationTest, MatchOnMinLengthShouldSwapWithAdjacentStage) {
    std::string inputPipe =
        "[{$project: {_id: true, a: '$b'}}, "
        "{$match: {a: {$_internalSchemaMinLength: 1}}}]";
    std::string outputPipe =
        "[{$match: {b: {$_internalSchemaMinLength: 1}}},"
        "{$project: {_id: true, a: '$b'}}]";
    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe);

    inputPipe =
        "[{$project: {redacted: false}}, "
        "{$match: {a: {$_internalSchemaMinLength: 1}}}]";
    outputPipe =
        "[{$match: {a: {$_internalSchemaMinLength: 1}}},"
        "{$project: {redacted: false, _id: true}}]";
    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe);

    inputPipe =
        "[{$addFields : {a : {$const: 1}}}, "
        "{$match: {b: {$_internalSchemaMinLength: 1}}}]";
    outputPipe =
        "[{$match: {b: {$_internalSchemaMinLength: 1}}},"
        "{$addFields: {a: {$const: 1}}}]";
    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe);
}

TEST_F(PipelineOptimizationTest, MatchOnMaxLengthShouldSwapWithAdjacentStage) {
    std::string inputPipe =
        "[{$project: {_id: true, a: '$b'}}, "
        "{$match: {a: {$_internalSchemaMaxLength: 1}}}]";
    std::string outputPipe =
        "[{$match: {b: {$_internalSchemaMaxLength: 1}}},"
        "{$project: {_id: true, a: '$b'}}]";
    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe);

    inputPipe =
        "[{$project: {redacted: false}}, "
        "{$match: {a: {$_internalSchemaMaxLength: 1}}}]";
    outputPipe =
        "[{$match: {a: {$_internalSchemaMaxLength: 1}}}, "
        "{$project: {redacted: false, _id: true}}]";
    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe);

    inputPipe =
        "[{$addFields : {a : {$const: 1}}}, "
        "{$match: {b: {$_internalSchemaMaxLength: 1}}}]";
    outputPipe =
        "[{$match: {b: {$_internalSchemaMaxLength: 1}}}, "
        "{$addFields: {a: {$const: 1}}}]";
    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe);
}

TEST_F(PipelineOptimizationTest, MatchOnInternalEqShouldSwapWithAdjacentStage) {
    std::string inputPipe =
        "[{$project: {_id: true, a: '$b'}}, "
        "{$match: {a: {$_internalSchemaEq: {c: 1}}}}]";
    std::string outputPipe =
        "[{$match: {b: {$_internalSchemaEq: {c: 1}}}}, "
        "{$project: {_id: true, a: '$b'}}]";
    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe);

    inputPipe =
        "[{$project: {redacted: false, _id: true}}, "
        "{$match: {a: {$_internalSchemaEq: {c: 1}}}}]";
    outputPipe =
        "[{$match: {a: {$_internalSchemaEq: {c: 1}}}}, "
        "{$project: {redacted: false, _id: true}}]";
    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe);

    inputPipe =
        "[{$addFields : {a : {$const: 1}}}, "
        "{$match: {b: {$_internalSchemaEq: {c: 1}}}}]";
    outputPipe =
        "[{$match: {b: {$_internalSchemaEq: {c: 1}}}}, "
        "{$addFields: {a: {$const: 1}}}]";
    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe);
}

TEST_F(PipelineOptimizationTest, MatchOnXorShouldSwapIfEverySubExpressionIsEligible) {
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

TEST_F(PipelineOptimizationTest, MatchOnFmodShouldSwapWithAdjacentStage) {
    std::string inputPipe =
        "[{$project: {_id: true, a: '$b'}}, "
        "{$match: {a: {$_internalSchemaFmod: [5, 0]}}}]";
    std::string outputPipe =
        "[{$match: {b: {$_internalSchemaFmod: [5, 0]}}}, "
        "{$project: {_id: true, a: '$b'}}]";
    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe);

    inputPipe =
        "[{$project: {redacted: false, _id: true}}, "
        "{$match: {a: {$_internalSchemaFmod: [5, 0]}}}]";
    outputPipe =
        "[{$match: {a: {$_internalSchemaFmod: [5, 0]}}}, "
        "{$project: {redacted: false, _id: true}}]";
    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe);

    inputPipe =
        "[{$addFields : {a : {$const: 1}}}, "
        "{$match: {b: {$_internalSchemaFmod: [5, 0]}}}]";
    outputPipe =
        "[{$match: {b: {$_internalSchemaFmod: [5, 0]}}}, "
        "{$addFields: {a: {$const: 1}}}]";
    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe);
}

class ChangeStreamPipelineOptimizationTest : public ServiceContextTest {
public:
    struct ExpressionContextOptionsStruct {
        bool inRouter;
    };

    ChangeStreamPipelineOptimizationTest()
        : ChangeStreamPipelineOptimizationTest({.inRouter = false}) {}

    ChangeStreamPipelineOptimizationTest(ExpressionContextOptionsStruct options) {
        _opCtx = _testServiceContext.makeOperationContext();
        _expCtx = make_intrusive<ExpressionContextForTest>(
            _opCtx.get(),
            NamespaceString::createNamespaceString_forTest(
                boost::none, "unittests", "pipeline_test"));
        _expCtx->setOperationContext(_opCtx.get());
        _expCtx->setUUID(UUID::gen());
        _expCtx->setInRouter(options.inRouter);
        setMockReplicationCoordinatorOnOpCtx(_expCtx->getOperationContext());
    }

    BSONObj changestreamStage(const std::string& stageStr) {
        return fromjson("{$changeStream: " + stageStr + "}");
    }

    BSONObj matchStage(const std::string& stageStr) {
        return fromjson("{$match: " + stageStr + "}");
    }

    BSONObj redactStage(const std::string& stageStr) {
        return fromjson("{$redact: " + stageStr + "}");
    }

    std::unique_ptr<Pipeline> makePipeline(const std::vector<BSONObj>& rawPipeline) {
        auto pipeline = Pipeline::parse(rawPipeline, _expCtx);
        return pipeline;
    }

    static std::string generateEventResumeToken() {
        ResumeTokenData resumeTokenDataIn{Timestamp{1001, 3},
                                          ResumeTokenData::kDefaultTokenVersion,
                                          0,
                                          UUID::gen(),
                                          Value(Document{{"operationType", "drop"_sd}})};
        return ResumeToken(resumeTokenDataIn).toBSON().toString();
    }

private:
    QueryTestServiceContext _testServiceContext;
    ServiceContext::UniqueOperationContext _opCtx;
    boost::intrusive_ptr<ExpressionContextForTest> _expCtx;
};

TEST_F(ChangeStreamPipelineOptimizationTest, ChangeStreamLookUpSize) {
    auto pipeline = makePipeline(
        {changestreamStage("{fullDocument: 'updateLookup', showExpandedEvents: true}")});
    ASSERT_EQ(pipeline->size(), getChangeStreamStageSize());

    // Make sure the change lookup is at the end.
    assertStageAtPos<DocumentSourceChangeStreamAddPostImage>(pipeline->getSources(), -1 /* pos */);
}

TEST_F(ChangeStreamPipelineOptimizationTest, ChangeStreamLookupSwapsWithIndependentMatch) {
    // We enable the 'showExpandedEvents' flag to avoid injecting an additional $match stage which
    // filters out newly added events.
    auto pipeline =
        makePipeline({changestreamStage("{fullDocument: 'updateLookup', showExpandedEvents: true}"),
                      matchStage("{extra: 'predicate'}")});
    pipeline->optimizePipeline();

    // Make sure the $match stage has swapped before the change look up.
    assertStageAtPos<DocumentSourceChangeStreamAddPostImage>(pipeline->getSources(), -1 /* pos */);
}

TEST_F(ChangeStreamPipelineOptimizationTest, ChangeStreamLookupDoesNotSwapWithMatchOnPostImage) {
    // We enable the 'showExpandedEvents' flag to avoid injecting an additional $match stage which
    // filters out newly added eve
    auto pipeline =
        makePipeline({changestreamStage("{fullDocument: 'updateLookup', showExpandedEvents: true}"),
                      matchStage("{fullDocument: null}")});
    pipeline->optimizePipeline();

    // Make sure the $match stage stays at the end.
    assertStageAtPos<DocumentSourceMatch>(pipeline->getSources(), -1 /* pos */);
}

TEST_F(ChangeStreamPipelineOptimizationTest, FullDocumentBeforeChangeLookupSize) {
    // We enable the 'showExpandedEvents' flag to avoid injecting an additional $match stage which
    // filters out newly added events.
    auto pipeline = makePipeline(
        {changestreamStage("{fullDocumentBeforeChange: 'required', showExpandedEvents: true}")});
    ASSERT_EQ(pipeline->size(), getChangeStreamStageSize());

    // Make sure the pre-image lookup is at the end.
    assertStageAtPos<DocumentSourceChangeStreamAddPreImage>(pipeline->getSources(), -1 /* pos */);
}

TEST_F(ChangeStreamPipelineOptimizationTest,
       FullDocumentBeforeChangeLookupSwapsWithIndependentMatch) {
    // We enable the 'showExpandedEvents' flag to avoid injecting an additional $match stage which
    // filters out newly added events.
    auto pipeline = makePipeline(
        {changestreamStage("{fullDocumentBeforeChange: 'required', showExpandedEvents: true}"),
         matchStage("{extra: 'predicate'}")});
    pipeline->optimizePipeline();

    // Make sure the $match stage has swapped before the change look up.
    assertStageAtPos<DocumentSourceChangeStreamAddPreImage>(pipeline->getSources(), -1 /* pos */);
}

TEST_F(ChangeStreamPipelineOptimizationTest,
       FullDocumentBeforeChangeDoesNotSwapWithMatchOnPreImage) {
    // We enable the 'showExpandedEvents' flag to avoid injecting an additional $match stage which
    // filters out newly added events.
    auto pipeline = makePipeline(
        {changestreamStage("{fullDocumentBeforeChange: 'required', showExpandedEvents: true}"),
         matchStage("{fullDocumentBeforeChange: null}")});
    pipeline->optimizePipeline();

    // Make sure the $match stage stays at the end.
    assertStageAtPos<DocumentSourceMatch>(pipeline->getSources(), -1 /* pos */);
}

TEST_F(ChangeStreamPipelineOptimizationTest,
       ChangeStreamEnsureResumeTokenSwapsWithJsonSchemaMatch) {
    auto pipeline = makePipeline(
        {changestreamStage("{resumeAfter: " + generateEventResumeToken() + "}"),
         matchStage(
             "{$jsonSchema: {properties: {documentKey: {properties: {_id: {enum: [1, 2]}}}}}}")});

    // Assert $match is the last stage before optimization.
    assertStageAtPos<DocumentSourceMatch>(pipeline->getSources(), -1);

    pipeline->optimizePipeline();

    // Assert that $match swaps with $_internalChangeStreamHandleTopologyChange after optimization.
    assertStageAtPos<DocumentSourceMatch>(pipeline->getSources(), -2);
    assertStageAtPos<DocumentSourceChangeStreamEnsureResumeTokenPresent>(pipeline->getSources(),
                                                                         -1);
}

// To enforce the $_internalChangeStreamHandleTopologyChange stage.
class ChangeStreamPipelineOptimizationTestWithMongoS : public ChangeStreamPipelineOptimizationTest {
public:
    ChangeStreamPipelineOptimizationTestWithMongoS()
        : ChangeStreamPipelineOptimizationTest({.inRouter = true}) {}
};

TEST_F(ChangeStreamPipelineOptimizationTestWithMongoS,
       ChangeStreamHandleTopologyChangeSwapsWithRedact) {
    auto pipeline =
        makePipeline({changestreamStage("{showExpandedEvents: true}"), redactStage("'$$PRUNE'")});
    pipeline->optimizePipeline();

    // Assert that $redact swaps with $_internalChangeStreamHandleTopologyChange after optimization.
    assertStageAtPos<DocumentSourceRedact>(pipeline->getSources(), -2 /* pos */);
    assertStageAtPos<DocumentSourceChangeStreamHandleTopologyChange>(pipeline->getSources(),
                                                                     -1 /* pos */);
}

TEST_F(ChangeStreamPipelineOptimizationTestWithMongoS,
       ChangeStreamHandleTopologyChangeSwapsWithJsonSchemaMatch) {
    auto pipeline = makePipeline(
        {changestreamStage("{}"),
         matchStage(
             "{$jsonSchema: {properties: {documentKey: {properties: {_id: {enum: [1, 2]}}}}}}")});

    // Assert $match is the last stage before optimization.
    assertStageAtPos<DocumentSourceMatch>(pipeline->getSources(), -1);

    pipeline->optimizePipeline();

    // Assert that $match swaps with $_internalChangeStreamHandleTopologyChange after optimization.
    assertStageAtPos<DocumentSourceMatch>(pipeline->getSources(), -2);
    assertStageAtPos<DocumentSourceChangeStreamHandleTopologyChange>(pipeline->getSources(), -1);
}

TEST_F(PipelineOptimizationTest, SortLimProjLimBecomesTopKSortProj) {
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

    auto pipeline = assertPipelineOptimizesTo(inputPipe, outputPipe);
    assertPipelineSerializesTo(*pipeline, boost::none, serializedPipe);

    SerializationOptions options{.serializeForCloning = true};
    serializedPipe =
        "[{$sort: {a: 1, $_internalLimit: 5}}"
        ",{$project : {_id: true, a: true}}"
        "]";
    assertPipelineSerializesTo(*pipeline, options, serializedPipe);
}

TEST_F(PipelineOptimizationTest, SortProjUnwindLimLimBecomesSortProjUnwindLim) {
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

    auto pipeline = assertPipelineOptimizesTo(inputPipe, outputPipe);
    assertPipelineSerializesTo(*pipeline, boost::none, serializedPipe);

    SerializationOptions options{.serializeForCloning = true};
    assertPipelineSerializesTo(*pipeline, options, serializedPipe);
}

TEST_F(PipelineOptimizationTest, SortSkipLimBecomesTopKSortSkip) {
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

    auto pipeline = assertPipelineOptimizesTo(inputPipe, outputPipe);
    assertPipelineSerializesTo(*pipeline, boost::none, serializedPipe);

    SerializationOptions options{.serializeForCloning = true};
    serializedPipe =
        "[{$sort: {a: 1, $_internalLimit: 7}}"
        ",{$skip: 2}"
        "]";
    assertPipelineSerializesTo(*pipeline, options, serializedPipe);
}

TEST_F(PipelineOptimizationTest, LimDoesNotCoalesceWithSortInSortProjGroupLim) {
    std::string inputPipe =
        "[{$sort: {a: 1}}"
        ",{$project : {a: 1}}"
        ",{$group: {_id: '$a'}}"
        ",{$limit: 5}"
        "]";

    std::string outputPipe =
        "[{$sort: {sortKey: {a: 1}}}"
        ",{$project: {_id: true, a: true}}"
        ",{$group: {_id: '$a', $willBeMerged: false}}"
        ",{$limit: 5}"
        "]";

    std::string serializedPipe =
        "[{$sort: {a: 1}}"
        ",{$project : {_id: true, a: true}}"
        ",{$group: {_id: '$a', $willBeMerged: false}}"
        ",{$limit: 5}"
        "]";

    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe, serializedPipe);
}

TEST_F(PipelineOptimizationTest, SortProjSkipLimBecomesTopKSortSkipProj) {
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

    auto pipeline = assertPipelineOptimizesTo(inputPipe, outputPipe);
    assertPipelineSerializesTo(*pipeline, boost::none, serializedPipe);

    SerializationOptions options{.serializeForCloning = true};
    serializedPipe =
        "[{$sort: {a: 1, $_internalLimit: 8}}"
        ",{$skip: 3}"
        ",{$project : {_id: true, a: true}}"
        "]";
    assertPipelineSerializesTo(*pipeline, options, serializedPipe);
}

TEST_F(PipelineOptimizationTest, SortSkipProjSkipLimSkipLimBecomesTopKSortSkipProj) {
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

    auto pipeline = assertPipelineOptimizesTo(inputPipe, outputPipe);
    assertPipelineSerializesTo(*pipeline, boost::none, serializedPipe);

    SerializationOptions options{.serializeForCloning = true};
    serializedPipe =
        "[{$sort: {a: 1, $_internalLimit: 15}}"
        ",{$skip: 12}"
        ",{$project : {_id: true, a: true}}"
        "]";
    assertPipelineSerializesTo(*pipeline, options, serializedPipe);
}

TEST_F(PipelineOptimizationTest, MatchGetsPushedIntoBothChildrenOfUnion) {
    assertPipelineOptimizesAndSerializesTo(
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

TEST_F(PipelineOptimizationTest, MatchPushedBeforeReplaceRoot) {
    std::string inputPipe =
        "[{$replaceRoot: { newRoot: '$subDocument' }}, "
        "{$match: { x: 2 }}]";
    std::string outputPipe =
        "["
        " {$match: {$or: [{'subDocument.x': {$eq: 2}},"
        " {'subDocument': {$not: {$type: [3]}}}, {'subDocument': {$type: [4]}}]}},"
        " {$replaceRoot: {newRoot: '$subDocument'}}"
        "]";
    std::string serializedPipe =
        "["
        " {$match: {$or: [{'subDocument.x': {$eq: 2}},"
        " {'subDocument': {$type: [4]}}, {'subDocument': {$not: {$type: [3]}}}]}},"
        " {$replaceRoot: {newRoot: '$subDocument'}}"
        "]";
    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe, serializedPipe);
}

TEST_F(PipelineOptimizationTest, MatchPushedBeforeReplaceWith) {
    std::string inputPipe =
        "["
        " {$replaceWith: '$subDocument'},"
        " {$match: {x: 6.98}}"
        "]";
    std::string outputPipe =
        "["
        " {$match: {$or: [{'subDocument.x': {$eq: 6.98}},"
        " {'subDocument': {$not: {$type: [3]}}}, {'subDocument': {$type: [4]}}]}},"
        " {$replaceRoot: {newRoot: '$subDocument'}}"
        "]";
    std::string serializedPipe =
        "["
        " {$match: {$or: [{'subDocument.x': {$eq: 6.98}},"
        " {'subDocument': {$type: [4]}}, {'subDocument': {$not: {$type: [3]}}}]}},"
        " {$replaceRoot: {newRoot: '$subDocument'}}"
        "]";
    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe, serializedPipe);
}

TEST_F(PipelineOptimizationTest, MatchPushedBeforeReplaceWithComplex) {
    std::string inputPipe =
        "["
        " {$replaceWith: '$subDocument'},"
        " {$match: {$or: [{x: 'big'}, {y: 'small'}]}}"
        "]";
    std::string outputPipe =
        "["
        " {$match: {$or: [{'subDocument.x': {$eq: 'big'}},"
        " {'subDocument.y': {$eq: 'small'}},"
        " {'subDocument': {$not: {$type: [3]}}}, {'subDocument': {$type: [4]}}]}},"
        " {$replaceRoot: {newRoot: '$subDocument'}}"
        "]";
    std::string serializedPipe =
        "["
        " {$match: {$or: [{'subDocument.x': {$eq: 'big'}},"
        " {'subDocument.y': {$eq: 'small'}},"
        " {'subDocument': {$type: [4]}}, {'subDocument': {$not: {$type: [3]}}}]}},"
        " {$replaceRoot: {newRoot: '$subDocument'}}"
        "]";
    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe, serializedPipe);
}

TEST_F(PipelineOptimizationTest, MatchPushedBeforeReplaceWithNestedAnd) {
    std::string inputPipe =
        "["
        " {$replaceWith: '$subDocument'},"
        " {$match: {$and: [{x: 'big', y: 'small'}, {$and: [{a: 'big', b: 'small'}]}]}}"
        "]";
    std::string outputPipe =
        "["
        " {$match: {$or: [{$and: [{'subDocument.a': {$eq: 'big'}},"
        " {'subDocument.b': {$eq: 'small'}},"
        " {'subDocument.x': {$eq: 'big'}},"
        " {'subDocument.y': {$eq: 'small'}}]},"
        " {'subDocument': {$not: {$type: [3]}}}, {'subDocument': {$type: [4]}}]}},"
        " {$replaceRoot: {newRoot: '$subDocument'}}"
        "]";
    std::string serializedPipe =
        "["
        " {$match: {$or: [{$and: [{$and: [{'subDocument.x': {$eq: 'big'}},"
        " {'subDocument.y': {$eq: 'small'}}]},"
        " {$and: [{$and: [{'subDocument.a': {$eq: 'big'}},"
        " {'subDocument.b': {$eq: 'small'}}]}]}]},"
        " {'subDocument': {$type: [4]}}, {'subDocument': {$not: {$type: [3]}}}]}},"
        " {$replaceRoot: {newRoot: '$subDocument'}}"
        "]";
    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe, serializedPipe);
}

TEST_F(PipelineOptimizationTest, MatchPushedBeforeReplaceWithAndOr) {
    std::string inputPipe =
        "["
        " {$replaceWith: '$subDocument'},"
        " {$match: {$and: [{a: 'big', b: 'small'}, {$or: [{'lord': 'cat'}, {'friend': 'dog'}]}]}}"
        "]";
    std::string outputPipe =
        "["
        " {$match: {$or: [{$and: [{$or: [{'subDocument.friend': {$eq: 'dog'}},"
        " {'subDocument.lord': {$eq: 'cat'}}]},"
        " {'subDocument.a': {$eq: 'big'}},"
        " {'subDocument.b': {$eq: 'small'}}]},"
        " {'subDocument': {$not: {$type: [3]}}}, {'subDocument': {$type: [4]}}]}},"
        " {$replaceRoot: {newRoot: '$subDocument'}}"
        "]";
    std::string serializedPipe =
        "["
        " {$match: {$or: [{$and: [{$and: [{'subDocument.a': {$eq: 'big'}},"
        " {'subDocument.b': {$eq: 'small'}}]},"
        " {$or: [{'subDocument.lord': {$eq: 'cat'}},"
        " {'subDocument.friend': {$eq: 'dog'}}]}]},"
        " {'subDocument': {$type: [4]}}, {'subDocument': {$not: {$type: [3]}}}]}},"
        " {$replaceRoot: {newRoot: '$subDocument'}}"
        "]";
    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe, serializedPipe);
}

TEST_F(PipelineOptimizationTest, MultipleMatchesPushedBeforeReplaceWith) {
    std::string inputPipe =
        "["
        " {$replaceWith: '$subDocument'},"
        " {$match: {x: 'small'}},"
        " {$match: {y: 1}}"
        "]";
    std::string outputPipe =
        "["
        " {$match: {$or: [{$and: [{'subDocument.x': {$eq: 'small'}},"
        " {'subDocument.y': {$eq: 1}}]},"
        " {'subDocument': {$not: {$type: [3]}}}, {'subDocument': {$type: [4]}}]}},"
        " {$replaceRoot: {newRoot: '$subDocument'}}"
        "]";
    std::string serializedPipe =
        "["
        " {$match: {$and: [{$or: [{'subDocument.x': {$eq: 'small'}},"
        " {'subDocument': {$type: [4]}}, {'subDocument': {$not: {$type: [3]}}}]},"
        " {$or: [{'subDocument.y': {$eq: 1}},"
        " {'subDocument': {$type: [4]}}, {'subDocument': {$not: {$type: [3]}}}]}]}},"
        " {$replaceRoot: {newRoot: '$subDocument'}}"
        "]";
    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe, serializedPipe);
}

TEST_F(PipelineOptimizationTest, MatchPushedBeforeMultipleReplaceWiths) {
    std::string inputPipe =
        "["
        " {$replaceWith: '$subDocumentA'},"
        " {$replaceWith: '$subDocumentB'},"
        " {$match: {'x.a': 2}}"
        "]";
    std::string outputPipe =
        "["
        " {$match: {$or: [{'subDocumentA.subDocumentB.x.a': {$eq: 2}},"
        " {'subDocumentA': {$not: {$type: [3]}}},"
        " {'subDocumentA.subDocumentB': {$not: {$type: [3]}}},"
        " {'subDocumentA': {$type: [4]}}, {'subDocumentA.subDocumentB': {$type: [4]}}]}},"
        " {$replaceRoot: {newRoot: '$subDocumentA'}},"
        " {$replaceRoot: {newRoot: '$subDocumentB'}}"
        "]";
    std::string serializedPipe =
        "["
        " {$match: {$or: [{'subDocumentA.subDocumentB.x.a': {$eq: 2}},"
        " {'subDocumentA.subDocumentB': {$type: [4]}},"
        " {'subDocumentA.subDocumentB': {$not: {$type: [3]}}},"
        " {'subDocumentA': {$type: [4]}}, {'subDocumentA': {$not: {$type: [3]}}}]}},"
        " {$replaceRoot: {newRoot: '$subDocumentA'}},"
        " {$replaceRoot: {newRoot: '$subDocumentB'}}"
        "]";
    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe, serializedPipe);
}

TEST_F(PipelineOptimizationTest, NoReplaceWithMatchOptForExprMatch) {
    std::string inputPipe =
        "["
        " {$replaceWith: '$subDocument'},"
        " {$match: {$expr: {$eq: ['$x', 2]}}}"
        "]";
    std::string outputPipe =
        "["
        " {$replaceRoot: {newRoot: '$subDocument'}},"
        " {$match: {$and: [{$expr: {$eq: ['$x', {$const: 2}]}},"
        " {'x': {$_internalExprEq: 2}}]}}"
        "]";
    std::string serializedPipe =
        "["
        " {$replaceRoot: {newRoot: '$subDocument'}},"
        " {$match: {$expr: {$eq: ['$x', 2]}}}"
        "]";
    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe, serializedPipe);
}

// TODO SERVER-88463: Enable match pushdown when predicates in the previous stage and the $match
// stage are independent but have the same name
TEST_F(PipelineOptimizationTest, NoReplaceWithMatchOptSamePredicateName) {
    std::string inputPipe =
        "["
        " {$replaceWith: '$subDocument'},"
        " {$match: {'subDocument.x': 2}}"
        "]";
    std::string outputPipe =
        "["
        " {$replaceRoot: {newRoot: '$subDocument'}},"
        " {$match: {'subDocument.x': {$eq: 2}}}"
        "]";
    std::string serializedPipe =
        "["
        " {$replaceRoot: {newRoot: '$subDocument'}},"
        " {$match: {'subDocument.x': 2}}"
        "]";
    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe, serializedPipe);
}

// TODO SERVER-88463: Enable match pushdown when predicates in the previous stage and the $match
// stage are independent but have the same name
TEST_F(PipelineOptimizationTest, MatchNotPushedBeforeMultipleReplaceWithsSamePredName) {
    std::string inputPipe =
        "["
        " {$replaceWith: '$subDocument'},"
        " {$replaceWith: '$subDocument'},"
        " {$match: {'x.a': 2}}"
        "]";
    std::string outputPipe =
        "["
        " {$replaceRoot: {newRoot: '$subDocument'}},"
        " {$match: {$or: [{'subDocument.x.a': {$eq: 2}},"
        " {'subDocument': {$not: {$type: [3]}}}, {'subDocument': {$type: [4]}}]}},"
        " {$replaceRoot: {newRoot: '$subDocument'}}"
        "]";
    std::string serializedPipe =
        "["
        " {$replaceRoot: {newRoot: '$subDocument'}},"
        " {$match: {$or: [{'subDocument.x.a': {$eq: 2}},"
        " {'subDocument': {$type: [4]}}, {'subDocument': {$not: {$type: [3]}}}]}},"
        " {$replaceRoot: {newRoot: '$subDocument'}}"
        "]";
    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe, serializedPipe);
}

TEST_F(PipelineOptimizationTest, internalAllCollectionStatsAbsorbsMatchOnNs) {
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

TEST_F(PipelineOptimizationTest, internalAllCollectionStatsAbsorbsSeveralMatchesOnNs) {
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

TEST_F(PipelineOptimizationTest, internalAllCollectionStatsDoesNotAbsorbMatchNotOnNs) {
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

TEST_F(PipelineOptimizationTest, ProjectGetsPushedIntoBothChildrenOfUnion) {
    assertPipelineOptimizesAndSerializesTo(
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
    assertPipelineOptimizesAndSerializesTo(
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
    assertPipelineOptimizesAndSerializesTo(
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

TEST_F(PipelineOptimizationTest, UnionWithViewsSampleUseCase) {
    // Test that if someone uses $unionWith to query one logical collection from four physical
    // collections then the query and projection can get pushed down to next to each collection
    // access.
    assertPipelineOptimizesAndSerializesTo(
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

std::unique_ptr<Pipeline> getOptimizedPipeline(const BSONObj inputBson) {
    QueryTestServiceContext testServiceContext;
    auto opCtx = testServiceContext.makeOperationContext();

    ASSERT_EQUALS(inputBson["pipeline"].type(), BSONType::array);
    std::vector<BSONObj> rawPipeline;
    for (auto&& stageElem : inputBson["pipeline"].Array()) {
        ASSERT_EQUALS(stageElem.type(), BSONType::object);
        rawPipeline.push_back(stageElem.embeddedObject());
    }
    AggregateCommandRequest request(kTestNss, rawPipeline);
    boost::intrusive_ptr<ExpressionContextForTest> ctx =
        new ExpressionContextForTest(opCtx.get(), request);
    ctx->setMongoProcessInterface(std::make_shared<StubExplainInterface>());
    unittest::TempDir tempDir("PipelineTest");
    ctx->setTempDir(tempDir.path());

    auto outputPipe = Pipeline::parse(request.getPipeline(), ctx);
    outputPipe->optimizePipeline();
    return outputPipe;
}

void assertTwoPipelinesOptimizeAndMergeTo(const std::string& inputPipe1,
                                          const std::string& inputPipe2,
                                          const std::string& outputPipe) {
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
    auto opts = SerializationOptions{
        .verbosity = boost::make_optional(ExplainOptions::Verbosity::kQueryPlanner)};
    ASSERT_VALUE_EQ(Value(pipeline1->writeExplainOps(opts)), Value(outputBson["pipeline"]));
}

TEST_F(PipelineOptimizationTest, MergeUnwindPipelineWithSortLimitPipelineDoesNotSwapIfNoPreserve) {
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

TEST_F(PipelineOptimizationTest, MergeUnwindPipelineWithSortLimitPipelineDoesSwapWithPreserve) {
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

TEST_F(PipelineOptimizationTest,
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

TEST_F(PipelineOptimizationTest, MergeUnwindPipelineWithSortLimitPipelinePlacesLimitProperly) {
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

TEST_F(PipelineOptimizationTest, CoalesceAdjacentExclusionProjectionsSimple) {
    const std::string inputPipe =
        "[{ $project: { a: false } }"
        ",{ $project: { b: false } }"
        "]";
    const std::string outputPipe =
        "[{ $project: { a: false, b: false, _id: true } }"
        "]";
    assertPipelineOptimizesTo(inputPipe, outputPipe);
}

TEST_F(PipelineOptimizationTest, CoalesceAdjacentExclusionProjectionsTernary) {
    const std::string inputPipe =
        "[{ $project: { a: false } }"
        ",{ $project: { b: false } }"
        ",{ $project: { c: false } }"
        "]";
    const std::string outputPipe =
        "[{$project: {a: false, b: false, c: false, _id: true}}"
        "]";
    // Projections are coalesced.
    assertPipelineOptimizesTo(inputPipe, outputPipe);
}

TEST_F(PipelineOptimizationTest, CoalesceAdjacentExclusionProjectionsIdFalse) {
    const std::string inputPipe =
        "[{ $project: { a: false } }"
        ",{ $project: { _id: false } }"
        "]";
    const std::string outputPipe =
        "[{ $project: { _id: false, a: false } }"
        "]";
    // Projections are coalesced.
    assertPipelineOptimizesTo(inputPipe, outputPipe);
}

TEST_F(PipelineOptimizationTest, CoalesceAdjacentExclusionProjectionsIdTrue) {
    const std::string inputPipe =
        "[{ $project: { _id: true } }"
        ",{ $project: { a: false, _id: true } }"
        "]";
    const std::string outputPipe =
        "[{ $project: { _id: true } }"
        ",{ $project: { a: false, _id: true } }"
        "]";
    // Projections are not coalesced.
    assertPipelineOptimizesTo(inputPipe, outputPipe);
}

TEST_F(PipelineOptimizationTest, CoalesceAdjacentExclusionProjectionsUnset) {
    const std::string inputPipe =
        "[{ $unset: 'a' }"
        ",{ $unset: 'b' }"
        "]";
    const std::string outputPipe =
        "[{ $project: { a: false, b: false, _id: true } }"
        "]";
    // Projections are coalesced.
    assertPipelineOptimizesTo(inputPipe, outputPipe);
}

TEST_F(PipelineOptimizationTest, CoalesceAdjacentExclusionProjectionsNestedFirst) {
    const std::string inputPipe =
        "[{ $project: { a: { b: false } } }"
        ",{ $project: { a: false } }"
        "]";
    const std::string outputPipe =
        "[{ $project: { a: { b: false }, _id: true } }"
        ",{ $project: { a: false, _id: true } }"
        "]";
    // Projections with nested fields are not coalesced.
    assertPipelineOptimizesTo(inputPipe, outputPipe);
}

TEST_F(PipelineOptimizationTest, CoalesceAdjacentExclusionProjectionsNestedSecond) {
    const std::string inputPipe =
        "[{ $project: { a: false } }"
        ",{ $project: { a: { b: false } } }"
        "]";
    const std::string outputPipe =
        "[{ $project: { a: false, _id: true } }"
        ",{ $project: { a: { b: false }, _id: true } }"
        "]";
    // Projections with nested fields are not coalesced.
    assertPipelineOptimizesTo(inputPipe, outputPipe);
}

TEST_F(PipelineOptimizationTest, internalListCollectionsAbsorbsMatchOnDb) {
    std::string inputPipe =
        "["
        " {$_internalListCollections: {}},"
        " {$match: {db: 'testDb', a: 10}}"
        "]";
    std::string outputPipe =
        "["
        " {$_internalListCollections: {match: {db: {$eq: 'testDb'}}}},"
        " {$match: {a: {$eq: 10}}}"
        "]";
    std::string serializedPipe =
        "["
        " {$_internalListCollections: {}},"
        " {$match: {db: {$eq: 'testDb'}}},"
        " {$match: {a: {$eq: 10}}}"
        "]";
    assertPipelineOptimizesAndSerializesTo(
        inputPipe, outputPipe, serializedPipe, kAdminCollectionlessNss);
}

TEST_F(PipelineOptimizationTest, internalListCollectionsAbsorbsSeveralMatchesOnDb) {
    std::string inputPipe =
        "["
        " {$_internalListCollections: {}},"
        " {$match: {db: {$gt: 0}}},"
        " {$match: {a: 10}},"
        " {$match: {db: {$ne: 5}}}"
        "]";
    std::string outputPipe =
        "["
        " {$_internalListCollections: {match: {$and: [{db: {$gt: 0}}, {db: {$not: {$eq: "
        "5}}}]}}},"
        " {$match: {a: {$eq: 10}}}"
        "]";
    std::string serializedPipe =
        "["
        " {$_internalListCollections: {}},"
        " {$match: {$and: [{db: {$gt: 0}}, {db: {$not: {$eq: 5}}}]}},"
        " {$match: {a: {$eq: 10}}}"
        "]";
    assertPipelineOptimizesAndSerializesTo(
        inputPipe, outputPipe, serializedPipe, kAdminCollectionlessNss);
}

TEST_F(PipelineOptimizationTest, internalAllCollectionStatsDoesNotAbsorbMatchNotOnDb) {
    std::string inputPipe =
        "["
        " {$_internalListCollections: {}},"
        " {$match: {a: 10}}"
        "]";
    std::string outputPipe =
        "["
        " {$_internalListCollections: {}},"
        " {$match: {a: {$eq: 10}}}"
        "]";
    std::string serializedPipe =
        "["
        " {$_internalListCollections: {}},"
        " {$match: {a: 10}}"
        "]";
    assertPipelineOptimizesAndSerializesTo(
        inputPipe, outputPipe, serializedPipe, kAdminCollectionlessNss);
}

TEST_F(PipelineOptimizationTest, SerializationForLogging) {
    std::vector<BSONObj> pipeVec = {fromjson("{$match: {'a.c': {$eq: 5}}}"),
                                    fromjson("{$project: {_id: true}}")};

    QueryTestServiceContext serviceContext;
    auto opCtx = serviceContext.makeOperationContext();
    AggregateCommandRequest request(kTestNss, pipeVec);
    boost::intrusive_ptr<ExpressionContextForTest> ctx =
        new ExpressionContextForTest(opCtx.get(), request);
    auto pipeline = Pipeline::parse(pipeVec, ctx);

    Value unredacted = Value(pipeline->serializeToBson());
    Value redacted = Value(std::vector<BSONObj>{fromjson("{$match: {'a.c': {$eq: '###'}}}"),
                                                fromjson("{$project: {_id: '###'}}")});

    // With log redaction enabled, all serialization for logging should produce 'redacted'.
    logv2::setShouldRedactLogs(true);
    ASSERT_VALUE_EQ(Value(pipeline->serializeForLogging()), redacted);
    ASSERT_VALUE_EQ(Value(Pipeline::serializePipelineForLogging(pipeVec)), redacted);
    ASSERT_VALUE_EQ(Value(Pipeline::serializeContainerForLogging(pipeline->getSources())),
                    redacted);

    // With log redaction disabled, we should get the input pipeline back.
    logv2::setShouldRedactLogs(false);
    ASSERT_VALUE_EQ(Value(pipeline->serializeForLogging()), unredacted);
    ASSERT_VALUE_EQ(Value(Pipeline::serializePipelineForLogging(pipeVec)), unredacted);
    ASSERT_VALUE_EQ(Value(Pipeline::serializeContainerForLogging(pipeline->getSources())),
                    unredacted);
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
            RoutingContext routingCtx(opCtx, {ns});
            return CommonProcessInterface::findOwningShard(opCtx, routingCtx, ns);
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
        return NamespaceString::createNamespaceString_forTest(kDBName, "lookupColl");
    }

    BSONObj pipelineFromJsonArray(const std::string& array) {
        return fromjson("{pipeline: " + array + "}");
    }

    void doTest(const std::string& inputPipeJson,
                const std::string& shardPipeJson,
                const std::string& mergePipeJson,
                boost::optional<OrderedPathSet> shardKey = boost::none) {
        const BSONObj inputBson = pipelineFromJsonArray(inputPipeJson);
        const BSONObj shardPipeExpected = pipelineFromJsonArray(shardPipeJson);
        const BSONObj mergePipeExpected = pipelineFromJsonArray(mergePipeJson);

        ASSERT_EQUALS(inputBson["pipeline"].type(), BSONType::array);
        std::vector<BSONObj> rawPipeline;
        for (auto&& stageElem : inputBson["pipeline"].Array()) {
            ASSERT_EQUALS(stageElem.type(), BSONType::object);
            rawPipeline.push_back(stageElem.embeddedObject());
        }
        AggregateCommandRequest request(kTestNss, rawPipeline);
        boost::intrusive_ptr<ExpressionContextForTest> ctx = createExpressionContext(request);
        unittest::TempDir tempDir("PipelineTest");
        ctx->setTempDir(tempDir.path());
        ctx->setMongoProcessInterface(
            std::make_shared<Sharded::ShardMergerMongoProcessInterface>(getCatalogCacheMock()));

        // For $graphLookup and $lookup, we have to populate the resolvedNamespaces so that the
        // operations will be able to have a resolved view definition.
        auto lookupCollNs = getLookupCollNs();
        ctx->setResolvedNamespace(lookupCollNs, {lookupCollNs, std::vector<BSONObj>{}});

        // Test that we can both split the pipeline and reassemble it into its original form.
        mergePipe = Pipeline::parse(request.getPipeline(), ctx);
        mergePipe->optimizePipeline();

        auto splitPipeline =
            sharded_agg_helpers::SplitPipeline::split(std::move(mergePipe), shardKey);
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
    std::unique_ptr<Pipeline> mergePipe;
    std::unique_ptr<Pipeline> shardPipe;
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

    void doTest(const std::string& inputPipeJson,
                const std::string& shardPipeJson,
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
    doTest(
        "[{$match: {x: 4}}, {$skip: 10}, {$group: {_id: '$y'}}, {$limit: 5}]" /*inputPipeJson*/,
        "[{$match: {x: {$eq: 4}}}, {$project: {y: true, _id: false}}]" /*shardPipeJson*/,
        "[{$skip: 10}, {$group: {_id: '$y', $willBeMerged: false}}, {$limit: 5}]" /*mergePipeJson*/);
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
           "[{$limit:1}, {$group: {_id: '$_id', $willBeMerged: false}}]" /*mergePipeJson*/);
};

TEST_F(PipelineOptimizations, LimitFieldsSentFromShardsToMergerJustNeedsNonId) {
    doTest("[{$limit:1}, {$group: {_id: '$a.b'}}]" /*inputPipeJson*/,
           "[{$limit:1}, {$project: {a: {b: true}, _id: false}}]" /*shardPipeJson*/,
           "[{$limit:1}, {$group: {_id: '$a.b', $willBeMerged: false}}]" /*mergePipeJson*/);
};

TEST_F(PipelineOptimizations, LimitFieldsSentFromShardsToMergerNothingNeeded) {
    static const std::string kInputPipeJson =
        "[{$limit:1},"
        "{$group: {_id: {$const: null}, count: {$sum: {$const: 1}}}}]";
    doTest(kInputPipeJson,
           "[{$limit:1}, {$project: {_id: true}}]" /*shardPipeJson*/,
           "[{$limit:1}, {$group: {_id: {$const: null}, count: {$sum: {$const: 1}}, $willBeMerged: "
           "false}}]" /*mergePipeJson*/);
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
        "[{$group: {_id:{a: '$a'}, $willBeMerged: false}},{$project:{_id: true, a: true}},{$limit: "
        "5}]" /*mergePipeJson*/);
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

namespace pushDownGroupPrecededBySort {

TEST_F(PipelineOptimizations, ShouldNotPushdownGroupWithoutShardKey) {
    doTest("[{$sort: {a: 1}}, {$group: {_id: '$_id'}}]" /*inputPipeJson*/,
           "[{$sort: {sortKey: {a: 1}}}, {$project: {_id: true}}]" /*shardPipeJson*/,
           "[{$group: {_id: '$_id', $willBeMerged: false}}]" /*mergePipeJson*/);
};

TEST_F(PipelineOptimizations, ShouldNotPushdownGroupIfNotOnShardKey) {
    const OrderedPathSet shardKey = {"a", "c"};
    doTest("[{$sort: {a: 1}}, {$group: {_id: '$c'}}]" /*inputPipeJson*/,
           "[{$sort: {sortKey: {a: 1}}}, {$project: {c: true, _id: false}}]" /*shardPipeJson*/,
           "[{$group: {_id: '$c', $willBeMerged: false}}]" /*mergePipeJson*/,
           shardKey);
};

TEST_F(PipelineOptimizations, ShouldNotPushdownGroupIfProjectDoesNotPreserveShardKey) {
    const OrderedPathSet shardKey = {"shardKey"};
    doTest(
        "[{$project: {shardKey: '$other'}}, {$sort: {shardKey: 1}}, {$group: {_id: '$shardKey'}}]" /*inputPipeJson*/
        ,
        "[{$project: {_id: true, shardKey: '$other'}}, {$sort: {sortKey: {shardKey: 1}}}]" /*shardPipeJson*/
        ,
        "[{$group: {_id: '$shardKey', $willBeMerged: false}}]" /*mergePipeJson*/,
        shardKey);
};

TEST_F(PipelineOptimizations, ShouldNotPushdownGroupIfShardKeyIsUnset) {
    const OrderedPathSet shardKey = {"shardKey"};
    doTest(
        "[{$unset: 'shardKey'}, {$sort: {shardKey: 1}}, {$group: {_id: '$shardKey'}}]" /*inputPipeJson*/
        ,
        "[{$project: {shardKey: false, _id: true}}"
        ",{$sort: {sortKey: {shardKey: 1}}}"
        ",{$project: {shardKey: true, _id: false}}]" /*shardPipeJson*/,
        "[{$group: {_id: '$shardKey', $willBeMerged: false}}]" /*mergePipeJson*/,
        shardKey);
};

TEST_F(PipelineOptimizations, ShouldNotPushdownGroupIfAddFieldsOverwritesShardKey) {
    const OrderedPathSet shardKey = {"shardKey"};
    doTest(
        "[{$addFields: {shardKey: '$other'}}, {$sort: {shardKey: 1}}, {$group: {_id: '$shardKey'}}]" /*inputPipeJson*/
        ,
        "[{$addFields: {shardKey: '$other'}}"
        ",{$sort: {sortKey: {shardKey: 1}}}"
        ",{$project: {shardKey: true, _id: false}}]" /*shardPipeJson*/,
        "[{$group: {_id: '$shardKey', $willBeMerged: false}}]" /*mergePipeJson*/,
        shardKey);
};

TEST_F(PipelineOptimizations, ShouldNotPushdownGroupIfUsingAddFieldsWithoutShardFilteringDistinct) {
    RAIIServerParameterControllerForTest controller("featureFlagShardFilteringDistinctScan", false);
    const OrderedPathSet shardKey = {"shardKey"};
    doTest(
        "[{$addFields: {new: '$shardKey'}}, {$sort: {shardKey: 1}}, {$group: {_id: '$shardKey'}}]" /*inputPipeJson*/
        ,
        "[{$addFields: {new: '$shardKey'}}"
        ",{$sort: {sortKey: {shardKey: 1}}}"
        ",{$project: {shardKey: true, _id: false}}]" /*shardPipeJson*/,
        "[{$group: {_id: '$shardKey'}}]" /*mergePipeJson*/,
        shardKey);
};

TEST_F(PipelineOptimizations, ShouldPushdownGroupIfUsingAddFields) {
    RAIIServerParameterControllerForTest controller("featureFlagShardFilteringDistinctScan", true);
    const OrderedPathSet shardKey = {"shardKey"};
    doTest(
        "[{$addFields: {new: '$shardKey'}}, {$sort: {shardKey: 1}}, {$group: {_id: '$shardKey'}}]", /*inputPipeJson*/
        "[{$addFields: {new: '$shardKey'}}"
        ",{$sort: {sortKey: {shardKey: 1}}}"
        ",{$group: {_id: '$shardKey', $willBeMerged: false}}]", /*shardPipeJson*/
        // Empty merge as group is fully pushed down.
        "[]", /*mergePipeJson*/
        shardKey);
};

TEST_F(PipelineOptimizations, ShouldPushdownGroupOnShardKey) {
    RAIIServerParameterControllerForTest controller("featureFlagShardFilteringDistinctScan", true);
    const OrderedPathSet shardKey = {"_id"};
    doTest(
        "[{$sort: {a: 1}}, {$group: {_id: '$_id'}}]" /*inputPipeJson*/,
        "[{$sort: {sortKey: {a: 1}}}, {$group: {_id: '$_id', $willBeMerged: false}}]" /*shardPipeJson*/
        ,
        // Empty merge as group is fully pushed down.
        "[]" /*mergePipeJson*/,
        shardKey);
};

TEST_F(PipelineOptimizations, ShouldPushdownGroupOnSupersetOfShardKey) {
    RAIIServerParameterControllerForTest controller("featureFlagShardFilteringDistinctScan", true);
    const OrderedPathSet shardKey = {"a", "b"};
    doTest("[{$sort: {a: 1}}, {$group: {_id: {a: '$a', b: '$b', c: '$c'}}}]", /*inputPipeJson*/
           "[{$sort: {sortKey: {a: 1}}}, {$group: {_id: {a: '$a', b: '$b', c: '$c'}, "
           "$willBeMerged: false}}]", /*shardPipeJson*/
           // Empty merge as group is fully pushed down.
           "[]", /*mergePipeJson*/
           shardKey);
};

TEST_F(PipelineOptimizations, ShouldPushdownGroupOnSupersetOfShardKeyInArray) {
    RAIIServerParameterControllerForTest controller("featureFlagShardFilteringDistinctScan", true);
    const OrderedPathSet shardKey = {"a", "b"};
    doTest("[{$sort: {a: 1}}, {$group: {_id: ['$a', '$b', '$c']}}]", /*inputPipeJson*/
           "[{$sort: {sortKey: {a: 1}}}, {$group: {_id: ['$a', '$b', '$c'], $willBeMerged: "
           "false}}]", /*shardPipeJson*/
           // Empty merge as group is fully pushed down.
           "[]", /*mergePipeJson*/
           shardKey);
};

TEST_F(PipelineOptimizations, ShouldPushdownGroupOnSupersetOfShardKeyInNestedStructure) {
    RAIIServerParameterControllerForTest controller("featureFlagShardFilteringDistinctScan", true);
    const OrderedPathSet shardKey = {"a", "b"};
    doTest("[{$sort: {a: 1}}, {$group: {_id: {foo:[{bar:'$a'}, '$b', '$c']}}}]", /*inputPipeJson*/
           "[{$sort: {sortKey: {a: 1}}}, {$group: {_id: {foo:[{bar:'$a'}, '$b', '$c']}, "
           "$willBeMerged: false}}]", /*shardPipeJson*/
           // Empty merge as group is fully pushed down.
           "[]", /*mergePipeJson*/
           shardKey);
};

TEST_F(PipelineOptimizations, ShouldPushdownGroupOnSupersetOfShardKeyWithIrrelevantFieldsModified) {
    RAIIServerParameterControllerForTest controller("featureFlagShardFilteringDistinctScan", true);
    const OrderedPathSet shardKey = {"a", "b"};
    doTest(
        "[{$addFields: {c:{$const:'foobar'}}}, {$sort: {a: 1}}, {$group: {_id: ['$a', '$b', "
        "'$c']}}]", /*inputPipeJson*/
        "[{$addFields: {c:{$const:'foobar'}}}, {$sort: {sortKey: {a: 1}}}, {$group: {_id: ['$a', "
        "'$b', '$c'], $willBeMerged: false}}]", /*shardPipeJson*/
        // Empty merge as group is fully pushed down.
        "[]", /*mergePipeJson*/
        shardKey);
};

TEST_F(PipelineOptimizations, ShouldPushdownGroupOnShardKeyWithDuplicatesViaRenameProject) {
    RAIIServerParameterControllerForTest controller("featureFlagShardFilteringDistinctScan", true);
    const OrderedPathSet shardKey = {"a"};
    doTest(
        "[{$project:{'a':true, b:'$a'}}, {$sort: {a: 1}}, {$group: {_id: ['$a', '$b']}}]", /*inputPipeJson*/
        "[{$project:{_id:true, 'a':true, b:'$a'}}, {$sort: {sortKey: {a: 1}}}, {$group: {_id: "
        "['$a', '$b'], $willBeMerged: false}}]", /*shardPipeJson*/
        // Empty merge as group is fully pushed down.
        "[]", /*mergePipeJson*/
        shardKey);
};

TEST_F(PipelineOptimizations, ShouldPushdownGroupOnShardKeyWithDuplicatesViaRenameAddFields) {
    RAIIServerParameterControllerForTest controller("featureFlagShardFilteringDistinctScan", true);
    const OrderedPathSet shardKey = {"a"};
    doTest(
        "[{$addFields:{b:'$a'}}, {$sort: {a: 1}}, {$group: {_id: ['$a', '$b']}}]", /*inputPipeJson*/
        "[{$addFields:{b:'$a'}}, {$sort: {sortKey: {a: 1}}}, {$group: {_id: ['$a', '$b'], "
        "$willBeMerged: false}}]", /*shardPipeJson*/
        // Empty merge as group is fully pushed down.
        "[]", /*mergePipeJson*/
        shardKey);
};

TEST_F(PipelineOptimizations, ShouldPushdownGroupOnShardKeyWithUnrelatedExclusion) {
    RAIIServerParameterControllerForTest controller("featureFlagShardFilteringDistinctScan", true);
    const OrderedPathSet shardKey = {"a"};
    doTest(
        "[{$project:{'c':false}}, {$sort: {a: 1}}, {$group: {_id: ['$a', '$b']}}]", /*inputPipeJson*/
        "[{$project:{'c':false, _id:true}}, {$sort: {sortKey: {a: 1}}}, {$group: {_id: ['$a', "
        "'$b'], $willBeMerged: false}}]", /*shardPipeJson*/
        // Empty merge as group is fully pushed down.
        "[]", /*mergePipeJson*/
        shardKey);
};

TEST_F(PipelineOptimizations, ShouldNotPushdownGroupIfComputesValue) {
    // A group _id which is _derived_ from the shard key is not sufficient.
    const OrderedPathSet shardKey = {"shardKey"};
    doTest(
        "[{$sort: {shardKey: 1}}, {$group: {_id: {'$add':[{$const: 1}, '$shardKey']}}}]", /*inputPipeJson*/
        "[{$sort: {sortKey: {shardKey: 1}}} ,{$project: {shardKey: true, _id: false}}]", /*shardPipeJson*/
        "[{$group: {_id: {'$add':[{$const: 1}, '$shardKey']}, $willBeMerged: false}}]", /*mergePipeJson*/
        shardKey);
};

TEST_F(PipelineOptimizations, ShouldPushdownGroupOnShardKeyWithRenames) {
    RAIIServerParameterControllerForTest controller("featureFlagShardFilteringDistinctScan", true);
    const OrderedPathSet shardKey = {"shardKey"};
    doTest(
        "[{$project: {rename: '$shardKey'}}"
        ",{$project: {anotherRename: '$rename'}}"
        ",{$sort: {anotherRename: 1}}"
        ",{$group: {_id: '$anotherRename'}}]" /*inputPipeJson*/,
        "[{$project: {_id: true, rename: '$shardKey'}}"
        ",{$project: {_id: true, anotherRename: '$rename'}}"
        ",{$sort: {sortKey: {anotherRename: 1}}}"
        ",{$group: {_id: '$anotherRename', $willBeMerged: false}}]" /*shardPipeJson*/,
        // Empty merge as group is fully pushed down.
        "[]" /*mergePipeJson*/,
        shardKey);
};

TEST_F(PipelineOptimizations, ShouldPushdownGroupOnShardKeyWithMultipleRenames) {
    RAIIServerParameterControllerForTest controller("featureFlagShardFilteringDistinctScan", true);
    const OrderedPathSet shardKey = {"shardKey"};
    doTest(
        "[{$project: {rename: '$shardKey'}}"
        ",{$project: {anotherRename: '$rename', anotherRename2: '$rename'}}"
        ",{$sort: {anotherRename2: 1}}"
        ",{$group: {_id: '$anotherRename2'}}]" /*inputPipeJson*/,
        "[{$project: {_id: true, rename: '$shardKey'}}"
        ",{$project: {_id: true, anotherRename: '$rename', anotherRename2: '$rename'}}"
        ",{$sort: {sortKey: {anotherRename2: 1}}}"
        ",{$group: {_id: '$anotherRename2', $willBeMerged: false}}]" /*shardPipeJson*/,
        // Empty merge as group is fully pushed down.
        "[]" /*mergePipeJson*/,
        shardKey);
};

TEST_F(PipelineOptimizations, ShouldPushdownGroupOnShardKeyWithMatchBetweenSortAndGroup) {
    RAIIServerParameterControllerForTest controller("featureFlagShardFilteringDistinctScan", true);
    const OrderedPathSet shardKey = {"shardKey"};
    doTest(
        "[{$sort: {shardKey: 1}}, {$match: {shardKey: 'val'}}, {$group: {_id: '$shardKey'}}]" /*inputPipeJson*/
        ,
        "[{$match: {shardKey: {$eq: 'val'}}}"
        ",{$sort: {sortKey: {shardKey: 1}}}"
        ",{$group: {_id: '$shardKey', $willBeMerged: false}}]" /*shardPipeJson*/,
        // Empty merge as group is fully pushed down.
        "[]" /*mergePipeJson*/,
        shardKey);
};

TEST_F(PipelineOptimizations, ShouldPushdownGroupOnShardKeyWithFirstAccumulator) {
    RAIIServerParameterControllerForTest controller("featureFlagShardFilteringDistinctScan", true);
    const OrderedPathSet shardKey = {"shardKey"};
    doTest(
        "[{$sort: {shardKey: 1}}, {$group: {_id: '$shardKey', first: {$first: '$other'}}}]" /*inputPipeJson*/
        ,
        "[{$sort: {sortKey: {shardKey: 1}}}, {$group: {_id: '$shardKey', first: {$first: "
        "'$other'}, $willBeMerged: false}}]" /*shardPipeJson*/,
        // Empty merge as group is fully pushed down.
        "[]" /*mergePipeJson*/
        ,
        shardKey);
};

TEST_F(PipelineOptimizations, ShouldPushdownGroupOnShardKeyWithTopAccumulator) {
    RAIIServerParameterControllerForTest controller("featureFlagShardFilteringDistinctScan", true);
    const OrderedPathSet shardKey = {"shardKey"};
    doTest(
        "[{$sort: {shardKey: 1}}"
        ",{$group: {_id: '$shardKey', top: {$top: {output: '$other', sortBy: {other: 1}}}}}]" /*inputPipeJson*/
        ,
        "[{$sort: {sortKey: {shardKey: 1}}}"
        ",{$group: {_id: '$shardKey', top: {$top: {output: '$other', sortBy: {other: 1}}}, "
        "$willBeMerged: false}}]" /*shardPipeJson*/
        ,
        // Empty merge as group is fully pushed down.
        "[]" /*mergePipeJson*/,
        shardKey);
};

}  // namespace pushDownGroupPrecededBySort

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

    void doTest(const std::string& inputPipeJson,
                const std::string& shardPipeJson,
                const std::string& mergePipeJson,
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
            NamespaceString::createNamespaceString_forTest(kDBName, "outColl"),
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
            NamespaceString::createNamespaceString_forTest(kDBName, "outColl"),
            CollectionRoutingInfo{
                ChunkManager{makeStandaloneRoutingTableHistory(std::move(rt)), timestamp},
                DatabaseTypeValueHandle(
                    DatabaseType{DatabaseName::createDatabaseName_forTest(boost::none, kDBName),
                                 kMyShardName,
                                 DatabaseVersion{}})});

        static const std::string kSentPipeJson = "[{$merge: {into: {db: '" + kDBName +
            "', coll: 'outColl'}, on: '_id', "
            "whenMatched: 'merge', whenNotMatched: 'insert', "
            "allowMergeOnNullishValues: true}}]";

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
    const auto nss = NamespaceString::createNamespaceString_forTest(kDBName, "outColl");

    getCatalogCacheMock()->setCollectionReturnValue(
        nss,
        CatalogCacheMock::makeCollectionRoutingInfoUnsplittable(
            nss, ShardId("dbPrimary"), DatabaseVersion{UUID::gen(), timestamp}, kMyShardName));

    doTest("[{$out: 'outColl'}]" /*inputPipeJson*/,
           "[]" /*shardPipeJson*/,
           "[{$out: {coll: 'outColl', db: '" + kDBName + "'}}]" /*mergePipeJson*/,
           kMyShardName /* mergeShardId */);
};

TEST_F(PipelineOptimizationsShardMerger, MergeWithUntrackedCollection) {
    RAIIServerParameterControllerForTest featureFlagController(
        "featureFlagAllowMergeOnNullishValues", true);

    const Timestamp timestamp{1, 1};
    getCatalogCacheMock()->setCollectionReturnValue(
        NamespaceString::createNamespaceString_forTest(kDBName, "outColl"),
        CollectionRoutingInfo{
            ChunkManager{RoutingTableHistoryValueHandle{OptionalRoutingTableHistory{}}, timestamp},
            DatabaseTypeValueHandle(
                DatabaseType{DatabaseName::createDatabaseName_forTest(boost::none, kDBName),
                             kMyShardName,
                             DatabaseVersion{UUID::gen(), timestamp}})});
    doTest("[{$merge: 'outColl'}]" /*inputPipeJson*/,
           "[]" /*shardPipeJson*/,
           "[{$merge: {into: {db: '" + kDBName +
               "', coll: 'outColl'}, on: '_id', "
               "whenMatched: 'merge', whenNotMatched: 'insert', "
               "allowMergeOnNullishValues: true}}]" /*mergePipeJson*/,
           kMyShardName /*needsSpecificShardMerger*/);
};

TEST_F(PipelineOptimizationsShardMerger, MergeWithShardedCollection) {
    RAIIServerParameterControllerForTest featureFlagController(
        "featureFlagAllowMergeOnNullishValues", true);
    doMergeWithCollectionWithRoutingTableTest(false /*unsplittable*/);
};

TEST_F(PipelineOptimizationsShardMerger, MergeWithUnsplittableCollection) {
    RAIIServerParameterControllerForTest featureFlagController(
        "featureFlagAllowMergeOnNullishValues", true);
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
        CollectionRoutingInfo{
            ChunkManager{makeStandaloneRoutingTableHistory(std::move(rt)), timestamp},
            DatabaseTypeValueHandle(
                DatabaseType{DatabaseName::createDatabaseName_forTest(boost::none, kDBName),
                             kMyShardName,
                             DatabaseVersion{UUID::gen(), timestamp}})});
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
        CollectionRoutingInfo{
            ChunkManager{makeStandaloneRoutingTableHistory(std::move(rt)), timestamp},
            DatabaseTypeValueHandle(
                DatabaseType{DatabaseName::createDatabaseName_forTest(boost::none, kDBName),
                             kMyShardName,
                             DatabaseVersion{UUID::gen(), timestamp}})});
    doTest(
        "[{$lookup: {from : 'lookupColl', as : 'same', localField: 'left', foreignField: 'right'}}]" /* inputPipeJson */
        ,
        "[]" /* shardPipeJson */,
        "[{$lookup: {from : 'lookupColl', as : 'same', localField: 'left', foreignField: 'right'}}]" /* mergePipeJson */
        ,
        kMyShardName /* needsSpecificShardMerger */);
};

}  // namespace needsSpecificShardMerger

namespace mustRunOnRouter {
using HostTypeRequirement = StageConstraints::HostTypeRequirement;
using PipelineMustRunOnRouterTest = AggregationContextFixture;

TEST_F(PipelineMustRunOnRouterTest, UnsplittablePipelineMustRunOnRouter) {
    setExpCtx({.inRouter = true, .allowDiskUse = false});
    auto pipeline = makePipeline({matchStage("{x: 5}"), runOnRouter()});
    ASSERT_TRUE(pipeline->requiredToRunOnRouter());

    pipeline->optimizePipeline();
    ASSERT_TRUE(pipeline->requiredToRunOnRouter());
}

TEST_F(PipelineMustRunOnRouterTest, UnsplittableRouterPipelineAssertsIfDisallowedStagePresent) {
    setExpCtx({.inRouter = true, .allowDiskUse = true});
    auto pipeline = makePipeline({matchStage("{x: 5}"), runOnRouter(), sortStage("{x: 1}")});
    pipeline->optimizePipeline();

    // The entire pipeline must run on router, but $sort cannot do so when 'allowDiskUse' is true.
    ASSERT_TRUE(pipeline->requiredToRunOnRouter());
    ASSERT_NOT_OK(pipeline->canRunOnRouter());
}

DEATH_TEST_F(PipelineMustRunOnRouterTest,
             SplittablePipelineMustMergeOnRouterAfterSplit,
             "invariant") {
    setExpCtx({.inRouter = true, .allowDiskUse = false});
    auto pipeline =
        makePipeline({matchStage("{x: 5}"), splitStage(HostTypeRequirement::kNone), runOnRouter()});

    // We don't need to run the entire pipeline on router because we can split at
    // $_internalSplitPipeline.
    ASSERT_FALSE(pipeline->requiredToRunOnRouter());

    auto splitPipeline = sharded_agg_helpers::SplitPipeline::split(std::move(pipeline));
    ASSERT(splitPipeline.shardsPipeline);
    ASSERT(splitPipeline.mergePipeline);

    ASSERT_TRUE(splitPipeline.mergePipeline->requiredToRunOnRouter());

    // Calling 'requiredToRunOnRouter' on the shard pipeline will hit an invariant.
    splitPipeline.shardsPipeline->requiredToRunOnRouter();
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

TEST_F(PipelineMustRunOnRouterTest, SplitRouterMergePipelineAssertsIfShardStagePresent) {
    setExpCtx({.inRouter = true, .allowDiskUse = true});
    auto expCtx = getExpCtx();
    expCtx->setMongoProcessInterface(std::make_shared<FakeMongoProcessInterface>());
    auto pipeline = makePipeline(
        {matchStage("{x: 5}"), splitStage(HostTypeRequirement::kNone), runOnRouter(), outStage()});

    // We don't need to run the entire pipeline on router because we can split at
    // $_internalSplitPipeline.
    ASSERT_FALSE(pipeline->requiredToRunOnRouter());

    auto splitPipeline = sharded_agg_helpers::SplitPipeline::split(std::move(pipeline));

    // The merge pipeline must run on router, but $out needs to run on the primary shard.
    ASSERT_TRUE(splitPipeline.mergePipeline->requiredToRunOnRouter());
    ASSERT_NOT_OK(splitPipeline.mergePipeline->canRunOnRouter());
}

TEST_F(PipelineMustRunOnRouterTest, SplittablePipelineAssertsIfRouterStageOnShardSideOfSplit) {
    setExpCtx({.inRouter = true, .allowDiskUse = false});
    auto pipeline = makePipeline(
        {matchStage("{x: 5}"), runOnRouter(), splitStage(HostTypeRequirement::kAnyShard)});
    pipeline->optimizePipeline();

    // The 'runOnRouter' stage comes before any splitpoint, so this entire pipeline must run on
    // rotuer. However, the pipeline *cannot* run on router and *must* split at
    // $_internalSplitPipeline due to the latter's 'anyShard' requirement. The rotuer stage would
    // end up on the shard side of this split, and so it asserts.
    ASSERT_TRUE(pipeline->requiredToRunOnRouter());
    ASSERT_NOT_OK(pipeline->canRunOnRouter());
}

TEST_F(PipelineMustRunOnRouterTest, SplittablePipelineRunsUnsplitOnRouterIfSplitpointIsEligible) {
    setExpCtx({.inRouter = true, .allowDiskUse = false});
    auto pipeline =
        makePipeline({matchStage("{x: 5}"), runOnRouter(), splitStage(HostTypeRequirement::kNone)});
    pipeline->optimizePipeline();

    // The 'runOnRouter' stage is before the splitpoint, so this entire pipeline must run on router.
    // In this case, the splitpoint is itself eligible to run on router, and so we are able to
    // return true.
    ASSERT_TRUE(pipeline->requiredToRunOnRouter());
}

}  // namespace mustRunOnRouter

namespace DeferredSort {
using PipelineDeferredMergeSortTest = AggregationContextFixture;
using HostTypeRequirement = StageConstraints::HostTypeRequirement;

TEST_F(PipelineDeferredMergeSortTest, StageWithDeferredSortDoesNotSplit) {
    setExpCtx({.inRouter = true, .allowDiskUse = false});
    auto splitPipeline = makeAndSplitPipeline({mockDeferredSortStage(),
                                               swappableStage(),
                                               splitStage(HostTypeRequirement::kNone),
                                               matchStage("{b: 5}")});
    verifyPipelineForDeferredMergeSortTest(std::move(splitPipeline),
                                           2 /* shardsPipelineSize */,
                                           2 /* mergePipelineSize */,
                                           BSON("a" << 1));
}

TEST_F(PipelineDeferredMergeSortTest, EarliestSortIsSelectedIfDeferred) {
    setExpCtx({.inRouter = true, .allowDiskUse = false});
    auto splitPipeline = makeAndSplitPipeline({mockDeferredSortStage(),
                                               swappableStage(),
                                               sortStage("{NO: 1}"),
                                               splitStage(HostTypeRequirement::kNone),
                                               matchStage("{b: 5}")});
    verifyPipelineForDeferredMergeSortTest(std::move(splitPipeline),
                                           2 /* shardsPipelineSize */,
                                           3 /* mergePipelineSize */,
                                           BSON("a" << 1));
}

TEST_F(PipelineDeferredMergeSortTest, StageThatCantSwapGoesToMergingHalf) {
    setExpCtx({.inRouter = true, .allowDiskUse = false});
    auto match1 = matchStage("{a: 5}");
    auto match2 = matchStage("{b: 5}");
    auto splitPipeline = makeAndSplitPipeline(
        {mockDeferredSortStage(), match1, splitStage(HostTypeRequirement::kNone), match2});
    verifyPipelineForDeferredMergeSortTest(std::move(splitPipeline),
                                           1 /* shardsPipelineSize */,
                                           3 /* mergePipelineSize */,
                                           BSON("a" << 1));
}
}  // namespace DeferredSort
}  // namespace Sharded
}  // namespace Optimizations

class PipelineInitialSource : public ServiceContextTest {
public:
    std::unique_ptr<Pipeline> makePipeline(const std::string& pipelineStr) {
        std::vector<BSONObj> rawPipeline = {fromjson(pipelineStr)};
        auto opCtx = makeOperationContext();
        boost::intrusive_ptr<ExpressionContextForTest> ctx = new ExpressionContextForTest(
            opCtx.get(), AggregateCommandRequest(kTestNss, rawPipeline));
        return Pipeline::parse(rawPipeline, ctx);
    }
};

TEST_F(PipelineInitialSource, GeoNearInitialQuery) {
    auto pipe = makePipeline("{$geoNear: {distanceField: 'd', near: [0, 0], query: {a: 1}}}");
    ASSERT_BSONOBJ_EQ(pipe->getInitialQuery(), BSON("a" << 1));
}

TEST_F(PipelineInitialSource, MatchInitialQuery) {
    auto pipe = makePipeline("{$match: {'a': 4}}");
    ASSERT_BSONOBJ_EQ(pipe->getInitialQuery(), BSON("a" << 4));
}

// Contains test cases for validation done on pipeline creation.
namespace pipeline_validate {

class PipelineValidateTest : public AggregationContextFixture {
public:
    struct ExpressionContextOptionsStruct {
        bool hasCollectionName;
        bool setMockReplCoord;
    };

    boost::intrusive_ptr<mongo::ExpressionContextForTest> getExpCtx(
        ExpressionContextOptionsStruct options) {
        auto ctx = AggregationContextFixture::getExpCtx();

        // The db name string is always set to "a" (collectionless or not).
        ctx->setNamespaceString(
            (options.hasCollectionName)
                ? kTestNss  // Sets to a.collection when there should be a collection name.
                : NamespaceString::makeCollectionlessAggregateNSS(
                      DatabaseName::createDatabaseName_forTest(boost::none, "a")));

        if (options.setMockReplCoord) {
            setMockReplicationCoordinatorOnOpCtx(ctx->getOperationContext());
        }
        return ctx;
    }
};

TEST_F(PipelineValidateTest, AggregateOneNSNotValidForEmptyPipeline) {
    const std::vector<BSONObj> rawPipeline = {};
    auto ctx = getExpCtx({.hasCollectionName = false, .setMockReplCoord = false});

    ASSERT_THROWS_CODE(
        Pipeline::parse(rawPipeline, ctx), AssertionException, ErrorCodes::InvalidNamespace);
}

TEST_F(PipelineValidateTest, AggregateOneNSNotValidIfInitialStageRequiresCollection) {
    const std::vector<BSONObj> rawPipeline = {fromjson("{$match: {}}")};
    auto ctx = getExpCtx({.hasCollectionName = false, .setMockReplCoord = false});

    ASSERT_THROWS_CODE(
        Pipeline::parse(rawPipeline, ctx), AssertionException, ErrorCodes::InvalidNamespace);
}

TEST_F(PipelineValidateTest, AggregateOneNSValidIfInitialStageIsCollectionless) {
    auto ctx = getExpCtx({.hasCollectionName = true, .setMockReplCoord = false});
    auto collectionlessSource = DocumentSourceCollectionlessMock::create(ctx);

    makePipeline({collectionlessSource});
}

TEST_F(PipelineValidateTest, CollectionNSNotValidIfInitialStageIsCollectionless) {
    auto ctx = getExpCtx({.hasCollectionName = true, .setMockReplCoord = false});
    auto collectionlessSource = DocumentSourceCollectionlessMock::create(ctx);

    ASSERT_THROWS_CODE(Pipeline::parse({fromjson("{$listLocalSessions: {}}")},
                                       ctx),  // makePipeline({collectionlessSource}),
                       AssertionException,
                       ErrorCodes::InvalidNamespace);
}

TEST_F(PipelineValidateTest, AggregateOneNSValidForFacetPipelineRegardlessOfInitialStage) {
    const std::vector<BSONObj> rawPipeline = {fromjson("{$facet: {subPipe: [{$match: {}}]}}")};
    auto ctx = getExpCtx({.hasCollectionName = false, .setMockReplCoord = false});

    ASSERT_THROWS_CODE(
        Pipeline::parse(rawPipeline, ctx), AssertionException, ErrorCodes::InvalidNamespace);
}

TEST_F(PipelineValidateTest, ChangeStreamIsValidAsFirstStage) {
    const std::vector<BSONObj> rawPipeline = {fromjson("{$changeStream: {}}")};
    auto ctx = getExpCtx({.hasCollectionName = true, .setMockReplCoord = true});
    Pipeline::parse(rawPipeline, ctx);
}

TEST_F(PipelineValidateTest, ChangeStreamIsNotValidIfNotFirstStage) {
    const std::vector<BSONObj> rawPipeline = {fromjson("{$match: {custom: 'filter'}}"),
                                              fromjson("{$changeStream: {}}")};
    auto ctx = getExpCtx({.hasCollectionName = true, .setMockReplCoord = true});

    ASSERT_THROWS_CODE(Pipeline::parse(rawPipeline, ctx), AssertionException, 40602);
}


TEST_F(PipelineValidateTest, ChangeStreamIsNotValidIfNotFirstStageInFacet) {
    const std::vector<BSONObj> rawPipeline = {
        fromjson("{$facet: {subPipe: [{$match: {}}, {$changeStream: {}}]}}")};

    auto ctx = getExpCtx({.hasCollectionName = true, .setMockReplCoord = true});

    ASSERT_THROWS_CODE(Pipeline::parse(rawPipeline, ctx), AssertionException, 40600);
}


TEST_F(PipelineValidateTest, ChangeStreamSplitLargeEventIsValid) {
    const std::vector<BSONObj> rawPipeline = {fromjson("{$changeStream: {}}"),
                                              fromjson("{$changeStreamSplitLargeEvent: {}}")};
    auto ctx = getExpCtx({.hasCollectionName = true, .setMockReplCoord = true});
    Pipeline::parse(rawPipeline, ctx);
}

TEST_F(PipelineValidateTest, ChangeStreamSplitLargeEventIsNotValidWithoutChangeStream) {
    const std::vector<BSONObj> rawPipeline = {fromjson("{$changeStreamSplitLargeEvent: {}}")};
    auto ctx = getExpCtx({.hasCollectionName = true, .setMockReplCoord = true});
    ctx->setChangeStreamSpec(boost::none);

    ASSERT_THROWS_CODE(
        Pipeline::parse(rawPipeline, ctx), DBException, ErrorCodes::IllegalOperation);
}

TEST_F(PipelineValidateTest, ChangeStreamSplitLargeEventIsNotLastStage) {
    const std::vector<BSONObj> rawPipeline = {fromjson("{$changeStream: {}}"),
                                              fromjson("{$changeStreamSplitLargeEvent: {}}"),
                                              fromjson("{$match: {}}")};
    auto ctx = getExpCtx({.hasCollectionName = true, .setMockReplCoord = true});

    ASSERT_THROWS_CODE(Pipeline::parse(rawPipeline, ctx), DBException, 7182802);
}

TEST_F(PipelineValidateTest, ChangeStreamSplitLargeEventIsValidAfterMatch) {
    const std::vector<BSONObj> rawPipeline = {fromjson("{$changeStream: {}}"),
                                              fromjson("{$match: {custom: 'filter'}}"),
                                              fromjson("{$changeStreamSplitLargeEvent: {}}")};
    auto ctx = getExpCtx({.hasCollectionName = true, .setMockReplCoord = true});
    Pipeline::parse(rawPipeline, ctx);
}

TEST_F(PipelineValidateTest, ChangeStreamSplitLargeEventIsValidAfterRedact) {
    const std::vector<BSONObj> rawPipeline = {fromjson("{$changeStream: {}}"),
                                              fromjson("{$redact: '$$PRUNE'}"),
                                              fromjson("{$changeStreamSplitLargeEvent: {}}")};
    auto ctx = getExpCtx({.hasCollectionName = true, .setMockReplCoord = true});
    Pipeline::parse(rawPipeline, ctx);
}

using DocumentSourceDisallowedInTransactions = DocumentSourceDisallowedInTransactions;
TEST_F(PipelineValidateTest, TopLevelPipelineValidatedForStagesIllegalInTransactions) {
    auto ctx = AggregationContextFixture::getExpCtx();
    ctx->getOperationContext()->setInMultiDocumentTransaction();

    // Make a pipeline with a legal $match, and then an illegal mock stage, and verify that pipeline
    // creation fails with the expected error code.
    ASSERT_THROWS_CODE(
        makePipeline({matchStage("{_id: 3}"), DocumentSourceDisallowedInTransactions::create(ctx)}),
        AssertionException,
        ErrorCodes::OperationNotSupportedInTransaction);
}

TEST_F(PipelineValidateTest, FacetPipelineValidatedForStagesIllegalInTransactions) {
    auto ctx = AggregationContextFixture::getExpCtx();
    ctx->getOperationContext()->setInMultiDocumentTransaction();

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
    auto pipeline = makePipeline({});

    auto depsTracker = pipeline->getDependencies(DepsTracker::kAllMetadata);
    ASSERT_TRUE(depsTracker.needWholeDocument);
    ASSERT_FALSE(depsTracker.getNeedsMetadata(DocumentMetadataFields::kTextScore));

    depsTracker =
        pipeline->getDependencies(DepsTracker::kAllMetadata & ~DepsTracker::kOnlyTextScore);
    ASSERT_TRUE(depsTracker.needWholeDocument);
}

TEST_F(PipelineDependenciesTest, ShouldRequireWholeDocumentIfAnyStageDoesNotSupportDeps) {
    auto ctx = getExpCtx();
    auto needsASeeNext = DocumentSourceNeedsASeeNext::create(ctx);
    auto notSupported = DocumentSourceDependenciesNotSupported::create(ctx);
    auto pipeline = makePipeline({needsASeeNext, notSupported});

    auto depsTracker = pipeline->getDependencies(DepsTracker::kAllMetadata);
    ASSERT_TRUE(depsTracker.needWholeDocument);
    // The inputs did not have a text score available, so we should not require a text score.
    ASSERT_FALSE(depsTracker.getNeedsMetadata(DocumentMetadataFields::kTextScore));

    // Now in the other order.
    pipeline = makePipeline({notSupported, needsASeeNext});

    depsTracker = pipeline->getDependencies(DepsTracker::kAllMetadata);
    ASSERT_TRUE(depsTracker.needWholeDocument);
}

TEST_F(PipelineDependenciesTest, ShouldRequireWholeDocumentIfNoStageReturnsExhaustiveFields) {
    auto ctx = getExpCtx();
    auto needsASeeNext = DocumentSourceNeedsASeeNext::create(ctx);
    auto pipeline = makePipeline({needsASeeNext});

    auto depsTracker = pipeline->getDependencies(DepsTracker::kNoMetadata);
    ASSERT_TRUE(depsTracker.needWholeDocument);
}

TEST_F(PipelineDependenciesTest, ShouldNotRequireWholeDocumentIfAnyStageReturnsExhaustiveFields) {
    auto ctx = getExpCtx();
    auto needsASeeNext = DocumentSourceNeedsASeeNext::create(ctx);
    auto needsOnlyB = DocumentSourceNeedsOnlyB::create(ctx);
    auto pipeline = makePipeline({needsASeeNext, needsOnlyB});

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
    auto pipeline = makePipeline({needsOnlyB, needsASeeNext});

    auto depsTracker = pipeline->getDependencies(DepsTracker::kAllMetadata);
    ASSERT_FALSE(depsTracker.needWholeDocument);
    ASSERT_FALSE(depsTracker.getNeedsMetadata(DocumentMetadataFields::kTextScore));

    // 'needsOnlyB' claims to know all its field dependencies, so we shouldn't add any from
    // 'needsASeeNext'.
    ASSERT_EQ(depsTracker.fields.size(), 1UL);
    ASSERT_EQ(depsTracker.fields.count("b"), 1UL);
}

TEST_F(PipelineDependenciesTest, ShouldNotRequireTextScoreIfThereIsNoScoreAvailable) {
    auto pipeline = makePipeline({});

    auto depsTracker = pipeline->getDependencies(DepsTracker::kAllMetadata);
    ASSERT_FALSE(depsTracker.getNeedsMetadata(DocumentMetadataFields::kTextScore));
}

TEST_F(PipelineDependenciesTest, ShouldThrowIfTextScoreIsNeededButNotPresent) {
    auto ctx = getExpCtx();
    auto needsText = DocumentSourceNeedsMetaField::create(ctx, DocumentMetadataFields::kTextScore);
    auto pipeline = makePipeline({needsText});

    ASSERT_THROWS(pipeline->getDependencies(DepsTracker::kNoMetadata), AssertionException);
}

TEST_F(PipelineDependenciesTest,
       ShouldRequireTextScoreIfAvailableAndNoStageReturnsExhaustiveMetaAndNeedsMerge) {
    auto ctx = getExpCtx();

    // When needsMerge is true, the consumer might implicitly use textScore, if it's available.
    ctx->setNeedsMerge(true);

    auto pipeline = makePipeline({});
    auto deps = pipeline->getDependencies(DepsTracker::kOnlyTextScore);
    ASSERT_TRUE(deps.getNeedsMetadata(DocumentMetadataFields::kTextScore));

    pipeline = makePipeline({DocumentSourceNeedsASeeNext::create(ctx)});
    deps = pipeline->getDependencies(DepsTracker::kOnlyTextScore);
    ASSERT_TRUE(deps.getNeedsMetadata(DocumentMetadataFields::kTextScore));

    // When needsMerge is false, if no stage explicitly uses textScore then we know it isn't needed.
    ctx->setNeedsMerge(false);

    pipeline = makePipeline({});
    deps = pipeline->getDependencies(DepsTracker::kOnlyTextScore);
    ASSERT_FALSE(deps.getNeedsMetadata(DocumentMetadataFields::kTextScore));

    pipeline = makePipeline({DocumentSourceNeedsASeeNext::create(ctx)});
    deps = pipeline->getDependencies(DepsTracker::kOnlyTextScore);
    ASSERT_FALSE(deps.getNeedsMetadata(DocumentMetadataFields::kTextScore));
}

TEST_F(PipelineDependenciesTest, ShouldNotRequireTextScoreIfAvailableButDefinitelyNotNeeded) {
    auto ctx = getExpCtx();
    auto stripsTextScore = DocumentSourceStripsMetadata::create(ctx);
    auto needsText = DocumentSourceNeedsMetaField::create(ctx, DocumentMetadataFields::kTextScore);
    auto pipeline = makePipeline({stripsTextScore, needsText});

    auto depsTracker = pipeline->getDependencies(DepsTracker::kOnlyTextScore);

    // 'stripsTextScore' claims that no further stage will need metadata information, so we
    // shouldn't have the text score as a dependency.
    ASSERT_FALSE(depsTracker.getNeedsMetadata(DocumentMetadataFields::kTextScore));
}

TEST_F(PipelineDependenciesTest, ValidateMetaDependenciesPassesIfMetaFieldNeededAfterGenerated) {
    auto ctx = getExpCtx();
    auto generatesScore =
        DocumentSourceGeneratesMetaField::create(ctx, DocumentMetadataFields::kScore);
    auto needsScore = DocumentSourceNeedsMetaField::create(ctx, DocumentMetadataFields::kScore);

    auto pipeline = makePipeline({generatesScore, needsScore});

    // The meta dependencies are valid since it generates the score prior to needing the score.
    ASSERT_DOES_NOT_THROW(pipeline->validateMetaDependencies());
}

TEST_F(PipelineDependenciesTest, ValidateMetaDependenciesThrowsIfMetaFieldNeededBeforeGenerated) {
    auto ctx = getExpCtx();
    auto needsScore = DocumentSourceNeedsMetaField::create(ctx, DocumentMetadataFields::kScore);
    auto generatesScore =
        DocumentSourceGeneratesMetaField::create(ctx, DocumentMetadataFields::kScore);

    auto pipeline = makePipeline({needsScore, generatesScore});

    // The meta dependencies are invalid since it requests the score prior to score being generated.
    ASSERT_THROWS_CODE(pipeline->validateMetaDependencies(), AssertionException, 40218);
}

TEST_F(PipelineDependenciesTest,
       ValidateMetaDependenciesThrowsIfMetaFieldNeededAfterMetadataStripped) {
    auto ctx = getExpCtx();

    auto generatesScore =
        DocumentSourceGeneratesMetaField::create(ctx, DocumentMetadataFields::kScore);
    auto stripsMetadata = DocumentSourceStripsMetadata::create(ctx);
    auto needsScore = DocumentSourceNeedsMetaField::create(ctx, DocumentMetadataFields::kScore);

    auto pipeline = makePipeline({generatesScore, stripsMetadata, needsScore});

    // The meta dependencies are invalid since it requests the score after metadata is stripped.
    ASSERT_THROWS_CODE(pipeline->validateMetaDependencies(), AssertionException, 40218);
}

TEST_F(PipelineDependenciesTest,
       ValidateMetaDependenciesPassesIfMetaFieldNeededBeforeMetadataStripped) {
    auto ctx = getExpCtx();

    auto generatesScore =
        DocumentSourceGeneratesMetaField::create(ctx, DocumentMetadataFields::kScore);
    auto needsScore = DocumentSourceNeedsMetaField::create(ctx, DocumentMetadataFields::kScore);
    auto stripsMetadata = DocumentSourceStripsMetadata::create(ctx);

    auto pipeline = makePipeline({generatesScore, needsScore, stripsMetadata});

    // The meta dependencies are valid since it requests the score before metadata is stripped.
    ASSERT_DOES_NOT_THROW(pipeline->validateMetaDependencies());
}

TEST_F(PipelineDependenciesTest,
       ValidateMetaDependenciesPassesIfMetaFieldGeneratedAfterMetadataStripped) {
    auto ctx = getExpCtx();

    auto stripsMetadata = DocumentSourceStripsMetadata::create(ctx);
    auto generatesScore =
        DocumentSourceGeneratesMetaField::create(ctx, DocumentMetadataFields::kScore);
    auto needsScore = DocumentSourceNeedsMetaField::create(ctx, DocumentMetadataFields::kScore);

    auto pipeline = makePipeline({stripsMetadata, generatesScore, needsScore});

    // The meta dependencies are valid since it generates the score after metadata is stripped.
    ASSERT_DOES_NOT_THROW(pipeline->validateMetaDependencies());
}

TEST_F(PipelineDependenciesTest, ValidateMetaDependenciesThrowsMetadataStrippedMultipleTimes) {
    auto ctx = getExpCtx();

    auto stripsMetadata1 = DocumentSourceStripsMetadata::create(ctx);
    auto generatesScore =
        DocumentSourceGeneratesMetaField::create(ctx, DocumentMetadataFields::kScore);
    auto stripsMetadata2 = DocumentSourceStripsMetadata::create(ctx);
    auto needsScore = DocumentSourceNeedsMetaField::create(ctx, DocumentMetadataFields::kScore);
    auto stripsMetadata3 = DocumentSourceStripsMetadata::create(ctx);

    auto pipeline = makePipeline(
        {stripsMetadata1, generatesScore, stripsMetadata2, needsScore, stripsMetadata3});

    // The meta dependencies are invalid since it requests the score after metadata is stripped.
    ASSERT_THROWS_CODE(pipeline->validateMetaDependencies(), AssertionException, 40218);
}

TEST_F(PipelineDependenciesTest, ValidateMetaDependenciesPassesMetadataStrippedMultipleTimes) {
    auto ctx = getExpCtx();

    auto stripsMetadata1 = DocumentSourceStripsMetadata::create(ctx);
    auto generatesScore =
        DocumentSourceGeneratesMetaField::create(ctx, DocumentMetadataFields::kScore);
    auto needsScore = DocumentSourceNeedsMetaField::create(ctx, DocumentMetadataFields::kScore);
    auto stripsMetadata2 = DocumentSourceStripsMetadata::create(ctx);

    auto pipeline = makePipeline({stripsMetadata1, generatesScore, needsScore, stripsMetadata2});

    // The meta dependencies are valid since it neither stages that strip metadata interfere between
    // when the score and generated and needed.
    ASSERT_DOES_NOT_THROW(pipeline->validateMetaDependencies());
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
    auto pipeline = makePipeline({needsAProducesBC, needsCDProducesE, needsBE});

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
    auto pipeline = makePipeline({producer, consumer});

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
    auto pipeline = makePipeline({producer, consumer});

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
    auto pipeline = makePipeline({mockSource()});
    {
        // Tracking renames backwards.
        trackPipelineRenames(makePipeline({mockSource()}),
                             {"a", "b", "c.d"} /* pathsOfInterest */,
                             Tracking::backwards);
    }
    {
        // Tracking renames forwards.
        trackPipelineRenames(makePipeline({mockSource()}),
                             {"a", "b", "c.d"} /* pathsOfInterest */,
                             Tracking::forwards);
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
        trackPipelineRenames(makePipeline({mockSource(), NoModifications::create(expCtx)}),
                             {"a", "b", "c.d"} /* pathsOfInterest */,
                             Tracking::backwards);
    }
    {
        // Tracking renames forwards.
        trackPipelineRenames(makePipeline({mockSource(), NoModifications::create(expCtx)}),
                             {"a", "b", "c.d"} /* pathsOfInterest */,
                             Tracking::forwards);
    }
    {
        // Tracking renames backwards.
        trackPipelineRenames(makePipeline({mockSource(),
                                           NoModifications::create(expCtx),
                                           NoModifications::create(expCtx),
                                           NoModifications::create(expCtx)}),
                             {"a", "b", "c.d"} /* pathsOfInterest */,
                             Tracking::backwards);
    }
    {
        // Tracking renames forwards.
        trackPipelineRenames(makePipeline({mockSource(),
                                           NoModifications::create(expCtx),
                                           NoModifications::create(expCtx),
                                           NoModifications::create(expCtx)}),
                             {"a", "b", "c.d"} /* pathsOfInterest */,
                             Tracking::forwards);
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
    auto pipeline = makePipeline({mockSource(),
                                  NoModifications::create(expCtx),
                                  NotSupported::create(expCtx),
                                  NoModifications::create(expCtx)});

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
    auto pipeline = makePipeline({mockSource(), RenamesAToB::create(expCtx)});
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
        // The mock stage reports that it duplicates "a" field into "b".
        // This resembles: {"$addFields":{"b":"$a"}}
        // The resulting document can have both "b" and "a", both with the original value of "a". As
        // we are traversing backwards, both "a" and "b" should map to "a".
        auto renames = semantic_analysis::renamedPaths(
            pipeline->getSources().crbegin(), pipeline->getSources().crend(), {"b", "a"});
        ASSERT(static_cast<bool>(renames));
        auto nameMap = *renames;
        ASSERT_EQ(nameMap.size(), 2UL);
        ASSERT_EQ(nameMap["b"], "a");
        ASSERT_EQ(nameMap["a"], "a");
    }
    {
        // In the forwards direction, "b" does not survive this stage; it is overwritten by the
        // value of "a". This functionally modifies "b", so as documented renamedPaths should return
        // boost::none.
        auto renames = semantic_analysis::renamedPaths(
            pipeline->getSources().cbegin(), pipeline->getSources().cend(), {"b", "a"});
        ASSERT_FALSE(static_cast<bool>(renames));
    }

    {
        // As above, any previous value of "b" does not survive past this stage.
        auto renames = semantic_analysis::renamedPaths(
            pipeline->getSources().cbegin(), pipeline->getSources().cend(), {"b"});
        ASSERT_FALSE(static_cast<bool>(renames));
    }

    {
        // In the forwards direction, following only "a", it has only been renamed, so it _has_ been
        // preserved.
        auto renames = semantic_analysis::renamedPaths(
            pipeline->getSources().cbegin(), pipeline->getSources().cend(), {"a"});
        ASSERT(static_cast<bool>(renames));
        auto nameMap = *renames;
        ASSERT_EQ(nameMap.size(), 1UL);
        ASSERT_EQ(nameMap["a"], "b");
    }
}

TEST_F(PipelineRenameTracking, ReportsIdentityMapWhenGivenEmptyIteratorRange) {
    auto expCtx = getExpCtx();
    {
        // Tracking backwards.
        trackPipelineRenamesOnEmptyRange(makePipeline({mockSource(), RenamesAToB::create(expCtx)}),
                                         {"b"} /* pathsOfInterest */,
                                         Tracking::backwards);
    }
    {
        // Tracking forwards.
        trackPipelineRenamesOnEmptyRange(makePipeline({mockSource(), RenamesAToB::create(expCtx)}),
                                         {"b"} /* pathsOfInterest */,
                                         Tracking::forwards);
    }
    {
        // Tracking backwards.
        trackPipelineRenamesOnEmptyRange(makePipeline({mockSource(), RenamesAToB::create(expCtx)}),
                                         {"b", "c.d"} /* pathsOfInterest */,
                                         Tracking::backwards);
    }
    {
        // Tracking forwards.
        trackPipelineRenamesOnEmptyRange(makePipeline({mockSource(), RenamesAToB::create(expCtx)}),
                                         {"b", "c.d"} /* pathsOfInterest */,
                                         Tracking::forwards);
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
        auto pipeline =
            makePipeline({mockSource(), RenamesAToB::create(expCtx), RenamesBToC::create(expCtx)});
        const auto& stages = pipeline->getSources();
        auto renames = semantic_analysis::renamedPaths(stages.crbegin(), stages.crend(), {"c"});
        ASSERT(static_cast<bool>(renames));
        auto nameMap = *renames;
        ASSERT_EQ(nameMap.size(), 1UL);
        ASSERT_EQ(nameMap["c"], "a");
    }
    {
        // Tracking forwards.
        auto pipeline =
            makePipeline({mockSource(), RenamesAToB::create(expCtx), RenamesBToC::create(expCtx)});
        const auto& stages = pipeline->getSources();
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
        trackPipelineRenames(
            makePipeline({mockSource(), RenamesAToB::create(expCtx), RenamesBToA::create(expCtx)}),
            {"a"} /* pathsOfInterest */,
            Tracking::backwards);
    }
    {
        // Tracking forwards.
        trackPipelineRenames(
            makePipeline({mockSource(), RenamesAToB::create(expCtx), RenamesBToA::create(expCtx)}),
            {"a"} /* pathsOfInterest */,
            Tracking::forwards);
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
    auto pipeline = makePipeline(
        {mockSource(),
         matchStage("{x: 1}"),
         sortStage("{y: -1}"),
         DocumentSourceProject::create(BSON("x" << 1 << "y" << 1), expCtx, "$project"_sd)});
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
    auto pipeline = makePipeline({mockSource(), lookupStage(lookupSpec)});

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
    auto pipeline = makePipeline({mockDeferredSortStage(), graphLookupStage(graphLookupSpec)});

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
    auto pipeline = makePipeline({mockSource(), lookupStage(lookupSpec)});

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
    auto pipeline = makePipeline({mockSource(), graphLookupStage(graphLookupSpec)});

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
    auto pipeline = makePipeline(
        {mockSource(), DocumentSourceFacet::createFromBson(facetSpec.firstElement(), expCtx)});

    auto involvedNssSet = pipeline->getInvolvedCollections();
    ASSERT_EQ(involvedNssSet.size(), 3UL);
    ASSERT(involvedNssSet.find(resolvedNss) != involvedNssSet.end());
    ASSERT(involvedNssSet.find(nssIncludedInResolvedView) != involvedNssSet.end());
    ASSERT(involvedNssSet.find(normalCollectionNss) != involvedNssSet.end());
};

}  // namespace
}  // namespace mongo
