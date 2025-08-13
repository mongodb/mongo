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

#include "mongo/bson/bsonobj.h"
#include "mongo/db/global_catalog/type_chunk.h"
#include "mongo/db/keypattern.h"
#include "mongo/db/local_catalog/shard_role_catalog/collection_metadata.h"
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
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/executor/thread_pool_task_executor.h"
#include "mongo/util/future.h"
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
void resumeMigrationCoordinationsOnStepUp(OperationContext* opCtx);

/**
 * Instructs the recipient shard to release its critical section.
 */
ExecutorFuture<void> launchReleaseCriticalSectionOnRecipientFuture(
    OperationContext* opCtx,
    const ShardId& recipientShardId,
    const NamespaceString& nss,
    const MigrationSessionId& sessionId);

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
void resumeMigrationRecipientsOnStepUp(OperationContext* opCtx);

/**
 * Recovers all unfinished migrations pending recovery.
 * Note: This method assumes its caller is preventing new migrations from starting.
 */
void drainMigrationsPendingRecovery(OperationContext* opCtx);

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
void drainMigrationsOnFcvDowngrade(OperationContext* opCtx);

}  // namespace migrationutil
}  // namespace mongo
