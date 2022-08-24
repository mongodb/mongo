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

#include "mongo/db/repl/transaction_oplog_application.h"

#include "mongo/db/catalog_raii.h"
#include "mongo/db/commands/txn_cmds_gen.h"
#include "mongo/db/concurrency/exception_util.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/index_builds_coordinator.h"
#include "mongo/db/repl/apply_ops.h"
#include "mongo/db/repl/storage_interface_impl.h"
#include "mongo/db/repl/timestamp_block.h"
#include "mongo/db/session/session_catalog_mongod.h"
#include "mongo/db/transaction/transaction_history_iterator.h"
#include "mongo/db/transaction/transaction_participant.h"
#include "mongo/logv2/log.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kReplication


namespace mongo {
using repl::OplogEntry;
namespace {
// If enabled, causes _applyPrepareTransaction to hang before preparing the transaction participant.
MONGO_FAIL_POINT_DEFINE(applyOpsHangBeforePreparingTransaction);

// Failpoint that will cause reconstructPreparedTransactions to return early.
MONGO_FAIL_POINT_DEFINE(skipReconstructPreparedTransactions);

// Failpoint that causes apply prepare transaction oplog entry's ops to fail with write
// conflict error.
MONGO_FAIL_POINT_DEFINE(applyPrepareTxnOpsFailsWithWriteConflict);

MONGO_FAIL_POINT_DEFINE(hangBeforeSessionCheckOutForApplyPrepare);

// Apply the oplog entries for a prepare or a prepared commit during recovery/initial sync.
Status _applyOperationsForTransaction(OperationContext* opCtx,
                                      const std::vector<OplogEntry>& ops,
                                      repl::OplogApplication::Mode oplogApplicationMode) noexcept {
    // Apply each the operations via repl::applyOperation.
    for (const auto& op : ops) {
        try {
            if (op.getOpType() == repl::OpTypeEnum::kNoop) {
                continue;
            }

            // Presently, it is not allowed to run a prepared transaction with a command
            // inside. TODO(SERVER-46105)
            invariant(!op.isCommand());
            AutoGetCollection coll(opCtx, op.getNss(), MODE_IX);
            const bool isDataConsistent = true;
            auto status = repl::applyOperation_inlock(opCtx,
                                                      coll.getDb(),
                                                      &op,
                                                      false /*alwaysUpsert*/,
                                                      oplogApplicationMode,
                                                      isDataConsistent);
            if (!status.isOK()) {
                return status;
            }
        } catch (const DBException& ex) {
            // Ignore NamespaceNotFound errors if we are in initial sync or recovering mode.
            const bool ignoreException = ex.code() == ErrorCodes::NamespaceNotFound &&
                (oplogApplicationMode == repl::OplogApplication::Mode::kInitialSync ||
                 oplogApplicationMode == repl::OplogApplication::Mode::kRecovering);

            if (!ignoreException) {
                LOGV2_DEBUG(
                    21845,
                    1,
                    "Error applying operation in transaction. {error}- oplog entry: {oplogEntry}",
                    "Error applying operation in transaction",
                    "error"_attr = redact(ex),
                    "oplogEntry"_attr = redact(op.toBSONForLogging()));
                return exceptionToStatus();
            }
            LOGV2_DEBUG(21846,
                        1,
                        "Encountered but ignoring error: {error} while applying operations for "
                        "transaction because we are either in initial "
                        "sync or recovering mode - oplog entry: {oplogEntry}",
                        "Encountered but ignoring error while applying operations for transaction "
                        "because we are either in initial sync or recovering mode",
                        "error"_attr = redact(ex),
                        "oplogEntry"_attr = redact(op.toBSONForLogging()),
                        "oplogApplicationMode"_attr =
                            repl::OplogApplication::modeToString(oplogApplicationMode));
        }
    }
    return Status::OK();
}

/**
 * Helper that will read the entire sequence of oplog entries for the transaction and apply each of
 * them.
 *
 * Currently used for oplog application of a commitTransaction oplog entry during recovery and
 * rollback.
 */
Status _applyTransactionFromOplogChain(OperationContext* opCtx,
                                       const OplogEntry& entry,
                                       repl::OplogApplication::Mode mode,
                                       Timestamp commitTimestamp,
                                       Timestamp durableTimestamp) {
    invariant(mode == repl::OplogApplication::Mode::kRecovering);

    auto ops = readTransactionOperationsFromOplogChain(opCtx, entry, {});

    const auto dbName = entry.getNss().db().toString();
    Status status = Status::OK();

    writeConflictRetry(opCtx, "replaying prepared transaction", dbName, [&] {
        WriteUnitOfWork wunit(opCtx);

        // we might replay a prepared transaction behind oldest timestamp.
        opCtx->recoveryUnit()->setRoundUpPreparedTimestamps(true);

        BSONObjBuilder resultWeDontCareAbout;

        status = _applyOperationsForTransaction(opCtx, ops, mode);
        if (status.isOK()) {
            // If the transaction was empty then we have no locks, ensure at least Global IX.
            Lock::GlobalLock lk(opCtx, MODE_IX);
            opCtx->recoveryUnit()->setPrepareTimestamp(commitTimestamp);
            wunit.prepare();

            // Calls setCommitTimestamp() to set commit timestamp of the transaction and
            // clears the commit timestamp in the recovery unit when tsBlock goes out of the
            // scope. It is necessary that we clear the commit timestamp because there can be
            // another transaction in the same recovery unit calling setTimestamp().
            TimestampBlock tsBlock(opCtx, commitTimestamp);
            opCtx->recoveryUnit()->setDurableTimestamp(durableTimestamp);
            wunit.commit();
        }
    });
    return status;
}
}  // namespace

/**
 * Helper used to get previous oplog entry from the same transaction.
 */
repl::OplogEntry getPreviousOplogEntry(OperationContext* opCtx, const repl::OplogEntry& entry) {
    const auto prevOpTime = entry.getPrevWriteOpTimeInTransaction();
    invariant(prevOpTime);
    TransactionHistoryIterator iter(prevOpTime.value());
    invariant(iter.hasNext());
    const auto prevOplogEntry = iter.next(opCtx);

    return prevOplogEntry;
}


Status applyCommitTransaction(OperationContext* opCtx,
                              const OplogEntry& entry,
                              repl::OplogApplication::Mode mode) {
    IDLParserContext ctx("commitTransaction");
    auto commitOplogEntryOpTime = entry.getOpTime();
    auto commitCommand = CommitTransactionOplogObject::parse(ctx, entry.getObject());
    invariant(commitCommand.getCommitTimestamp());

    switch (mode) {
        case repl::OplogApplication::Mode::kRecovering: {
            return _applyTransactionFromOplogChain(opCtx,
                                                   entry,
                                                   mode,
                                                   *commitCommand.getCommitTimestamp(),
                                                   commitOplogEntryOpTime.getTimestamp());
        }
        case repl::OplogApplication::Mode::kInitialSync: {
            // Initial sync should never apply 'commitTransaction' since it unpacks committed
            // transactions onto various applier threads.
            MONGO_UNREACHABLE;
        }
        case repl::OplogApplication::Mode::kApplyOpsCmd: {
            // Return error if run via applyOps command.
            uasserted(50987, "commitTransaction is only used internally by secondaries.");
        }
        case repl::OplogApplication::Mode::kSecondary: {
            // Transaction operations are in its own batch, so we can modify their opCtx.
            invariant(entry.getSessionId());
            invariant(entry.getTxnNumber());
            opCtx->setLogicalSessionId(*entry.getSessionId());
            opCtx->setTxnNumber(*entry.getTxnNumber());
            if (auto txnRetryCounter = entry.getOperationSessionInfo().getTxnRetryCounter()) {
                opCtx->setTxnRetryCounter(*txnRetryCounter);
            }
            opCtx->setInMultiDocumentTransaction();

            // The write on transaction table may be applied concurrently, so refreshing state
            // from disk may read that write, causing starting a new transaction on an existing
            // txnNumber. Thus, we start a new transaction without refreshing state from disk.
            auto mongoDSessionCatalog = MongoDSessionCatalog::get(opCtx);
            auto sessionCheckout = mongoDSessionCatalog->checkOutSessionWithoutRefresh(opCtx);

            auto transaction = TransactionParticipant::get(opCtx);
            invariant(transaction);
            transaction.unstashTransactionResources(opCtx, "commitTransaction");
            transaction.commitPreparedTransaction(
                opCtx, *commitCommand.getCommitTimestamp(), commitOplogEntryOpTime);
            return Status::OK();
        }
    }
    MONGO_UNREACHABLE;
}

Status applyAbortTransaction(OperationContext* opCtx,
                             const OplogEntry& entry,
                             repl::OplogApplication::Mode mode) {
    switch (mode) {
        case repl::OplogApplication::Mode::kRecovering: {
            // We don't put transactions into the prepare state until the end of recovery,
            // so there is no transaction to abort.
            return Status::OK();
        }
        case repl::OplogApplication::Mode::kInitialSync: {
            // We don't put transactions into the prepare state until the end of initial sync,
            // so there is no transaction to abort.
            return Status::OK();
        }
        case repl::OplogApplication::Mode::kApplyOpsCmd: {
            // Return error if run via applyOps command.
            uasserted(50972, "abortTransaction is only used internally by secondaries.");
        }
        case repl::OplogApplication::Mode::kSecondary: {
            // Transaction operations are in its own batch, so we can modify their opCtx.
            invariant(entry.getSessionId());
            invariant(entry.getTxnNumber());
            opCtx->setLogicalSessionId(*entry.getSessionId());
            opCtx->setTxnNumber(*entry.getTxnNumber());
            if (auto txnRetryCounter = entry.getOperationSessionInfo().getTxnRetryCounter()) {
                opCtx->setTxnRetryCounter(*txnRetryCounter);
            }
            opCtx->setInMultiDocumentTransaction();

            // The write on transaction table may be applied concurrently, so refreshing state
            // from disk may read that write, causing starting a new transaction on an existing
            // txnNumber. Thus, we start a new transaction without refreshing state from disk.
            auto mongoDSessionCatalog = MongoDSessionCatalog::get(opCtx);
            auto sessionCheckout = mongoDSessionCatalog->checkOutSessionWithoutRefresh(opCtx);

            auto transaction = TransactionParticipant::get(opCtx);
            transaction.unstashTransactionResources(opCtx, "abortTransaction");
            transaction.abortTransaction(opCtx);
            return Status::OK();
        }
    }
    MONGO_UNREACHABLE;
}

std::pair<std::vector<OplogEntry>, bool> _readTransactionOperationsFromOplogChain(
    OperationContext* opCtx,
    const OplogEntry& lastEntryInTxn,
    const std::vector<OplogEntry*>& cachedOps,
    const bool checkForCommands) noexcept {
    bool isTransactionWithCommand = false;
    // Ensure future transactions read without a timestamp.
    invariant(RecoveryUnit::ReadSource::kNoTimestamp ==
              opCtx->recoveryUnit()->getTimestampReadSource());

    std::vector<OplogEntry> ops;

    // The cachedOps are the ops for this transaction that are from the same oplog application batch
    // as the commit or prepare, those which have not necessarily been written to the oplog.  These
    // ops are in order of increasing timestamp.
    const auto oldestEntryInBatch = cachedOps.empty() ? lastEntryInTxn : *cachedOps.front();

    // The lastEntryWrittenToOplogOpTime is the OpTime of the latest entry for this transaction
    // which is expected to be present in the oplog.  It is the entry before the first cachedOp,
    // unless there are no cachedOps in which case it is the entry before the commit or prepare.
    const auto lastEntryWrittenToOplogOpTime = oldestEntryInBatch.getPrevWriteOpTimeInTransaction();
    invariant(lastEntryWrittenToOplogOpTime < lastEntryInTxn.getOpTime());

    TransactionHistoryIterator iter(lastEntryWrittenToOplogOpTime.value());

    // If we started with a prepared commit, we want to forget about that operation and move onto
    // the prepare.
    auto prepareOrUnpreparedCommit = lastEntryInTxn;
    if (lastEntryInTxn.isPreparedCommit()) {
        // A prepared-commit must be in its own batch and thus have no cached ops.
        invariant(cachedOps.empty());
        invariant(iter.hasNext());
        prepareOrUnpreparedCommit = iter.nextFatalOnErrors(opCtx);
    }
    invariant(prepareOrUnpreparedCommit.getCommandType() == OplogEntry::CommandType::kApplyOps);

    // The non-DurableReplOperation fields of the extracted transaction operations will match those
    // of the lastEntryInTxn. For a prepared commit, this will include the commit oplog entry's
    // 'ts' field, which is what we want.
    auto lastEntryInTxnObj = lastEntryInTxn.getEntry().toBSON();

    // First retrieve and transform the ops from the oplog, which will be retrieved in reverse
    // order.
    while (iter.hasNext()) {
        const auto& operationEntry = iter.nextFatalOnErrors(opCtx);
        invariant(operationEntry.isPartialTransaction());
        auto prevOpsEnd = ops.size();
        repl::ApplyOps::extractOperationsTo(operationEntry, lastEntryInTxnObj, &ops);

        // Because BSONArrays do not have fast way of determining size without iterating through
        // them, and we also have no way of knowing how many oplog entries are in a transaction
        // without iterating, reversing each applyOps and then reversing the whole array is
        // about as good as we can do to get the entire thing in chronological order.  Fortunately
        // STL arrays of BSON objects should be fast to reverse (just pointer copies).
        std::reverse(ops.begin() + prevOpsEnd, ops.end());
    }
    std::reverse(ops.begin(), ops.end());

    // Next retrieve and transform the ops from the current batch, which are in increasing timestamp
    // order.
    for (auto* cachedOp : cachedOps) {
        const auto& operationEntry = *cachedOp;
        invariant(operationEntry.isPartialTransaction());
        repl::ApplyOps::extractOperationsTo(operationEntry, lastEntryInTxnObj, &ops);
    }

    // Reconstruct the operations from the prepare or unprepared commit oplog entry.
    repl::ApplyOps::extractOperationsTo(prepareOrUnpreparedCommit, lastEntryInTxnObj, &ops);

    // It is safe to assume that any commands inside `ops` are real commands to be applied, as
    // opposed to auxiliary commands such as "commit" and "abort".
    if (checkForCommands) {
        for (auto&& op : ops) {
            if (op.isCommand()) {
                isTransactionWithCommand = true;
                break;
            }
        }
    }
    return std::make_pair(ops, isTransactionWithCommand);
}

std::vector<OplogEntry> readTransactionOperationsFromOplogChain(
    OperationContext* opCtx,
    const OplogEntry& lastEntryInTxn,
    const std::vector<OplogEntry*>& cachedOps) noexcept {
    auto result = _readTransactionOperationsFromOplogChain(
        opCtx, lastEntryInTxn, cachedOps, false /*checkForCommands*/);
    return std::get<0>(result);
}

std::pair<std::vector<OplogEntry>, bool> readTransactionOperationsFromOplogChainAndCheckForCommands(
    OperationContext* opCtx,
    const OplogEntry& lastEntryInTxn,
    const std::vector<OplogEntry*>& cachedOps) noexcept {
    return _readTransactionOperationsFromOplogChain(
        opCtx, lastEntryInTxn, cachedOps, true /*checkForCommands*/);
}

namespace {
/**
 * This is the part of applyPrepareTransaction which is common to steady state, initial sync and
 * recovery oplog application.
 */
Status _applyPrepareTransaction(OperationContext* opCtx,
                                const OplogEntry& entry,
                                repl::OplogApplication::Mode mode) {

    // The operations here are reconstructed at their prepare time.  However, that time will
    // be ignored because there is an outer write unit of work during their application.
    // The prepare time of the transaction is set explicitly below.
    auto ops = readTransactionOperationsFromOplogChain(opCtx, entry, {});

    if (mode == repl::OplogApplication::Mode::kRecovering ||
        mode == repl::OplogApplication::Mode::kInitialSync) {
        // We might replay a prepared transaction behind oldest timestamp.  Note that since this is
        // scoped to the storage transaction, and readTransactionOperationsFromOplogChain implicitly
        // abandons the storage transaction when it releases the global lock, this must be done
        // after readTransactionOperationsFromOplogChain.
        opCtx->recoveryUnit()->setRoundUpPreparedTimestamps(true);
    }

    // Block application of prepare oplog entries on secondaries when a concurrent background index
    // build is running.
    // This will prevent hybrid index builds from corrupting an index on secondary nodes if a
    // prepared transaction becomes prepared during a build but commits after the index build
    // commits.
    // When two-phase index builds are in use, this is both unnecessary and unsafe. Due to locking,
    // we can guarantee that a transaction prepared on a primary during an index build will always
    // commit before that index build completes. Because two-phase index builds replicate start and
    // commit oplog entries, it will never be possible to replicate a prepared transaction, commit
    // an index build, then commit the transaction, the bug described above.
    // This blocking behavior can also introduce a deadlock with two-phase index builds on
    // a secondary if a prepared transaction blocks on an index build, but the index build can't
    // re-acquire its X lock because of the transaction.
    for (const auto& op : ops) {
        if (op.getOpType() == repl::OpTypeEnum::kNoop) {
            continue;
        }

        auto indexBuildsCoord = IndexBuildsCoordinator::get(opCtx);
        auto ns = op.getNss();
        auto uuid = *op.getUuid();
        if (indexBuildsCoord->inProgForCollection(uuid, IndexBuildProtocol::kSinglePhase)) {
            LOGV2_WARNING(21849,
                          "Blocking replication until single-phase index builds are finished on "
                          "collection, due to prepared transaction",
                          "namespace"_attr = redact(ns.toString()),
                          "uuid"_attr = uuid);
            indexBuildsCoord->awaitNoIndexBuildInProgressForCollection(
                opCtx, uuid, IndexBuildProtocol::kSinglePhase);
        }
    }

    // Transaction operations are in their own batch, so we can modify their opCtx.
    invariant(entry.getSessionId());
    invariant(entry.getTxnNumber());
    opCtx->setLogicalSessionId(*entry.getSessionId());
    opCtx->setTxnNumber(*entry.getTxnNumber());
    if (auto txnRetryCounter = entry.getOperationSessionInfo().getTxnRetryCounter()) {
        opCtx->setTxnRetryCounter(*txnRetryCounter);
    }
    opCtx->setInMultiDocumentTransaction();

    return writeConflictRetry(opCtx, "applying prepare transaction", entry.getNss().ns(), [&] {
        // The write on transaction table may be applied concurrently, so refreshing state
        // from disk may read that write, causing starting a new transaction on an existing
        // txnNumber. Thus, we start a new transaction without refreshing state from disk.
        hangBeforeSessionCheckOutForApplyPrepare.pauseWhileSet();
        auto mongoDSessionCatalog = MongoDSessionCatalog::get(opCtx);
        auto sessionCheckout = mongoDSessionCatalog->checkOutSessionWithoutRefresh(opCtx);

        auto txnParticipant = TransactionParticipant::get(opCtx);

        // We reset the recovery unit on retries, so make sure that we ignore prepare conflicts.
        opCtx->recoveryUnit()->setPrepareConflictBehavior(
            PrepareConflictBehavior::kIgnoreConflictsAllowWrites);

        // Release the WUOW, transaction lock resources and abort storage transaction so that the
        // writeConflictRetry loop will be able to retry applying transactional ops on WCE error.
        ScopeGuard abortOnError([&txnParticipant, opCtx] {
            // Abort the transaction and invalidate the session it is associated with.
            txnParticipant.abortTransaction(opCtx);
            txnParticipant.invalidate(opCtx);
        });

        // Starts the WUOW.
        txnParticipant.unstashTransactionResources(opCtx, "prepareTransaction");

        // Set this in case the application of any ops need to use the prepare timestamp of this
        // transaction. It should be cleared automatically when the transaction finishes.
        if (mode == repl::OplogApplication::Mode::kRecovering ||
            mode == repl::OplogApplication::Mode::kInitialSync) {
            txnParticipant.setPrepareOpTimeForRecovery(opCtx, entry.getOpTime());
        }

        auto status = _applyOperationsForTransaction(opCtx, ops, mode);

        if (opCtx->isRetryableWrite()) {
            for (const auto& op : ops) {
                if (!op.getStatementIds().empty()) {
                    txnParticipant.addCommittedStmtIds(
                        opCtx, op.getStatementIds(), entry.getOpTime());
                }
            }
        }

        if (MONGO_unlikely(applyPrepareTxnOpsFailsWithWriteConflict.shouldFail())) {
            LOGV2(4947101, "Hit applyPrepareTxnOpsFailsWithWriteConflict failpoint");
            status = Status(ErrorCodes::WriteConflict,
                            "Prepare transaction apply ops failed due to write conflict");
        }

        if (status == ErrorCodes::WriteConflict) {
            throwWriteConflictException(
                "Conflict encountered when applying a prepare transaction.");
        }
        fassert(31137, status);

        if (MONGO_unlikely(applyOpsHangBeforePreparingTransaction.shouldFail())) {
            LOGV2(21847, "Hit applyOpsHangBeforePreparingTransaction failpoint");
            applyOpsHangBeforePreparingTransaction.pauseWhileSet(opCtx);
        }

        txnParticipant.prepareTransaction(opCtx, entry.getOpTime());
        // Prepare transaction success.
        abortOnError.dismiss();

        txnParticipant.stashTransactionResources(opCtx);
        return Status::OK();
    });
}

/**
 * Apply a prepared transaction when we are reconstructing prepared transactions.
 */
void _reconstructPreparedTransaction(OperationContext* opCtx,
                                     const OplogEntry& prepareEntry,
                                     repl::OplogApplication::Mode mode) {
    repl::UnreplicatedWritesBlock uwb(opCtx);

    // Snapshot transaction can never conflict with the PBWM lock.
    opCtx->lockState()->setShouldConflictWithSecondaryBatchApplication(false);

    // When querying indexes, we return the record matching the key if it exists, or an
    // adjacent document. This means that it is possible for us to hit a prepare conflict if
    // we query for an incomplete key and an adjacent key is prepared.
    // We ignore prepare conflicts on recovering nodes because they may encounter prepare
    // conflicts that did not occur on the primary.
    opCtx->recoveryUnit()->setPrepareConflictBehavior(
        PrepareConflictBehavior::kIgnoreConflictsAllowWrites);

    // We might replay a prepared transaction behind oldest timestamp.
    opCtx->recoveryUnit()->setRoundUpPreparedTimestamps(true);

    // Checks out the session, applies the operations and prepares the transaction.
    uassertStatusOK(_applyPrepareTransaction(opCtx, prepareEntry, mode));
}
}  // namespace

/**
 * Make sure that if we are in replication recovery, we don't apply the prepare transaction oplog
 * entry until we either see a commit transaction oplog entry or are at the very end of recovery.
 * Otherwise, only apply the prepare transaction oplog entry if we are a secondary. We shouldn't get
 * here for initial sync and applyOps should error.
 */
Status applyPrepareTransaction(OperationContext* opCtx,
                               const OplogEntry& entry,
                               repl::OplogApplication::Mode mode) {
    switch (mode) {
        case repl::OplogApplication::Mode::kRecovering: {
            if (!serverGlobalParams.enableMajorityReadConcern) {
                LOGV2_ERROR(
                    21850,
                    "Cannot replay a prepared transaction when 'enableMajorityReadConcern' is "
                    "set to false. Restart the server with --enableMajorityReadConcern=true "
                    "to complete recovery");
                fassertFailed(51146);
            }

            // Don't apply the operations from the prepared transaction until either we see a commit
            // transaction oplog entry during recovery or are at the end of recovery.
            return Status::OK();
        }
        case repl::OplogApplication::Mode::kInitialSync: {
            // Initial sync should never apply 'prepareTransaction' since it unpacks committed
            // transactions onto various applier threads at commit time.
            MONGO_UNREACHABLE;
        }
        case repl::OplogApplication::Mode::kApplyOpsCmd: {
            // Return error if run via applyOps command.
            uasserted(51145,
                      "prepare applyOps oplog entry is only used internally by secondaries.");
        }
        case repl::OplogApplication::Mode::kSecondary: {
            return _applyPrepareTransaction(opCtx, entry, repl::OplogApplication::Mode::kSecondary);
        }
    }
    MONGO_UNREACHABLE;
}

void reconstructPreparedTransactions(OperationContext* opCtx, repl::OplogApplication::Mode mode) {
    if (MONGO_unlikely(skipReconstructPreparedTransactions.shouldFail())) {
        LOGV2(21848, "Hit skipReconstructPreparedTransactions failpoint");
        return;
    }

    // Ensure future transactions read without a timestamp.
    invariant(RecoveryUnit::ReadSource::kNoTimestamp ==
              opCtx->recoveryUnit()->getTimestampReadSource());

    DBDirectClient client(opCtx);
    FindCommandRequest findRequest{NamespaceString::kSessionTransactionsTableNamespace};
    findRequest.setFilter(BSON("state"
                               << "prepared"));
    const auto cursor = client.find(std::move(findRequest));

    // Iterate over each entry in the transactions table that has a prepared transaction.
    while (cursor->more()) {
        const auto txnRecordObj = cursor->next();
        const auto txnRecord = SessionTxnRecord::parse(
            IDLParserContext("recovering prepared transaction"), txnRecordObj);

        invariant(txnRecord.getState() == DurableTxnStateEnum::kPrepared);

        // Get the prepareTransaction oplog entry corresponding to this transactions table entry.
        const auto prepareOpTime = txnRecord.getLastWriteOpTime();
        invariant(!prepareOpTime.isNull());
        TransactionHistoryIterator iter(prepareOpTime);
        invariant(iter.hasNext());
        auto prepareOplogEntry = iter.nextFatalOnErrors(opCtx);

        {
            // Make a new opCtx so that we can set the lsid when applying the prepare transaction
            // oplog entry.
            auto newClient =
                opCtx->getServiceContext()->makeClient("reconstruct-prepared-transactions");
            AlternativeClientRegion acr(newClient);
            const auto newOpCtx = cc().makeOperationContext();

            _reconstructPreparedTransaction(newOpCtx.get(), prepareOplogEntry, mode);
        }
    }
}
}  // namespace mongo
