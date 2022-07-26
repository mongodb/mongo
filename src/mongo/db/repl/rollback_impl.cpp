/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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


#include "mongo/platform/basic.h"

#include "mongo/db/repl/rollback_impl.h"
#include "mongo/db/repl/rollback_impl_gen.h"

#include <fmt/format.h>

#include "mongo/bson/util/bson_extract.h"
#include "mongo/db/catalog/collection_catalog.h"
#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/catalog/import_collection_oplog_entry_gen.h"
#include "mongo/db/commands.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/concurrency/exception_util.h"
#include "mongo/db/concurrency/replication_state_transition_lock_guard.h"
#include "mongo/db/database_name.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/index_builds_coordinator.h"
#include "mongo/db/kill_sessions_local.h"
#include "mongo/db/logical_time_validator.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/apply_ops.h"
#include "mongo/db/repl/drop_pending_collection_reaper.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/replication_process.h"
#include "mongo/db/repl/roll_back_local_operations.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/repl/tenant_migration_access_blocker_util.h"
#include "mongo/db/repl/transaction_oplog_application.h"
#include "mongo/db/s/type_shard_identity.h"
#include "mongo/db/server_recovery.h"
#include "mongo/db/session_catalog_mongod.h"
#include "mongo/db/session_txn_record_gen.h"
#include "mongo/db/storage/historical_ident_tracker.h"
#include "mongo/db/storage/remove_saver.h"
#include "mongo/db/transaction_history_iterator.h"
#include "mongo/logv2/log.h"
#include "mongo/s/catalog/type_config_version.h"
#include "mongo/util/scopeguard.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kReplicationRollback


namespace mongo {
namespace repl {

using namespace fmt::literals;

MONGO_FAIL_POINT_DEFINE(rollbackHangAfterTransitionToRollback);
MONGO_FAIL_POINT_DEFINE(rollbackToTimestampHangCommonPointBeforeReplCommitPoint);
MONGO_FAIL_POINT_DEFINE(rollbackHangBeforeTransitioningToRollback);

namespace {

// Used to set RollbackImpl::_newCounts to force a collection scan to fix count.
constexpr long long kCollectionScanRequired = -1;

RollbackImpl::Listener kNoopListener;

// The name of the insert, update and delete commands as found in oplog command entries.
constexpr auto kInsertCmdName = "insert"_sd;
constexpr auto kUpdateCmdName = "update"_sd;
constexpr auto kDeleteCmdName = "delete"_sd;
constexpr auto kNumRecordsFieldName = "numRecords"_sd;
constexpr auto kToFieldName = "to"_sd;
constexpr auto kDropTargetFieldName = "dropTarget"_sd;

/**
 * Parses the o2 field of a drop or rename oplog entry for the count of the collection that was
 * dropped.
 */
boost::optional<long long> _parseDroppedCollectionCount(const OplogEntry& oplogEntry) {
    auto commandType = oplogEntry.getCommandType();
    auto desc = OplogEntry::CommandType::kDrop == commandType ? "drop"_sd : "rename"_sd;

    auto obj2 = oplogEntry.getObject2();
    if (!obj2) {
        LOGV2_WARNING(21634,
                      "Unable to get collection count from oplog entry without the o2 field",
                      "type"_attr = desc,
                      "oplogEntry"_attr = redact(oplogEntry.toBSONForLogging()));
        return boost::none;
    }

    long long count = 0;
    // TODO: Use IDL to parse o2 object. See txn_cmds.idl for example.
    auto status = bsonExtractIntegerField(*obj2, kNumRecordsFieldName, &count);
    if (!status.isOK()) {
        LOGV2_WARNING(21635,
                      "Failed to parse oplog entry for collection count",
                      "type"_attr = desc,
                      "error"_attr = status,
                      "oplogEntry"_attr = redact(oplogEntry.toBSONForLogging()));
        return boost::none;
    }

    if (count < 0) {
        LOGV2_WARNING(21636,
                      "Invalid collection count found in oplog entry",
                      "type"_attr = desc,
                      "count"_attr = count,
                      "oplogEntry"_attr = redact(oplogEntry.toBSONForLogging()));
        return boost::none;
    }

    LOGV2_DEBUG(21590,
                2,
                "Parsed collection count of oplog entry",
                "count"_attr = count,
                "type"_attr = desc,
                "oplogEntry"_attr = redact(oplogEntry.toBSONForLogging()));
    return count;
}

}  // namespace

constexpr const char* RollbackImpl::kRollbackRemoveSaverType;
constexpr const char* RollbackImpl::kRollbackRemoveSaverWhy;

bool RollbackImpl::shouldCreateDataFiles() {
    return gCreateRollbackDataFiles.load();
}

RollbackImpl::RollbackImpl(OplogInterface* localOplog,
                           OplogInterface* remoteOplog,
                           StorageInterface* storageInterface,
                           ReplicationProcess* replicationProcess,
                           ReplicationCoordinator* replicationCoordinator,
                           Listener* listener)
    : _listener(listener),
      _localOplog(localOplog),
      _remoteOplog(remoteOplog),
      _storageInterface(storageInterface),
      _replicationProcess(replicationProcess),
      _replicationCoordinator(replicationCoordinator) {

    invariant(localOplog);
    invariant(remoteOplog);
    invariant(storageInterface);
    invariant(replicationProcess);
    invariant(replicationCoordinator);
    invariant(listener);
}

RollbackImpl::RollbackImpl(OplogInterface* localOplog,
                           OplogInterface* remoteOplog,
                           StorageInterface* storageInterface,
                           ReplicationProcess* replicationProcess,
                           ReplicationCoordinator* replicationCoordinator)
    : RollbackImpl(localOplog,
                   remoteOplog,
                   storageInterface,
                   replicationProcess,
                   replicationCoordinator,
                   &kNoopListener) {}

RollbackImpl::~RollbackImpl() {
    shutdown();
}

Status RollbackImpl::runRollback(OperationContext* opCtx) {
    _rollbackStats.startTime = opCtx->getServiceContext()->getFastClockSource()->now();

    auto status = _transitionToRollback(opCtx);
    if (!status.isOK()) {
        return status;
    }
    _listener->onTransitionToRollback();

    if (MONGO_unlikely(rollbackHangAfterTransitionToRollback.shouldFail())) {
        LOGV2(21591,
              "rollbackHangAfterTransitionToRollback fail point enabled. Blocking until fail "
              "point is disabled (rollback_impl)");
        rollbackHangAfterTransitionToRollback.pauseWhileSet(opCtx);
    }

    // We clear the SizeRecoveryState before we recover to a stable timestamp. This ensures that we
    // only use size adjustment markings from the storage and replication recovery processes in this
    // rollback.
    sizeRecoveryState(opCtx->getServiceContext()).clearStateBeforeRecovery();

    // After successfully transitioning to the ROLLBACK state, we must always transition back to
    // SECONDARY, even if we fail at any point during the rollback process.
    ON_BLOCK_EXIT([this, opCtx] { _transitionFromRollbackToSecondary(opCtx); });
    ON_BLOCK_EXIT([this, opCtx] { _summarizeRollback(opCtx); });

    auto commonPointSW = _findCommonPoint(opCtx);
    if (!commonPointSW.isOK()) {
        return commonPointSW.getStatus();
    }

    const auto commonPoint = commonPointSW.getValue();
    const OpTime commonPointOpTime = commonPoint.getOpTime();
    _rollbackStats.commonPoint = commonPointOpTime;
    _listener->onCommonPointFound(commonPointOpTime.getTimestamp());

    // Now that we have found the common point, we make sure to proceed only if the rollback
    // period is not too long.
    status = _checkAgainstTimeLimit(commonPoint);
    if (!status.isOK()) {
        return status;
    }

    // Increment the Rollback ID of this node. The Rollback ID is a natural number that it is
    // incremented by 1 every time a rollback occurs. Note that the Rollback ID must be incremented
    // before modifying any local data.
    status = _replicationProcess->incrementRollbackID(opCtx);
    if (!status.isOK()) {
        return status;
    }
    _rollbackStats.rollbackId = _replicationProcess->getRollbackID();
    _listener->onRollbackIDIncremented();

    // This function cannot fail without terminating the process.
    _runPhaseFromAbortToReconstructPreparedTxns(opCtx, commonPoint);
    _listener->onPreparedTransactionsReconstructed();

    // We can now accept interruptions again.
    if (_isInShutdown()) {
        return Status(ErrorCodes::ShutdownInProgress, "rollback shutting down");
    }

    // At this point, the last applied and durable optimes on this node still point to ops on
    // the divergent branch of history. We therefore update the last optimes to the top of the
    // oplog, which should now be at the common point.
    _replicationCoordinator->resetLastOpTimesFromOplog(opCtx);
    status = _triggerOpObserver(opCtx);
    if (!status.isOK()) {
        return status;
    }
    _listener->onRollbackOpObserver(_observerInfo);

    LOGV2(21592, "Rollback complete");

    return Status::OK();
}

void RollbackImpl::shutdown() {
    stdx::lock_guard<Latch> lock(_mutex);
    _inShutdown = true;
}

bool RollbackImpl::_isInShutdown() const {
    stdx::lock_guard<Latch> lock(_mutex);
    return _inShutdown;
}

void RollbackImpl::_killAllUserOperations(OperationContext* opCtx) {
    invariant(opCtx);
    ServiceContext* serviceCtx = opCtx->getServiceContext();
    invariant(serviceCtx);

    int numOpsKilled = 0;
    int numOpsRunning = 0;

    for (ServiceContext::LockedClientsCursor cursor(serviceCtx); Client* client = cursor.next();) {
        stdx::lock_guard<Client> lk(*client);
        if (client->isFromSystemConnection() && !client->canKillSystemOperationInStepdown(lk)) {
            continue;
        }

        OperationContext* toKill = client->getOperationContext();

        if (toKill && toKill->getOpID() == opCtx->getOpID()) {
            // Don't kill the rollback thread.
            continue;
        }

        if (toKill && !toKill->isKillPending()) {
            serviceCtx->killOperation(lk, toKill, ErrorCodes::InterruptedDueToReplStateChange);
            numOpsKilled++;
        } else {
            numOpsRunning++;
        }
    }

    // Update the metrics for tracking user operations during state transitions.
    _replicationCoordinator->updateAndLogStateTransitionMetrics(
        ReplicationCoordinator::OpsKillingStateTransitionEnum::kRollback,
        numOpsKilled,
        numOpsRunning);
}

Status RollbackImpl::_transitionToRollback(OperationContext* opCtx) {
    invariant(opCtx);
    if (_isInShutdown()) {
        return Status(ErrorCodes::ShutdownInProgress, "rollback shutting down");
    }

    LOGV2(21593, "Transition to ROLLBACK");
    {
        rollbackHangBeforeTransitioningToRollback.pauseWhileSet(opCtx);
        ReplicationStateTransitionLockGuard rstlLock(
            opCtx, MODE_X, ReplicationStateTransitionLockGuard::EnqueueOnly());

        // Kill all user operations to ensure we can successfully acquire the RSTL. Since the node
        // must be a secondary, this is only killing readers, whose connections will be closed
        // shortly regardless.
        _killAllUserOperations(opCtx);

        rstlLock.waitForLockUntil(Date_t::max());

        auto status = _replicationCoordinator->setFollowerModeRollback(opCtx);
        if (!status.isOK()) {
            static constexpr char message[] = "Cannot perform replica set state transition";
            LOGV2(21594,
                  message,
                  "currentState"_attr = _replicationCoordinator->getMemberState().toString(),
                  "targetState"_attr = MemberState(MemberState::RS_ROLLBACK).toString(),
                  "error"_attr = status);
            status.addContext(str::stream() << message << ", current state: "
                                            << _replicationCoordinator->getMemberState().toString()
                                            << ", target state: "
                                            << MemberState(MemberState::RS_ROLLBACK).toString());
            return status;
        }
    }
    return Status::OK();
}

void RollbackImpl::_stopAndWaitForIndexBuilds(OperationContext* opCtx) {
    invariant(opCtx);

    // Aborts all active, two-phase index builds.
    [[maybe_unused]] auto stoppedIndexBuilds =
        IndexBuildsCoordinator::get(opCtx)->stopIndexBuildsForRollback(opCtx);

    // Get a list of all databases.
    StorageEngine* storageEngine = opCtx->getServiceContext()->getStorageEngine();
    std::vector<DatabaseName> dbs;
    {
        Lock::GlobalLock lk(opCtx, MODE_IS);
        dbs = storageEngine->listDatabases();
    }

    // Wait for all background operations to complete by waiting on each database. Single-phase
    // index builds are not stopped before rollback, so we must wait for these index builds to
    // complete.
    std::vector<DatabaseName> dbNames(dbs.begin(), dbs.end());
    LOGV2(21595, "Waiting for all background operations to complete before starting rollback");
    for (auto dbName : dbNames) {
        auto numInProg = IndexBuildsCoordinator::get(opCtx)->numInProgForDb(dbName.toString());
        if (numInProg > 0) {
            LOGV2_DEBUG(21596,
                        1,
                        "Waiting for {numBackgroundOperationsInProgress} "
                        "background operations to complete on database '{db}'",
                        "Waiting for background operations to complete",
                        "numBackgroundOperationsInProgress"_attr = numInProg,
                        "db"_attr = dbName);
            IndexBuildsCoordinator::get(opCtx)->awaitNoBgOpInProgForDb(opCtx, dbName.toString());
        }
    }

    LOGV2(21597, "Finished waiting for background operations to complete before rollback");
}

StatusWith<std::set<NamespaceString>> RollbackImpl::_namespacesForOp(const OplogEntry& oplogEntry) {
    NamespaceString opNss = oplogEntry.getNss();
    OpTypeEnum opType = oplogEntry.getOpType();
    std::set<NamespaceString> namespaces;

    // No namespaces for a no-op.
    if (opType == OpTypeEnum::kNoop) {
        return std::set<NamespaceString>();
    }

    // CRUD ops have the proper namespace in the operation 'ns' field.
    if (opType == OpTypeEnum::kInsert || opType == OpTypeEnum::kUpdate ||
        opType == OpTypeEnum::kDelete) {
        return std::set<NamespaceString>({opNss});
    }

    // If the operation is a command, then we need to extract the appropriate namespaces from the
    // command object, as opposed to just using the 'ns' field of the oplog entry itself.
    if (opType == OpTypeEnum::kCommand) {
        auto obj = oplogEntry.getObject();
        auto firstElem = obj.firstElement();

        // Does not handle 'applyOps' entries.
        invariant(oplogEntry.getCommandType() != OplogEntry::CommandType::kApplyOps,
                  "_namespacesForOp does not handle 'applyOps' oplog entries.");

        switch (oplogEntry.getCommandType()) {
            case OplogEntry::CommandType::kRenameCollection: {
                // Add both the 'from' and 'to' namespaces.
                namespaces.insert(NamespaceString(firstElem.valueStringDataSafe()));
                namespaces.insert(NamespaceString(obj.getStringField("to")));
                break;
            }
            case OplogEntry::CommandType::kDropDatabase: {
                // There is no specific namespace to save for a drop database operation.
                break;
            }
            case OplogEntry::CommandType::kDbCheck:
            case OplogEntry::CommandType::kEmptyCapped: {
                // These commands do not need to be supported by rollback. 'convertToCapped' should
                // always be converted to lower level DDL operations, and 'emptycapped' is a
                // testing-only command.
                std::string message = str::stream()
                    << "Encountered unsupported command type '" << firstElem.fieldName()
                    << "' during rollback.";
                return Status(ErrorCodes::UnrecoverableRollbackError, message);
            }
            case OplogEntry::CommandType::kCreate:
            case OplogEntry::CommandType::kDrop:
            case OplogEntry::CommandType::kImportCollection:
            case OplogEntry::CommandType::kCreateIndexes:
            case OplogEntry::CommandType::kDropIndexes:
            case OplogEntry::CommandType::kStartIndexBuild:
            case OplogEntry::CommandType::kAbortIndexBuild:
            case OplogEntry::CommandType::kCommitIndexBuild:
            case OplogEntry::CommandType::kCollMod: {
                // For all other command types, we should be able to parse the collection name from
                // the first command argument.
                try {
                    auto cmdNss = CommandHelpers::parseNsCollectionRequired(opNss.db(), obj);
                    namespaces.insert(cmdNss);
                } catch (const DBException& ex) {
                    return ex.toStatus();
                }
                break;
            }
            case OplogEntry::CommandType::kCommitTransaction:
            case OplogEntry::CommandType::kAbortTransaction: {
                break;
            }
            case OplogEntry::CommandType::kApplyOps:
            default:
                // Every possible command type should be handled above.
                MONGO_UNREACHABLE
        }
    }

    return namespaces;
}

void RollbackImpl::_restoreTxnsTableEntryFromRetryableWrites(OperationContext* opCtx,
                                                             Timestamp stableTimestamp) {
    auto client = std::make_unique<DBDirectClient>(opCtx);
    // Query for retryable writes oplog entries with a non-null 'prevWriteOpTime' value
    // less than or equal to the 'stableTimestamp'. This query intends to include no-op
    // retryable writes oplog entries that have been applied through a migration process.
    const auto filter = BSON("op" << BSON("$in" << BSON_ARRAY("i"
                                                              << "u"
                                                              << "d")));
    // We use the 'fromMigrate' field to differentiate migrated retryable writes entries from
    // transactions entries.
    const auto filterFromMigration = BSON("op"
                                          << "n"
                                          << "fromMigrate" << true);
    FindCommandRequest findRequest{NamespaceString::kRsOplogNamespace};
    findRequest.setFilter(BSON("ts" << BSON("$gt" << stableTimestamp) << "txnNumber"
                                    << BSON("$exists" << true) << "stmtId"
                                    << BSON("$exists" << true) << "prevOpTime.ts"
                                    << BSON("$gte" << Timestamp(1, 0) << "$lte" << stableTimestamp)
                                    << "$or" << BSON_ARRAY(filter << filterFromMigration)));
    auto cursor = client->find(std::move(findRequest));
    while (cursor->more()) {
        auto doc = cursor->next();
        auto swEntry = OplogEntry::parse(doc);
        fassert(5530502, swEntry.isOK());
        auto entry = swEntry.getValue();
        auto prevWriteOpTime = *entry.getPrevWriteOpTimeInTransaction();
        OperationSessionInfo opSessionInfo = entry.getOperationSessionInfo();
        const auto sessionId = *opSessionInfo.getSessionId();
        const auto txnNumber = *opSessionInfo.getTxnNumber();
        const auto wallClockTime = entry.getWallClockTime();

        invariant(!prevWriteOpTime.isNull() && prevWriteOpTime.getTimestamp() <= stableTimestamp);
        // This is a retryable writes oplog entry with a non-null 'prevWriteOpTime' value that
        // is less than or equal to the 'stableTimestamp'.
        LOGV2(5530501,
              "Restoring sessions entry to be consistent with 'stableTimestamp'",
              "stableTimestamp"_attr = stableTimestamp,
              "sessionId"_attr = sessionId,
              "txnNumber"_attr = txnNumber,
              "lastWriteOpTime"_attr = prevWriteOpTime);
        SessionTxnRecord sessionTxnRecord;
        sessionTxnRecord.setSessionId(sessionId);
        sessionTxnRecord.setTxnNum(txnNumber);
        try {
            TransactionHistoryIterator iter(prevWriteOpTime);
            auto nextOplogEntry = iter.next(opCtx);
            sessionTxnRecord.setLastWriteOpTime(nextOplogEntry.getOpTime());
            sessionTxnRecord.setLastWriteDate(nextOplogEntry.getWallClockTime());
        } catch (ExceptionFor<ErrorCodes::IncompleteTransactionHistory>&) {
            // It's possible that the next entry in the oplog chain has been truncated due to
            // oplog cap maintenance.
            sessionTxnRecord.setLastWriteOpTime(prevWriteOpTime);
            sessionTxnRecord.setLastWriteDate(wallClockTime);
        }
        const auto nss = NamespaceString::kSessionTransactionsTableNamespace;
        writeConflictRetry(opCtx, "updateSessionTransactionsTableInRollback", nss.ns(), [&] {
            opCtx->recoveryUnit()->allowUntimestampedWrite();
            AutoGetCollection collection(opCtx, nss, MODE_IX);
            auto filter = BSON(SessionTxnRecord::kSessionIdFieldName << sessionId.toBSON());
            UnreplicatedWritesBlock uwb(opCtx);
            // Perform an untimestamped write so that it will not be rolled back on recovering
            // to the 'stableTimestamp' if we were to crash. This is safe because this update is
            // meant to be consistent with the 'stableTimestamp' and not the common point.
            Helpers::upsert(opCtx, nss, filter, sessionTxnRecord.toBSON(), /*fromMigrate=*/false);
        });
    }
    // Take a stable checkpoint so that writes to the 'config.transactions' table are
    // persisted to disk before truncating the oplog. If we were to take an unstable checkpoint, we
    // would have to update replication metadata like 'minValid.appliedThrough' to be consistent
    // with the oplog.
    opCtx->recoveryUnit()->waitUntilUnjournaledWritesDurable(opCtx,
                                                             /*stableCheckpoint=*/true);
}

void RollbackImpl::_runPhaseFromAbortToReconstructPreparedTxns(
    OperationContext* opCtx, RollBackLocalOperations::RollbackCommonPoint commonPoint) noexcept {
    // Stop and wait for all background index builds to complete before starting the rollback
    // process.
    _stopAndWaitForIndexBuilds(opCtx);
    _listener->onBgIndexesComplete();

    // Before computing record store counts, abort all active transactions. This ensures that
    // the count adjustments are based on correct values where no prepared transactions are
    // active and all in-memory counts have been rolled-back.
    // Before calling recoverToStableTimestamp, we must abort the storage transaction of any
    // prepared transaction. This will require us to scan all sessions and call
    // abortPreparedTransactionForRollback() on any txnParticipant with a prepared transaction.
    killSessionsAbortAllPreparedTransactions(opCtx);

    // Ask the record store for the pre-rollback counts of any collections whose counts will
    // change and create a map with the adjusted counts for post-rollback. While finding the
    // common point, we keep track of how much each collection's count will change during the
    // rollback. Note: these numbers are relative to the common point, not the stable timestamp,
    // and thus must be set after recovering from the oplog.
    auto status = _findRecordStoreCounts(opCtx);
    fassert(31227, status);

    if (shouldCreateDataFiles()) {
        // Write a rollback file for each namespace that has documents that would be deleted by
        // rollback. We need to do this after aborting prepared transactions. Otherwise, we risk
        // unecessary prepare conflicts when trying to read documents that were modified by
        // those prepared transactions, which we know we will abort anyway.
        status = _writeRollbackFiles(opCtx);
        fassert(31228, status);
    } else {
        LOGV2(21598, "Not writing rollback files. 'createRollbackDataFiles' set to false");
    }

    // If there were rolled back operations on any session, invalidate all sessions.
    // We invalidate sessions before we recover so that we avoid invalidating sessions that had
    // just recovered prepared transactions.
    if (!_observerInfo.rollbackSessionIds.empty()) {
        MongoDSessionCatalog::invalidateAllSessions(opCtx);
    }

    // Recover to the stable timestamp.
    auto stableTimestamp = _recoverToStableTimestamp(opCtx);

    _rollbackStats.stableTimestamp = stableTimestamp;
    _listener->onRecoverToStableTimestamp(stableTimestamp);

    // Rollback historical ident entries.
    HistoricalIdentTracker::get(opCtx).rollbackTo(stableTimestamp);

    // Log the total number of insert and update operations that have been rolled back as a
    // result of recovering to the stable timestamp.
    auto getCommandCount = [&](StringData key) {
        const auto& m = _observerInfo.rollbackCommandCounts;
        auto it = m.find(key);
        return (it == m.end()) ? 0 : it->second;
    };
    LOGV2(21599,
          "Rollback reverted {insert} insert operations, {update} update operations and {delete} "
          "delete operations.",
          "Rollback reverted command counts",
          "insert"_attr = getCommandCount(kInsertCmdName),
          "update"_attr = getCommandCount(kUpdateCmdName),
          "delete"_attr = getCommandCount(kDeleteCmdName));

    // Retryable writes create derived updates to the transactions table which can be coalesced into
    // one operation, so certain session operations history may be lost after restoring to the
    // 'stableTimestamp'. We must scan the oplog and restore the transactions table entries to
    // detail the last executed writes.
    _restoreTxnsTableEntryFromRetryableWrites(opCtx, stableTimestamp);

    // During replication recovery, we truncate all oplog entries with timestamps greater than the
    // oplog truncate after point. If we entered rollback, we are guaranteed to have at least one
    // oplog entry after the common point.
    LOGV2(21600,
          "Marking to truncate all oplog entries with timestamps greater than "
          "{commonPoint}",
          "Marking to truncate all oplog entries with timestamps greater than common point",
          "commonPoint"_attr = commonPoint.getOpTime().getTimestamp());
    Timestamp truncatePoint = commonPoint.getOpTime().getTimestamp();

    // Persist the truncate point to the 'oplogTruncateAfterPoint' document. We save this value so
    // that the replication recovery logic knows where to truncate the oplog. We save this value
    // durably to match the behavior during startup recovery. This must occur after we successfully
    // recover to a stable timestamp. If recovering to a stable timestamp fails and we still
    // truncate the oplog then the oplog will not match the data files. If we crash at any earlier
    // point, we will recover, find a new sync source, and restart roll back (if necessary on the
    // new sync source). This is safe because a crash before this point would recover to a stable
    // checkpoint anyways at or earlier than the stable timestamp.
    //
    // Note that storage engine timestamp recovery only restores the database *data* to a stable
    // timestamp, but does not revert the oplog, which must be done as part of the rollback process.
    _replicationProcess->getConsistencyMarkers()->setOplogTruncateAfterPoint(opCtx, truncatePoint);
    _rollbackStats.truncateTimestamp = truncatePoint;
    _listener->onSetOplogTruncateAfterPoint(truncatePoint);

    // Align the drop pending reaper state with what's on disk. Oplog recovery depends on those
    // being consistent.
    _resetDropPendingState(opCtx);

    // Run the recovery process.
    _replicationProcess->getReplicationRecovery()->recoverFromOplog(opCtx, stableTimestamp);
    _listener->onRecoverFromOplog();

    // Sets the correct post-rollback counts on any collections whose counts changed during the
    // rollback.
    _correctRecordStoreCounts(opCtx);

    tenant_migration_access_blocker::recoverTenantMigrationAccessBlockers(opCtx);

    // Reconstruct prepared transactions after counts have been adjusted. Since prepared
    // transactions were aborted (i.e. the in-memory counts were rolled-back) before computing
    // collection counts, reconstruct the prepared transactions now, adding on any additional counts
    // to the now corrected record store.
    reconstructPreparedTransactions(opCtx, OplogApplication::Mode::kRecovering);
}

void RollbackImpl::_correctRecordStoreCounts(OperationContext* opCtx) {
    // This function explicitly does not check for shutdown since a clean shutdown post oplog
    // truncation is not allowed to occur until the record store counts are corrected.
    auto catalog = CollectionCatalog::get(opCtx);
    for (const auto& uiCount : _newCounts) {
        const auto uuid = uiCount.first;
        const auto coll = catalog->lookupCollectionByUUID(opCtx, uuid);
        invariant(coll,
                  str::stream() << "The collection with UUID " << uuid
                                << " is unexpectedly missing in the CollectionCatalog");
        const auto nss = coll->ns();
        invariant(!nss.isEmpty(),
                  str::stream() << "The collection with UUID " << uuid << " has no namespace.");
        const auto ident = coll->getRecordStore()->getIdent();
        invariant(!ident.empty(),
                  str::stream() << "The collection with UUID " << uuid << " has no ident.");

        auto newCount = uiCount.second;
        // If the collection is marked for size adjustment, then we made sure the collection size
        // was accurate at the stable timestamp and we can trust replication recovery to keep it
        // correct. This is necessary for capped collections whose deletions will be untracked
        // if we just set the collection count here.
        if (sizeRecoveryState(opCtx->getServiceContext())
                .collectionAlwaysNeedsSizeAdjustment(ident)) {
            LOGV2_DEBUG(
                21601,
                2,
                "Not setting collection count to {newCount} for {namespace} ({uuid}) "
                "[{ident}] because it is marked for size adjustment.",
                "Not setting collection count because namespace is marked for size adjustment",
                "newCount"_attr = newCount,
                "namespace"_attr = nss.ns(),
                "uuid"_attr = uuid.toString(),
                "ident"_attr = ident);
            continue;
        }

        // If _findRecordStoreCounts() is unable to determine the correct count from the oplog
        // (most likely due to a 4.0 drop oplog entry without the count information), we will
        // determine the correct count here post-recovery using a collection scan.
        if (kCollectionScanRequired == newCount) {
            LOGV2(21602,
                  "Scanning collection {namespace} ({uuid}) to fix collection count.",
                  "Scanning collection to fix collection count",
                  "namespace"_attr = nss.ns(),
                  "uuid"_attr = uuid.toString());
            AutoGetCollectionForRead collToScan(opCtx, nss);
            invariant(coll == collToScan.getCollection(),
                      str::stream() << "Catalog returned invalid collection: " << nss.ns() << " ("
                                    << uuid.toString() << ")");
            auto exec = collToScan->makePlanExecutor(opCtx,
                                                     collToScan.getCollection(),
                                                     PlanYieldPolicy::YieldPolicy::INTERRUPT_ONLY,
                                                     Collection::ScanDirection::kForward);
            long long countFromScan = 0;
            PlanExecutor::ExecState state;
            while (PlanExecutor::ADVANCED ==
                   (state = exec->getNext(static_cast<BSONObj*>(nullptr), nullptr))) {
                ++countFromScan;
            }
            if (PlanExecutor::IS_EOF != state) {
                // We ignore errors here because crashing or leaving rollback would only leave
                // collection counts more inaccurate.
                LOGV2_WARNING(21637,
                              "Failed to set count of {namespace} ({uuid}) [{ident}] due to failed "
                              "collection scan: {error}",
                              "Failed to set count of namespace due to failed collection scan",
                              "namespace"_attr = nss.ns(),
                              "uuid"_attr = uuid.toString(),
                              "ident"_attr = ident,
                              "error"_attr = exec->stateToStr(state));
                continue;
            }
            newCount = countFromScan;
        }

        auto status =
            _storageInterface->setCollectionCount(opCtx, {nss.db().toString(), uuid}, newCount);
        if (!status.isOK()) {
            // We ignore errors here because crashing or leaving rollback would only leave
            // collection counts more inaccurate.
            LOGV2_WARNING(21638,
                          "Failed to set count of {namespace} ({uuid}) [{ident}] to {newCount}. "
                          "Received: {error}",
                          "Failed to set count of namespace",
                          "namespace"_attr = nss.ns(),
                          "uuid"_attr = uuid.toString(),
                          "ident"_attr = ident,
                          "newCount"_attr = newCount,
                          "error"_attr = status);
        } else {
            LOGV2_DEBUG(21603,
                        2,
                        "Set collection count of {namespace} ({uuid}) [{ident}] to {newCount}.",
                        "Set collection count of namespace",
                        "namespace"_attr = nss.ns(),
                        "uuid"_attr = uuid.toString(),
                        "ident"_attr = ident,
                        "newCount"_attr = newCount);
        }
    }
}

Status RollbackImpl::_findRecordStoreCounts(OperationContext* opCtx) {
    auto catalog = CollectionCatalog::get(opCtx);
    auto storageEngine = opCtx->getServiceContext()->getStorageEngine();

    LOGV2(21604, "Finding record store counts");
    for (const auto& uiCount : _countDiffs) {
        auto uuid = uiCount.first;
        auto countDiff = uiCount.second;
        if (countDiff == 0) {
            continue;
        }

        auto nss = catalog->lookupNSSByUUID(opCtx, uuid);
        StorageInterface::CollectionCount oldCount = 0;

        // Drop-pending collections are not visible to rollback via the catalog when they are
        // managed by the storage engine. See StorageEngine::supportsPendingDrops().
        if (!nss) {
            invariant(storageEngine->supportsPendingDrops(),
                      str::stream() << "The collection with UUID " << uuid
                                    << " is unexpectedly missing in the CollectionCatalog");
            auto it = _pendingDrops.find(uuid);
            if (it == _pendingDrops.end()) {
                _newCounts[uuid] = kCollectionScanRequired;
                continue;
            }
            const auto& dropPendingInfo = it->second;
            nss = dropPendingInfo.nss;
            invariant(dropPendingInfo.count >= 0,
                      str::stream() << "The collection with UUID " << uuid
                                    << " was dropped with a negative collection count of "
                                    << dropPendingInfo.count
                                    << " in the drop or rename oplog entry. Unable to reset "
                                       "collection count during rollback.");
            oldCount = static_cast<StorageInterface::CollectionCount>(dropPendingInfo.count);
        } else {
            auto countSW = _storageInterface->getCollectionCount(opCtx, *nss);
            if (!countSW.isOK()) {
                return countSW.getStatus();
            }
            oldCount = countSW.getValue();
        }

        if (oldCount > static_cast<uint64_t>(std::numeric_limits<long long>::max())) {
            LOGV2_WARNING(21639,
                          "Count for {namespace} ({uuid}) was {oldCount} which is larger than the "
                          "maximum int64_t value. Not attempting to fix "
                          "count during rollback.",
                          "Count for namespace was larger than the maximum int64_t value. Not "
                          "attempting to fix count during rollback",
                          "namespace"_attr = nss->ns(),
                          "uuid"_attr = uuid.toString(),
                          "oldCount"_attr = oldCount);
            continue;
        }

        long long oldCountSigned = static_cast<long long>(oldCount);
        auto newCount = oldCountSigned + countDiff;

        if (newCount < 0) {
            LOGV2_WARNING(
                21640,
                "Attempted to set count for {namespace} ({uuid}) to {newCount} but set it to 0 "
                "instead. This is likely due to the count previously "
                "becoming inconsistent from an unclean shutdown or a rollback that could "
                "not fix the count correctly. Old count: {oldCount}. Count change: {countDiff}",
                "Attempted to set count for namespace but set it to 0 instead. This is likely due "
                "to the count previously becoming inconsistent from an unclean shutdown or a "
                "rollback that could not fix the count correctly",
                "namespace"_attr = nss->ns(),
                "uuid"_attr = uuid.toString(),
                "newCount"_attr = newCount,
                "oldCount"_attr = oldCount,
                "countDiff"_attr = countDiff);
            newCount = 0;
        }
        LOGV2_DEBUG(
            21605,
            2,
            "Record count of {namespace} ({uuid}) before rollback is {oldCount}. Setting it "
            "to {newCount}, due to change of {countDiff}",
            "Setting record count for namespace after rollback",
            "namespace"_attr = nss->ns(),
            "uuid"_attr = uuid.toString(),
            "oldCount"_attr = oldCount,
            "newCount"_attr = newCount,
            "countDiff"_attr = countDiff);
        _newCounts[uuid] = newCount;
    }

    return Status::OK();
}

/**
 * Process a single oplog entry that is getting rolled back and update the necessary rollback info
 * structures.
 */
Status RollbackImpl::_processRollbackOp(OperationContext* opCtx, const OplogEntry& oplogEntry) {
    ++_observerInfo.numberOfEntriesObserved;

    NamespaceString opNss = oplogEntry.getNss();
    OpTypeEnum opType = oplogEntry.getOpType();

    // For applyOps entries, we process each sub-operation individually.
    if (oplogEntry.getCommandType() == OplogEntry::CommandType::kApplyOps) {
        if (oplogEntry.shouldPrepare()) {
            // Uncommitted prepared transactions are always aborted before rollback begins, which
            // rolls back collection counts. Processing the operation here would result in
            // double-counting the sub-operations when correcting collection counts later.
            // Additionally, this logic makes an assumption that transactions are only ever
            // committed when the prepare operation is majority committed. This implies that when a
            // prepare oplog entry is rolled-back, it is guaranteed that it has never committed.
            return Status::OK();
        }
        if (oplogEntry.isPartialTransaction()) {
            // This oplog entry will be processed when we rollback the implicit commit for the
            // unprepared transaction (applyOps without partialTxn field).
            return Status::OK();
        }
        // Follow chain on applyOps oplog entries to process entire unprepared transaction.
        // The beginning of the applyOps chain may precede the common point.
        auto status = _processRollbackOpForApplyOps(opCtx, oplogEntry);
        if (const auto prevOpTime = oplogEntry.getPrevWriteOpTimeInTransaction()) {
            for (TransactionHistoryIterator iter(*prevOpTime); status.isOK() && iter.hasNext();) {
                status = _processRollbackOpForApplyOps(opCtx, iter.next(opCtx));
            }
        }
        return status;
    }

    // No information to record for a no-op.
    if (opType == OpTypeEnum::kNoop) {
        return Status::OK();
    }

    // Extract the appropriate namespaces from the oplog operation.
    auto namespacesSW = _namespacesForOp(oplogEntry);
    if (!namespacesSW.isOK()) {
        return namespacesSW.getStatus();
    } else {
        _observerInfo.rollbackNamespaces.insert(namespacesSW.getValue().begin(),
                                                namespacesSW.getValue().end());
    }

    // If the operation being rolled back has a session id, then we add it to the set of
    // sessions that had operations rolled back.
    OperationSessionInfo opSessionInfo = oplogEntry.getOperationSessionInfo();
    auto sessionId = opSessionInfo.getSessionId();
    if (sessionId) {
        _observerInfo.rollbackSessionIds.insert(sessionId->getId());
    }

    // Keep track of the _ids of inserted and updated documents, as we may need to write them out to
    // a rollback file.
    if (opType == OpTypeEnum::kInsert || opType == OpTypeEnum::kUpdate) {
        const auto uuid = oplogEntry.getUuid();
        invariant(uuid,
                  str::stream() << "Oplog entry to roll back is unexpectedly missing a UUID: "
                                << redact(oplogEntry.toBSONForLogging()));
        const auto idElem = oplogEntry.getIdElement();
        if (!idElem.eoo()) {
            // We call BSONElement::wrap() on each _id element to create a new BSONObj with an owned
            // buffer, as the underlying storage may be gone when we access this map to write
            // rollback files.
            _observerInfo.rollbackDeletedIdsMap[uuid.get()].insert(idElem.wrap());
            const auto cmdName = opType == OpTypeEnum::kInsert ? kInsertCmdName : kUpdateCmdName;
            ++_observerInfo.rollbackCommandCounts[cmdName];
        }
    }

    if (opType == OpTypeEnum::kInsert) {
        auto idVal = oplogEntry.getObject().getStringField("_id");
        if (serverGlobalParams.clusterRole == ClusterRole::ShardServer &&
            opNss == NamespaceString::kServerConfigurationNamespace &&
            idVal == ShardIdentityType::IdName) {
            // Check if the creation of the shard identity document is being rolled back.
            _observerInfo.shardIdentityRolledBack = true;
            LOGV2_WARNING(21641,
                          "Shard identity document rollback detected. oplog op: {oplogEntry}",
                          "Shard identity document rollback detected",
                          "oplogEntry"_attr = redact(oplogEntry.toBSONForLogging()));
        } else if (serverGlobalParams.clusterRole == ClusterRole::ConfigServer &&
                   opNss == VersionType::ConfigNS) {
            // Check if the creation of the config server config version document is being rolled
            // back.
            _observerInfo.configServerConfigVersionRolledBack = true;
            LOGV2_WARNING(21642,
                          "Config version document rollback detected. oplog op: {oplogEntry}",
                          "Config version document rollback detected",
                          "oplogEntry"_attr = redact(oplogEntry.toBSONForLogging()));
        }

        // Rolling back an insert must decrement the count by 1.
        _countDiffs[oplogEntry.getUuid().get()] -= 1;
    } else if (opType == OpTypeEnum::kDelete) {
        // Rolling back a delete must increment the count by 1.
        _countDiffs[oplogEntry.getUuid().get()] += 1;
    } else if (opType == OpTypeEnum::kCommand) {
        if (oplogEntry.getCommandType() == OplogEntry::CommandType::kCreate) {
            // If we roll back a create, then we do not need to change the size of that uuid.
            _countDiffs.erase(oplogEntry.getUuid().get());
            _pendingDrops.erase(oplogEntry.getUuid().get());
            _newCounts.erase(oplogEntry.getUuid().get());
        } else if (oplogEntry.getCommandType() == OplogEntry::CommandType::kImportCollection) {
            auto importEntry = mongo::ImportCollectionOplogEntry::parse(
                IDLParserContext("importCollectionOplogEntry"), oplogEntry.getObject());
            // Nothing to roll back if this is a dryRun.
            if (!importEntry.getDryRun()) {
                const auto& catalogEntry = importEntry.getCatalogEntry();
                auto importTargetUUID =
                    invariant(UUID::parse(catalogEntry["md"]["options"]["uuid"]),
                              str::stream() << "Oplog entry to roll back is unexpectedly missing "
                                               "import collection UUID: "
                                            << redact(oplogEntry.toBSONForLogging()));
                // If we roll back an import, then we do not need to change the size of that uuid.
                _countDiffs.erase(importTargetUUID);
                _pendingDrops.erase(importTargetUUID);
                _newCounts.erase(importTargetUUID);
            }
        } else if (oplogEntry.getCommandType() == OplogEntry::CommandType::kDrop) {
            // If we roll back a collection drop, parse the o2 field for the collection count for
            // use later by _findRecordStoreCounts().
            // This will be used to reconcile collection counts in the case where the drop-pending
            // collection is managed by the storage engine and is not accessible through the UUID
            // catalog.
            // Adding a _newCounts entry ensures that the count will be set after the rollback.
            const auto uuid = oplogEntry.getUuid().get();
            invariant(_countDiffs.find(uuid) == _countDiffs.end(),
                      str::stream() << "Unexpected existing count diff for " << uuid.toString()
                                    << " op: " << redact(oplogEntry.toBSONForLogging()));
            if (auto countResult = _parseDroppedCollectionCount(oplogEntry)) {
                PendingDropInfo info;
                info.count = *countResult;
                const auto& opNss = oplogEntry.getNss();
                info.nss =
                    CommandHelpers::parseNsCollectionRequired(opNss.db(), oplogEntry.getObject());
                _pendingDrops[uuid] = info;
                _newCounts[uuid] = info.count;
            } else {
                _newCounts[uuid] = kCollectionScanRequired;
            }
        } else if (oplogEntry.getCommandType() == OplogEntry::CommandType::kRenameCollection &&
                   oplogEntry.getObject()[kDropTargetFieldName].trueValue()) {
            // If we roll back a rename with a dropped target collection, parse the o2 field for the
            // target collection count for use later by _findRecordStoreCounts().
            // This will be used to reconcile collection counts in the case where the drop-pending
            // collection is managed by the storage engine and is not accessible through the UUID
            // catalog.
            // Adding a _newCounts entry ensures that the count will be set after the rollback.
            auto dropTargetUUID = invariant(
                UUID::parse(oplogEntry.getObject()[kDropTargetFieldName]),
                str::stream()
                    << "Oplog entry to roll back is unexpectedly missing dropTarget UUID: "
                    << redact(oplogEntry.toBSONForLogging()));
            invariant(_countDiffs.find(dropTargetUUID) == _countDiffs.end(),
                      str::stream()
                          << "Unexpected existing count diff for " << dropTargetUUID.toString()
                          << " op: " << redact(oplogEntry.toBSONForLogging()));
            if (auto countResult = _parseDroppedCollectionCount(oplogEntry)) {
                PendingDropInfo info;
                info.count = *countResult;
                info.nss = NamespaceString(oplogEntry.getObject()[kToFieldName].String());
                _pendingDrops[dropTargetUUID] = info;
                _newCounts[dropTargetUUID] = info.count;
            } else {
                _newCounts[dropTargetUUID] = kCollectionScanRequired;
            }
        } else if (oplogEntry.getCommandType() == OplogEntry::CommandType::kCommitTransaction) {
            // If we are rolling-back the commit of a prepared transaction, use the prepare oplog
            // entry to compute size adjustments. After recovering to the stable timestamp, prepared
            // transactions are reconstituted and any count adjustments will be replayed and
            // committed again.
            if (const auto prevOpTime = oplogEntry.getPrevWriteOpTimeInTransaction()) {
                for (TransactionHistoryIterator iter(*prevOpTime); iter.hasNext();) {
                    auto nextOplogEntry = iter.next(opCtx);
                    if (nextOplogEntry.getCommandType() != OplogEntry::CommandType::kApplyOps) {
                        continue;
                    }
                    auto status = _processRollbackOpForApplyOps(opCtx, nextOplogEntry);
                    if (!status.isOK()) {
                        return status;
                    }
                }
            }
            return Status::OK();
        }
    }

    // Keep count of major commands that will be rolled back.
    if (opType == OpTypeEnum::kCommand) {
        ++_observerInfo.rollbackCommandCounts[oplogEntry.getObject().firstElementFieldName()];
    }
    if (opType == OpTypeEnum::kDelete) {
        ++_observerInfo.rollbackCommandCounts[kDeleteCmdName];
    }

    return Status::OK();
}

Status RollbackImpl::_processRollbackOpForApplyOps(OperationContext* opCtx,
                                                   const OplogEntry& oplogEntry) {
    invariant(oplogEntry.getCommandType() == OplogEntry::CommandType::kApplyOps);

    try {
        // Roll back operations in reverse order in order to account for non-commutative
        // members of applyOps (e.g., commands inside of multi-document transactions).
        auto subOps = ApplyOps::extractOperations(oplogEntry);
        for (auto reverseIt = subOps.rbegin(); reverseIt != subOps.rend(); ++reverseIt) {
            auto subStatus = _processRollbackOp(opCtx, *reverseIt);
            if (!subStatus.isOK()) {
                return subStatus;
            }
        }
    } catch (DBException& e) {
        return e.toStatus();
    }

    return Status::OK();
}

StatusWith<RollBackLocalOperations::RollbackCommonPoint> RollbackImpl::_findCommonPoint(
    OperationContext* opCtx) {
    if (_isInShutdown()) {
        return Status(ErrorCodes::ShutdownInProgress, "rollback shutting down");
    }

    LOGV2(21606, "Finding common point");

    // We save some aggregate information about all operations that are rolled back, so that we can
    // pass this information to the rollback op observer. In most cases, other subsystems do not
    // need to know extensive details about every operation that rolled back, so to reduce
    // complexity by adding observer methods for every operation type, we provide a set of
    // information that should be suitable for most other subsystems to take the necessary actions
    // on a rollback event. This rollback info is kept in memory, so if we crash after we collect
    // it, it may be lost. However, if we crash any time between recovering to a stable timestamp
    // and completing oplog recovery, we assume that this information is not needed, since the node
    // restarting will have cleared out any invalid in-memory state anyway.
    auto onLocalOplogEntryFn = [&](const BSONObj& operation) {
        OplogEntry oplogEntry(operation);
        return _processRollbackOp(opCtx, oplogEntry);
    };

    // Calls syncRollBackLocalOperations to find the common point and run onLocalOplogEntryFn on
    // each oplog entry up until the common point. We only need the Timestamp of the common point
    // for the oplog truncate after point. Along the way, we save some information about the
    // rollback ops.
    auto commonPointSW =
        syncRollBackLocalOperations(*_localOplog, *_remoteOplog, onLocalOplogEntryFn);
    if (!commonPointSW.isOK()) {
        return commonPointSW.getStatus();
    }

    OpTime commonPointOpTime = commonPointSW.getValue().getOpTime();
    OpTime lastCommittedOpTime = _replicationCoordinator->getLastCommittedOpTime();
    OpTime committedSnapshot = _replicationCoordinator->getCurrentCommittedSnapshotOpTime();
    auto stableTimestamp =
        _storageInterface->getLastStableRecoveryTimestamp(opCtx->getServiceContext());

    LOGV2(21607,
          "Rollback common point is {commonPointOpTime}",
          "Rollback common point",
          "commonPointOpTime"_attr = commonPointOpTime);

    // This failpoint is used for testing the invariant below.
    if (MONGO_unlikely(rollbackToTimestampHangCommonPointBeforeReplCommitPoint.shouldFail()) &&
        (commonPointOpTime.getTimestamp() < lastCommittedOpTime.getTimestamp())) {
        LOGV2(5812200,
              "Hanging due to rollbackToTimestampHangCommonPointBeforeReplCommitPoint failpoint");
        rollbackToTimestampHangCommonPointBeforeReplCommitPoint.pauseWhileSet(opCtx);
    }

    // Rollback common point should be >= the replication commit point.
    invariant(commonPointOpTime.getTimestamp() >= lastCommittedOpTime.getTimestamp());
    invariant(commonPointOpTime >= lastCommittedOpTime);

    // Rollback common point should be >= the committed snapshot optime.
    invariant(commonPointOpTime.getTimestamp() >= committedSnapshot.getTimestamp());
    invariant(commonPointOpTime >= committedSnapshot);

    // Rollback common point should be >= the stable timestamp.
    invariant(stableTimestamp);
    if (commonPointOpTime.getTimestamp() < *stableTimestamp) {
        // This is an fassert rather than an invariant, since it can happen if the server was
        // recently upgraded to enableMajorityReadConcern=true.
        LOGV2_FATAL_NOTRACE(51121,
                            "Common point must be at least stable timestamp, common point: "
                            "{commonPoint}, stable timestamp: {stableTimestamp}",
                            "Common point must be at least stable timestamp",
                            "commonPoint"_attr = commonPointOpTime.getTimestamp(),
                            "stableTimestamp"_attr = *stableTimestamp);
    }

    return commonPointSW.getValue();
}

Status RollbackImpl::_checkAgainstTimeLimit(
    RollBackLocalOperations::RollbackCommonPoint commonPoint) {

    if (_isInShutdown()) {
        return Status(ErrorCodes::ShutdownInProgress, "rollback shutting down");
    }

    auto localOplogIter = _localOplog->makeIterator();
    const auto topOfOplogSW = localOplogIter->next();
    if (!topOfOplogSW.isOK()) {
        return Status(ErrorCodes::OplogStartMissing, "no oplog during rollback");
    }
    const auto topOfOplogBSON = topOfOplogSW.getValue().first;
    const auto topOfOplogOpAndWallTime = OpTimeAndWallTime::parse(topOfOplogBSON);

    _rollbackStats.lastLocalOptime = topOfOplogOpAndWallTime.opTime;

    auto topOfOplogWallTime = topOfOplogOpAndWallTime.wallTime;
    // We check the difference between the top of the oplog and the first oplog entry after the
    // common point when computing the rollback time limit.
    auto firstOpWallClockTimeAfterCommonPoint =
        commonPoint.getFirstOpWallClockTimeAfterCommonPoint();

    if (topOfOplogWallTime >= firstOpWallClockTimeAfterCommonPoint) {

        unsigned long long diff = durationCount<Seconds>(
            Milliseconds(topOfOplogWallTime - firstOpWallClockTimeAfterCommonPoint));

        _rollbackStats.lastLocalWallClockTime = topOfOplogWallTime;
        _rollbackStats.firstOpWallClockTimeAfterCommonPoint = firstOpWallClockTimeAfterCommonPoint;

        auto timeLimit = static_cast<unsigned long long>(gRollbackTimeLimitSecs.loadRelaxed());
        if (diff > timeLimit) {
            return Status(ErrorCodes::UnrecoverableRollbackError,
                          str::stream() << "not willing to roll back more than " << timeLimit
                                        << " seconds of data. Have: " << diff << " seconds.");
        }

    } else {
        LOGV2_WARNING(
            21643,
            "Wall clock times on oplog entries not monotonically increasing. This "
            "might indicate a backward clock skew. Time at first oplog after common point: "
            "{firstOpWallClockTimeAfterCommonPoint}. Time at top of oplog: {topOfOplogWallTime}",
            "Wall clock times on oplog entries not monotonically increasing. This might indicate a "
            "backward clock skew",
            "firstOpWallClockTimeAfterCommonPoint"_attr = firstOpWallClockTimeAfterCommonPoint,
            "topOfOplogWallTime"_attr = topOfOplogWallTime);
    }

    return Status::OK();
}

boost::optional<BSONObj> RollbackImpl::_findDocumentById(OperationContext* opCtx,
                                                         UUID uuid,
                                                         NamespaceString nss,
                                                         BSONElement id) {
    auto document = _storageInterface->findById(opCtx, {nss.db().toString(), uuid}, id);
    if (document.isOK()) {
        return document.getValue();
    } else if (document.getStatus().code() == ErrorCodes::NoSuchKey) {
        return boost::none;
    } else {
        LOGV2_FATAL_CONTINUE(
            21645,
            "Rollback failed to read document with {id} in namespace {namespace} with uuid "
            "{uuid}{error}",
            "Rollback failed to read document",
            "id"_attr = redact(id),
            "namespace"_attr = nss.ns(),
            "uuid"_attr = uuid.toString(),
            "error"_attr = causedBy(document.getStatus()));
        fassert(50751, document.getStatus());
    }

    MONGO_UNREACHABLE;
}

Status RollbackImpl::_writeRollbackFiles(OperationContext* opCtx) {
    auto catalog = CollectionCatalog::get(opCtx);
    auto storageEngine = opCtx->getServiceContext()->getStorageEngine();
    for (auto&& entry : _observerInfo.rollbackDeletedIdsMap) {
        const auto& uuid = entry.first;
        const auto nss = catalog->lookupNSSByUUID(opCtx, uuid);

        // Drop-pending collections are not visible to rollback via the catalog when they are
        // managed by the storage engine. See StorageEngine::supportsPendingDrops().
        if (!nss && storageEngine->supportsPendingDrops()) {
            LOGV2(21608,
                  "The collection with UUID {uuid} is missing in the CollectionCatalog. This could "
                  "be due to a dropped "
                  " collection. Not writing rollback file for uuid",
                  "Collection is missing in the CollectionCatalog. This could be due to a dropped "
                  "collection. Not writing rollback file for uuid",
                  "uuid"_attr = uuid);
            continue;
        }

        invariant(nss,
                  str::stream() << "The collection with UUID " << uuid
                                << " is unexpectedly missing in the CollectionCatalog");

        _writeRollbackFileForNamespace(opCtx, uuid, *nss, entry.second);
    }

    return Status::OK();
}

void RollbackImpl::_writeRollbackFileForNamespace(OperationContext* opCtx,
                                                  UUID uuid,
                                                  NamespaceString nss,
                                                  const SimpleBSONObjUnorderedSet& idSet) {
    RemoveSaver removeSaver(kRollbackRemoveSaverType, uuid.toString(), kRollbackRemoveSaverWhy);
    LOGV2(21609,
          "Preparing to write deleted documents to a rollback file for collection {namespace} with "
          "uuid {uuid} to {file}",
          "Preparing to write deleted documents to a rollback file",
          "namespace"_attr = nss.ns(),
          "uuid"_attr = uuid.toString(),
          "file"_attr = removeSaver.file().generic_string());

    // The RemoveSaver will save the data files in a directory structure similar to the following:
    //
    //     rollback
    //     ├── uuid
    //     │   └── removed.2018-03-20T20-23-01.21.bson
    //     ├── otheruuid
    //     │   ├── removed.2018-03-20T20-23-01.18.bson
    //     │   └── removed.2018-03-20T20-23-01.19.bson
    //
    // If this is the first data directory created, we save the full directory path in
    // _rollbackStats. Otherwise, we store the longest common prefix of the two directories.
    const auto& newDirectoryPath = removeSaver.root().generic_string();
    if (!_rollbackStats.rollbackDataFileDirectory) {
        _rollbackStats.rollbackDataFileDirectory = newDirectoryPath;
    } else {
        const auto& existingDirectoryPath = *_rollbackStats.rollbackDataFileDirectory;
        const auto& prefixEnd = std::mismatch(newDirectoryPath.begin(),
                                              newDirectoryPath.end(),
                                              existingDirectoryPath.begin(),
                                              existingDirectoryPath.end())
                                    .first;
        _rollbackStats.rollbackDataFileDirectory = std::string(newDirectoryPath.begin(), prefixEnd);
    }

    for (auto&& id : idSet) {
        // StorageInterface::findById() does not respect the collation, but because we are using
        // exact _id fields recorded in the oplog, we can get away with binary string
        // comparisons.
        auto document = _findDocumentById(opCtx, uuid, nss, id.firstElement());
        if (document) {
            fassert(50750, removeSaver.goingToDelete(*document));
        }
    }
    _listener->onRollbackFileWrittenForNamespace(std::move(uuid), std::move(nss));
}

Timestamp RollbackImpl::_recoverToStableTimestamp(OperationContext* opCtx) {
    // Recover to the stable timestamp while holding the global exclusive lock. This may throw,
    // which the caller must handle.
    Lock::GlobalWrite globalWrite(opCtx);
    return _storageInterface->recoverToStableTimestamp(opCtx);
}

Status RollbackImpl::_triggerOpObserver(OperationContext* opCtx) {
    if (_isInShutdown()) {
        return Status(ErrorCodes::ShutdownInProgress, "rollback shutting down");
    }
    LOGV2(21610, "Triggering the rollback op observer");
    opCtx->getServiceContext()->getOpObserver()->onReplicationRollback(opCtx, _observerInfo);
    return Status::OK();
}

void RollbackImpl::_transitionFromRollbackToSecondary(OperationContext* opCtx) {
    invariant(opCtx);

    // It is possible that this node has actually been removed due to a reconfig via
    // heartbeat during rollback. But it should be fine to transition to SECONDARY
    // and this won't change how the node reports its member state since topology
    // coordinator will always check if the node exists in its local config when
    // returning member state.
    LOGV2(21611, "Transition to SECONDARY");

    ReplicationStateTransitionLockGuard transitionGuard(opCtx, MODE_X);

    auto status = _replicationCoordinator->setFollowerMode(MemberState::RS_SECONDARY);
    if (!status.isOK()) {
        LOGV2_FATAL_NOTRACE(40408,
                            "Failed to transition into {targetState}; expected to be in "
                            "state {expectedState}; found self in "
                            "{actualState} {error}",
                            "Failed to perform replica set state transition",
                            "targetState"_attr = MemberState(MemberState::RS_SECONDARY),
                            "expectedState"_attr = MemberState(MemberState::RS_ROLLBACK),
                            "actualState"_attr = _replicationCoordinator->getMemberState(),
                            "error"_attr = causedBy(status));
    }
}

void RollbackImpl::_resetDropPendingState(OperationContext* opCtx) {
    // TODO(SERVER-38671): Remove this line when drop-pending idents are always supported with this
    // rolback method. Until then, we should assume that pending drops can be handled by either the
    // replication subsystem or the storage engine.
    DropPendingCollectionReaper::get(opCtx)->clearDropPendingState();

    // After recovering to a timestamp, the list of drop-pending idents maintained by the storage
    // engine is no longer accurate and needs to be cleared.
    auto storageEngine = opCtx->getServiceContext()->getStorageEngine();
    storageEngine->clearDropPendingState();

    std::vector<DatabaseName> dbNames = storageEngine->listDatabases();
    auto databaseHolder = DatabaseHolder::get(opCtx);
    for (const auto& dbName : dbNames) {
        Lock::DBLock dbLock(opCtx, dbName, MODE_X);
        auto db = databaseHolder->openDb(opCtx, dbName);
        db->checkForIdIndexesAndDropPendingCollections(opCtx);
    }
}

void RollbackImpl::_summarizeRollback(OperationContext* opCtx) const {
    logv2::DynamicAttributes attrs;
    attrs.add("startTime", _rollbackStats.startTime);
    auto now = opCtx->getServiceContext()->getFastClockSource()->now();
    attrs.add("endTime", now);
    auto syncSource = _remoteOplog->hostAndPort().toString();
    attrs.add("syncSource", syncSource);
    if (_rollbackStats.rollbackDataFileDirectory) {
        attrs.add("rollbackDataFileDirectory", *_rollbackStats.rollbackDataFileDirectory);
    }
    if (_rollbackStats.rollbackId) {
        attrs.add("rbid", *_rollbackStats.rollbackId);
    }
    if (_rollbackStats.lastLocalOptime) {
        attrs.add("lastOptimeRolledBack", *_rollbackStats.lastLocalOptime);
    }
    if (_rollbackStats.commonPoint) {
        attrs.add("commonPoint", *_rollbackStats.commonPoint);
    }
    if (_rollbackStats.lastLocalWallClockTime &&
        _rollbackStats.firstOpWallClockTimeAfterCommonPoint) {
        unsigned long long diff = durationCount<Seconds>(
            Milliseconds(*_rollbackStats.lastLocalWallClockTime -
                         *_rollbackStats.firstOpWallClockTimeAfterCommonPoint));

        attrs.add("lastWallClockTimeRolledBack", *_rollbackStats.lastLocalWallClockTime);
        attrs.add("firstOpWallClockTimeAfterCommonPoint",
                  *_rollbackStats.firstOpWallClockTimeAfterCommonPoint);
        attrs.add("wallClockTimeDiff", diff);
    }
    if (_rollbackStats.truncateTimestamp) {
        attrs.add("truncateTimestamp", *_rollbackStats.truncateTimestamp);
    }
    if (_rollbackStats.stableTimestamp) {
        attrs.add("stableTimestamp", *_rollbackStats.stableTimestamp);
    }
    attrs.add("shardIdentityRolledBack", _observerInfo.shardIdentityRolledBack);
    attrs.add("configServerConfigVersionRolledBack",
              _observerInfo.configServerConfigVersionRolledBack);
    attrs.add("affectedSessions", _observerInfo.rollbackSessionIds);
    attrs.add("affectedNamespaces", _observerInfo.rollbackNamespaces);
    attrs.add("rollbackCommandCounts", _observerInfo.rollbackCommandCounts);
    attrs.add("totalEntriesRolledBackIncludingNoops", _observerInfo.numberOfEntriesObserved);
    LOGV2(21612, "Rollback summary", attrs);
}

}  // namespace repl
}  // namespace mongo
