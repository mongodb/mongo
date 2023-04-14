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

#include "mongo/db/commands/mr_common.h"

#include <string>
#include <vector>

#include "mongo/db/auth/action_set.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/catalog/document_validation.h"
#include "mongo/db/commands.h"
#include "mongo/db/exec/inclusion_projection_executor.h"
#include "mongo/db/exec/projection_node.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/pipeline/accumulator_js_reduce.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_group.h"
#include "mongo/db/pipeline/document_source_match.h"
#include "mongo/db/pipeline/document_source_merge.h"
#include "mongo/db/pipeline/document_source_out.h"
#include "mongo/db/pipeline/document_source_project.h"
#include "mongo/db/pipeline/document_source_single_document_transformation.h"
#include "mongo/db/pipeline/document_source_sort.h"
#include "mongo/db/pipeline/document_source_unwind.h"
#include "mongo/db/pipeline/expression_function.h"
#include "mongo/db/pipeline/expression_js_emit.h"
#include "mongo/db/query/util/make_data_structure.h"
#include "mongo/util/intrusive_counter.h"
#include "mongo/util/str.h"

namespace mongo::map_reduce_common {

namespace {

using namespace std::string_literals;

Status interpretTranslationError(DBException* ex, const MapReduceCommandRequest& parsedMr) {
    auto status = ex->toStatus();
    auto outOptions = parsedMr.getOutOptions();
    auto outNss = NamespaceString{outOptions.getDatabaseName() ? *outOptions.getDatabaseName()
                                                               : parsedMr.getNamespace().db(),
                                  outOptions.getCollectionName()};
    std::string error;
    switch (static_cast<int>(ex->code())) {
        case ErrorCodes::InvalidNamespace:
            error = "Invalid output namespace {} for MapReduce"_format(outNss.ns());
            break;
        case 15976:
            error = "The mapReduce sort option must have at least one sort key";
            break;
        case 15958:
            error = "The limit specified to mapReduce must be positive";
            break;
        case 17017:
            error =
                "Cannot run mapReduce against an existing *sharded* output collection when using "
                "the replace action";
            break;
        case 17385:
        case 31319:
            error = "Can't output mapReduce results to special collection {}"_format(outNss.coll());
            break;
        case 31320:
        case 31321:
            error = "Can't output mapReduce results to internal DB {}"_format(
                outNss.dbName().toStringForErrorMsg());
            break;
        default:
            // Prepend MapReduce context in the event of an unknown exception.
            ex->addContext("MapReduce internal error");
            throw;
    }
    return status.withReason(std::move(error));
}

auto translateSort(boost::intrusive_ptr<ExpressionContext> expCtx, const BSONObj& sort) {
    return DocumentSourceSort::create(expCtx, sort);
}

auto translateMap(boost::intrusive_ptr<ExpressionContext> expCtx, std::string code) {
    auto emitExpression = ExpressionInternalJsEmit::create(
        expCtx.get(),
        ExpressionFieldPath::parse(expCtx.get(), "$$ROOT", expCtx->variablesParseState),
        code);
    auto node = std::make_unique<projection_executor::InclusionNode>(
        ProjectionPolicies{ProjectionPolicies::DefaultIdPolicy::kExcludeId});
    node->addExpressionForPath(FieldPath{"emits"s}, std::move(emitExpression));
    auto inclusion = std::unique_ptr<TransformerInterface>{
        std::make_unique<projection_executor::InclusionProjectionExecutor>(
            expCtx,
            ProjectionPolicies{ProjectionPolicies::DefaultIdPolicy::kExcludeId},
            std::move(node))};
    return make_intrusive<DocumentSourceSingleDocumentTransformation>(
        expCtx, std::move(inclusion), DocumentSourceProject::kStageName, false);
}

auto translateReduce(boost::intrusive_ptr<ExpressionContext> expCtx, std::string code) {
    auto initializer = ExpressionArray::create(expCtx.get(), {});
    auto argument = ExpressionFieldPath::parse(expCtx.get(), "$emits", expCtx->variablesParseState);
    auto reduceFactory = [expCtx, funcSource = std::move(code)]() {
        return AccumulatorInternalJsReduce::create(expCtx.get(), funcSource);
    };
    AccumulationStatement jsReduce("value",
                                   AccumulationExpression(std::move(initializer),
                                                          std::move(argument),
                                                          std::move(reduceFactory),
                                                          AccumulatorInternalJsReduce::kName));
    auto groupKeyExpression =
        ExpressionFieldPath::parse(expCtx.get(), "$emits.k", expCtx->variablesParseState);
    return DocumentSourceGroup::create(expCtx,
                                       std::move(groupKeyExpression),
                                       makeVector<AccumulationStatement>(std::move(jsReduce)),
                                       boost::none);
}

auto translateFinalize(boost::intrusive_ptr<ExpressionContext> expCtx,
                       MapReduceJavascriptCodeOrNull codeObj) {
    return codeObj.getCode().map([&](auto&& code) {
        auto jsExpression = ExpressionFunction::create(
            expCtx.get(),
            ExpressionArray::create(
                expCtx.get(),
                makeVector<boost::intrusive_ptr<Expression>>(
                    ExpressionFieldPath::parse(expCtx.get(), "$_id", expCtx->variablesParseState),
                    ExpressionFieldPath::parse(
                        expCtx.get(), "$value", expCtx->variablesParseState))),
            code,
            ExpressionFunction::kJavaScript);
        auto node = std::make_unique<projection_executor::InclusionNode>(
            ProjectionPolicies{ProjectionPolicies::DefaultIdPolicy::kIncludeId});
        node->addProjectionForPath(FieldPath{"_id"s});
        node->addExpressionForPath(FieldPath{"value"s}, std::move(jsExpression));
        auto inclusion = std::unique_ptr<TransformerInterface>{
            std::make_unique<projection_executor::InclusionProjectionExecutor>(
                expCtx,
                ProjectionPolicies{ProjectionPolicies::DefaultIdPolicy::kIncludeId},
                std::move(node))};
        return make_intrusive<DocumentSourceSingleDocumentTransformation>(
            expCtx, std::move(inclusion), DocumentSourceProject::kStageName, false);
    });
}

auto translateOutReplace(boost::intrusive_ptr<ExpressionContext> expCtx,
                         NamespaceString targetNss) {
    return DocumentSourceOut::create(std::move(targetNss), expCtx);
}

auto translateOutMerge(boost::intrusive_ptr<ExpressionContext> expCtx,
                       NamespaceString targetNss,
                       boost::optional<ChunkVersion> targetCollectionPlacementVersion) {
    return DocumentSourceMerge::create(std::move(targetNss),
                                       expCtx,
                                       MergeWhenMatchedModeEnum::kReplace,
                                       MergeWhenNotMatchedModeEnum::kInsert,
                                       boost::none,  // Let variables
                                       boost::none,  // pipeline
                                       std::set<FieldPath>{FieldPath("_id"s)},
                                       std::move(targetCollectionPlacementVersion));
}

auto translateOutReduce(boost::intrusive_ptr<ExpressionContext> expCtx,
                        NamespaceString targetNss,
                        boost::optional<ChunkVersion> targetCollectionPlacementVersion,
                        std::string reduceCode,
                        boost::optional<MapReduceJavascriptCodeOrNull> finalizeCode) {
    // Because of communication for sharding, $merge must hold on to a serializable BSON object
    // at the moment so we reparse here. Note that the reduce function signature expects 2
    // arguments, the first being the key and the second being the array of values to reduce.
    auto reduceObj =
        BSON("args" << BSON_ARRAY("$_id" << BSON_ARRAY("$value"
                                                       << "$$new.value"))
                    << "body" << reduceCode << "lang" << ExpressionFunction::kJavaScript);

    auto reduceSpec = BSON(DocumentSourceProject::kStageName << BSON(
                               "value" << BSON(ExpressionFunction::kExpressionName << reduceObj)));
    auto pipelineSpec = boost::make_optional(std::vector<BSONObj>{reduceSpec});

    // Build finalize $project stage if given.
    if (finalizeCode && finalizeCode->hasCode()) {
        auto finalizeObj = BSON("args" << BSON_ARRAY("$_id"
                                                     << "$value")
                                       << "body" << finalizeCode->getCode().value() << "lang"
                                       << ExpressionFunction::kJavaScript);
        auto finalizeSpec =
            BSON(DocumentSourceProject::kStageName
                 << BSON("value" << BSON(ExpressionFunction::kExpressionName << finalizeObj)));
        pipelineSpec->emplace_back(std::move(finalizeSpec));
    }

    return DocumentSourceMerge::create(std::move(targetNss),
                                       expCtx,
                                       MergeWhenMatchedModeEnum::kPipeline,
                                       MergeWhenNotMatchedModeEnum::kInsert,
                                       boost::none,  // Let variables
                                       pipelineSpec,
                                       std::set<FieldPath>{FieldPath("_id"s)},
                                       std::move(targetCollectionPlacementVersion));
}

void rejectRequestsToCreateShardedCollections(
    const MapReduceOutOptions& outOptions,
    const boost::optional<ChunkVersion>& targetCollectionPlacementVersion) {
    uassert(ErrorCodes::InvalidOptions,
            "Combination of 'out.sharded' and 'replace' output mode is not supported. Cannot "
            "replace an existing sharded collection or create a new sharded collection. Please "
            "create the sharded collection first and use a different output mode or consider using "
            "an unsharded collection.",
            !(outOptions.getOutputType() == OutputType::Replace && outOptions.isSharded()));
    uassert(ErrorCodes::InvalidOptions,
            "Cannot use mapReduce to create a new sharded collection. Please create and shard the"
            " target collection before proceeding.",
            targetCollectionPlacementVersion || !outOptions.isSharded());
}

auto translateOut(boost::intrusive_ptr<ExpressionContext> expCtx,
                  const MapReduceOutOptions& outOptions,
                  NamespaceString targetNss,
                  boost::optional<ChunkVersion> targetCollectionPlacementVersion,
                  std::string reduceCode,
                  boost::optional<MapReduceJavascriptCodeOrNull> finalizeCode) {
    rejectRequestsToCreateShardedCollections(outOptions, targetCollectionPlacementVersion);

    switch (outOptions.getOutputType()) {
        case OutputType::Replace:
            return boost::make_optional(translateOutReplace(expCtx, std::move(targetNss)));
        case OutputType::Merge:
            return boost::make_optional(translateOutMerge(
                expCtx, std::move(targetNss), std::move(targetCollectionPlacementVersion)));
        case OutputType::Reduce:
            return boost::make_optional(
                translateOutReduce(expCtx,
                                   std::move(targetNss),
                                   std::move(targetCollectionPlacementVersion),
                                   std::move(reduceCode),
                                   std::move(finalizeCode)));
        case OutputType::InMemory:;
    }
    return boost::optional<boost::intrusive_ptr<mongo::DocumentSource>>{};
}

}  // namespace

OutputOptions parseOutputOptions(const std::string& dbname, const BSONObj& cmdObj) {
    OutputOptions outputOptions;

    outputOptions.outNonAtomic = true;
    if (cmdObj["out"].type() == String) {
        outputOptions.collectionName = cmdObj["out"].String();
        outputOptions.outType = OutputType::Replace;
    } else if (cmdObj["out"].type() == Object) {
        BSONObj o = cmdObj["out"].embeddedObject();

        if (o.hasElement("normal")) {
            outputOptions.outType = OutputType::Replace;
            outputOptions.collectionName = o["normal"].String();
        } else if (o.hasElement("replace")) {
            outputOptions.outType = OutputType::Replace;
            outputOptions.collectionName = o["replace"].String();
        } else if (o.hasElement("merge")) {
            outputOptions.outType = OutputType::Merge;
            outputOptions.collectionName = o["merge"].String();
        } else if (o.hasElement("reduce")) {
            outputOptions.outType = OutputType::Reduce;
            outputOptions.collectionName = o["reduce"].String();
        } else if (o.hasElement("inline")) {
            outputOptions.outType = OutputType::InMemory;
            uassert(ErrorCodes::InvalidOptions,
                    "cannot specify 'sharded' in combination with 'inline'",
                    !o.hasElement("sharded"));
        } else {
            uasserted(13522,
                      str::stream() << "please specify one of "
                                    << "[replace|merge|reduce|inline] in 'out' object");
        }

        if (o.hasElement("db")) {
            outputOptions.outDB = o["db"].String();
            uassert(ErrorCodes::CommandNotSupported,
                    "cannot target internal database as output",
                    !(NamespaceString(outputOptions.outDB, outputOptions.collectionName)
                          .isOnInternalDb()));
        }
        if (o.hasElement("nonAtomic")) {
            uassert(
                15895,
                str::stream()
                    << "The nonAtomic:false option is no longer allowed in the mapReduce command. "
                    << "Please omit or specify nonAtomic:true",
                o["nonAtomic"].Bool());
        }
    } else {
        uasserted(13606, "'out' has to be a string or an object");
    }

    if (outputOptions.outType != OutputType::InMemory) {
        const StringData outDb(outputOptions.outDB.empty() ? dbname : outputOptions.outDB);
        const NamespaceString nss(outDb, outputOptions.collectionName);
        uassert(ErrorCodes::InvalidNamespace,
                str::stream() << "Invalid 'out' namespace: " << nss.ns(),
                nss.isValid());
        outputOptions.finalNamespace = std::move(nss);
    }

    return outputOptions;
}

Status checkAuthForMapReduce(const BasicCommand* commandTemplate,
                             OperationContext* opCtx,
                             const DatabaseName& dbName,
                             const BSONObj& cmdObj) {
    OutputOptions outputOptions = parseOutputOptions(dbName.db(), cmdObj);

    ResourcePattern inputResource(commandTemplate->parseResourcePattern(dbName, cmdObj));

    auto mapReduceField = cmdObj.firstElement();
    const auto emptyNss =
        mapReduceField.type() == mongo::String && mapReduceField.valueStringData().empty();
    uassert(ErrorCodes::InvalidNamespace,
            str::stream() << "Invalid input namespace " << inputResource.databaseToMatch() << "."
                          << (emptyNss ? "" : cmdObj["mapReduce"].String()),
            !emptyNss && inputResource.isExactNamespacePattern());

    auto* as = AuthorizationSession::get(opCtx->getClient());
    if (!as->isAuthorizedForActionsOnResource(inputResource, ActionType::find)) {
        return {ErrorCodes::Unauthorized, "unauthorized"};
    }

    if (outputOptions.outType != OutputType::InMemory) {
        ActionSet outputActions;
        outputActions.addAction(ActionType::insert);
        if (outputOptions.outType == OutputType::Replace) {
            outputActions.addAction(ActionType::remove);
        } else {
            outputActions.addAction(ActionType::update);
        }

        if (shouldBypassDocumentValidationForCommand(cmdObj)) {
            outputActions.addAction(ActionType::bypassDocumentValidation);
        }

        ResourcePattern outputResource(
            ResourcePattern::forExactNamespace(NamespaceString(outputOptions.finalNamespace)));
        uassert(ErrorCodes::InvalidNamespace,
                str::stream() << "Invalid target namespace " << outputResource.ns().ns(),
                outputResource.ns().isValid());

        // TODO: check if outputNs exists and add createCollection privilege if not
        if (!as->isAuthorizedForActionsOnResource(outputResource, outputActions)) {
            return {ErrorCodes::Unauthorized, "unauthorized"};
        }
    }

    return Status::OK();
}

bool mrSupportsWriteConcern(const BSONObj& cmd) {
    if (!cmd.hasField("out")) {
        return false;
    } else if (cmd["out"].type() == Object && cmd["out"].Obj().hasField("inline")) {
        return false;
    } else {
        return true;
    }
}

std::unique_ptr<Pipeline, PipelineDeleter> translateFromMR(
    MapReduceCommandRequest parsedMr, boost::intrusive_ptr<ExpressionContext> expCtx) {
    auto outNss = NamespaceString{parsedMr.getOutOptions().getDatabaseName()
                                      ? *parsedMr.getOutOptions().getDatabaseName()
                                      : parsedMr.getNamespace().db(),
                                  parsedMr.getOutOptions().getCollectionName()};

    std::set<FieldPath> shardKey;
    boost::optional<ChunkVersion> targetCollectionPlacementVersion;
    // If non-inline output, verify that the target collection is *not* sharded by anything other
    // than _id.
    if (parsedMr.getOutOptions().getOutputType() != OutputType::InMemory) {
        std::tie(shardKey, targetCollectionPlacementVersion) =
            expCtx->mongoProcessInterface->ensureFieldsUniqueOrResolveDocumentKey(
                expCtx, boost::none, boost::none, outNss);
        uassert(31313,
                "The mapReduce target collection must either be unsharded or sharded by {_id: 1} "
                "or {_id: 'hashed'}",
                shardKey == std::set<FieldPath>{FieldPath("_id"s)});
    }

    try {
        auto pipeline = Pipeline::create(
            makeFlattenedList<boost::intrusive_ptr<DocumentSource>>(
                parsedMr.getQuery().map(
                    [&](auto&& query) { return DocumentSourceMatch::create(query, expCtx); }),
                parsedMr.getSort().map([&](auto&& sort) { return translateSort(expCtx, sort); }),
                parsedMr.getLimit().map(
                    [&](auto&& limit) { return DocumentSourceLimit::create(expCtx, limit); }),
                translateMap(expCtx, parsedMr.getMap().getCode()),
                DocumentSourceUnwind::create(expCtx, "emits", false, boost::none),
                translateReduce(expCtx, parsedMr.getReduce().getCode()),
                parsedMr.getFinalize().flat_map(
                    [&](auto&& finalize) { return translateFinalize(expCtx, finalize); }),
                translateOut(expCtx,
                             parsedMr.getOutOptions(),
                             std::move(outNss),
                             std::move(targetCollectionPlacementVersion),
                             parsedMr.getReduce().getCode(),
                             parsedMr.getFinalize())),
            expCtx);
        pipeline->optimizePipeline();
        return pipeline;
    } catch (DBException& ex) {
        uassertStatusOK(interpretTranslationError(&ex, parsedMr));
    }
    MONGO_UNREACHABLE;
}

}  // namespace mongo::map_reduce_common
