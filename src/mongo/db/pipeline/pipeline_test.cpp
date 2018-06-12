/**
 *    Copyright (C) 2012 10gen Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#include "mongo/platform/basic.h"

#include <boost/intrusive_ptr.hpp>
#include <string>
#include <vector>

#include "mongo/db/operation_context_noop.h"
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/dependencies.h"
#include "mongo/db/pipeline/document.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_change_stream.h"
#include "mongo/db/pipeline/document_source_internal_split_pipeline.h"
#include "mongo/db/pipeline/document_source_lookup_change_post_image.h"
#include "mongo/db/pipeline/document_source_match.h"
#include "mongo/db/pipeline/document_source_mock.h"
#include "mongo/db/pipeline/document_source_out.h"
#include "mongo/db/pipeline/document_source_project.h"
#include "mongo/db/pipeline/document_source_sort.h"
#include "mongo/db/pipeline/document_value_test_util.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/pipeline/field_path.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/query/collation/collator_interface_mock.h"
#include "mongo/db/query/query_test_service_context.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/dbtests/dbtests.h"
#include "mongo/unittest/death_test.h"

namespace mongo {
namespace {

using boost::intrusive_ptr;
using std::string;
using std::vector;

const NamespaceString kTestNss = NamespaceString("a.collection");

void setMockReplicationCoordinatorOnOpCtx(OperationContext* opCtx) {
    repl::ReplicationCoordinator::set(
        opCtx->getServiceContext(),
        stdx::make_unique<repl::ReplicationCoordinatorMock>(opCtx->getServiceContext()));
}

namespace Optimizations {
namespace Local {

BSONObj pipelineFromJsonArray(const std::string& jsonArray) {
    return fromjson("{pipeline: " + jsonArray + "}");
}

void assertPipelineOptimizesAndSerializesTo(std::string inputPipeJson,
                                            std::string outputPipeJson,
                                            std::string serializedPipeJson) {
    QueryTestServiceContext testServiceContext;
    auto opCtx = testServiceContext.makeOperationContext();

    const BSONObj inputBson = pipelineFromJsonArray(inputPipeJson);
    const BSONObj outputPipeExpected = pipelineFromJsonArray(outputPipeJson);
    const BSONObj serializePipeExpected = pipelineFromJsonArray(serializedPipeJson);

    ASSERT_EQUALS(inputBson["pipeline"].type(), BSONType::Array);
    vector<BSONObj> rawPipeline;
    for (auto&& stageElem : inputBson["pipeline"].Array()) {
        ASSERT_EQUALS(stageElem.type(), BSONType::Object);
        rawPipeline.push_back(stageElem.embeddedObject());
    }
    AggregationRequest request(kTestNss, rawPipeline);
    intrusive_ptr<ExpressionContextForTest> ctx =
        new ExpressionContextForTest(opCtx.get(), request);

    // For $graphLookup and $lookup, we have to populate the resolvedNamespaces so that the
    // operations will be able to have a resolved view definition.
    NamespaceString lookupCollNs("a", "lookupColl");
    ctx->setResolvedNamespace(lookupCollNs, {lookupCollNs, std::vector<BSONObj>{}});

    auto outputPipe = uassertStatusOK(Pipeline::parse(request.getPipeline(), ctx));
    outputPipe->optimizePipeline();

    ASSERT_VALUE_EQ(Value(outputPipe->writeExplainOps(ExplainOptions::Verbosity::kQueryPlanner)),
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

TEST(PipelineOptimizationTest, MoveMatchBeforeAddFieldsIfInvolvedFieldsNotRelated) {
    string inputPipe = "[{$addFields : {a : 1}}, {$match : {b : 1}}]";

    string outputPipe = "[{$match : {b : {$eq : 1}}}, {$addFields : {a : {$const : 1}}}]";

    string serializedPipe = "[{$match: {b : 1}}, {$addFields: {a : {$const : 1}}}]";

    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe, serializedPipe);
}

TEST(PipelineOptimizationTest, MatchDoesNotMoveBeforeAddFieldsIfInvolvedFieldsAreRelated) {
    string inputPipe = "[{$addFields : {a : 1}}, {$match : {a : 1}}]";

    string outputPipe = "[{$addFields : {a : {$const : 1}}}, {$match : {a : {$eq : 1}}}]";

    string serializedPipe = "[{$addFields : {a : {$const : 1}}}, {$match: {a : 1}}]";

    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe, serializedPipe);
}

TEST(PipelineOptimizationTest, MatchOnTopLevelFieldDoesNotMoveBeforeAddFieldsOfNestedPath) {
    string inputPipe = "[{$addFields : {'a.b' : 1}}, {$match : {a : 1}}]";

    string outputPipe = "[{$addFields : {a : {b : {$const : 1}}}}, {$match : {a : {$eq : 1}}}]";

    string serializedPipe = "[{$addFields: {a: {b: {$const: 1}}}}, {$match: {a: 1}}]";

    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe, serializedPipe);
}

TEST(PipelineOptimizationTest, MatchOnNestedFieldDoesNotMoveBeforeAddFieldsOfPrefixOfPath) {
    string inputPipe = "[{$addFields : {a : 1}}, {$match : {'a.b' : 1}}]";

    string outputPipe = "[{$addFields : {a : {$const : 1}}}, {$match : {'a.b' : {$eq : 1}}}]";

    string serializedPipe = "[{$addFields : {a : {$const : 1}}}, {$match : {'a.b' : 1}}]";

    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe, serializedPipe);
}

TEST(PipelineOptimizationTest, MoveMatchOnNestedFieldBeforeAddFieldsOfDifferentNestedField) {
    string inputPipe = "[{$addFields : {'a.b' : 1}}, {$match : {'a.c' : 1}}]";

    string outputPipe = "[{$match : {'a.c' : {$eq : 1}}}, {$addFields : {a : {b : {$const : 1}}}}]";

    string serializedPipe = "[{$match : {'a.c' : 1}}, {$addFields : {a : {b: {$const : 1}}}}]";

    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe, serializedPipe);
}

TEST(PipelineOptimizationTest, MoveMatchBeforeAddFieldsWhenMatchedFieldIsPrefixOfAddedFieldName) {
    string inputPipe = "[{$addFields : {abcd : 1}}, {$match : {abc : 1}}]";

    string outputPipe = "[{$match : {abc : {$eq : 1}}}, {$addFields : {abcd: {$const: 1}}}]";

    string serializedPipe = "[{$match : {abc : 1}}, {$addFields : {abcd : {$const : 1}}}]";

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
    string inputPipe = "[{$match: {}}, {$match: {}}]";

    string outputPipe = "[{$match: {}}]";

    string serializedPipe = "[{$match: {$and: [{}, {}]}}]";

    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe, serializedPipe);
}

TEST(PipelineOptimizationTest, DoNotRemoveNonEmptyMatch) {
    string inputPipe = "[{$match: {_id: 1}}]";

    string outputPipe = "[{$match: {_id: {$eq : 1}}}]";

    string serializedPipe = "[{$match: {_id: 1}}]";

    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe, serializedPipe);
}

TEST(PipelineOptimizationTest, MoveMatchBeforeSort) {
    std::string inputPipe = "[{$sort: {b: 1}}, {$match: {a: 2}}]";
    std::string outputPipe = "[{$match: {a: {$eq : 2}}}, {$sort: {sortKey: {b: 1}}}]";
    std::string serializedPipe = "[{$match: {a: 2}}, {$sort: {b: 1}}]";
    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe, serializedPipe);
}

TEST(PipelineOptimizationTest, LookupShouldCoalesceWithUnwindOnAs) {
    string inputPipe =
        "[{$lookup: {from : 'lookupColl', as : 'same', localField: 'left', foreignField: "
        "'right'}}"
        ",{$unwind: {path: '$same'}}"
        "]";
    string outputPipe =
        "[{$lookup: {from : 'lookupColl', as : 'same', localField: 'left', foreignField: "
        "'right', unwinding: {preserveNullAndEmptyArrays: false}}}]";
    string serializedPipe =
        "[{$lookup: {from : 'lookupColl', as : 'same', localField: 'left', foreignField: "
        "'right'}}"
        ",{$unwind: {path: '$same'}}"
        "]";
    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe, serializedPipe);
}

TEST(PipelineOptimizationTest, LookupWithPipelineSyntaxShouldCoalesceWithUnwindOnAs) {
    string inputPipe =
        "[{$lookup: {from : 'lookupColl', as : 'same', let: {}, pipeline: []}}"
        ",{$unwind: {path: '$same'}}"
        "]";
    string outputPipe =
        "[{$lookup: {from : 'lookupColl', as : 'same', let: {}, pipeline: [], "
        "unwinding: {preserveNullAndEmptyArrays: false}}}]";
    string serializedPipe =
        "[{$lookup: {from : 'lookupColl', as : 'same', let: {}, pipeline: []}}"
        ",{$unwind: {path: '$same'}}"
        "]";
    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe, serializedPipe);
}

TEST(PipelineOptimizationTest, LookupShouldCoalesceWithUnwindOnAsWithPreserveEmpty) {
    string inputPipe =
        "[{$lookup: {from : 'lookupColl', as : 'same', localField: 'left', foreignField: "
        "'right'}}"
        ",{$unwind: {path: '$same', preserveNullAndEmptyArrays: true}}"
        "]";
    string outputPipe =
        "[{$lookup: {from : 'lookupColl', as : 'same', localField: 'left', foreignField: "
        "'right', unwinding: {preserveNullAndEmptyArrays: true}}}]";
    string serializedPipe =
        "[{$lookup: {from : 'lookupColl', as : 'same', localField: 'left', foreignField: "
        "'right'}}"
        ",{$unwind: {path: '$same', preserveNullAndEmptyArrays: true}}"
        "]";
    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe, serializedPipe);
}

TEST(PipelineOptimizationTest, LookupShouldCoalesceWithUnwindOnAsWithIncludeArrayIndex) {
    string inputPipe =
        "[{$lookup: {from : 'lookupColl', as : 'same', localField: 'left', foreignField: "
        "'right'}}"
        ",{$unwind: {path: '$same', includeArrayIndex: 'index'}}"
        "]";
    string outputPipe =
        "[{$lookup: {from : 'lookupColl', as : 'same', localField: 'left', foreignField: "
        "'right', unwinding: {preserveNullAndEmptyArrays: false, includeArrayIndex: "
        "'index'}}}]";
    string serializedPipe =
        "[{$lookup: {from : 'lookupColl', as : 'same', localField: 'left', foreignField: "
        "'right'}}"
        ",{$unwind: {path: '$same', includeArrayIndex: 'index'}}"
        "]";
    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe, serializedPipe);
}

TEST(PipelineOptimizationTest, LookupShouldNotCoalesceWithUnwindNotOnAs) {
    string inputPipe =
        "[{$lookup: {from : 'lookupColl', as : 'same', localField: 'left', foreignField: "
        "'right'}}"
        ",{$unwind: {path: '$from'}}"
        "]";
    string outputPipe =
        "[{$lookup: {from : 'lookupColl', as : 'same', localField: 'left', foreignField: "
        "'right'}}"
        ",{$unwind: {path: '$from'}}"
        "]";
    assertPipelineOptimizesTo(inputPipe, outputPipe);
}

TEST(PipelineOptimizationTest, LookupWithPipelineSyntaxShouldNotCoalesceWithUnwindNotOnAs) {
    string inputPipe =
        "[{$lookup: {from : 'lookupColl', as : 'same', pipeline: []}}"
        ",{$unwind: {path: '$from'}}"
        "]";
    string outputPipe =
        "[{$lookup: {from : 'lookupColl', as : 'same', let: {}, pipeline: []}}"
        ",{$unwind: {path: '$from'}}"
        "]";
    assertPipelineOptimizesTo(inputPipe, outputPipe);
}

TEST(PipelineOptimizationTest, LookupShouldSwapWithMatch) {
    string inputPipe =
        "[{$lookup: {from: 'lookupColl', as: 'asField', localField: 'y', foreignField: "
        "'z'}}, "
        " {$match: {'independent': 0}}]";
    string outputPipe =
        "[{$match: {independent: {$eq : 0}}}, "
        " {$lookup: {from: 'lookupColl', as: 'asField', localField: 'y', foreignField: "
        "'z'}}]";
    string serializedPipe =
        "[{$match: {independent: 0}}, "
        "{$lookup: {from: 'lookupColl', as: 'asField', localField: 'y', foreignField: 'z'}}]";

    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe, serializedPipe);
}

TEST(PipelineOptimizationTest, LookupWithPipelineSyntaxShouldSwapWithMatch) {
    string inputPipe =
        "[{$lookup: {from: 'lookupColl', as: 'asField', pipeline: []}}, "
        " {$match: {'independent': 0}}]";
    string outputPipe =
        "[{$match: {independent: {$eq : 0}}}, "
        " {$lookup: {from: 'lookupColl', as: 'asField', let: {}, pipeline: []}}]";
    string serializedPipe =
        "[{$match: {independent: 0}}, "
        "{$lookup: {from: 'lookupColl', as: 'asField', let: {}, pipeline: []}}]";

    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe, serializedPipe);
}

TEST(PipelineOptimizationTest, LookupShouldSplitMatch) {
    string inputPipe =
        "[{$lookup: {from: 'lookupColl', as: 'asField', localField: 'y', foreignField: "
        "'z'}}, "
        " {$match: {'independent': 0, asField: {$eq: 3}}}]";
    string outputPipe =
        "[{$match: {independent: {$eq: 0}}}, "
        " {$lookup: {from: 'lookupColl', as: 'asField', localField: 'y', foreignField: "
        "'z'}}, "
        " {$match: {asField: {$eq: 3}}}]";
    assertPipelineOptimizesTo(inputPipe, outputPipe);
}

TEST(PipelineOptimizationTest, LookupShouldNotAbsorbMatchOnAs) {
    string inputPipe =
        "[{$lookup: {from: 'lookupColl', as: 'asField', localField: 'y', foreignField: "
        "'z'}}, "
        " {$match: {'asField.subfield': 0}}]";
    string outputPipe =
        "[{$lookup: {from: 'lookupColl', as: 'asField', localField: 'y', foreignField: "
        "'z'}}, "
        " {$match: {'asField.subfield': {$eq : 0}}}]";
    string serializedPipe =
        "[{$lookup: {from: 'lookupColl', as: 'asField', localField: 'y', foreignField: "
        "'z'}}, "
        " {$match: {'asField.subfield': 0}}]";

    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe, serializedPipe);
}

TEST(PipelineOptimizationTest, LookupShouldAbsorbUnwindMatch) {
    string inputPipe =
        "[{$lookup: {from: 'lookupColl', as: 'asField', localField: 'y', foreignField: "
        "'z'}}, "
        "{$unwind: '$asField'}, "
        "{$match: {'asField.subfield': {$eq: 1}}}]";
    string outputPipe =
        "[{$lookup: {from: 'lookupColl', as: 'asField', localField: 'y', foreignField: 'z', "
        "            unwinding: {preserveNullAndEmptyArrays: false}, "
        "            matching: {subfield: {$eq: 1}}}}]";
    string serializedPipe =
        "[{$lookup: {from: 'lookupColl', as: 'asField', localField: 'y', foreignField: "
        "'z'}}, "
        "{$unwind: {path: '$asField'}}, "
        "{$match: {'asField.subfield': {$eq: 1}}}]";
    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe, serializedPipe);
}

TEST(PipelineOptimizationTest, LookupWithPipelineSyntaxShouldAbsorbUnwindMatch) {
    string inputPipe =
        "[{$lookup: {from: 'lookupColl', as: 'asField', pipeline: []}}, "
        "{$unwind: '$asField'}, "
        "{$match: {'asField.subfield': {$eq: 1}}}]";
    string outputPipe =
        "[{$lookup: {from: 'lookupColl', as: 'asField', let: {}, "
        "pipeline: [{$match: {subfield: {$eq: 1}}}], "
        "unwinding: {preserveNullAndEmptyArrays: false} } } ]";
    string serializedPipe =
        "[{$lookup: {from: 'lookupColl', as: 'asField', let: {}, "
        "pipeline: [{$match: {subfield: {$eq: 1}}}]}}, "
        "{$unwind: {path: '$asField'}}]";
    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe, serializedPipe);
}

TEST(PipelineOptimizationTest, LookupShouldAbsorbUnwindAndSplitAndAbsorbMatch) {
    string inputPipe =
        "[{$lookup: {from: 'lookupColl', as: 'asField', localField: 'y', foreignField: "
        "'z'}}, "
        " {$unwind: '$asField'}, "
        " {$match: {'asField.subfield': {$eq: 1}, independentField: {$gt: 2}}}]";
    string outputPipe =
        "[{$match: {independentField: {$gt: 2}}}, "
        " {$lookup: { "
        "      from: 'lookupColl', "
        "      as: 'asField', "
        "      localField: 'y', "
        "      foreignField: 'z', "
        "      unwinding: { "
        "          preserveNullAndEmptyArrays: false"
        "      }, "
        "      matching: { "
        "          subfield: {$eq: 1} "
        "      } "
        " }}]";
    string serializedPipe =
        "[{$match: {independentField: {$gt: 2}}}, "
        " {$lookup: {from: 'lookupColl', as: 'asField', localField: 'y', foreignField: "
        "'z'}}, "
        " {$unwind: {path: '$asField'}}, "
        " {$match: {'asField.subfield': {$eq: 1}}}]";
    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe, serializedPipe);
}

TEST(PipelineOptimizationTest, LookupShouldNotSplitIndependentAndDependentOrClauses) {
    // If any child of the $or is dependent on the 'asField', then the $match cannot be moved above
    // the $lookup, and if any child of the $or is independent of the 'asField', then the $match
    // cannot be absorbed by the $lookup.
    string inputPipe =
        "[{$lookup: {from: 'lookupColl', as: 'asField', localField: 'y', foreignField: "
        "'z'}}, "
        " {$unwind: '$asField'}, "
        " {$match: {$or: [{'independent': {$gt: 4}}, "
        "                 {'asField.dependent': {$elemMatch: {a: {$eq: 1}}}}]}}]";
    string outputPipe =
        "[{$lookup: {from: 'lookupColl', as: 'asField', localField: 'y', foreignField: 'z', "
        "            unwinding: {preserveNullAndEmptyArrays: false}}}, "
        " {$match: {$or: [{'independent': {$gt: 4}}, "
        "                 {'asField.dependent': {$elemMatch: {a: {$eq: 1}}}}]}}]";
    string serializedPipe =
        "[{$lookup: {from: 'lookupColl', as: 'asField', localField: 'y', foreignField: "
        "'z'}}, "
        " {$unwind: {path: '$asField'}}, "
        " {$match: {$or: [{'independent': {$gt: 4}}, "
        "                 {'asField.dependent': {$elemMatch: {a: {$eq: 1}}}}]}}]";
    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe, serializedPipe);
}

TEST(PipelineOptimizationTest, LookupWithMatchOnArrayIndexFieldShouldNotCoalesce) {
    string inputPipe =
        "[{$lookup: {from: 'lookupColl', as: 'asField', localField: 'y', foreignField: "
        "'z'}}, "
        " {$unwind: {path: '$asField', includeArrayIndex: 'index'}}, "
        " {$match: {index: 0, 'asField.value': {$gt: 0}, independent: 1}}]";
    string outputPipe =
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
    string serializedPipe =
        "[{$match: {independent: {$eq: 1}}}, "
        " {$lookup: {from: 'lookupColl', as: 'asField', localField: 'y', foreignField: "
        "'z'}}, "
        " {$unwind: {path: '$asField', includeArrayIndex: 'index'}}, "
        " {$match: {$and: [{index: {$eq: 0}}, {'asField.value': {$gt: 0}}]}}]";
    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe, serializedPipe);
}

TEST(PipelineOptimizationTest, LookupWithUnwindPreservingNullAndEmptyArraysShouldNotCoalesce) {
    string inputPipe =
        "[{$lookup: {from: 'lookupColl', as: 'asField', localField: 'y', foreignField: "
        "'z'}}, "
        " {$unwind: {path: '$asField', preserveNullAndEmptyArrays: true}}, "
        " {$match: {'asField.value': {$gt: 0}, independent: 1}}]";
    string outputPipe =
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
    string serializedPipe =
        "[{$match: {independent: {$eq: 1}}}, "
        " {$lookup: {from: 'lookupColl', as: 'asField', localField: 'y', foreignField: "
        "'z'}}, "
        " {$unwind: {path: '$asField', preserveNullAndEmptyArrays: true}}, "
        " {$match: {'asField.value': {$gt: 0}}}]";
    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe, serializedPipe);
}

TEST(PipelineOptimizationTest, LookupDoesNotAbsorbElemMatch) {
    string inputPipe =
        "[{$lookup: {from: 'lookupColl', as: 'x', localField: 'y', foreignField: 'z'}}, "
        " {$unwind: '$x'}, "
        " {$match: {x: {$elemMatch: {a: 1}}}}]";
    string outputPipe =
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
    string serializedPipe =
        "[{$lookup: {from: 'lookupColl', as: 'x', localField: 'y', foreignField: 'z'}}, "
        " {$unwind: {path: '$x'}}, "
        " {$match: {x: {$elemMatch: {a: 1}}}}]";
    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe, serializedPipe);
}

TEST(PipelineOptimizationTest, LookupDoesSwapWithMatchOnLocalField) {
    string inputPipe =
        "[{$lookup: {from: 'lookupColl', as: 'x', localField: 'y', foreignField: 'z'}}, "
        " {$match: {y: {$eq: 3}}}]";
    string outputPipe =
        "[{$match: {y: {$eq: 3}}}, "
        " {$lookup: {from: 'lookupColl', as: 'x', localField: 'y', foreignField: 'z'}}]";
    assertPipelineOptimizesTo(inputPipe, outputPipe);
}

TEST(PipelineOptimizationTest, LookupDoesSwapWithMatchOnFieldWithSameNameAsForeignField) {
    string inputPipe =
        "[{$lookup: {from: 'lookupColl', as: 'x', localField: 'y', foreignField: 'z'}}, "
        " {$match: {z: {$eq: 3}}}]";
    string outputPipe =
        "[{$match: {z: {$eq: 3}}}, "
        " {$lookup: {from: 'lookupColl', as: 'x', localField: 'y', foreignField: 'z'}}]";
    assertPipelineOptimizesTo(inputPipe, outputPipe);
}

TEST(PipelineOptimizationTest, LookupDoesNotAbsorbUnwindOnSubfieldOfAsButStillMovesMatch) {
    string inputPipe =
        "[{$lookup: {from: 'lookupColl', as: 'x', localField: 'y', foreignField: 'z'}}, "
        " {$unwind: {path: '$x.subfield'}}, "
        " {$match: {'independent': 2, 'x.dependent': 2}}]";
    string outputPipe =
        "[{$match: {'independent': {$eq: 2}}}, "
        " {$lookup: {from: 'lookupColl', as: 'x', localField: 'y', foreignField: 'z'}}, "
        " {$match: {'x.dependent': {$eq: 2}}}, "
        " {$unwind: {path: '$x.subfield'}}]";
    assertPipelineOptimizesTo(inputPipe, outputPipe);
}

TEST(PipelineOptimizationTest, MatchShouldDuplicateItselfBeforeRedact) {
    string inputPipe = "[{$redact: '$$PRUNE'}, {$match: {a: 1, b:12}}]";
    string outputPipe =
        "[{$match: {$and: [{a: {$eq: 1}}, {b: {$eq: 12}}]}}, {$redact: '$$PRUNE'}, "
        "{$match: {$and: [{a: {$eq: 1}}, {b: {$eq: 12}}]}}]";
    string serializedPipe =
        "[{$match: {a: 1, b: 12}}, {$redact: '$$PRUNE'}, {$match: {a: 1, b: 12}}]";
    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe, serializedPipe);
}

TEST(PipelineOptimizationTest, MatchShouldSwapWithUnwind) {
    string inputPipe =
        "[{$unwind: '$a.b.c'}, "
        "{$match: {'b': 1}}]";
    string outputPipe =
        "[{$match: {'b': {$eq : 1}}}, "
        "{$unwind: {path: '$a.b.c'}}]";
    string serializedPipe = "[{$match: {b: 1}}, {$unwind: {path: '$a.b.c'}}]";
    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe, serializedPipe);
}

TEST(PipelineOptimizationTest, MatchOnPrefixShouldNotSwapOnUnwind) {
    string inputPipe =
        "[{$unwind: {path: '$a.b.c'}}, "
        "{$match: {'a.b': 1}}]";
    string outputPipe =
        "[{$unwind: {path: '$a.b.c'}}, "
        "{$match: {'a.b': {$eq : 1}}}]";
    string serializedPipe = "[{$unwind: {path: '$a.b.c'}}, {$match: {'a.b': 1}}]";
    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe, serializedPipe);
}

TEST(PipelineOptimizationTest, MatchShouldSplitOnUnwind) {
    string inputPipe =
        "[{$unwind: '$a.b'}, "
        "{$match: {$and: [{f: {$eq: 5}}, "
        "                 {$nor: [{'a.d': 1, c: 5}, {'a.b': 3, c: 5}]}]}}]";
    string outputPipe =
        "[{$match: {$and: [{f: {$eq: 5}},"
        "                  {$nor: [{$and: [{'a.d': {$eq: 1}}, {c: {$eq: 5}}]}]}]}},"
        "{$unwind: {path: '$a.b'}}, "
        "{$match: {$nor: [{$and: [{'a.b': {$eq: 3}}, {c: {$eq: 5}}]}]}}]";
    assertPipelineOptimizesTo(inputPipe, outputPipe);
}

TEST(PipelineOptimizationTest, MatchShouldNotOptimizeWithElemMatch) {
    string inputPipe =
        "[{$unwind: {path: '$a.b'}}, "
        "{$match: {a: {$elemMatch: {b: {d: 1}}}}}]";
    string outputPipe =
        "[{$unwind: {path: '$a.b'}}, "
        "{$match: {a: {$elemMatch: {b: {$eq : {d: 1}}}}}}]";
    string serializedPipe =
        "[{$unwind : {path : '$a.b'}}, {$match : {a : {$elemMatch : {b : {d : 1}}}}}]";
    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe, serializedPipe);
}

TEST(PipelineOptimizationTest, MatchShouldNotOptimizeWhenMatchingOnIndexField) {
    string inputPipe =
        "[{$unwind: {path: '$a', includeArrayIndex: 'foo'}}, "
        " {$match: {foo: 0, b: 1}}]";
    string outputPipe =
        "[{$match: {b: {$eq: 1}}}, "
        " {$unwind: {path: '$a', includeArrayIndex: 'foo'}}, "
        " {$match: {foo: {$eq: 0}}}]";
    assertPipelineOptimizesTo(inputPipe, outputPipe);
}

TEST(PipelineOptimizationTest, MatchWithNorOnlySplitsIndependentChildren) {
    string inputPipe =
        "[{$unwind: {path: '$a'}}, "
        "{$match: {$nor: [{$and: [{a: {$eq: 1}}, {b: {$eq: 1}}]}, {b: {$eq: 2}} ]}}]";
    string outputPipe =
        "[{$match: {$nor: [{b: {$eq: 2}}]}}, "
        "{$unwind: {path: '$a'}}, "
        "{$match: {$nor: [{$and: [{a: {$eq: 1}}, {b: {$eq: 1}}]}]}}]";
    assertPipelineOptimizesTo(inputPipe, outputPipe);
}

TEST(PipelineOptimizationTest, MatchWithOrDoesNotSplit) {
    string inputPipe =
        "[{$unwind: {path: '$a'}}, "
        "{$match: {$or: [{a: {$eq: 'dependent'}}, {b: {$eq: 'independent'}}]}}]";
    string outputPipe =
        "[{$unwind: {path: '$a'}}, "
        "{$match: {$or: [{a: {$eq: 'dependent'}}, {b: {$eq: 'independent'}}]}}]";
    assertPipelineOptimizesTo(inputPipe, outputPipe);
}

TEST(PipelineOptimizationTest, UnwindBeforeDoubleMatchShouldRepeatedlyOptimize) {
    string inputPipe =
        "[{$unwind: '$a'}, "
        "{$match: {b: {$gt: 0}}}, "
        "{$match: {a: 1, c: 1}}]";
    string outputPipe =
        "[{$match: {$and: [{b: {$gt: 0}}, {c: {$eq: 1}}]}},"
        "{$unwind: {path: '$a'}}, "
        "{$match: {a: {$eq: 1}}}]";
    assertPipelineOptimizesTo(inputPipe, outputPipe);
}

TEST(PipelineOptimizationTest, GraphLookupShouldCoalesceWithUnwindOnAs) {
    string inputPipe =
        "[{$graphLookup: {from: 'lookupColl', as: 'out', connectToField: 'b', "
        "                 connectFromField: 'c', startWith: '$d'}}, "
        " {$unwind: '$out'}]";

    string outputPipe =
        "[{$graphLookup: {from: 'lookupColl', as: 'out', connectToField: 'b', "
        "                 connectFromField: 'c', startWith: '$d', "
        "                 unwinding: {preserveNullAndEmptyArrays: false}}}]";

    string serializedPipe =
        "[{$graphLookup: {from: 'lookupColl', as: 'out', connectToField: 'b', "
        "                 connectFromField: 'c', startWith: '$d'}}, "
        " {$unwind: {path: '$out'}}]";
    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe, serializedPipe);
}

TEST(PipelineOptimizationTest, GraphLookupShouldCoalesceWithUnwindOnAsWithPreserveEmpty) {
    string inputPipe =
        "[{$graphLookup: {from: 'lookupColl', as: 'out', connectToField: 'b', "
        "                 connectFromField: 'c', startWith: '$d'}}, "
        " {$unwind: {path: '$out', preserveNullAndEmptyArrays: true}}]";

    string outputPipe =
        "[{$graphLookup: {from: 'lookupColl', as: 'out', connectToField: 'b', "
        "                 connectFromField: 'c', startWith: '$d', "
        "                 unwinding: {preserveNullAndEmptyArrays: true}}}]";

    string serializedPipe =
        "[{$graphLookup: {from: 'lookupColl', as: 'out', connectToField: 'b', "
        "                 connectFromField: 'c', startWith: '$d'}}, "
        " {$unwind: {path: '$out', preserveNullAndEmptyArrays: true}}]";
    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe, serializedPipe);
}

TEST(PipelineOptimizationTest, GraphLookupShouldCoalesceWithUnwindOnAsWithIncludeArrayIndex) {
    string inputPipe =
        "[{$graphLookup: {from: 'lookupColl', as: 'out', connectToField: 'b', "
        "                 connectFromField: 'c', startWith: '$d'}}, "
        " {$unwind: {path: '$out', includeArrayIndex: 'index'}}]";

    string outputPipe =
        "[{$graphLookup: {from: 'lookupColl', as: 'out', connectToField: 'b', "
        "                 connectFromField: 'c', startWith: '$d', "
        "                 unwinding: {preserveNullAndEmptyArrays: false, "
        "                             includeArrayIndex: 'index'}}}]";

    string serializedPipe =
        "[{$graphLookup: {from: 'lookupColl', as: 'out', connectToField: 'b', "
        "                 connectFromField: 'c', "
        "                 startWith: '$d'}}, "
        " {$unwind: {path: '$out', includeArrayIndex: 'index'}}]";
    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe, serializedPipe);
}

TEST(PipelineOptimizationTest, GraphLookupShouldNotCoalesceWithUnwindNotOnAs) {
    string inputPipe =
        "[{$graphLookup: {from: 'lookupColl', as: 'out', connectToField: 'b', "
        "                 connectFromField: 'c', startWith: '$d'}}, "
        " {$unwind: '$nottherightthing'}]";

    string outputPipe =
        "[{$graphLookup: {from: 'lookupColl', as: 'out', connectToField: 'b', "
        "                 connectFromField: 'c', startWith: '$d'}}, "
        " {$unwind: {path: '$nottherightthing'}}]";
    assertPipelineOptimizesTo(inputPipe, outputPipe);
}

TEST(PipelineOptimizationTest, GraphLookupShouldSwapWithMatch) {
    string inputPipe =
        "[{$graphLookup: {"
        "    from: 'lookupColl',"
        "    as: 'results',"
        "    connectToField: 'to',"
        "    connectFromField: 'from',"
        "    startWith: '$startVal'"
        " }},"
        " {$match: {independent: 'x'}}"
        "]";
    string outputPipe =
        "[{$match: {independent: {$eq : 'x'}}},"
        " {$graphLookup: {"
        "    from: 'lookupColl',"
        "    as: 'results',"
        "    connectToField: 'to',"
        "    connectFromField: 'from',"
        "    startWith: '$startVal'"
        " }}]";
    string serializedPipe =
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

TEST(PipelineOptimizationTest, ExclusionProjectShouldSwapWithIndependentMatch) {
    string inputPipe = "[{$project: {redacted: 0}}, {$match: {unrelated: 4}}]";
    string outputPipe = "[{$match: {unrelated: {$eq : 4}}}, {$project: {redacted: false}}]";
    string serializedPipe = "[{$match : {unrelated : 4}}, {$project : {redacted : false}}]";
    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe, serializedPipe);
}

TEST(PipelineOptimizationTest, ExclusionProjectShouldNotSwapWithMatchOnExcludedFields) {
    std::string pipeline =
        "[{$project: {subdoc: {redacted: false}}}, {$match: {'subdoc.redacted': {$eq : 4}}}]";
    assertPipelineOptimizesTo(pipeline, pipeline);
}

TEST(PipelineOptimizationTest, MatchShouldSplitIfPartIsIndependentOfExclusionProjection) {
    string inputPipe =
        "[{$project: {redacted: 0}},"
        " {$match: {redacted: 'x', unrelated: 4}}]";
    string outputPipe =
        "[{$match: {unrelated: {$eq: 4}}},"
        " {$project: {redacted: false}},"
        " {$match: {redacted: {$eq: 'x'}}}]";
    assertPipelineOptimizesTo(inputPipe, outputPipe);
}

TEST(PipelineOptimizationTest, InclusionProjectShouldSwapWithIndependentMatch) {
    string inputPipe = "[{$project: {included: 1}}, {$match: {included: 4}}]";
    string outputPipe =
        "[{$match: {included: {$eq : 4}}}, {$project: {_id: true, included: true}}]";
    string serializedPipe =
        "[{$match : {included : 4}}, {$project : {_id: true, included : true}}]";
    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe, serializedPipe);
}

TEST(PipelineOptimizationTest, InclusionProjectShouldNotSwapWithMatchOnFieldsNotIncluded) {
    string inputPipe =
        "[{$project: {_id: true, included: true, subdoc: {included: true}}},"
        " {$match: {notIncluded: 'x', unrelated: 4}}]";
    string outputPipe =
        "[{$project: {_id: true, included: true, subdoc: {included: true}}},"
        " {$match: {$and: [{notIncluded: {$eq: 'x'}}, {unrelated: {$eq: 4}}]}}]";
    string serializedPipe =
        "[{$project: {_id: true, included: true, subdoc: {included: true}}},"
        " {$match: {notIncluded: 'x', unrelated: 4}}]";
    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe, serializedPipe);
}

TEST(PipelineOptimizationTest, MatchShouldSplitIfPartIsIndependentOfInclusionProjection) {
    string inputPipe =
        "[{$project: {_id: true, included: true}},"
        " {$match: {included: 'x', unrelated: 4}}]";
    string outputPipe =
        "[{$match: {included: {$eq: 'x'}}},"
        " {$project: {_id: true, included: true}},"
        " {$match: {unrelated: {$eq: 4}}}]";
    assertPipelineOptimizesTo(inputPipe, outputPipe);
}

TEST(PipelineOptimizationTest, TwoMatchStagesShouldBothPushIndependentPartsBeforeProjection) {
    string inputPipe =
        "[{$project: {_id: true, included: true}},"
        " {$match: {included: 'x', unrelated: 4}},"
        " {$match: {included: 'y', unrelated: 5}}]";
    string outputPipe =
        "[{$match: {$and: [{included: {$eq: 'x'}}, {included: {$eq: 'y'}}]}},"
        " {$project: {_id: true, included: true}},"
        " {$match: {$and: [{unrelated: {$eq: 4}}, {unrelated: {$eq: 5}}]}}]";
    assertPipelineOptimizesTo(inputPipe, outputPipe);
}

TEST(PipelineOptimizationTest, NeighboringMatchesShouldCoalesce) {
    string inputPipe =
        "[{$match: {x: 'x'}},"
        " {$match: {y: 'y'}}]";
    string outputPipe = "[{$match: {$and: [{x: {$eq: 'x'}}, {y: {$eq : 'y'}}]}}]";
    string serializedPipe = "[{$match: {$and: [{x: 'x'}, {y: 'y'}]}}]";
    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe, serializedPipe);
}

TEST(PipelineOptimizationTest, MatchShouldNotSwapBeforeLimit) {
    string inputPipe = "[{$limit: 3}, {$match: {y: 'y'}}]";
    string outputPipe = "[{$limit: 3}, {$match: {y: {$eq : 'y'}}}]";
    string serializedPipe = "[{$limit: 3}, {$match: {y: 'y'}}]";
    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe, serializedPipe);
}

TEST(PipelineOptimizationTest, MatchShouldNotSwapBeforeSkip) {
    string inputPipe = "[{$skip: 3}, {$match: {y: 'y'}}]";
    string outputPipe = "[{$skip: 3}, {$match: {y: {$eq : 'y'}}}]";
    string serializedPipe = "[{$skip: 3}, {$match: {y: 'y'}}]";
    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe, serializedPipe);
}

TEST(PipelineOptimizationTest, MatchShouldMoveAcrossProjectRename) {
    string inputPipe = "[{$project: {_id: true, a: '$b'}}, {$match: {a: {$eq: 1}}}]";
    string outputPipe = "[{$match: {b: {$eq: 1}}}, {$project: {_id: true, a: '$b'}}]";
    assertPipelineOptimizesTo(inputPipe, outputPipe);
}

TEST(PipelineOptimizationTest, MatchShouldMoveAcrossAddFieldsRename) {
    string inputPipe = "[{$addFields: {a: '$b'}}, {$match: {a: {$eq: 1}}}]";
    string outputPipe = "[{$match: {b: {$eq: 1}}}, {$addFields: {a: '$b'}}]";
    assertPipelineOptimizesTo(inputPipe, outputPipe);
}

TEST(PipelineOptimizationTest, MatchShouldMoveAcrossProjectRenameWithExplicitROOT) {
    string inputPipe = "[{$project: {_id: true, a: '$$ROOT.b'}}, {$match: {a: {$eq: 1}}}]";
    string outputPipe = "[{$match: {b: {$eq: 1}}}, {$project: {_id: true, a: '$$ROOT.b'}}]";
    assertPipelineOptimizesTo(inputPipe, outputPipe);
}

TEST(PipelineOptimizationTest, MatchShouldMoveAcrossAddFieldsRenameWithExplicitCURRENT) {
    string inputPipe = "[{$addFields: {a: '$$CURRENT.b'}}, {$match: {a: {$eq: 1}}}]";
    string outputPipe = "[{$match: {b: {$eq: 1}}}, {$addFields: {a: '$b'}}]";
    assertPipelineOptimizesTo(inputPipe, outputPipe);
}

TEST(PipelineOptimizationTest, PartiallyDependentMatchWithRenameShouldSplitAcrossAddFields) {
    string inputPipe =
        "[{$addFields: {'a.b': '$c', d: {$add: ['$e', '$f']}}},"
        "{$match: {$and: [{$or: [{'a.b': 1}, {x: 2}]}, {d: 3}]}}]";
    string outputPipe =
        "[{$match: {$or: [{c: {$eq: 1}}, {x: {$eq: 2}}]}},"
        "{$addFields: {a: {b: '$c'}, d: {$add: ['$e', '$f']}}},"
        "{$match: {d: {$eq: 3}}}]";
    assertPipelineOptimizesTo(inputPipe, outputPipe);
}

TEST(PipelineOptimizationTest, NorCanSplitAcrossProjectWithRename) {
    string inputPipe =
        "[{$project: {_id: false, x: true, y: '$z'}},"
        "{$match: {$nor: [{w: {$eq: 1}}, {y: {$eq: 1}}]}}]";
    string outputPipe =
        "[{$match: {$nor: [{z: {$eq: 1}}]}},"
        "{$project: {_id: false, x: true, y: '$z'}},"
        "{$match: {$nor: [{w: {$eq: 1}}]}}]";
    assertPipelineOptimizesTo(inputPipe, outputPipe);
}

TEST(PipelineOptimizationTest, MatchCanMoveAcrossSeveralRenames) {
    string inputPipe =
        "[{$project: {_id: false, c: '$d'}},"
        "{$addFields: {b: '$c'}},"
        "{$project: {a: '$b', z: 1}},"
        "{$match: {a: 1, z: 2}}]";
    string outputPipe =
        "[{$match: {d: {$eq: 1}}},"
        "{$project: {_id: false, c: '$d'}},"
        "{$match: {z: {$eq: 2}}},"
        "{$addFields: {b: '$c'}},"
        "{$project: {_id: true, z: true, a: '$b'}}]";
    assertPipelineOptimizesTo(inputPipe, outputPipe);
}

TEST(PipelineOptimizationTest, RenameShouldNotBeAppliedToDependentMatch) {
    string pipeline =
        "[{$project: {_id: false, x: {$add: ['$foo', '$bar']}, y: '$z'}},"
        "{$match: {$or: [{x: {$eq: 1}}, {y: {$eq: 1}}]}}]";
    assertPipelineOptimizesTo(pipeline, pipeline);
}

TEST(PipelineOptimizationTest, MatchCannotMoveAcrossAddFieldsRenameOfDottedPath) {
    string pipeline = "[{$addFields: {a: '$b.c'}}, {$match: {a: {$eq: 1}}}]";
    assertPipelineOptimizesTo(pipeline, pipeline);
}

TEST(PipelineOptimizationTest, MatchCannotMoveAcrossProjectRenameOfDottedPath) {
    string inputPipe = "[{$project: {_id: false, a: '$$CURRENT.b.c'}}, {$match: {a: {$eq: 1}}}]";
    string outputPipe = "[{$project: {_id: false, a: '$b.c'}}, {$match: {a: {$eq: 1}}}]";
    assertPipelineOptimizesTo(inputPipe, outputPipe);
}

TEST(PipelineOptimizationTest, MatchWithTypeShouldMoveAcrossRename) {
    string inputPipe = "[{$addFields: {a: '$b'}}, {$match: {a: {$type: 4}}}]";
    string outputPipe = "[{$match: {b: {$type: [4]}}}, {$addFields: {a: '$b'}}]";
    assertPipelineOptimizesTo(inputPipe, outputPipe);
}

TEST(PipelineOptimizationTest, MatchOnArrayFieldCanSplitAcrossRenameWithMapAndProject) {
    string inputPipe =
        "[{$project: {d: {$map: {input: '$a', as: 'iter', in: {e: '$$iter.b', f: {$add: "
        "['$$iter.c', 1]}}}}}}, {$match: {'d.e': 1, 'd.f': 1}}]";
    string outputPipe =
        "[{$match: {'a.b': {$eq: 1}}}, {$project: {_id: true, d: {$map: {input: '$a', as: 'iter', "
        "in: {e: '$$iter.b', f: {$add: ['$$iter.c', {$const: 1}]}}}}}}, {$match: {'d.f': {$eq: "
        "1}}}]";
    assertPipelineOptimizesTo(inputPipe, outputPipe);
}

TEST(PipelineOptimizationTest, MatchOnArrayFieldCanSplitAcrossRenameWithMapAndAddFields) {
    string inputPipe =
        "[{$addFields: {d: {$map: {input: '$a', as: 'iter', in: {e: '$$iter.b', f: {$add: "
        "['$$iter.c', 1]}}}}}}, {$match: {'d.e': 1, 'd.f': 1}}]";
    string outputPipe =
        "[{$match: {'a.b': {$eq: 1}}}, {$addFields: {d: {$map: {input: '$a', as: 'iter', in: {e: "
        "'$$iter.b', f: {$add: ['$$iter.c', {$const: 1}]}}}}}}, {$match: {'d.f': {$eq: 1}}}]";
    assertPipelineOptimizesTo(inputPipe, outputPipe);
}

TEST(PipelineOptimizationTest, MatchCannotSwapWithLimit) {
    string pipeline = "[{$limit: 3}, {$match: {x: {$gt: 0}}}]";
    assertPipelineOptimizesTo(pipeline, pipeline);
}

TEST(PipelineOptimizationTest, MatchCannotSwapWithSortLimit) {
    string inputPipe = "[{$sort: {x: -1}}, {$limit: 3}, {$match: {x: {$gt: 0}}}]";
    string outputPipe = "[{$sort: {sortKey: {x: -1}, limit: 3}}, {$match: {x: {$gt: 0}}}]";
    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe, inputPipe);
}

// Descriptive test. The following internal match expression *could* participate in pipeline
// optimizations, but it currently does not.
TEST(PipelineOptimizationTest, MatchOnMinItemsShouldNotSwapSinceCategoryIsArrayMatching) {
    string inputPipe =
        "[{$project: {_id: true, a: '$b'}}, "
        "{$match: {a: {$_internalSchemaMinItems: 1}}}]";
    assertPipelineOptimizesTo(inputPipe, inputPipe);

    inputPipe =
        "[{$project: {redacted: false}}, "
        "{$match: {a: {$_internalSchemaMinItems: 1}}}]";
    assertPipelineOptimizesTo(inputPipe, inputPipe);

    inputPipe =
        "[{$addFields : {a : {$const: 1}}}, "
        "{$match: {b: {$_internalSchemaMinItems: 1}}}]";
    assertPipelineOptimizesTo(inputPipe, inputPipe);
}

// Descriptive test. The following internal match expression *could* participate in pipeline
// optimizations, but it currently does not.
TEST(PipelineOptimizationTest, MatchOnMaxItemsShouldNotSwapSinceCategoryIsArrayMatching) {
    string inputPipe =
        "[{$project: {_id: true, a: '$b'}}, "
        "{$match: {a: {$_internalSchemaMaxItems: 1}}}]";
    assertPipelineOptimizesTo(inputPipe, inputPipe);

    inputPipe =
        "[{$project: {redacted: false}}, "
        "{$match: {a: {$_internalSchemaMaxItems: 1}}}]";
    assertPipelineOptimizesTo(inputPipe, inputPipe);

    inputPipe =
        "[{$addFields : {a : {$const: 1}}}, "
        "{$match: {b: {$_internalSchemaMaxItems: 1}}}]";
    assertPipelineOptimizesTo(inputPipe, inputPipe);
}

// Descriptive test. The following internal match expression *could* participate in pipeline
// optimizations, but it currently does not.
TEST(PipelineOptimizationTest,
     MatchOnAllElemMatchFromIndexShouldNotSwapSinceCategoryIsArrayMatching) {
    string inputPipe =
        "[{$project: {_id: true, a: '$b'}}, "
        "{$match: {a: {$_internalSchemaAllElemMatchFromIndex: [1, {b: {$gt: 0}}]}}}]";
    assertPipelineOptimizesTo(inputPipe, inputPipe);

    inputPipe =
        "[{$project: {redacted: false}}, "
        "{$match: {a: {$_internalSchemaAllElemMatchFromIndex: [1, {b: {$gt: 0}}]}}}]";
    assertPipelineOptimizesTo(inputPipe, inputPipe);

    inputPipe =
        "[{$addFields : {a : {$const: 1}}}, "
        "{$match: {a: {$_internalSchemaAllElemMatchFromIndex: [1, {b: {$gt: 0}}]}}}]";
    assertPipelineOptimizesTo(inputPipe, inputPipe);
}

// Descriptive test. The following internal match expression *could* participate in pipeline
// optimizations, but it currently does not.
TEST(PipelineOptimizationTest, MatchOnArrayIndexShouldNotSwapSinceCategoryIsArrayMatching) {
    string inputPipe = R"(
        [{$project: {_id: true, a: '$b'}},
        {$match: {a: {$_internalSchemaMatchArrayIndex:
           {index: 0, namePlaceholder: 'i', expression: {i: {$lt: 0}}}}}}])";
    assertPipelineOptimizesTo(inputPipe, inputPipe);

    inputPipe = R"(
        [{$project: {redacted: false}},
        {$match: {a: {$_internalSchemaMatchArrayIndex:
           {index: 0, namePlaceholder: 'i', expression: {i: {$lt: 0}}}}}}])";
    assertPipelineOptimizesTo(inputPipe, inputPipe);

    inputPipe = R"(
        [{$addFields : {a : {$const: 1}}},
        {$match: {a: {$_internalSchemaMatchArrayIndex:
           {index: 0, namePlaceholder: 'i', expression: {i: {$lt: 0}}}}}}])";
    assertPipelineOptimizesTo(inputPipe, inputPipe);
}

// Descriptive test. The following internal match expression *could* participate in pipeline
// optimizations, but it currently does not.
TEST(PipelineOptimizationTest, MatchOnUniqueItemsShouldNotSwapSinceCategoryIsArrayMatching) {
    string inputPipe =
        "[{$project: {_id: true, a: '$b'}}, "
        "{$match: {a: {$_internalSchemaUniqueItems: true}}}]";
    assertPipelineOptimizesTo(inputPipe, inputPipe);

    inputPipe =
        "[{$project: {redacted: false}}, "
        "{$match: {a: {$_internalSchemaUniqueItems: true}}}]";
    assertPipelineOptimizesTo(inputPipe, inputPipe);

    inputPipe =
        "[{$addFields : {a : {$const: 1}}}, "
        "{$match: {a: {$_internalSchemaUniqueItems: true}}}]";
    assertPipelineOptimizesTo(inputPipe, inputPipe);
}

// Descriptive test. The following internal match expression *could* participate in pipeline
// optimizations, but it currently does not.
TEST(PipelineOptimizationTest, MatchOnObjectMatchShouldNotSwapSinceCategoryIsOther) {
    string inputPipe =
        "[{$project: {_id: true, a: '$b'}}, "
        "{$match: {a: {$_internalSchemaObjectMatch: {b: 1}}}}]";
    string outputPipe =
        "[{$project: {_id: true, a: '$b'}}, "
        "{$match: {a: {$_internalSchemaObjectMatch: {b: {$eq: 1}}}}}]";
    string serializedPipe =
        "[{$project: {_id: true, a: '$b'}},"
        "{$match: {a: {$_internalSchemaObjectMatch: {b: 1}}}}]";

    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe, serializedPipe);

    inputPipe =
        "[{$project: {redacted: false}}, "
        "{$match: {a: {$_internalSchemaObjectMatch: {b: 1}}}}]";
    outputPipe =
        "[{$project: {redacted: false}},"
        "{$match: {a: {$_internalSchemaObjectMatch: {b: {$eq: 1}}}}}]";
    serializedPipe =
        "[{$project: {redacted: false}},"
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
    string inputPipe =
        "[{$project: {_id: true, a: '$b'}}, "
        "{$match: {$_internalSchemaMinProperties: 2}}]";
    assertPipelineOptimizesTo(inputPipe, inputPipe);

    inputPipe =
        "[{$project: {redacted: false}}, "
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
    string inputPipe =
        "[{$project: {_id: true, a: '$b'}}, "
        "{$match: {$_internalSchemaMaxProperties: 2}}]";
    assertPipelineOptimizesTo(inputPipe, inputPipe);

    inputPipe =
        "[{$project: {redacted: false}}, "
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
    string inputPipe = R"(
        [{$project: {_id: true, a: '$b'}},
        {$match: {$_internalSchemaAllowedProperties: {
            properties: ['b'],
            namePlaceholder: 'i',
            patternProperties: [],
            otherwise: {i: 1}
        }}}])";
    string outputPipe = R"(
        [{$project: {_id: true, a: '$b'}},
        {$match: {$_internalSchemaAllowedProperties: {
            properties: ['b'],
            namePlaceholder: 'i',
            patternProperties: [],
            otherwise: {i: {$eq : 1}}
        }}}])";
    string serializedPipe = R"(
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
        [{$project: {redacted: false}},
        {$match: {$_internalSchemaAllowedProperties: {
            properties: ['b'],
            namePlaceholder: 'i',
            patternProperties: [],
            otherwise: {i: {$eq: 1}
        }}}}])";
    serializedPipe = R"(
        [{$project: {redacted: false}},
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
    string inputPipe =
        "[{$project: {_id: true, a: '$b'}}, "
        "{$match: {$_internalSchemaCond: [{a: 1}, {b: 1}, {c: 1}]}}]";
    string outputPipe =
        "[{$project: {_id: true, a: '$b'}}, "
        "{$match: {$_internalSchemaCond: [{a: {$eq : 1}}, {b: {$eq : 1}}, {c: {$eq : 1}}]}}]";
    string serializedPipe =
        "[{$project: {_id: true, a: '$b'}}, "
        "{$match: {$_internalSchemaCond: [{a: 1}, {b: 1}, {c: 1}]}}]";
    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe, serializedPipe);

    inputPipe =
        "[{$project: {redacted: false}}, "
        "{$match: {$_internalSchemaCond: [{a: 1}, {b: 1}, {c: 1}]}}]";
    outputPipe =
        "[{$project: {redacted: false}}, "
        "{$match: {$_internalSchemaCond: [{a: {$eq : 1}}, {b: {$eq: 1}}, {c: {$eq: 1}}]}}]";
    serializedPipe =
        "[{$project: {redacted: false}}, "
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
    string inputPipe =
        "[{$project: {_id: true, a: '$b'}}, "
        "{$match: {$_internalSchemaRootDocEq: {a: 1}}}]";
    assertPipelineOptimizesTo(inputPipe, inputPipe);

    inputPipe =
        "[{$project: {redacted: false}}, "
        "{$match: {$_internalSchemaRootDocEq: {a: 1}}}]";
    assertPipelineOptimizesTo(inputPipe, inputPipe);

    inputPipe =
        "[{$addFields : {a : {$const: 1}}}, "
        "{$match: {$_internalSchemaRootDocEq: {a: 1}}}]";
    assertPipelineOptimizesTo(inputPipe, inputPipe);
}

// Descriptive test. The following internal match expression *could* participate in pipeline
// optimizations, but it currently does not.
TEST(PipelineOptimizationTest, MatchOnInternalSchemaTypeShouldNotSwapSinceCategoryIsOther) {
    string inputPipe =
        "[{$project: {_id: true, a: '$b'}}, "
        "{$match: {a: {$_internalSchemaType: 1}}}]";
    string outputPipe =
        "[{$project: {_id: true, a: '$b'}}, "
        " {$match: {a: {$_internalSchemaType: [1]}}}]";
    string serializedPipe =
        "[{$project: {_id: true, a: '$b'}}, "
        "{$match: {a: {$_internalSchemaType: 1}}}]";
    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe, serializedPipe);

    inputPipe =
        "[{$project: {redacted: false}}, "
        "{$match: {a: {$_internalSchemaType: 1}}}]";
    outputPipe =
        "[{$project: {redacted: false}}, "
        " {$match: {a: {$_internalSchemaType: [1]}}}]";
    serializedPipe =
        "[{$project: {redacted: false}}, "
        "{$match: {a: {$_internalSchemaType: 1}}}]";
    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe, serializedPipe);

    inputPipe =
        "[{$addFields : {a : {$const: 1}}}, "
        "{$match: {b: {$_internalSchemaType: 1}}}]";
    outputPipe =
        "[{$addFields : {a : {$const: 1}}}, "
        " {$match: {b: {$_internalSchemaType: [1]}}}]";
    serializedPipe =
        "[{$addFields : {a : {$const: 1}}}, "
        "{$match: {b: {$_internalSchemaType: 1}}}]";
    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe, serializedPipe);
}

TEST(PipelineOptimizationTest, MatchOnMinLengthShouldSwapWithAdjacentStage) {
    string inputPipe =
        "[{$project: {_id: true, a: '$b'}}, "
        "{$match: {a: {$_internalSchemaMinLength: 1}}}]";
    string outputPipe =
        "[{$match: {b: {$_internalSchemaMinLength: 1}}},"
        "{$project: {_id: true, a: '$b'}}]";
    assertPipelineOptimizesTo(inputPipe, outputPipe);

    inputPipe =
        "[{$project: {redacted: false}}, "
        "{$match: {a: {$_internalSchemaMinLength: 1}}}]";
    outputPipe =
        "[{$match: {a: {$_internalSchemaMinLength: 1}}},"
        "{$project: {redacted: false}}]";
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
    string inputPipe =
        "[{$project: {_id: true, a: '$b'}}, "
        "{$match: {a: {$_internalSchemaMaxLength: 1}}}]";
    string outputPipe =
        "[{$match: {b: {$_internalSchemaMaxLength: 1}}},"
        "{$project: {_id: true, a: '$b'}}]";
    assertPipelineOptimizesTo(inputPipe, outputPipe);

    inputPipe =
        "[{$project: {redacted: false}}, "
        "{$match: {a: {$_internalSchemaMaxLength: 1}}}]";
    outputPipe =
        "[{$match: {a: {$_internalSchemaMaxLength: 1}}}, "
        "{$project: {redacted: false}}]";
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
    string inputPipe =
        "[{$project: {_id: true, a: '$b'}}, "
        "{$match: {a: {$_internalSchemaEq: {c: 1}}}}]";
    string outputPipe =
        "[{$match: {b: {$_internalSchemaEq: {c: 1}}}}, "
        "{$project: {_id: true, a: '$b'}}]";
    assertPipelineOptimizesTo(inputPipe, outputPipe);

    inputPipe =
        "[{$project: {redacted: false}}, "
        "{$match: {a: {$_internalSchemaEq: {c: 1}}}}]";
    outputPipe =
        "[{$match: {a: {$_internalSchemaEq: {c: 1}}}}, "
        "{$project: {redacted: false}}]";
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
    string inputPipe =
        "[{$project: {_id: true, a: '$b', c: '$d'}}, "
        "{$match: {$_internalSchemaXor: [{a: 1}, {c: 1}]}}]";
    string outputPipe =
        "[{$match: {$_internalSchemaXor: [{b: {$eq: 1}}, {d: {$eq: 1}}]}}, "
        "{$project: {_id: true, a: '$b', c: '$d'}}]";
    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe, outputPipe);

    inputPipe =
        "[{$project: {redacted: false}}, "
        "{$match: {$_internalSchemaXor: [{a: 1}, {b: 1}]}}]";
    outputPipe =
        "[{$match: {$_internalSchemaXor: [{a: {$eq : 1}}, {b: {$eq : 1}}]}}, "
        "{$project: {redacted: false}}]";
    string serializedPipe =
        "[{$match: {$_internalSchemaXor: [{a: 1}, {b: 1}]}}, "
        " {$project: {redacted: false}}]";
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
        "{$match: {$_internalSchemaXor: [{b: {$eq: 1}}, {a: {$eq: 1}}]}}]";
    serializedPipe =
        "[{$addFields: {a: {$const: 1}}}, "
        "{$match: {$_internalSchemaXor: [{b: 1}, {a: 1}]}}]";
    assertPipelineOptimizesAndSerializesTo(inputPipe, outputPipe, serializedPipe);
}

TEST(PipelineOptimizationTest, MatchOnFmodShouldSwapWithAdjacentStage) {
    string inputPipe =
        "[{$project: {_id: true, a: '$b'}}, "
        "{$match: {a: {$_internalSchemaFmod: [5, 0]}}}]";
    string outputPipe =
        "[{$match: {b: {$_internalSchemaFmod: [5, 0]}}}, "
        "{$project: {_id: true, a: '$b'}}]";
    assertPipelineOptimizesTo(inputPipe, outputPipe);

    inputPipe =
        "[{$project: {redacted: false}}, "
        "{$match: {a: {$_internalSchemaFmod: [5, 0]}}}]";
    outputPipe =
        "[{$match: {a: {$_internalSchemaFmod: [5, 0]}}}, "
        "{$project: {redacted: false}}]";
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

    intrusive_ptr<ExpressionContext> expCtx(new ExpressionContextForTest(kTestNss));
    expCtx->opCtx = opCtx.get();
    setMockReplicationCoordinatorOnOpCtx(expCtx->opCtx);

    auto spec = BSON("$changeStream" << BSON("fullDocument"
                                             << "updateLookup"));
    auto stages = DocumentSourceChangeStream::createFromBson(spec.firstElement(), expCtx);
    ASSERT_EQ(stages.size(), 4UL);
    // Make sure the change lookup is at the end.
    ASSERT(dynamic_cast<DocumentSourceLookupChangePostImage*>(stages.back().get()));

    auto matchPredicate = BSON("extra"
                               << "predicate");
    stages.push_back(DocumentSourceMatch::create(matchPredicate, expCtx));
    auto pipeline = uassertStatusOK(Pipeline::create(stages, expCtx));
    pipeline->optimizePipeline();

    // Make sure the $match stage has swapped before the change look up.
    ASSERT(dynamic_cast<DocumentSourceLookupChangePostImage*>(pipeline->getSources().back().get()));
}

TEST(PipelineOptimizationTest, ChangeStreamLookupDoesNotSwapWithMatchOnPostImage) {
    QueryTestServiceContext testServiceContext;
    auto opCtx = testServiceContext.makeOperationContext();

    intrusive_ptr<ExpressionContext> expCtx(new ExpressionContextForTest(kTestNss));
    expCtx->opCtx = opCtx.get();
    setMockReplicationCoordinatorOnOpCtx(expCtx->opCtx);

    auto spec = BSON("$changeStream" << BSON("fullDocument"
                                             << "updateLookup"));
    auto stages = DocumentSourceChangeStream::createFromBson(spec.firstElement(), expCtx);
    ASSERT_EQ(stages.size(), 4UL);
    // Make sure the change lookup is at the end.
    ASSERT(dynamic_cast<DocumentSourceLookupChangePostImage*>(stages.back().get()));

    stages.push_back(DocumentSourceMatch::create(
        BSON(DocumentSourceLookupChangePostImage::kFullDocumentFieldName << BSONNULL), expCtx));
    auto pipeline = uassertStatusOK(Pipeline::create(stages, expCtx));
    pipeline->optimizePipeline();

    // Make sure the $match stage stays at the end.
    ASSERT(dynamic_cast<DocumentSourceMatch*>(pipeline->getSources().back().get()));
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

}  // namespace Local

namespace Sharded {

class Base {
public:
    // These all return json arrays of pipeline operators
    virtual string inputPipeJson() = 0;
    virtual string shardPipeJson() = 0;
    virtual string mergePipeJson() = 0;

    BSONObj pipelineFromJsonArray(const string& array) {
        return fromjson("{pipeline: " + array + "}");
    }
    virtual void run() {
        const BSONObj inputBson = pipelineFromJsonArray(inputPipeJson());
        const BSONObj shardPipeExpected = pipelineFromJsonArray(shardPipeJson());
        const BSONObj mergePipeExpected = pipelineFromJsonArray(mergePipeJson());

        ASSERT_EQUALS(inputBson["pipeline"].type(), BSONType::Array);
        vector<BSONObj> rawPipeline;
        for (auto&& stageElem : inputBson["pipeline"].Array()) {
            ASSERT_EQUALS(stageElem.type(), BSONType::Object);
            rawPipeline.push_back(stageElem.embeddedObject());
        }
        AggregationRequest request(kTestNss, rawPipeline);
        intrusive_ptr<ExpressionContextForTest> ctx =
            new ExpressionContextForTest(&_opCtx, request);

        // For $graphLookup and $lookup, we have to populate the resolvedNamespaces so that the
        // operations will be able to have a resolved view definition.
        NamespaceString lookupCollNs("a", "lookupColl");
        ctx->setResolvedNamespace(lookupCollNs, {lookupCollNs, std::vector<BSONObj>{}});

        // Test that we can both split the pipeline and reassemble it into its original form.
        mergePipe = uassertStatusOK(Pipeline::parse(request.getPipeline(), ctx));
        mergePipe->optimizePipeline();

        shardPipe = mergePipe->splitForSharded();
        ASSERT(shardPipe);

        ASSERT_VALUE_EQ(Value(shardPipe->writeExplainOps(ExplainOptions::Verbosity::kQueryPlanner)),
                        Value(shardPipeExpected["pipeline"]));
        ASSERT_VALUE_EQ(Value(mergePipe->writeExplainOps(ExplainOptions::Verbosity::kQueryPlanner)),
                        Value(mergePipeExpected["pipeline"]));
    }

    virtual ~Base() {}

protected:
    std::unique_ptr<Pipeline, PipelineDeleter> mergePipe;
    std::unique_ptr<Pipeline, PipelineDeleter> shardPipe;

private:
    OperationContextNoop _opCtx;
};

// General test to make sure all optimizations support empty pipelines
class Empty : public Base {
    string inputPipeJson() {
        return "[]";
    }
    string shardPipeJson() {
        return "[]";
    }
    string mergePipeJson() {
        return "[]";
    }
};

namespace moveFinalUnwindFromShardsToMerger {

class OneUnwind : public Base {
    string inputPipeJson() {
        return "[{$unwind: {path: '$a'}}]}";
    }
    string shardPipeJson() {
        return "[]}";
    }
    string mergePipeJson() {
        return "[{$unwind: {path: '$a'}}]}";
    }
};

class TwoUnwind : public Base {
    string inputPipeJson() {
        return "[{$unwind: {path: '$a'}}, {$unwind: {path: '$b'}}]}";
    }
    string shardPipeJson() {
        return "[]}";
    }
    string mergePipeJson() {
        return "[{$unwind: {path: '$a'}}, {$unwind: {path: '$b'}}]}";
    }
};

class UnwindNotFinal : public Base {
    string inputPipeJson() {
        return "[{$unwind: {path: '$a'}}, {$match: {a:1}}]}";
    }
    string shardPipeJson() {
        return "[{$unwind: {path: '$a'}}, {$match: {a:{$eq:1}}}]}";
    }
    string mergePipeJson() {
        return "[]}";
    }
};

class UnwindWithOther : public Base {
    string inputPipeJson() {
        return "[{$match: {a:1}}, {$unwind: {path: '$a'}}]}";
    }
    string shardPipeJson() {
        return "[{$match: {a:{$eq:1}}}]}";
    }
    string mergePipeJson() {
        return "[{$unwind: {path: '$a'}}]}";
    }
};
}  // namespace moveFinalUnwindFromShardsToMerger


namespace limitFieldsSentFromShardsToMerger {
// These tests use $limit to split the pipelines between shards and merger as it is
// always a split point and neutral in terms of needed fields.

class NeedWholeDoc : public Base {
    string inputPipeJson() {
        return "[{$limit:1}]";
    }
    string shardPipeJson() {
        return "[{$limit:1}]";
    }
    string mergePipeJson() {
        return "[{$limit:1}]";
    }
};

class JustNeedsId : public Base {
    string inputPipeJson() {
        return "[{$limit:1}, {$group: {_id: '$_id'}}]";
    }
    string shardPipeJson() {
        return "[{$limit:1}, {$project: {_id:true}}]";
    }
    string mergePipeJson() {
        return "[{$limit:1}, {$group: {_id: '$_id'}}]";
    }
};

class JustNeedsNonId : public Base {
    string inputPipeJson() {
        return "[{$limit:1}, {$group: {_id: '$a.b'}}]";
    }
    string shardPipeJson() {
        return "[{$limit:1}, {$project: {_id: false, a: {b: true}}}]";
    }
    string mergePipeJson() {
        return "[{$limit:1}, {$group: {_id: '$a.b'}}]";
    }
};

class NothingNeeded : public Base {
    string inputPipeJson() {
        return "[{$limit:1}"
               ",{$group: {_id: {$const: null}, count: {$sum: {$const: 1}}}}"
               "]";
    }
    string shardPipeJson() {
        return "[{$limit:1}"
               ",{$project: {_id: true}}"
               "]";
    }
    string mergePipeJson() {
        return "[{$limit:1}"
               ",{$group: {_id: {$const: null}, count: {$sum: {$const: 1}}}}"
               "]";
    }
};

class ShardAlreadyExhaustive : public Base {
    // No new project should be added. This test reflects current behavior where the
    // 'a' field is still sent because it is explicitly asked for, even though it
    // isn't actually needed. If this changes in the future, this test will need to
    // change.
    string inputPipeJson() {
        return "[{$project: {_id:true, a:true}}"
               ",{$group: {_id: '$_id'}}"
               "]";
    }
    string shardPipeJson() {
        return "[{$project: {_id:true, a:true}}"
               ",{$group: {_id: '$_id'}}"
               "]";
    }
    string mergePipeJson() {
        return "[{$group: {_id: '$$ROOT._id', $doingMerge: true}}"
               "]";
    }
};

class ShardedSortMatchProjSkipLimBecomesMatchTopKSortSkipProj : public Base {
    string inputPipeJson() {
        return "[{$sort: {a : 1}}"
               ",{$match: {a: 1}}"
               ",{$project : {a: 1}}"
               ",{$skip : 3}"
               ",{$limit: 5}"
               "]";
    }
    string shardPipeJson() {
        return "[{$match: {a: {$eq : 1}}}"
               ",{$sort: {sortKey: {a: 1}, limit: 8}}"
               ",{$project: {_id: true, a: true}}"
               "]";
    }
    string mergePipeJson() {
        return "[{$sort: {sortKey: {a: 1}, mergePresorted: true, limit: 8}}"
               ",{$skip: 3}"
               ",{$project: {_id: true, a: true}}"
               "]";
    }
};

class ShardedMatchProjLimDoesNotBecomeMatchLimProj : public Base {
    string inputPipeJson() {
        return "[{$match: {a: 1}}"
               ",{$project : {a: 1}}"
               ",{$limit: 5}"
               "]";
    }
    string shardPipeJson() {
        return "[{$match: {a: {$eq : 1}}}"
               ",{$project: {_id: true, a: true}}"
               ",{$limit: 5}"
               "]";
    }
    string mergePipeJson() {
        return "[{$limit: 5}]";
    }
};

class ShardedSortProjLimBecomesTopKSortProj : public Base {
    string inputPipeJson() {
        return "[{$sort: {a : 1}}"
               ",{$project : {a: 1}}"
               ",{$limit: 5}"
               "]";
    }
    string shardPipeJson() {
        return "[{$sort: {sortKey: {a: 1}, limit: 5}}"
               ",{$project: {_id: true, a: true}}"
               "]";
    }
    string mergePipeJson() {
        return "[{$sort: {sortKey: {a: 1}, mergePresorted: true, limit: 5}}"
               ",{$project: {_id: true, a: true}}"
               "]";
    }
};

class ShardedSortGroupProjLimDoesNotBecomeTopKSortProjGroup : public Base {
    string inputPipeJson() {
        return "[{$sort: {a : 1}}"
               ",{$group : {_id: {a: '$a'}}}"
               ",{$project : {a: 1}}"
               ",{$limit: 5}"
               "]";
    }
    string shardPipeJson() {
        return "[{$sort: {sortKey: {a: 1}}}"
               ",{$project : {_id: false, a: true}}"
               "]";
    }
    string mergePipeJson() {
        return "[{$sort: {sortKey: {a: 1}, mergePresorted: true}}"
               ",{$group : {_id: {a: '$a'}}}"
               ",{$project: {_id: true, a: true}}"
               ",{$limit: 5}"
               "]";
    }
};

class ShardedMatchSortProjLimBecomesMatchTopKSortProj : public Base {
    string inputPipeJson() {
        return "[{$match: {a: {$eq : 1}}}"
               ",{$sort: {a: -1}}"
               ",{$project : {a: 1}}"
               ",{$limit: 6}"
               "]";
    }
    string shardPipeJson() {
        return "[{$match: {a: {$eq : 1}}}"
               ",{$sort: {sortKey: {a: -1}, limit: 6}}"
               ",{$project: {_id: true, a: true}}"
               "]";
    }
    string mergePipeJson() {
        return "[{$sort: {sortKey: {a: -1}, mergePresorted: true, limit: 6}}"
               ",{$project: {_id: true, a: true}}"
               "]";
    }
};

}  // namespace limitFieldsSentFromShardsToMerger

namespace coalesceLookUpAndUnwind {

class ShouldCoalesceUnwindOnAs : public Base {
    string inputPipeJson() {
        return "[{$lookup: {from : 'lookupColl', as : 'same', localField: 'left', foreignField: "
               "'right'}}"
               ",{$unwind: {path: '$same'}}"
               "]";
    }
    string shardPipeJson() {
        return "[]";
    }
    string mergePipeJson() {
        return "[{$lookup: {from : 'lookupColl', as : 'same', localField: 'left', foreignField: "
               "'right', unwinding: {preserveNullAndEmptyArrays: false}}}]";
    }
};

class ShouldCoalesceUnwindOnAsWithPreserveEmpty : public Base {
    string inputPipeJson() {
        return "[{$lookup: {from : 'lookupColl', as : 'same', localField: 'left', foreignField: "
               "'right'}}"
               ",{$unwind: {path: '$same', preserveNullAndEmptyArrays: true}}"
               "]";
    }
    string shardPipeJson() {
        return "[]";
    }
    string mergePipeJson() {
        return "[{$lookup: {from : 'lookupColl', as : 'same', localField: 'left', foreignField: "
               "'right', unwinding: {preserveNullAndEmptyArrays: true}}}]";
    }
};

class ShouldCoalesceUnwindOnAsWithIncludeArrayIndex : public Base {
    string inputPipeJson() {
        return "[{$lookup: {from : 'lookupColl', as : 'same', localField: 'left', foreignField: "
               "'right'}}"
               ",{$unwind: {path: '$same', includeArrayIndex: 'index'}}"
               "]";
    }
    string shardPipeJson() {
        return "[]";
    }
    string mergePipeJson() {
        return "[{$lookup: {from : 'lookupColl', as : 'same', localField: 'left', foreignField: "
               "'right', unwinding: {preserveNullAndEmptyArrays: false, includeArrayIndex: "
               "'index'}}}]";
    }
};

class ShouldNotCoalesceUnwindNotOnAs : public Base {
    string inputPipeJson() {
        return "[{$lookup: {from : 'lookupColl', as : 'same', localField: 'left', foreignField: "
               "'right'}}"
               ",{$unwind: {path: '$from'}}"
               "]";
    }
    string shardPipeJson() {
        return "[]";
    }
    string mergePipeJson() {
        return "[{$lookup: {from : 'lookupColl', as : 'same', localField: 'left', foreignField: "
               "'right'}}"
               ",{$unwind: {path: '$from'}}"
               "]";
    }
};

}  // namespace coalesceLookUpAndUnwind

namespace needsPrimaryShardMerger {

class needsPrimaryShardMergerBase : public Base {
public:
    void run() override {
        Base::run();
        ASSERT_EQUALS(mergePipe->needsPrimaryShardMerger(), needsPrimaryShardMerger());
        ASSERT(!shardPipe->needsPrimaryShardMerger());
    }
    virtual bool needsPrimaryShardMerger() = 0;
};

class Out : public needsPrimaryShardMergerBase {
    bool needsPrimaryShardMerger() {
        return true;
    }
    string inputPipeJson() {
        return "[{$out: 'outColl'}]";
    }
    string shardPipeJson() {
        return "[]";
    }
    string mergePipeJson() {
        return "[{$out: 'outColl'}]";
    }
};

class Project : public needsPrimaryShardMergerBase {
    bool needsPrimaryShardMerger() {
        return false;
    }
    string inputPipeJson() {
        return "[{$project: {a : 1}}]";
    }
    string shardPipeJson() {
        return "[{$project: {_id: true, a: true}}]";
    }
    string mergePipeJson() {
        return "[]";
    }
};

class LookUp : public needsPrimaryShardMergerBase {
    bool needsPrimaryShardMerger() {
        return true;
    }
    string inputPipeJson() {
        return "[{$lookup: {from : 'lookupColl', as : 'same', localField: 'left', foreignField: "
               "'right'}}]";
    }
    string shardPipeJson() {
        return "[]";
    }
    string mergePipeJson() {
        return "[{$lookup: {from : 'lookupColl', as : 'same', localField: 'left', foreignField: "
               "'right'}}]";
    }
};

}  // namespace needsPrimaryShardMerger

namespace mustRunOnMongoS {

// Like a DocumentSourceMock, but must run on mongoS and can be used anywhere in the pipeline.
class DocumentSourceMustRunOnMongoS : public DocumentSourceMock {
public:
    DocumentSourceMustRunOnMongoS() : DocumentSourceMock({}) {}

    StageConstraints constraints(Pipeline::SplitState pipeState) const final {
        // Overrides DocumentSourceMock's required position.
        return {StreamType::kStreaming,
                PositionRequirement::kNone,
                HostTypeRequirement::kMongoS,
                DiskUseRequirement::kNoDiskUse,
                FacetRequirement::kNotAllowed,
                TransactionRequirement::kAllowed};
    }

    static boost::intrusive_ptr<DocumentSourceMustRunOnMongoS> create() {
        return new DocumentSourceMustRunOnMongoS();
    }
};

using HostTypeRequirement = DocumentSource::StageConstraints::HostTypeRequirement;
using PipelineMustRunOnMongoSTest = AggregationContextFixture;

TEST_F(PipelineMustRunOnMongoSTest, UnsplittablePipelineMustRunOnMongoS) {
    auto expCtx = getExpCtx();
    expCtx->inMongos = true;

    auto match = DocumentSourceMatch::create(fromjson("{x: 5}"), expCtx);
    auto runOnMongoS = DocumentSourceMustRunOnMongoS::create();

    auto pipeline = uassertStatusOK(Pipeline::create({match, runOnMongoS}, expCtx));
    ASSERT_TRUE(pipeline->requiredToRunOnMongos());

    pipeline->optimizePipeline();
    ASSERT_TRUE(pipeline->requiredToRunOnMongos());
}

TEST_F(PipelineMustRunOnMongoSTest, UnsplittableMongoSPipelineAssertsIfDisallowedStagePresent) {
    auto expCtx = getExpCtx();

    expCtx->allowDiskUse = true;
    expCtx->inMongos = true;

    auto match = DocumentSourceMatch::create(fromjson("{x: 5}"), expCtx);
    auto runOnMongoS = DocumentSourceMustRunOnMongoS::create();
    auto sort = DocumentSourceSort::create(expCtx, fromjson("{x: 1}"));

    auto pipeline = uassertStatusOK(Pipeline::create({match, runOnMongoS, sort}, expCtx));
    pipeline->optimizePipeline();

    // The entire pipeline must run on mongoS, but $sort cannot do so when 'allowDiskUse' is true.
    ASSERT_THROWS_CODE(
        pipeline->requiredToRunOnMongos(), AssertionException, ErrorCodes::IllegalOperation);
}

DEATH_TEST_F(PipelineMustRunOnMongoSTest,
             SplittablePipelineMustMergeOnMongoSAfterSplit,
             "invariant") {
    auto expCtx = getExpCtx();
    expCtx->inMongos = true;

    auto match = DocumentSourceMatch::create(fromjson("{x: 5}"), expCtx);
    auto split = DocumentSourceInternalSplitPipeline::create(expCtx, HostTypeRequirement::kNone);
    auto runOnMongoS = DocumentSourceMustRunOnMongoS::create();

    auto pipeline = uassertStatusOK(Pipeline::create({match, split, runOnMongoS}, expCtx));

    // We don't need to run the entire pipeline on mongoS because we can split at
    // $_internalSplitPipeline.
    ASSERT_FALSE(pipeline->requiredToRunOnMongos());

    auto shardPipe = pipeline->splitForSharded();
    ASSERT(shardPipe);

    // The merge half of the pipeline must run on mongoS.
    ASSERT_TRUE(pipeline->requiredToRunOnMongos());

    // Calling 'requiredToRunOnMongos' on the shard pipeline will hit an invariant.
    shardPipe->requiredToRunOnMongos();
}

TEST_F(PipelineMustRunOnMongoSTest, SplitMongoSMergePipelineAssertsIfShardStagePresent) {
    auto expCtx = getExpCtx();

    expCtx->allowDiskUse = true;
    expCtx->inMongos = true;

    auto match = DocumentSourceMatch::create(fromjson("{x: 5}"), expCtx);
    auto split = DocumentSourceInternalSplitPipeline::create(expCtx, HostTypeRequirement::kNone);
    auto runOnMongoS = DocumentSourceMustRunOnMongoS::create();
    auto outSpec = fromjson("{$out: 'outcoll'}");
    auto out = DocumentSourceOut::createFromBson(outSpec["$out"], expCtx);

    auto pipeline = uassertStatusOK(Pipeline::create({match, split, runOnMongoS, out}, expCtx));

    // We don't need to run the entire pipeline on mongoS because we can split at
    // $_internalSplitPipeline.
    ASSERT_FALSE(pipeline->requiredToRunOnMongos());

    auto shardPipe = pipeline->splitForSharded();
    ASSERT(shardPipe);

    // The merge pipeline must run on mongoS, but $out needs to run on  the primary shard.
    ASSERT_THROWS_CODE(
        pipeline->requiredToRunOnMongos(), AssertionException, ErrorCodes::IllegalOperation);
}

TEST_F(PipelineMustRunOnMongoSTest, SplittablePipelineAssertsIfMongoSStageOnShardSideOfSplit) {
    auto expCtx = getExpCtx();
    expCtx->inMongos = true;

    auto match = DocumentSourceMatch::create(fromjson("{x: 5}"), expCtx);
    auto runOnMongoS = DocumentSourceMustRunOnMongoS::create();
    auto split =
        DocumentSourceInternalSplitPipeline::create(expCtx, HostTypeRequirement::kAnyShard);

    auto pipeline = uassertStatusOK(Pipeline::create({match, runOnMongoS, split}, expCtx));
    pipeline->optimizePipeline();

    // The 'runOnMongoS' stage comes before any splitpoint, so this entire pipeline must run on
    // mongoS. However, the pipeline *cannot* run on mongoS and *must* split at
    // $_internalSplitPipeline due to the latter's 'anyShard' requirement. The mongoS stage would
    // end up on the shard side of this split, and so it asserts.
    ASSERT_THROWS_CODE(
        pipeline->requiredToRunOnMongos(), AssertionException, ErrorCodes::IllegalOperation);
}

TEST_F(PipelineMustRunOnMongoSTest, SplittablePipelineRunsUnsplitOnMongoSIfSplitpointIsEligible) {
    auto expCtx = getExpCtx();
    expCtx->inMongos = true;

    auto match = DocumentSourceMatch::create(fromjson("{x: 5}"), expCtx);
    auto runOnMongoS = DocumentSourceMustRunOnMongoS::create();
    auto split = DocumentSourceInternalSplitPipeline::create(expCtx, HostTypeRequirement::kNone);

    auto pipeline = uassertStatusOK(Pipeline::create({match, runOnMongoS, split}, expCtx));
    pipeline->optimizePipeline();

    // The 'runOnMongoS' stage is before the splitpoint, so this entire pipeline must run on mongoS.
    // In this case, the splitpoint is itself eligible to run on mongoS, and so we are able to
    // return true.
    ASSERT_TRUE(pipeline->requiredToRunOnMongos());
}

}  // namespace mustRunOnMongoS
}  // namespace Sharded
}  // namespace Optimizations

TEST(PipelineInitialSource, GeoNearInitialQuery) {
    OperationContextNoop _opCtx;
    const std::vector<BSONObj> rawPipeline = {
        fromjson("{$geoNear: {distanceField: 'd', near: [0, 0], query: {a: 1}}}")};
    intrusive_ptr<ExpressionContextForTest> ctx = new ExpressionContextForTest(
        &_opCtx, AggregationRequest(NamespaceString("a.collection"), rawPipeline));
    auto pipe = uassertStatusOK(Pipeline::parse(rawPipeline, ctx));
    ASSERT_BSONOBJ_EQ(pipe->getInitialQuery(), BSON("a" << 1));
}

TEST(PipelineInitialSource, MatchInitialQuery) {
    OperationContextNoop _opCtx;
    const std::vector<BSONObj> rawPipeline = {fromjson("{$match: {'a': 4}}")};
    intrusive_ptr<ExpressionContextForTest> ctx = new ExpressionContextForTest(
        &_opCtx, AggregationRequest(NamespaceString("a.collection"), rawPipeline));

    auto pipe = uassertStatusOK(Pipeline::parse(rawPipeline, ctx));
    ASSERT_BSONOBJ_EQ(pipe->getInitialQuery(), BSON("a" << 4));
}

// Contains test cases for validation done on pipeline creation.
namespace pipeline_validate {

using PipelineValidateTest = AggregationContextFixture;

class DocumentSourceCollectionlessMock : public DocumentSourceMock {
public:
    DocumentSourceCollectionlessMock() : DocumentSourceMock({}) {}

    StageConstraints constraints(Pipeline::SplitState pipeState) const final {
        StageConstraints constraints(StreamType::kStreaming,
                                     PositionRequirement::kFirst,
                                     HostTypeRequirement::kNone,
                                     DiskUseRequirement::kNoDiskUse,
                                     FacetRequirement::kNotAllowed,
                                     TransactionRequirement::kAllowed);
        constraints.isIndependentOfAnyCollection = true;
        constraints.requiresInputDocSource = false;
        return constraints;
    }

    static boost::intrusive_ptr<DocumentSourceCollectionlessMock> create() {
        return new DocumentSourceCollectionlessMock();
    }
};

TEST_F(PipelineValidateTest, AggregateOneNSNotValidForEmptyPipeline) {
    const std::vector<BSONObj> rawPipeline = {};
    auto ctx = getExpCtx();

    ctx->ns = NamespaceString::makeCollectionlessAggregateNSS("a");

    ASSERT_NOT_OK(Pipeline::parse(rawPipeline, ctx).getStatus());
}

TEST_F(PipelineValidateTest, AggregateOneNSNotValidIfInitialStageRequiresCollection) {
    const std::vector<BSONObj> rawPipeline = {fromjson("{$match: {}}")};
    auto ctx = getExpCtx();

    ctx->ns = NamespaceString::makeCollectionlessAggregateNSS("a");

    ASSERT_NOT_OK(Pipeline::parse(rawPipeline, ctx).getStatus());
}

TEST_F(PipelineValidateTest, AggregateOneNSValidIfInitialStageIsCollectionless) {
    auto collectionlessSource = DocumentSourceCollectionlessMock::create();
    auto ctx = getExpCtx();

    ctx->ns = NamespaceString::makeCollectionlessAggregateNSS("a");

    ASSERT_OK(Pipeline::create({collectionlessSource}, ctx).getStatus());
}

TEST_F(PipelineValidateTest, CollectionNSNotValidIfInitialStageIsCollectionless) {
    auto collectionlessSource = DocumentSourceCollectionlessMock::create();
    auto ctx = getExpCtx();

    ctx->ns = kTestNss;

    ASSERT_NOT_OK(Pipeline::create({collectionlessSource}, ctx).getStatus());
}

TEST_F(PipelineValidateTest, AggregateOneNSValidForFacetPipelineRegardlessOfInitialStage) {
    const std::vector<BSONObj> rawPipeline = {fromjson("{$match: {}}")};
    auto ctx = getExpCtx();

    ctx->ns = NamespaceString::makeCollectionlessAggregateNSS("unittests");

    ASSERT_OK(Pipeline::parseFacetPipeline(rawPipeline, ctx).getStatus());
}

TEST_F(PipelineValidateTest, ChangeStreamIsValidAsFirstStage) {
    const std::vector<BSONObj> rawPipeline = {fromjson("{$changeStream: {}}")};
    auto ctx = getExpCtx();
    setMockReplicationCoordinatorOnOpCtx(ctx->opCtx);
    ctx->ns = NamespaceString("a.collection");
    ASSERT_OK(Pipeline::parse(rawPipeline, ctx).getStatus());
}

TEST_F(PipelineValidateTest, ChangeStreamIsNotValidIfNotFirstStage) {
    const std::vector<BSONObj> rawPipeline = {fromjson("{$match: {custom: 'filter'}}"),
                                              fromjson("{$changeStream: {}}")};
    auto ctx = getExpCtx();
    setMockReplicationCoordinatorOnOpCtx(ctx->opCtx);
    ctx->ns = NamespaceString("a.collection");
    auto parseStatus = Pipeline::parse(rawPipeline, ctx).getStatus();
    ASSERT_EQ(parseStatus, ErrorCodes::duplicateCodeForTest(40602));
}

TEST_F(PipelineValidateTest, ChangeStreamIsNotValidIfNotFirstStageInFacet) {
    const std::vector<BSONObj> rawPipeline = {fromjson("{$match: {custom: 'filter'}}"),
                                              fromjson("{$changeStream: {}}")};
    auto ctx = getExpCtx();
    setMockReplicationCoordinatorOnOpCtx(ctx->opCtx);
    ctx->ns = NamespaceString("a.collection");
    auto parseStatus = Pipeline::parseFacetPipeline(rawPipeline, ctx).getStatus();
    ASSERT_EQ(parseStatus, ErrorCodes::duplicateCodeForTest(40600));
    ASSERT(std::string::npos != parseStatus.reason().find("$changeStream"));
}

class DocumentSourceDisallowedInTransactions : public DocumentSourceMock {
public:
    DocumentSourceDisallowedInTransactions() : DocumentSourceMock({}) {}

    StageConstraints constraints(Pipeline::SplitState pipeState) const final {
        return StageConstraints{StreamType::kStreaming,
                                PositionRequirement::kNone,
                                HostTypeRequirement::kNone,
                                DiskUseRequirement::kNoDiskUse,
                                FacetRequirement::kAllowed,
                                TransactionRequirement::kNotAllowed};
    }

    static boost::intrusive_ptr<DocumentSourceDisallowedInTransactions> create() {
        return new DocumentSourceDisallowedInTransactions();
    }
};

TEST_F(PipelineValidateTest, TopLevelPipelineValidatedForStagesIllegalInTransactions) {
    BSONObj readConcernSnapshot = BSON("readConcern" << BSON("level"
                                                             << "snapshot"));
    auto ctx = getExpCtx();
    ctx->inMultiDocumentTransaction = true;

    // Make a pipeline with a legal $match, and then an illegal mock stage, and verify that pipeline
    // creation fails with the expected error code.
    auto matchStage = DocumentSourceMatch::create(BSON("_id" << 3), ctx);
    auto illegalStage = DocumentSourceDisallowedInTransactions::create();
    auto pipeline = Pipeline::create({matchStage, illegalStage}, ctx);
    ASSERT_NOT_OK(pipeline.getStatus());
    ASSERT_EQ(pipeline.getStatus(), ErrorCodes::duplicateCodeForTest(50742));
}

TEST_F(PipelineValidateTest, FacetPipelineValidatedForStagesIllegalInTransactions) {
    BSONObj readConcernSnapshot = BSON("readConcern" << BSON("level"
                                                             << "snapshot"));
    auto ctx = getExpCtx();
    ctx->inMultiDocumentTransaction = true;

    // Make a pipeline with a legal $match, and then an illegal mock stage, and verify that pipeline
    // creation fails with the expected error code.
    auto matchStage = DocumentSourceMatch::create(BSON("_id" << 3), ctx);
    auto illegalStage = DocumentSourceDisallowedInTransactions::create();
    auto pipeline = Pipeline::createFacetPipeline({matchStage, illegalStage}, ctx);
    ASSERT_NOT_OK(pipeline.getStatus());
    ASSERT_EQ(pipeline.getStatus(), ErrorCodes::duplicateCodeForTest(50742));
}

}  // namespace pipeline_validate

namespace Dependencies {

using PipelineDependenciesTest = AggregationContextFixture;

TEST_F(PipelineDependenciesTest, EmptyPipelineShouldRequireWholeDocument) {
    auto pipeline = unittest::assertGet(Pipeline::create({}, getExpCtx()));

    auto depsTracker = pipeline->getDependencies(DepsTracker::MetadataAvailable::kNoMetadata);
    ASSERT_TRUE(depsTracker.needWholeDocument);
    ASSERT_FALSE(depsTracker.getNeedsMetadata(DepsTracker::MetadataType::TEXT_SCORE));

    depsTracker = pipeline->getDependencies(DepsTracker::MetadataAvailable::kTextScore);
    ASSERT_TRUE(depsTracker.needWholeDocument);
    ASSERT_TRUE(depsTracker.getNeedsMetadata(DepsTracker::MetadataType::TEXT_SCORE));
}

//
// Some dummy DocumentSources with different dependencies.
//

// Like a DocumentSourceMock, but can be used anywhere in the pipeline.
class DocumentSourceDependencyDummy : public DocumentSourceMock {
public:
    DocumentSourceDependencyDummy() : DocumentSourceMock({}) {}

    StageConstraints constraints(Pipeline::SplitState pipeState) const final {
        // Overrides DocumentSourceMock's required position.
        return {StreamType::kStreaming,
                PositionRequirement::kNone,
                HostTypeRequirement::kNone,
                DiskUseRequirement::kNoDiskUse,
                FacetRequirement::kAllowed,
                TransactionRequirement::kAllowed};
    }
};

class DocumentSourceDependenciesNotSupported : public DocumentSourceDependencyDummy {
public:
    DepsTracker::State getDependencies(DepsTracker* deps) const final {
        return DepsTracker::State::NOT_SUPPORTED;
    }

    static boost::intrusive_ptr<DocumentSourceDependenciesNotSupported> create() {
        return new DocumentSourceDependenciesNotSupported();
    }
};

class DocumentSourceNeedsASeeNext : public DocumentSourceDependencyDummy {
public:
    DepsTracker::State getDependencies(DepsTracker* deps) const final {
        deps->fields.insert("a");
        return DepsTracker::State::SEE_NEXT;
    }

    static boost::intrusive_ptr<DocumentSourceNeedsASeeNext> create() {
        return new DocumentSourceNeedsASeeNext();
    }
};

class DocumentSourceNeedsOnlyB : public DocumentSourceDependencyDummy {
public:
    DepsTracker::State getDependencies(DepsTracker* deps) const final {
        deps->fields.insert("b");
        return DepsTracker::State::EXHAUSTIVE_FIELDS;
    }

    static boost::intrusive_ptr<DocumentSourceNeedsOnlyB> create() {
        return new DocumentSourceNeedsOnlyB();
    }
};

class DocumentSourceNeedsOnlyTextScore : public DocumentSourceDependencyDummy {
public:
    DepsTracker::State getDependencies(DepsTracker* deps) const final {
        deps->setNeedsMetadata(DepsTracker::MetadataType::TEXT_SCORE, true);
        return DepsTracker::State::EXHAUSTIVE_META;
    }

    static boost::intrusive_ptr<DocumentSourceNeedsOnlyTextScore> create() {
        return new DocumentSourceNeedsOnlyTextScore();
    }
};

class DocumentSourceStripsTextScore : public DocumentSourceDependencyDummy {
public:
    DepsTracker::State getDependencies(DepsTracker* deps) const final {
        return DepsTracker::State::EXHAUSTIVE_META;
    }

    static boost::intrusive_ptr<DocumentSourceStripsTextScore> create() {
        return new DocumentSourceStripsTextScore();
    }
};

TEST_F(PipelineDependenciesTest, ShouldRequireWholeDocumentIfAnyStageDoesNotSupportDeps) {
    auto ctx = getExpCtx();
    auto needsASeeNext = DocumentSourceNeedsASeeNext::create();
    auto notSupported = DocumentSourceDependenciesNotSupported::create();
    auto pipeline = unittest::assertGet(Pipeline::create({needsASeeNext, notSupported}, ctx));

    auto depsTracker = pipeline->getDependencies(DepsTracker::MetadataAvailable::kNoMetadata);
    ASSERT_TRUE(depsTracker.needWholeDocument);
    // The inputs did not have a text score available, so we should not require a text score.
    ASSERT_FALSE(depsTracker.getNeedsMetadata(DepsTracker::MetadataType::TEXT_SCORE));

    // Now in the other order.
    pipeline = unittest::assertGet(Pipeline::create({notSupported, needsASeeNext}, ctx));

    depsTracker = pipeline->getDependencies(DepsTracker::MetadataAvailable::kNoMetadata);
    ASSERT_TRUE(depsTracker.needWholeDocument);
}

TEST_F(PipelineDependenciesTest, ShouldRequireWholeDocumentIfNoStageReturnsExhaustiveFields) {
    auto ctx = getExpCtx();
    auto needsASeeNext = DocumentSourceNeedsASeeNext::create();
    auto pipeline = unittest::assertGet(Pipeline::create({needsASeeNext}, ctx));

    auto depsTracker = pipeline->getDependencies(DepsTracker::MetadataAvailable::kNoMetadata);
    ASSERT_TRUE(depsTracker.needWholeDocument);
}

TEST_F(PipelineDependenciesTest, ShouldNotRequireWholeDocumentIfAnyStageReturnsExhaustiveFields) {
    auto ctx = getExpCtx();
    auto needsASeeNext = DocumentSourceNeedsASeeNext::create();
    auto needsOnlyB = DocumentSourceNeedsOnlyB::create();
    auto pipeline = unittest::assertGet(Pipeline::create({needsASeeNext, needsOnlyB}, ctx));

    auto depsTracker = pipeline->getDependencies(DepsTracker::MetadataAvailable::kNoMetadata);
    ASSERT_FALSE(depsTracker.needWholeDocument);
    ASSERT_EQ(depsTracker.fields.size(), 2UL);
    ASSERT_EQ(depsTracker.fields.count("a"), 1UL);
    ASSERT_EQ(depsTracker.fields.count("b"), 1UL);
}

TEST_F(PipelineDependenciesTest, ShouldNotAddAnyRequiredFieldsAfterFirstStageWithExhaustiveFields) {
    auto ctx = getExpCtx();
    auto needsOnlyB = DocumentSourceNeedsOnlyB::create();
    auto needsASeeNext = DocumentSourceNeedsASeeNext::create();
    auto pipeline = unittest::assertGet(Pipeline::create({needsOnlyB, needsASeeNext}, ctx));

    auto depsTracker = pipeline->getDependencies(DepsTracker::MetadataAvailable::kNoMetadata);
    ASSERT_FALSE(depsTracker.needWholeDocument);
    ASSERT_FALSE(depsTracker.getNeedsMetadata(DepsTracker::MetadataType::TEXT_SCORE));

    // 'needsOnlyB' claims to know all its field dependencies, so we shouldn't add any from
    // 'needsASeeNext'.
    ASSERT_EQ(depsTracker.fields.size(), 1UL);
    ASSERT_EQ(depsTracker.fields.count("b"), 1UL);
}

TEST_F(PipelineDependenciesTest, ShouldNotRequireTextScoreIfThereIsNoScoreAvailable) {
    auto ctx = getExpCtx();
    auto pipeline = unittest::assertGet(Pipeline::create({}, ctx));

    auto depsTracker = pipeline->getDependencies(DepsTracker::MetadataAvailable::kNoMetadata);
    ASSERT_FALSE(depsTracker.getNeedsMetadata(DepsTracker::MetadataType::TEXT_SCORE));
}

TEST_F(PipelineDependenciesTest, ShouldThrowIfTextScoreIsNeededButNotPresent) {
    auto ctx = getExpCtx();
    auto needsText = DocumentSourceNeedsOnlyTextScore::create();
    auto pipeline = unittest::assertGet(Pipeline::create({needsText}, ctx));

    ASSERT_THROWS(pipeline->getDependencies(DepsTracker::MetadataAvailable::kNoMetadata),
                  AssertionException);
}

TEST_F(PipelineDependenciesTest, ShouldRequireTextScoreIfAvailableAndNoStageReturnsExhaustiveMeta) {
    auto ctx = getExpCtx();
    auto pipeline = unittest::assertGet(Pipeline::create({}, ctx));

    auto depsTracker = pipeline->getDependencies(DepsTracker::MetadataAvailable::kTextScore);
    ASSERT_TRUE(depsTracker.getNeedsMetadata(DepsTracker::MetadataType::TEXT_SCORE));

    auto needsASeeNext = DocumentSourceNeedsASeeNext::create();
    pipeline = unittest::assertGet(Pipeline::create({needsASeeNext}, ctx));
    depsTracker = pipeline->getDependencies(DepsTracker::MetadataAvailable::kTextScore);
    ASSERT_TRUE(depsTracker.getNeedsMetadata(DepsTracker::MetadataType::TEXT_SCORE));
}

TEST_F(PipelineDependenciesTest, ShouldNotRequireTextScoreIfAvailableButDefinitelyNotNeeded) {
    auto ctx = getExpCtx();
    auto stripsTextScore = DocumentSourceStripsTextScore::create();
    auto needsText = DocumentSourceNeedsOnlyTextScore::create();
    auto pipeline = unittest::assertGet(Pipeline::create({stripsTextScore, needsText}, ctx));

    auto depsTracker = pipeline->getDependencies(DepsTracker::MetadataAvailable::kTextScore);

    // 'stripsTextScore' claims that no further stage will need metadata information, so we
    // shouldn't have the text score as a dependency.
    ASSERT_FALSE(depsTracker.getNeedsMetadata(DepsTracker::MetadataType::TEXT_SCORE));
}

}  // namespace Dependencies

class All : public Suite {
public:
    All() : Suite("PipelineOptimizations") {}
    void setupTests() {
        add<Optimizations::Sharded::Empty>();
        add<Optimizations::Sharded::coalesceLookUpAndUnwind::ShouldCoalesceUnwindOnAs>();
        add<Optimizations::Sharded::coalesceLookUpAndUnwind::
                ShouldCoalesceUnwindOnAsWithPreserveEmpty>();
        add<Optimizations::Sharded::coalesceLookUpAndUnwind::
                ShouldCoalesceUnwindOnAsWithIncludeArrayIndex>();
        add<Optimizations::Sharded::coalesceLookUpAndUnwind::ShouldNotCoalesceUnwindNotOnAs>();
        add<Optimizations::Sharded::moveFinalUnwindFromShardsToMerger::OneUnwind>();
        add<Optimizations::Sharded::moveFinalUnwindFromShardsToMerger::TwoUnwind>();
        add<Optimizations::Sharded::moveFinalUnwindFromShardsToMerger::UnwindNotFinal>();
        add<Optimizations::Sharded::moveFinalUnwindFromShardsToMerger::UnwindWithOther>();
        add<Optimizations::Sharded::limitFieldsSentFromShardsToMerger::NeedWholeDoc>();
        add<Optimizations::Sharded::limitFieldsSentFromShardsToMerger::JustNeedsId>();
        add<Optimizations::Sharded::limitFieldsSentFromShardsToMerger::JustNeedsNonId>();
        add<Optimizations::Sharded::limitFieldsSentFromShardsToMerger::NothingNeeded>();
        add<Optimizations::Sharded::limitFieldsSentFromShardsToMerger::ShardAlreadyExhaustive>();
        add<Optimizations::Sharded::limitFieldsSentFromShardsToMerger::
                ShardedSortMatchProjSkipLimBecomesMatchTopKSortSkipProj>();
        add<Optimizations::Sharded::limitFieldsSentFromShardsToMerger::
                ShardedMatchProjLimDoesNotBecomeMatchLimProj>();
        add<Optimizations::Sharded::limitFieldsSentFromShardsToMerger::
                ShardedSortProjLimBecomesTopKSortProj>();
        add<Optimizations::Sharded::limitFieldsSentFromShardsToMerger::
                ShardedSortGroupProjLimDoesNotBecomeTopKSortProjGroup>();
        add<Optimizations::Sharded::limitFieldsSentFromShardsToMerger::
                ShardedMatchSortProjLimBecomesMatchTopKSortProj>();
        add<Optimizations::Sharded::limitFieldsSentFromShardsToMerger::ShardAlreadyExhaustive>();
        add<Optimizations::Sharded::needsPrimaryShardMerger::Out>();
        add<Optimizations::Sharded::needsPrimaryShardMerger::Project>();
        add<Optimizations::Sharded::needsPrimaryShardMerger::LookUp>();
    }
};

SuiteInstance<All> myall;

}  // namespace
}  // namespace mongo
