/**
 * Copyright (c) 2011 10gen Inc.
 *
 * This program is free software: you can redistribute it and/or  modify
 * it under the terms of the GNU Affero General Public License, version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, the copyright holders give permission to link the
 * code of portions of this program with the OpenSSL library under certain
 * conditions as described in each individual source file and distribute
 * linked combinations including the program with the OpenSSL library. You
 * must comply with the GNU Affero General Public License in all respects for
 * all of the code used other than as permitted herein. If you modify file(s)
 * with this exception, you may extend this exception to your version of the
 * file(s), but you are not obligated to do so. If you do not wish to do so,
 * delete this exception statement from your version. If you delete this
 * exception statement from all source files in the program, then also delete
 * it in the license file.
 */

#include "mongo/platform/basic.h"

// This file defines functions from both of these headers
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/pipeline/pipeline_optimizations.h"

#include <algorithm>

#include "mongo/base/error_codes.h"
#include "mongo/db/bson/dotted_path_support.h"
#include "mongo/db/catalog/document_validation.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/accumulator.h"
#include "mongo/db/pipeline/document.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_geo_near.h"
#include "mongo/db/pipeline/document_source_match.h"
#include "mongo/db/pipeline/document_source_merge_cursors.h"
#include "mongo/db/pipeline/document_source_out.h"
#include "mongo/db/pipeline/document_source_project.h"
#include "mongo/db/pipeline/document_source_unwind.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

using boost::intrusive_ptr;
using std::endl;
using std::ostringstream;
using std::string;
using std::vector;

namespace dps = ::mongo::dotted_path_support;

using ChangeStreamRequirement = DocumentSource::StageConstraints::ChangeStreamRequirement;
using HostTypeRequirement = DocumentSource::StageConstraints::HostTypeRequirement;
using PositionRequirement = DocumentSource::StageConstraints::PositionRequirement;
using DiskUseRequirement = DocumentSource::StageConstraints::DiskUseRequirement;
using FacetRequirement = DocumentSource::StageConstraints::FacetRequirement;
using StreamType = DocumentSource::StageConstraints::StreamType;

Pipeline::Pipeline(const intrusive_ptr<ExpressionContext>& pTheCtx) : pCtx(pTheCtx) {}

Pipeline::Pipeline(SourceContainer stages, const intrusive_ptr<ExpressionContext>& expCtx)
    : _sources(std::move(stages)), pCtx(expCtx) {}

Pipeline::~Pipeline() {
    invariant(_disposed);
}

StatusWith<std::unique_ptr<Pipeline, Pipeline::Deleter>> Pipeline::parse(
    const std::vector<BSONObj>& rawPipeline, const intrusive_ptr<ExpressionContext>& expCtx) {
    return parseTopLevelOrFacetPipeline(rawPipeline, expCtx, false);
}

StatusWith<std::unique_ptr<Pipeline, Pipeline::Deleter>> Pipeline::parseFacetPipeline(
    const std::vector<BSONObj>& rawPipeline, const intrusive_ptr<ExpressionContext>& expCtx) {
    return parseTopLevelOrFacetPipeline(rawPipeline, expCtx, true);
}

StatusWith<std::unique_ptr<Pipeline, Pipeline::Deleter>> Pipeline::parseTopLevelOrFacetPipeline(
    const std::vector<BSONObj>& rawPipeline,
    const intrusive_ptr<ExpressionContext>& expCtx,
    const bool isFacetPipeline) {

    SourceContainer stages;

    for (auto&& stageObj : rawPipeline) {
        auto parsedSources = DocumentSource::parse(expCtx, stageObj);
        stages.insert(stages.end(), parsedSources.begin(), parsedSources.end());
    }

    return createTopLevelOrFacetPipeline(std::move(stages), expCtx, isFacetPipeline);
}

StatusWith<std::unique_ptr<Pipeline, Pipeline::Deleter>> Pipeline::create(
    SourceContainer stages, const intrusive_ptr<ExpressionContext>& expCtx) {
    return createTopLevelOrFacetPipeline(std::move(stages), expCtx, false);
}

StatusWith<std::unique_ptr<Pipeline, Pipeline::Deleter>> Pipeline::createFacetPipeline(
    SourceContainer stages, const intrusive_ptr<ExpressionContext>& expCtx) {
    return createTopLevelOrFacetPipeline(std::move(stages), expCtx, true);
}

StatusWith<std::unique_ptr<Pipeline, Pipeline::Deleter>> Pipeline::createTopLevelOrFacetPipeline(
    SourceContainer stages,
    const intrusive_ptr<ExpressionContext>& expCtx,
    const bool isFacetPipeline) {
    std::unique_ptr<Pipeline, Pipeline::Deleter> pipeline(new Pipeline(std::move(stages), expCtx),
                                                          Pipeline::Deleter(expCtx->opCtx));
    try {
        if (isFacetPipeline) {
            pipeline->validateFacetPipeline();
        } else {
            pipeline->validatePipeline();
        }
    } catch (const DBException& ex) {
        return ex.toStatus();
    }

    pipeline->stitch();
    return std::move(pipeline);
}

void Pipeline::validatePipeline() const {
    // Verify that the specified namespace is valid for the initial stage of this pipeline.
    const NamespaceString& nss = pCtx->ns;

    if (_sources.empty()) {
        if (nss.isCollectionlessAggregateNS()) {
            uasserted(ErrorCodes::InvalidNamespace,
                      "{aggregate: 1} is not valid for an empty pipeline.");
        }
    } else if (!dynamic_cast<DocumentSourceMergeCursors*>(_sources.front().get())) {
        // The $mergeCursors stage can take {aggregate: 1} or a normal namespace. Aside from this,
        // {aggregate: 1} is only valid for collectionless sources, and vice-versa.
        const auto firstStageConstraints = _sources.front()->constraints(_splitState);

        if (nss.isCollectionlessAggregateNS() &&
            !firstStageConstraints.isIndependentOfAnyCollection) {
            uasserted(ErrorCodes::InvalidNamespace,
                      str::stream() << "{aggregate: 1} is not valid for '"
                                    << _sources.front()->getSourceName()
                                    << "'; a collection is required.");
        }

        if (!nss.isCollectionlessAggregateNS() &&
            firstStageConstraints.isIndependentOfAnyCollection) {
            uasserted(ErrorCodes::InvalidNamespace,
                      str::stream() << "'" << _sources.front()->getSourceName()
                                    << "' can only be run with {aggregate: 1}");
        }

        // If the first stage is a $changeStream stage, then all stages in the pipeline must be
        // either $changeStream stages or whitelisted as being able to run in a change stream.
        if (firstStageConstraints.isChangeStreamStage()) {
            for (auto&& source : _sources) {
                uassert(ErrorCodes::IllegalOperation,
                        str::stream() << source->getSourceName()
                                      << " is not permitted in a $changeStream pipeline",
                        source->constraints(_splitState).isAllowedInChangeStream());
            }
        }
    }

    // Verify that each stage is in a legal position within the pipeline.
    ensureAllStagesAreInLegalPositions();
}

void Pipeline::validateFacetPipeline() const {
    if (_sources.empty()) {
        uasserted(ErrorCodes::BadValue, "sub-pipeline in $facet stage cannot be empty");
    }

    for (auto&& stage : _sources) {
        auto stageConstraints = stage->constraints(_splitState);
        if (!stageConstraints.isAllowedInsideFacetStage()) {
            uasserted(40600,
                      str::stream() << stage->getSourceName()
                                    << " is not allowed to be used within a $facet stage");
        }
        // We expect a stage within a $facet stage to have these properties.
        invariant(stageConstraints.requiredPosition == PositionRequirement::kNone);
        invariant(!stageConstraints.isIndependentOfAnyCollection);
    }

    // Facet pipelines cannot have any stages which are initial sources. We've already validated the
    // first stage, and the 'ensureAllStagesAreInLegalPositions' method checks that there are no
    // initial sources in positions 1...N, so we can just return its result directly.
    ensureAllStagesAreInLegalPositions();
}

void Pipeline::ensureAllStagesAreInLegalPositions() const {
    size_t i = 0;
    for (auto&& stage : _sources) {
        auto constraints = stage->constraints(_splitState);

        // Verify that all stages adhere to their PositionRequirement constraints.
        if (constraints.requiredPosition == PositionRequirement::kFirst && i != 0) {
            uasserted(40602,
                      str::stream() << stage->getSourceName()
                                    << " is only valid as the first stage in a pipeline.");
        }
        auto matchStage = dynamic_cast<DocumentSourceMatch*>(stage.get());
        if (i != 0 && matchStage && matchStage->isTextQuery()) {
            uasserted(17313, "$match with $text is only allowed as the first pipeline stage");
        }

        if (constraints.requiredPosition == PositionRequirement::kLast &&
            i != _sources.size() - 1) {
            uasserted(40601,
                      str::stream() << stage->getSourceName()
                                    << " can only be the final stage in the pipeline");
        }
        ++i;

        // Verify that we are not attempting to run a mongoS-only stage on mongoD.
        uassert(40644,
                str::stream() << stage->getSourceName() << " can only be run on mongoS",
                !(constraints.hostRequirement == HostTypeRequirement::kMongoS && !pCtx->inMongos));
    }
}

void Pipeline::optimizePipeline() {
    SourceContainer optimizedSources;

    SourceContainer::iterator itr = _sources.begin();

    // We could be swapping around stages during this process, so disconnect the pipeline to prevent
    // us from entering a state with dangling pointers.
    unstitch();
    while (itr != _sources.end()) {
        invariant((*itr).get());
        itr = (*itr).get()->optimizeAt(itr, &_sources);
    }

    // Once we have reached our final number of stages, optimize each individually.
    for (auto&& source : _sources) {
        if (auto out = source->optimize()) {
            optimizedSources.push_back(out);
        }
    }
    _sources.swap(optimizedSources);
    stitch();
}

bool Pipeline::aggSupportsWriteConcern(const BSONObj& cmd) {
    auto pipelineElement = cmd["pipeline"];
    if (pipelineElement.type() != BSONType::Array) {
        return false;
    }

    for (auto stage : pipelineElement.Obj()) {
        if (stage.type() != BSONType::Object) {
            return false;
        }

        if (stage.Obj().hasField("$out")) {
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

std::unique_ptr<Pipeline, Pipeline::Deleter> Pipeline::splitForSharded() {
    invariant(!isSplitForShards());
    invariant(!isSplitForMerge());
    invariant(!_unsplitSources);

    // Create and initialize the shard spec we'll return. We start with an empty pipeline on the
    // shards and all work being done in the merger. Optimizations can move operations between
    // the pipelines to be more efficient.
    std::unique_ptr<Pipeline, Pipeline::Deleter> shardPipeline(new Pipeline(pCtx),
                                                               Pipeline::Deleter(pCtx->opCtx));

    // Keep a copy of the original source list in case we need to reset the pipeline from split to
    // unsplit later.
    shardPipeline->_unsplitSources.emplace(_sources);

    // The order in which optimizations are applied can have significant impact on the
    // efficiency of the final pipeline. Be Careful!
    Optimizations::Sharded::findSplitPoint(shardPipeline.get(), this);
    Optimizations::Sharded::moveFinalUnwindFromShardsToMerger(shardPipeline.get(), this);
    Optimizations::Sharded::limitFieldsSentFromShardsToMerger(shardPipeline.get(), this);

    shardPipeline->_splitState = SplitState::kSplitForShards;
    _splitState = SplitState::kSplitForMerge;

    stitch();

    return shardPipeline;
}

void Pipeline::unsplitFromSharded(
    std::unique_ptr<Pipeline, Pipeline::Deleter> pipelineForMergingShard) {
    invariant(isSplitForShards());
    invariant(!isSplitForMerge());
    invariant(pipelineForMergingShard);
    invariant(_unsplitSources);

    // Clear the merge source list so that destroying the pipeline object won't dispose of the
    // stages. We still have a reference to each of the stages which will be moved back to the shard
    // pipeline via '_unsplitSources'.
    pipelineForMergingShard->_sources.clear();
    pipelineForMergingShard.reset();

    // Set '_sources' to its original state, re-stitch, and clear the '_unsplitSources' optional.
    _sources = *_unsplitSources;
    _unsplitSources.reset();

    _splitState = SplitState::kUnsplit;

    stitch();
}

void Pipeline::Optimizations::Sharded::findSplitPoint(Pipeline* shardPipe, Pipeline* mergePipe) {
    while (!mergePipe->_sources.empty()) {
        intrusive_ptr<DocumentSource> current = mergePipe->_sources.front();
        mergePipe->_sources.pop_front();

        // Check if this source is splittable.
        SplittableDocumentSource* splittable =
            dynamic_cast<SplittableDocumentSource*>(current.get());

        if (!splittable) {
            // Move the source from the merger _sources to the shard _sources.
            shardPipe->_sources.push_back(current);
        } else {
            // Split this source into 'merge' and 'shard' _sources.
            intrusive_ptr<DocumentSource> shardSource = splittable->getShardSource();
            auto mergeSources = splittable->getMergeSources();

            // A source may not simultaneously be present on both sides of the split.
            invariant(std::find(mergeSources.begin(), mergeSources.end(), shardSource) ==
                      mergeSources.end());

            if (shardSource)
                shardPipe->_sources.push_back(shardSource);

            // Add the stages in reverse order, so that they appear in the pipeline in the same
            // order as they were returned by the stage.
            for (auto it = mergeSources.rbegin(); it != mergeSources.rend(); ++it) {
                mergePipe->_sources.push_front(*it);
            }

            break;
        }
    }
}

void Pipeline::Optimizations::Sharded::moveFinalUnwindFromShardsToMerger(Pipeline* shardPipe,
                                                                         Pipeline* mergePipe) {
    while (!shardPipe->_sources.empty() &&
           dynamic_cast<DocumentSourceUnwind*>(shardPipe->_sources.back().get())) {
        mergePipe->_sources.push_front(shardPipe->_sources.back());
        shardPipe->_sources.pop_back();
    }
}

void Pipeline::Optimizations::Sharded::limitFieldsSentFromShardsToMerger(Pipeline* shardPipe,
                                                                         Pipeline* mergePipe) {
    auto depsMetadata = DocumentSourceMatch::isTextQuery(shardPipe->getInitialQuery())
        ? DepsTracker::MetadataAvailable::kTextScore
        : DepsTracker::MetadataAvailable::kNoMetadata;
    DepsTracker mergeDeps(mergePipe->getDependencies(depsMetadata));
    if (mergeDeps.needWholeDocument)
        return;  // the merge needs all fields, so nothing we can do.

    // Empty project is "special" so if no fields are needed, we just ask for _id instead.
    if (mergeDeps.fields.empty())
        mergeDeps.fields.insert("_id");

    // Remove metadata from dependencies since it automatically flows through projection and we
    // don't want to project it in to the document.
    mergeDeps.setNeedTextScore(false);

    // HEURISTIC: only apply optimization if none of the shard stages have an exhaustive list of
    // field dependencies. While this may not be 100% ideal in all cases, it is simple and
    // avoids the worst cases by ensuring that:
    // 1) Optimization IS applied when the shards wouldn't have known their exhaustive list of
    //    dependencies. This situation can happen when a $sort is before the first $project or
    //    $group. Without the optimization, the shards would have to reify and transmit full
    //    objects even though only a subset of fields are needed.
    // 2) Optimization IS NOT applied immediately following a $project or $group since it would
    //    add an unnecessary project (and therefore a deep-copy).
    for (auto&& source : shardPipe->_sources) {
        DepsTracker dt(depsMetadata);
        if (source->getDependencies(&dt) & DocumentSource::EXHAUSTIVE_FIELDS)
            return;
    }
    // if we get here, add the project.
    boost::intrusive_ptr<DocumentSource> project = DocumentSourceProject::createFromBson(
        BSON("$project" << mergeDeps.toProjection()).firstElement(), shardPipe->pCtx);
    shardPipe->_sources.push_back(project);
}

BSONObj Pipeline::getInitialQuery() const {
    if (_sources.empty())
        return BSONObj();

    /* look for an initial $match */
    DocumentSourceMatch* match = dynamic_cast<DocumentSourceMatch*>(_sources.front().get());
    if (match) {
        return match->getQuery();
    }

    DocumentSourceGeoNear* geoNear = dynamic_cast<DocumentSourceGeoNear*>(_sources.front().get());
    if (geoNear) {
        return geoNear->getQuery();
    }

    return BSONObj();
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

bool Pipeline::canRunOnMongos() const {
    return _pipelineCanRunOnMongoS().isOK();
}

bool Pipeline::requiredToRunOnMongos() const {
    invariant(!isSplitForShards());

    for (auto&& stage : _sources) {
        // If this pipeline is capable of splitting before the mongoS-only stage, then the pipeline
        // as a whole is not required to run on mongoS.
        if (isUnsplit() && dynamic_cast<SplittableDocumentSource*>(stage.get())) {
            return false;
        }

        auto hostRequirement = stage->constraints(_splitState).resolvedHostTypeRequirement(pCtx);

        // If a mongoS-only stage occurs before a splittable stage, or if the pipeline is already
        // split, this entire pipeline must run on mongoS.
        if (hostRequirement == HostTypeRequirement::kMongoS) {
            // Verify that the remainder of this pipeline can run on mongoS.
            auto mongosRunStatus = _pipelineCanRunOnMongoS();

            uassert(mongosRunStatus.code(),
                    str::stream() << stage->getSourceName() << " must run on mongoS, but "
                                  << mongosRunStatus.reason(),
                    mongosRunStatus.isOK());

            return true;
        }
    }

    return false;
}

std::vector<NamespaceString> Pipeline::getInvolvedCollections() const {
    std::vector<NamespaceString> collections;
    for (auto&& source : _sources) {
        source->addInvolvedCollections(&collections);
    }
    return collections;
}

vector<Value> Pipeline::serialize() const {
    vector<Value> serializedSources;
    for (auto&& source : _sources) {
        source->serializeToArray(serializedSources);
    }
    return serializedSources;
}

void Pipeline::unstitch() {
    for (auto&& stage : _sources) {
        stage->setSource(nullptr);
    }
}

void Pipeline::stitch() {
    if (_sources.empty()) {
        return;
    }
    // Chain together all the stages.
    DocumentSource* prevSource = _sources.front().get();
    prevSource->setSource(nullptr);
    for (SourceContainer::iterator iter(++_sources.begin()), listEnd(_sources.end());
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
    for (SourceContainer::const_iterator it = _sources.begin(); it != _sources.end(); ++it) {
        (*it)->serializeToArray(array, verbosity);
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

DepsTracker Pipeline::getDependencies(DepsTracker::MetadataAvailable metadataAvailable) const {
    DepsTracker deps(metadataAvailable);
    const bool scopeHasVariables = pCtx->variablesParseState.hasDefinedVariables();
    bool skipFieldsAndMetadataDeps = false;
    bool knowAllFields = false;
    bool knowAllMeta = false;
    for (auto&& source : _sources) {
        DepsTracker localDeps(deps.getMetadataAvailable());
        DocumentSource::GetDepsReturn status = source->getDependencies(&localDeps);

        deps.vars.insert(localDeps.vars.begin(), localDeps.vars.end());

        if ((skipFieldsAndMetadataDeps |= (status == DocumentSource::NOT_SUPPORTED))) {
            // Assume this stage needs everything. We may still know something about our
            // dependencies if an earlier stage returned EXHAUSTIVE_FIELDS or EXHAUSTIVE_META. If
            // this scope has variables, we need to keep enumerating the remaining stages but will
            // skip adding any further field or metadata dependencies.
            if (scopeHasVariables) {
                continue;
            } else {
                break;
            }
        }

        if (!knowAllFields) {
            deps.fields.insert(localDeps.fields.begin(), localDeps.fields.end());
            if (localDeps.needWholeDocument)
                deps.needWholeDocument = true;
            knowAllFields = status & DocumentSource::EXHAUSTIVE_FIELDS;
        }

        if (!knowAllMeta) {
            if (localDeps.getNeedTextScore())
                deps.setNeedTextScore(true);

            if (localDeps.getNeedSortKey())
                deps.setNeedSortKey(true);

            knowAllMeta = status & DocumentSource::EXHAUSTIVE_META;
        }

        // If there are variables defined at this pipeline's scope, there may be dependencies upon
        // them in subsequent stages. Keep enumerating.
        if (knowAllMeta && knowAllFields && !scopeHasVariables) {
            break;
        }
    }

    if (!knowAllFields)
        deps.needWholeDocument = true;  // don't know all fields we need

    if (metadataAvailable & DepsTracker::MetadataAvailable::kTextScore) {
        // If there is a text score, assume we need to keep it if we can't prove we don't. If we are
        // the first half of a pipeline which has been split, future stages might need it.
        if (!knowAllMeta)
            deps.setNeedTextScore(true);
    } else {
        // If there is no text score available, then we don't need to ask for it.
        deps.setNeedTextScore(false);
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
            (pCtx->allowDiskUse &&
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

boost::intrusive_ptr<DocumentSource> Pipeline::popFrontWithCriteria(
    StringData targetStageName, stdx::function<bool(const DocumentSource* const)> predicate) {
    if (_sources.empty() || _sources.front()->getSourceName() != targetStageName) {
        return nullptr;
    }
    auto targetStage = _sources.front();

    if (predicate && !predicate(targetStage.get())) {
        return nullptr;
    }

    _sources.pop_front();
    stitch();
    return targetStage;
}
}  // namespace mongo
