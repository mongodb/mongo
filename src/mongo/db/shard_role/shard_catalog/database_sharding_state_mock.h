// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/db/database_name.h"
#include "mongo/db/global_catalog/type_database_gen.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/shard_role/shard_catalog/database_sharding_runtime.h"
#include "mongo/db/versioning_protocol/database_version.h"
#include "mongo/db/versioning_protocol/stale_exception.h"
#include "mongo/util/modules.h"

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
    void clearDbMetadata(OperationContext* opCtx);

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
