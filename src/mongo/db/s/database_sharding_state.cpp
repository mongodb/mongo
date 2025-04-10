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

#include "mongo/db/s/database_sharding_state.h"

#include <absl/container/node_hash_map.h>
#include <absl/meta/type_traits.h>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <fmt/format.h>
#include <memory>
#include <mutex>
#include <string>

#include <boost/optional/optional.hpp>

#include "mongo/base/error_codes.h"
#include "mongo/db/cluster_role.h"
#include "mongo/db/concurrency/lock_manager_defs.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/s/operation_sharding_state.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
#include "mongo/db/shard_id.h"
#include "mongo/db/transaction_resources.h"
#include "mongo/logv2/log.h"
#include "mongo/s/sharding_state.h"
#include "mongo/s/stale_exception.h"
#include "mongo/stdx/mutex.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/database_name_util.h"
#include "mongo/util/decorable.h"
#include "mongo/util/str.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {
namespace {

class DatabaseShardingStateMap {
    DatabaseShardingStateMap& operator=(const DatabaseShardingStateMap&) = delete;
    DatabaseShardingStateMap(const DatabaseShardingStateMap&) = delete;

public:
    static const ServiceContext::Decoration<DatabaseShardingStateMap> get;

    DatabaseShardingStateMap() {}

    struct DSSAndLock {
        DSSAndLock(const DatabaseName& dbName)
            : dss(std::make_unique<DatabaseShardingState>(dbName)) {}

        std::shared_mutex dssMutex;  // NOLINT
        std::unique_ptr<DatabaseShardingState> dss;
    };

    DSSAndLock* getOrCreate(const DatabaseName& dbName) {
        stdx::lock_guard<stdx::mutex> lg(_mutex);

        auto it = _databases.find(dbName);
        if (it == _databases.end()) {
            auto inserted = _databases.try_emplace(dbName, std::make_unique<DSSAndLock>(dbName));
            invariant(inserted.second);
            it = std::move(inserted.first);
        }

        return it->second.get();
    }

    std::vector<DatabaseName> getDatabaseNames() {
        stdx::lock_guard lg(_mutex);
        std::vector<DatabaseName> result;
        result.reserve(_databases.size());
        for (const auto& [dbName, _] : _databases) {
            result.emplace_back(dbName);
        }
        return result;
    }

private:
    stdx::mutex _mutex;

    // Entries of the _databases map must never be deleted or replaced. This is to guarantee that a
    // 'dbName' is always associated to the same 'ResourceMutex'.
    using DatabasesMap = stdx::unordered_map<DatabaseName, std::unique_ptr<DSSAndLock>>;
    DatabasesMap _databases;
};

const ServiceContext::Decoration<DatabaseShardingStateMap> DatabaseShardingStateMap::get =
    ServiceContext::declareDecoration<DatabaseShardingStateMap>();


void checkPlacementConflictTimestamp(const boost::optional<LogicalTime> atClusterTime,
                                     const DatabaseVersion& receivedDatabaseVersion,
                                     const DatabaseName& dbName,
                                     const DatabaseVersion& installedDatabaseVersion) {
    // placementConflictTimestamp equal to Timestamp(0, 0) means ignore, even for atClusterTime
    // transactions.
    const auto shouldIgnorePlacementConflict = receivedDatabaseVersion.getPlacementConflictTime()
        ? receivedDatabaseVersion.getPlacementConflictTime()->asTimestamp() == Timestamp(0, 0)
        : false;

    if (atClusterTime && !shouldIgnorePlacementConflict) {
        uassert(ErrorCodes::MigrationConflict,
                str::stream() << "Database " << dbName.toStringForErrorMsg()
                              << " has undergone a catalog change operation at time "
                              << installedDatabaseVersion.getTimestamp()
                              << " and no longer satisfies the requirements for the current "
                                 "transaction which requires "
                              << atClusterTime->asTimestamp() << ". Transaction will be aborted.",
                atClusterTime->asTimestamp() >= installedDatabaseVersion.getTimestamp());
    } else if (receivedDatabaseVersion.getPlacementConflictTime() &&
               !shouldIgnorePlacementConflict) {
        uassert(ErrorCodes::MigrationConflict,
                str::stream() << "Database " << dbName.toStringForErrorMsg()
                              << " has undergone a catalog change operation at time "
                              << installedDatabaseVersion.getTimestamp()
                              << " and no longer satisfies the requirements for the current "
                                 "transaction which requires "
                              << receivedDatabaseVersion.getPlacementConflictTime()->asTimestamp()
                              << ". Transaction will be aborted.",
                receivedDatabaseVersion.getPlacementConflictTime()->asTimestamp() >=
                    installedDatabaseVersion.getTimestamp());
    }
}

}  // namespace

DatabaseShardingState::DatabaseShardingState(const DatabaseName& dbName) : _dbName(dbName) {}

DatabaseShardingState::ScopedExclusiveDatabaseShardingState::ScopedExclusiveDatabaseShardingState(
    std::unique_lock<std::shared_mutex> lock, DatabaseShardingState* dss)  // NOLINT
    : _lock(std::move(lock)), _dss(dss) {}

DatabaseShardingState::ScopedSharedDatabaseShardingState::ScopedSharedDatabaseShardingState(
    std::shared_lock<std::shared_mutex> lock, DatabaseShardingState* dss)  // NOLINT
    : _lock(std::move(lock)), _dss(dss) {}

DatabaseShardingState::ScopedExclusiveDatabaseShardingState DatabaseShardingState::acquireExclusive(
    OperationContext* opCtx, const DatabaseName& dbName) {

    DatabaseShardingStateMap::DSSAndLock* dssAndLock =
        DatabaseShardingStateMap::get(opCtx->getServiceContext()).getOrCreate(dbName);

    // First lock the shared_mutex associated to this dbName to guarantee stability of the
    // DatabaseShardingState pointer. After that, it is safe to get and store the
    // DatabaseShadingState*, as long as the shared_mutex is kept locked.
    return ScopedExclusiveDatabaseShardingState(std::unique_lock(dssAndLock->dssMutex),
                                                dssAndLock->dss.get());
}

DatabaseShardingState::ScopedSharedDatabaseShardingState DatabaseShardingState::acquireShared(
    OperationContext* opCtx, const DatabaseName& dbName) {

    DatabaseShardingStateMap::DSSAndLock* dssAndLock =
        DatabaseShardingStateMap::get(opCtx->getServiceContext()).getOrCreate(dbName);

    // First lock the shared_mutex associated to this dbName to guarantee stability of the
    // DatabaseShardingState pointer. After that, it is safe to get and store the
    // DatabaseShadingState*, as long as the shared_mutex is kept locked.
    return ScopedSharedDatabaseShardingState(std::shared_lock(dssAndLock->dssMutex),  // NOLINT
                                             dssAndLock->dss.get());
}

DatabaseShardingState::ScopedExclusiveDatabaseShardingState
DatabaseShardingState::assertDbLockedAndAcquireExclusive(OperationContext* opCtx,
                                                         const DatabaseName& dbName) {
    dassert(shard_role_details::getLocker(opCtx)->isDbLockedForMode(dbName, MODE_IS));
    return acquireExclusive(opCtx, dbName);
}

DatabaseShardingState::ScopedSharedDatabaseShardingState
DatabaseShardingState::assertDbLockedAndAcquireShared(OperationContext* opCtx,
                                                      const DatabaseName& dbName) {
    dassert(shard_role_details::getLocker(opCtx)->isDbLockedForMode(dbName, MODE_IS));
    return acquireShared(opCtx, dbName);
}

std::vector<DatabaseName> DatabaseShardingState::getDatabaseNames(OperationContext* opCtx) {
    auto& databasesMap = DatabaseShardingStateMap::get(opCtx->getServiceContext());
    return databasesMap.getDatabaseNames();
}

void DatabaseShardingState::assertMatchingDbVersion(OperationContext* opCtx,
                                                    const DatabaseName& dbName) {
    const auto receivedVersion = OperationShardingState::get(opCtx).getDbVersion(dbName);
    if (!receivedVersion) {
        return;
    }

    const auto scopedDss = acquireShared(opCtx, dbName);
    scopedDss->assertMatchingDbVersion(opCtx, *receivedVersion);
}

void DatabaseShardingState::assertMatchingDbVersion(OperationContext* opCtx,
                                                    const DatabaseVersion& receivedVersion) const {
    {
        const auto critSecSignal =
            getCriticalSectionSignal(shard_role_details::getLocker(opCtx)->isWriteLocked()
                                         ? ShardingMigrationCriticalSection::kWrite
                                         : ShardingMigrationCriticalSection::kRead);
        const auto optCritSecReason = getCriticalSectionReason();

        uassert(StaleDbRoutingVersion(_dbName, receivedVersion, boost::none, critSecSignal),
                str::stream() << "The critical section for the database "
                              << _dbName.toStringForErrorMsg()
                              << " is acquired with reason: " << getCriticalSectionReason(),
                !critSecSignal);
    }

    const auto wantedVersion = getDbVersion(opCtx);
    uassert(StaleDbRoutingVersion(_dbName, receivedVersion, boost::none),
            str::stream() << "No cached info for the database " << _dbName.toStringForErrorMsg(),
            wantedVersion);

    uassert(StaleDbRoutingVersion(_dbName, receivedVersion, *wantedVersion),
            str::stream() << "Version mismatch for the database " << _dbName.toStringForErrorMsg(),
            receivedVersion == *wantedVersion);

    // Check placement conflicts for multi-document transactions.
    const auto atClusterTime = repl::ReadConcernArgs::get(opCtx).getArgsAtClusterTime();
    checkPlacementConflictTimestamp(atClusterTime, receivedVersion, _dbName, *wantedVersion);
}

void DatabaseShardingState::assertIsPrimaryShardForDb(OperationContext* opCtx) const {
    if (_dbName == DatabaseName::kConfig || _dbName == DatabaseName::kAdmin) {
        uassert(7393700,
                fmt::format("The config server is the primary shard for database: {}",
                            _dbName.toStringForErrorMsg()),
                serverGlobalParams.clusterRole.has(ClusterRole::ConfigServer));
        return;
    }

    auto expectedDbVersion = OperationShardingState::get(opCtx).getDbVersion(_dbName);

    uassert(ErrorCodes::IllegalOperation,
            str::stream() << "Received request without the version for the database "
                          << _dbName.toStringForErrorMsg(),
            expectedDbVersion);

    assertMatchingDbVersion(opCtx, *expectedDbVersion);

    const auto primaryShardId = _dbInfo->getPrimary();
    const auto thisShardId = ShardingState::get(opCtx)->shardId();
    uassert(ErrorCodes::IllegalOperation,
            str::stream() << "This is not the primary shard for the database "
                          << _dbName.toStringForErrorMsg() << ". Expected: " << primaryShardId
                          << " Actual: " << thisShardId,
            primaryShardId == thisShardId);
}

void DatabaseShardingState::setDbInfo_DEPRECATED(OperationContext* opCtx,
                                                 const DatabaseType& dbInfo) {
    invariant(shard_role_details::getLocker(opCtx)->isDbLockedForMode(_dbName, MODE_IX));

    LOGV2(7286900,
          "Setting this node's cached database info",
          logAttrs(_dbName),
          "dbVersion"_attr = dbInfo.getVersion());

    _dbInfo.emplace(dbInfo);
}

void DatabaseShardingState::setDbInfo(OperationContext* opCtx, const DatabaseType& dbInfo) {
    // During the recovery phase, when the ShardingRecoveryService is reading from disk and
    // populating the DatabaseShardingState, the ShardingState is not yet initialized. Therefore,
    // the following sanity check cannot be performed, as it requires knowing which ShardId this
    // shard belongs to.
    if (ShardingState::get(opCtx)->enabled()) {
        const auto thisShardId = ShardingState::get(opCtx)->shardId();
        tassert(
            10003604,
            fmt::format(
                "Expected to be setting this node's cached database info with its corresponding "
                "database version. Found primary shard in the database info: {}, expected: {} for "
                "database: {} and dbVersion: {}.",
                dbInfo.getPrimary().toString(),
                thisShardId.toString(),
                _dbName.toStringForErrorMsg(),
                dbInfo.getVersion().toString()),
            dbInfo.getPrimary() == thisShardId);
    }

    LOGV2(10003605,
          "Setting this node's cached database info",
          logAttrs(_dbName),
          "dbVersion"_attr = dbInfo.getVersion());

    _dbInfo.emplace(dbInfo);
}

void DatabaseShardingState::clearDbInfo_DEPRECATED(OperationContext* opCtx,
                                                   bool cancelOngoingRefresh) {
    invariant(shard_role_details::getLocker(opCtx)->isDbLockedForMode(_dbName, MODE_IX));

    if (cancelOngoingRefresh) {
        _cancelDbMetadataRefresh();
    }

    LOGV2(7286901, "Clearing this node's cached database info", logAttrs(_dbName));

    _dbInfo = boost::none;
}

void DatabaseShardingState::clearDbInfo(OperationContext* opCtx) {
    LOGV2(10003602, "Clearing this node's cached database info", logAttrs(_dbName));

    _dbInfo = boost::none;
}

boost::optional<DatabaseVersion> DatabaseShardingState::getDbVersion(
    OperationContext* opCtx) const {
    return _dbInfo ? boost::optional<DatabaseVersion>(_dbInfo->getVersion()) : boost::none;
}

void DatabaseShardingState::enterCriticalSectionCatchUpPhase(OperationContext* opCtx,
                                                             const BSONObj& reason) {
    _critSec.enterCriticalSectionCatchUpPhase(reason);

    _cancelDbMetadataRefresh();
}

void DatabaseShardingState::enterCriticalSectionCommitPhase(OperationContext* opCtx,
                                                            const BSONObj& reason) {
    _critSec.enterCriticalSectionCommitPhase(reason);
}

void DatabaseShardingState::exitCriticalSection(OperationContext* opCtx, const BSONObj& reason) {
    _critSec.exitCriticalSection(reason);
}

void DatabaseShardingState::exitCriticalSectionNoChecks(OperationContext* opCtx) {
    _critSec.exitCriticalSectionNoChecks();
}

void DatabaseShardingState::setMovePrimaryInProgress(OperationContext* opCtx) {
    invariant(shard_role_details::getLocker(opCtx)->isDbLockedForMode(_dbName, MODE_X));
    _movePrimaryInProgress = true;
}

void DatabaseShardingState::unsetMovePrimaryInProgress(OperationContext* opCtx) {
    invariant(shard_role_details::getLocker(opCtx)->isDbLockedForMode(_dbName, MODE_IX));
    _movePrimaryInProgress = false;
}

void DatabaseShardingState::setDbMetadataRefreshFuture(SharedSemiFuture<void> future,
                                                       CancellationSource cancellationSource) {
    invariant(!_dbMetadataRefresh);
    _dbMetadataRefresh.emplace(std::move(future), std::move(cancellationSource));
}

boost::optional<SharedSemiFuture<void>> DatabaseShardingState::getDbMetadataRefreshFuture() const {
    return _dbMetadataRefresh ? boost::optional<SharedSemiFuture<void>>(_dbMetadataRefresh->future)
                              : boost::none;
}

void DatabaseShardingState::resetDbMetadataRefreshFuture() {
    _dbMetadataRefresh = boost::none;
}

void DatabaseShardingState::_cancelDbMetadataRefresh() {
    if (_dbMetadataRefresh) {
        _dbMetadataRefresh->cancellationSource.cancel();
    }
}

boost::optional<bool> DatabaseShardingState::_isPrimaryShardForDb(OperationContext* opCtx) const {
    return _dbInfo
        ? boost::optional<bool>(_dbInfo->getPrimary() == ShardingState::get(opCtx)->shardId())
        : boost::none;
}

}  // namespace mongo
