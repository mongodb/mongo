/**
 *    Copyright (C) 2018 MongoDB Inc.
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
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/db/s/database_sharding_state.h"

#include "mongo/db/operation_context.h"
#include "mongo/db/s/operation_sharding_state.h"
#include "mongo/s/stale_exception.h"
#include "mongo/util/fail_point_service.h"
#include "mongo/util/stacktrace.h"

namespace mongo {

MONGO_FP_DECLARE(checkForDbVersionMismatch);

const Database::Decoration<DatabaseShardingState> DatabaseShardingState::get =
    Database::declareDecoration<DatabaseShardingState>();

DatabaseShardingState::DatabaseShardingState() = default;

void DatabaseShardingState::enterCriticalSectionCatchUpPhase(OperationContext* opCtx) {
    invariant(opCtx->lockState()->isDbLockedForMode(get.owner(this)->name(), MODE_X));
    _critSec.enterCriticalSectionCatchUpPhase();
    // TODO (SERVER-33313): call CursorManager::invalidateAll() on all collections in this database
    // with 'fromMovePrimary=true' and a predicate to only invalidate the cursor if the opCtx on its
    // PlanExecutor has a client dbVersion.
}

void DatabaseShardingState::enterCriticalSectionCommitPhase(OperationContext* opCtx) {
    invariant(opCtx->lockState()->isDbLockedForMode(get.owner(this)->name(), MODE_X));
    _critSec.enterCriticalSectionCommitPhase();
}

void DatabaseShardingState::exitCriticalSection(OperationContext* opCtx,
                                                boost::optional<DatabaseVersion> newDbVersion) {
    invariant(opCtx->lockState()->isDbLockedForMode(get.owner(this)->name(), MODE_X));
    _critSec.exitCriticalSection();
    _dbVersion = newDbVersion;
}

void DatabaseShardingState::setDbVersion(OperationContext* opCtx,
                                         boost::optional<DatabaseVersion> newDbVersion) {
    invariant(opCtx->lockState()->isDbLockedForMode(get.owner(this)->name(), MODE_X));
    _dbVersion = newDbVersion;
}

void DatabaseShardingState::checkDbVersion(OperationContext* opCtx) const {
    invariant(opCtx->lockState()->isLocked());
    const auto dbName = get.owner(this)->name();

    const auto clientDbVersion = OperationShardingState::get(opCtx).getDbVersion(dbName);
    if (!clientDbVersion) {
        return;
    }

    if (!MONGO_FAIL_POINT(checkForDbVersionMismatch)) {
        // While checking the dbVersion and triggering a cache refresh on StaleDbVersion is under
        // development, only check for dbVersion mismatch if explicitly asked to.
        return;
    }

    auto criticalSectionSignal = _critSec.getSignal(opCtx->lockState()->isWriteLocked()
                                                        ? ShardingMigrationCriticalSection::kWrite
                                                        : ShardingMigrationCriticalSection::kRead);
    if (criticalSectionSignal) {
        // TODO (SERVER-33773): Set movePrimary critical section signal on the
        // OperationShardingState (so that the operation can wait outside the DBLock for the
        // movePrimary critical section to end before returning to the client).

        uasserted(StaleDbRoutingVersion(dbName, *clientDbVersion, boost::none),
                  "migration critical section active");
    }

    uassert(StaleDbRoutingVersion(dbName, *clientDbVersion, boost::none),
            "don't know dbVersion",
            _dbVersion);
    uassert(StaleDbRoutingVersion(dbName, *clientDbVersion, *_dbVersion),
            "dbVersion mismatch",
            *clientDbVersion == *_dbVersion);
}

}  // namespace mongo
