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

#pragma once

#include "mongo/db/exec/classic/plan_stage.h"
#include "mongo/db/exec/classic/working_set.h"
#include "mongo/db/exec/plan_stats.h"
#include "mongo/db/exec/shard_filterer_impl.h"
#include "mongo/db/local_catalog/shard_role_catalog/scoped_collection_metadata.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/query/compiler/physical_model/query_solution/stage_types.h"

#include <memory>

namespace mongo {

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

    static const char* kStageType;

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
