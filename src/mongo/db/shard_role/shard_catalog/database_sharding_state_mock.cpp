// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/shard_role/shard_catalog/database_sharding_state_mock.h"

#include "mongo/base/error_codes.h"
#include "mongo/db/shard_role/lock_manager/lock_manager_defs.h"
#include "mongo/util/assert_util.h"

namespace mongo {

DatabaseShardingStateMock::DatabaseShardingStateMock(const DatabaseName& dbName)
    : DatabaseShardingRuntime(dbName) {}

DatabaseShardingStateMock::ScopedDatabaseShardingStateMock DatabaseShardingStateMock::acquire(
    OperationContext* opCtx, const DatabaseName& dbName) {
    return ScopedDatabaseShardingStateMock(
        ScopedDatabaseShardingState::acquireScopedDatabaseShardingState(opCtx, dbName, MODE_IS));
}

DatabaseShardingStateMock::ScopedDatabaseShardingStateMock::ScopedDatabaseShardingStateMock(
    ScopedDatabaseShardingState&& scopedDss)
    : _scopedDss(std::move(scopedDss)) {}

void DatabaseShardingStateMock::checkDbVersionOrThrow(OperationContext* opCtx) const {
    if (_staleInfo) {
        uasserted(StaleDbRoutingVersion(*_staleInfo), "Mock StaleDbRoutingVersion exception");
    }
}

void DatabaseShardingStateMock::checkDbVersionOrThrow(
    OperationContext* opCtx, const DatabaseVersion& receivedVersion) const {
    if (_staleInfo) {
        invariant(_staleInfo->getVersionReceived() == receivedVersion);
        uasserted(StaleDbRoutingVersion(*_staleInfo), "Mock StaleDbRoutingVersion exception");
    }
}

void DatabaseShardingStateMock::expectFailureDbVersionCheckWithUnknownMetadata(
    const DatabaseVersion& receivedVersion) {
    _staleInfo = StaleDbRoutingVersion(_dbName, receivedVersion, boost::none);
}

void DatabaseShardingStateMock::expectFailureDbVersionCheckWithMismatchingVersion(
    const DatabaseVersion& wantedVersion, const DatabaseVersion& receivedVersion) {
    _staleInfo = StaleDbRoutingVersion(_dbName, receivedVersion, wantedVersion);
}

void DatabaseShardingStateMock::expectFailureDbVersionCheckWithCriticalSection(
    const DatabaseVersion& receivedVersion, const BSONObj& csReason) {
    _criticalSection.enterCriticalSectionCatchUpPhase(csReason);
    _staleInfo =
        StaleDbRoutingVersion(_dbName,
                              receivedVersion,
                              boost::none,
                              getCriticalSectionSignal(ShardingMigrationCriticalSection::kWrite));
}

void DatabaseShardingStateMock::clearExpectedFailureDbVersionCheck() {
    _staleInfo = boost::none;
    _criticalSection.exitCriticalSectionNoChecks();
}

void DatabaseShardingStateMock::setDbMetadata(OperationContext* opCtx,
                                              const DatabaseType& dbMetadata) {
    _dbMetadataAccessor.setAccessType(opCtx,
                                      DatabaseShardingMetadataAccessor::AccessType::kWriteAccess);
    _dbMetadataAccessor.setDbMetadata(opCtx, dbMetadata.getPrimary(), dbMetadata.getVersion());
    _dbMetadataAccessor.setAccessType(opCtx,
                                      DatabaseShardingMetadataAccessor::AccessType::kReadAccess);
}

void DatabaseShardingStateMock::clearDbMetadata(OperationContext* opCtx) {
    _dbMetadataAccessor.setAccessType(opCtx,
                                      DatabaseShardingMetadataAccessor::AccessType::kWriteAccess);
    _dbMetadataAccessor.clearDbMetadata(opCtx);
    _dbMetadataAccessor.setAccessType(opCtx,
                                      DatabaseShardingMetadataAccessor::AccessType::kReadAccess);
}

}  // namespace mongo
