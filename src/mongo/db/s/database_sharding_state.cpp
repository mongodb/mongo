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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include "mongo/db/s/database_sharding_state.h"

#include "mongo/db/operation_context.h"
#include "mongo/db/s/operation_sharding_state.h"
#include "mongo/logv2/log.h"
#include "mongo/s/database_version_helpers.h"
#include "mongo/s/stale_exception.h"
#include "mongo/util/fail_point.h"

namespace mongo {
namespace {

class DatabaseShardingStateMap {
    DatabaseShardingStateMap& operator=(const DatabaseShardingStateMap&) = delete;
    DatabaseShardingStateMap(const DatabaseShardingStateMap&) = delete;

public:
    static const ServiceContext::Decoration<DatabaseShardingStateMap> get;

    DatabaseShardingStateMap() {}

    DatabaseShardingState& getOrCreate(const StringData dbName) {
        stdx::lock_guard<Latch> lg(_mutex);

        auto it = _databases.find(dbName);
        if (it == _databases.end()) {
            auto inserted =
                _databases.try_emplace(dbName, std::make_unique<DatabaseShardingState>(dbName));
            invariant(inserted.second);
            it = std::move(inserted.first);
        }

        return *it->second;
    }

private:
    using DatabasesMap = StringMap<std::shared_ptr<DatabaseShardingState>>;

    Mutex _mutex = MONGO_MAKE_LATCH("DatabaseShardingStateMap::_mutex");
    DatabasesMap _databases;
};

const ServiceContext::Decoration<DatabaseShardingStateMap> DatabaseShardingStateMap::get =
    ServiceContext::declareDecoration<DatabaseShardingStateMap>();

}  // namespace

DatabaseShardingState::DatabaseShardingState(const StringData dbName)
    : _dbName(dbName.toString()) {}

DatabaseShardingState* DatabaseShardingState::get(OperationContext* opCtx,
                                                  const StringData dbName) {
    // db lock must be held to have a reference to the database sharding state
    dassert(opCtx->lockState()->isDbLockedForMode(dbName, MODE_IS));

    auto& databasesMap = DatabaseShardingStateMap::get(opCtx->getServiceContext());
    return &databasesMap.getOrCreate(dbName);
}

void DatabaseShardingState::enterCriticalSectionCatchUpPhase(OperationContext* opCtx, DSSLock&) {
    invariant(opCtx->lockState()->isDbLockedForMode(_dbName, MODE_X));
    _critSec.enterCriticalSectionCatchUpPhase();
}

void DatabaseShardingState::enterCriticalSectionCommitPhase(OperationContext* opCtx, DSSLock&) {
    invariant(opCtx->lockState()->isDbLockedForMode(_dbName, MODE_X));
    _critSec.enterCriticalSectionCommitPhase();
}

void DatabaseShardingState::exitCriticalSection(OperationContext* opCtx,
                                                boost::optional<DatabaseVersion> newDbVersion,
                                                DSSLock&) {
    invariant(opCtx->lockState()->isDbLockedForMode(_dbName, MODE_IX));
    _critSec.exitCriticalSection();
    _dbVersion = newDbVersion;
}

boost::optional<DatabaseVersion> DatabaseShardingState::getDbVersion(OperationContext* opCtx,
                                                                     DSSLock&) const {
    if (!opCtx->lockState()->isDbLockedForMode(_dbName, MODE_X)) {
        invariant(opCtx->lockState()->isDbLockedForMode(_dbName, MODE_IS));
    }
    return _dbVersion;
}

void DatabaseShardingState::setDbVersion(OperationContext* opCtx,
                                         boost::optional<DatabaseVersion> newDbVersion,
                                         DSSLock&) {
    invariant(opCtx->lockState()->isDbLockedForMode(_dbName, MODE_X));
    LOGV2(21950,
          "Setting this node's cached database version for {db} to {newDbVersion}",
          "Setting this node's cached database version",
          "db"_attr = _dbName,
          "newDbVersion"_attr = (newDbVersion ? newDbVersion->toBSON() : BSONObj()));
    _dbVersion = newDbVersion;
}

void DatabaseShardingState::checkDbVersion(OperationContext* opCtx, DSSLock&) const {
    invariant(opCtx->lockState()->isLocked());

    const auto clientDbVersion = OperationShardingState::get(opCtx).getDbVersion(_dbName);
    if (!clientDbVersion)
        return;

    auto criticalSectionSignal = _critSec.getSignal(opCtx->lockState()->isWriteLocked()
                                                        ? ShardingMigrationCriticalSection::kWrite
                                                        : ShardingMigrationCriticalSection::kRead);
    if (criticalSectionSignal) {
        OperationShardingState::get(opCtx).setMovePrimaryCriticalSectionSignal(
            criticalSectionSignal);

        uasserted(StaleDbRoutingVersion(_dbName, *clientDbVersion, boost::none),
                  "movePrimary critical section active");
    }

    uassert(StaleDbRoutingVersion(_dbName, *clientDbVersion, boost::none),
            "don't know dbVersion",
            _dbVersion);
    uassert(StaleDbRoutingVersion(_dbName, *clientDbVersion, *_dbVersion),
            "dbVersion mismatch",
            databaseVersion::equal(*clientDbVersion, *_dbVersion));
}

MovePrimarySourceManager* DatabaseShardingState::getMovePrimarySourceManager(DSSLock&) {
    return _sourceMgr;
}

void DatabaseShardingState::setMovePrimarySourceManager(OperationContext* opCtx,
                                                        MovePrimarySourceManager* sourceMgr,
                                                        DSSLock&) {
    invariant(opCtx->lockState()->isDbLockedForMode(_dbName, MODE_X));
    invariant(sourceMgr);
    invariant(!_sourceMgr);

    _sourceMgr = sourceMgr;
}

void DatabaseShardingState::clearMovePrimarySourceManager(OperationContext* opCtx, DSSLock&) {
    invariant(opCtx->lockState()->isDbLockedForMode(_dbName, MODE_IX));
    _sourceMgr = nullptr;
}

}  // namespace mongo
