// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/db/global_catalog/type_chunk.h"
#include "mongo/db/keypattern.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/persistent_task_store.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/s/balancer_stats_registry.h"
#include "mongo/db/s/migration_coordinator_document_gen.h"
#include "mongo/db/s/migration_recipient_recovery_document_gen.h"
#include "mongo/db/s/migration_session_id.h"
#include "mongo/db/session/logical_session_id.h"
#include "mongo/db/session/logical_session_id_gen.h"
#include "mongo/db/shard_role/shard_catalog/collection_metadata.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/executor/thread_pool_task_executor.h"
#include "mongo/s/request_types/move_range_request_gen.h"
#include "mongo/util/future.h"
#include "mongo/util/modules.h"
#include "mongo/util/uuid.h"

#include <cstddef>
#include <memory>

#include <boost/optional/optional.hpp>

namespace mongo {

class BSONObj;
class NamespaceString;
class ShardId;

namespace migrationutil {

/**
 * Returns the BSON reason document used to acquire a migration critical section for a moveRange
 * (chunk migration) request. The migrationId makes the reason unique per migration attempt.
 */
BSONObj makeCriticalSectionReasonForMoveRange(const ShardsvrMoveRangeRequest& request,
                                              const UUID& migrationId);

/**
 * Creates a report document with the provided parameters:
 *
 * {
 *     source:                          "shard0000"
 *     destination:                     "shard0001"
 *     isDonorShard:                    true or false
 *     chunk:                           {"min": <MinKey>, "max": <MaxKey>}
 *     collection:                      "dbName.collName"
 *     sessionOplogEntriesToBeMigratedSoFar: <Number>
 *     sessionOplogEntriesSkipped:      <Number>
 * }
 *
 */
BSONObj makeMigrationStatusDocumentSource(
    const NamespaceString& nss,
    const ShardId& fromShard,
    const ShardId& toShard,
    const bool& isDonorShard,
    const BSONObj& min,
    const BSONObj& max,
    boost::optional<long long> sessionOplogEntriesToBeMigratedSoFar,
    boost::optional<long long> sessionOplogEntriesSkippedSoFarLowerBound);

/**
 * Creates a report document with the provided parameters:
 *
 * {
 *     source:                      "shard0000"
 *     destination:                 "shard0001"
 *     isDonorShard:                true or false
 *     chunk:                       {"min": <MinKey>, "max": <MaxKey>}
 *     collection:                  "dbName.collName"
 *     sessionOplogEntriesMigrated: <Number>
 * }
 *
 */
BSONObj makeMigrationStatusDocumentDestination(
    const NamespaceString& nss,
    const ShardId& fromShard,
    const ShardId& toShard,
    const bool& isDonorShard,
    const BSONObj& min,
    const BSONObj& max,
    boost::optional<long long> sessionOplogEntriesMigrated);

/**
 * Writes the migration coordinator document to config.migrationCoordinators and waits for majority
 * write concern.
 */
void insertMigrationCoordinatorDoc(OperationContext* opCtx,
                                   const MigrationCoordinatorDocument& migrationDoc);
void updateMigrationCoordinatorDoc(OperationContext* opCtx,
                                   const MigrationCoordinatorDocument& migrationdoc);

/**
 * Updates the migration coordinator document to set the decision field to "committed" and waits
 * for majority writeConcern.
 */
void persistCommitDecision(OperationContext* opCtx,
                           const MigrationCoordinatorDocument& migrationDoc);

/**
 * Updates the migration coordinator document to set the decision field to "aborted" and waits for
 * majority writeConcern.
 */
void persistAbortDecision(OperationContext* opCtx,
                          const MigrationCoordinatorDocument& migrationDoc);


/**
 * Advances the optime for the current transaction by performing a write operation as a retryable
 * write. This is to prevent a write of the deletion task once the decision has been recorded.
 */
void advanceTransactionOnRecipient(OperationContext* opCtx,
                                   const ShardId& recipientId,
                                   const LogicalSessionId& lsid,
                                   TxnNumber txnNumber);

/**
 * Submits an asynchronous task to scan config.migrationCoordinators and drive each unfinished
 * migration coordination to completion.
 */
[[MONGO_MOD_PUBLIC]] void resumeMigrationCoordinationsOnStepUp(OperationContext* opCtx,
                                                               long long term);

/**
 * Instructs the recipient shard to release its critical section.
 *
 * TODO (completeMigration): Remove clearShardCatalogCache field when v9.0 branches out.
 */
ExecutorFuture<void> launchReleaseCriticalSectionOnRecipientFuture(
    OperationContext* opCtx,
    const ShardId& recipientShardId,
    const NamespaceString& nss,
    const MigrationSessionId& sessionId,
    bool clearShardCatalogCache);

/**
 * Emits the change-stream "chunk migrated" event for a committed migration. Reads the post-commit
 * placement from the catalog cache to decide whether the donor retains any chunk of the collection,
 * then notifies change streams. Used by both the same-term commit path and the recovery re-emit
 * path, so the migration-specific fields are passed explicitly.
 */
void notifyChangeStreamsOnChunkMigrationCommitted(OperationContext* opCtx,
                                                  const NamespaceString& nss,
                                                  const UUID& collectionUuid,
                                                  const ShardId& fromShard,
                                                  const ShardId& toShard,
                                                  bool transfersFirstChunkToRecipient);

/**
 * Writes the migration recipient recovery document to config.migrationRecipients and waits for
 * majority write concern.
 */
void persistMigrationRecipientRecoveryDocument(
    OperationContext* opCtx, const MigrationRecipientRecoveryDocument& migrationRecipientDoc);

/**
 * Deletes the migration recipient recovery document from config.migrationRecipients and waits for
 * majority write concern.
 */
void deleteMigrationRecipientRecoveryDocument(OperationContext* opCtx, const UUID& migrationId);

/**
 * If there was any ongoing receiveChunk that requires recovery (i.e that has reached the
 * critical section stage), restores the MigrationDestinationManager state.
 */
[[MONGO_MOD_PUBLIC]] void resumeMigrationRecipientsOnStepUp(OperationContext* opCtx);

/**
 * Recovers all unfinished migrations pending recovery.
 * Note: This method assumes its caller is preventing new migrations from starting.
 */
// TODO (SERVER-98118): remove [[MONGO_MOD_NEEDS_REPLACEMENT]] once 9.0 becomes last LTS.
[[MONGO_MOD_NEEDS_REPLACEMENT]] void drainMigrationsPendingRecovery(OperationContext* opCtx);

/**
 * Asserts no migration is in progress or pending recovery.
 */
// TODO (SERVER-98118): remove once 9.0 becomes last LTS.
[[MONGO_MOD_NEEDS_REPLACEMENT]] void assertNoMigrationsRemaining(OperationContext* opCtx);

/**
 * Refreshes the filtering metadata for the given namespace from the config server, retrying until
 * success or stepdown. Drives any pending migration coordinator recovery as a side effect.
 */
void refreshFilteringMetadataUntilSuccess(OperationContext* opCtx, const NamespaceString& nss);

/**
 * Registers two RangeDeleterService recovery jobs. Must be called during onStepUpBegin so both
 * jobs are registered before the RangeDeleterService scan can complete and unblock range
 * deletions.
 *
 * The first is for the legacy standalone migration recovery path
 * (resumeMigrationCoordinationsOnStepUp), which recovers all standalone coordinators as a single
 * batch and resolves the job once the batch finishes.
 *
 * The second is for the MoveRangeCoordinator recovered from disk. The coordinator resolves the
 * job when it completes, or ShardingCoordinatorService resolves it during rebuild when there is no
 * MoveRangeCoordinator to recover.
 */
[[MONGO_MOD_PUBLIC]] void registerMigrationRecoveryJobs(OperationContext* opCtx, long long term);

/**
 * Submits an asynchronous task to recover the migration until it succeeds or the node steps down.
 */
SemiFuture<void> asyncRecoverMigrationUntilSuccessOrStepDown(OperationContext* opCtx,
                                                             const NamespaceString& nss);

/**
 * Exhaust any active and recovery-pending migration. This method is meant to be exclusively called
 * within the context of a FCV downgrade.
 * TODO SERVER-103838 Remove this method and its invocations once 9.0 becomes LTS.
 */
[[MONGO_MOD_PUBLIC]] void drainMigrationsOnFcvDowngrade(OperationContext* opCtx);

}  // namespace migrationutil
}  // namespace mongo
