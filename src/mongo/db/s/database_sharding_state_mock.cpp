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

#include "mongo/db/s/database_sharding_state_mock.h"

#include "mongo/base/error_codes.h"
#include "mongo/db/concurrency/lock_manager_defs.h"
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
    _critSec.enterCriticalSectionCatchUpPhase(csReason);
    _staleInfo =
        StaleDbRoutingVersion(_dbName,
                              receivedVersion,
                              boost::none,
                              getCriticalSectionSignal(ShardingMigrationCriticalSection::kWrite));
}

void DatabaseShardingStateMock::clearExpectedFailureDbVersionCheck() {
    _staleInfo = boost::none;
    _critSec.exitCriticalSectionNoChecks();
}

}  // namespace mongo
