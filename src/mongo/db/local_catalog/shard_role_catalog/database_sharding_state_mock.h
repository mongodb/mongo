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

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/db/database_name.h"
#include "mongo/db/global_catalog/type_database_gen.h"
#include "mongo/db/local_catalog/shard_role_catalog/database_sharding_runtime.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/versioning_protocol/database_version.h"
#include "mongo/db/versioning_protocol/stale_exception.h"

#include <boost/optional/optional.hpp>

namespace mongo {

/**
 * Mock implementation of DatabaseShardingRuntime used for testing database sharding state logic in
 * isolation.
 *
 * Provides mechanisms to simulate database version mismatches and critical section conditions
 * without requiring a real sharding state environment. Should only be used in unit tests.
 */
class DatabaseShardingStateMock : public DatabaseShardingRuntime {
    DatabaseShardingStateMock(const DatabaseShardingStateMock&) = delete;
    DatabaseShardingStateMock& operator=(const DatabaseShardingStateMock&) = delete;

public:
    DatabaseShardingStateMock(const DatabaseName& dbName);
    ~DatabaseShardingStateMock() override = default;

    class ScopedDatabaseShardingStateMock {
    public:
        ScopedDatabaseShardingStateMock(ScopedDatabaseShardingStateMock&&) = default;

        DatabaseShardingStateMock* operator->() const {
            return checked_cast<DatabaseShardingStateMock*>(&*_scopedDss);
        }
        DatabaseShardingStateMock& operator*() const {
            return checked_cast<DatabaseShardingStateMock&>(*_scopedDss);
        }

    private:
        friend class DatabaseShardingStateMock;

        ScopedDatabaseShardingStateMock(ScopedDatabaseShardingState&& scopedDss);

        ScopedDatabaseShardingState _scopedDss;
    };

    static ScopedDatabaseShardingStateMock acquire(OperationContext* opCtx,
                                                   const DatabaseName& dbName);

    /**
     * Simulates a check against a received database version. The mock can be configured to fail
     * under certain simulated error conditions.
     */
    void checkDbVersionOrThrow(OperationContext* opCtx) const override;
    void checkDbVersionOrThrow(OperationContext* opCtx,
                               const DatabaseVersion& receivedVersion) const override;

    /**
     * Overrides the default check with no-op behavior. This mock method intentionally does not
     * validate the primary shard.
     */
    void assertIsPrimaryShardForDb(OperationContext* opCtx) const override {}

    /**
     * Sets this node's cached database metadata without critical section checks.
     */
    void setDbMetadata(OperationContext* opCtx, const DatabaseType& dbMetadata);

    /**
     * Resets this node's cached database metadata without critical section checks.
     */
    void clearDbMetadata();

    /**
     * Configures the mock to simulate a failure in db version check due to unknown metadata.
     */
    void expectFailureDbVersionCheckWithUnknownMetadata(const DatabaseVersion& receivedVersion);

    /**
     * Configures the mock to simulate a db version mismatch failure during version check.
     */
    void expectFailureDbVersionCheckWithMismatchingVersion(const DatabaseVersion& wantedVersion,
                                                           const DatabaseVersion& receivedVersion);

    /**
     * Configures the mock to simulate a failure caused by an active critical section during version
     * check.
     */
    void expectFailureDbVersionCheckWithCriticalSection(const DatabaseVersion& receivedVersion,
                                                        const BSONObj& csReason);

    /**
     * Clears any previously configured failure simulation for version checks.
     */
    void clearExpectedFailureDbVersionCheck();

private:
    /**
     * Holds simulated stale metadata for db version check failures in tests. When set, it indicates
     * what kind of db version exception should be triggered.
     */
    boost::optional<StaleDbRoutingVersion> _staleInfo;
};

}  // namespace mongo
