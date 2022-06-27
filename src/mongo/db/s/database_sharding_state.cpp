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


#include "mongo/platform/basic.h"

#include "mongo/db/s/database_sharding_state.h"

#include "mongo/db/operation_context.h"
#include "mongo/db/s/operation_sharding_state.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/logv2/log.h"
#include "mongo/s/database_version.h"
#include "mongo/s/stale_exception.h"
#include "mongo/util/fail_point.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding


namespace mongo {
namespace {

class DatabaseShardingStateMap {
    DatabaseShardingStateMap& operator=(const DatabaseShardingStateMap&) = delete;
    DatabaseShardingStateMap(const DatabaseShardingStateMap&) = delete;

public:
    static const ServiceContext::Decoration<DatabaseShardingStateMap> get;

    DatabaseShardingStateMap() {}

    std::shared_ptr<DatabaseShardingState> getOrCreate(const StringData dbName) {
        stdx::lock_guard<Latch> lg(_mutex);

        auto it = _databases.find(dbName);
        if (it == _databases.end()) {
            auto inserted =
                _databases.try_emplace(dbName, std::make_unique<DatabaseShardingState>(dbName));
            invariant(inserted.second);
            it = std::move(inserted.first);
        }

        return it->second;
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
    return databasesMap.getOrCreate(dbName).get();
}

void DatabaseShardingState::checkIsPrimaryShardForDb(OperationContext* opCtx, StringData dbName) {
    invariant(dbName != NamespaceString::kConfigDb);

    uassert(ErrorCodes::IllegalOperation,
            "Request sent without attaching database version",
            OperationShardingState::get(opCtx).hasDbVersion());

    const auto dbPrimaryShardId = [&]() {
        // TODO SERVER-63706 Use dbName directly
        Lock::DBLock dbWriteLock(opCtx, DatabaseName(boost::none, dbName), MODE_IS);
        auto dss = DatabaseShardingState::get(opCtx, dbName);
        auto dssLock = DatabaseShardingState::DSSLock::lockShared(opCtx, dss);
        // The following call will also ensure that the database version matches
        return dss->getDatabaseInfo(opCtx, dssLock).getPrimary();
    }();

    const auto thisShardId = ShardingState::get(opCtx)->shardId();

    uassert(ErrorCodes::IllegalOperation,
            str::stream() << "This is not the primary shard for db " << dbName
                          << " expected: " << dbPrimaryShardId << " shardId: " << thisShardId,
            dbPrimaryShardId == thisShardId);
}

std::shared_ptr<DatabaseShardingState> DatabaseShardingState::getSharedForLockFreeReads(
    OperationContext* opCtx, const StringData dbName) {
    auto& databasesMap = DatabaseShardingStateMap::get(opCtx->getServiceContext());
    return databasesMap.getOrCreate(dbName);
}

void DatabaseShardingState::enterCriticalSectionCatchUpPhase(OperationContext* opCtx,
                                                             DSSLock&,
                                                             const BSONObj& reason) {
    invariant(opCtx->lockState()->isDbLockedForMode(_dbName, MODE_X));
    _critSec.enterCriticalSectionCatchUpPhase(reason);
}

void DatabaseShardingState::enterCriticalSectionCommitPhase(OperationContext* opCtx,
                                                            DSSLock&,
                                                            const BSONObj& reason) {
    invariant(opCtx->lockState()->isDbLockedForMode(_dbName, MODE_X));
    _critSec.enterCriticalSectionCommitPhase(reason);
}

void DatabaseShardingState::exitCriticalSection(OperationContext* opCtx, const BSONObj& reason) {
    const auto dssLock = DSSLock::lockExclusive(opCtx, this);
    _critSec.exitCriticalSection(reason);
}

DatabaseType DatabaseShardingState::getDatabaseInfo(OperationContext* opCtx,
                                                    DSSLock& dssLock) const {
    checkDbVersion(opCtx, dssLock);
    invariant(_optDatabaseInfo);
    return _optDatabaseInfo.get();
}

boost::optional<DatabaseVersion> DatabaseShardingState::getDbVersion(OperationContext* opCtx,
                                                                     DSSLock&) const {
    if (!opCtx->lockState()->isDbLockedForMode(_dbName, MODE_X)) {
        invariant(opCtx->lockState()->isDbLockedForMode(_dbName, MODE_IS));
    }
    return (_optDatabaseInfo) ? boost::optional<DatabaseVersion>(_optDatabaseInfo->getVersion())
                              : boost::none;
}

void DatabaseShardingState::clearDatabaseInfo(OperationContext* opCtx) {
    LOGV2(5369110, "Clearing node's cached database info", "db"_attr = _dbName);
    const auto dssLock = DSSLock::lockExclusive(opCtx, this);
    _optDatabaseInfo = boost::none;
}

void DatabaseShardingState::setDatabaseInfo(OperationContext* opCtx,
                                            DatabaseType&& newDatabaseInfo,
                                            DSSLock& dssLock) {
    invariant(opCtx->lockState()->isDbLockedForMode(_dbName, MODE_X));
    LOGV2(5369111,
          "Setting this node's cached database info",
          "db"_attr = _dbName,
          "newDatabaseVersion"_attr = newDatabaseInfo.getVersion());
    _optDatabaseInfo.emplace(std::move(newDatabaseInfo));
}

void DatabaseShardingState::checkDbVersion(OperationContext* opCtx, DSSLock&) const {
    invariant(opCtx->lockState()->isLocked());

    const auto clientDbVersion = OperationShardingState::get(opCtx).getDbVersion(_dbName);
    if (!clientDbVersion)
        return;

    {
        auto criticalSectionSignal = _critSec.getSignal(
            opCtx->lockState()->isWriteLocked() ? ShardingMigrationCriticalSection::kWrite
                                                : ShardingMigrationCriticalSection::kRead);
        const std::string reason =
            _critSec.getReason() ? _critSec.getReason()->toString() : "unknown";
        uassert(
            StaleDbRoutingVersion(_dbName, *clientDbVersion, boost::none, criticalSectionSignal),
            str::stream() << "The critical section for " << _dbName
                          << " is acquired with reason: " << reason,
            !criticalSectionSignal);
    }

    uassert(StaleDbRoutingVersion(_dbName, *clientDbVersion, boost::none),
            str::stream() << "sharding status of database " << _dbName
                          << " is not currently known and needs to be recovered",
            _optDatabaseInfo);

    const auto& dbVersion = _optDatabaseInfo->getVersion();
    uassert(StaleDbRoutingVersion(_dbName, *clientDbVersion, dbVersion),
            str::stream() << "dbVersion mismatch for database " << _dbName,
            *clientDbVersion == dbVersion);
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

void DatabaseShardingState::clearMovePrimarySourceManager(OperationContext* opCtx) {
    invariant(opCtx->lockState()->isDbLockedForMode(_dbName, MODE_IX));
    const auto dssLock = DSSLock::lockExclusive(opCtx, this);
    _sourceMgr = nullptr;
}

}  // namespace mongo
