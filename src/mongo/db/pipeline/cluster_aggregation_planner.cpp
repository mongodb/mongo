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

#include "mongo/db/pipeline/cluster_aggregation_planner.h"

#include "mongo/db/pipeline/document_source_match.h"
#include "mongo/db/pipeline/document_source_merge_cursors.h"
#include "mongo/db/pipeline/document_source_project.h"
#include "mongo/db/pipeline/document_source_sort.h"
#include "mongo/db/pipeline/document_source_unwind.h"

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
 */
void findSplitPoint(Pipeline* shardPipe, Pipeline* mergePipe) {
    while (!mergePipe->getSources().empty()) {
        boost::intrusive_ptr<DocumentSource> current = mergePipe->popFront();

        // Check if this source is splittable.
        NeedsMergerDocumentSource* splittable =
            dynamic_cast<NeedsMergerDocumentSource*>(current.get());

        if (!splittable) {
            // Move the source from the merger _sources to the shard _sources.
            shardPipe->pushBack(current);
        } else {
            // Split this source into 'merge' and 'shard' _sources.
            boost::intrusive_ptr<DocumentSource> shardSource = splittable->getShardSource();
            auto mergeSources = splittable->getMergeSources();

            // A source may not simultaneously be present on both sides of the split.
            invariant(std::find(mergeSources.begin(), mergeSources.end(), shardSource) ==
                      mergeSources.end());

            if (shardSource)
                shardPipe->pushBack(shardSource);

            // Add the stages in reverse order, so that they appear in the pipeline in the same
            // order as they were returned by the stage.
            for (auto it = mergeSources.rbegin(); it != mergeSources.rend(); ++it) {
                mergePipe->addInitialSource(*it);
            }

            break;
        }
    }
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
    for (auto&& source : shardPipe->getSources()) {
        DepsTracker dt(depsMetadata);
        if (source->getDependencies(&dt) & DocumentSource::EXHAUSTIVE_FIELDS)
            return;
    }
    // if we get here, add the project.
    boost::intrusive_ptr<DocumentSource> project = DocumentSourceProject::createFromBson(
        BSON("$project" << mergeDeps.toProjection()).firstElement(), shardPipe->getContext());
    shardPipe->pushBack(project);
}
}  // namespace

void performSplitPipelineOptimizations(Pipeline* shardPipeline, Pipeline* mergingPipeline) {
    // The order in which optimizations are applied can have significant impact on the
    // efficiency of the final pipeline. Be Careful!
    findSplitPoint(shardPipeline, mergingPipeline);
    moveFinalUnwindFromShardsToMerger(shardPipeline, mergingPipeline);
    limitFieldsSentFromShardsToMerger(shardPipeline, mergingPipeline);
}

boost::optional<BSONObj> popLeadingMergeSort(Pipeline* pipeline) {
    // Remove a leading $sort iff it is a mergesort, since the ARM cannot handle blocking $sort.
    auto frontSort = pipeline->popFrontWithNameAndCriteria(
        DocumentSourceSort::kStageName, [](const DocumentSource* const source) {
            return static_cast<const DocumentSourceSort* const>(source)->mergingPresorted();
        });

    if (frontSort) {
        auto sortStage = static_cast<DocumentSourceSort*>(frontSort.get());
        if (auto sortLimit = sortStage->getLimitSrc()) {
            // There was a limit stage absorbed into the sort stage, so we need to preserve that.
            pipeline->addInitialSource(sortLimit);
        }
        return sortStage
            ->sortKeyPattern(DocumentSourceSort::SortKeySerialization::kForSortKeyMerging)
            .toBson();
    }
    return boost::none;
}

void addMergeCursorsSource(Pipeline* mergePipeline,
                           std::vector<RemoteCursor> remoteCursors,
                           executor::TaskExecutor* executor) {
    AsyncResultsMergerParams armParams;
    if (auto sort = popLeadingMergeSort(mergePipeline)) {
        armParams.setSort(std::move(*sort));
    }
    armParams.setRemotes(std::move(remoteCursors));
    armParams.setTailableMode(mergePipeline->getContext()->tailableMode);
    armParams.setNss(mergePipeline->getContext()->ns);
    mergePipeline->addInitialSource(DocumentSourceMergeCursors::create(
        executor, std::move(armParams), mergePipeline->getContext()));
}

}  // namespace cluster_aggregation_planner
}  // namespace mongo
