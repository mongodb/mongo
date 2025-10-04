/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/database_name.h"
#include "mongo/db/global_catalog/ddl/move_primary_coordinator_document_gen.h"
#include "mongo/db/global_catalog/ddl/sharding_ddl_coordinator.h"
#include "mongo/db/global_catalog/ddl/sharding_ddl_coordinator_service.h"
#include "mongo/db/global_catalog/type_database_gen.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/write_ops/write_ops.h"
#include "mongo/db/sharding_environment/client/shard.h"
#include "mongo/db/versioning_protocol/database_version.h"
#include "mongo/executor/scoped_task_executor.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/cancellation.h"
#include "mongo/util/future.h"

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

class MovePrimaryCoordinator final
    : public RecoverableShardingDDLCoordinator<MovePrimaryCoordinatorDocument,
                                               MovePrimaryCoordinatorPhaseEnum> {
public:
    using StateDoc = MovePrimaryCoordinatorDocument;
    using Phase = MovePrimaryCoordinatorPhaseEnum;

    MovePrimaryCoordinator(ShardingDDLCoordinatorService* service, const BSONObj& initialState);
    ~MovePrimaryCoordinator() override = default;

    void checkIfOptionsConflict(const BSONObj& doc) const override;
    bool canAlwaysStartWhenUserWritesAreDisabled() const override;

protected:
    logv2::DynamicAttributes getCoordinatorLogAttrs() const override;

private:
    StringData serializePhase(const Phase& phase) const override;
    void appendCommandInfo(BSONObjBuilder* cmdInfoBuilder) const override;
    bool _mustAlwaysMakeProgress() override;
    ExecutorFuture<void> _runImpl(std::shared_ptr<executor::ScopedTaskExecutor> executor,
                                  const CancellationToken& token) noexcept override;
    ExecutorFuture<void> _cleanupOnAbort(std::shared_ptr<executor::ScopedTaskExecutor> executor,
                                         const CancellationToken& token,
                                         const Status& status) noexcept override;

    ExecutorFuture<void> runMovePrimaryWorkflow(
        std::shared_ptr<executor::ScopedTaskExecutor> executor,
        const CancellationToken& token) noexcept;

    /**
     * Clone data to the recipient shard.
     */
    void cloneData(OperationContext* opCtx);

    /**
     * Requests to the recipient to clone all the collections of the given database currently owned
     * by this shard. Once the cloning is complete, the recipient returns the list of the actually
     * cloned collections.
     */
    std::vector<NamespaceString> cloneDataToRecipient(OperationContext* opCtx);

    /**
     * Logs in the `config.changelog` collection a specific event for `movePrimary` operations.
     */
    void logChange(OperationContext* opCtx,
                   const std::string& what,
                   const Status& status = Status::OK()) const;

    /**
     * Returns the list of unsharded collections for the given database. These are the collections
     * the recipient is expected to clone.
     */
    std::vector<NamespaceString> getCollectionsToClone(OperationContext* opCtx) const;

    /**
     * Ensures that there are no orphaned collections in the recipient's catalog data, asserting
     * otherwise.
     */
    void assertNoOrphanedDataOnRecipient(
        OperationContext* opCtx, const std::vector<NamespaceString>& collectionsToClone) const;

    /**
     * Ensures that the list of actually cloned collections (returned by the cloning command)
     * matches the list of collections to clone (persisted in the coordinator document).
     */
    void assertClonedData(const std::vector<NamespaceString>& clonedCollections) const;

    /**
     * Commits the new primary shard for the given database to the config server. The database
     * version is passed to the config server's command as an idempotency key.
     */
    void commitMetadataToConfig(OperationContext* opCtx,
                                const DatabaseVersion& preCommitDbVersion) const;

    /**
     * Retrieves the metadata for the database after the commit to the config server.
     */
    DatabaseType getPostCommitDatabaseMetadata(OperationContext* opCtx) const;

    /**
     * Ensures that the metadata changes have been actually commited on the config server, asserting
     * otherwise. This is a pedantic check to rule out any potentially disastrous problems.
     */
    void assertChangedMetadataOnConfig(OperationContext* opCtx,
                                       const DatabaseType& postCommitDbType,
                                       const DatabaseVersion& preCommitDbVersion) const;

    /**
     * Commits the database metadata to the new primary shard and removes it from the old primary
     * shard.
     */
    void commitMetadataToShards(OperationContext* opCtx,
                                const DatabaseVersion& preCommitDbVersion,
                                const std::shared_ptr<executor::ScopedTaskExecutor>& executor,
                                const CancellationToken& token);

    /**
     * Clears the database metadata in the local catalog cache. Secondary nodes clear the database
     * metadata as a result of exiting the critical section of the primary node
     * (`kExitCriticalSection` phase).
     */
    void clearDbMetadataOnPrimary(OperationContext* opCtx) const;

    /**
     * Drops stale collections on the donor.
     */
    void dropStaleDataOnDonor(OperationContext* opCtx) const;

    /**
     * Drops possible orphaned collections on the recipient.
     */
    void dropOrphanedDataOnRecipient(OperationContext* opCtx,
                                     std::shared_ptr<executor::ScopedTaskExecutor> executor);

    /**
     * Fetches database metadata from the global catalog and installs it in the shard catalog. This
     * operation is necessary when the FCV is transitioning to 9.0 to prevent potential races with
     * _shardsvrCloneAuthoritativeMetadata during the upgrade phase.
     *
     * TODO (SERVER-98118): Remove this method once v9.0 become last-lts.
     */
    void cloneAuthoritativeDatabaseMetadata(OperationContext* opCtx) const;

    /**
     * Blocks write operations on the database, causing them to fail with the
     * `MovePrimaryInProgress` error.
     */
    void blockWritesLegacy(OperationContext* opCtx) const;

    /**
     * Unblocks write operations on the database.
     */
    void unblockWritesLegacy(OperationContext* opCtx) const;

    /**
     * Blocks write operations on the database, causing them to wait until the critical section has
     * entered.
     */
    void blockWrites(OperationContext* opCtx) const;

    /**
     * Blocks read operations on the database, causing them to wait until the critical section has
     * entered.
     */
    void blockReads(OperationContext* opCtx) const;

    /**
     * Unblocks read and write operations on the database.
     */
    void unblockReadsAndWrites(OperationContext* opCtx) const;

    /**
     * Requests the recipient to enter the critical section on the database, causing the database
     * metadata refreshes to block.
     */
    void enterCriticalSectionOnRecipient(
        OperationContext* opCtx,
        const std::shared_ptr<executor::ScopedTaskExecutor>& executor,
        const CancellationToken& token);

    /**
     * Requests the recipient to exit the critical section on the database, causing the database
     * metadata refreshes to unblock.
     */
    void exitCriticalSectionOnRecipient(
        OperationContext* opCtx,
        const std::shared_ptr<executor::ScopedTaskExecutor>& executor,
        const CancellationToken& token);

    const DatabaseName _dbName;
    const BSONObj _csReason;
};

}  // namespace mongo
