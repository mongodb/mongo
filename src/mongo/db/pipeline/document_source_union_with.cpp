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

#include <iterator>

#include "mongo/db/commands/test_commands_enabled.h"
#include "mongo/db/pipeline/document_source_documents.h"
#include "mongo/db/pipeline/document_source_match.h"
#include "mongo/db/pipeline/document_source_queue.h"
#include "mongo/db/pipeline/document_source_single_document_transformation.h"
#include "mongo/db/pipeline/document_source_union_with.h"
#include "mongo/db/pipeline/document_source_union_with_gen.h"
#include "mongo/db/views/resolved_view.h"
#include "mongo/logv2/log.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery


namespace mongo {

REGISTER_DOCUMENT_SOURCE(unionWith,
                         DocumentSourceUnionWith::LiteParsed::parse,
                         DocumentSourceUnionWith::createFromBson,
                         AllowedWithApiStrict::kAlways);

namespace {
std::unique_ptr<Pipeline, PipelineDeleter> buildPipelineFromViewDefinition(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    ExpressionContext::ResolvedNamespace resolvedNs,
    std::vector<BSONObj> currentPipeline) {

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
        expCtx->copyForSubPipeline(expCtx->ns, resolvedNs.uuid), resolvedNs, currentPipeline, opts);
}

}  // namespace

DocumentSourceUnionWith::~DocumentSourceUnionWith() {
    if (_pipeline && _pipeline->getContext()->explain) {
        _pipeline->dispose(pExpCtx->opCtx);
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
        unionNss =
            NamespaceStringUtil::parseNamespaceFromRequest(nss.dbName(), spec.valueStringData());
    } else {
        auto unionWithSpec =
            UnionWithSpec::parse(IDLParserContext(kStageName), spec.embeddedObject());
        if (unionWithSpec.getColl()) {
            unionNss = NamespaceStringUtil::parseNamespaceFromRequest(nss.dbName(),
                                                                      *unionWithSpec.getColl());
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
            &requiredPrivileges,
            std::move(pipeline.requiredPrivileges(isMongos, bypassDocumentValidation)));
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
        unionNss = NamespaceStringUtil::parseNamespaceFromRequest(expCtx->ns.dbName(),
                                                                  elem.valueStringData());
    } else {
        auto unionWithSpec =
            UnionWithSpec::parse(IDLParserContext(kStageName), elem.embeddedObject());
        if (unionWithSpec.getColl()) {
            unionNss = NamespaceStringUtil::parseNamespaceFromRequest(expCtx->ns.dbName(),
                                                                      *unionWithSpec.getColl());
        } else {
            // if no collection specified, it must have $documents as first field in pipeline
            validateUnionWithCollectionlessPipeline(unionWithSpec.getPipeline());
            unionNss = NamespaceString::makeCollectionlessAggregateNSS(expCtx->ns.dbName());
        }
        pipeline = unionWithSpec.getPipeline().value_or(std::vector<BSONObj>{});
    }
    return make_intrusive<DocumentSourceUnionWith>(
        expCtx,
        buildPipelineFromViewDefinition(
            expCtx, expCtx->getResolvedNamespace(std::move(unionNss)), std::move(pipeline)));
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
        auto serializedPipe = _pipeline->serializeToBson();
        logStartingSubPipeline(serializedPipe);
        try {
            _pipeline =
                pExpCtx->mongoProcessInterface->attachCursorSourceToPipeline(_pipeline.release());
            _executionState = ExecutionProgress::kIteratingSubPipeline;
        } catch (const ExceptionFor<ErrorCodes::CommandOnShardedViewNotSupportedOnMongod>& e) {
            _pipeline = buildPipelineFromViewDefinition(
                pExpCtx,
                ExpressionContext::ResolvedNamespace{e->getNamespace(), e->getPipeline()},
                serializedPipe);
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
    const ExceptionFor<ErrorCodes::CommandOnShardedViewNotSupportedOnMongod>& e) {
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
        if (pExpCtx->explain >= ExplainOptions::Verbosity::kExecStats) {
            auto cloneForExplain = nextStage->clone(_pipeline->getContext());
            if (!_cachedPipeline.empty()) {
                cloneForExplain->setSource(_cachedPipeline.back().get());
            }
            _cachedPipeline.push_back(std::move(cloneForExplain));
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

        if (!_pipeline->getContext()->explain) {
            _pipeline->dispose(pExpCtx->opCtx);
            _pipeline.reset();
        }
    }
}

Value DocumentSourceUnionWith::serialize(SerializationOptions opts) const {
    auto explain = opts.verbosity;
    if (opts.redactIdentifiers || opts.replacementForLiteralArgs) {
        MONGO_UNIMPLEMENTED_TASSERT(7484307);
    }

    auto collectionless = _pipeline->getContext()->ns.isCollectionlessAggregateNS();
    if (explain) {
        // There are several different possible states depending on the explain verbosity as well as
        // the other stages in the pipeline:
        //  * If verbosity is queryPlanner, then the sub-pipeline should be untouched and we can
        //  explain it directly.
        //  * If verbosity is execStats or allPlansExecution, then whether or not to explain the
        //  sub-pipeline depends on if we've started reading from it. For instance, there could be a
        //  $limit stage after the $unionWith which results in only reading from the base collection
        //  branch and not the sub-pipeline.
        Pipeline* pipeCopy = nullptr;
        if (*explain == ExplainOptions::Verbosity::kQueryPlanner) {
            pipeCopy = Pipeline::create(_pipeline->getSources(), _pipeline->getContext()).release();
        } else if (*explain >= ExplainOptions::Verbosity::kExecStats &&
                   _executionState > ExecutionProgress::kIteratingSource) {
            // We've either exhausted the sub-pipeline or at least started iterating it. Use the
            // cached pipeline to get the explain output since the '_pipeline' may have been
            // modified for any optimizations or pushdowns into the initial $cursor stage.
            pipeCopy = Pipeline::create(_cachedPipeline, _pipeline->getContext()).release();
        } else {
            // The plan does not require reading from the sub-pipeline, so just include the
            // serialization in the explain output.
            BSONArrayBuilder bab;
            for (auto&& stage : _pipeline->serialize(explain))
                bab << stage;
            auto spec = collectionless
                ? DOC("pipeline" << bab.arr())
                : DOC("coll" << _pipeline->getContext()->ns.coll() << "pipeline" << bab.arr());
            return Value(DOC(getSourceName() << spec));
        }

        invariant(pipeCopy);
        BSONObj explainLocal =
            pExpCtx->mongoProcessInterface->preparePipelineAndExplain(pipeCopy, *explain);
        LOGV2_DEBUG(4553501, 3, "$unionWith attached cursor to pipeline for explain");
        // We expect this to be an explanation of a pipeline -- there should only be one field.
        invariant(explainLocal.nFields() == 1);

        auto spec = collectionless ? DOC("pipeline" << explainLocal.firstElement())
                                   : DOC("coll" << _pipeline->getContext()->ns.coll() << "pipeline"
                                                << explainLocal.firstElement());
        return Value(DOC(getSourceName() << spec));
    } else {
        BSONArrayBuilder bab;
        for (auto&& stage : _pipeline->serialize())
            bab << stage;
        auto spec = collectionless
            ? DOC("pipeline" << bab.arr())
            : DOC("coll" << _pipeline->getContext()->ns.coll() << "pipeline" << bab.arr());
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
    return getContext()->opCtx == opCtx &&
        (!_pipeline || _pipeline->validateOperationContext(opCtx));
}

void DocumentSourceUnionWith::addInvolvedCollections(
    stdx::unordered_set<NamespaceString>* collectionNames) const {
    collectionNames->insert(_pipeline->getContext()->ns);
    collectionNames->merge(_pipeline->getInvolvedCollections());
}

}  // namespace mongo
