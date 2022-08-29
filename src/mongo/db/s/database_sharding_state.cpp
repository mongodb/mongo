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
    // TODO SERVER-63706 Use dbName directly
    dassert(opCtx->lockState()->isDbLockedForMode(DatabaseName(boost::none, dbName), MODE_IS));

    auto& databasesMap = DatabaseShardingStateMap::get(opCtx->getServiceContext());
    return databasesMap.getOrCreate(dbName).get();
}

std::shared_ptr<DatabaseShardingState> DatabaseShardingState::getSharedForLockFreeReads(
    OperationContext* opCtx, const StringData dbName) {
    auto& databasesMap = DatabaseShardingStateMap::get(opCtx->getServiceContext());
    return databasesMap.getOrCreate(dbName);
}

void DatabaseShardingState::enterCriticalSectionCatchUpPhase(OperationContext* opCtx,
                                                             DSSLock& dssLock,
                                                             const BSONObj& reason) {
    invariant(opCtx->lockState()->isDbLockedForMode(DatabaseName(boost::none, _dbName), MODE_X));
    _critSec.enterCriticalSectionCatchUpPhase(reason);

    cancelDbMetadataRefresh(dssLock);
}

void DatabaseShardingState::enterCriticalSectionCommitPhase(OperationContext* opCtx,
                                                            DSSLock&,
                                                            const BSONObj& reason) {
    invariant(opCtx->lockState()->isDbLockedForMode(DatabaseName(boost::none, _dbName), MODE_X));
    _critSec.enterCriticalSectionCommitPhase(reason);
}

void DatabaseShardingState::exitCriticalSection(OperationContext* opCtx, const BSONObj& reason) {
    const auto dssLock = DSSLock::lockExclusive(opCtx, this);
    _critSec.exitCriticalSection(reason);
}

MovePrimarySourceManager* DatabaseShardingState::getMovePrimarySourceManager(DSSLock&) {
    return _sourceMgr;
}

void DatabaseShardingState::setMovePrimarySourceManager(OperationContext* opCtx,
                                                        MovePrimarySourceManager* sourceMgr,
                                                        DSSLock&) {
    invariant(opCtx->lockState()->isDbLockedForMode(DatabaseName(boost::none, _dbName), MODE_X));
    invariant(sourceMgr);
    invariant(!_sourceMgr);

    _sourceMgr = sourceMgr;
}

void DatabaseShardingState::clearMovePrimarySourceManager(OperationContext* opCtx) {
    invariant(opCtx->lockState()->isDbLockedForMode(DatabaseName(boost::none, _dbName), MODE_IX));
    const auto dssLock = DSSLock::lockExclusive(opCtx, this);
    _sourceMgr = nullptr;
}

void DatabaseShardingState::setDbMetadataRefreshFuture(SharedSemiFuture<void> future,
                                                       CancellationSource cancellationSource,
                                                       const DSSLock&) {
    invariant(!_dbMetadataRefresh);
    _dbMetadataRefresh.emplace(std::move(future), std::move(cancellationSource));
}

boost::optional<SharedSemiFuture<void>> DatabaseShardingState::getDbMetadataRefreshFuture(
    const DSSLock&) const {
    return _dbMetadataRefresh ? boost::optional<SharedSemiFuture<void>>(_dbMetadataRefresh->future)
                              : boost::none;
}

void DatabaseShardingState::resetDbMetadataRefreshFuture(const DSSLock&) {
    _dbMetadataRefresh = boost::none;
}

void DatabaseShardingState::cancelDbMetadataRefresh(const DSSLock&) {
    if (_dbMetadataRefresh) {
        _dbMetadataRefresh->cancellationSource.cancel();
    }
}

}  // namespace mongo
