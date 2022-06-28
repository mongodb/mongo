/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#include "mongo/db/logical_session_id.h"
#include "mongo/db/s/migration_coordinator_document_gen.h"
#include "mongo/s/catalog/type_chunk.h"

namespace mongo {

namespace migrationutil {
/**
 * Manages the migration commit/abort process, including updates to config.rangeDeletions on the
 * donor and the recipient, and updates to the routing table on the config server.
 */
class MigrationCoordinator {
public:
    MigrationCoordinator(MigrationSessionId sessionId,
                         ShardId donorShard,
                         ShardId recipientShard,
                         NamespaceString collectionNamespace,
                         UUID collectionUuid,
                         ChunkRange range,
                         ChunkVersion preMigrationChunkVersion,
                         bool waitForDelete);
    MigrationCoordinator(const MigrationCoordinatorDocument& doc);
    MigrationCoordinator(const MigrationCoordinator&) = delete;
    MigrationCoordinator& operator=(const MigrationCoordinator&) = delete;
    MigrationCoordinator(MigrationCoordinator&&) = delete;
    MigrationCoordinator& operator=(MigrationCoordinator&&) = delete;

    ~MigrationCoordinator();

    const UUID& getMigrationId() const;
    const LogicalSessionId& getLsid() const;
    TxnNumber getTxnNumber() const;

    /**
     * Initializes persistent state required to ensure that orphaned ranges are properly handled,
     * even after failover, by doing the following:
     *
     * 1) Inserts a document into the local config.migrationCoordinators with the lsid and
     * recipientId and waits for majority writeConcern. 2) Inserts a document into the local
     * config.rangeDeletions with the collectionUUID, range to delete, and "pending: true" and waits
     * for majority writeConcern.
     */
    void startMigration(OperationContext* opCtx);

    /**
     * Saves the decision.
     *
     * This method is non-blocking and does not perform any I/O.
     */
    void setMigrationDecision(DecisionEnum decision);

    /**
     * If a decision has been set, makes the decision durable, then communicates the decision by
     * updating the local (donor's) and remote (recipient's) config.rangeDeletions entries.
     *
     * If the decision was to commit, returns a future that is set when range deletion for
     * the donated range completes.
     */
    boost::optional<SemiFuture<void>> completeMigration(OperationContext* opCtx,
                                                        bool acquireCSOnRecipient);

    /**
     * Deletes the persistent state for this migration from config.migrationCoordinators.
     */
    void forgetMigration(OperationContext* opCtx);

    /**
     * Asynchronously releases the recipient critical section without waiting for it to finish. Sets
     * the _releaseRecipientCriticalSectionFuture future that will be readied once the recipient
     * critical section has been released.
     */
    void launchReleaseRecipientCriticalSection(OperationContext* opCtx);

private:
    /**
     * Deletes the range deletion task from the recipient node and marks the range deletion task on
     * the donor as ready to be processed. Returns a future that is set when range deletion for
     * the donated range completes.
     */
    SemiFuture<void> _commitMigrationOnDonorAndRecipient(OperationContext* opCtx,
                                                         bool acquireCSOnRecipient);

    /**
     * Deletes the range deletion task from the donor node and marks the range deletion task on the
     * recipient node as ready to be processed.
     */
    void _abortMigrationOnDonorAndRecipient(OperationContext* opCtx, bool acquireCSOnRecipient);

    /**
     * Waits for the completion of _releaseRecipientCriticalSectionFuture
     */
    void waitForReleaseRecipientCriticalSectionFuture(OperationContext* opCtx);

    MigrationCoordinatorDocument _migrationInfo;
    bool _waitForDelete = false;
    boost::optional<ExecutorFuture<void>> _releaseRecipientCriticalSectionFuture;
};

}  // namespace migrationutil
}  // namespace mongo
