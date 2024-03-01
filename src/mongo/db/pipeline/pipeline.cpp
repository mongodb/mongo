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

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>
// IWYU pragma: no_include "ext/alloc_traits.h"
#include <algorithm>
#include <bitset>
#include <exception>
#include <iterator>
#include <ostream>
#include <string>
#include <utility>

#include "mongo/base/error_codes.h"
#include "mongo/base/exact_cast.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/util/builder_fwd.h"
#include "mongo/db/bson/dotted_path_support.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/plan_stats.h"
#include "mongo/db/matcher/expression_algo.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/aggregate_command_gen.h"
#include "mongo/db/pipeline/change_stream_helpers.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_change_stream_gen.h"
#include "mongo/db/pipeline/document_source_match.h"
#include "mongo/db/pipeline/document_source_merge.h"
#include "mongo/db/pipeline/document_source_out.h"
#include "mongo/db/pipeline/document_source_single_document_transformation.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/lite_parsed_pipeline.h"
#include "mongo/db/pipeline/resume_token.h"
#include "mongo/db/pipeline/search/search_helper.h"
#include "mongo/db/pipeline/stage_constraints.h"
#include "mongo/db/pipeline/transformer_interface.h"
#include "mongo/db/query/explain_options.h"
#include "mongo/db/query/query_knobs_gen.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/platform/compiler.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/duration.h"
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
    doc.addField("nReturned", Value(nReturned));

    invariant(stats.executionTime);
    auto executionTimeMillisEstimate = durationCount<Milliseconds>(*stats.executionTime);
    doc.addField("executionTimeMillisEstimate", Value(executionTimeMillisEstimate));
    return Value(doc.freeze());
}

/**
 * Performs validation checking specific to top-level pipelines. Throws an assertion if the
 * pipeline is invalid.
 */
void validateTopLevelPipeline(const Pipeline& pipeline) {
    // Verify that the specified namespace is valid for the initial stage of this pipeline.
    auto expCtx = pipeline.getContext();
    const NamespaceString& nss = expCtx->ns;

    const auto& sources = pipeline.getSources();

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
        const bool isChangeStream = firstStageConstraints.isChangeStreamStage();
        // Record whether any of the stages in the pipeline is a $changeStreamSplitLargeEvent.
        bool hasChangeStreamSplitLargeEventStage = false;
        for (auto&& source : sources) {
            uassert(ErrorCodes::IllegalOperation,
                    str::stream() << source->getSourceName()
                                  << " is not permitted in a $changeStream pipeline",
                    !(isChangeStream && !source->constraints().isAllowedInChangeStream()));
            // Check whether any stages must only be run in a change stream pipeline.
            uassert(ErrorCodes::IllegalOperation,
                    str::stream() << source->getSourceName()
                                  << " can only be used in a $changeStream pipeline",
                    !(source->constraints().requiresChangeStream() && !isChangeStream));
            // Check whether this is a change stream split stage.
            if ("$changeStreamSplitLargeEvent"_sd == source->getSourceName()) {
                hasChangeStreamSplitLargeEventStage = true;
            }
        }
        auto spec = isChangeStream ? expCtx->changeStreamSpec : boost::none;
        auto hasSplitEventResumeToken = spec &&
            change_stream::resolveResumeTokenFromSpec(expCtx, *spec).fragmentNum.has_value();
        uassert(ErrorCodes::ChangeStreamFatalError,
                "To resume from a split event, the $changeStream pipeline must include a "
                "$changeStreamSplitLargeEvent stage",
                !(hasSplitEventResumeToken && !hasChangeStreamSplitLargeEventStage));
    }

    // Verify that usage of $searchMeta and $search is legal. Note that on mongos, we defer this
    // check until after we've established cursors on the shards to resolve any views.
    if (expCtx->opCtx->getServiceContext() && !expCtx->inMongos) {
        search_helpers::assertSearchMetaAccessValid(sources, expCtx.get());
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
    auto expCtx = newExpCtx ? newExpCtx : getContext();
    SourceContainer clonedStages;
    for (auto&& stage : _sources) {
        clonedStages.push_back(stage->clone(expCtx));
    }
    return create(std::move(clonedStages), expCtx);
}

template <class T>
std::unique_ptr<Pipeline, PipelineDeleter> Pipeline::parseCommon(
    const std::vector<T>& rawPipeline,
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    PipelineValidatorCallback validator,
    bool isFacetPipeline,
    std::function<BSONObj(T)> getElemFunc) {

    // Before parsing the pipeline, make sure it's not so long that it will make us run out of
    // memory.
    uassert(7749501,
            str::stream() << "Pipeline length must be no longer than "
                          << internalPipelineLengthLimit << " stages.",
            static_cast<int>(rawPipeline.size()) <= internalPipelineLengthLimit);

    SourceContainer stages;
    for (auto&& stageElem : rawPipeline) {
        auto parsedSources = DocumentSource::parse(expCtx, getElemFunc(stageElem));
        stages.insert(stages.end(), parsedSources.begin(), parsedSources.end());
    }

    std::unique_ptr<Pipeline, PipelineDeleter> pipeline(new Pipeline(std::move(stages), expCtx),
                                                        PipelineDeleter(expCtx->opCtx));

    // First call the top level validator, unless this is a $facet
    // (nested) pipeline. Then call the context-specific validator if one
    // is provided.
    if (!isFacetPipeline) {
        validateTopLevelPipeline(*pipeline);
    }
    if (validator) {
        validator(*pipeline);
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

    return parseCommon<BSONElement>(rawStages, expCtx, validator, false, [](BSONElement e) {
        uassert(6253720, "Pipeline array element must be an object", e.type() == BSONType::Object);
        return e.embeddedObject();
    });
}

std::unique_ptr<Pipeline, PipelineDeleter> Pipeline::parse(
    const std::vector<BSONObj>& rawPipeline,
    const intrusive_ptr<ExpressionContext>& expCtx,
    PipelineValidatorCallback validator) {
    return parseCommon<BSONObj>(rawPipeline, expCtx, validator, false, [](BSONObj o) { return o; });
}

std::unique_ptr<Pipeline, PipelineDeleter> Pipeline::parseFacetPipeline(
    const std::vector<BSONObj>& rawPipeline,
    const intrusive_ptr<ExpressionContext>& expCtx,
    PipelineValidatorCallback validator) {
    return parseCommon<BSONObj>(rawPipeline, expCtx, validator, true, [](BSONObj o) { return o; });
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
    uassert(5054701,
            str::stream() << "Pipeline length must be no longer than "
                          << internalPipelineLengthLimit << " stages",
            static_cast<int>(_sources.size()) <= internalPipelineLengthLimit);

    checkValidOperationContext();

    // Keep track of stages which can only appear once.
    std::set<StringData> singleUseStages;

    for (auto sourceIter = _sources.begin(); sourceIter != _sources.end(); ++sourceIter) {
        auto& stage = *sourceIter;
        auto constraints = stage->constraints(_splitState);

        // Verify that all stages adhere to their PositionRequirement constraints.
        uassert(40602,
                str::stream() << stage->getSourceName()
                              << " is only valid as the first stage in a pipeline",
                !(constraints.requiredPosition == PositionRequirement::kFirst &&
                  sourceIter != _sources.begin()));

        // TODO SERVER-73790: use PositionRequirement::kCustom to validate $match.
        auto matchStage = dynamic_cast<DocumentSourceMatch*>(stage.get());
        uassert(17313,
                "$match with $text is only allowed as the first pipeline stage",
                !(sourceIter != _sources.begin() && matchStage && matchStage->isTextQuery()));

        uassert(40601,
                str::stream() << stage->getSourceName()
                              << " can only be the final stage in the pipeline",
                !(constraints.requiredPosition == PositionRequirement::kLast &&
                  std::next(sourceIter) != _sources.end()));

        // If the stage has a special requirement about its position, validate it.
        if (constraints.requiredPosition == PositionRequirement::kCustom) {
            stage->validatePipelinePosition(alreadyOptimized, sourceIter, _sources);
        }

        // Verify that we are not attempting to run a mongoS-only stage on mongoD.
        uassert(40644,
                str::stream() << stage->getSourceName() << " can only be run on mongoS",
                !(constraints.hostRequirement == HostTypeRequirement::kMongoS && !pCtx->inMongos));

        uassert(
            ErrorCodes::OperationNotSupportedInTransaction,
            str::stream() << "Stage not supported inside of a multi-document transaction: "
                          << stage->getSourceName(),
            !(pCtx->opCtx->inMultiDocumentTransaction() && !constraints.isAllowedInTransaction()));

        // Verify that a stage which can only appear once doesn't appear more than that.
        uassert(7183900,
                str::stream() << stage->getSourceName() << " can only be used once in the pipeline",
                !(constraints.canAppearOnlyOnceInPipeline &&
                  !singleUseStages.insert(stage->getSourceName()).second));

        tassert(7355707,
                "If a stage is broadcast to all shard servers then it must be a data source.",
                constraints.hostRequirement != HostTypeRequirement::kAllShardHosts ||
                    !constraints.requiresInputDocSource);
    }
}

void Pipeline::optimizePipeline() {
    // If the disablePipelineOptimization failpoint is enabled, the pipeline won't be optimized.
    if (MONGO_unlikely(disablePipelineOptimization.shouldFail())) {
        return;
    }
    optimizeContainer(&_sources);
    optimizeEachStage(&_sources);
}

void Pipeline::optimizeContainer(SourceContainer* container) {
    SourceContainer::iterator itr = container->begin();
    try {
        while (itr != container->end()) {
            invariant((*itr).get());
            itr = (*itr).get()->optimizeAt(itr, container);
        }
    } catch (DBException& ex) {
        ex.addContext("Failed to optimize pipeline");
        throw;
    }

    stitch(container);
}

void Pipeline::optimizeEachStage(SourceContainer* container) {
    SourceContainer optimizedSources;
    try {
        // We should have our final number of stages. Optimize each individually.
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

    // Check for a null operation context to make sure that all children detached correctly.
    checkValidOperationContext();
}

void Pipeline::reattachToOperationContext(OperationContext* opCtx) {
    pCtx->opCtx = opCtx;

    for (auto&& source : _sources) {
        source->reattachToOperationContext(opCtx);
    }

    checkValidOperationContext();
}

bool Pipeline::validateOperationContext(const OperationContext* opCtx) const {
    return std::all_of(_sources.begin(), _sources.end(), [this, opCtx](const auto& s) {
        // All sources in a pipeline must share its expression context. Subpipelines may have a
        // different expression context, but must point to the same operation context. Let the
        // sources validate this themselves since they don't all have the same subpipelines, etc.
        return s->getContext() == getContext() && s->validateOperationContext(opCtx);
    });
}

void Pipeline::checkValidOperationContext() const {
    tassert(7406000,
            str::stream()
                << "All DocumentSources and subpipelines must have the same operation context",
            validateOperationContext(getContext()->opCtx));
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

void Pipeline::parameterize() {
    if (!_sources.empty()) {
        if (auto matchStage = dynamic_cast<DocumentSourceMatch*>(_sources.front().get())) {
            MatchExpression::parameterize(matchStage->getMatchExpression());
            _isParameterized = true;
        }
    }
}

void Pipeline::unparameterize() {
    if (!_sources.empty()) {
        if (auto matchStage = dynamic_cast<DocumentSourceMatch*>(_sources.front().get())) {
            // Sets max param count in MatchExpression::parameterize() to 0, clearing
            // MatchExpression auto-parameterization before pipeline to ABT translation.
            MatchExpression::unparameterize(matchStage->getMatchExpression());
            _isParameterized = false;
        }
    }
}

bool Pipeline::canParameterize() {
    if (!_sources.empty()) {
        // First stage must be a DocumentSourceMatch.
        return _sources.begin()->get()->getSourceName() == DocumentSourceMatch::kStageName;
    }
    return false;
}

boost::optional<ShardId> Pipeline::needsSpecificShardMerger() const {
    for (const auto& stage : _sources) {
        if (auto mergeShardId = stage->constraints(SplitState::kSplitForMerge).mergeShardId) {
            return mergeShardId;
        }
    }
    return boost::none;
}

bool Pipeline::needsMongosMerger() const {
    return std::any_of(_sources.begin(), _sources.end(), [&](const auto& stage) {
        return stage->constraints(SplitState::kSplitForMerge).resolvedHostTypeRequirement(pCtx) ==
            HostTypeRequirement::kMongoS;
    });
}

bool Pipeline::needsAllShardHosts() const {
    return std::any_of(_sources.begin(), _sources.end(), [&](const auto& stage) {
        return stage->constraints().resolvedHostTypeRequirement(pCtx) ==
            HostTypeRequirement::kAllShardHosts;
    });
}

bool Pipeline::needsShard() const {
    return std::any_of(_sources.begin(), _sources.end(), [&](const auto& stage) {
        auto hostType = stage->constraints().resolvedHostTypeRequirement(pCtx);
        return (hostType == HostTypeRequirement::kAnyShard ||
                hostType == HostTypeRequirement::kAllShardHosts);
    });
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
            LOGV2_DEBUG(8346100,
                        1,
                        "stage {stage} is required to run on mongoS",
                        "stage"_attr = stage->getSourceName());
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
                                           boost::optional<const SerializationOptions&> opts) {
    vector<Value> serializedSources;
    for (auto&& source : container) {
        source->serializeToArray(serializedSources, opts ? opts.get() : SerializationOptions());
    }
    return serializedSources;
}

vector<Value> Pipeline::serialize(boost::optional<const SerializationOptions&> opts) const {
    return serializeContainer(_sources, opts);
}

vector<BSONObj> Pipeline::serializeToBson(boost::optional<const SerializationOptions&> opts) const {
    const auto serialized = serialize(opts);
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

vector<Value> Pipeline::writeExplainOps(const SerializationOptions& opts) const {
    vector<Value> array;
    for (auto&& stage : _sources) {
        auto beforeSize = array.size();
        stage->serializeToArray(array, opts);
        auto afterSize = array.size();
        // Append execution stats to the serialized stage if the specified verbosity is
        // 'executionStats' or 'allPlansExecution'.
        invariant(afterSize - beforeSize == 1u);
        if (*opts.verbosity >= ExplainOptions::Verbosity::kExecStats) {
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

void Pipeline::addVariableRefs(std::set<Variables::Id>* refs) const {
    for (auto&& source : _sources) {
        source->addVariableRefs(refs);
    }
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

    OrderedPathSet generatedPaths;
    bool hasUnsupportedStage = false;
    bool knowAllFields = false;
    bool knowAllMeta = false;
    for (auto&& source : container) {
        DepsTracker localDeps(deps.getUnavailableMetadata());
        DepsTracker::State status = source->getDependencies(&localDeps);

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
            for (const auto& field : localDeps.fields) {
                // If a field was generated within the pipeline, we don't need to count it as a
                // dependency of the pipeline as a whole when it is used in later stages.
                if (!expression::containsDependency({field}, generatedPaths)) {
                    deps.fields.emplace(field);
                }
            }
            if (localDeps.needWholeDocument)
                deps.needWholeDocument = true;
            knowAllFields = status & DepsTracker::State::EXHAUSTIVE_FIELDS;

            // Check if this stage modifies any fields that we should track for use by later stages.
            // Fields which are part of exclusion projections should not be marked as generated,
            // despite them being modified.
            auto localGeneratedPaths = source->getModifiedPaths();
            auto isExclusionProjection = [&]() {
                const auto projStage =
                    exact_pointer_cast<DocumentSourceSingleDocumentTransformation*>(source.get());
                return projStage &&
                    projStage->getType() ==
                    TransformerInterface::TransformerType::kExclusionProjection;
            };
            if (localGeneratedPaths.type == DocumentSource::GetModPathsReturn::Type::kFiniteSet &&
                !isExclusionProjection()) {
                auto newPathNames = localGeneratedPaths.getNewNames();
                generatedPaths.insert(newPathNames.begin(), newPathNames.end());
            }
        }

        if (!hasUnsupportedStage && !knowAllMeta) {
            deps.requestMetadata(localDeps.metadataDeps());
            knowAllMeta = status & DepsTracker::State::EXHAUSTIVE_META;
        }
        deps.requestMetadata(localDeps.searchMetadataDeps());
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

Status Pipeline::canRunOnMongos() const {
    for (auto&& stage : _sources) {
        auto constraints = stage->constraints(_splitState);
        auto hostRequirement = constraints.resolvedHostTypeRequirement(pCtx);

        const bool needsShard = (hostRequirement == HostTypeRequirement::kAnyShard ||
                                 hostRequirement == HostTypeRequirement::kAllShardHosts);

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

void Pipeline::appendPipeline(std::unique_ptr<Pipeline, PipelineDeleter> otherPipeline) {
    auto& otherPipelineSources = otherPipeline->getSources();
    while (!otherPipelineSources.empty()) {
        _sources.push_back(std::move(otherPipelineSources.front()));
        otherPipelineSources.pop_front();
    }
    constexpr bool alreadyOptimized = false;
    validateCommon(alreadyOptimized);
    stitch();
}


std::unique_ptr<Pipeline, PipelineDeleter> Pipeline::makePipeline(
    const std::vector<BSONObj>& rawPipeline,
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    MakePipelineOptions opts) {
    auto pipeline = Pipeline::parse(rawPipeline, expCtx, opts.validator);

    if (opts.optimize) {
        pipeline->optimizePipeline();
    }

    constexpr bool alreadyOptimized = true;
    pipeline->validateCommon(alreadyOptimized);

    if (opts.attachCursorSource) {
        pipeline = expCtx->mongoProcessInterface->preparePipelineForExecution(
            pipeline.release(), opts.shardTargetingPolicy, std::move(opts.readConcern));
    }

    expCtx->initializeReferencedSystemVariables();

    return pipeline;
}

std::unique_ptr<Pipeline, PipelineDeleter> Pipeline::makePipeline(
    AggregateCommandRequest& aggRequest,
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    boost::optional<BSONObj> shardCursorsSortSpec,
    const MakePipelineOptions opts) {
    tassert(7393500, "AttachCursorSource must be set to true.", opts.attachCursorSource);

    boost::optional<BSONObj> readConcern;
    // If readConcern is set on opts and aggRequest, assert they are equal.
    if (opts.readConcern && aggRequest.getReadConcern()) {
        tassert(7393501,
                "Read concern on aggRequest and makePipelineOpts must match.",
                opts.readConcern->binaryEqual(*aggRequest.getReadConcern()));
        readConcern = aggRequest.getReadConcern();
    } else {
        readConcern = aggRequest.getReadConcern() ? aggRequest.getReadConcern() : opts.readConcern;
    }

    auto pipeline = Pipeline::parse(aggRequest.getPipeline(), expCtx, opts.validator);
    if (opts.optimize) {
        pipeline->optimizePipeline();
    }

    constexpr bool alreadyOptimized = true;
    pipeline->validateCommon(alreadyOptimized);
    aggRequest.setPipeline(pipeline->serializeToBson());

    return expCtx->mongoProcessInterface->preparePipelineForExecution(aggRequest,
                                                                      pipeline.release(),
                                                                      expCtx,
                                                                      shardCursorsSortSpec,
                                                                      opts.shardTargetingPolicy,
                                                                      std::move(readConcern));
}

Pipeline::SourceContainer::iterator Pipeline::optimizeEndOfPipeline(
    Pipeline::SourceContainer::iterator itr, Pipeline::SourceContainer* container) {
    // We must create a new SourceContainer representing the subsection of the pipeline we wish to
    // optimize, since otherwise calls to optimizeAt() will overrun these limits.
    auto endOfPipeline = Pipeline::SourceContainer(std::next(itr), container->end());
    Pipeline::optimizeContainer(&endOfPipeline);
    Pipeline::optimizeEachStage(&endOfPipeline);
    container->erase(std::next(itr), container->end());
    container->splice(std::next(itr), endOfPipeline);

    return std::next(itr);
}

std::unique_ptr<Pipeline, PipelineDeleter> Pipeline::makePipelineFromViewDefinition(
    const boost::intrusive_ptr<ExpressionContext>& subPipelineExpCtx,
    ExpressionContext::ResolvedNamespace resolvedNs,
    std::vector<BSONObj> currentPipeline,
    MakePipelineOptions opts) {

    // Update subpipeline's ExpressionContext with the resolved namespace.
    subPipelineExpCtx->ns = resolvedNs.ns;

    if (resolvedNs.pipeline.empty()) {
        return Pipeline::makePipeline(currentPipeline, subPipelineExpCtx, opts);
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

    return Pipeline::makePipeline(resolvedPipeline, subPipelineExpCtx, opts);
}

}  // namespace mongo
