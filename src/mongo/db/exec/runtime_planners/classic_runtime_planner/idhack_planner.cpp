// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/classic/idhack.h"
#include "mongo/db/exec/classic/projection.h"
#include "mongo/db/exec/classic/return_key.h"
#include "mongo/db/exec/classic/shard_filter.h"
#include "mongo/db/exec/classic/sort_key_generator.h"
#include "mongo/db/exec/runtime_planners/classic_runtime_planner/planner_interface.h"
#include "mongo/db/shard_role/shard_catalog/index_descriptor.h"

namespace mongo::classic_runtime_planner {

IdHackPlanner::IdHackPlanner(PlannerData plannerData, const IndexCatalogEntry* entry)
    : ClassicPlannerInterface(std::move(plannerData)) {
    auto collection = collections().getMainCollectionPtrOrAcquisition();
    std::unique_ptr<PlanStage> stage =
        std::make_unique<IDHackStage>(cq()->getExpCtxRaw(), cq(), ws(), collection, entry);

    // Might have to filter out orphaned docs.
    if (plannerOptions() & QueryPlannerParams::INCLUDE_SHARD_FILTER) {
        auto shardFilterer = collection.getShardingFilter();
        invariant(shardFilterer,
                  "Attempting to use shard filter when there's no shard filter available for "
                  "the collection");

        stage = std::make_unique<ShardFilterStage>(
            cq()->getExpCtxRaw(), std::move(*shardFilterer), ws(), std::move(stage));
    }

    const auto* cqProjection = cq()->getProj();

    // Add a SortKeyGeneratorStage if the query requested sortKey metadata.
    if (cq()->metadataDeps()[DocumentMetadataFields::kSortKey]) {
        stage = std::make_unique<SortKeyGeneratorStage>(
            cq()->getExpCtxRaw(), std::move(stage), ws(), cq()->getFindCommandRequest().getSort());
    }

    if (cq()->getFindCommandRequest().getReturnKey()) {
        // If returnKey was requested, add ReturnKeyStage to return only the index keys in
        // the resulting documents. If a projection was also specified, it will be ignored,
        // with the exception the $meta sortKey projection, which can be used along with the
        // returnKey.
        stage = std::make_unique<ReturnKeyStage>(
            cq()->getExpCtxRaw(),
            cqProjection ? cqProjection->extractSortKeyMetaFields() : std::vector<FieldPath>{},
            ws(),
            std::move(stage));
    } else if (cqProjection) {
        // There might be a projection. The idhack stage will always fetch the full
        // document, so we don't support covered projections. However, we might use the
        // simple inclusion fast path.
        // Stuff the right data into the params depending on what proj impl we use.
        if (!cqProjection->isSimple()) {
            stage = std::make_unique<ProjectionStageDefault>(
                cq()->getExpCtxRaw(),
                cq()->getFindCommandRequest().getProjection(),
                cq()->getProj(),
                ws(),
                std::move(stage));
        } else {
            stage = std::make_unique<ProjectionStageSimple>(
                cq()->getExpCtxRaw(),
                cq()->getFindCommandRequest().getProjection(),
                cq()->getProj(),
                ws(),
                std::move(stage));
        }
    }
    setRoot(std::move(stage));
}

Status IdHackPlanner::doPlan(PlanYieldPolicy* planYieldPolicy) {
    // Nothing to do.
    return Status::OK();
}

std::unique_ptr<QuerySolution> IdHackPlanner::extractQuerySolution() {
    // IDHACK queries bypass the planning process, and therefore don't have a 'QuerySolution'.
    return nullptr;
}

PlanRankingResult IdHackPlanner::extractPlanRankingResult() {
    tassert(11974301,
            "Expected `extractPlanRankingResult` to only be called with get executor deferred "
            "feature flag enabled.",
            cq()->getExpCtx()->getIfrContext()->getSavedFlagValue(
                feature_flags::gFeatureFlagGetExecutorDeferredEngineChoice));
    return PlanRankingResult{.usedIdhack = true,
                             .execState = SavedExecState{ClassicExecState{.workingSet = extractWs(),
                                                                          .root = extractRoot()}},
                             .plannerParams = extractPlannerParams()};
}

const QuerySolution* IdHackPlanner::querySolution() const {
    // IDHACK queries bypass the planning process, and therefore don't have a 'QuerySolution'.
    return nullptr;
}
}  // namespace mongo::classic_runtime_planner
