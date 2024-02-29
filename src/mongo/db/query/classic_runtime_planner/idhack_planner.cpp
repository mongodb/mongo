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

#include "mongo/db/query/classic_runtime_planner/planner_interface.h"

#include "mongo/db/exec/idhack.h"
#include "mongo/db/exec/return_key.h"
#include "mongo/db/exec/shard_filter.h"
#include "mongo/db/exec/sort_key_generator.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/query/projection_policies.h"
#include "mongo/db/query/query_planner_common.h"

namespace mongo::classic_runtime_planner {

IdHackPlanner::IdHackPlanner(PlannerData plannerData, const IndexDescriptor* descriptor)
    : ClassicPlannerInterface(std::move(plannerData)) {
    auto collection = collections().getMainCollectionPtrOrAcquisition();
    std::unique_ptr<PlanStage> stage =
        std::make_unique<IDHackStage>(cq()->getExpCtxRaw(), cq(), ws(), collection, descriptor);

    // Might have to filter out orphaned docs.
    if (plannerOptions() & QueryPlannerParams::INCLUDE_SHARD_FILTER) {
        auto shardFilterer = collection.getShardingFilter(opCtx());
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
            cqProjection ? QueryPlannerCommon::extractSortKeyMetaFieldsFromProjection(*cqProjection)
                         : std::vector<FieldPath>{},
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
}  // namespace mongo::classic_runtime_planner
