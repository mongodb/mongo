// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/exec/classic/plan_stage.h"
#include "mongo/db/exec/classic/working_set.h"
#include "mongo/db/exec/plan_stats.h"
#include "mongo/db/exec/shard_filterer_impl.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/query/compiler/physical_model/query_solution/stage_types.h"
#include "mongo/db/shard_role/shard_catalog/scoped_collection_metadata.h"
#include "mongo/util/modules.h"

#include <memory>
#include <string_view>

namespace mongo {
using namespace std::literals::string_view_literals;

/**
 * This stage drops documents (called "orphans") that don't logically belong to this shard according
 * to the the provided 'collectionFilter'. No data should be returned from a query in ranges of
 * migrations that committed after the query started, or from ranges not owned when the query began.
 *
 * A related system will ensure that the data migrated away from a shard will not be deleted as long
 * as there are active queries from before the migration. By holding onto a copy of the provided
 * 'collectionFilter', this stage signals to the sharding subsystem that the data required at the
 * associated shard version cannot yet be deleted. In other words, no migrated data should be
 * removed from a shard while there are queries that were active before the migration.
 *
 * Preconditions: Child must be fetched.
 */
class ShardFilterStage final : public PlanStage {
public:
    ShardFilterStage(ExpressionContext* expCtx,
                     ScopedCollectionFilter collectionFilter,
                     WorkingSet* ws,
                     std::unique_ptr<PlanStage> child);
    ~ShardFilterStage() override;

    bool isEOF() const final;
    StageState doWork(WorkingSetID* out) final;

    StageType stageType() const final {
        return STAGE_SHARDING_FILTER;
    }

    std::unique_ptr<PlanStageStats> getStats() final;

    const SpecificStats* getSpecificStats() const final;

    static constexpr std::string_view kStageType = "SHARDING_FILTER"sv;

private:
    WorkingSet* _ws;

    ShardingFilterStats _specificStats;

    // Note: it is important that this owns the ScopedCollectionFilter from the time this stage
    // is constructed. See ScopedCollectionFilter class comment and MetadataManager comment for
    // details. The existence of the ScopedCollectionFilter prevents data which may have been
    // migrated from being deleted while the query is still active. If we didn't hold one
    // ScopedCollectionFilter for the entire query, it'd be possible for data which the query
    // needs to read to be deleted while it's still running.
    ShardFiltererImpl _shardFilterer;
};

}  // namespace mongo
