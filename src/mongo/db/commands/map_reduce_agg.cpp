/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#include <boost/intrusive_ptr.hpp>
#include <boost/optional.hpp>
#include <cstdint>
#include <string>
#include <utility>

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/map_reduce_agg.h"
#include "mongo/db/commands/map_reduce_javascript_code.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_group.h"
#include "mongo/db/pipeline/document_source_limit.h"
#include "mongo/db/pipeline/document_source_match.h"
#include "mongo/db/pipeline/document_source_merge.h"
#include "mongo/db/pipeline/document_source_out.h"
#include "mongo/db/pipeline/document_source_project.h"
#include "mongo/db/pipeline/document_source_single_document_transformation.h"
#include "mongo/db/pipeline/document_source_sort.h"
#include "mongo/db/pipeline/document_source_unwind.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/expression_javascript.h"
#include "mongo/db/pipeline/parsed_aggregation_projection_node.h"
#include "mongo/db/pipeline/parsed_inclusion_projection.h"
#include "mongo/db/pipeline/pipeline_d.h"
#include "mongo/db/query/map_reduce_output_format.h"
#include "mongo/db/query/util/make_data_structure.h"
#include "mongo/util/intrusive_counter.h"

namespace mongo::map_reduce_agg {

namespace {

using namespace std::string_literals;

auto translateSort(boost::intrusive_ptr<ExpressionContext> expCtx,
                   const BSONObj& sort,
                   const boost::optional<std::int64_t>& limit) {
    return DocumentSourceSort::create(expCtx, sort, limit.get_value_or(-1));
}

auto translateMap(boost::intrusive_ptr<ExpressionContext> expCtx, std::string code) {
    auto emitExpression = ExpressionInternalJsEmit::create(
        expCtx, ExpressionFieldPath::parse(expCtx, "$$ROOT", expCtx->variablesParseState), code);
    auto node = std::make_unique<parsed_aggregation_projection::InclusionNode>(
        ProjectionPolicies{ProjectionPolicies::DefaultIdPolicy::kExcludeId});
    node->addExpressionForPath(FieldPath{"emits"s}, std::move(emitExpression));
    auto inclusion = std::unique_ptr<TransformerInterface>{
        std::make_unique<parsed_aggregation_projection::ParsedInclusionProjection>(
            expCtx,
            ProjectionPolicies{ProjectionPolicies::DefaultIdPolicy::kExcludeId},
            std::move(node))};
    return make_intrusive<DocumentSourceSingleDocumentTransformation>(
        expCtx, std::move(inclusion), DocumentSourceProject::kStageName, false);
}

auto translateReduce(boost::intrusive_ptr<ExpressionContext> expCtx, std::string code) {
    auto accumulatorArguments = ExpressionObject::create(
        expCtx,
        make_vector<std::pair<std::string, boost::intrusive_ptr<Expression>>>(
            std::pair{"data"s,
                      ExpressionFieldPath::parse(expCtx, "$emits", expCtx->variablesParseState)},
            std::pair{"eval"s, ExpressionConstant::create(expCtx, Value{code})}));
    auto jsReduce = AccumulationStatement{
        "value",
        std::move(accumulatorArguments),
        AccumulationStatement::getFactory(AccumulatorInternalJsReduce::kAccumulatorName)};
    auto groupExpr = ExpressionFieldPath::parse(expCtx, "$emits.k", expCtx->variablesParseState);
    return DocumentSourceGroup::create(expCtx,
                                       std::move(groupExpr),
                                       make_vector<AccumulationStatement>(std::move(jsReduce)),
                                       boost::none);
}

auto translateFinalize(boost::intrusive_ptr<ExpressionContext> expCtx, std::string code) {
    auto jsExpression = ExpressionInternalJs::create(
        expCtx,
        ExpressionArray::create(
            expCtx,
            make_vector<boost::intrusive_ptr<Expression>>(
                ExpressionFieldPath::parse(expCtx, "$_id", expCtx->variablesParseState),
                ExpressionFieldPath::parse(expCtx, "$value", expCtx->variablesParseState))),
        code);
    auto node = std::make_unique<parsed_aggregation_projection::InclusionNode>(
        ProjectionPolicies{ProjectionPolicies::DefaultIdPolicy::kExcludeId});
    node->addExpressionForPath(FieldPath{"value"s}, std::move(jsExpression));
    auto inclusion = std::unique_ptr<TransformerInterface>{
        std::make_unique<parsed_aggregation_projection::ParsedInclusionProjection>(
            expCtx,
            ProjectionPolicies{ProjectionPolicies::DefaultIdPolicy::kExcludeId},
            std::move(node))};
    return make_intrusive<DocumentSourceSingleDocumentTransformation>(
        expCtx, std::move(inclusion), DocumentSourceProject::kStageName, false);
}

auto translateOutReplace(boost::intrusive_ptr<ExpressionContext> expCtx,
                         const StringData inputDatabase,
                         NamespaceString targetNss) {
    uassert(31278,
            "MapReduce must output to the database belonging to its input collection - Input: "s +
                inputDatabase + "Output: " + targetNss.db(),
            inputDatabase == targetNss.db());
    return DocumentSourceOut::create(std::move(targetNss), expCtx);
}

auto translateOutMerge(boost::intrusive_ptr<ExpressionContext> expCtx, NamespaceString targetNss) {
    return DocumentSourceMerge::create(targetNss,
                                       expCtx,
                                       MergeWhenMatchedModeEnum::kReplace,
                                       MergeWhenNotMatchedModeEnum::kInsert,
                                       boost::none,  // Let variables
                                       boost::none,  // pipeline
                                       std::set<FieldPath>{FieldPath("_id"s)},
                                       boost::none);  // targetCollectionVersion
}

auto translateOutReduce(boost::intrusive_ptr<ExpressionContext> expCtx,
                        NamespaceString targetNss,
                        std::string code) {
    // Because of communication for sharding, $merge must hold on to a serializable BSON object
    // at the moment so we reparse here.
    auto reduceObj = BSON("args" << BSON_ARRAY("$value"
                                               << "$$new.value")
                                 << "eval" << code);

    auto finalProjectSpec =
        BSON(DocumentSourceProject::kStageName
             << BSON("value" << BSON(ExpressionInternalJs::kExpressionName << reduceObj)));
    auto pipelineSpec = boost::make_optional(std::vector<BSONObj>{finalProjectSpec});
    return DocumentSourceMerge::create(targetNss,
                                       expCtx,
                                       MergeWhenMatchedModeEnum::kPipeline,
                                       MergeWhenNotMatchedModeEnum::kInsert,
                                       boost::none,  // Let variables
                                       pipelineSpec,
                                       std::set<FieldPath>{FieldPath("_id"s)},
                                       boost::none);  // targetCollectionVersion
}

auto translateOut(boost::intrusive_ptr<ExpressionContext> expCtx,
                  const OutputType outputType,
                  const StringData inputDatabase,
                  NamespaceString targetNss,
                  std::string reduceCode) {
    switch (outputType) {
        case OutputType::Replace:
            return boost::make_optional(translateOutReplace(expCtx, inputDatabase, targetNss));
        case OutputType::Merge:
            return boost::make_optional(translateOutMerge(expCtx, targetNss));
        case OutputType::Reduce:
            return boost::make_optional(translateOutReduce(expCtx, targetNss, reduceCode));
        case OutputType::InMemory:;
    }
    return boost::optional<boost::intrusive_ptr<mongo::DocumentSource>>{};
}

auto makeExpressionContext(OperationContext* opCtx, const MapReduce& parsedMr) {
    // AutoGetCollectionForReadCommand will throw if the sharding version for this connection is
    // out of date.
    AutoGetCollectionForReadCommand ctx(
        opCtx, parsedMr.getNamespace(), AutoGetCollection::ViewMode::kViewsPermitted);
    uassert(ErrorCodes::CommandNotSupportedOnView,
            "mapReduce on a view is not yet supported",
            !ctx.getView());

    auto resolvedCollator = PipelineD::resolveCollator(
        opCtx, parsedMr.getCollation().get_value_or(BSONObj()), ctx.getCollection());

    // The UUID of the collection for the execution namespace of this aggregation.
    auto uuid =
        ctx.getCollection() ? boost::make_optional(ctx.getCollection()->uuid()) : boost::none;

    auto runtimeConstants = Variables::generateRuntimeConstants(opCtx);
    if (parsedMr.getScope()) {
        runtimeConstants.setJsScope(parsedMr.getScope()->getObj());
    }

    // Manually build an ExpressionContext with the desired options for the translated
    // aggregation. The one option worth noting here is allowDiskUse, which is required to allow
    // the $group stage of the translated pipeline to spill to disk.
    return make_intrusive<ExpressionContext>(
        opCtx,
        boost::none,  // explain
        false,        // fromMongos
        false,        // needsmerge
        true,         // allowDiskUse
        parsedMr.getBypassDocumentValidation().get_value_or(false),
        parsedMr.getNamespace(),
        parsedMr.getCollation().get_value_or(BSONObj()),
        runtimeConstants,
        std::move(resolvedCollator),
        MongoProcessInterface::create(opCtx),
        StringMap<ExpressionContext::ResolvedNamespace>{},  // resolvedNamespaces
        uuid);
}

}  // namespace

bool runAggregationMapReduce(OperationContext* opCtx,
                             const std::string& dbname,
                             const BSONObj& cmd,
                             std::string& errmsg,
                             BSONObjBuilder& result) {
    auto exhaustPipelineIntoBSONArray = [](auto&& pipeline) {
        BSONArrayBuilder bab;
        while (auto&& doc = pipeline->getNext())
            bab.append(doc->toBson());
        return bab.arr();
    };

    auto parsedMr = MapReduce::parse(IDLParserErrorContext("MapReduce"), cmd);
    auto expCtx = makeExpressionContext(opCtx, parsedMr);
    auto runnablePipeline = [&]() {
        auto pipeline = translateFromMR(parsedMr, expCtx);
        return expCtx->mongoProcessInterface->attachCursorSourceToPipelineForLocalRead(
            expCtx, pipeline.release());
    }();

    if (parsedMr.getOutOptions().getOutputType() == OutputType::InMemory) {
        map_reduce_output_format::appendInlineResponse(
            exhaustPipelineIntoBSONArray(runnablePipeline),
            boost::get_optional_value_or(parsedMr.getVerbose(), false),
            false,
            &result);
    } else {
        // For non-inline output, the pipeline should not return any results however getNext() still
        // needs to be called once to ensure documents are written to the output collection.
        invariant(!runnablePipeline->getNext());

        map_reduce_output_format::appendOutResponse(
            parsedMr.getOutOptions().getDatabaseName(),
            parsedMr.getOutOptions().getCollectionName(),
            boost::get_optional_value_or(parsedMr.getVerbose(), false),
            false,
            &result);
    }

    return true;
}

std::unique_ptr<Pipeline, PipelineDeleter> translateFromMR(
    MapReduce parsedMr, boost::intrusive_ptr<ExpressionContext> expCtx) {

    // TODO: It would be good to figure out what kind of errors this would produce in the Status.
    // It would be better not to produce something incomprehensible out of an internal translation.
    return uassertStatusOK(Pipeline::create(
        makeFlattenedList<boost::intrusive_ptr<DocumentSource>>(
            parsedMr.getQuery().map(
                [&](auto&& query) { return DocumentSourceMatch::create(query, expCtx); }),
            parsedMr.getSort().map(
                [&](auto&& sort) { return translateSort(expCtx, sort, parsedMr.getLimit()); }),
            translateMap(expCtx, parsedMr.getMap().getCode()),
            DocumentSourceUnwind::create(expCtx, "emits", false, boost::none),
            translateReduce(expCtx, parsedMr.getReduce().getCode()),
            parsedMr.getFinalize().map([&](auto&& finalize) {
                return translateFinalize(expCtx, parsedMr.getFinalize()->getCode());
            }),
            translateOut(expCtx,
                         parsedMr.getOutOptions().getOutputType(),
                         parsedMr.getNamespace().db(),
                         NamespaceString{parsedMr.getOutOptions().getDatabaseName()
                                             ? *parsedMr.getOutOptions().getDatabaseName()
                                             : parsedMr.getNamespace().db(),
                                         parsedMr.getOutOptions().getCollectionName()},
                         parsedMr.getReduce().getCode())),
        expCtx));
}

}  // namespace mongo::map_reduce_agg
