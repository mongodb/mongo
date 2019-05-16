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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kReplication

#include "mongo/platform/basic.h"

#include "mongo/db/repl/transaction_oplog_application.h"

#include "mongo/db/background.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/commands/txn_cmds_gen.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/index_builds_coordinator.h"
#include "mongo/db/repl/apply_ops.h"
#include "mongo/db/repl/storage_interface_impl.h"
#include "mongo/db/repl/timestamp_block.h"
#include "mongo/db/session_catalog_mongod.h"
#include "mongo/db/transaction_history_iterator.h"
#include "mongo/db/transaction_participant.h"
#include "mongo/util/log.h"

namespace mongo {
using repl::OplogEntry;
namespace {
// If enabled, causes _applyPrepareTransaction to hang before preparing the transaction participant.
MONGO_FAIL_POINT_DEFINE(applyOpsHangBeforePreparingTransaction);

// Failpoint that will cause reconstructPreparedTransactions to return early.
MONGO_FAIL_POINT_DEFINE(skipReconstructPreparedTransactions);


// Apply the oplog entries for a prepare or a prepared commit during recovery/initial sync.
Status _applyOperationsForTransaction(OperationContext* opCtx,
                                      const repl::MultiApplier::Operations& ops,
                                      repl::OplogApplication::Mode oplogApplicationMode) {
    // Apply each the operations via repl::applyOperation.
    for (const auto& op : ops) {
        AutoGetCollection coll(opCtx, op.getNss(), MODE_IX);
        auto status = repl::applyOperation_inlock(
            opCtx, coll.getDb(), op.toBSON(), false /*alwaysUpsert*/, oplogApplicationMode);
        if (!status.isOK()) {
            return status;
        }
    }
    return Status::OK();
}

/**
 * Helper that will find the previous oplog entry for that transaction, then for old-style applyOps
 * entries, will transform it to be a normal applyOps command and applies the oplog entry.
 *
 * For new-style transactions, with multiple oplog entries, will then read the entire set of oplog
 * entries for the transaction and apply each of them.
 *
 * Currently used for oplog application of a commitTransaction oplog entry during recovery, rollback
 * and initial sync.
 */
Status _applyTransactionFromOplogChain(OperationContext* opCtx,
                                       const OplogEntry& entry,
                                       repl::OplogApplication::Mode mode,
                                       Timestamp commitTimestamp,
                                       Timestamp durableTimestamp) {
    invariant(mode == repl::OplogApplication::Mode::kRecovering ||
              mode == repl::OplogApplication::Mode::kInitialSync);

    repl::MultiApplier::Operations ops;
    {
        // Traverse the oplog chain with its own snapshot and read timestamp.
        ReadSourceScope readSourceScope(opCtx);

        // Get the corresponding prepareTransaction oplog entry.
        const auto prepareOpTime = entry.getPrevWriteOpTimeInTransaction();
        invariant(prepareOpTime);
        TransactionHistoryIterator iter(prepareOpTime.get());
        invariant(iter.hasNext());
        const auto prepareOplogEntry = iter.next(opCtx);
        ops = readTransactionOperationsFromOplogChain(opCtx, prepareOplogEntry, {});
    }

    const auto dbName = entry.getNss().db().toString();
    Status status = Status::OK();

    writeConflictRetry(opCtx, "replaying prepared transaction", dbName, [&] {
        WriteUnitOfWork wunit(opCtx);

        // we might replay a prepared transaction behind oldest timestamp.
        opCtx->recoveryUnit()->setRoundUpPreparedTimestamps(true);

        BSONObjBuilder resultWeDontCareAbout;

        status = _applyOperationsForTransaction(opCtx, ops, mode);
        if (status.isOK()) {
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

Status applyCommitTransaction(OperationContext* opCtx,
                              const OplogEntry& entry,
                              repl::OplogApplication::Mode mode) {
    // Return error if run via applyOps command.
    uassert(50987,
            "commitTransaction is only used internally by secondaries.",
            mode != repl::OplogApplication::Mode::kApplyOpsCmd);

    IDLParserErrorContext ctx("commitTransaction");
    auto commitOplogEntryOpTime = entry.getOpTime();
    auto commitCommand = CommitTransactionOplogObject::parse(ctx, entry.getObject());
    const bool prepared = !commitCommand.getPrepared() || *commitCommand.getPrepared();
    if (!prepared)
        return Status::OK();
    invariant(commitCommand.getCommitTimestamp());

    if (mode == repl::OplogApplication::Mode::kRecovering ||
        mode == repl::OplogApplication::Mode::kInitialSync) {
        return _applyTransactionFromOplogChain(opCtx,
                                               entry,
                                               mode,
                                               *commitCommand.getCommitTimestamp(),
                                               commitOplogEntryOpTime.getTimestamp());
    }

    invariant(mode == repl::OplogApplication::Mode::kSecondary);

    // Transaction operations are in its own batch, so we can modify their opCtx.
    invariant(entry.getSessionId());
    invariant(entry.getTxnNumber());
    opCtx->setLogicalSessionId(*entry.getSessionId());
    opCtx->setTxnNumber(*entry.getTxnNumber());

    // The write on transaction table may be applied concurrently, so refreshing state
    // from disk may read that write, causing starting a new transaction on an existing
    // txnNumber. Thus, we start a new transaction without refreshing state from disk.
    MongoDOperationContextSessionWithoutRefresh sessionCheckout(opCtx);

    auto transaction = TransactionParticipant::get(opCtx);
    invariant(transaction);
    transaction.unstashTransactionResources(opCtx, "commitTransaction");
    transaction.commitPreparedTransaction(
        opCtx, *commitCommand.getCommitTimestamp(), commitOplogEntryOpTime);
    return Status::OK();
}

Status applyAbortTransaction(OperationContext* opCtx,
                             const OplogEntry& entry,
                             repl::OplogApplication::Mode mode) {
    // Return error if run via applyOps command.
    uassert(50972,
            "abortTransaction is only used internally by secondaries.",
            mode != repl::OplogApplication::Mode::kApplyOpsCmd);

    // We don't put transactions into the prepare state until the end of recovery and initial sync,
    // so there is no transaction to abort.
    if (mode == repl::OplogApplication::Mode::kRecovering ||
        mode == repl::OplogApplication::Mode::kInitialSync) {
        return Status::OK();
    }

    invariant(mode == repl::OplogApplication::Mode::kSecondary);

    // Transaction operations are in its own batch, so we can modify their opCtx.
    invariant(entry.getSessionId());
    invariant(entry.getTxnNumber());
    opCtx->setLogicalSessionId(*entry.getSessionId());
    opCtx->setTxnNumber(*entry.getTxnNumber());
    // The write on transaction table may be applied concurrently, so refreshing state
    // from disk may read that write, causing starting a new transaction on an existing
    // txnNumber. Thus, we start a new transaction without refreshing state from disk.
    MongoDOperationContextSessionWithoutRefresh sessionCheckout(opCtx);

    auto transaction = TransactionParticipant::get(opCtx);
    transaction.unstashTransactionResources(opCtx, "abortTransaction");
    transaction.abortActiveTransaction(opCtx);
    return Status::OK();
}

repl::MultiApplier::Operations readTransactionOperationsFromOplogChain(
    OperationContext* opCtx,
    const OplogEntry& commitOrPrepare,
    const std::vector<OplogEntry*> cachedOps) {
    repl::MultiApplier::Operations ops;

    // Get the previous oplog entry.
    auto currentOpTime = commitOrPrepare.getOpTime();

    // The cachedOps are the ops for this transaction that are from the same oplog application batch
    // as the commit or prepare, those which have not necessarily been written to the oplog.  These
    // ops are in order of increasing timestamp.

    // The lastEntryOpTime is the OpTime of the last (latest OpTime) entry for this transaction
    // which is expected to be present in the oplog.  It is the entry before the first cachedOp,
    // unless there are no cachedOps in which case it is the entry before the commit or prepare.
    const auto lastEntryOpTime = (cachedOps.empty() ? commitOrPrepare : *cachedOps.front())
                                     .getPrevWriteOpTimeInTransaction();
    invariant(lastEntryOpTime < currentOpTime);

    TransactionHistoryIterator iter(lastEntryOpTime.get());
    // Empty commits are not allowed, but empty prepares are.
    invariant(commitOrPrepare.getCommandType() != OplogEntry::CommandType::kCommitTransaction ||
              !cachedOps.empty() || iter.hasNext());
    auto commitOrPrepareObj = commitOrPrepare.toBSON();

    // First retrieve and transform the ops from the oplog, which will be retrieved in reverse
    // order.
    while (iter.hasNext()) {
        const auto& operationEntry = iter.next(opCtx);
        invariant(operationEntry.isPartialTransaction());
        auto prevOpsEnd = ops.size();
        repl::ApplyOps::extractOperationsTo(operationEntry, commitOrPrepareObj, &ops);
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
        repl::ApplyOps::extractOperationsTo(operationEntry, commitOrPrepareObj, &ops);
    }

    // Reconstruct the operations from the commit or prepare oplog entry.
    if (commitOrPrepare.getCommandType() == OplogEntry::CommandType::kApplyOps) {
        repl::ApplyOps::extractOperationsTo(commitOrPrepare, commitOrPrepareObj, &ops);
    }
    return ops;
}

namespace {
/**
 * This is the part of applyPrepareTransaction which is common to steady state, initial sync and
 * recovery oplog application.
 */
Status _applyPrepareTransaction(OperationContext* opCtx,
                                const OplogEntry& entry,
                                repl::OplogApplication::Mode oplogApplicationMode) {

    // The operations here are reconstructed at their prepare time.  However, that time will
    // be ignored because there is an outer write unit of work during their application.
    // The prepare time of the transaction is set explicitly below.
    auto ops = [&] {
        ReadSourceScope readSourceScope(opCtx);
        return readTransactionOperationsFromOplogChain(opCtx, entry, {});
    }();

    if (oplogApplicationMode == repl::OplogApplication::Mode::kRecovering ||
        oplogApplicationMode == repl::OplogApplication::Mode::kInitialSync) {
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
    for (const auto& op : ops) {
        auto ns = op.getNss();
        auto uuid = *op.getUuid();
        if (BackgroundOperation::inProgForNs(ns)) {
            warning() << "blocking replication until index builds are finished on "
                      << redact(ns.toString()) << ", due to prepared transaction";
            BackgroundOperation::awaitNoBgOpInProgForNs(ns);
            IndexBuildsCoordinator::get(opCtx)->awaitNoIndexBuildInProgressForCollection(uuid);
        }
    }

    // Transaction operations are in their own batch, so we can modify their opCtx.
    invariant(entry.getSessionId());
    invariant(entry.getTxnNumber());
    opCtx->setLogicalSessionId(*entry.getSessionId());
    opCtx->setTxnNumber(*entry.getTxnNumber());
    // The write on transaction table may be applied concurrently, so refreshing state
    // from disk may read that write, causing starting a new transaction on an existing
    // txnNumber. Thus, we start a new transaction without refreshing state from disk.
    MongoDOperationContextSessionWithoutRefresh sessionCheckout(opCtx);

    auto transaction = TransactionParticipant::get(opCtx);
    transaction.unstashTransactionResources(opCtx, "prepareTransaction");

    auto status = _applyOperationsForTransaction(opCtx, ops, oplogApplicationMode);
    if (!status.isOK())
        return status;

    if (MONGO_FAIL_POINT(applyOpsHangBeforePreparingTransaction)) {
        LOG(0) << "Hit applyOpsHangBeforePreparingTransaction failpoint";
        MONGO_FAIL_POINT_PAUSE_WHILE_SET_OR_INTERRUPTED(opCtx,
                                                        applyOpsHangBeforePreparingTransaction);
    }

    transaction.prepareTransaction(opCtx, entry.getOpTime());
    transaction.stashTransactionResources(opCtx);

    return Status::OK();
}

/**
 * Apply a prepared transaction during recovery.
 */
Status applyRecoveredPrepareTransaction(OperationContext* opCtx,
                                        const OplogEntry& entry,
                                        repl::OplogApplication::Mode mode) {
    // Snapshot transactions never conflict with the PBWM lock.
    invariant(!opCtx->lockState()->shouldConflictWithSecondaryBatchApplication());
    // We might replay a prepared transaction behind oldest timestamp.
    opCtx->recoveryUnit()->setRoundUpPreparedTimestamps(true);
    return _applyPrepareTransaction(opCtx, entry, mode);
}
}  // namespace

/**
 * Make sure that if we are in replication recovery or initial sync, we don't apply the prepare
 * transaction oplog entry until we either see a commit transaction oplog entry or are at the very
 * end of recovery/initial sync. Otherwise, only apply the prepare transaction oplog entry if we are
 * a secondary.
 */
Status applyPrepareTransaction(OperationContext* opCtx,
                               const OplogEntry& entry,
                               repl::OplogApplication::Mode oplogApplicationMode) {
    // Don't apply the operations from the prepared transaction until either we see a commit
    // transaction oplog entry during recovery or are at the end of recovery.
    if (oplogApplicationMode == repl::OplogApplication::Mode::kRecovering) {
        if (!serverGlobalParams.enableMajorityReadConcern) {
            error() << "Cannot replay a prepared transaction when 'enableMajorityReadConcern' is "
                       "set to false. Restart the server with --enableMajorityReadConcern=true "
                       "to complete recovery.";
        }
        fassert(51146, serverGlobalParams.enableMajorityReadConcern);
        return Status::OK();
    }

    // Don't apply the operations from the prepared transaction until either we see a commit
    // transaction oplog entry during the oplog application phase of initial sync or are at the end
    // of initial sync.
    if (oplogApplicationMode == repl::OplogApplication::Mode::kInitialSync) {
        return Status::OK();
    }

    // Return error if run via applyOps command.
    uassert(51145,
            "prepareTransaction oplog entry is only used internally by secondaries.",
            oplogApplicationMode != repl::OplogApplication::Mode::kApplyOpsCmd);

    invariant(oplogApplicationMode == repl::OplogApplication::Mode::kSecondary);
    return _applyPrepareTransaction(opCtx, entry, oplogApplicationMode);
}

void reconstructPreparedTransactions(OperationContext* opCtx, repl::OplogApplication::Mode mode) {
    if (MONGO_FAIL_POINT(skipReconstructPreparedTransactions)) {
        log() << "Hit skipReconstructPreparedTransactions failpoint";
        return;
    }
    // Read the transactions table with its own snapshot and read timestamp.
    ReadSourceScope readSourceScope(opCtx);

    DBDirectClient client(opCtx);
    const auto cursor = client.query(NamespaceString::kSessionTransactionsTableNamespace,
                                     {BSON("state"
                                           << "prepared")});

    // Iterate over each entry in the transactions table that has a prepared transaction.
    while (cursor->more()) {
        const auto txnRecordObj = cursor->next();
        const auto txnRecord = SessionTxnRecord::parse(
            IDLParserErrorContext("recovering prepared transaction"), txnRecordObj);

        invariant(txnRecord.getState() == DurableTxnStateEnum::kPrepared);

        // Get the prepareTransaction oplog entry corresponding to this transactions table entry.
        const auto prepareOpTime = txnRecord.getLastWriteOpTime();
        invariant(!prepareOpTime.isNull());
        TransactionHistoryIterator iter(prepareOpTime);
        invariant(iter.hasNext());
        auto prepareOplogEntry = iter.next(opCtx);

        {
            // Make a new opCtx so that we can set the lsid when applying the prepare transaction
            // oplog entry.
            auto newClient =
                opCtx->getServiceContext()->makeClient("reconstruct-prepared-transactions");
            AlternativeClientRegion acr(newClient);
            const auto newOpCtx = cc().makeOperationContext();
            repl::UnreplicatedWritesBlock uwb(newOpCtx.get());

            // Snapshot transaction can never conflict with the PBWM lock.
            newOpCtx->lockState()->setShouldConflictWithSecondaryBatchApplication(false);

            // TODO: SERVER-40177 This should be removed once it is guaranteed operations applied on
            // recovering nodes cannot encounter unnecessary prepare conflicts.
            newOpCtx->recoveryUnit()->setIgnorePrepared(true);

            // Checks out the session, applies the operations and prepares the transaction.
            uassertStatusOK(
                applyRecoveredPrepareTransaction(newOpCtx.get(), prepareOplogEntry, mode));
        }
    }
}
}  // namespace mongo
