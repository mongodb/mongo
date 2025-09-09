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


#include "mongo/db/pipeline/document_source_union_with.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/exec/agg/pipeline_builder.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/pipeline/document_source_documents.h"
#include "mongo/db/pipeline/document_source_hybrid_scoring_util.h"
#include "mongo/db/pipeline/document_source_match.h"
#include "mongo/db/pipeline/document_source_queue.h"
#include "mongo/db/pipeline/document_source_single_document_transformation.h"
#include "mongo/db/pipeline/document_source_union_with_gen.h"
#include "mongo/db/pipeline/expression_context_builder.h"
#include "mongo/db/pipeline/process_interface/mongo_process_interface.h"
#include "mongo/db/pipeline/search/search_helper.h"
#include "mongo/db/query/allowed_contexts.h"
#include "mongo/db/views/resolved_view.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/compiler.h"
#include "mongo/util/namespace_string_util.h"
#include "mongo/util/str.h"
#include "mongo/util/uuid.h"

#include <iterator>

#include "variables.h"

#include <absl/container/flat_hash_map.h>
#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery


namespace mongo {

REGISTER_DOCUMENT_SOURCE(unionWith,
                         DocumentSourceUnionWith::LiteParsed::parse,
                         DocumentSourceUnionWith::createFromBson,
                         AllowedWithApiStrict::kAlways);
ALLOCATE_DOCUMENT_SOURCE_ID(unionWith, DocumentSourceUnionWith::id)

namespace {
std::unique_ptr<Pipeline> buildPipelineFromViewDefinition(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    ResolvedNamespace resolvedNs,
    std::vector<BSONObj> currentPipeline,
    NamespaceString userNss) {
    auto validatorCallback = [](const Pipeline& pipeline) {
        for (const auto& src : pipeline.getSources()) {
            uassert(31441,
                    str::stream() << src->getSourceName()
                                  << " is not allowed within a $unionWith's sub-pipeline",
                    src->constraints().isAllowedInUnionPipeline());
        }
    };

    MakePipelineOptions opts;
    opts.attachCursorSource = false;
    // Only call optimize() here if we actually have a pipeline to resolve in the view definition.
    opts.optimize = !resolvedNs.pipeline.empty();
    opts.validator = validatorCallback;

    auto subExpCtx = makeCopyForSubPipelineFromExpressionContext(
        expCtx, resolvedNs.ns, resolvedNs.uuid, userNss);

    return Pipeline::makePipelineFromViewDefinition(
        subExpCtx, resolvedNs, std::move(currentPipeline), opts, userNss);
}

MONGO_COMPILER_NOINLINE void logShardedViewFound(
    const ExceptionFor<ErrorCodes::CommandOnShardedViewNotSupportedOnMongod>& e,
    const std::vector<BSONObj>& pipeline) {
    LOGV2_DEBUG(4556300,
                3,
                "$unionWith found view definition. ns: {namespace}, pipeline: {pipeline}. New "
                "$unionWith sub-pipeline: {new_pipe}",
                logAttrs(e->getNamespace()),
                "pipeline"_attr = Value(e->getPipeline()),
                "new_pipe"_attr = pipeline);
}
}  // namespace

DocumentSourceUnionWith::DocumentSourceUnionWith(
    const DocumentSourceUnionWith& original,
    const boost::intrusive_ptr<ExpressionContext>& newExpCtx)
    : DocumentSource(kStageName, newExpCtx),
      _sharedState(std::make_shared<UnionWithSharedState>(
          original._sharedState->_pipeline->clone(
              newExpCtx ? makeCopyForSubPipelineFromExpressionContext(
                              newExpCtx,
                              newExpCtx->getResolvedNamespace(original._userNss).ns,
                              newExpCtx->getResolvedNamespace(original._userNss).uuid)
                        : nullptr),
          nullptr,
          UnionWithSharedState::ExecutionProgress::kIteratingSource,
          original._sharedState->_variables,
          original._sharedState->_variablesParseState)),
      _userNss(original._userNss),
      _userPipeline(original._userPipeline) {

    _sharedState->_pipeline->getContext()->setInUnionWith(true);

    tassert(10577700,
            "explain settings are different for $unionWith and its sub-pipeline",
            getExpCtx()->getExplain() == _sharedState->_pipeline->getContext()->getExplain());
}

DocumentSourceUnionWith::DocumentSourceUnionWith(
    const boost::intrusive_ptr<ExpressionContext>& expCtx, std::unique_ptr<Pipeline> pipeline)
    : DocumentSource(kStageName, expCtx) {

    auto variables = Variables();
    auto variablesParseState = VariablesParseState(variables.useIdGenerator());

    _sharedState = std::make_shared<UnionWithSharedState>(
        std::move(pipeline),
        nullptr,
        UnionWithSharedState::ExecutionProgress::kIteratingSource,
        std::move(variables),
        std::move(variablesParseState));

    if (!_sharedState->_pipeline->getContext()->getNamespaceString().isOnInternalDb()) {
        serviceOpCounters(getExpCtx()->getOperationContext()).gotNestedAggregate();
    }
    _sharedState->_pipeline->getContext()->setInUnionWith(true);
    tassert(10577701,
            "explain settings are different for $unionWith and its sub-pipeline",
            getExpCtx()->getExplain() == _sharedState->_pipeline->getContext()->getExplain());
}

DocumentSourceUnionWith::DocumentSourceUnionWith(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    NamespaceString unionNss,
    std::vector<BSONObj> pipeline)
    : DocumentSourceUnionWith(
          expCtx,
          buildPipelineFromViewDefinition(
              expCtx, expCtx->getResolvedNamespace(unionNss), pipeline, unionNss)) {
    // Save state regarding the resolved namespace in case we are running explain with
    // 'executionStats' or 'allPlansExecution' on a $unionWith with a view on a mongod. Otherwise we
    // wouldn't be able to see details about the execution of the view pipeline in the explain
    // result.
    ResolvedNamespace resolvedNs = expCtx->getResolvedNamespace(unionNss);
    if (expCtx->getExplain() &&
        expCtx->getExplain().value() != explain::VerbosityEnum::kQueryPlanner &&
        !resolvedNs.pipeline.empty()) {
        _resolvedNsForView = resolvedNs;
    }

    _userNss = std::move(unionNss);
    _userPipeline = std::move(pipeline);
}

DocumentSourceUnionWith::~DocumentSourceUnionWith() {
    // When in explain command, the sub-pipeline was not disposed in 'doDispose()', so we need to
    // dispose it here.
    if (getExpCtx()->getExplain()) {
        if (_sharedState->_execPipeline) {
            _sharedState->_execPipeline->reattachToOperationContext(
                getExpCtx()->getOperationContext());
            _sharedState->_execPipeline->dispose();
        }
    }
}

void validateUnionWithCollectionlessPipeline(
    const boost::optional<std::vector<mongo::BSONObj>>& pipeline) {
    const auto errMsg =
        "$unionWith stage without explicit collection must have a pipeline with $documents as "
        "first stage";

    uassert(ErrorCodes::FailedToParse, errMsg, pipeline && pipeline->size() > 0);
    const auto firstStageBson = (*pipeline)[0];
    LOGV2_DEBUG(5909700,
                4,
                "$unionWith validating collectionless pipeline",
                "pipeline"_attr = Pipeline::serializePipelineForLogging(*pipeline),
                "first"_attr = redact(firstStageBson));
    uassert(ErrorCodes::FailedToParse,
            errMsg,
            (firstStageBson.hasField(DocumentSourceDocuments::kStageName) ||
             firstStageBson.hasField(DocumentSourceQueue::kStageName))

    );
}

boost::intrusive_ptr<DocumentSource> DocumentSourceUnionWith::clone(
    const boost::intrusive_ptr<ExpressionContext>& newExpCtx) const {
    // At this point the ExpressionContext already has info about any resolved namespaces, so there
    // is no need to resolve them again when creating the clone.
    return make_intrusive<DocumentSourceUnionWith>(*this, newExpCtx);
}

std::unique_ptr<DocumentSourceUnionWith::LiteParsed> DocumentSourceUnionWith::LiteParsed::parse(
    const NamespaceString& nss, const BSONElement& spec, const LiteParserOptions& options) {
    uassert(ErrorCodes::FailedToParse,
            str::stream()
                << "the $unionWith stage specification must be an object or string, but found "
                << typeName(spec.type()),
            spec.type() == BSONType::object || spec.type() == BSONType::string);

    NamespaceString unionNss;
    boost::optional<LiteParsedPipeline> liteParsedPipeline;
    if (spec.type() == BSONType::string) {
        unionNss = NamespaceStringUtil::deserialize(nss.dbName(), spec.valueStringData());
    } else {
        auto unionWithSpec =
            UnionWithSpec::parse(spec.embeddedObject(), IDLParserContext(kStageName));
        if (unionWithSpec.getColl()) {
            unionNss = NamespaceStringUtil::deserialize(nss.dbName(), *unionWithSpec.getColl());
        } else {
            // If no collection specified, it must have $documents as first field in pipeline.
            validateUnionWithCollectionlessPipeline(unionWithSpec.getPipeline());
            unionNss = NamespaceString::makeCollectionlessAggregateNSS(nss.dbName());
        }

        // Recursively lite parse the nested pipeline, if one exists.
        if (unionWithSpec.getPipeline()) {
            liteParsedPipeline = LiteParsedPipeline(unionNss, *unionWithSpec.getPipeline());
        }
    }

    return std::make_unique<DocumentSourceUnionWith::LiteParsed>(
        spec.fieldName(), std::move(unionNss), std::move(liteParsedPipeline));
}

PrivilegeVector DocumentSourceUnionWith::LiteParsed::requiredPrivileges(
    bool isMongos, bool bypassDocumentValidation) const {
    PrivilegeVector requiredPrivileges;
    invariant(_pipelines.size() <= 1);
    invariant(_foreignNss);

    // If no pipeline is specified, then assume that we're reading directly from the collection.
    // Otherwise check whether the pipeline starts with an "initial source" indicating that we don't
    // require the "find" privilege.
    if (_pipelines.empty() || !_pipelines[0].startsWithInitialSource()) {
        Privilege::addPrivilegeToPrivilegeVector(
            &requiredPrivileges,
            Privilege(ResourcePattern::forExactNamespace(*_foreignNss), ActionType::find));
    }

    // Add the sub-pipeline privileges, if one was specified.
    if (!_pipelines.empty()) {
        const LiteParsedPipeline& pipeline = _pipelines[0];
        Privilege::addPrivilegesToPrivilegeVector(
            &requiredPrivileges, pipeline.requiredPrivileges(isMongos, bypassDocumentValidation));
    }
    return requiredPrivileges;
}

boost::intrusive_ptr<DocumentSource> DocumentSourceUnionWith::createFromBson(
    BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    uassert(ErrorCodes::FailedToParse,
            str::stream()
                << "the $unionWith stage specification must be an object or string, but found "
                << typeName(elem.type()),
            elem.type() == BSONType::object || elem.type() == BSONType::string);

    NamespaceString unionNss;
    std::vector<BSONObj> pipeline;
    if (elem.type() == BSONType::string) {
        unionNss = NamespaceStringUtil::deserialize(expCtx->getNamespaceString().dbName(),
                                                    elem.valueStringData());
    } else {
        // TODO SERVER-108117 Validate that the isHybridSearch flag is only set internally. See
        // helper hybrid_scoring_util::validateIsHybridSearchNotSetByUser to handle this.
        auto unionWithSpec =
            UnionWithSpec::parse(elem.embeddedObject(), IDLParserContext(kStageName));
        if (unionWithSpec.getColl()) {
            unionNss = NamespaceStringUtil::deserialize(expCtx->getNamespaceString().dbName(),
                                                        *unionWithSpec.getColl());
        } else {
            // if no collection specified, it must have $documents as first field in pipeline
            validateUnionWithCollectionlessPipeline(unionWithSpec.getPipeline());
            unionNss = NamespaceString::makeCollectionlessAggregateNSS(
                expCtx->getNamespaceString().dbName());
        }
        pipeline = unionWithSpec.getPipeline().value_or(std::vector<BSONObj>{});
        if (unionWithSpec.getIsHybridSearch() ||
            hybrid_scoring_util::isHybridSearchPipeline(pipeline)) {
            // If there is a hybrid search stage in our pipeline, then we should validate that we
            // are not running on a timeseries collection.
            //
            // If the hybrid search flag is set to true, this request may have
            // come from a mongos that does not know if the collection is a valid collection for
            // hybrid search. Therefore, we must validate it here.
            hybrid_scoring_util::assertForeignCollectionIsNotTimeseries(unionNss, expCtx);
        }
    }
    return make_intrusive<DocumentSourceUnionWith>(
        expCtx, std::move(unionNss), std::move(pipeline));
}

DocumentSourceContainer::iterator DocumentSourceUnionWith::doOptimizeAt(
    DocumentSourceContainer::iterator itr, DocumentSourceContainer* container) {
    auto duplicateAcrossUnion = [&](auto&& nextStage) {
        _sharedState->_pipeline->addFinalSource(
            nextStage->clone(_sharedState->_pipeline->getContext()));
        // Apply the same rewrite to the cached pipeline if available.
        if (getExpCtx()->getExplain() >= ExplainOptions::Verbosity::kExecStats) {
            _pushedDownStages.push_back(nextStage->serialize().getDocument().toBson());
        }
        auto newStageItr = container->insert(itr, std::move(nextStage));
        container->erase(std::next(itr));
        return newStageItr == container->begin() ? newStageItr : std::prev(newStageItr);
    };
    if (std::next(itr) != container->end()) {
        if (auto nextMatch = dynamic_cast<DocumentSourceMatch*>((*std::next(itr)).get()))
            return duplicateAcrossUnion(nextMatch);
        else if (auto nextProject = dynamic_cast<DocumentSourceSingleDocumentTransformation*>(
                     (*std::next(itr)).get()))
            return duplicateAcrossUnion(nextProject);
    }
    return std::next(itr);
};

Value DocumentSourceUnionWith::serialize(const SerializationOptions& opts) const {
    auto collectionless =
        _sharedState->_pipeline->getContext()->getNamespaceString().isCollectionlessAggregateNS();
    if (opts.isSerializingForExplain()) {
        // There are several different possible states depending on the explain verbosity as well as
        // the other stages in the pipeline:
        //  * If verbosity is queryPlanner, then the sub-pipeline should be untouched and we can
        //  explain it directly.
        //  * If verbosity is execStats or allPlansExecution, then whether or not to explain the
        //  sub-pipeline depends on if we've started reading from it. For instance, there could be a
        //  $limit stage after the $unionWith which results in only reading from the base collection
        //  branch and not the sub-pipeline.
        Pipeline* pipeCopy = nullptr;
        if (*opts.verbosity == ExplainOptions::Verbosity::kQueryPlanner) {
            pipeCopy = Pipeline::create(_sharedState->_pipeline->getSources(),
                                        _sharedState->_pipeline->getContext())
                           .release();
        } else if (*opts.verbosity >= ExplainOptions::Verbosity::kExecStats &&
                   _sharedState->_executionState >
                       UnionWithSharedState::ExecutionProgress::kIteratingSource) {
            std::vector<BSONObj> recoveredPipeline;
            // We've either exhausted the sub-pipeline or at least started iterating it. Use the
            // cached user pipeline and pushed down stages to get the explain output since the
            // '_pipeline' may have been modified for any optimizations or pushdowns into the
            // initial $cursor stage.
            recoveredPipeline.reserve(_userPipeline.size() + _pushedDownStages.size());
            std::move(
                _userPipeline.begin(), _userPipeline.end(), std::back_inserter(recoveredPipeline));
            std::move(_pushedDownStages.begin(),
                      _pushedDownStages.end(),
                      std::back_inserter(recoveredPipeline));
            // We reset the variables to their inital state for another execution.
            // TODO SERVER-94227 we probably don't need to do any validation as part of this parsing
            // pass?
            _sharedState->_variables.copyToExpCtx(_sharedState->_variablesParseState,
                                                  _sharedState->_pipeline->getContext().get());

            // Resolve the view definition, if there is one.
            if (_resolvedNsForView.has_value()) {
                // This takes care of the case where this code is executing on a mongod and we have
                // the full catalog information, so we can resolve the view.
                pipeCopy =
                    buildPipelineFromViewDefinition(
                        getExpCtx(),
                        ResolvedNamespace{_resolvedNsForView->ns, _resolvedNsForView->pipeline},
                        std::move(recoveredPipeline),
                        _userNss)
                        .release();
            } else {
                pipeCopy = Pipeline::parse(recoveredPipeline, _sharedState->_pipeline->getContext())
                               .release();
            }
        } else {
            // The plan does not require reading from the sub-pipeline, so just include the
            // serialization in the explain output.
            BSONArrayBuilder bab;
            for (auto&& stage : _sharedState->_pipeline->serialize(opts))
                bab << stage;
            auto spec = collectionless
                ? DOC("pipeline" << bab.arr())
                : DOC("coll"
                      << opts.serializeIdentifier(
                             _sharedState->_pipeline->getContext()->getNamespaceString().coll())
                      << "pipeline" << bab.arr());
            return Value(DOC(getSourceName() << spec));
        }

        invariant(pipeCopy);

        auto preparePipelineAndExplain = [&](Pipeline* ownedPipeline) {
            // Query settings are looked up after parsing and therefore are not populated in the
            // context of the unionWith '_pipeline' as part of DocumentSourceUnionWith
            // constructor. Attach query settings to the '_pipeline->getContext()' by copying
            // them from the parent query ExpressionContext.
            //
            // NOTE: this is done here, as opposed to at the beginning of the serialize() method
            // because serialize() is called when generating query shape, however, at that
            // moment no query settings are present in the parent context.
            _sharedState->_pipeline->getContext()->setQuerySettingsIfNotPresent(
                getExpCtx()->getQuerySettings());

            return getExpCtx()->getMongoProcessInterface()->preparePipelineAndExplain(
                ownedPipeline, *opts.verbosity);
        };

        BSONObj explainLocal = [&] {
            auto serializedPipe = pipeCopy->serializeToBson();
            try {
                return preparePipelineAndExplain(pipeCopy);
            } catch (const ExceptionFor<ErrorCodes::CommandOnShardedViewNotSupportedOnMongod>& e) {
                logShardedViewFound(e, _sharedState->_pipeline->serializeToBson());
                // This takes care of the case where this code is executing on mongos and we had to
                // get the view pipeline from a shard.
                auto resolvedPipeline = buildPipelineFromViewDefinition(
                    getExpCtx(),
                    ResolvedNamespace{e->getNamespace(), e->getPipeline()},
                    std::move(serializedPipe),
                    _userNss);
                return preparePipelineAndExplain(resolvedPipeline.release());
            }
        }();

        LOGV2_DEBUG(4553501, 3, "$unionWith attached cursor to pipeline for explain");
        // We expect this to be an explanation of a pipeline -- there should only be one field.
        invariant(explainLocal.nFields() == 1);

        auto spec = collectionless
            ? DOC("pipeline" << explainLocal.firstElement())
            : DOC("coll" << opts.serializeIdentifier(
                                _sharedState->_pipeline->getContext()->getNamespaceString().coll())
                         << "pipeline" << explainLocal.firstElement());
        return Value(DOC(getSourceName() << spec));
    } else if (opts.isSerializingForQueryStats()) {
        // Query shapes must reflect the original, unresolved and unoptimized pipeline, so we need a
        // special case here if we are serializing the stage for that purpose. Otherwise, we should
        // return the current (optimized) pipeline for introspection with explain, etc.
        // TODO SERVER-94227: we don't need to do any validation as part of this parsing pass.
        const auto serializedPipeline =
            Pipeline::parse(_userPipeline, _sharedState->_pipeline->getContext())
                ->serializeToBson(opts);
        auto spec = collectionless ? DOC("pipeline" << serializedPipeline)
                                   : DOC("coll" << opts.serializeIdentifier(_userNss.coll())
                                                << "pipeline" << serializedPipeline);
        return Value(DOC(getSourceName() << spec));
    } else {
        MutableDocument spec;
        if (!collectionless) {
            // When serializing to BSON before sending the request from router to shard, use the
            // underlying namespace rather than _userNss, since the pipeline is already resolved.
            // Using _userNss here could incorrectly retain the view name, leading to duplicated
            // view resolution and stages (e.g. $search applied twice).
            const auto underlyingNss = _sharedState->_pipeline->getContext()->getNamespaceString();
            spec["coll"] = Value(opts.serializeIdentifier(underlyingNss.coll()));
        }
        spec["pipeline"] = Value(_sharedState->_pipeline->serializeToBson(opts));
        bool isHybridSearch = hybrid_scoring_util::isHybridSearchPipeline(_userPipeline);
        if (isHybridSearch) {
            spec[hybrid_scoring_util::kIsHybridSearchFlagFieldName] = Value(isHybridSearch);
        }
        return Value(DOC(getSourceName() << spec.freezeToValue()));
    }
}

// Extracting dependencies for the outer collection. Although, this method walks the inner pipeline,
// the field dependencies are not collected - only variable dependencies are.
DepsTracker::State DocumentSourceUnionWith::getDependencies(DepsTracker* deps) const {
    if (!_sharedState->_pipeline) {
        return DepsTracker::State::SEE_NEXT;
    }

    DepsTracker subDeps;
    // Get the subpipeline dependencies.
    for (auto&& source : _sharedState->_pipeline->getSources()) {
        source->getDependencies(&subDeps);
    }

    return DepsTracker::State::SEE_NEXT;
}

void DocumentSourceUnionWith::addVariableRefs(std::set<Variables::Id>* refs) const {
    // Add sub-pipeline variable dependencies. Do not add SEARCH_META as a dependency, since it is
    // scoped to one pipeline.
    std::set<Variables::Id> subPipeRefs;
    _sharedState->_pipeline->addVariableRefs(&subPipeRefs);
    for (auto&& varId : subPipeRefs) {
        if (varId != Variables::kSearchMetaId)
            refs->insert(varId);
    }
}

void DocumentSourceUnionWith::detachSourceFromOperationContext() {
    // We have an execution pipeline we're going to execute across multiple commands, so we need to
    // detach it from the operation context when it goes out of scope.
    if (_sharedState->_execPipeline) {
        _sharedState->_execPipeline->detachFromOperationContext();
    }
    // Some methods require pipeline to have a valid operation context. Normally, a pipeline and the
    // corresponding execution pipeline share the same expression context containing a pointer to
    // the operation context, but it might not be the case anymore when a pipeline is cloned with
    // another expression context.
    if (_sharedState->_pipeline) {
        _sharedState->_pipeline->detachFromOperationContext();
    }
}

void DocumentSourceUnionWith::reattachSourceToOperationContext(OperationContext* opCtx) {
    // We have an execution pipeline we're going to execute across multiple commands, so we need to
    // propagate the new operation context to the pipeline stages.
    if (_sharedState->_execPipeline) {
        _sharedState->_execPipeline->reattachToOperationContext(opCtx);
    }
    // Some methods require pipeline to have a valid operation context. Normally, a pipeline and the
    // corresponding execution pipeline share the same expression context containing a pointer to
    // the operation context, but it might not be the case anymore when a pipeline is cloned with
    // another expression context.
    if (_sharedState->_pipeline) {
        _sharedState->_pipeline->reattachToOperationContext(opCtx);
    }
}

bool DocumentSourceUnionWith::validateSourceOperationContext(const OperationContext* opCtx) const {
    return getExpCtx()->getOperationContext() == opCtx &&
        (!_sharedState->_pipeline || _sharedState->_pipeline->validateOperationContext(opCtx));
}

void DocumentSourceUnionWith::addInvolvedCollections(
    stdx::unordered_set<NamespaceString>* collectionNames) const {
    collectionNames->insert(_sharedState->_pipeline->getContext()->getNamespaceString());
    collectionNames->merge(_sharedState->_pipeline->getInvolvedCollections());
}

}  // namespace mongo
