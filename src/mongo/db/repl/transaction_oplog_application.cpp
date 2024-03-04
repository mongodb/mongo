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


#include <algorithm>
#include <boost/optional.hpp>
#include <cstdint>
#include <iterator>
#include <memory>
#include <mutex>
#include <string>

#include <boost/cstdint.hpp>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

#include "mongo/base/error_codes.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/timestamp.h"
#include "mongo/client/dbclient_cursor.h"
#include "mongo/db/client.h"
#include "mongo/db/commands/txn_cmds_gen.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/concurrency/exception_util.h"
#include "mongo/db/concurrency/lock_manager_defs.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/index_builds_coordinator.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/op_observer/op_observer.h"
#include "mongo/db/query/find_command.h"
#include "mongo/db/repl/apply_ops_command_info.h"
#include "mongo/db/repl/oplog_entry_gen.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/repl/timestamp_block.h"
#include "mongo/db/repl/transaction_oplog_application.h"
#include "mongo/db/repl_index_build_state.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
#include "mongo/db/session/internal_session_pool.h"
#include "mongo/db/session/logical_session_id.h"
#include "mongo/db/session/logical_session_id_gen.h"
#include "mongo/db/session/logical_session_id_helpers.h"
#include "mongo/db/session/session_catalog_mongod.h"
#include "mongo/db/session/session_txn_record_gen.h"
#include "mongo/db/shard_role.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/db/transaction/transaction_history_iterator.h"
#include "mongo/db/transaction/transaction_participant.h"
#include "mongo/db/transaction_resources.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_attr.h"
#include "mongo/logv2/log_component.h"
#include "mongo/logv2/redaction.h"
#include "mongo/platform/compiler.h"
#include "mongo/s/sharding_feature_flags_gen.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/scopeguard.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kReplication


namespace mongo {

using repl::ApplierOperation;
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

class ScopedSetTxnInfoOnOperationContext {
public:
    ScopedSetTxnInfoOnOperationContext(OperationContext* opCtx,
                                       const LogicalSessionId& lsid,
                                       const TxnNumber& txnNumber,
                                       boost::optional<TxnRetryCounter> txnRetryCounter)
        : _opCtx(opCtx) {
        stdx::lock_guard<Client> lk(*opCtx->getClient());
        opCtx->setLogicalSessionId(lsid);
        opCtx->setTxnNumber(txnNumber);
        if (txnRetryCounter.has_value()) {
            opCtx->setTxnRetryCounter(*txnRetryCounter);
        }
        opCtx->setInMultiDocumentTransaction();
    }

    // The opCtx may be used to apply later operations in a batch, clean up before reusing.
    ~ScopedSetTxnInfoOnOperationContext() {
        stdx::lock_guard<Client> lk(*_opCtx->getClient());
        _opCtx->resetMultiDocumentTransactionState();
    }

    OperationContext* _opCtx;
};

// Given a vector of OplogEntry pointers, copy and return a vector of OplogEntry's.
std::vector<OplogEntry> _copyOps(const std::vector<const OplogEntry*>& ops) {
    std::vector<OplogEntry> res;
    res.reserve(ops.size());
    std::transform(ops.begin(),
                   ops.end(),
                   std::back_inserter(res),
                   [](const auto* op) -> const OplogEntry& { return *op; });

    return res;
}

// Returns all the committed statement IDs from the transaction operations if the transaction is
// a retryable internal transcation.
template <typename Operation>
boost::optional<std::vector<StmtId>> _getCommittedStmtIds(const LogicalSessionId& lsid,
                                                          const std::vector<Operation>& txnOps) {
    // The template type 'Operation' is expected to be either 'OplogEntry'
    // or 'OplogEntry*', so these functions are used to convert it to the
    // latter regardless of which one is the instantiated
    struct OpConverter {
        static const OplogEntry* asPtr(const OplogEntry& op) {
            return &op;
        }
        static const OplogEntry* asPtr(const OplogEntry* op) {
            return op;
        }
    };

    // Only retryable internal transactions need to deal with statement IDs.
    if (isInternalSessionForRetryableWrite(lsid)) {
        std::vector<StmtId> committedStmtIds;
        for (const auto& op : txnOps) {
            const auto& stmtIds = OpConverter::asPtr(op)->getStatementIds();
            committedStmtIds.insert(committedStmtIds.end(), stmtIds.begin(), stmtIds.end());
        }
        return committedStmtIds;
    }

    return boost::none;
}

// Apply the oplog entries for a prepare or a prepared commit during recovery/initial sync.
Status _applyOperationsForTransaction(OperationContext* opCtx,
                                      const std::vector<OplogEntry>& txnOps,
                                      repl::OplogApplication::Mode oplogApplicationMode) {

    const bool allowCollectionCreatinInPreparedTransactions =
        feature_flags::gCreateCollectionInPreparedTransactions.isEnabled(
            serverGlobalParams.featureCompatibility.acquireFCVSnapshot());
    // Apply each the operations via repl::applyOperation.
    for (const auto& op : txnOps) {
        try {
            if (op.getOpType() == repl::OpTypeEnum::kNoop) {
                continue;
            }

            if (!allowCollectionCreatinInPreparedTransactions) {
                // Presently, it is not allowed to run a prepared transaction with a command inside.
                // TODO(SERVER-46105)
                invariant(!op.isCommand());
            }

            auto coll = acquireCollection(
                opCtx,
                CollectionAcquisitionRequest(op.getNss(),
                                             AcquisitionPrerequisites::kPretendUnsharded,
                                             repl::ReadConcernArgs::get(opCtx),
                                             AcquisitionPrerequisites::kWrite),
                MODE_IX);
            const bool isDataConsistent = true;
            auto status = [&] {
                if (op.isCommand()) {
                    invariant(op.getCommandType() == OplogEntry::CommandType::kCreate ||
                                  op.getCommandType() == OplogEntry::CommandType::kCreateIndexes,
                              "Multi-document transactions only support 'create' and "
                              "'createIndexes' command");
                    return repl::applyCommand_inlock(
                        opCtx, ApplierOperation{&op}, oplogApplicationMode);
                }
                return repl::applyOperation_inlock(opCtx,
                                                   coll,
                                                   ApplierOperation{&op},
                                                   false /*alwaysUpsert*/,
                                                   oplogApplicationMode,
                                                   isDataConsistent);
            }();
            if (!status.isOK()) {
                return status;
            }
        } catch (const StorageUnavailableException&) {
            // Retriable error.
            throw;
        } catch (const DBException& ex) {
            // Ignore NamespaceNotFound errors if we are in initial sync or recovering mode.
            // During recovery we reconsutuct prepared transactions at the end after applying all
            // the oplogs, so 'NamespaceNotFound' error shouldn't be hit whether it is a stable or
            // unstable recovery. However we have some scenarios when this error should be skipped:
            //  1- This code path can be called while applying commit oplog during unstable recovery
            //     when 'startupRecoveryForRestore' is set.
            //  2- During selective backup:
            //     - During restore when 'recoverFromOplogAsStandalone' is set which is usually be
            //       done in a stable recovery mode.
            //     - After the restore finished as the standalone node started with the flag
            //       'takeUnstableCheckpointOnShutdown' so after restarting the node as a replica
            //       set member it will go through unstable recovery.
            const bool ignoreException = ex.code() == ErrorCodes::NamespaceNotFound &&
                (oplogApplicationMode == repl::OplogApplication::Mode::kInitialSync ||
                 repl::OplogApplication::inRecovering(oplogApplicationMode));

            if (ex.code() == ErrorCodes::NamespaceNotFound &&
                oplogApplicationMode == repl::OplogApplication::Mode::kStableRecovering) {
                repl::OplogApplication::checkOnOplogFailureForRecovery(
                    opCtx, op.getNss(), redact(op.toBSONForLogging()), redact(ex));
            }

            if (!ignoreException) {
                LOGV2_DEBUG(21845,
                            1,
                            "Error applying operation in transaction",
                            "error"_attr = redact(ex),
                            "oplogEntry"_attr = redact(op.toBSONForLogging()));
                return exceptionToStatus();
            }
            LOGV2_DEBUG(21846,
                        1,
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
    invariant(repl::OplogApplication::inRecovering(mode));
    invariant(entry.getSessionId().has_value(),
              "A transaction operation must always have a session id");
    invariant(entry.getTxnNumber().has_value(),
              "A transaction operation must always have a txn number");
    ScopedSetTxnInfoOnOperationContext scopedTxnInfo(
        opCtx,
        *entry.getSessionId(),
        *entry.getTxnNumber(),
        entry.getOperationSessionInfo().getTxnRetryCounter());

    auto ops = readTransactionOperationsFromOplogChain(opCtx, entry, {});

    const auto dbName = entry.getNss().dbName();
    Status status = Status::OK();

    repl::writeConflictRetryWithLimit(
        opCtx, "replaying prepared transaction", NamespaceString(dbName), [&] {
            WriteUnitOfWork wunit(opCtx);

            // We might replay a prepared transaction behind oldest timestamp.
            shard_role_details::getRecoveryUnit(opCtx)->setRoundUpPreparedTimestamps(true);

            BSONObjBuilder resultWeDontCareAbout;

            status = _applyOperationsForTransaction(opCtx, ops, mode);
            if (status.isOK()) {
                // If the transaction was empty then we have no locks, ensure at least Global
                // IX.
                Lock::GlobalLock lk(opCtx, MODE_IX);
                shard_role_details::getRecoveryUnit(opCtx)->setPrepareTimestamp(commitTimestamp);
                wunit.prepare();

                // Calls setCommitTimestamp() to set commit timestamp of the transaction and
                // clears the commit timestamp in the recovery unit when tsBlock goes out of the
                // scope. It is necessary that we clear the commit timestamp because there can
                // be another transaction in the same recovery unit calling setTimestamp().
                TimestampBlock tsBlock(opCtx, commitTimestamp);
                shard_role_details::getRecoveryUnit(opCtx)->setDurableTimestamp(durableTimestamp);
                wunit.commit();
            }
        });

    return status;
}

/**
 * This is the part of applyCommitTransaction which is common to both split and non-split commit
 * ops in secondary oplog application mode.
 */
Status _applyCommitTransaction(OperationContext* opCtx,
                               const OplogEntry& commitOp,
                               const LogicalSessionId& lsid,
                               TxnNumber txnNumber,
                               Timestamp commitTimestamp) {
    ScopedSetTxnInfoOnOperationContext scopedTxnInfo(
        opCtx, lsid, txnNumber, commitOp.getOperationSessionInfo().getTxnRetryCounter());

    // The write on transaction table may be applied concurrently, so refreshing state
    // from disk may read that write, causing starting a new transaction on an existing
    // txnNumber. Thus, we start a new transaction without refreshing state from disk.
    auto mongoDSessionCatalog = MongoDSessionCatalog::get(opCtx);
    auto sessionCheckout = mongoDSessionCatalog->checkOutSessionWithoutRefresh(opCtx);

    auto txnParticipant = TransactionParticipant::get(opCtx);
    invariant(txnParticipant);
    txnParticipant.unstashTransactionResources(opCtx, "commitTransaction");
    txnParticipant.commitPreparedTransaction(opCtx, commitTimestamp, commitOp.getOpTime());

    return Status::OK();
}

/**
 * This is the part of applyAbortTransaction which is common to both split and non-split abort
 * ops in secondary oplog application mode.
 */
Status _applyAbortTransaction(OperationContext* opCtx,
                              const OplogEntry& abortOp,
                              const LogicalSessionId& lsid,
                              TxnNumber txnNumber) {
    ScopedSetTxnInfoOnOperationContext scopedTxnInfo(
        opCtx, lsid, txnNumber, abortOp.getOperationSessionInfo().getTxnRetryCounter());

    // The write on transaction table may be applied concurrently, so refreshing state
    // from disk may read that write, causing starting a new transaction on an existing
    // txnNumber. Thus, we start a new transaction without refreshing state from disk.
    auto mongoDSessionCatalog = MongoDSessionCatalog::get(opCtx);
    auto sessionCheckout = mongoDSessionCatalog->checkOutSessionWithoutRefresh(opCtx);

    auto txnParticipant = TransactionParticipant::get(opCtx);
    invariant(txnParticipant);
    txnParticipant.unstashTransactionResources(opCtx, "abortTransaction");
    txnParticipant.abortTransaction(opCtx);

    return Status::OK();
}
}  // namespace

Status applyCommitTransaction(OperationContext* opCtx,
                              const ApplierOperation& op,
                              repl::OplogApplication::Mode mode) {
    IDLParserContext ctx("commitTransaction");
    auto commitCommand = CommitTransactionOplogObject::parse(ctx, op->getObject());
    auto commitTimestamp = *commitCommand.getCommitTimestamp();

    switch (mode) {
        case repl::OplogApplication::Mode::kUnstableRecovering:
        case repl::OplogApplication::Mode::kStableRecovering: {
            return _applyTransactionFromOplogChain(
                opCtx, *op, mode, commitTimestamp, op->getOpTime().getTimestamp());
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
            switch (op.instruction) {
                case repl::ApplicationInstruction::applyOplogEntry:
                case repl::ApplicationInstruction::applyTopLevelPreparedTxnOp: {
                    // Checkout the session and apply non-split or top-level commit op.
                    invariant(!op.subSession);
                    invariant(!op.preparedTxnOps);
                    return _applyCommitTransaction(
                        opCtx, *op, *op->getSessionId(), *op->getTxnNumber(), commitTimestamp);
                }
                case repl::ApplicationInstruction::applySplitPreparedTxnOp: {
                    // Checkout the session and apply split commit op.
                    invariant(op.subSession);
                    invariant(!op.preparedTxnOps);
                    return _applyCommitTransaction(opCtx,
                                                   *op,
                                                   (*op.subSession).getSessionId(),
                                                   (*op.subSession).getTxnNumber(),
                                                   commitTimestamp);
                }
            }
        }
    }
    MONGO_UNREACHABLE;
}

Status applyAbortTransaction(OperationContext* opCtx,
                             const ApplierOperation& op,
                             repl::OplogApplication::Mode mode) {
    switch (mode) {
        case repl::OplogApplication::Mode::kUnstableRecovering:
        case repl::OplogApplication::Mode::kStableRecovering: {
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
            switch (op.instruction) {
                case repl::ApplicationInstruction::applyOplogEntry:
                case repl::ApplicationInstruction::applyTopLevelPreparedTxnOp: {
                    // Checkout the session and apply non-split or top-level abort op.
                    invariant(!op.subSession);
                    invariant(!op.preparedTxnOps);
                    return _applyAbortTransaction(
                        opCtx, *op, *op->getSessionId(), *op->getTxnNumber());
                }
                case repl::ApplicationInstruction::applySplitPreparedTxnOp: {
                    // Checkout the session and apply split abort op.
                    invariant(op.subSession);
                    invariant(!op.preparedTxnOps);
                    return _applyAbortTransaction(opCtx,
                                                  *op,
                                                  (*op.subSession).getSessionId(),
                                                  (*op.subSession).getTxnNumber());
                }
            }
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
              shard_role_details::getRecoveryUnit(opCtx)->getTimestampReadSource());

    std::vector<OplogEntry> ops;

    // The cachedOps are the ops for this transaction that are from the same oplog application batch
    // as the commit or prepare, those which have not necessarily been written to the oplog.  These
    // ops are in order of increasing timestamp.
    const auto oldestEntryInBatch = cachedOps.empty() ? lastEntryInTxn : *cachedOps.front();

    // The lastEntryWrittenToOplogOpTime is the OpTime of the latest entry for this
    // transaction which is expected to be present in the oplog.  It is the entry
    // before the first cachedOp, unless there are no cachedOps in which case it is
    // the entry before the commit or prepare.
    const auto lastEntryWrittenToOplogOpTime = oldestEntryInBatch.getPrevWriteOpTimeInTransaction();
    invariant(lastEntryWrittenToOplogOpTime < lastEntryInTxn.getOpTime());

    TransactionHistoryIterator iter(lastEntryWrittenToOplogOpTime.value());

    // If we started with a prepared commit, we want to forget about that operation
    // and move onto the prepare.
    auto prepareOrUnpreparedCommit = lastEntryInTxn;
    if (lastEntryInTxn.isPreparedCommit()) {
        // A prepared-commit must be in its own batch and thus have no cached ops.
        invariant(cachedOps.empty());
        invariant(iter.hasNext());
        prepareOrUnpreparedCommit = iter.nextFatalOnErrors(opCtx);
    }
    invariant(prepareOrUnpreparedCommit.getCommandType() == OplogEntry::CommandType::kApplyOps);

    // The non-DurableReplOperation fields of the extracted transaction operations
    // will match those of the lastEntryInTxn. For a prepared commit, this will
    // include the commit oplog entry's 'ts' field, which is what we want.
    auto lastEntryInTxnObj = lastEntryInTxn.getEntry().toBSON();

    // First retrieve and transform the ops from the oplog, which will be retrieved
    // in reverse order.
    while (iter.hasNext()) {
        const auto& operationEntry = iter.nextFatalOnErrors(opCtx);
        invariant(operationEntry.isPartialTransaction());
        auto prevOpsEnd = ops.size();
        repl::ApplyOps::extractOperationsTo(operationEntry, lastEntryInTxnObj, &ops);

        // Because BSONArrays do not have fast way of determining size without
        // iterating through them, and we also have no way of knowing how many oplog
        // entries are in a transaction without iterating, reversing each applyOps
        // and then reversing the whole array is about as good as we can do to get
        // the entire thing in chronological order.  Fortunately STL arrays of BSON
        // objects should be fast to reverse (just pointer copies).
        std::reverse(ops.begin() + prevOpsEnd, ops.end());
    }
    std::reverse(ops.begin(), ops.end());

    // Next retrieve and transform the ops from the current batch, which are in
    // increasing timestamp order.
    for (auto* cachedOp : cachedOps) {
        const auto& operationEntry = *cachedOp;
        invariant(operationEntry.isPartialTransaction());
        repl::ApplyOps::extractOperationsTo(operationEntry, lastEntryInTxnObj, &ops);
    }

    // Reconstruct the operations from the prepare or unprepared commit oplog entry.
    repl::ApplyOps::extractOperationsTo(prepareOrUnpreparedCommit, lastEntryInTxnObj, &ops);

    // It is safe to assume that any commands inside `ops` are real commands to be
    // applied, as opposed to auxiliary commands such as "commit" and "abort".
    if (checkForCommands) {
        for (auto&& op : ops) {
            if (op.isCommand()) {
                isTransactionWithCommand = true;
                break;
            }
        }
    }
    return {std::move(ops), isTransactionWithCommand};
}

std::vector<OplogEntry> readTransactionOperationsFromOplogChain(
    OperationContext* opCtx,
    const OplogEntry& lastEntryInTxn,
    const std::vector<OplogEntry*>& cachedOps) noexcept {
    auto [txnOps, _] = _readTransactionOperationsFromOplogChain(
        opCtx, lastEntryInTxn, cachedOps, false /*checkForCommands*/);
    return std::move(txnOps);
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
 * This is the part of applyPrepareTransaction which is common to steady state, initial
 * sync and recovery oplog application.
 *
 * Note: when this is called to apply a split prepared transaction, the txnOps here represents
 * a subset of all the ops in the transaction, but when being called by a top-level prepared
 * transaction, it's just an empty array. Future changes that depend on transaction operations
 * should be careful about the differences.
 */
Status _applyPrepareTransaction(OperationContext* opCtx,
                                const OplogEntry& prepareOp,
                                const LogicalSessionId& lsid,
                                TxnNumber txnNumber,
                                const std::vector<OplogEntry>& txnOps,
                                repl::OplogApplication::Mode mode,
                                boost::optional<std::vector<StmtId>> stmtIds = boost::none) {
    ScopedSetTxnInfoOnOperationContext scopedTxnInfo(
        opCtx, lsid, txnNumber, prepareOp.getOperationSessionInfo().getTxnRetryCounter());

    // Block application of prepare oplog entries on secondaries when a concurrent
    // background index build is running. This will prevent hybrid index builds from
    // corrupting an index on secondary nodes if a prepared transaction becomes prepared
    // during a build but commits after the index build commits. When two-phase index
    // builds are in use, this is both unnecessary and unsafe. Due to locking, we can
    // guarantee that a transaction prepared on a primary during an index build will
    // always commit before that index build completes. Because two-phase index builds
    // replicate start and commit oplog entries, it will never be possible to replicate
    // a prepared transaction, commit an index build, then commit the transaction, the
    // bug described above. This blocking behavior can also introduce a deadlock with
    // two-phase index builds on a secondary if a prepared transaction blocks on an
    // index build, but the index build can't re-acquire its X lock because of the
    // transaction.
    for (const auto& op : txnOps) {
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
                          "namespace"_attr = redact(toStringForLogging(ns)),
                          "uuid"_attr = uuid);
            indexBuildsCoord->awaitNoIndexBuildInProgressForCollection(
                opCtx, uuid, IndexBuildProtocol::kSinglePhase);
        }
    }

    return repl::writeConflictRetryWithLimit(
        opCtx, "applying prepare transaction", prepareOp.getNss(), [&] {
            // The write on transaction table may be applied concurrently, so refreshing
            // state from disk may read that write, causing starting a new transaction
            // on an existing txnNumber. Thus, we start a new transaction without
            // refreshing state from disk.
            hangBeforeSessionCheckOutForApplyPrepare.pauseWhileSet();
            auto mongoDSessionCatalog = MongoDSessionCatalog::get(opCtx);
            auto sessionCheckout = mongoDSessionCatalog->checkOutSessionWithoutRefresh(opCtx);
            auto txnParticipant = TransactionParticipant::get(opCtx);

            // We reset the recovery unit on retries, so make sure that we set the
            // necessary states.

            // When querying indexes, we return the record matching the key if it exists,
            // or an adjacent document. This means that it is possible for us to hit a
            // prepare conflict if we query for an incomplete key and an adjacent key is
            // prepared. We ignore prepare conflicts on recovering nodes because they may
            // may encounter prepare conflicts that did not occur on the primary.
            shard_role_details::getRecoveryUnit(opCtx)->setPrepareConflictBehavior(
                PrepareConflictBehavior::kIgnoreConflictsAllowWrites);

            // We might replay a prepared transaction behind oldest timestamp.
            if (repl::OplogApplication::inRecovering(mode) ||
                mode == repl::OplogApplication::Mode::kInitialSync) {
                shard_role_details::getRecoveryUnit(opCtx)->setRoundUpPreparedTimestamps(true);
            }

            // Release WUOW, transaction lock resources and abort storage transaction
            // so that the writeConflictRetry loop will be able to retry applying the
            // transactional ops on WCE error.
            ScopeGuard abortOnError([&txnParticipant, opCtx] {
                // Abort transaction and invalidate the session it is associated with.
                txnParticipant.abortTransaction(opCtx);
                txnParticipant.invalidate(opCtx);
            });

            // Starts the WUOW.
            txnParticipant.unstashTransactionResources(
                opCtx, "prepareTransaction", repl::OplogApplication::inRecovering(mode));

            // Set this in case the application of any ops needs to use the prepare timestamp
            // of this transaction. It should be cleared automatically when the txn finishes.
            if (repl::OplogApplication::inRecovering(mode) ||
                mode == repl::OplogApplication::Mode::kInitialSync) {
                txnParticipant.setPrepareOpTimeForRecovery(opCtx, prepareOp.getOpTime());
            }

            auto status = _applyOperationsForTransaction(opCtx, txnOps, mode);

            // Add committed statement IDs if this is a retryable internal transaction.
            // They are used when this node becomes primary to avoid re-executing
            // committed txn statements.
            const auto& committedStmtIds = stmtIds ? stmtIds : _getCommittedStmtIds(lsid, txnOps);
            if (committedStmtIds) {
                txnParticipant.addCommittedStmtIds(opCtx, *committedStmtIds, prepareOp.getOpTime());
            }

            if (MONGO_unlikely(applyPrepareTxnOpsFailsWithWriteConflict.shouldFail())) {
                LOGV2(4947101, "Hit applyPrepareTxnOpsFailsWithWriteConflict failpoint");
                throwWriteConflictException(
                    "Prepare transaction apply ops failed due to write conflict");
            }

            fassert(31137, status);

            if (MONGO_unlikely(applyOpsHangBeforePreparingTransaction.shouldFail())) {
                LOGV2(21847, "Hit applyOpsHangBeforePreparingTransaction failpoint");
                applyOpsHangBeforePreparingTransaction.pauseWhileSet(opCtx);
            }

            // If we are in a recovery mode, we should have set roundUpPreparedTimestamps to true
            // above, and that setting should have persisted after we unstashed the transaction
            // resources and started the WUOW above. This setting is necessary for the subsequent
            // call to prepareTransaction to succeed in the case where the prepare timestamp is
            // older than the stable timestamp.
            invariant(!repl::OplogApplication::inRecovering(mode) ||
                      shard_role_details::getRecoveryUnit(opCtx)->getRoundUpPreparedTimestamps());

            txnParticipant.prepareTransaction(opCtx, prepareOp.getOpTime());

            auto opObserver = opCtx->getServiceContext()->getOpObserver();
            invariant(opObserver);
            opObserver->onTransactionPrepareNonPrimary(
                opCtx, *prepareOp.getSessionId(), txnOps, prepareOp.getOpTime());

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
                                     const OplogEntry& prepareOp,
                                     repl::OplogApplication::Mode mode) {
    repl::UnreplicatedWritesBlock uwb(opCtx);

    // The operations here are reconstructed at their prepare time. However, that time
    // will be ignored because there is an outer write unit of work during their
    // application. The prepare time of the transaction is set explicitly below.
    auto ops = readTransactionOperationsFromOplogChain(opCtx, prepareOp, {});

    // Checks out the session, applies the operations and prepares the transaction.
    uassertStatusOK(_applyPrepareTransaction(
        opCtx, prepareOp, *prepareOp.getSessionId(), *prepareOp.getTxnNumber(), ops, mode));
}
}  // namespace

/**
 * Make sure that if we are in replication recovery, we don't apply the prepare
 * transaction oplog entry until we either see a commit transaction oplog entry or are
 * at the very end of recovery. Otherwise, only apply the prepare transaction oplog
 * entry if we are a secondary. We shouldn't get here for initial sync and applyOps
 * should error.
 */
Status applyPrepareTransaction(OperationContext* opCtx,
                               const ApplierOperation& op,
                               repl::OplogApplication::Mode mode) {
    switch (mode) {
        case repl::OplogApplication::Mode::kUnstableRecovering:
        case repl::OplogApplication::Mode::kStableRecovering: {
            if (!serverGlobalParams.enableMajorityReadConcern) {
                LOGV2_ERROR(21850,
                            "Cannot replay a prepared transaction when "
                            "'enableMajorityReadConcern' is "
                            "set to false. Restart the server with "
                            "--enableMajorityReadConcern=true "
                            "to complete recovery");
                fassertFailed(51146);
            }

            // Don't apply the operations from the prepared transaction until either we
            // see a commit transaction oplog entry during recovery or are at the end of
            // recovery.
            return Status::OK();
        }
        case repl::OplogApplication::Mode::kInitialSync: {
            // Initial sync should never apply 'prepareTransaction' since it unpacks
            // committed transactions onto various applier threads at commit time.
            MONGO_UNREACHABLE;
        }
        case repl::OplogApplication::Mode::kApplyOpsCmd: {
            // Return error if run via applyOps command.
            uasserted(51145,
                      "prepare applyOps oplog entry is only used internally by secondaries.");
        }
        case repl::OplogApplication::Mode::kSecondary: {
            switch (op.instruction) {
                case repl::ApplicationInstruction::applyOplogEntry: {
                    // Not possible for secondary when applying prepare oplog entries.
                    MONGO_UNREACHABLE;
                }
                case repl::ApplicationInstruction::applySplitPreparedTxnOp: {
                    // Checkout the session and apply split prepare op.
                    invariant(op.subSession);
                    invariant(op.preparedTxnOps);
                    return _applyPrepareTransaction(opCtx,
                                                    *op,
                                                    (*op.subSession).getSessionId(),
                                                    (*op.subSession).getTxnNumber(),
                                                    _copyOps(*op.preparedTxnOps),
                                                    repl::OplogApplication::Mode::kSecondary);
                }
                case repl::ApplicationInstruction::applyTopLevelPreparedTxnOp: {
                    // Checkout the session and apply top-level prepare op.
                    invariant(!op.subSession);
                    invariant(op.preparedTxnOps);
                    const auto& txnOps = *op.preparedTxnOps;
                    // For a top-level transaction, the actual transaction operations should've
                    // already been applied by its split transactions. So here we just pass it
                    // an empty array of transaction operations. However if this is a retryable
                    // internal transaction, we need to pass the committed statement IDs.
                    auto stmtIds = _getCommittedStmtIds(*op->getSessionId(), txnOps);
                    return _applyPrepareTransaction(opCtx,
                                                    *op,
                                                    *op->getSessionId(),
                                                    *op->getTxnNumber(),
                                                    {},
                                                    mode,
                                                    std::move(stmtIds));
                }
            }
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
              shard_role_details::getRecoveryUnit(opCtx)->getTimestampReadSource());

    DBDirectClient client(opCtx);
    FindCommandRequest findRequest{NamespaceString::kSessionTransactionsTableNamespace};
    findRequest.setFilter(BSON("state"
                               << "prepared"));
    const auto cursor = client.find(std::move(findRequest));

    // Iterate over each entry in the transactions table that has a prepared
    // transaction.
    while (cursor->more()) {
        const auto txnRecordObj = cursor->next();
        const auto txnRecord = SessionTxnRecord::parse(
            IDLParserContext("recovering prepared transaction"), txnRecordObj);

        invariant(txnRecord.getState() == DurableTxnStateEnum::kPrepared);

        // Get the prepareTransaction oplog entry corresponding to this transactions
        // table entry.
        const auto prepareOpTime = txnRecord.getLastWriteOpTime();
        invariant(!prepareOpTime.isNull());
        TransactionHistoryIterator iter(prepareOpTime);
        invariant(iter.hasNext());
        auto prepareOplogEntry = iter.nextFatalOnErrors(opCtx);

        {
            // Make a new opCtx so that we can set the lsid when applying the prepare
            // transaction oplog entry.
            auto newClient = opCtx->getServiceContext()
                                 ->getService(ClusterRole::ShardServer)
                                 ->makeClient("reconstruct-prepared-transactions");

            AlternativeClientRegion acr(newClient);
            const auto newOpCtx = cc().makeOperationContext();

            // Ignore interruptions while reconstructing prepared transactions, so that we do not
            // fassert and crash due to interruptions inside this call.
            newOpCtx->runWithoutInterruptionExceptAtGlobalShutdown(
                [&] { _reconstructPreparedTransaction(newOpCtx.get(), prepareOplogEntry, mode); });
        }
    }
}
}  // namespace mongo
