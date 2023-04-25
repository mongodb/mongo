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

#include "mongo/db/s/move_primary/move_primary_donor_service.h"
#include "mongo/db/s/move_primary_coordinator_document_gen.h"
#include "mongo/db/s/sharding_ddl_coordinator.h"
#include "mongo/s/client/shard.h"

namespace mongo {

class MovePrimaryCoordinator final
    : public RecoverableShardingDDLCoordinator<MovePrimaryCoordinatorDocument,
                                               MovePrimaryCoordinatorPhaseEnum> {
public:
    using StateDoc = MovePrimaryCoordinatorDocument;
    using Phase = MovePrimaryCoordinatorPhaseEnum;

    MovePrimaryCoordinator(ShardingDDLCoordinatorService* service, const BSONObj& initialState);
    virtual ~MovePrimaryCoordinator() = default;

    void checkIfOptionsConflict(const BSONObj& doc) const override;
    bool canAlwaysStartWhenUserWritesAreDisabled() const override;

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

    bool onlineClonerPossiblyNeverCreated() const;
    bool onlineClonerPossiblyCleanedUp() const;
    bool onlineClonerAllowedToBeMissing() const;
    void recoverOnlineCloner(OperationContext* opCtx);
    void createOnlineCloner(OperationContext* opCtx);

    /**
     * Clone data to the recipient without using the online cloning machinery.
     */
    void cloneDataLegacy(OperationContext* opCtx);

    /**
     * Clone data to the recipient using the online cloning machinery.
     */
    void cloneDataUntilReadyForCatchup(OperationContext* opCtx, const CancellationToken& token);

    void informOnlineClonerOfBlockingWrites(OperationContext* opCtx);
    void waitUntilOnlineClonerPrepared(const CancellationToken& token);

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
    std::vector<NamespaceString> getUnshardedCollections(OperationContext* opCtx) const;

    /**
     * Ensures that there are no orphaned collections in the recipient's catalog data, asserting
     * otherwise.
     */
    void assertNoOrphanedDataOnRecipient(
        OperationContext* opCtx, const std::vector<NamespaceString>& collectionsToClone) const;


    /**
     * Requests to the recipient to clone all the collections of the given database currently owned
     * by this shard. Once the cloning is complete, the recipient returns the list of the actually
     * cloned collections.
     */
    std::vector<NamespaceString> cloneDataToRecipient(OperationContext* opCtx) const;

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
     * Ensures that the metadata changes have been actually commited on the config server, asserting
     * otherwise. This is a pedantic check to rule out any potentially disastrous problems.
     */
    void assertChangedMetadataOnConfig(OperationContext* opCtx,
                                       const DatabaseVersion& preCommitDbVersion) const;

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
     * Blocks write operations on the database, causing them to fail with the
     * `MovePrimaryInProgress` error.
     *
     * TODO (SERVER-71566): This is a synchronization mechanism specifically designed for
     * `movePrimary` operations. It will likely be replaced by the critical section once the time
     * frame in which writes are blocked is reduced. Writes are already blocked in the `kCatchup`
     * phase.
     */
    void blockWritesLegacy(OperationContext* opCtx) const;

    /**
     * Unblocks write operations on the database.
     *
     * TODO (SERVER-71566): This is a synchronization mechanism specifically designed for
     * `movePrimary` operations. It will likely be replaced by the critical section once the time
     * frame in which writes are blocked is reduced. Reads and writes are already unblocked in the
    // `kExitCriticalSection` phase.
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
    void enterCriticalSectionOnRecipient(OperationContext* opCtx);

    /**
     * Requests the recipient to exit the critical section on the database, causing the database
     * metadata refreshes to unblock.
     */
    void exitCriticalSectionOnRecipient(OperationContext* opCtx);

    void cleanupOnlineCloner(OperationContext* opCtx, const CancellationToken& token);
    void cleanupOnAbortWithoutOnlineCloner(OperationContext* opCtx,
                                           std::shared_ptr<executor::ScopedTaskExecutor> executor);
    void cleanupOnAbortWithOnlineCloner(OperationContext* opCtx,
                                        const CancellationToken& token,
                                        const Status& status);

    const DatabaseName _dbName;
    const BSONObj _csReason;
    std::shared_ptr<MovePrimaryDonor> _onlineCloner;
};

}  // namespace mongo
