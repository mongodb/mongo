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

namespace mongo {
const Database::Decoration<DatabaseShardingState> DatabaseShardingState::get =
    Database::declareDecorationWithOwner<DatabaseShardingState>();

DatabaseShardingState::DatabaseShardingState(Database* db) : _db(db) {}

void DatabaseShardingState::enterCriticalSection(OperationContext* opCtx) {
    invariant(opCtx->lockState()->isDbLockedForMode(_db->name(), MODE_X));
    invariant(!_critSecSignal);
    _critSecSignal = std::make_shared<Notification<void>>();
    // TODO (SERVER-33313): call CursorManager::invalidateAll() on all collections in this database
    // with 'fromMovePrimary=true' and a predicate to only invalidate the cursor if the opCtx on its
    // PlanExecutor has a client dbVersion.
}

void DatabaseShardingState::exitCriticalSection(OperationContext* opCtx,
                                                boost::optional<DatabaseVersion> newDbVersion) {
    invariant(opCtx->lockState()->isDbLockedForMode(_db->name(), MODE_X));
    invariant(_critSecSignal);
    _critSecSignal->set();
    _critSecSignal.reset();
    _dbVersion = newDbVersion;
}

std::shared_ptr<Notification<void>> DatabaseShardingState::getCriticalSectionSignal() const {
    return _critSecSignal;
}

void DatabaseShardingState::setDbVersion(OperationContext* opCtx,
                                         boost::optional<DatabaseVersion> newDbVersion) {
    invariant(opCtx->lockState()->isDbLockedForMode(_db->name(), MODE_X));
    _dbVersion = newDbVersion;
}

void DatabaseShardingState::checkDbVersion(OperationContext* opCtx) const {
    invariant(opCtx->lockState()->isLocked());

    if (_critSecSignal) {
        // TODO (SERVER-33097): Set movePrimary critical section signal on the
        // OperationShardingState (so that the operation can wait outside the DBLock for the
        // movePrimary critical section to end before returning to the client).

        // TODO (SERVER-33098): throw StaleDbVersion.
    }

    // TODO (SERVER-33098): check the client's dbVersion (from the OperationShardingState) against
    // _dbVersion, and throw StaleDbVersion if they don't match.
    return;
}

}  // namespace mongo
