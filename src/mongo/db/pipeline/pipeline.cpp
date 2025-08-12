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
#include <boost/range/combine.hpp>
#include <boost/smart_ptr.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>
// IWYU pragma: no_include "ext/alloc_traits.h"
#include "mongo/base/error_codes.h"
#include "mongo/base/exact_cast.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/util/builder_fwd.h"
#include "mongo/db/exec/agg/pipeline_builder.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/plan_stats.h"
#include "mongo/db/matcher/expression_algo.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/aggregate_command_gen.h"
#include "mongo/db/pipeline/change_stream_helpers.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_match.h"
#include "mongo/db/pipeline/document_source_merge.h"
#include "mongo/db/pipeline/document_source_out.h"
#include "mongo/db/pipeline/document_source_single_document_transformation.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/lite_parsed_pipeline.h"
#include "mongo/db/pipeline/resume_token.h"
#include "mongo/db/pipeline/search/search_helper.h"
#include "mongo/db/pipeline/search/search_helper_bson_obj.h"
#include "mongo/db/pipeline/stage_constraints.h"
#include "mongo/db/pipeline/transformer_interface.h"
#include "mongo/db/query/compiler/rewrites/matcher/expression_parameterization.h"
#include "mongo/db/query/explain_options.h"
#include "mongo/db/query/plan_summary_stats_visitor.h"
#include "mongo/db/query/query_knobs_gen.h"
#include "mongo/db/query/timeseries/timeseries_translation.h"
#include "mongo/db/views/resolved_view.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/platform/compiler.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/duration.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/str.h"

#include <algorithm>
#include <exception>
#include <iterator>
#include <string>
#include <utility>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault


namespace mongo {

namespace {

/**
 * Performs validation checking specific to top-level pipelines. Throws an assertion if the
 * pipeline is invalid.
 */
void validateTopLevelPipeline(const Pipeline& pipeline) {
    // Verify that the specified namespace is valid for the initial stage of this pipeline.
    auto expCtx = pipeline.getContext();
    const NamespaceString& nss = expCtx->getNamespaceString();

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
        auto spec = isChangeStream ? expCtx->getChangeStreamSpec() : boost::none;
        auto hasSplitEventResumeToken = spec &&
            change_stream::resolveResumeTokenFromSpec(expCtx, *spec).fragmentNum.has_value();
        uassert(ErrorCodes::ChangeStreamFatalError,
                "To resume from a split event, the $changeStream pipeline must include a "
                "$changeStreamSplitLargeEvent stage",
                !(hasSplitEventResumeToken && !hasChangeStreamSplitLargeEventStage));
    }

    // Verify that usage of $searchMeta and $search is legal. Note that on routers, we defer this
    // check until after we've established cursors on the shards to resolve any views.
    if (expCtx->getOperationContext()->getServiceContext() && !expCtx->getInRouter()) {
        search_helpers::assertSearchMetaAccessValid(sources, expCtx.get());
    }
}

void validateForTimeseries(const DocumentSourceContainer* sources) {
    for (const auto& stage : *sources) {
        uassert(10557302,
                str::stream() << stage->getSourceName()
                              << " is unsupported for timeseries collections",
                stage->constraints().canRunOnTimeseries);
    }
}

}  // namespace

MONGO_FAIL_POINT_DEFINE(disablePipelineOptimization);

using boost::intrusive_ptr;

using HostTypeRequirement = StageConstraints::HostTypeRequirement;
using PositionRequirement = StageConstraints::PositionRequirement;
using DiskUseRequirement = StageConstraints::DiskUseRequirement;
using StreamType = StageConstraints::StreamType;

constexpr MatchExpressionParser::AllowedFeatureSet Pipeline::kAllowedMatcherFeatures;
constexpr MatchExpressionParser::AllowedFeatureSet Pipeline::kGeoNearMatcherFeatures;

Pipeline::Pipeline(const intrusive_ptr<ExpressionContext>& pTheCtx) : pCtx(pTheCtx) {}

Pipeline::Pipeline(DocumentSourceContainer stages, const intrusive_ptr<ExpressionContext>& expCtx)
    : _sources(std::move(stages)), pCtx(expCtx) {}

std::unique_ptr<Pipeline> Pipeline::clone(
    const boost::intrusive_ptr<ExpressionContext>& newExpCtx) const {
    auto expCtx = newExpCtx ? newExpCtx : getContext();
    DocumentSourceContainer clonedStages;
    for (auto&& stage : _sources) {
        clonedStages.push_back(stage->clone(expCtx));
    }
    return create(std::move(clonedStages), expCtx);
}

template <class T>
std::unique_ptr<Pipeline> Pipeline::parseCommon(
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

    DocumentSourceContainer stages;
    for (auto&& stageElem : rawPipeline) {
        auto parsedSources = DocumentSource::parse(expCtx, getElemFunc(stageElem));
        stages.insert(stages.end(), parsedSources.begin(), parsedSources.end());
    }

    std::unique_ptr<Pipeline> pipeline(new Pipeline(std::move(stages), expCtx));

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

    return pipeline;
}

std::unique_ptr<Pipeline> Pipeline::parseFromArray(BSONElement rawPipelineElement,
                                                   const intrusive_ptr<ExpressionContext>& expCtx,
                                                   PipelineValidatorCallback validator) {

    tassert(6253719,
            "Expected array for Pipeline::parseFromArray",
            rawPipelineElement.type() == BSONType::array);
    auto rawStages = rawPipelineElement.Array();

    return parseCommon<BSONElement>(rawStages, expCtx, validator, false, [](BSONElement e) {
        uassert(6253720, "Pipeline array element must be an object", e.type() == BSONType::object);
        return e.embeddedObject();
    });
}

std::unique_ptr<Pipeline> Pipeline::parse(const std::vector<BSONObj>& rawPipeline,
                                          const intrusive_ptr<ExpressionContext>& expCtx,
                                          PipelineValidatorCallback validator) {
    return parseCommon<BSONObj>(rawPipeline, expCtx, validator, false, [](BSONObj o) { return o; });
}

std::unique_ptr<Pipeline> Pipeline::parseFacetPipeline(
    const std::vector<BSONObj>& rawPipeline,
    const intrusive_ptr<ExpressionContext>& expCtx,
    PipelineValidatorCallback validator) {
    return parseCommon<BSONObj>(rawPipeline, expCtx, validator, true, [](BSONObj o) { return o; });
}

std::unique_ptr<Pipeline> Pipeline::create(DocumentSourceContainer stages,
                                           const intrusive_ptr<ExpressionContext>& expCtx) {
    std::unique_ptr<Pipeline> pipeline(new Pipeline(std::move(stages), expCtx));

    constexpr bool alreadyOptimized = false;
    pipeline->validateCommon(alreadyOptimized);
    return pipeline;
}

void Pipeline::validateCommon(bool alreadyOptimized) const {
    uassert(5054701,
            str::stream() << "Pipeline length must be no longer than "
                          << internalPipelineLengthLimit << " stages",
            static_cast<int>(_sources.size()) <= internalPipelineLengthLimit);

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

        // Verify that we are not attempting to run a router-only stage on a data bearing node.
        uassert(
            40644,
            str::stream() << stage->getSourceName() << " can only be run on router",
            !(constraints.hostRequirement == HostTypeRequirement::kRouter && !pCtx->getInRouter()));

        uassert(ErrorCodes::OperationNotSupportedInTransaction,
                str::stream() << "Stage not supported inside of a multi-document transaction: "
                              << stage->getSourceName(),
                !(pCtx->getOperationContext()->inMultiDocumentTransaction() &&
                  !constraints.isAllowedInTransaction()));

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

void Pipeline::validateWithCollectionMetadata(const CollectionOrViewAcquisition& collOrView) const {
    if (collOrView.collectionExists() && collOrView.getCollectionPtr()->isTimeseriesCollection()) {
        validateForTimeseries(&_sources);
    }
}

void Pipeline::validateWithCollectionMetadata(const CollectionRoutingInfo& cri) const {
    if (cri.getChunkManager().isTimeseriesCollection()) {
        validateForTimeseries(&_sources);
    }
}

void Pipeline::performPreOptimizationRewrites(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                              const CollectionRoutingInfo& cri) {
    tassert(10706511,
            "unexpected attempt to modify a frozen pipeline in "
            "'Pipeline::performPreOptimizationRewrites()'",
            !_frozen);

    // The only supported translation is for viewless timeseries collections.
    timeseries::translateStagesIfRequired(expCtx, *this, cri);
};

void Pipeline::performPreOptimizationRewrites(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                              const CollectionOrViewAcquisition& collOrView) {
    tassert(10706512,
            "unexpected attempt to modify a frozen pipeline in "
            "'Pipeline::performPreOptimizationRewrites()'",
            !_frozen);

    // The only supported translation is for viewless timeseries collections.
    timeseries::translateStagesIfRequired(expCtx, *this, collOrView);
}

void Pipeline::optimizePipeline() {
    tassert(10706501,
            "unexpected attempt to modify a frozen pipeline in 'Pipeline::optimizePipeline()'",
            !_frozen);
    // If the disablePipelineOptimization failpoint is enabled, the pipeline won't be optimized.
    if (MONGO_unlikely(disablePipelineOptimization.shouldFail())) {
        return;
    }
    optimizeContainer(&_sources);
    optimizeEachStage(&_sources);
}

void Pipeline::optimizeContainer(DocumentSourceContainer* container) {
    DocumentSourceContainer::iterator itr = container->begin();
    try {
        while (itr != container->end()) {
            invariant((*itr).get());
            itr = (*itr).get()->optimizeAt(itr, container);
        }
    } catch (DBException& ex) {
        ex.addContext("Failed to optimize pipeline");
        throw;
    }
}

void Pipeline::optimizeEachStage(DocumentSourceContainer* container) {
    DocumentSourceContainer optimizedSources;
    try {
        // We should have our final number of stages. Optimize each individually.
        for (auto&& source : *container) {
            if (auto out = source->optimize()) {
                optimizedSources.push_back(std::move(out));
            }
        }
        container->swap(optimizedSources);
    } catch (DBException& ex) {
        ex.addContext("Failed to optimize pipeline");
        throw;
    }
}

bool Pipeline::aggHasWriteStage(const BSONObj& cmd) {
    auto pipelineElement = cmd["pipeline"];
    if (pipelineElement.type() != BSONType::array) {
        return false;
    }

    for (auto stage : pipelineElement.Obj()) {
        if (stage.type() != BSONType::object) {
            return false;
        }

        if (auto obj = stage.Obj(); obj.hasField(DocumentSourceOut::kStageName) ||
            obj.hasField(DocumentSourceMerge::kStageName)) {
            return true;
        }
    }

    return false;
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
            parameterizeMatchExpression(matchStage->getMatchExpression());
            _isParameterized = true;
        }
    }
}

void Pipeline::unparameterize() {
    if (!_sources.empty()) {
        if (auto matchStage = dynamic_cast<DocumentSourceMatch*>(_sources.front().get())) {
            // Sets max param count in parameterizeMatchExpression() to 0, clearing
            // MatchExpression auto-parameterization before pipeline to ABT translation.
            unparameterizeMatchExpression(matchStage->getMatchExpression());
            _isParameterized = false;
        }
    }
}

bool Pipeline::canParameterize() const {
    if (!_sources.empty()) {
        // First stage must be a DocumentSourceMatch.
        return _sources.begin()->get()->getSourceName() == DocumentSourceMatch::kStageName;
    }
    return false;
}

boost::optional<ShardId> Pipeline::needsSpecificShardMerger() const {
    for (const auto& stage : _sources) {
        if (auto mergeShardId =
                stage->constraints(PipelineSplitState::kSplitForMerge).mergeShardId) {
            return mergeShardId;
        }
    }
    return boost::none;
}

bool Pipeline::needsRouterMerger() const {
    return std::any_of(_sources.begin(), _sources.end(), [&](const auto& stage) {
        return stage->constraints(PipelineSplitState::kSplitForMerge)
                   .resolvedHostTypeRequirement(pCtx) == HostTypeRequirement::kRouter;
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

bool Pipeline::requiredToRunOnRouter() const {
    invariant(_splitState != PipelineSplitState::kSplitForShards);

    for (auto&& stage : _sources) {
        // If this pipeline is capable of splitting before the mongoS-only stage, then the pipeline
        // as a whole is not required to run on mongoS.
        if (_splitState == PipelineSplitState::kUnsplit && stage->distributedPlanLogic()) {
            return false;
        }

        auto hostRequirement = stage->constraints(_splitState).resolvedHostTypeRequirement(pCtx);

        // If a router-only stage occurs before a splittable stage, or if the pipeline is already
        // split, this entire pipeline must run on router.
        if (hostRequirement == HostTypeRequirement::kRouter) {
            LOGV2_DEBUG(8346100,
                        1,
                        "stage {stage} is required to run on router",
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


std::vector<BSONObj> Pipeline::serializePipelineForLogging(const std::vector<BSONObj>& pipeline) {
    std::vector<BSONObj> redacted;
    for (auto&& b : pipeline) {
        redacted.push_back(redact(b));
    }
    return redacted;
}

std::vector<BSONObj> Pipeline::serializeForLogging(
    boost::optional<const SerializationOptions&> opts) const {
    std::vector<BSONObj> serialized = serializeToBson(opts);
    return serializePipelineForLogging(serialized);
}

std::vector<BSONObj> Pipeline::serializeContainerForLogging(
    const DocumentSourceContainer& container, boost::optional<const SerializationOptions&> opts) {
    std::vector<Value> serialized = serializeContainer(container, opts);
    std::vector<BSONObj> redacted;
    for (auto&& stage : serialized) {
        invariant(stage.getType() == BSONType::object);
        redacted.push_back(redact(stage.getDocument().toBson()));
    }
    return redacted;
}

std::vector<Value> Pipeline::serializeContainer(const DocumentSourceContainer& container,
                                                boost::optional<const SerializationOptions&> opts) {
    std::vector<Value> serializedSources;
    for (auto&& source : container) {
        source->serializeToArray(serializedSources, opts ? opts.get() : SerializationOptions());
    }
    return serializedSources;
}

std::vector<Value> Pipeline::serialize(boost::optional<const SerializationOptions&> opts) const {
    return serializeContainer(_sources, opts);
}

std::vector<BSONObj> Pipeline::serializeToBson(
    boost::optional<const SerializationOptions&> opts) const {
    const auto serialized = serialize(opts);
    std::vector<BSONObj> asBson;
    asBson.reserve(serialized.size());
    for (auto&& stage : serialized) {
        invariant(stage.getType() == BSONType::object);
        asBson.push_back(stage.getDocument().toBson());
    }
    return asBson;
}

std::vector<Value> Pipeline::writeExplainOps(const SerializationOptions& opts) const {
    std::vector<Value> array;
    for (auto&& stage : _sources) {
        auto beforeSize = array.size();
        stage->serializeToArray(array, opts);
        auto afterSize = array.size();
        invariant(afterSize - beforeSize == 1u);
    }
    return array;
}

void Pipeline::addInitialSource(intrusive_ptr<DocumentSource> source) {
    tassert(10706502,
            "unexpected attempt to modify a frozen pipeline in 'Pipeline::addInitialSource()'",
            !_frozen);
    _sources.push_front(source);
}

void Pipeline::addFinalSource(intrusive_ptr<DocumentSource> source) {
    tassert(10706503,
            "unexpected attempt to modify a frozen pipeline in 'Pipeline::addFinalSource()'",
            !_frozen);
    _sources.push_back(source);
}

void Pipeline::addSourceAtPosition(boost::intrusive_ptr<DocumentSource> source, size_t index) {
    tassert(10601105,
            "The index must be positive and less than or equal to the size of the source list",
            index <= _sources.size() && index >= 0);
    tassert(10706510,
            "unexpected attempt to modify a frozen pipeline in 'Pipeline::addSourceAtPosition()'",
            !_frozen);

    auto sourceIter = _sources.begin();
    std::advance(sourceIter, index);
    _sources.insert(sourceIter, source);
}

void Pipeline::addVariableRefs(std::set<Variables::Id>* refs) const {
    for (auto&& source : _sources) {
        source->addVariableRefs(refs);
    }
}

DepsTracker Pipeline::getDependencies(
    DepsTracker::MetadataDependencyValidation availableMetadata) const {
    return getDependenciesForContainer(getContext(), _sources, availableMetadata);
}

DepsTracker Pipeline::getDependenciesForContainer(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const DocumentSourceContainer& container,
    DepsTracker::MetadataDependencyValidation availableMetadata) {
    DepsTracker deps(availableMetadata);

    OrderedPathSet generatedPaths;
    bool hasUnsupportedStage = false;

    // knowAllFields / knowAllMeta means we have determined all the field / metadata dependencies of
    // the pipeline, and further stages will not affect that result.
    bool knowAllFields = false;
    bool knowAllMeta = false;

    // It's important to iterate through the stages left-to-right so that metadata validation is
    // done correctly. A stage anywhere in the pipeline may setMetadataAvailable(), but
    // references to that metadata are only valid downstream of the metadata-generating stage.
    for (auto&& source : container) {
        DepsTracker localDeps(deps.getAvailableMetadata());
        DepsTracker::State status = source->getDependencies(&localDeps);

        deps.needRandomGenerator |= localDeps.needRandomGenerator;

        if (status == DepsTracker::State::NOT_SUPPORTED) {
            // We don't know anything about this stage, so we have to assume it depends on
            // everything. We may still know something about our dependencies if an earlier stage
            // returned EXHAUSTIVE_FIELDS or EXHAUSTIVE_META.
            hasUnsupportedStage = true;
        }

        // If we ever saw an unsupported stage, don't bother continuing to track field and metadata
        // deps: we already have to assume the pipeline depends on everything. We should keep
        // tracking available metadata (by setMetadataAvailable()) so that requests to read metadata
        // (by setNeedsMetadata()) can be validated correctly.
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
                    projStage->getTransformerType() ==
                    TransformerInterface::TransformerType::kExclusionProjection;
            };
            if (localGeneratedPaths.type == DocumentSource::GetModPathsReturn::Type::kFiniteSet &&
                !isExclusionProjection()) {
                auto newPathNames = localGeneratedPaths.getNewNames();
                generatedPaths.insert(newPathNames.begin(), newPathNames.end());
            }
        }

        // This stage may have generated more available metadata; add to set of all available
        // metadata in the pipeline so we can correctly validate if downstream stages want to access
        // the metadata.
        deps.setMetadataAvailable(localDeps.getAvailableMetadata());
        if (!hasUnsupportedStage && !knowAllMeta) {
            deps.setNeedsMetadata(localDeps.metadataDeps());
            knowAllMeta = status & DepsTracker::State::EXHAUSTIVE_META;
        }
    }

    if (!knowAllFields)
        deps.needWholeDocument = true;  // don't know all fields we need

    if (expCtx->getNeedsMerge() && !knowAllMeta) {
        // There is a text score available. If we are the first half of a split pipeline, then we
        // have to assume future stages might depend on the textScore (unless we've encountered a
        // stage that doesn't preserve metadata).

        // TODO SERVER-100404: This would be more correct if we did the same for all meta fields
        // like deps.setNeedsMetadata(deps.getAvailableMetadata()).
        if (deps.getAvailableMetadata()[DocumentMetadataFields::kTextScore]) {
            deps.setNeedsMetadata(DocumentMetadataFields::kTextScore);
        }
    }

    return deps;
}

// TODO SERVER-100902 Split $meta validation out of DepsTracker.
void Pipeline::validateMetaDependencies(QueryMetadataBitSet availableMetadata) const {
    // TODO SERVER-35424 / SERVER-99965 Right now we don't validate geo near metadata here, so
    // we mark it as available. We should implement better dependency tracking for $geoNear.
    availableMetadata |= DepsTracker::kAllGeoNearData;

    DepsTracker deps(availableMetadata);
    for (auto&& source : _sources) {
        // Calls to setNeedsMetadata() inside the per-stage implementations of getDependencies() may
        // trigger a uassert if the metadata requested is not available to that stage. That is where
        // validation occurs.
        DepsTracker::State status = source->getDependencies(&deps);
        auto mayDestroyMetadata = status & DepsTracker::State::EXHAUSTIVE_META;
        if (mayDestroyMetadata) {
            // TODO SERVER-100443 Right now this only actually clears "score" and "scoreDetails",
            // but we should reset all fields to be validated in downstream stages.
            deps.clearMetadataAvailable();
        }
    }
}

bool Pipeline::generatesMetadataType(DocumentMetadataFields::MetaType type) const {
    DepsTracker deps = getDependencies(DepsTracker::kNoMetadata);
    return deps.getAvailableMetadata()[type];
}

Status Pipeline::canRunOnRouter() const {
    for (auto&& stage : _sources) {
        auto constraints = stage->constraints(_splitState);
        auto hostRequirement = constraints.resolvedHostTypeRequirement(pCtx);

        const bool needsShard = (hostRequirement == HostTypeRequirement::kAnyShard ||
                                 hostRequirement == HostTypeRequirement::kAllShardHosts);

        const bool mustWriteToDisk =
            (constraints.diskRequirement == DiskUseRequirement::kWritesPersistentData);
        const bool mayWriteTmpDataAndDiskUseIsAllowed =
            (pCtx->getAllowDiskUse() && !pCtx->getOperationContext()->readOnly() &&
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
    tassert(10706504,
            "unexpected attempt to modify a frozen pipeline in 'Pipeline::pushBack()'",
            !_frozen);
    _sources.push_back(std::move(newStage));
}

boost::intrusive_ptr<DocumentSource> Pipeline::popBack() {
    tassert(10706505,
            "unexpected attempt to modify a frozen pipeline in 'Pipeline::popBack()'",
            !_frozen);
    if (_sources.empty()) {
        return nullptr;
    }
    auto targetStage = std::move(_sources.back());
    _sources.pop_back();
    return targetStage;
}

boost::intrusive_ptr<DocumentSource> Pipeline::popFront() {
    tassert(10706506,
            "unexpected attempt to modify a frozen pipeline in 'Pipeline::popFront()'",
            !_frozen);
    if (_sources.empty()) {
        return nullptr;
    }
    auto targetStage = std::move(_sources.front());
    _sources.pop_front();
    return targetStage;
}

DocumentSource* Pipeline::peekFront() const {
    return _sources.empty() ? nullptr : _sources.front().get();
}

boost::intrusive_ptr<DocumentSource> Pipeline::popFrontWithName(StringData targetStageName) {
    tassert(10706507,
            "attempting to modify a frozen pipeline in 'Pipeline::popFrontWithName()'",
            !_frozen);
    return popFrontWithNameAndCriteria(targetStageName, nullptr);
}

boost::intrusive_ptr<DocumentSource> Pipeline::popFrontWithNameAndCriteria(
    StringData targetStageName, std::function<bool(const DocumentSource* const)> predicate) {
    tassert(10706508,
            "attempting to modify a frozen pipeline in 'Pipeline::popFrontWithNameAndCriteria()'",
            !_frozen);
    if (_sources.empty() || _sources.front()->getSourceName() != targetStageName) {
        return nullptr;
    }
    const auto& targetStage = _sources.front();

    if (predicate && !predicate(targetStage.get())) {
        return nullptr;
    }

    return popFront();
}

void Pipeline::appendPipeline(std::unique_ptr<Pipeline> otherPipeline) {
    tassert(10706509,
            "attempting to modify a frozen pipeline in 'Pipeline::appendPipeline()'",
            !_frozen);
    auto& otherPipelineSources = otherPipeline->getSources();
    while (!otherPipelineSources.empty()) {
        _sources.push_back(std::move(otherPipelineSources.front()));
        otherPipelineSources.pop_front();
    }
    constexpr bool alreadyOptimized = false;
    validateCommon(alreadyOptimized);
}


std::unique_ptr<Pipeline> Pipeline::makePipeline(
    const std::vector<BSONObj>& rawPipeline,
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    MakePipelineOptions opts) {
    auto pipeline = Pipeline::parse(rawPipeline, expCtx, opts.validator);

    expCtx->initializeReferencedSystemVariables();

    bool alreadyOptimized = opts.alreadyOptimized;

    if (opts.optimize) {
        pipeline->optimizePipeline();
        alreadyOptimized = true;
    }

    pipeline->validateCommon(alreadyOptimized);

    if (opts.attachCursorSource) {
        // Creating AggregateCommandRequest in order to pass all necessary 'opts' to the
        // preparePipelineForExecution().
        AggregateCommandRequest aggRequest(expCtx->getNamespaceString(),
                                           pipeline->serializeToBson());
        pipeline = expCtx->getMongoProcessInterface()->preparePipelineForExecution(
            expCtx,
            aggRequest,
            pipeline.release(),
            boost::none /* shardCursorsSortSpec */,
            opts.shardTargetingPolicy,
            std::move(opts.readConcern),
            opts.useCollectionDefaultCollator);
    }

    return pipeline;
}

std::unique_ptr<Pipeline> Pipeline::makePipeline(
    AggregateCommandRequest& aggRequest,
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    boost::optional<BSONObj> shardCursorsSortSpec,
    const MakePipelineOptions opts) {
    tassert(10892201,
            "shardCursorsSortSpec must not be set if attachCursorSource is false.",
            opts.attachCursorSource || shardCursorsSortSpec == boost::none);

    boost::optional<BSONObj> readConcern;
    // If readConcern is set on opts and aggRequest, assert they are equal.
    if (opts.readConcern && aggRequest.getReadConcern()) {
        readConcern = aggRequest.getReadConcern()->toBSONInner();
        tassert(7393501,
                "Read concern on aggRequest and makePipelineOpts must match.",
                opts.readConcern->binaryEqual(*readConcern));
    } else {
        readConcern = aggRequest.getReadConcern() ? aggRequest.getReadConcern()->toBSONInner()
                                                  : opts.readConcern;
    }

    auto pipeline = Pipeline::parse(aggRequest.getPipeline(), expCtx, opts.validator);
    if (opts.optimize) {
        pipeline->optimizePipeline();
    }

    constexpr bool alreadyOptimized = true;
    pipeline->validateCommon(alreadyOptimized);
    aggRequest.setPipeline(pipeline->serializeToBson());

    if (opts.attachCursorSource) {
        pipeline = expCtx->getMongoProcessInterface()->preparePipelineForExecution(
            expCtx,
            aggRequest,
            pipeline.release(),
            shardCursorsSortSpec,
            opts.shardTargetingPolicy,
            std::move(readConcern),
            opts.useCollectionDefaultCollator);
    }

    return pipeline;
}

DocumentSourceContainer::iterator Pipeline::optimizeEndOfPipeline(
    DocumentSourceContainer::iterator itr, DocumentSourceContainer* container) {
    // We must create a new DocumentSourceContainer representing the subsection of the pipeline we
    // wish to optimize, since otherwise calls to optimizeAt() will overrun these limits.
    auto endOfPipeline = DocumentSourceContainer(std::next(itr), container->end());
    Pipeline::optimizeContainer(&endOfPipeline);
    Pipeline::optimizeEachStage(&endOfPipeline);
    container->erase(std::next(itr), container->end());
    container->splice(std::next(itr), endOfPipeline);

    return std::next(itr);
}

std::unique_ptr<Pipeline> Pipeline::viewPipelineHelperForSearch(
    const boost::intrusive_ptr<ExpressionContext>& subPipelineExpCtx,
    ResolvedNamespace resolvedNs,
    std::vector<BSONObj> currentPipeline,
    MakePipelineOptions opts,
    NamespaceString originalNs) {
    // Search queries on mongot-indexed views behave differently than non-search aggregations on
    // views. When a user pipeline contains a $search/$vectorSearch stage, idLookup will apply the
    // view transforms as part of its subpipeline. In this way, the view stages will always
    // be applied directly after $_internalSearchMongotRemote and before the remaining
    // stages of the user pipeline. This is to ensure the stages following
    // $search/$vectorSearch in the user pipeline will receive the modified documents: when
    // storedSource is disabled, idLookup will retrieve full/unmodified documents during
    // (from the _id values returned by mongot), apply the view's data transforms, and pass
    // said transformed documents through the rest of the user pipeline.
    const ResolvedView resolvedView{resolvedNs.ns, resolvedNs.pipeline, BSONObj()};
    subPipelineExpCtx->setView(
        boost::make_optional(std::make_pair(originalNs, resolvedView.getPipeline())));

    // return the user pipeline without appending the view stages.
    return Pipeline::makePipeline(currentPipeline, subPipelineExpCtx, opts);
}
std::unique_ptr<Pipeline> Pipeline::makePipelineFromViewDefinition(
    const boost::intrusive_ptr<ExpressionContext>& subPipelineExpCtx,
    ResolvedNamespace resolvedNs,
    std::vector<BSONObj> currentPipeline,
    MakePipelineOptions opts,
    NamespaceString originalNs) {

    // Update subpipeline's ExpressionContext with the resolved namespace.
    subPipelineExpCtx->setNamespaceString(resolvedNs.ns);

    if (resolvedNs.pipeline.empty()) {
        return Pipeline::makePipeline(currentPipeline, subPipelineExpCtx, opts);
    }

    if (search_helper_bson_obj::isMongotPipeline(currentPipeline)) {
        return Pipeline::viewPipelineHelperForSearch(
            subPipelineExpCtx, resolvedNs, currentPipeline, opts, originalNs);
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

void Pipeline::detachFromOperationContext() {
    pCtx->setOperationContext(nullptr);
    for (auto&& source : _sources) {
        source->detachSourceFromOperationContext();
    }
    checkValidOperationContext();
}

void Pipeline::reattachToOperationContext(OperationContext* opCtx) {
    pCtx->setOperationContext(opCtx);
    for (auto&& source : _sources) {
        source->reattachSourceToOperationContext(opCtx);
    }
    checkValidOperationContext();
}

bool Pipeline::validateOperationContext(const OperationContext* opCtx) const {
    return std::all_of(_sources.begin(), _sources.end(), [this, opCtx](const auto& source) {
        // All sources in a pipeline must share its expression context. Subpipelines may have a
        // different expression context, but must point to the same operation context. Let the
        // sources validate this themselves since they don't all have the same subpipelines, etc.
        return source->getExpCtx() == getContext() && source->validateSourceOperationContext(opCtx);
    });
}

void Pipeline::checkValidOperationContext() const {
    tassert(10713712,
            str::stream()
                << "All DocumentSources and subpipelines must have the same operation context",
            validateOperationContext(getContext()->getOperationContext()));
}

}  // namespace mongo
