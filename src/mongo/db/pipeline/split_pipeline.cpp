/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include "mongo/db/pipeline/split_pipeline.h"

#include "mongo/db/pipeline/document_source_group.h"
#include "mongo/db/pipeline/document_source_project.h"
#include "mongo/db/pipeline/document_source_sequential_document_cache.h"
#include "mongo/db/pipeline/document_source_skip.h"
#include "mongo/db/pipeline/document_source_sort.h"
#include "mongo/db/pipeline/document_source_unwind.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/pipeline/semantic_analysis.h"
#include "mongo/db/query/compiler/dependency_analysis/dependencies.h"

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {
namespace sharded_agg_helpers {
namespace {
boost::optional<BSONObj> getOwnedOrNone(boost::optional<BSONObj> obj) {
    if (obj) {
        return obj->getOwned();
    }
    return boost::none;
}

/**
 * Returns true if the final stage of the pipeline limits the number of documents it could output
 * (such as a $limit stage).
 *
 * This function is not meant to exhaustively catch every single case where a pipeline might have
 * some kind of limit. It's only here so that propagateDocLimitsToShards() can avoid adding an
 * obviously unnecessary $limit to a shard's pipeline.
 */
boost::optional<long long> getPipelineLimit(Pipeline* pipeline) {
    for (auto source_it = pipeline->getSources().crbegin();
         source_it != pipeline->getSources().crend();
         ++source_it) {
        const auto source = source_it->get();

        auto limitStage = dynamic_cast<DocumentSourceLimit*>(source);
        if (limitStage) {
            return limitStage->getLimit();
        }

        auto sortStage = dynamic_cast<DocumentSourceSort*>(source);
        if (sortStage) {
            return sortStage->getLimit();
        }

        auto cursorStage = dynamic_cast<DocumentSourceSort*>(source);
        if (cursorStage) {
            return cursorStage->getLimit();
        }

        // If this stage is one that can swap with a $limit stage, then we can look at the previous
        // stage to see if it includes a limit. Otherwise, we give up trying to find a limit on this
        // stage's output.
        if (!source->constraints().canSwapWithSkippingOrLimitingStage) {
            break;
        }
    }

    return boost::none;
}
}  // namespace

/**
 * Serves as a factory to create a SplitPipeline. Contains helper functions and holds
 * intermediate state to split a pipeline. After calling split(), release() can be called to
 * release the constructed SplitPipeline.
 */
class PipelineSplitter {
public:
    PipelineSplitter(std::unique_ptr<Pipeline> pipelineToSplit,
                     boost::optional<OrderedPathSet> shardKeyPaths)
        : _splitPipeline(
              SplitPipeline::mergeOnlyWithEmptyShardsPipeline(std::move(pipelineToSplit))),

          _initialShardKeyPaths(std::move(shardKeyPaths)) {}

    PipelineSplitter(const PipelineSplitter& other) = delete;
    ~PipelineSplitter() = default;

    /**
     * Splits the pipeline. Adds as many stages as possible from the merge pipeline to the
     * shards pipeline, until a stage needs to be split.
     */
    PipelineSplitter& split() {
        // Before splitting the pipeline, we need to do dependency analysis to validate if we have
        // text score metadata. This is because the planner will not have any way of knowing
        // whether the split half provides this metadata after shards are targeted, because the
        // shard executing the merging half only sees a $mergeCursors stage.
        _splitPipeline.mergePipeline->validateMetaDependencies();

        // We will move stages one by one from the merging half to the shards, as possible.
        _findSplitPoint();

        // The order in which optimizations are applied can have significant impact on the
        // efficiency of the final pipeline. Be Careful!
        _moveEligibleStreamingStagesBeforeSortOnShards();
        if (_splitPipeline.mergePipeline->getContext()
                ->isFeatureFlagShardFilteringDistinctScanEnabled()) {
            _moveGroupFollowingSortFromMergerToShards();
        }
        _moveFinalUnwindFromShardsToMerger();
        _propagateDocLimitToShards();
        _limitFieldsSentFromShardsToMerger();

        _abandonCacheIfSentToShards();
        _splitPipeline.shardsPipeline->setSplitState(PipelineSplitState::kSplitForShards);
        _splitPipeline.mergePipeline->setSplitState(PipelineSplitState::kSplitForMerge);

        return *this;
    };

    /**
     * Releases its internal SplitPipeline.
     */
    SplitPipeline release() {
        return std::move(_splitPipeline);
    }

private:
    /**
     * Checks if any document sources remain in the merge pipeline.
     */
    bool _mergePipeHasNext() const {
        return !_splitPipeline.mergePipeline->empty();
    }

    /**
     * Pops and returns the first document source (together with its distributed plan logic) from
     * the merge pipeline.
     */
    auto _popFirstMergeStage() {
        boost::intrusive_ptr<DocumentSource> current = _splitPipeline.mergePipeline->popFront();
        return std::make_pair(current,
                              current->pipelineDependentDistributedPlanLogic(
                                  {.pipelinePrefix = *_splitPipeline.shardsPipeline,
                                   .pipelineSuffix = *_splitPipeline.mergePipeline,
                                   .shardKeyPaths = _initialShardKeyPaths}));
    }

    /**
     * Helper for find split point that handles the split after a stage that must be on
     * the merging half of the pipeline defers being added to the merging pipeline.
     */
    void _finishFindSplitPointAfterDeferral(
        boost::intrusive_ptr<DocumentSource> deferredStage,
        boost::optional<BSONObj> mergeSort,
        DocumentSource::DistributedPlanLogic::movePastFunctionType moveCheckFunc) {
        while (_mergePipeHasNext()) {
            auto [current, distributedPlanLogic] = _popFirstMergeStage();
            if (!moveCheckFunc(*current)) {
                _splitPipeline.mergePipeline->addInitialSource(std::move(current));
                break;
            }

            // Check if this source is splittable.
            if (distributedPlanLogic) {
                // If this stage also would like to split, split here. Don't defer multiple stages.
                _addSplitStages(*distributedPlanLogic);

                // The sort that was earlier in the pipeline takes precedence.
                if (!mergeSort) {
                    mergeSort = getOwnedOrNone(distributedPlanLogic->mergeSortPattern);
                }
                break;
            }

            // Move the source from the merger _sources to the shard _sources.
            _splitPipeline.shardsPipeline->addFinalSource(current);
        }

        // We got to the end of the pipeline or found a split point.
        if (deferredStage) {
            _splitPipeline.mergePipeline->addInitialSource(std::move(deferredStage));
        }
        _splitPipeline.shardCursorsSortSpec = getOwnedOrNone(mergeSort);
    }

    /**
     * Splits the current stage according to its distributed plan logic. Can add at most one
     * shard stage to the back of the shard pipeline and multiple stages at the front of the
     * merge pipeline.
     */
    void _addSplitStages(const DocumentSource::DistributedPlanLogic& distributedPlanLogic) {
        // This stage must be split, split it normally.
        // Add in reverse order since we add each to the front and this would flip the order
        // otherwise.
        for (auto reverseIt = distributedPlanLogic.mergingStages.rbegin();
             reverseIt != distributedPlanLogic.mergingStages.rend();
             ++reverseIt) {
            tassert(6448012,
                    "A stage cannot simultaneously be present on both sides of a pipeline split",
                    distributedPlanLogic.shardsStage != *reverseIt);
            _splitPipeline.mergePipeline->addInitialSource(*reverseIt);
        }

        if (distributedPlanLogic.shardsStage) {
            _splitPipeline.shardsPipeline->addFinalSource(
                std::move(distributedPlanLogic.shardsStage));
        }
    }

    /**
     * Push a stage down to the shardsPipeline completely, with no matching merge stage for
     * the router.
     */
    void pushdownEntireStage(boost::intrusive_ptr<DocumentSource> source) {
        // Verify that this stage has not accidentally been left in the merge pipeline.
        tassert(9245700,
                "Erroneously attempted to pushdown a stage which is still in mergePipeline",
                source != _splitPipeline.mergePipeline->peekFront());
        _splitPipeline.shardsPipeline->addFinalSource(std::move(source));
    }

    /**
     * Moves everything before a splittable stage to the shards. If there are no splittable
     * stages, moves everything to the shards. It is not safe to call this optimization multiple
     * times.
     */
    void _findSplitPoint() {
        while (_mergePipeHasNext()) {
            auto [current, distributedPlanLogic] = _popFirstMergeStage();

            // Check if this source is splittable.
            if (!distributedPlanLogic) {
                // Move the source from the merger _sources to the shard _sources.
                pushdownEntireStage(std::move(current));
                continue;
            }

            // If the stage doesn't require a split, save it and defer the split point.
            if (!distributedPlanLogic->needsSplit) {
                if (distributedPlanLogic->shardsStage) {
                    _splitPipeline.shardsPipeline->addFinalSource(
                        std::move(distributedPlanLogic->shardsStage));
                }

                tassert(6253721,
                        "Must have deferral function if deferring pipeline split",
                        distributedPlanLogic->canMovePast);
                auto mergingStageList = distributedPlanLogic->mergingStages;
                tassert(6448007,
                        "Only support deferring at most one stage for now.",
                        mergingStageList.size() <= 1);

                _finishFindSplitPointAfterDeferral(
                    mergingStageList.empty() ? nullptr : std::move(*mergingStageList.begin()),
                    getOwnedOrNone(distributedPlanLogic->mergeSortPattern),
                    distributedPlanLogic->canMovePast);
            } else {
                // Otherwise, must split this stage.
                _addSplitStages(*distributedPlanLogic);
                _splitPipeline.shardCursorsSortSpec =
                    getOwnedOrNone(distributedPlanLogic->mergeSortPattern);
            }

            // We found the split point.
            return;
        }

        _splitPipeline.shardCursorsSortSpec = boost::none;
    }

    /**
     * When the last stage of shard pipeline is $sort, move stages that can run on shards and
     * don't rename or modify the fields in $sort from merge pipeline. The function starts from
     * the beginning of the merge pipeline and finds the first consecutive eligible stages.
     */
    void _moveEligibleStreamingStagesBeforeSortOnShards() {
        if (!_splitPipeline.shardCursorsSortSpec)
            return;

        tassert(5363800,
                "Expected non-empty shardPipe consisting of at least a $sort stage",
                !_splitPipeline.shardsPipeline->empty());
        if (!dynamic_cast<DocumentSourceSort*>(
                _splitPipeline.shardsPipeline->getSources().back().get())) {
            // Expected last stage on the shards to be a $sort.
            return;
        }
        auto sortPaths = _splitPipeline.shardCursorsSortSpec->getFieldNames<OrderedPathSet>();
        auto firstMergeStage = _splitPipeline.mergePipeline->getSources().cbegin();
        std::function<bool(DocumentSource*)> distributedPlanLogicCallback =
            [](DocumentSource* stage) {
                return !static_cast<bool>(stage->distributedPlanLogic());
            };
        auto [lastUnmodified, renameMap] =
            semantic_analysis::findLongestViablePrefixPreservingPaths(
                firstMergeStage,
                _splitPipeline.mergePipeline->getSources().cend(),
                sortPaths,
                distributedPlanLogicCallback);
        for (const auto& sortPath : sortPaths) {
            auto pair = renameMap.find(sortPath);
            if (pair == renameMap.end() || pair->first != pair->second) {
                return;
            }
        }
        _splitPipeline.shardsPipeline->getSources().insert(
            _splitPipeline.shardsPipeline->getSources().end(), firstMergeStage, lastUnmodified);
        _splitPipeline.mergePipeline->getSources().erase(firstMergeStage, lastUnmodified);
    }

    /**
     * In case the last shard stage is a $sort and the first merge stage is a $group which is
     * grouping on (a superset of) the shard key, the $group is partially moved to the shards. The
     * $group is partially moved to the shards because it consumes and discards the sort-order
     * generated per-shard, so we don't need to preserve this sort order at the merge stage.
     */
    void _moveGroupFollowingSortFromMergerToShards() {
        if (_splitPipeline.shardsPipeline->empty() || _splitPipeline.mergePipeline->empty()) {
            return;
        }

        const auto lastShardSort = dynamic_cast<DocumentSourceSort*>(
            _splitPipeline.shardsPipeline->getSources().back().get());
        const auto firstMergerGroup = dynamic_cast<DocumentSourceGroup*>(
            _splitPipeline.mergePipeline->getSources().front().get());
        if (!lastShardSort || !firstMergerGroup) {
            return;
        }

        if (!firstMergerGroup->groupIsOnShardKey(*_splitPipeline.shardsPipeline,
                                                 _initialShardKeyPaths)) {
            return;
        }

        auto [group, groupDistributedLogic] = _popFirstMergeStage();
        _splitPipeline.shardCursorsSortSpec = boost::none;
        if (groupDistributedLogic) {
            // The $group can be partially pushed down to the shards.
            _addSplitStages(*groupDistributedLogic);
        } else {
            // The $group can be entirely pushed down to the shards.
            pushdownEntireStage(group);
        }
    }

    /**
     * If the final stage on shards is to unwind an array, move that stage to the merger. This
     * cuts down on network traffic and allows us to take advantage of reduced copying in
     * unwind.
     */
    void _moveFinalUnwindFromShardsToMerger() {
        while (!_splitPipeline.shardsPipeline->empty() &&
               dynamic_cast<DocumentSourceUnwind*>(
                   _splitPipeline.shardsPipeline->getSources().back().get())) {
            _splitPipeline.mergePipeline->addInitialSource(
                _splitPipeline.shardsPipeline->popBack());
        }
    }

    /**
     * If the merging pipeline includes a $limit stage that creates an upper bound on how many
     * input documents it needs to compute the aggregation, we can use that as an upper bound on
     * how many documents each of the shards needs to produce. Propagating that upper bound to
     * the shards (using a $limit in the shard pipeline) can reduce the number of documents the
     * shards need to process and transfer over the network (see SERVER-36881).
     *
     * If there are $skip stages before the $limit, the skipped documents also contribute to the
     * upper bound.
     */
    void _propagateDocLimitToShards() {
        long long numDocumentsNeeded = 0;

        for (auto&& source : _splitPipeline.mergePipeline->getSources()) {
            auto skipStage = dynamic_cast<DocumentSourceSkip*>(source.get());
            if (skipStage) {
                numDocumentsNeeded += skipStage->getSkip();
                continue;
            }

            auto limitStage = dynamic_cast<DocumentSourceLimit*>(source.get());
            if (limitStage) {
                numDocumentsNeeded += limitStage->getLimit();

                auto existingShardLimit = getPipelineLimit(_splitPipeline.shardsPipeline.get());
                if (existingShardLimit && *existingShardLimit <= numDocumentsNeeded) {
                    // The sharding pipeline already has a limit that is no greater than the limit
                    // we were going to add, so no changes are necessary.
                    break;
                }

                auto shardLimit = DocumentSourceLimit::create(
                    _splitPipeline.mergePipeline->getContext(), numDocumentsNeeded);
                _splitPipeline.shardsPipeline->addFinalSource(shardLimit);

                // We have successfully applied a limit to the number of documents we need from each
                // shard.
                break;
            }

            // If there are any stages in the merge pipeline before the $skip and $limit stages,
            // then we cannot use the $limit to determine an upper bound, unless those stages could
            // be swapped with the $limit.
            if (!source->constraints().canSwapWithSkippingOrLimitingStage) {
                break;
            }
        }
    }

    /**
     * Adds a stage to the end of 'shardPipe' explicitly requesting all fields that 'mergePipe'
     * needs. This is only done if it heuristically determines that it is needed. This
     * optimization can reduce the amount of network traffic and can also enable the shards to
     * convert less source BSON into Documents.
     */
    void _limitFieldsSentFromShardsToMerger() {
        DepsTracker mergeDeps(
            _splitPipeline.mergePipeline->getDependencies(DepsTracker::NoMetadataValidation()));
        if (mergeDeps.needWholeDocument)
            return;  // the merge needs all fields, so nothing we can do.

        // Empty project is "special" so if no fields are needed, we just ask for _id instead.
        if (mergeDeps.fields.empty())
            mergeDeps.fields.insert("_id");

        // HEURISTIC: only apply optimization if none of the shard stages have an exhaustive list of
        // field dependencies. While this may not be 100% ideal in all cases, it is simple and
        // avoids the worst cases by ensuring that:
        // 1) Optimization IS applied when the shards wouldn't have known their exhaustive list of
        //    dependencies. This situation can happen when a $sort is before the first $project or
        //    $group. Without the optimization, the shards would have to reify and transmit full
        //    objects even though only a subset of fields are needed.
        // 2) Optimization IS NOT applied immediately following a $project or $group since it would
        //    add an unnecessary project (and therefore a deep-copy).
        for (auto&& source : _splitPipeline.shardsPipeline->getSources()) {
            DepsTracker dt;
            if (source->getDependencies(&dt) & DepsTracker::State::EXHAUSTIVE_FIELDS)
                return;
        }
        // if we get here, add the project.
        boost::intrusive_ptr<DocumentSource> project = DocumentSourceProject::createFromBson(
            BSON("$project" << mergeDeps.toProjectionWithoutMetadata()).firstElement(),
            _splitPipeline.shardsPipeline->getContext());
        _splitPipeline.shardsPipeline->pushBack(std::move(project));
    }

    /**
     * Non-correlated pipeline caching is only supported locally. When the
     * DocumentSourceSequentialDocumentCache stage has been moved to the shards pipeline,
     * abandon the associated local cache.
     */
    void _abandonCacheIfSentToShards() {
        for (auto&& stage : _splitPipeline.shardsPipeline->getSources()) {
            if (StringData(stage->getSourceName()) ==
                DocumentSourceSequentialDocumentCache::kStageName) {
                static_cast<DocumentSourceSequentialDocumentCache*>(stage.get())->abandonCache();
            }
        }
    }

    // Output.
    SplitPipeline _splitPipeline;

    // Stores the shard key paths as they are named at the start of the pipeline.
    boost::optional<OrderedPathSet> _initialShardKeyPaths;
};

SplitPipeline SplitPipeline::split(std::unique_ptr<Pipeline> pipelineToSplit,
                                   boost::optional<OrderedPathSet> shardKeyPaths) {
    return PipelineSplitter(std::move(pipelineToSplit), std::move(shardKeyPaths)).split().release();
}

}  // namespace sharded_agg_helpers
}  // namespace mongo
