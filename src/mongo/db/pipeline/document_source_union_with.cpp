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


#include "variables.h"
#include <absl/container/flat_hash_map.h>
#include <iterator>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/pipeline/document_source_documents.h"
#include "mongo/db/pipeline/document_source_match.h"
#include "mongo/db/pipeline/document_source_queue.h"
#include "mongo/db/pipeline/document_source_single_document_transformation.h"
#include "mongo/db/pipeline/document_source_union_with.h"
#include "mongo/db/pipeline/document_source_union_with_gen.h"
#include "mongo/db/pipeline/process_interface/mongo_process_interface.h"
#include "mongo/db/query/allowed_contexts.h"
#include "mongo/db/query/plan_summary_stats.h"
#include "mongo/db/views/resolved_view.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_attr.h"
#include "mongo/logv2/log_component.h"
#include "mongo/platform/compiler.h"
#include "mongo/util/namespace_string_util.h"
#include "mongo/util/str.h"
#include "mongo/util/uuid.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery


namespace mongo {

REGISTER_DOCUMENT_SOURCE(unionWith,
                         DocumentSourceUnionWith::LiteParsed::parse,
                         DocumentSourceUnionWith::createFromBson,
                         AllowedWithApiStrict::kAlways);

namespace {
std::unique_ptr<Pipeline, PipelineDeleter> buildPipelineFromViewDefinition(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    ResolvedNamespace resolvedNs,
    std::vector<BSONObj> currentPipeline,
    NamespaceString userNss) {
    auto validatorCallback = [](const Pipeline& pipeline) {
        const auto& sources = pipeline.getSources();
        std::for_each(sources.begin(), sources.end(), [](auto& src) {
            uassert(31441,
                    str::stream() << src->getSourceName()
                                  << " is not allowed within a $unionWith's sub-pipeline",
                    src->constraints().isAllowedInUnionPipeline());
        });
    };

    MakePipelineOptions opts;
    opts.attachCursorSource = false;
    // Only call optimize() here if we actually have a pipeline to resolve in the view definition.
    opts.optimize = !resolvedNs.pipeline.empty();
    opts.validator = validatorCallback;

    return Pipeline::makePipelineFromViewDefinition(
        expCtx->copyForSubPipeline(resolvedNs.ns, resolvedNs.uuid),
        resolvedNs,
        std::move(currentPipeline),
        opts,
        userNss);
}

}  // namespace

DocumentSourceUnionWith::DocumentSourceUnionWith(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    std::unique_ptr<Pipeline, PipelineDeleter> pipeline)
    : DocumentSource(kStageName, expCtx),
      _pipeline(std::move(pipeline)),
      _variablesParseState(_variables.useIdGenerator()) {
    if (!_pipeline->getContext()->getNamespaceString().isOnInternalDb()) {
        serviceOpCounters(expCtx->getOperationContext()).gotNestedAggregate();
    }
    _pipeline->getContext()->setInUnionWith(true);
}

DocumentSourceUnionWith::DocumentSourceUnionWith(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    NamespaceString unionNss,
    std::vector<BSONObj> pipeline)
    : DocumentSourceUnionWith(
          expCtx,
          buildPipelineFromViewDefinition(
              expCtx, expCtx->getResolvedNamespace(unionNss), pipeline, unionNss)) {
    _userNss = std::move(unionNss);
    _userPipeline = std::move(pipeline);
}

DocumentSourceUnionWith::~DocumentSourceUnionWith() {
    if (_pipeline && _pipeline->getContext()->getExplain()) {
        _pipeline->dispose(pExpCtx->getOperationContext());
        _pipeline.reset();
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
                "pipeline"_attr = pipeline,
                "first"_attr = firstStageBson);
    uassert(ErrorCodes::FailedToParse,
            errMsg,
            // TODO SERVER-59628 replace with constraints check
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
    const NamespaceString& nss, const BSONElement& spec) {
    uassert(ErrorCodes::FailedToParse,
            str::stream()
                << "the $unionWith stage specification must be an object or string, but found "
                << typeName(spec.type()),
            spec.type() == BSONType::Object || spec.type() == BSONType::String);

    NamespaceString unionNss;
    boost::optional<LiteParsedPipeline> liteParsedPipeline;
    if (spec.type() == BSONType::String) {
        unionNss = NamespaceStringUtil::deserialize(nss.dbName(), spec.valueStringData());
    } else {
        auto unionWithSpec =
            UnionWithSpec::parse(IDLParserContext(kStageName), spec.embeddedObject());
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
            elem.type() == BSONType::Object || elem.type() == BSONType::String);

    NamespaceString unionNss;
    std::vector<BSONObj> pipeline;
    if (elem.type() == BSONType::String) {
        unionNss = NamespaceStringUtil::deserialize(expCtx->getNamespaceString().dbName(),
                                                    elem.valueStringData());
    } else {
        auto unionWithSpec =
            UnionWithSpec::parse(IDLParserContext(kStageName), elem.embeddedObject());
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
    }
    return make_intrusive<DocumentSourceUnionWith>(
        expCtx, std::move(unionNss), std::move(pipeline));
}

DocumentSource::GetNextResult DocumentSourceUnionWith::doGetNext() {
    if (!_pipeline) {
        // We must have already been disposed, so we're finished.
        return GetNextResult::makeEOF();
    }

    if (_executionState == ExecutionProgress::kIteratingSource) {
        auto nextInput = pSource->getNext();
        if (!nextInput.isEOF()) {
            return nextInput;
        }
        _executionState = ExecutionProgress::kStartingSubPipeline;
        // All documents from the base collection have been returned, switch to iterating the sub-
        // pipeline by falling through below.
    }

    if (_executionState == ExecutionProgress::kStartingSubPipeline) {
        // Since the subpipeline will be executed again for explain, we store the starting
        // state of the variables to reset them later.
        if (pExpCtx->getExplain()) {
            auto expCtx = _pipeline->getContext();
            _variables = expCtx->variables;
            _variablesParseState =
                expCtx->variablesParseState.copyWith(_variables.useIdGenerator());
        }

        auto serializedPipe = _pipeline->serializeToBson();
        logStartingSubPipeline(serializedPipe);
        try {
            // Query settings are looked up after parsing and therefore are not populated in the
            // context of the unionWith '_pipeline' as part of DocumentSourceUnionWith constructor.
            // Attach query settings to the '_pipeline->getContext()' by copying them from the
            // parent query ExpressionContext.
            _pipeline->getContext()->setQuerySettingsIfNotPresent(pExpCtx->getQuerySettings());

            LOGV2_DEBUG(9497002,
                        5,
                        "$unionWith before pipeline prep: ",
                        "pipeline"_attr = _pipeline->serializeToBson());
            _pipeline = pExpCtx->getMongoProcessInterface()->preparePipelineForExecution(
                _pipeline.release());
            LOGV2_DEBUG(9497003,
                        5,
                        "$unionWith POST pipeline prep: ",
                        "pipeline"_attr = _pipeline->serializeToBson());

            _executionState = ExecutionProgress::kIteratingSubPipeline;
        } catch (const ExceptionFor<ErrorCodes::CommandOnShardedViewNotSupportedOnMongod>& e) {
            _pipeline = buildPipelineFromViewDefinition(
                pExpCtx,
                ResolvedNamespace{e->getNamespace(), e->getPipeline()},
                std::move(serializedPipe),
                e->getNamespace());
            logShardedViewFound(e);
            return doGetNext();
        }
    }

    // The $unionWith stage takes responsibility for disposing of its Pipeline. When the outer
    // Pipeline that contains the $unionWith is disposed of, it will propagate dispose() to its
    // subpipeline.
    _pipeline.get_deleter().dismissDisposal();

    auto res = _pipeline->getNext();
    if (res)
        return std::move(*res);

    // Record the plan summary stats after $unionWith operation is done.
    accumulatePipelinePlanSummaryStats(*_pipeline, _stats.planSummaryStats);

    _executionState = ExecutionProgress::kFinished;
    return GetNextResult::makeEOF();
}

// The use of these logging macros is done in separate NOINLINE functions to reduce the stack space
// used on the hot getNext() path. This is done to avoid stack overflows.
MONGO_COMPILER_NOINLINE void DocumentSourceUnionWith::logStartingSubPipeline(
    const std::vector<BSONObj>& serializedPipe) {
    LOGV2_DEBUG(23869,
                1,
                "$unionWith attaching cursor to pipeline {pipeline}",
                "pipeline"_attr = serializedPipe);
}

MONGO_COMPILER_NOINLINE void DocumentSourceUnionWith::logShardedViewFound(
    const ExceptionFor<ErrorCodes::CommandOnShardedViewNotSupportedOnMongod>& e) const {
    LOGV2_DEBUG(4556300,
                3,
                "$unionWith found view definition. ns: {namespace}, pipeline: {pipeline}. New "
                "$unionWith sub-pipeline: {new_pipe}",
                logAttrs(e->getNamespace()),
                "pipeline"_attr = Value(e->getPipeline()),
                "new_pipe"_attr = _pipeline->serializeToBson());
}

Pipeline::SourceContainer::iterator DocumentSourceUnionWith::doOptimizeAt(
    Pipeline::SourceContainer::iterator itr, Pipeline::SourceContainer* container) {
    auto duplicateAcrossUnion = [&](auto&& nextStage) {
        _pipeline->addFinalSource(nextStage->clone(_pipeline->getContext()));
        // Apply the same rewrite to the cached pipeline if available.
        if (pExpCtx->getExplain() >= ExplainOptions::Verbosity::kExecStats) {
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

bool DocumentSourceUnionWith::usedDisk() {
    if (_pipeline) {
        _stats.planSummaryStats.usedDisk =
            _stats.planSummaryStats.usedDisk || _pipeline->usedDisk();
    }
    return _stats.planSummaryStats.usedDisk;
}

void DocumentSourceUnionWith::doDispose() {
    if (_pipeline) {
        _pipeline.get_deleter().dismissDisposal();
        _stats.planSummaryStats.usedDisk =
            _stats.planSummaryStats.usedDisk || _pipeline->usedDisk();
        accumulatePipelinePlanSummaryStats(*_pipeline, _stats.planSummaryStats);

        if (!_pipeline->getContext()->getExplain()) {
            _pipeline->dispose(pExpCtx->getOperationContext());
            _userPipeline.clear();
            _pushedDownStages.clear();
            _pipeline.reset();
        }
    }
}

Value DocumentSourceUnionWith::serialize(const SerializationOptions& opts) const {
    auto collectionless =
        _pipeline->getContext()->getNamespaceString().isCollectionlessAggregateNS();
    if (opts.verbosity) {
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
            pipeCopy = Pipeline::create(_pipeline->getSources(), _pipeline->getContext()).release();
        } else if (*opts.verbosity >= ExplainOptions::Verbosity::kExecStats &&
                   _executionState > ExecutionProgress::kIteratingSource) {
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
            _variables.copyToExpCtx(_variablesParseState, _pipeline->getContext().get());
            pipeCopy = Pipeline::parse(recoveredPipeline, _pipeline->getContext()).release();
        } else {
            // The plan does not require reading from the sub-pipeline, so just include the
            // serialization in the explain output.
            BSONArrayBuilder bab;
            for (auto&& stage : _pipeline->serialize(opts))
                bab << stage;
            auto spec = collectionless
                ? DOC("pipeline" << bab.arr())
                : DOC("coll" << opts.serializeIdentifier(
                                    _pipeline->getContext()->getNamespaceString().coll())
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
            _pipeline->getContext()->setQuerySettingsIfNotPresent(pExpCtx->getQuerySettings());

            return pExpCtx->getMongoProcessInterface()->preparePipelineAndExplain(ownedPipeline,
                                                                                  *opts.verbosity);
        };

        BSONObj explainLocal = [&] {
            auto serializedPipe = pipeCopy->serializeToBson();
            try {
                return preparePipelineAndExplain(pipeCopy);
            } catch (const ExceptionFor<ErrorCodes::CommandOnShardedViewNotSupportedOnMongod>& e) {
                logShardedViewFound(e);
                auto resolvedPipeline = buildPipelineFromViewDefinition(
                    pExpCtx,
                    ResolvedNamespace{e->getNamespace(), e->getPipeline()},
                    std::move(serializedPipe),
                    e->getNamespace());
                return preparePipelineAndExplain(resolvedPipeline.release());
            }
        }();

        LOGV2_DEBUG(4553501, 3, "$unionWith attached cursor to pipeline for explain");
        // We expect this to be an explanation of a pipeline -- there should only be one field.
        invariant(explainLocal.nFields() == 1);

        auto spec = collectionless
            ? DOC("pipeline" << explainLocal.firstElement())
            : DOC("coll" << opts.serializeIdentifier(
                                _pipeline->getContext()->getNamespaceString().coll())
                         << "pipeline" << explainLocal.firstElement());
        return Value(DOC(getSourceName() << spec));
    } else {
        // Query shapes must reflect the original, unresolved and unoptimized pipeline, so we need a
        // special case here if we are serializing the stage for that purpose. Otherwise, we should
        // return the current (optimized) pipeline for introspection with explain, etc.
        auto serializedPipeline = [&]() -> std::vector<BSONObj> {
            if (opts.transformIdentifiers ||
                opts.literalPolicy != LiteralSerializationPolicy::kUnchanged) {
                // TODO SERVER-94227 we don't need to do any validation as part of this parsing
                // pass.
                return Pipeline::parse(_userPipeline, _pipeline->getContext())
                    ->serializeToBson(opts);
            }
            return _pipeline->serializeToBson(opts);
        }();

        auto spec = collectionless ? DOC("pipeline" << serializedPipeline)
                                   : DOC("coll" << opts.serializeIdentifier(_userNss.coll())
                                                << "pipeline" << serializedPipeline);
        return Value(DOC(getSourceName() << spec));
    }
}

// Extracting dependencies for the outer collection. Although, this method walks the inner pipeline,
// the field dependencies are not collected - only variable dependencies are.
DepsTracker::State DocumentSourceUnionWith::getDependencies(DepsTracker* deps) const {
    if (!_pipeline) {
        return DepsTracker::State::SEE_NEXT;
    }

    // We only need to know what variable dependencies exist in the subpipeline. So without
    // knowledge of what metadata is in fact unavailable, we "lie" and say that all metadata
    // is available to avoid tripping any assertions.
    DepsTracker subDeps(DepsTracker::kNoMetadata);
    // Get the subpipeline dependencies.
    for (auto&& source : _pipeline->getSources()) {
        source->getDependencies(&subDeps);
    }

    return DepsTracker::State::SEE_NEXT;
}

void DocumentSourceUnionWith::addVariableRefs(std::set<Variables::Id>* refs) const {
    // Add sub-pipeline variable dependencies. Do not add SEARCH_META as a dependency, since it is
    // scoped to one pipeline.
    std::set<Variables::Id> subPipeRefs;
    _pipeline->addVariableRefs(&subPipeRefs);
    for (auto&& varId : subPipeRefs) {
        if (varId != Variables::kSearchMetaId)
            refs->insert(varId);
    }
}

void DocumentSourceUnionWith::detachFromOperationContext() {
    // We have a pipeline we're going to be executing across multiple calls to getNext(), so we
    // use Pipeline::detachFromOperationContext() to take care of updating the Pipeline's
    // ExpressionContext.
    if (_pipeline) {
        _pipeline->detachFromOperationContext();
    }
}

void DocumentSourceUnionWith::reattachToOperationContext(OperationContext* opCtx) {
    // We have a pipeline we're going to be executing across multiple calls to getNext(), so we
    // use Pipeline::reattachToOperationContext() to take care of updating the Pipeline's
    // ExpressionContext.
    if (_pipeline) {
        _pipeline->reattachToOperationContext(opCtx);
    }
}

bool DocumentSourceUnionWith::validateOperationContext(const OperationContext* opCtx) const {
    return getContext()->getOperationContext() == opCtx &&
        (!_pipeline || _pipeline->validateOperationContext(opCtx));
}

void DocumentSourceUnionWith::addInvolvedCollections(
    stdx::unordered_set<NamespaceString>* collectionNames) const {
    collectionNames->insert(_pipeline->getContext()->getNamespaceString());
    collectionNames->merge(_pipeline->getInvolvedCollections());
}

}  // namespace mongo
