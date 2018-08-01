/**
 * Copyright (C) 2018 MongoDB Inc.
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
 * must comply with the GNU Affero General Public License in all respects
 * for all of the code used other than as permitted herein. If you modify
 * file(s) with this exception, you may extend this exception to your
 * version of the file(s), but you are not obligated to do so. If you do not
 * wish to do so, delete this exception statement from your version. If you
 * delete this exception statement from all source files in the program,
 * then also delete it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/s/query/cluster_aggregation_planner.h"

#include "mongo/db/pipeline/document_source_group.h"
#include "mongo/db/pipeline/document_source_limit.h"
#include "mongo/db/pipeline/document_source_match.h"
#include "mongo/db/pipeline/document_source_merge_cursors.h"
#include "mongo/db/pipeline/document_source_out.h"
#include "mongo/db/pipeline/document_source_project.h"
#include "mongo/db/pipeline/document_source_skip.h"
#include "mongo/db/pipeline/document_source_sort.h"
#include "mongo/db/pipeline/document_source_unwind.h"
#include "mongo/db/pipeline/document_source_update_on_add_shard.h"
#include "mongo/executor/task_executor_pool.h"
#include "mongo/s/catalog_cache.h"
#include "mongo/s/grid.h"
#include "mongo/s/query/router_stage_limit.h"
#include "mongo/s/query/router_stage_pipeline.h"
#include "mongo/s/query/router_stage_remove_metadata_fields.h"
#include "mongo/s/query/router_stage_skip.h"
#include "mongo/s/shard_id.h"
#include "mongo/s/shard_key_pattern.h"

namespace mongo {
namespace cluster_aggregation_planner {

namespace {
/**
 * Moves everything before a splittable stage to the shards. If there are no splittable stages,
 * moves everything to the shards.
 *
 * It is not safe to call this optimization multiple times.
 *
 * NOTE: looks for NeedsMergerDocumentSources and uses that API
 *
 * Returns the sort specification if the input streams are sorted, and false otherwise.
 */
boost::optional<BSONObj> findSplitPoint(Pipeline::SourceContainer* shardPipe, Pipeline* mergePipe) {
    while (!mergePipe->getSources().empty()) {
        boost::intrusive_ptr<DocumentSource> current = mergePipe->popFront();

        // Check if this source is splittable.
        NeedsMergerDocumentSource* splittable =
            dynamic_cast<NeedsMergerDocumentSource*>(current.get());

        if (!splittable) {
            // Move the source from the merger _sources to the shard _sources.
            shardPipe->push_back(current);
        } else {
            // Split this source into 'merge' and 'shard' _sources.
            boost::intrusive_ptr<DocumentSource> shardSource = splittable->getShardSource();
            auto mergeLogic = splittable->mergingLogic();

            // A source may not simultaneously be present on both sides of the split.
            invariant(shardSource != mergeLogic.mergingStage);

            if (shardSource)
                shardPipe->push_back(std::move(shardSource));

            if (mergeLogic.mergingStage)
                mergePipe->addInitialSource(std::move(mergeLogic.mergingStage));

            return mergeLogic.inputSortPattern;
        }
    }
    return boost::none;
}

/**
 * If the final stage on shards is to unwind an array, move that stage to the merger. This cuts down
 * on network traffic and allows us to take advantage of reduced copying in unwind.
 */
void moveFinalUnwindFromShardsToMerger(Pipeline* shardPipe, Pipeline* mergePipe) {
    while (!shardPipe->getSources().empty() &&
           dynamic_cast<DocumentSourceUnwind*>(shardPipe->getSources().back().get())) {
        mergePipe->addInitialSource(shardPipe->popBack());
    }
}

/**
 * Adds a stage to the end of 'shardPipe' explicitly requesting all fields that 'mergePipe' needs.
 * This is only done if it heuristically determines that it is needed. This optimization can reduce
 * the amount of network traffic and can also enable the shards to convert less source BSON into
 * Documents.
 */
void limitFieldsSentFromShardsToMerger(Pipeline* shardPipe, Pipeline* mergePipe) {
    DepsTracker mergeDeps(mergePipe->getDependencies(DepsTracker::kAllMetadataAvailable));
    if (mergeDeps.needWholeDocument)
        return;  // the merge needs all fields, so nothing we can do.

    // Empty project is "special" so if no fields are needed, we just ask for _id instead.
    if (mergeDeps.fields.empty())
        mergeDeps.fields.insert("_id");

    // Remove metadata from dependencies since it automatically flows through projection and we
    // don't want to project it in to the document.
    mergeDeps.setNeedsMetadata(DepsTracker::MetadataType::TEXT_SCORE, false);

    // HEURISTIC: only apply optimization if none of the shard stages have an exhaustive list of
    // field dependencies. While this may not be 100% ideal in all cases, it is simple and
    // avoids the worst cases by ensuring that:
    // 1) Optimization IS applied when the shards wouldn't have known their exhaustive list of
    //    dependencies. This situation can happen when a $sort is before the first $project or
    //    $group. Without the optimization, the shards would have to reify and transmit full
    //    objects even though only a subset of fields are needed.
    // 2) Optimization IS NOT applied immediately following a $project or $group since it would
    //    add an unnecessary project (and therefore a deep-copy).
    for (auto&& source : shardPipe->getSources()) {
        DepsTracker dt(DepsTracker::kAllMetadataAvailable);
        if (source->getDependencies(&dt) & DepsTracker::State::EXHAUSTIVE_FIELDS)
            return;
    }
    // if we get here, add the project.
    boost::intrusive_ptr<DocumentSource> project = DocumentSourceProject::createFromBson(
        BSON("$project" << mergeDeps.toProjection()).firstElement(), shardPipe->getContext());
    shardPipe->pushBack(project);
}

bool isMergeSkipOrLimit(const boost::intrusive_ptr<DocumentSource>& stage) {
    return (dynamic_cast<DocumentSourceLimit*>(stage.get()) ||
            dynamic_cast<DocumentSourceMergeCursors*>(stage.get()) ||
            dynamic_cast<DocumentSourceSkip*>(stage.get()));
}

bool isAllLimitsAndSkips(Pipeline* pipeline) {
    const auto stages = pipeline->getSources();
    return std::all_of(
        stages.begin(), stages.end(), [](const auto& stage) { return isMergeSkipOrLimit(stage); });
}

ClusterClientCursorGuard convertPipelineToRouterStages(
    std::unique_ptr<Pipeline, PipelineDeleter> pipeline, ClusterClientCursorParams&& cursorParams) {
    auto* opCtx = pipeline->getContext()->opCtx;

    // We expect the pipeline to be fully executable at this point, so if the pipeline was all skips
    // and limits we expect it to start with a $mergeCursors stage.
    auto mergeCursors =
        checked_cast<DocumentSourceMergeCursors*>(pipeline->getSources().front().get());
    // Replace the pipeline with RouterExecStages.
    std::unique_ptr<RouterExecStage> root = mergeCursors->convertToRouterStage();
    pipeline->popFront();
    while (!pipeline->getSources().empty()) {
        if (auto skip = pipeline->popFrontWithName(DocumentSourceSkip::kStageName)) {
            root = std::make_unique<RouterStageSkip>(
                opCtx, std::move(root), static_cast<DocumentSourceSkip*>(skip.get())->getSkip());
        } else if (auto limit = pipeline->popFrontWithName(DocumentSourceLimit::kStageName)) {
            root = std::make_unique<RouterStageLimit>(
                opCtx, std::move(root), static_cast<DocumentSourceLimit*>(limit.get())->getLimit());
        } else {
            // We previously checked that everything was a $mergeCursors, $skip, or $limit. We
            // already popped off the $mergeCursors, so everything else should be a $skip or a
            // $limit.
            MONGO_UNREACHABLE;
        }
    }
    // We are executing the pipeline without using an actual Pipeline, so we need to strip out any
    // Document metadata ourselves.
    return ClusterClientCursorImpl::make(
        opCtx,
        std::make_unique<RouterStageRemoveMetadataFields>(
            opCtx, std::move(root), Document::allMetadataFieldNames),
        std::move(cursorParams));
}

bool stageCanRunInParallel(const boost::intrusive_ptr<DocumentSource>& stage,
                           const std::set<std::string>& nameOfShardKeyFieldsUponEntryToStage) {
    if (auto needsMerger = dynamic_cast<NeedsMergerDocumentSource*>(stage.get())) {
        return needsMerger->canRunInParallelBeforeOut(nameOfShardKeyFieldsUponEntryToStage);
    } else {
        // This stage is fine to execute in parallel on each stream. For example, a $match can be
        // applied to each stream in parallel.
        return true;
    }
}

std::string mapToString(const StringMap<std::string>& map) {
    StringBuilder sb;
    sb << "{";
    for (auto&& entry : map) {
        if (sb.len() != 1) {
            sb << ", ";
        }
        sb << entry.first << ": " << entry.second;
    }
    sb << "}";
    return sb.str();
}

BSONObj buildNewKeyPattern(const ShardKeyPattern& shardKey, StringMap<std::string> renames) {
    BSONObjBuilder newPattern;
    for (auto&& elem : shardKey.getKeyPattern().toBSON()) {
        auto it = renames.find(elem.fieldNameStringData());
        invariant(it != renames.end(),
                  str::stream() << "Could not find new name of shard key field \""
                                << elem.fieldName()
                                << "\": rename map was "
                                << mapToString(renames));
        newPattern.appendAs(elem, it->second);
    }
    return newPattern.obj();
}

StringMap<std::string> computeShardKeyRenameMap(const Pipeline* mergePipeline,
                                                std::set<std::string>&& pathsOfShardKey) {
    auto traversalStart = mergePipeline->getSources().crbegin();
    auto traversalEnd = mergePipeline->getSources().crend();
    const auto leadingGroup =
        dynamic_cast<DocumentSourceGroup*>(mergePipeline->getSources().front().get());
    if (leadingGroup && leadingGroup->doingMerge()) {
        // A leading $group stage will not report to preserve any fields, since it blows away the
        // _id and replaces it with something new. It possibly renames some fields, but when
        // computing the new shard key we are interested in the name of the shard key *in the middle
        // of the $group*. The $exchange will be inserted between the shard-local groups and the
        // global groups. Thus we want to exclude this stage from our rename tracking.
        traversalEnd = std::prev(traversalEnd);
    }
    auto renameMap = Pipeline::renamedPaths(traversalStart, traversalEnd, pathsOfShardKey);
    invariant(renameMap,
              str::stream()
                  << "Analyzed pipeline was thought to preserve the shard key fields, but did not: "
                  << Value(mergePipeline->serialize()).toString());
    return *renameMap;
}

/**
 * Returns true if any stage in the pipeline would modify any of the fields in 'shardKeyPaths', or
 * if there is any stage in the pipeline requires a unified stream to do its computation like a
 * $limit would.
 *
 * Purposefully takes 'shardKeyPaths' by value so that it can be modified throughout.
 */
bool anyStageModifiesShardKeyOrNeedsMerge(std::set<std::string> shardKeyPaths,
                                          const Pipeline* mergePipeline) {
    const auto& stages = mergePipeline->getSources();
    for (auto it = stages.crbegin(); it != stages.crend(); ++it) {
        const auto& stage = *it;
        auto renames = stage->renamedPaths(std::move(shardKeyPaths));
        if (!renames) {
            return true;
        }
        shardKeyPaths.clear();
        for (auto&& rename : *renames) {
            shardKeyPaths.insert(rename.second);
        }
        if (!stageCanRunInParallel(stage, shardKeyPaths)) {
            // In order for this stage to work it needs a single input stream which it wouldn't get
            // if we inserted an exchange before it.
            return true;
        }
    }
    return false;
}

boost::optional<ShardedExchangePolicy> walkPipelineBackwardsTrackingShardKey(
    OperationContext* opCtx,
    const boost::intrusive_ptr<const DocumentSourceOut>& outStage,
    const Pipeline* mergePipeline,
    const ChunkManager& chunkManager) {

    const ShardKeyPattern& shardKey = chunkManager.getShardKeyPattern();
    std::set<std::string> shardKeyPaths;
    for (auto&& path : shardKey.getKeyPatternFields()) {
        shardKeyPaths.emplace(path->dottedField().toString());
    }
    if (anyStageModifiesShardKeyOrNeedsMerge(shardKeyPaths, mergePipeline)) {
        return boost::none;
    }

    // All the fields of the shard key are preserved by the pipeline, but they might be renamed. To
    // set up the $exchange, we need to build a fake shard key pattern which uses the names of the
    // shard key fields as they are at the split point of the pipeline.
    auto renames = computeShardKeyRenameMap(mergePipeline, std::move(shardKeyPaths));
    ShardKeyPattern newShardKey(buildNewKeyPattern(shardKey, renames));

    // Given the new shard key fields, build the distribution map.
    StringMap<std::vector<ChunkRange>> distribution;
    for (auto&& chunk : chunkManager.chunks()) {
        // Append the boundaries with the new names from the new shard key.
        auto translateBoundary = [&renames](const BSONObj& oldBoundary) {
            BSONObjBuilder bob;
            for (auto&& elem : oldBoundary) {
                bob.appendAs(elem, renames[elem.fieldNameStringData()]);
            }
            return bob.obj();
        };
        distribution[chunk.getShardId().toString()].emplace_back(translateBoundary(chunk.getMin()),
                                                                 translateBoundary(chunk.getMax()));
    }
    return ShardedExchangePolicy{
        ExchangePolicyEnum::kRange,
        ShardDistributionInfo{ShardKeyPattern{std::move(newShardKey)}, std::move(distribution)}};
}

}  // namespace

SplitPipeline splitPipeline(std::unique_ptr<Pipeline, PipelineDeleter> pipeline) {
    auto& expCtx = pipeline->getContext();
    // Re-brand 'pipeline' as the merging pipeline. We will move stages one by one from the merging
    // half to the shards, as possible.
    auto mergePipeline = std::move(pipeline);

    Pipeline::SourceContainer shardStages;
    boost::optional<BSONObj> inputsSort = findSplitPoint(&shardStages, mergePipeline.get());
    auto shardsPipeline = uassertStatusOK(Pipeline::create(std::move(shardStages), expCtx));

    // The order in which optimizations are applied can have significant impact on the efficiency of
    // the final pipeline. Be Careful!
    moveFinalUnwindFromShardsToMerger(shardsPipeline.get(), mergePipeline.get());
    limitFieldsSentFromShardsToMerger(shardsPipeline.get(), mergePipeline.get());
    shardsPipeline->setSplitState(Pipeline::SplitState::kSplitForShards);
    mergePipeline->setSplitState(Pipeline::SplitState::kSplitForMerge);

    return {std::move(shardsPipeline), std::move(mergePipeline), std::move(inputsSort)};
}

void addMergeCursorsSource(Pipeline* mergePipeline,
                           const LiteParsedPipeline& liteParsedPipeline,
                           BSONObj cmdSentToShards,
                           std::vector<RemoteCursor> remoteCursors,
                           const std::vector<ShardId>& targetedShards,
                           boost::optional<BSONObj> shardCursorsSortSpec,
                           executor::TaskExecutor* executor) {
    auto* opCtx = mergePipeline->getContext()->opCtx;
    AsyncResultsMergerParams armParams;
    armParams.setSort(shardCursorsSortSpec);
    armParams.setRemotes(std::move(remoteCursors));
    armParams.setTailableMode(mergePipeline->getContext()->tailableMode);
    armParams.setNss(mergePipeline->getContext()->ns);

    OperationSessionInfo sessionInfo;
    sessionInfo.setSessionId(opCtx->getLogicalSessionId());
    sessionInfo.setTxnNumber(opCtx->getTxnNumber());
    armParams.setOperationSessionInfo(sessionInfo);

    // For change streams, we need to set up a custom stage to establish cursors on new shards when
    // they are added, to ensure we don't miss results from the new shards.
    auto mergeCursorsStage = DocumentSourceMergeCursors::create(
        executor, std::move(armParams), mergePipeline->getContext());
    if (liteParsedPipeline.hasChangeStream()) {
        mergePipeline->addInitialSource(DocumentSourceUpdateOnAddShard::create(
            mergePipeline->getContext(),
            Grid::get(opCtx)->getExecutorPool()->getArbitraryExecutor(),
            mergeCursorsStage,
            targetedShards,
            cmdSentToShards));
    }
    mergePipeline->addInitialSource(std::move(mergeCursorsStage));
}

ClusterClientCursorGuard buildClusterCursor(OperationContext* opCtx,
                                            std::unique_ptr<Pipeline, PipelineDeleter> pipeline,
                                            ClusterClientCursorParams&& cursorParams) {
    if (isAllLimitsAndSkips(pipeline.get())) {
        // We can optimize this Pipeline to avoid going through any DocumentSources at all and thus
        // skip the expensive BSON->Document->BSON conversion.
        return convertPipelineToRouterStages(std::move(pipeline), std::move(cursorParams));
    }
    return ClusterClientCursorImpl::make(
        opCtx, std::make_unique<RouterStagePipeline>(std::move(pipeline)), std::move(cursorParams));
}

boost::optional<ShardedExchangePolicy> checkIfEligibleForExchange(OperationContext* opCtx,
                                                                  const Pipeline* mergePipeline) {
    const auto grid = Grid::get(opCtx);
    invariant(grid);

    const auto outStage =
        dynamic_cast<DocumentSourceOut*>(mergePipeline->getSources().back().get());
    if (!outStage || outStage->getMode() == WriteModeEnum::kModeReplaceCollection) {
        // If there's no $out stage we won't try to do an $exchange. If the $out stage is using mode
        // "replaceCollection", then there's no point doing an $exchange because all the writes will
        // go to a single node, so we should just perform the merge on that host.
        return boost::none;
    }

    const auto routingInfo = uassertStatusOK(
        grid->catalogCache()->getCollectionRoutingInfo(opCtx, outStage->getOutputNs()));
    if (!routingInfo.cm()) {
        return boost::none;
    }

    // The collection is sharded and we have an $out stage! Here we assume the $out stage has
    // already verified that the shard key pattern is compatible with the unique key being used.
    // Assuming this, we just have to make sure the shard key is preserved (though possibly renamed)
    // all the way to the front of the merge pipeline. If this is the case then for any document
    // entering the merging pipeline we can predict which shard it will need to end up being
    // inserted on. With this ability we can insert an exchange on the shards to partition the
    // documents based on which shard will end up owning them. Then each shard can perform a merge
    // of only those documents which belong to it (optimistically, barring chunk migrations).
    return walkPipelineBackwardsTrackingShardKey(opCtx, outStage, mergePipeline, *routingInfo.cm());
}

}  // namespace cluster_aggregation_planner
}  // namespace mongo
