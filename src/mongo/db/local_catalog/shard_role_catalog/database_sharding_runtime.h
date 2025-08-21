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
#include "mongo/db/global_catalog/ddl/sharding_migration_critical_section.h"
#include "mongo/db/global_catalog/type_database_gen.h"
#include "mongo/db/local_catalog/shard_role_catalog/database_sharding_metadata_accessor.h"
#include "mongo/db/local_catalog/shard_role_catalog/database_sharding_state.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/versioning_protocol/database_version.h"
#include "mongo/util/cancellation.h"
#include "mongo/util/future.h"

#include <utility>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

/**
 * See the comments for DatabaseShardingState for more information on how this class fits in the
 * sharding architecture.
 */
class DatabaseShardingRuntime : public DatabaseShardingState {
    DatabaseShardingRuntime(const DatabaseShardingRuntime&) = delete;
    DatabaseShardingRuntime& operator=(const DatabaseShardingRuntime&) = delete;

public:
    DatabaseShardingRuntime(const DatabaseName& dbName);
    ~DatabaseShardingRuntime() override;

    /**
     * Obtains the sharding runtime for the specified database, along with a resource lock in shared
     * mode protecting it from concurrent modifications, which will be held until the object goes
     * out of scope.
     */
    class ScopedSharedDatabaseShardingRuntime {
    public:
        ScopedSharedDatabaseShardingRuntime(ScopedSharedDatabaseShardingRuntime&&) = default;

        const DatabaseShardingRuntime* operator->() const {
            return checked_cast<DatabaseShardingRuntime*>(&*_scopedDss);
        }
        const DatabaseShardingRuntime& operator*() const {
            return checked_cast<DatabaseShardingRuntime&>(*_scopedDss);
        }

    private:
        friend class DatabaseShardingRuntime;

        ScopedSharedDatabaseShardingRuntime(ScopedDatabaseShardingState&& scopedDss);

        ScopedDatabaseShardingState _scopedDss;
    };

    /**
     * Obtains the sharding runtime for the specified database, along with a resource lock in
     * exclusive mode protecting it from concurrent modifications, which will be held until the
     * object goes out of scope.
     */
    class ScopedExclusiveDatabaseShardingRuntime {
    public:
        ScopedExclusiveDatabaseShardingRuntime(ScopedExclusiveDatabaseShardingRuntime&&) = default;

        DatabaseShardingRuntime* operator->() const {
            return checked_cast<DatabaseShardingRuntime*>(&*_scopedDss);
        }
        DatabaseShardingRuntime& operator*() const {
            return checked_cast<DatabaseShardingRuntime&>(*_scopedDss);
        }

    private:
        friend class DatabaseShardingRuntime;

        ScopedExclusiveDatabaseShardingRuntime(ScopedDatabaseShardingState&& scopedDss);

        ScopedDatabaseShardingState _scopedDss;
    };

    static ScopedExclusiveDatabaseShardingRuntime acquireExclusive(OperationContext* opCtx,
                                                                   const DatabaseName& dbName);

    static ScopedSharedDatabaseShardingRuntime acquireShared(OperationContext* opCtx,
                                                             const DatabaseName& dbName);

    static ScopedExclusiveDatabaseShardingRuntime assertDbLockedAndAcquireExclusive(
        OperationContext* opCtx, const DatabaseName& dbName);

    static ScopedSharedDatabaseShardingRuntime assertDbLockedAndAcquireShared(
        OperationContext* opCtx, const DatabaseName& dbName);

    void checkDbVersionOrThrow(OperationContext* opCtx) const override;

    void checkDbVersionOrThrow(OperationContext* opCtx,
                               const DatabaseVersion& receivedVersion) const override;

    void assertIsPrimaryShardForDb(OperationContext* opCtx) const override;

    bool isMovePrimaryInProgress() const override {
        return _dbMetadataAccessor.isMovePrimaryInProgress();
    }

    boost::optional<DatabaseVersion> getDbVersion() const {
        return _dbMetadataAccessor.getDbVersion();
    }

    boost::optional<ShardId> getDbPrimaryShard() const {
        return _dbMetadataAccessor.getDbPrimaryShard();
    }

    /**
     * Sets this node's cached database info.
     */
    void setDbMetadata(OperationContext* opCtx, const DatabaseType& dbMetadata);

    /**
     * Resets this node's cached database info.
     */
    void clearDbMetadata();

    /**
     * Methods to control the databases's critical section. Must be called with the database X lock
     * held.
     */
    void enterCriticalSectionCatchUpPhase(const BSONObj& reason);
    void enterCriticalSectionCommitPhase(const BSONObj& reason);
    void exitCriticalSection(const BSONObj& reason);
    void exitCriticalSectionNoChecks();

    auto getCriticalSectionSignal(ShardingMigrationCriticalSection::Operation op) const {
        return _criticalSection.getSignal(op);
    }

    auto getCriticalSectionReason() const {
        return _criticalSection.getReason();
    }

    /**
     * Declares that a `movePrimary` operation on this database is in progress. This causes
     * write operations on this database to fail with the `MovePrimaryInProgress` error.
     *
     * Must be called with the database locked in X mode.
     */
    void setMovePrimaryInProgress(OperationContext* opCtx);

    /**
     * Declares that the `movePrimary` operation on this database is over. This re-enables write
     * operations on this database.
     *
     * Must be called with the database locked in IX mode.
     */
    void unsetMovePrimaryInProgress(OperationContext* opCtx);


    // DEPRECATED methods

    /**
     * Sets this node's cached database info in a non-authoritative way.
     *
     * The caller must hold the database lock in MODE_IX.
     */
    void setDbInfo_DEPRECATED(OperationContext* opCtx, const DatabaseType& dbInfo);

    /**
     * Resets this node's cached database info in a non-authoritative way.
     *
     * NOTE: Only the thread that refreshes the database metadata (which calls the function
     * `onDbVersionMismatch`) actually needs to change the default initialization of
     * `cancelOngoingRefresh`. This parameter must be ignored in any other case.
     *
     * The caller must hold the database lock in MODE_IX.
     *
     * NOTE: This method is deprecated and should not be used. In the authoritative model,
     * database refreshes are not required, and there is no need to lock the database. The
     * method is retained for backward compatibility, but its usage is discouraged in favor of
     * the updated approach.
     */
    void clearDbInfo_DEPRECATED(OperationContext* opCtx, bool cancelOngoingRefresh = true);

    /**
     * Sets the database metadata refresh future for other threads to wait on it.
     */
    void setDbMetadataRefreshFuture_DEPRECATED(SharedSemiFuture<void> future,
                                               CancellationSource cancellationSource);

    /**
     * If there is an ongoing database metadata refresh, returns the future to wait on it,
     * otherwise `boost::none`.
     */
    boost::optional<SharedSemiFuture<void>> getMetadataRefreshFuture() const;

    /**
     * Resets the database metadata refresh future to `boost::none`.
     */
    void resetDbMetadataRefreshFuture_DEPRECATED();

protected:
    const DatabaseName _dbName;

    // Tracks the database metadata and its lifecycle for this database.
    DatabaseShardingMetadataAccessor _dbMetadataAccessor;

    // Tracks the critical section state for this database.
    ShardingMigrationCriticalSection _criticalSection;

private:
    // DEPRECATED methods and attributes

    struct DbMetadataRefresh {
        DbMetadataRefresh(SharedSemiFuture<void> future, CancellationSource cancellationSource)
            : future(std::move(future)), cancellationSource(std::move(cancellationSource)) {};

        // Tracks the ongoing database metadata refresh.
        SharedSemiFuture<void> future;

        // Cancellation source to cancel the ongoing database metadata refresh.
        CancellationSource cancellationSource;
    };

    /**
     * Cancel any ongoing database metadata refresh.
     */
    void _cancelDbMetadataRefresh_DEPRECATED();

    // Tracks the ongoing database metadata refresh. Possibly keeps a future for other threads
    // to wait on it, and a cancellation source to cancel the ongoing database metadata refresh.
    boost::optional<DbMetadataRefresh> _dbMetadataRefresh;
};

}  // namespace mongo
