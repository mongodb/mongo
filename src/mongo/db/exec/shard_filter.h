/**
 *    Copyright (C) 2013 10gen Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include "mongo/db/exec/plan_stage.h"
#include "mongo/db/s/metadata_manager.h"

namespace mongo {

/**
 * This stage drops documents that didn't belong to the shard we're executing on at the time of
 * construction. This matches the contract for sharded cursorids which guarantees that a
 * StaleConfigException will be thrown early or the cursorid for its entire lifetime will return
 * documents matching the shard version set on the connection at the time of cursorid creation.
 *
 * A related system will ensure that the data migrated away from a shard will not be deleted as
 * long as there are active queries from before the migration. Currently, "active queries" is
 * defined by cursorids so it is important that the metadata used in this stage uses the same
 * version as the cursorid. Therefore, you must wrap any Runner using this Stage in a
 * ClientCursor during the same lock grab as constructing the Runner.
 *
 * BEGIN NOTE FROM GREG
 *
 * There are three sharded query contracts:
 *
 * 0) Migration commit takes the db lock - i.e. is serialized with writes and reads.
 * 1) No data should be returned from a query in ranges of migrations that committed after the
 * query started, or from ranges not owned when the query began.
 * 2) No migrated data should be removed from a shard while there are queries that were active
 * before the migration.
 *
 * As implementation details, collection metadata is used to determine the ranges of all data
 * not actively migrated (or orphaned).  CursorIds are currently used to establish "active"
 * queries before migration commit.
 *
 * Combining all this: if a query is started in a db lock and acquires in that (same) lock the
 * collection metadata and a cursorId, the query will return results for exactly the ranges in
 * the metadata (though of arbitrary staleness).  This is the sharded collection query contract.
 *
 * END NOTE FROM GREG
 *
 * Preconditions: Child must be fetched.  TODO: when covering analysis is in just build doc
 * and check that against shard key.  See SERVER-5022.
 */
class ShardFilterStage final : public PlanStage {
public:
    ShardFilterStage(OperationContext* opCtx,
                     ScopedCollectionMetadata metadata,
                     WorkingSet* ws,
                     PlanStage* child);
    ~ShardFilterStage();

    bool isEOF() final;
    StageState doWork(WorkingSetID* out) final;

    StageType stageType() const final {
        return STAGE_SHARDING_FILTER;
    }

    std::unique_ptr<PlanStageStats> getStats() final;

    const SpecificStats* getSpecificStats() const final;

    static const char* kStageType;

private:
    WorkingSet* _ws;

    // Stats
    ShardingFilterStats _specificStats;

    // Note: it is important that this is the metadata from the time this stage is constructed.
    // See class comment for details.
    ScopedCollectionMetadata _metadata;
};

}  // namespace mongo
