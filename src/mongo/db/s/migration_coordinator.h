// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/global_catalog/type_chunk.h"
#include "mongo/db/keypattern.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/s/migration_coordinator_document_gen.h"
#include "mongo/db/s/migration_session_id.h"
#include "mongo/db/s/range_deletion_task_gen.h"
#include "mongo/db/session/logical_session_id.h"
#include "mongo/db/session/logical_session_id_gen.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/db/versioning_protocol/chunk_version.h"
#include "mongo/util/future.h"
#include "mongo/util/modules.h"
#include "mongo/util/uuid.h"

#include <utility>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {
namespace migrationutil {

/**
 * Manages the migration commit/abort process, including updates to config.rangeDeletions on the
 * donor and the recipient, and updates to the routing table on the config server.
 */
class [[MONGO_MOD_NEEDS_REPLACEMENT]] MigrationCoordinator {
public:
    MigrationCoordinator(UUID migrationId,
                         MigrationSessionId sessionId,
                         ShardId donorShard,
                         ShardId recipientShard,
                         NamespaceString collectionNamespace,
                         UUID collectionUuid,
                         ChunkRange range,
                         ChunkVersion preMigrationChunkVersion,
                         const KeyPattern& shardKeyPattern,
                         ChunkVersion currentShardVersion,
                         bool waitForDelete,
                         ManagementModeEnum mode = ManagementModeEnum::kStandalone);

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
     * Sets the shard key pattern on the coordinator. Needs to be called by migration recovery to
     * allow the range deletion task to access the shard key pattern.
     */
    void setShardKeyPattern(const boost::optional<KeyPattern>& shardKeyPattern);

    /**
     * Persistently sets/gets whether this chunk migration will cause the recipent to start owning
     * data for the parent collection. The value is expected to be set by MigrationSourceManager at
     * the beginning of the clone phase and consumed upon commit to generate the related op entry
     * for change stream readers.
     */
    void setTransfersFirstCollectionChunkToRecipient(OperationContext* opCtx, bool value);
    bool getTransfersFirstCollectionChunkToRecipient();

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
     *
     * TODO (SERVER-127253) Remove clearShardCatalogCache when v9.0 branches out.
     */
    boost::optional<SharedSemiFuture<void>> completeMigration(OperationContext* opCtx,
                                                              bool clearShardCatalogCache);

    /**
     * Deletes the persistent state for this migration from config.migrationCoordinators.
     */
    void forgetMigration(OperationContext* opCtx);

    /**
     * Asynchronously releases the recipient critical section without waiting for it to finish. Sets
     * the _releaseRecipientCriticalSectionFuture future that will be readied once the recipient
     * critical section has been released.
     *
     * TODO (SERVER-127253) Remove clearShardCatalogCache when v9.0 branches out.
     */
    void launchReleaseRecipientCriticalSection(OperationContext* opCtx,
                                               bool clearShardCatalogCache);

private:
    /**
     * Deletes the range deletion task from the recipient node and marks the range deletion task on
     * the donor as ready to be processed. Returns a future that is set when range deletion for
     * the donated range completes.
     */
    SharedSemiFuture<void> _commitMigrationOnDonorAndRecipient(OperationContext* opCtx);

    /**
     * Deletes the range deletion task from the donor node and marks the range deletion task on the
     * recipient node as ready to be processed.
     */
    void _abortMigrationOnDonorAndRecipient(OperationContext* opCtx);

    /**
     * Waits for the completion of _releaseRecipientCriticalSectionFuture and ignores ShardNotFound
     * exceptions.
     */
    void _waitForReleaseRecipientCriticalSectionFutureIgnoreShardNotFound(OperationContext* opCtx);

    MigrationCoordinatorDocument _migrationInfo;
    boost::optional<KeyPattern> _shardKeyPattern;
    ChunkVersion _shardVersionPriorToTheMigration;
    bool _waitForDelete = false;
    boost::optional<ExecutorFuture<void>> _releaseRecipientCriticalSectionFuture;
    boost::optional<RangeDeletionTask> _donorRangeDeletionTask;
};

}  // namespace migrationutil
}  // namespace mongo
