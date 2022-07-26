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
#include "mongo/logv2/log.h"

#include <algorithm>

#include "mongo/base/error_codes.h"
#include "mongo/db/bson/dotted_path_support.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/accumulator.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_match.h"
#include "mongo/db/pipeline/document_source_merge.h"
#include "mongo/db/pipeline/document_source_out.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/lite_parsed_pipeline.h"
#include "mongo/db/pipeline/search_helper.h"
#include "mongo/db/query/query_knobs_gen.h"
#include "mongo/db/storage/storage_options.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/str.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault


namespace mongo {

namespace {

// Given a serialized document source, appends execution stats 'nReturned' and
// 'executionTimeMillisEstimate' to it.
Value appendCommonExecStats(Value docSource, const CommonStats& stats) {
    invariant(docSource.getType() == BSONType::Object);
    MutableDocument doc(docSource.getDocument());
    auto nReturned = static_cast<long long>(stats.advanced);
    invariant(stats.executionTimeMillis);
    auto executionTimeMillisEstimate = static_cast<long long>(*stats.executionTimeMillis);
    doc.addField("nReturned", Value(nReturned));
    doc.addField("executionTimeMillisEstimate", Value(executionTimeMillisEstimate));
    return Value(doc.freeze());
}

/**
 * Performs validation checking specific to top-level pipelines. Throws an assertion if the
 * pipeline is invalid.
 */
void validateTopLevelPipeline(const Pipeline& pipeline) {
    // Verify that the specified namespace is valid for the initial stage of this pipeline.
    const NamespaceString& nss = pipeline.getContext()->ns;

    auto sources = pipeline.getSources();

    if (sources.empty()) {
        uassert(ErrorCodes::InvalidNamespace,
                "{aggregate: 1} is not valid for an empty pipeline.",
                !nss.isCollectionlessAggregateNS());
        return;
    }

    if ("$mergeCursors"_sd != sources.front()->getSourceName()) {
        // The $mergeCursors stage can take {aggregate: 1} or a normal namespace. Aside from this,
        // {aggregate: 1} is only valid for collectionless sources, and vice-versa.
        const auto firstStageConstraints = sources.front()->constraints();

        uassert(ErrorCodes::InvalidNamespace,
                str::stream() << "{aggregate: 1} is not valid for '"
                              << sources.front()->getSourceName() << "'; a collection is required.",
                !(nss.isCollectionlessAggregateNS() &&
                  !firstStageConstraints.isIndependentOfAnyCollection));

        uassert(ErrorCodes::InvalidNamespace,
                str::stream() << "'" << sources.front()->getSourceName()
                              << "' can only be run with {aggregate: 1}",
                !(!nss.isCollectionlessAggregateNS() &&
                  firstStageConstraints.isIndependentOfAnyCollection));

        // If the first stage is a $changeStream stage, then all stages in the pipeline must be
        // either $changeStream stages or allowlisted as being able to run in a change stream.
        if (firstStageConstraints.isChangeStreamStage()) {
            for (auto&& source : sources) {
                uassert(ErrorCodes::IllegalOperation,
                        str::stream() << source->getSourceName()
                                      << " is not permitted in a $changeStream pipeline",
                        source->constraints().isAllowedInChangeStream());
            }
        }
    }

    // Verify that usage of $searchMeta and $search is legal.
    if (pipeline.getContext()->opCtx->getServiceContext()) {
        getSearchHelpers(pipeline.getContext()->opCtx->getServiceContext())
            ->assertSearchMetaAccessValid(sources, pipeline.getContext().get());
    }
}

}  // namespace

MONGO_FAIL_POINT_DEFINE(disablePipelineOptimization);

using boost::intrusive_ptr;
using std::endl;
using std::ostringstream;
using std::string;
using std::vector;

namespace dps = ::mongo::dotted_path_support;

using ChangeStreamRequirement = StageConstraints::ChangeStreamRequirement;
using HostTypeRequirement = StageConstraints::HostTypeRequirement;
using PositionRequirement = StageConstraints::PositionRequirement;
using DiskUseRequirement = StageConstraints::DiskUseRequirement;
using FacetRequirement = StageConstraints::FacetRequirement;
using StreamType = StageConstraints::StreamType;

constexpr MatchExpressionParser::AllowedFeatureSet Pipeline::kAllowedMatcherFeatures;
constexpr MatchExpressionParser::AllowedFeatureSet Pipeline::kGeoNearMatcherFeatures;

Pipeline::Pipeline(const intrusive_ptr<ExpressionContext>& pTheCtx) : pCtx(pTheCtx) {}

Pipeline::Pipeline(SourceContainer stages, const intrusive_ptr<ExpressionContext>& expCtx)
    : _sources(std::move(stages)), pCtx(expCtx) {}

Pipeline::~Pipeline() {
    invariant(_disposed);
}

std::unique_ptr<Pipeline, PipelineDeleter> Pipeline::clone(
    const boost::intrusive_ptr<ExpressionContext>& newExpCtx) const {
    SourceContainer clonedStages;
    for (auto&& stage : _sources) {
        clonedStages.push_back(stage->clone(newExpCtx));
    }
    return create(clonedStages, newExpCtx ? newExpCtx : getContext());
}

template <class T>
std::unique_ptr<Pipeline, PipelineDeleter> Pipeline::parseCommon(
    const std::vector<T>& rawPipeline,
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    PipelineValidatorCallback validator,
    std::function<BSONObj(T)> getElemFunc) {

    SourceContainer stages;

    for (auto&& stageElem : rawPipeline) {
        auto parsedSources = DocumentSource::parse(expCtx, getElemFunc(stageElem));
        stages.insert(stages.end(), parsedSources.begin(), parsedSources.end());
    }

    std::unique_ptr<Pipeline, PipelineDeleter> pipeline(new Pipeline(std::move(stages), expCtx),
                                                        PipelineDeleter(expCtx->opCtx));

    // First call the context-specific validator, which may be different for top-level pipelines
    // versus nested pipelines.
    if (validator)
        validator(*pipeline);
    else {
        validateTopLevelPipeline(*pipeline);
    }

    // Next run through the common validation rules that apply to every pipeline.
    constexpr bool alreadyOptimized = false;
    pipeline->validateCommon(alreadyOptimized);

    pipeline->stitch();
    return pipeline;
}

std::unique_ptr<Pipeline, PipelineDeleter> Pipeline::parseFromArray(
    BSONElement rawPipelineElement,
    const intrusive_ptr<ExpressionContext>& expCtx,
    PipelineValidatorCallback validator) {

    tassert(6253719,
            "Expected array for Pipeline::parseFromArray",
            rawPipelineElement.type() == BSONType::Array);
    auto rawStages = rawPipelineElement.Array();

    return parseCommon<BSONElement>(rawStages, expCtx, validator, [](BSONElement e) {
        uassert(6253720, "Pipeline array element must be an object", e.type() == BSONType::Object);
        return e.embeddedObject();
    });
}

std::unique_ptr<Pipeline, PipelineDeleter> Pipeline::parse(
    const std::vector<BSONObj>& rawPipeline,
    const intrusive_ptr<ExpressionContext>& expCtx,
    PipelineValidatorCallback validator) {

    return parseCommon<BSONObj>(rawPipeline, expCtx, validator, [](BSONObj o) { return o; });
}

std::unique_ptr<Pipeline, PipelineDeleter> Pipeline::create(
    SourceContainer stages, const intrusive_ptr<ExpressionContext>& expCtx) {
    std::unique_ptr<Pipeline, PipelineDeleter> pipeline(new Pipeline(std::move(stages), expCtx),
                                                        PipelineDeleter(expCtx->opCtx));

    constexpr bool alreadyOptimized = false;
    pipeline->validateCommon(alreadyOptimized);
    pipeline->stitch();
    return pipeline;
}

void Pipeline::validateCommon(bool alreadyOptimized) const {
    size_t i = 0;

    uassert(ErrorCodes::FailedToParse,
            str::stream() << "Pipeline length must be no longer than "
                          << internalPipelineLengthLimit << " stages",
            static_cast<int>(_sources.size()) <= internalPipelineLengthLimit);

    for (auto&& stage : _sources) {
        auto constraints = stage->constraints(_splitState);

        // Verify that all stages adhere to their PositionRequirement constraints.
        uassert(40602,
                str::stream() << stage->getSourceName()
                              << " is only valid as the first stage in a pipeline",
                !(constraints.requiredPosition == PositionRequirement::kFirst && i != 0));
        uassert(40603,
                str::stream() << stage->getSourceName()
                              << " is only valid as the first stage in an optimized pipeline",
                !(alreadyOptimized &&
                  constraints.requiredPosition == PositionRequirement::kFirstAfterOptimization &&
                  i != 0));

        auto matchStage = dynamic_cast<DocumentSourceMatch*>(stage.get());
        uassert(17313,
                "$match with $text is only allowed as the first pipeline stage",
                !(i != 0 && matchStage && matchStage->isTextQuery()));

        uassert(40601,
                str::stream() << stage->getSourceName()
                              << " can only be the final stage in the pipeline",
                !(constraints.requiredPosition == PositionRequirement::kLast &&
                  i != _sources.size() - 1));
        ++i;

        // Verify that we are not attempting to run a mongoS-only stage on mongoD.
        uassert(40644,
                str::stream() << stage->getSourceName() << " can only be run on mongoS",
                !(constraints.hostRequirement == HostTypeRequirement::kMongoS && !pCtx->inMongos));

        uassert(
            ErrorCodes::OperationNotSupportedInTransaction,
            str::stream() << "Stage not supported inside of a multi-document transaction: "
                          << stage->getSourceName(),
            !(pCtx->opCtx->inMultiDocumentTransaction() && !constraints.isAllowedInTransaction()));
    }
}

void Pipeline::optimizePipeline() {
    // If the disablePipelineOptimization failpoint is enabled, the pipeline won't be optimized.
    if (MONGO_unlikely(disablePipelineOptimization.shouldFail())) {
        return;
    }

    optimizeContainer(&_sources);
}

void Pipeline::optimizeContainer(SourceContainer* container) {
    SourceContainer optimizedSources;

    SourceContainer::iterator itr = container->begin();
    try {
        while (itr != container->end()) {
            invariant((*itr).get());
            itr = (*itr).get()->optimizeAt(itr, container);
        }

        // Once we have reached our final number of stages, optimize each individually.
        for (auto&& source : *container) {
            if (auto out = source->optimize()) {
                optimizedSources.push_back(out);
            }
        }
        container->swap(optimizedSources);
    } catch (DBException& ex) {
        ex.addContext("Failed to optimize pipeline");
        throw;
    }

    stitch(container);
}

bool Pipeline::aggHasWriteStage(const BSONObj& cmd) {
    auto pipelineElement = cmd["pipeline"];
    if (pipelineElement.type() != BSONType::Array) {
        return false;
    }

    for (auto stage : pipelineElement.Obj()) {
        if (stage.type() != BSONType::Object) {
            return false;
        }

        if (stage.Obj().hasField(DocumentSourceOut::kStageName) ||
            stage.Obj().hasField(DocumentSourceMerge::kStageName)) {
            return true;
        }
    }

    return false;
}

void Pipeline::detachFromOperationContext() {
    pCtx->opCtx = nullptr;

    for (auto&& source : _sources) {
        source->detachFromOperationContext();
    }
}

void Pipeline::reattachToOperationContext(OperationContext* opCtx) {
    pCtx->opCtx = opCtx;

    for (auto&& source : _sources) {
        source->reattachToOperationContext(opCtx);
    }
}

void Pipeline::dispose(OperationContext* opCtx) {
    try {
        pCtx->opCtx = opCtx;

        // Make sure all stages are connected, in case we are being disposed via an error path and
        // were not stitched at the time of the error.
        stitch();

        if (!_sources.empty()) {
            _sources.back()->dispose();
        }
        _disposed = true;
    } catch (...) {
        std::terminate();
    }
}

bool Pipeline::usedDisk() {
    return std::any_of(
        _sources.begin(), _sources.end(), [](const auto& stage) { return stage->usedDisk(); });
}

BSONObj Pipeline::getInitialQuery() const {
    if (_sources.empty()) {
        return BSONObj{};
    }

    const DocumentSource* doc = _sources.front().get();
    if (doc->hasQuery()) {
        return doc->getQuery();
    }

    return BSONObj{};
}

bool Pipeline::needsPrimaryShardMerger() const {
    return std::any_of(_sources.begin(), _sources.end(), [&](const auto& stage) {
        return stage->constraints(SplitState::kSplitForMerge).hostRequirement ==
            HostTypeRequirement::kPrimaryShard;
    });
}

bool Pipeline::needsMongosMerger() const {
    return std::any_of(_sources.begin(), _sources.end(), [&](const auto& stage) {
        return stage->constraints(SplitState::kSplitForMerge).resolvedHostTypeRequirement(pCtx) ==
            HostTypeRequirement::kMongoS;
    });
}

bool Pipeline::needsShard() const {
    return std::any_of(_sources.begin(), _sources.end(), [&](const auto& stage) {
        auto hostType = stage->constraints().resolvedHostTypeRequirement(pCtx);
        return (hostType == HostTypeRequirement::kAnyShard ||
                hostType == HostTypeRequirement::kPrimaryShard);
    });
}

bool Pipeline::canRunOnMongos() const {
    return _pipelineCanRunOnMongoS().isOK();
}

bool Pipeline::requiredToRunOnMongos() const {
    invariant(_splitState != SplitState::kSplitForShards);

    for (auto&& stage : _sources) {
        // If this pipeline is capable of splitting before the mongoS-only stage, then the pipeline
        // as a whole is not required to run on mongoS.
        if (_splitState == SplitState::kUnsplit && stage->distributedPlanLogic()) {
            return false;
        }

        auto hostRequirement = stage->constraints(_splitState).resolvedHostTypeRequirement(pCtx);

        // If a mongoS-only stage occurs before a splittable stage, or if the pipeline is already
        // split, this entire pipeline must run on mongoS.
        if (hostRequirement == HostTypeRequirement::kMongoS) {
            // Verify that the remainder of this pipeline can run on mongoS.
            auto mongosRunStatus = _pipelineCanRunOnMongoS();

            uassertStatusOKWithContext(mongosRunStatus,
                                       str::stream() << stage->getSourceName()
                                                     << " must run on mongoS, but cannot");

            return true;
        }
    }

    return false;
}

stdx::unordered_set<NamespaceString> Pipeline::getInvolvedCollections() const {
    stdx::unordered_set<NamespaceString> collectionNames;
    for (auto&& source : _sources) {
        source->addInvolvedCollections(&collectionNames);
    }
    return collectionNames;
}

vector<Value> Pipeline::serializeContainer(const SourceContainer& container,
                                           boost::optional<ExplainOptions::Verbosity> explain) {
    vector<Value> serializedSources;
    for (auto&& source : container) {
        source->serializeToArray(serializedSources, explain);
    }
    return serializedSources;
}
vector<Value> Pipeline::serialize(boost::optional<ExplainOptions::Verbosity> explain) const {
    return serializeContainer(_sources, explain);
}

vector<BSONObj> Pipeline::serializeToBson(
    boost::optional<ExplainOptions::Verbosity> explain) const {
    const auto serialized = serialize(explain);
    std::vector<BSONObj> asBson;
    asBson.reserve(serialized.size());
    for (auto&& stage : serialized) {
        invariant(stage.getType() == BSONType::Object);
        asBson.push_back(stage.getDocument().toBson());
    }
    return asBson;
}

void Pipeline::stitch() {
    stitch(&_sources);
}

void Pipeline::stitch(SourceContainer* container) {
    if (container->empty()) {
        return;
    }

    // Chain together all the stages.
    DocumentSource* prevSource = container->front().get();
    prevSource->setSource(nullptr);
    for (Pipeline::SourceContainer::iterator iter(++container->begin()), listEnd(container->end());
         iter != listEnd;
         ++iter) {
        intrusive_ptr<DocumentSource> pTemp(*iter);
        pTemp->setSource(prevSource);
        prevSource = pTemp.get();
    }
}

boost::optional<Document> Pipeline::getNext() {
    invariant(!_sources.empty());
    auto nextResult = _sources.back()->getNext();
    while (nextResult.isPaused()) {
        nextResult = _sources.back()->getNext();
    }
    return nextResult.isEOF() ? boost::none
                              : boost::optional<Document>{nextResult.releaseDocument()};
}

vector<Value> Pipeline::writeExplainOps(ExplainOptions::Verbosity verbosity) const {
    vector<Value> array;
    for (auto&& stage : _sources) {
        auto beforeSize = array.size();
        stage->serializeToArray(array, verbosity);
        auto afterSize = array.size();
        // Append execution stats to the serialized stage if the specified verbosity is
        // 'executionStats' or 'allPlansExecution'.
        invariant(afterSize - beforeSize == 1u);
        if (verbosity >= ExplainOptions::Verbosity::kExecStats) {
            auto serializedStage = array.back();
            array.back() = appendCommonExecStats(serializedStage, stage->getCommonStats());
        }
    }
    return array;
}

void Pipeline::addInitialSource(intrusive_ptr<DocumentSource> source) {
    if (!_sources.empty()) {
        _sources.front()->setSource(source.get());
    }
    _sources.push_front(source);
}

void Pipeline::addFinalSource(intrusive_ptr<DocumentSource> source) {
    if (!_sources.empty()) {
        source->setSource(_sources.back().get());
    }
    _sources.push_back(source);
}

DepsTracker Pipeline::getDependencies(
    boost::optional<QueryMetadataBitSet> unavailableMetadata) const {
    return getDependenciesForContainer(getContext(), _sources, unavailableMetadata);
}

DepsTracker Pipeline::getDependenciesForContainer(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const SourceContainer& container,
    boost::optional<QueryMetadataBitSet> unavailableMetadata) {
    // If 'unavailableMetadata' was not specified, we assume all metadata is available. This allows
    // us to call 'deps.setNeedsMetadata()' without throwing.
    DepsTracker deps(unavailableMetadata.get_value_or(DepsTracker::kNoMetadata));

    bool hasUnsupportedStage = false;
    bool knowAllFields = false;
    bool knowAllMeta = false;
    for (auto&& source : container) {
        DepsTracker localDeps(deps.getUnavailableMetadata());
        DepsTracker::State status = source->getDependencies(&localDeps);

        deps.vars.insert(localDeps.vars.begin(), localDeps.vars.end());
        deps.needRandomGenerator |= localDeps.needRandomGenerator;

        if (status == DepsTracker::State::NOT_SUPPORTED) {
            // We don't know anything about this stage, so we have to assume it depends on
            // everything. We may still know something about our dependencies if an earlier stage
            // returned EXHAUSTIVE_FIELDS or EXHAUSTIVE_META.
            hasUnsupportedStage = true;
        }

        // If we ever saw an unsupported stage, don't bother continuing to track field and metadata
        // deps: we already have to assume the pipeline depends on everything.
        if (!hasUnsupportedStage && !knowAllFields) {
            deps.fields.insert(localDeps.fields.begin(), localDeps.fields.end());
            if (localDeps.needWholeDocument)
                deps.needWholeDocument = true;
            knowAllFields = status & DepsTracker::State::EXHAUSTIVE_FIELDS;
        }

        if (!hasUnsupportedStage && !knowAllMeta) {
            deps.requestMetadata(localDeps.metadataDeps());
            knowAllMeta = status & DepsTracker::State::EXHAUSTIVE_META;
        }
    }

    if (!knowAllFields)
        deps.needWholeDocument = true;  // don't know all fields we need

    if (!deps.getUnavailableMetadata()[DocumentMetadataFields::kTextScore]) {
        // There is a text score available. If we are the first half of a split pipeline, then we
        // have to assume future stages might depend on the textScore (unless we've encountered a
        // stage that doesn't preserve metadata).
        if (expCtx->needsMerge && !knowAllMeta) {
            deps.setNeedsMetadata(DocumentMetadataFields::kTextScore, true);
        }
    } else {
        // There is no text score available, so we don't need to ask for it.
        deps.setNeedsMetadata(DocumentMetadataFields::kTextScore, false);
    }

    return deps;
}

Status Pipeline::_pipelineCanRunOnMongoS() const {
    for (auto&& stage : _sources) {
        auto constraints = stage->constraints(_splitState);
        auto hostRequirement = constraints.resolvedHostTypeRequirement(pCtx);

        const bool needsShard = (hostRequirement == HostTypeRequirement::kAnyShard ||
                                 hostRequirement == HostTypeRequirement::kPrimaryShard);

        const bool mustWriteToDisk =
            (constraints.diskRequirement == DiskUseRequirement::kWritesPersistentData);
        const bool mayWriteTmpDataAndDiskUseIsAllowed =
            (pCtx->allowDiskUse && !pCtx->opCtx->readOnly() &&
             constraints.diskRequirement == DiskUseRequirement::kWritesTmpData);
        const bool needsDisk = (mustWriteToDisk || mayWriteTmpDataAndDiskUseIsAllowed);

        const bool needsToBlock = (constraints.streamType == StreamType::kBlocking);
        const bool blockingIsPermitted = !internalQueryProhibitBlockingMergeOnMongoS.load();

        // If nothing prevents this stage from running on mongoS, continue to the next stage.
        if (!needsShard && !needsDisk && (!needsToBlock || blockingIsPermitted)) {
            continue;
        }

        // Otherwise, return an error with an explanation.
        StringBuilder ss;
        ss << stage->getSourceName();

        if (needsShard) {
            ss << " must run on a shard";
        } else if (needsToBlock && !blockingIsPermitted) {
            ss << " is a blocking stage; running these stages on mongoS is disabled";
        } else if (mustWriteToDisk) {
            ss << " must write to disk";
        } else if (mayWriteTmpDataAndDiskUseIsAllowed) {
            ss << " may write to disk when 'allowDiskUse' is enabled";
        } else {
            MONGO_UNREACHABLE;
        }

        return {ErrorCodes::IllegalOperation, ss.str()};
    }

    return Status::OK();
}

void Pipeline::pushBack(boost::intrusive_ptr<DocumentSource> newStage) {
    if (!_sources.empty()) {
        newStage->setSource(_sources.back().get());
    }
    _sources.push_back(std::move(newStage));
}

boost::intrusive_ptr<DocumentSource> Pipeline::popBack() {
    if (_sources.empty()) {
        return nullptr;
    }
    auto targetStage = _sources.back();
    _sources.pop_back();
    return targetStage;
}

boost::intrusive_ptr<DocumentSource> Pipeline::popFront() {
    if (_sources.empty()) {
        return nullptr;
    }
    auto targetStage = _sources.front();
    _sources.pop_front();
    stitch();
    return targetStage;
}

DocumentSource* Pipeline::peekFront() const {
    return _sources.empty() ? nullptr : _sources.front().get();
}

boost::intrusive_ptr<DocumentSource> Pipeline::popFrontWithName(StringData targetStageName) {
    return popFrontWithNameAndCriteria(targetStageName, nullptr);
}

boost::intrusive_ptr<DocumentSource> Pipeline::popFrontWithNameAndCriteria(
    StringData targetStageName, std::function<bool(const DocumentSource* const)> predicate) {
    if (_sources.empty() || _sources.front()->getSourceName() != targetStageName) {
        return nullptr;
    }
    auto targetStage = _sources.front();

    if (predicate && !predicate(targetStage.get())) {
        return nullptr;
    }

    return popFront();
}

std::unique_ptr<Pipeline, PipelineDeleter> Pipeline::makePipeline(
    const std::vector<BSONObj>& rawPipeline,
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const MakePipelineOptions opts) {
    auto pipeline = Pipeline::parse(rawPipeline, expCtx, opts.validator);

    if (opts.optimize) {
        pipeline->optimizePipeline();
    }

    constexpr bool alreadyOptimized = true;
    pipeline->validateCommon(alreadyOptimized);

    if (opts.attachCursorSource) {
        pipeline = expCtx->mongoProcessInterface->attachCursorSourceToPipeline(
            pipeline.release(), opts.shardTargetingPolicy, std::move(opts.readConcern));
    }

    return pipeline;
}

Pipeline::SourceContainer::iterator Pipeline::optimizeEndOfPipeline(
    Pipeline::SourceContainer::iterator itr, Pipeline::SourceContainer* container) {
    // We must create a new SourceContainer representing the subsection of the pipeline we wish to
    // optimize, since otherwise calls to optimizeAt() will overrun these limits.
    auto endOfPipeline = Pipeline::SourceContainer(std::next(itr), container->end());
    Pipeline::optimizeContainer(&endOfPipeline);
    container->erase(std::next(itr), container->end());
    container->splice(std::next(itr), endOfPipeline);

    return std::next(itr);
}

Pipeline::SourceContainer::iterator Pipeline::optimizeAtEndOfPipeline(
    Pipeline::SourceContainer::iterator itr, Pipeline::SourceContainer* container) {
    if (itr == container->end()) {
        return itr;
    }
    itr = std::next(itr);
    try {
        while (itr != container->end()) {
            invariant((*itr).get());
            itr = (*itr).get()->optimizeAt(itr, container);
        }
    } catch (DBException& ex) {
        ex.addContext("Failed to optimize pipeline");
        throw;
    }
    return itr;
}

std::unique_ptr<Pipeline, PipelineDeleter> Pipeline::makePipelineFromViewDefinition(
    const boost::intrusive_ptr<ExpressionContext>& subPipelineExpCtx,
    ExpressionContext::ResolvedNamespace resolvedNs,
    std::vector<BSONObj> currentPipeline,
    MakePipelineOptions opts) {

    // Update subpipeline's ExpressionContext with the resolved namespace.
    subPipelineExpCtx->ns = resolvedNs.ns;

    if (resolvedNs.pipeline.empty()) {
        return Pipeline::makePipeline(std::move(currentPipeline), subPipelineExpCtx, opts);
    }
    auto resolvedPipeline = std::move(resolvedNs.pipeline);

    // When we get a resolved pipeline back, we may not yet have its namespaces available in the
    // expression context, e.g. if the view's pipeline contains a $lookup on another collection.
    LiteParsedPipeline liteParsedPipeline(resolvedNs.ns, resolvedPipeline);
    subPipelineExpCtx->addResolvedNamespaces(liteParsedPipeline.getInvolvedNamespaces());

    resolvedPipeline.reserve(currentPipeline.size() + resolvedPipeline.size());
    resolvedPipeline.insert(resolvedPipeline.end(),
                            std::make_move_iterator(currentPipeline.begin()),
                            std::make_move_iterator(currentPipeline.end()));

    return Pipeline::makePipeline(std::move(resolvedPipeline), subPipelineExpCtx, opts);
}

}  // namespace mongo
