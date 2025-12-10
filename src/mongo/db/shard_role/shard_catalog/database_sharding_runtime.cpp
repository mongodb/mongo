/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/db/shard_role/shard_catalog/database_sharding_runtime.h"

#include "mongo/base/error_codes.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
#include "mongo/db/shard_role/lock_manager/lock_manager_defs.h"
#include "mongo/db/shard_role/shard_catalog/operation_sharding_state.h"
#include "mongo/db/shard_role/transaction_resources.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/db/sharding_environment/sharding_feature_flags_gen.h"
#include "mongo/db/topology/cluster_role.h"
#include "mongo/db/topology/sharding_state.h"
#include "mongo/db/transaction/transaction_participant.h"
#include "mongo/db/versioning_protocol/stale_exception.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"
#include "mongo/util/testing_proctor.h"

#include <absl/container/node_hash_map.h>
#include <absl/meta/type_traits.h>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <fmt/format.h>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {
namespace {

// Check the placementConflictTime exists if we are running within a multi-document transaction.
void assertPlacementConflictTimePresentWhenRequired(
    OperationContext* opCtx,
    const DatabaseName& dbName,
    const boost::optional<LogicalTime> placementConflictTime) {
    if (opCtx->inMultiDocumentTransaction() && OperationShardingState::isComingFromRouter(opCtx) &&
        repl::ReadConcernArgs::get(opCtx).getLevel() !=
            repl::ReadConcernLevel::kSnapshotReadConcern) {
        if (placementConflictTime.has_value()) {
            return;
        }

        if (TestingProctor::instance().isEnabled()) {
            tasserted(9758701,
                      str::stream()
                          << "Routed operations in multi-document transactions with readConcern != "
                             "snapshot must carry a `placementConflictTime` for db "
                          << dbName.toStringForErrorMsg());
        } else {
            static logv2::SeveritySuppressor logSeverity{
                Minutes{1}, logv2::LogSeverity::Warning(), logv2::LogSeverity::Debug(5)};
            LOGV2_DEBUG(
                9758702,
                logSeverity().toInt(),
                "Detected a missing `placementConflictTime` for an operation in a multi-document "
                "transaction with readConcern != snapshot originating from a router.",
                "db"_attr = redact(dbName.toStringForErrorMsg()));
        }
    }
}

void checkPlacementConflictTimestamp(OperationContext* opCtx,
                                     const boost::optional<LogicalTime> atClusterTime,
                                     const DatabaseVersion& receivedDatabaseVersion,
                                     const DatabaseName& dbName,
                                     const DatabaseVersion& installedDatabaseVersion) {
    boost::optional<LogicalTime> placementConflictTime;
    bool skipAtClusterTimeAndPlacementConflictTimeChecks = false;

    if (feature_flags::gAddTransactionRuntimeContextAsAGenericArgument.isEnabled(
            VersionContext::getDecoration(opCtx),
            serverGlobalParams.featureCompatibility.acquireFCVSnapshot())) {
        // Fetch the placementConflictTime through the TransactionParticipant if we are within a
        // transaction.
        auto tp = TransactionParticipant::get(opCtx);
        if (tp) {
            placementConflictTime = tp.getPlacementConflictTimeForNonSnapshotReadConcern();

            const auto& createdDatabases = tp.getDatabasesCreatedAtTopRouter();
            skipAtClusterTimeAndPlacementConflictTimeChecks =
                (std::ranges::find(createdDatabases, dbName) != createdDatabases.end());
        }
    } else {
        // The placementConflictTimestamp equal to Timestamp(0, 0) means ignore, even for
        // atClusterTime transactions.
        placementConflictTime = receivedDatabaseVersion.getPlacementConflictTime_DEPRECATED();
        skipAtClusterTimeAndPlacementConflictTimeChecks = placementConflictTime.has_value() &&
            placementConflictTime->asTimestamp() == Timestamp(0, 0);
    }

    assertPlacementConflictTimePresentWhenRequired(opCtx, dbName, placementConflictTime);

    if (skipAtClusterTimeAndPlacementConflictTimeChecks) {
        return;
    }

    if (atClusterTime.has_value()) {
        uassert(ErrorCodes::MigrationConflict,
                str::stream() << "Database " << dbName.toStringForErrorMsg()
                              << " has undergone a catalog change operation at time "
                              << installedDatabaseVersion.getTimestamp()
                              << " and no longer satisfies the requirements for the current "
                                 "transaction which requires "
                              << atClusterTime->asTimestamp() << ". Transaction will be aborted.",
                atClusterTime->asTimestamp() >= installedDatabaseVersion.getTimestamp());
    } else if (placementConflictTime.has_value()) {
        uassert(ErrorCodes::MigrationConflict,
                str::stream() << "Database " << dbName.toStringForErrorMsg()
                              << " has undergone a catalog change operation at time "
                              << installedDatabaseVersion.getTimestamp()
                              << " and no longer satisfies the requirements for the current "
                                 "transaction which requires "
                              << placementConflictTime->asTimestamp()
                              << ". Transaction will be aborted.",
                placementConflictTime->asTimestamp() >= installedDatabaseVersion.getTimestamp());
    }
}

boost::optional<DatabaseVersion> getOperationReceivedVersion(OperationContext* opCtx,
                                                             const DatabaseName& dbName) {
    // If there is a version attached to the OperationContext, use it as the received version.
    if (OperationShardingState::isComingFromRouter(opCtx)) {
        return OperationShardingState::get(opCtx).getDbVersion(dbName);
    }

    // There is no database version information on the 'opCtx'. This means that the operation
    // represented by 'opCtx' is unversioned, and the database version is always OK for unversioned
    // operations.
    return boost::none;
}

}  // namespace

DatabaseShardingRuntime::DatabaseShardingRuntime(const DatabaseName& dbName)
    : _dbName(dbName), _dbMetadataAccessor(dbName) {}

DatabaseShardingRuntime::~DatabaseShardingRuntime() = default;

DatabaseShardingRuntime::ScopedSharedDatabaseShardingRuntime
DatabaseShardingRuntime::assertDbLockedAndAcquireShared(OperationContext* opCtx,
                                                        const DatabaseName& dbName) {
    dassert(shard_role_details::getLocker(opCtx)->isDbLockedForMode(dbName, MODE_IS));
    return ScopedSharedDatabaseShardingRuntime(
        ScopedDatabaseShardingState::acquireScopedDatabaseShardingState(opCtx, dbName, MODE_IS));
}

DatabaseShardingRuntime::ScopedExclusiveDatabaseShardingRuntime
DatabaseShardingRuntime::assertDbLockedAndAcquireExclusive(OperationContext* opCtx,
                                                           const DatabaseName& dbName) {
    dassert(shard_role_details::getLocker(opCtx)->isDbLockedForMode(dbName, MODE_IS));
    return ScopedExclusiveDatabaseShardingRuntime(
        ScopedDatabaseShardingState::acquireScopedDatabaseShardingState(opCtx, dbName, MODE_X));
}

DatabaseShardingRuntime::ScopedSharedDatabaseShardingRuntime DatabaseShardingRuntime::acquireShared(
    OperationContext* opCtx, const DatabaseName& dbName) {
    return ScopedSharedDatabaseShardingRuntime(
        ScopedDatabaseShardingState::acquireScopedDatabaseShardingState(opCtx, dbName, MODE_IS));
}

DatabaseShardingRuntime::ScopedExclusiveDatabaseShardingRuntime
DatabaseShardingRuntime::acquireExclusive(OperationContext* opCtx, const DatabaseName& dbName) {
    return ScopedExclusiveDatabaseShardingRuntime(
        ScopedDatabaseShardingState::acquireScopedDatabaseShardingState(opCtx, dbName, MODE_X));
}

DatabaseShardingRuntime::ScopedSharedDatabaseShardingRuntime::ScopedSharedDatabaseShardingRuntime(
    ScopedDatabaseShardingState&& scopedDss)
    : _scopedDss(std::move(scopedDss)) {}

DatabaseShardingRuntime::ScopedExclusiveDatabaseShardingRuntime::
    ScopedExclusiveDatabaseShardingRuntime(ScopedDatabaseShardingState&& scopedDss)
    : _scopedDss(std::move(scopedDss)) {}

void DatabaseShardingRuntime::checkDbVersionOrThrow(OperationContext* opCtx) const {
    const auto optReceivedDatabaseVersion = getOperationReceivedVersion(opCtx, _dbName);
    if (optReceivedDatabaseVersion) {
        checkDbVersionOrThrow(opCtx, *optReceivedDatabaseVersion);
    }
}

void DatabaseShardingRuntime::checkCriticalSectionOrThrow(
    OperationContext* opCtx, const DatabaseVersion& receivedVersion) const {
    const auto critSecSignal =
        getCriticalSectionSignal(shard_role_details::getLocker(opCtx)->isWriteLocked()
                                     ? ShardingMigrationCriticalSection::kWrite
                                     : ShardingMigrationCriticalSection::kRead);

    uassert(StaleDbRoutingVersion(_dbName, receivedVersion, boost::none, critSecSignal),
            str::stream() << "The critical section for the database "
                          << _dbName.toStringForErrorMsg()
                          << " is acquired with reason: " << getCriticalSectionReason(),
            !critSecSignal);
}

void DatabaseShardingRuntime::checkDbVersionOrThrow(OperationContext* opCtx,
                                                    const DatabaseVersion& receivedVersion) const {
    checkCriticalSectionOrThrow(opCtx, receivedVersion);

    uassert(StaleDbRoutingVersion(_dbName, receivedVersion, boost::none),
            str::stream() << "No cached info for the database " << _dbName.toStringForErrorMsg(),
            _dbMetadataAccessor.getDbVersion(opCtx));

    const auto wantedVersion = *(_dbMetadataAccessor.getDbVersion(opCtx));

    uassert(StaleDbRoutingVersion(_dbName, receivedVersion, wantedVersion),
            str::stream() << "Version mismatch for the database " << _dbName.toStringForErrorMsg(),
            receivedVersion == wantedVersion);

    // Check placement conflicts for multi-document transactions.
    const auto atClusterTime = repl::ReadConcernArgs::get(opCtx).getArgsAtClusterTime();
    checkPlacementConflictTimestamp(opCtx, atClusterTime, receivedVersion, _dbName, wantedVersion);
}

void DatabaseShardingRuntime::assertIsPrimaryShardForDb(OperationContext* opCtx) const {
    if (_dbName == DatabaseName::kConfig || _dbName == DatabaseName::kAdmin) {
        uassert(7393700,
                fmt::format("The config server is the primary shard for database: {}",
                            _dbName.toStringForErrorMsg()),
                serverGlobalParams.clusterRole.has(ClusterRole::ConfigServer));
        return;
    }

    auto expectedDbVersion = OperationShardingState::get(opCtx).getDbVersion(_dbName);

    uassert(ErrorCodes::IllegalOperation,
            fmt::format("Received request without the version for the database {}",
                        _dbName.toStringForErrorMsg()),
            expectedDbVersion);

    checkDbVersionOrThrow(opCtx, *expectedDbVersion);

    const auto thisShardId = ShardingState::get(opCtx)->shardId();

    tassert(10371100,
            fmt::format("Did not find a primary shard in the database metadata after validating "
                        "the database version: expected: {} for database: {} and dbVersion: {}.",
                        thisShardId.toString(),
                        _dbName.toStringForErrorMsg(),
                        _dbMetadataAccessor.getDbVersion(opCtx)->toString()),
            _dbMetadataAccessor.getDbPrimaryShard(opCtx));

    const auto primaryShardId = *(_dbMetadataAccessor.getDbPrimaryShard(opCtx));

    uassert(
        ErrorCodes::IllegalOperation,
        fmt::format("This is not the primary shard for the database {}. Expected: {} Actual: {}",
                    _dbName.toStringForErrorMsg(),
                    primaryShardId.toString(),
                    thisShardId.toString()),
        primaryShardId == thisShardId);
}

void DatabaseShardingRuntime::setDbMetadata(OperationContext* opCtx,
                                            const DatabaseType& dbMetadata) {
    // During the recovery phase, when the ShardingRecoveryService is reading from disk and
    // populating the DatabaseShardingState, the ShardingState is not yet initialized. Therefore,
    // the following sanity check cannot be performed, as it requires knowing which ShardId this
    // shard belongs to.
    if (ShardingState::get(opCtx)->enabled()) {
        const auto thisShardId = ShardingState::get(opCtx)->shardId();
        tassert(
            10003604,
            fmt::format(
                "Expected to be setting this node's cached database metadata with its "
                "corresponding database version. Found primary shard in the database metadata: {}, "
                "expected: {} for database: {} and dbVersion: {}.",
                dbMetadata.getPrimary().toString(),
                thisShardId.toString(),
                dbMetadata.getDbName().toStringForErrorMsg(),
                dbMetadata.getVersion().toString()),
            dbMetadata.getPrimary() == thisShardId);
    }

    _dbMetadataAccessor.setDbMetadata(opCtx, dbMetadata.getPrimary(), dbMetadata.getVersion());
}

void DatabaseShardingRuntime::clearDbMetadata(OperationContext* opCtx) {
    _dbMetadataAccessor.clearDbMetadata(opCtx);
}

void DatabaseShardingRuntime::enterCriticalSectionCatchUpPhase(const BSONObj& reason) {
    _criticalSection.enterCriticalSectionCatchUpPhase(reason);

    _cancelDbMetadataRefresh_DEPRECATED();
}

void DatabaseShardingRuntime::enterCriticalSectionCommitPhase(const BSONObj& reason) {
    _criticalSection.enterCriticalSectionCommitPhase(reason);

    _dbMetadataAccessor.setAccessType(DatabaseShardingMetadataAccessor::AccessType::kWriteAccess);
}

void DatabaseShardingRuntime::exitCriticalSection(const BSONObj& reason) {
    _criticalSection.exitCriticalSection(reason);

    _dbMetadataAccessor.setAccessType(DatabaseShardingMetadataAccessor::AccessType::kReadAccess);
}

void DatabaseShardingRuntime::exitCriticalSectionNoChecks() {
    _criticalSection.exitCriticalSectionNoChecks();

    _dbMetadataAccessor.setAccessType(DatabaseShardingMetadataAccessor::AccessType::kReadAccess);
}

void DatabaseShardingRuntime::setMovePrimaryInProgress(OperationContext* opCtx) {
    invariant(shard_role_details::getLocker(opCtx)->isDbLockedForMode(_dbName, MODE_X));

    _dbMetadataAccessor.setMovePrimaryInProgress();
}

void DatabaseShardingRuntime::unsetMovePrimaryInProgress(OperationContext* opCtx) {
    invariant(shard_role_details::getLocker(opCtx)->isDbLockedForMode(_dbName, MODE_IX));

    _dbMetadataAccessor.unsetMovePrimaryInProgress();
}

// DEPRECATED methods

void DatabaseShardingRuntime::setDbInfo_DEPRECATED(OperationContext* opCtx,
                                                   const DatabaseType& dbInfo) {
    invariant(shard_role_details::getLocker(opCtx)->isDbLockedForMode(_dbName, MODE_IX));

    _dbMetadataAccessor.setDbMetadata_UNSAFE(dbInfo.getPrimary(), dbInfo.getVersion());
}

void DatabaseShardingRuntime::clearDbInfo_DEPRECATED(OperationContext* opCtx,
                                                     bool cancelOngoingRefresh) {
    invariant(shard_role_details::getLocker(opCtx)->isDbLockedForMode(_dbName, MODE_IX));

    if (cancelOngoingRefresh) {
        _cancelDbMetadataRefresh_DEPRECATED();
    }

    _dbMetadataAccessor.clearDbMetadata(opCtx);
}

void DatabaseShardingRuntime::setDbMetadataRefreshFuture_DEPRECATED(
    SharedSemiFuture<void> future, CancellationSource cancellationSource) {
    invariant(!_dbMetadataRefresh);
    _dbMetadataRefresh.emplace(std::move(future), std::move(cancellationSource));
}

boost::optional<SharedSemiFuture<void>> DatabaseShardingRuntime::getMetadataRefreshFuture() const {
    return _dbMetadataRefresh ? boost::optional<SharedSemiFuture<void>>(_dbMetadataRefresh->future)
                              : boost::none;
}

void DatabaseShardingRuntime::resetDbMetadataRefreshFuture_DEPRECATED() {
    _dbMetadataRefresh = boost::none;
}

void DatabaseShardingRuntime::_cancelDbMetadataRefresh_DEPRECATED() {
    if (_dbMetadataRefresh) {
        _dbMetadataRefresh->cancellationSource.cancel();
    }
}

}  // namespace mongo
