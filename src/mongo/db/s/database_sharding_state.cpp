
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include "mongo/db/s/database_sharding_state.h"

#include "mongo/db/operation_context.h"
#include "mongo/db/s/operation_sharding_state.h"
#include "mongo/s/database_version_helpers.h"
#include "mongo/s/stale_exception.h"
#include "mongo/util/fail_point_service.h"
#include "mongo/util/log.h"

namespace mongo {

const Database::Decoration<DatabaseShardingState> DatabaseShardingState::get =
    Database::declareDecoration<DatabaseShardingState>();

DatabaseShardingState::DatabaseShardingState() = default;

void DatabaseShardingState::enterCriticalSectionCatchUpPhase(OperationContext* opCtx, DSSLock&) {
    invariant(opCtx->lockState()->isDbLockedForMode(get.owner(this)->name(), MODE_X));
    _critSec.enterCriticalSectionCatchUpPhase();
}

void DatabaseShardingState::enterCriticalSectionCommitPhase(OperationContext* opCtx, DSSLock&) {
    invariant(opCtx->lockState()->isDbLockedForMode(get.owner(this)->name(), MODE_X));
    _critSec.enterCriticalSectionCommitPhase();
}

void DatabaseShardingState::exitCriticalSection(OperationContext* opCtx,
                                                boost::optional<DatabaseVersion> newDbVersion,
                                                DSSLock&) {
    invariant(opCtx->lockState()->isDbLockedForMode(get.owner(this)->name(), MODE_IX));
    _critSec.exitCriticalSection();
    _dbVersion = newDbVersion;
}

boost::optional<DatabaseVersion> DatabaseShardingState::getDbVersion(OperationContext* opCtx,
                                                                     DSSLock&) const {
    if (!opCtx->lockState()->isDbLockedForMode(get.owner(this)->name(), MODE_X)) {
        invariant(opCtx->lockState()->isDbLockedForMode(get.owner(this)->name(), MODE_IS));
    }
    return _dbVersion;
}

void DatabaseShardingState::setDbVersion(OperationContext* opCtx,
                                         boost::optional<DatabaseVersion> newDbVersion,
                                         DSSLock&) {
    invariant(opCtx->lockState()->isDbLockedForMode(get.owner(this)->name(), MODE_X));
    log() << "setting this node's cached database version for " << get.owner(this)->name() << " to "
          << (newDbVersion ? newDbVersion->toBSON() : BSONObj());
    _dbVersion = newDbVersion;
}

void DatabaseShardingState::checkDbVersion(OperationContext* opCtx, DSSLock&) const {
    invariant(opCtx->lockState()->isLocked());
    const auto dbName = get.owner(this)->name();

    const auto clientDbVersion = OperationShardingState::get(opCtx).getDbVersion(dbName);
    if (!clientDbVersion) {
        return;
    }

    auto criticalSectionSignal = _critSec.getSignal(opCtx->lockState()->isWriteLocked()
                                                        ? ShardingMigrationCriticalSection::kWrite
                                                        : ShardingMigrationCriticalSection::kRead);
    if (criticalSectionSignal) {
        OperationShardingState::get(opCtx).setMovePrimaryCriticalSectionSignal(
            criticalSectionSignal);

        uasserted(StaleDbRoutingVersion(dbName, *clientDbVersion, boost::none),
                  "movePrimary critical section active");
    }

    uassert(StaleDbRoutingVersion(dbName, *clientDbVersion, boost::none),
            "don't know dbVersion",
            _dbVersion);
    uassert(StaleDbRoutingVersion(dbName, *clientDbVersion, *_dbVersion),
            "dbVersion mismatch",
            databaseVersion::equal(*clientDbVersion, *_dbVersion));
}

MovePrimarySourceManager* DatabaseShardingState::getMovePrimarySourceManager(DSSLock&) {
    return _sourceMgr;
}

void DatabaseShardingState::setMovePrimarySourceManager(OperationContext* opCtx,
                                                        MovePrimarySourceManager* sourceMgr,
                                                        DSSLock&) {
    invariant(opCtx->lockState()->isDbLockedForMode(get.owner(this)->name(), MODE_X));
    invariant(sourceMgr);
    invariant(!_sourceMgr);

    _sourceMgr = sourceMgr;
}

void DatabaseShardingState::clearMovePrimarySourceManager(OperationContext* opCtx, DSSLock&) {
    invariant(opCtx->lockState()->isDbLockedForMode(get.owner(this)->name(), MODE_IX));
    _sourceMgr = nullptr;
}

}  // namespace mongo
