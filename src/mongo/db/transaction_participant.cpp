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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kStorage

#define LOG_FOR_TRANSACTION(level) \
    MONGO_LOG_COMPONENT(level, ::mongo::logger::LogComponent::kTransaction)

#include "mongo/platform/basic.h"

#include "mongo/db/transaction_participant.h"

#include "mongo/db/catalog/index_catalog.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/commands/test_commands_enabled.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/concurrency/lock_state.h"
#include "mongo/db/concurrency/locker.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/curop_failpoint_helpers.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/index/index_access_method.h"
#include "mongo/db/op_observer.h"
#include "mongo/db/ops/update.h"
#include "mongo/db/query/get_executor.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/retryable_writes_stats.h"
#include "mongo/db/server_parameters.h"
#include "mongo/db/server_transactions_metrics.h"
#include "mongo/db/session.h"
#include "mongo/db/session_catalog.h"
#include "mongo/db/stats/fill_locker_info.h"
#include "mongo/db/transaction_history_iterator.h"
#include "mongo/util/fail_point_service.h"
#include "mongo/util/log.h"
#include "mongo/util/net/socket_utils.h"

namespace mongo {

// Server parameter that dictates the max number of milliseconds that any transaction lock request
// will wait for lock acquisition. If an operation provides a greater timeout in a lock request,
// maxTransactionLockRequestTimeoutMillis will override it. If this is set to a negative value, it
// is inactive and nothing will be overridden.
//
// 5 milliseconds will help avoid deadlocks, but will still allow fast-running metadata operations
// to run without aborting transactions.
MONGO_EXPORT_SERVER_PARAMETER(maxTransactionLockRequestTimeoutMillis, int, 5);

// Server parameter that dictates the lifetime given to each transaction.
// Transactions must eventually expire to preempt storage cache pressure immobilizing the system.
MONGO_EXPORT_SERVER_PARAMETER(transactionLifetimeLimitSeconds, std::int32_t, 60)
    ->withValidator([](const auto& potentialNewValue) {
        if (potentialNewValue < 1) {
            return Status(ErrorCodes::BadValue,
                          "transactionLifetimeLimitSeconds must be greater than or equal to 1s");
        }

        return Status::OK();
    });

namespace {

// Failpoint which will pause an operation just after allocating a point-in-time storage engine
// transaction.
MONGO_FAIL_POINT_DEFINE(hangAfterPreallocateSnapshot);

MONGO_FAIL_POINT_DEFINE(hangAfterReservingPrepareTimestamp);

MONGO_FAIL_POINT_DEFINE(hangAfterSettingPrepareStartTime);

MONGO_FAIL_POINT_DEFINE(hangBeforeReleasingTransactionOplogHole);

const auto getTransactionParticipant = Session::declareDecoration<TransactionParticipant>();

// The command names that are allowed in a prepared transaction.
const StringMap<int> preparedTxnCmdWhitelist = {
    {"abortTransaction", 1}, {"commitTransaction", 1}, {"prepareTransaction", 1}};

void fassertOnRepeatedExecution(const LogicalSessionId& lsid,
                                TxnNumber txnNumber,
                                StmtId stmtId,
                                const repl::OpTime& firstOpTime,
                                const repl::OpTime& secondOpTime) {
    severe() << "Statement id " << stmtId << " from transaction [ " << lsid.toBSON() << ":"
             << txnNumber << " ] was committed once with opTime " << firstOpTime
             << " and a second time with opTime " << secondOpTime
             << ". This indicates possible data corruption or server bug and the process will be "
                "terminated.";
    fassertFailed(40526);
}

struct ActiveTransactionHistory {
    boost::optional<SessionTxnRecord> lastTxnRecord;
    TransactionParticipant::CommittedStatementTimestampMap committedStatements;
    bool transactionCommitted{false};
    bool hasIncompleteHistory{false};
};

ActiveTransactionHistory fetchActiveTransactionHistory(OperationContext* opCtx,
                                                       const LogicalSessionId& lsid) {
    // Since we are using DBDirectClient to read the transactions table and the oplog, we should
    // never be reading from a snapshot, but directly from what is the latest on disk. This
    // invariant guards against programming errors where the default read concern on the
    // OperationContext could have been changed to something other than 'local'.
    invariant(repl::ReadConcernArgs::get(opCtx).getLevel() ==
              repl::ReadConcernLevel::kLocalReadConcern);

    ActiveTransactionHistory result;

    result.lastTxnRecord = [&]() -> boost::optional<SessionTxnRecord> {
        DBDirectClient client(opCtx);
        auto result =
            client.findOne(NamespaceString::kSessionTransactionsTableNamespace.ns(),
                           {BSON(SessionTxnRecord::kSessionIdFieldName << lsid.toBSON())});
        if (result.isEmpty()) {
            return boost::none;
        }

        return SessionTxnRecord::parse(IDLParserErrorContext("parse latest txn record for session"),
                                       result);
    }();

    if (!result.lastTxnRecord) {
        return result;
    }

    auto it = TransactionHistoryIterator(result.lastTxnRecord->getLastWriteOpTime());
    while (it.hasNext()) {
        try {
            const auto entry = it.next(opCtx);
            invariant(entry.getStatementId());

            if (*entry.getStatementId() == kIncompleteHistoryStmtId) {
                // Only the dead end sentinel can have this id for oplog write history
                invariant(entry.getObject2());
                invariant(entry.getObject2()->woCompare(TransactionParticipant::kDeadEndSentinel) ==
                          0);
                result.hasIncompleteHistory = true;
                continue;
            }

            const auto insertRes =
                result.committedStatements.emplace(*entry.getStatementId(), entry.getOpTime());
            if (!insertRes.second) {
                const auto& existingOpTime = insertRes.first->second;
                fassertOnRepeatedExecution(lsid,
                                           result.lastTxnRecord->getTxnNum(),
                                           *entry.getStatementId(),
                                           existingOpTime,
                                           entry.getOpTime());
            }

            // Either an applyOps oplog entry without a prepare flag or the state being kCommitted
            // marks the commit of a transaction.
            if ((entry.getCommandType() == repl::OplogEntry::CommandType::kApplyOps &&
                 !entry.shouldPrepare()) ||
                (result.lastTxnRecord->getState() == DurableTxnStateEnum::kCommitted)) {
                result.transactionCommitted = true;
            }
        } catch (const DBException& ex) {
            if (ex.code() == ErrorCodes::IncompleteTransactionHistory) {
                result.hasIncompleteHistory = true;
                break;
            }

            throw;
        }
    }

    return result;
}

void updateSessionEntry(OperationContext* opCtx, const UpdateRequest& updateRequest) {
    // Current code only supports replacement update.
    dassert(UpdateDriver::isDocReplacement(updateRequest.getUpdates()));

    AutoGetCollection autoColl(opCtx, NamespaceString::kSessionTransactionsTableNamespace, MODE_IX);

    uassert(40527,
            str::stream() << "Unable to persist transaction state because the session transaction "
                             "collection is missing. This indicates that the "
                          << NamespaceString::kSessionTransactionsTableNamespace.ns()
                          << " collection has been manually deleted.",
            autoColl.getCollection());

    WriteUnitOfWork wuow(opCtx);

    auto collection = autoColl.getCollection();
    auto idIndex = collection->getIndexCatalog()->findIdIndex(opCtx);

    uassert(40672,
            str::stream() << "Failed to fetch _id index for "
                          << NamespaceString::kSessionTransactionsTableNamespace.ns(),
            idIndex);

    auto indexAccess = collection->getIndexCatalog()->getEntry(idIndex)->accessMethod();
    // Since we are looking up a key inside the _id index, create a key object consisting of only
    // the _id field.
    auto idToFetch = updateRequest.getQuery().firstElement();
    auto toUpdateIdDoc = idToFetch.wrap();
    dassert(idToFetch.fieldNameStringData() == "_id"_sd);
    auto recordId = indexAccess->findSingle(opCtx, toUpdateIdDoc);
    auto startingSnapshotId = opCtx->recoveryUnit()->getSnapshotId();

    if (recordId.isNull()) {
        // Upsert case.
        auto status = collection->insertDocument(
            opCtx, InsertStatement(updateRequest.getUpdates()), nullptr, false);

        if (status == ErrorCodes::DuplicateKey) {
            throw WriteConflictException();
        }

        uassertStatusOK(status);
        wuow.commit();
        return;
    }

    auto originalRecordData = collection->getRecordStore()->dataFor(opCtx, recordId);
    auto originalDoc = originalRecordData.toBson();

    invariant(collection->getDefaultCollator() == nullptr);
    boost::intrusive_ptr<ExpressionContext> expCtx(new ExpressionContext(opCtx, nullptr));

    auto matcher =
        fassert(40673, MatchExpressionParser::parse(updateRequest.getQuery(), std::move(expCtx)));
    if (!matcher->matchesBSON(originalDoc)) {
        // Document no longer match what we expect so throw WCE to make the caller re-examine.
        throw WriteConflictException();
    }

    CollectionUpdateArgs args;
    args.update = updateRequest.getUpdates();
    args.criteria = toUpdateIdDoc;
    args.fromMigrate = false;

    collection->updateDocument(opCtx,
                               recordId,
                               Snapshotted<BSONObj>(startingSnapshotId, originalDoc),
                               updateRequest.getUpdates(),
                               false,  // indexesAffected = false because _id is the only index
                               nullptr,
                               &args);

    wuow.commit();
}

// Failpoint which allows different failure actions to happen after each write. Supports the
// parameters below, which can be combined with each other (unless explicitly disallowed):
//
// closeConnection (bool, default = true): Closes the connection on which the write was executed.
// failBeforeCommitExceptionCode (int, default = not specified): If set, the specified exception
//      code will be thrown, which will cause the write to not commit; if not specified, the write
//      will be allowed to commit.
MONGO_FAIL_POINT_DEFINE(onPrimaryTransactionalWrite);

}  // namespace

const BSONObj TransactionParticipant::kDeadEndSentinel(BSON("$incompleteOplogHistory" << 1));

TransactionParticipant::TransactionParticipant() = default;

TransactionParticipant::~TransactionParticipant() = default;

TransactionParticipant* TransactionParticipant::get(OperationContext* opCtx) {
    auto session = OperationContextSession::get(opCtx);
    if (!session) {
        return nullptr;
    }

    return get(session);
}

TransactionParticipant* TransactionParticipant::get(Session* session) {
    return &getTransactionParticipant(session);
}

void TransactionParticipant::performNoopWriteForNoSuchTransaction(OperationContext* opCtx) {
    repl::ReplicationCoordinator* replCoord =
        repl::ReplicationCoordinator::get(opCtx->getClient()->getServiceContext());

    // The locker must not have a max lock timeout when this noop write is performed, since if it
    // threw LockTimeout, this would be treated as a TransientTransactionError, which would indicate
    // it's resafe to retry the entire transaction. We cannot know it is safe to attach
    // TransientTransactionError until the noop write has been performed and the writeConcern has
    // been satisfied.
    invariant(!opCtx->lockState()->hasMaxLockTimeout());

    {
        Lock::DBLock dbLock(opCtx, "local", MODE_IX);
        Lock::CollectionLock collectionLock(opCtx->lockState(), "local.oplog.rs", MODE_IX);

        uassert(ErrorCodes::NotMaster,
                "Not primary when performing noop write for NoSuchTransaction error",
                replCoord->canAcceptWritesForDatabase(opCtx, "admin"));

        writeConflictRetry(
            opCtx, "performNoopWriteForNoSuchTransaction", "local.rs.oplog", [&opCtx] {
                WriteUnitOfWork wuow(opCtx);
                opCtx->getClient()->getServiceContext()->getOpObserver()->onOpMessage(
                    opCtx,
                    BSON("msg"
                         << "NoSuchTransaction"));
                wuow.commit();
            });
    }
}

const LogicalSessionId& TransactionParticipant::_sessionId() const {
    const auto* owningSession = getTransactionParticipant.owner(this);
    return owningSession->getSessionId();
}

OperationContext* TransactionParticipant::_opCtx() const {
    const auto* owningSession = getTransactionParticipant.owner(this);
    auto* opCtx = owningSession->currentOperation_forTest();
    invariant(opCtx);
    return opCtx;
}

void TransactionParticipant::_beginOrContinueRetryableWrite(WithLock wl, TxnNumber txnNumber) {
    if (txnNumber > _activeTxnNumber) {
        // New retryable write.
        _setNewTxnNumber(wl, txnNumber);
        _autoCommit = boost::none;
    } else {
        // Retrying a retryable write.
        uassert(ErrorCodes::InvalidOptions,
                "Must specify autocommit=false on all operations of a multi-statement transaction.",
                _txnState.isNone(wl));
        invariant(_autoCommit == boost::none);
    }
}

void TransactionParticipant::_continueMultiDocumentTransaction(WithLock wl, TxnNumber txnNumber) {
    uassert(ErrorCodes::NoSuchTransaction,
            str::stream()
                << "Given transaction number "
                << txnNumber
                << " does not match any in-progress transactions. The active transaction number is "
                << _activeTxnNumber,
            txnNumber == _activeTxnNumber && !_txnState.isNone(wl));

    if (_txnState.isInProgress(wl) && !_txnResourceStash) {
        // This indicates that the first command in the transaction failed but did not implicitly
        // abort the transaction. It is not safe to continue the transaction, in particular because
        // we have not saved the readConcern from the first statement of the transaction. Mark the
        // transaction as active here, since _abortTransactionOnSession() will assume we are
        // aborting an active transaction since there are no stashed resources.
        _transactionMetricsObserver.onUnstash(
            ServerTransactionsMetrics::get(getGlobalServiceContext()),
            getGlobalServiceContext()->getTickSource());
        _abortTransactionOnSession(wl);

        uasserted(ErrorCodes::NoSuchTransaction,
                  str::stream() << "Transaction " << txnNumber << " has been aborted.");
    }

    return;
}

void TransactionParticipant::_beginMultiDocumentTransaction(WithLock wl, TxnNumber txnNumber) {
    // Aborts any in-progress txns.
    _setNewTxnNumber(wl, txnNumber);
    _autoCommit = false;

    _txnState.transitionTo(wl, TransactionState::kInProgress);

    // Start tracking various transactions metrics.
    //
    // We measure the start time in both microsecond and millisecond resolution. The TickSource
    // provides microsecond resolution to record the duration of the transaction. The start "wall
    // clock" time can be considered an approximation to the microsecond measurement.
    auto now = getGlobalServiceContext()->getPreciseClockSource()->now();
    auto tickSource = getGlobalServiceContext()->getTickSource();

    _transactionExpireDate = now + Seconds(transactionLifetimeLimitSeconds.load());

    {
        stdx::lock_guard<stdx::mutex> lm(_metricsMutex);
        _transactionMetricsObserver.onStart(
            ServerTransactionsMetrics::get(getGlobalServiceContext()),
            *_autoCommit,
            tickSource,
            now,
            *_transactionExpireDate);
    }
    invariant(_transactionOperations.empty());
}

void TransactionParticipant::beginOrContinue(TxnNumber txnNumber,
                                             boost::optional<bool> autocommit,
                                             boost::optional<bool> startTransaction) {
    stdx::lock_guard<stdx::mutex> lg(_mutex);
    _checkValid(lg);

    uassert(ErrorCodes::TransactionTooOld,
            str::stream() << "Cannot start transaction " << txnNumber << " on session "
                          << _sessionId()
                          << " because a newer transaction "
                          << _activeTxnNumber
                          << " has already started.",
            txnNumber >= _activeTxnNumber);

    // Requests without an autocommit field are interpreted as retryable writes. They cannot specify
    // startTransaction, which is verified earlier when parsing the request.
    if (!autocommit) {
        invariant(!startTransaction);
        _beginOrContinueRetryableWrite(lg, txnNumber);
        return;
    }

    // Attempt to continue a multi-statement transaction. In this case, it is required that
    // autocommit be given as an argument on the request, and currently it can only be false, which
    // is verified earlier when parsing the request.
    invariant(*autocommit == false);

    if (!startTransaction) {
        _continueMultiDocumentTransaction(lg, txnNumber);
        return;
    }

    // Attempt to start a multi-statement transaction, which requires startTransaction be given as
    // an argument on the request. The 'startTransaction' argument currently can only be specified
    // as true, which is verified earlier, when parsing the request.
    invariant(*startTransaction);

    if (txnNumber == _activeTxnNumber) {
        // Servers in a sharded cluster can start a new transaction at the active transaction number
        // to allow internal retries by routers on re-targeting errors, like
        // StaleShard/DatabaseVersion or SnapshotTooOld.
        uassert(ErrorCodes::ConflictingOperationInProgress,
                "Only servers in a sharded cluster can start a new transaction at the active "
                "transaction number",
                serverGlobalParams.clusterRole != ClusterRole::None);

        // The active transaction number can only be reused if the transaction is aborted and has
        // not been involved in a two phase commit. Assuming routers target primaries in increasing
        // order of term and in the absence of byzantine messages, this check should never fail.
        const auto restartableStates = TransactionState::kAbortedWithoutPrepare;
        uassert(50911,
                str::stream() << "Cannot start a transaction at given transaction number "
                              << txnNumber
                              << " a transaction with the same number is in state "
                              << _txnState.toString(),
                _txnState.isInSet(lg, restartableStates));
    }

    _beginMultiDocumentTransaction(lg, txnNumber);
}

void TransactionParticipant::beginOrContinueTransactionUnconditionally(TxnNumber txnNumber) {
    stdx::lock_guard<stdx::mutex> lg(_mutex);

    // We don't check or fetch any on-disk state, so treat the transaction as 'valid' for the
    // purposes of this method and continue the transaction unconditionally
    _isValid = true;

    if (_activeTxnNumber != txnNumber) {
        _beginMultiDocumentTransaction(lg, txnNumber);
    }
}

void TransactionParticipant::_setSpeculativeTransactionOpTime(
    WithLock, OperationContext* opCtx, SpeculativeTransactionOpTime opTimeChoice) {
    repl::ReplicationCoordinator* replCoord =
        repl::ReplicationCoordinator::get(opCtx->getClient()->getServiceContext());
    opCtx->recoveryUnit()->setTimestampReadSource(
        opTimeChoice == SpeculativeTransactionOpTime::kAllCommitted
            ? RecoveryUnit::ReadSource::kAllCommittedSnapshot
            : RecoveryUnit::ReadSource::kLastAppliedSnapshot);
    opCtx->recoveryUnit()->preallocateSnapshot();
    auto readTimestamp = repl::StorageInterface::get(opCtx)->getPointInTimeReadTimestamp(opCtx);
    // Transactions do not survive term changes, so combining "getTerm" here with the
    // recovery unit timestamp does not cause races.
    _speculativeTransactionReadOpTime = {readTimestamp, replCoord->getTerm()};
    stdx::lock_guard<stdx::mutex> lm(_metricsMutex);
    _transactionMetricsObserver.onChooseReadTimestamp(readTimestamp);
}

void TransactionParticipant::_setSpeculativeTransactionReadTimestamp(WithLock,
                                                                     OperationContext* opCtx,
                                                                     Timestamp timestamp) {
    // Read concern code should have already set the timestamp on the recovery unit.
    invariant(timestamp == opCtx->recoveryUnit()->getPointInTimeReadTimestamp());

    repl::ReplicationCoordinator* replCoord =
        repl::ReplicationCoordinator::get(opCtx->getClient()->getServiceContext());
    opCtx->recoveryUnit()->preallocateSnapshot();
    _speculativeTransactionReadOpTime = {timestamp, replCoord->getTerm()};
    stdx::lock_guard<stdx::mutex> lm(_metricsMutex);
    _transactionMetricsObserver.onChooseReadTimestamp(timestamp);
}

TransactionParticipant::OplogSlotReserver::OplogSlotReserver(OperationContext* opCtx)
    : _opCtx(opCtx) {
    // Stash the transaction on the OperationContext on the stack. At the end of this function it
    // will be unstashed onto the OperationContext.
    TransactionParticipant::SideTransactionBlock sideTxn(opCtx);

    // Begin a new WUOW and reserve a slot in the oplog.
    WriteUnitOfWork wuow(opCtx);
    _oplogSlot = repl::getNextOpTime(opCtx);

    // Release the WUOW state since this WUOW is no longer in use.
    wuow.release();

    // We must lock the Client to change the Locker on the OperationContext.
    stdx::lock_guard<Client> lk(*opCtx->getClient());

    // The new transaction should have an empty locker, and thus we do not need to save it.
    invariant(opCtx->lockState()->getClientState() == Locker::ClientState::kInactive);
    _locker = opCtx->swapLockState(stdx::make_unique<LockerImpl>());
    // Inherit the locking setting from the original one.
    opCtx->lockState()->setShouldConflictWithSecondaryBatchApplication(
        _locker->shouldConflictWithSecondaryBatchApplication());
    _locker->unsetThreadId();
    if (opCtx->getLogicalSessionId()) {
        _locker->setDebugInfo("lsid: " + opCtx->getLogicalSessionId()->toBSON().toString());
    }

    // OplogSlotReserver is only used by primary, so always set max transaction lock timeout.
    invariant(opCtx->writesAreReplicated());
    // This thread must still respect the transaction lock timeout, since it can prevent the
    // transaction from making progress.
    auto maxTransactionLockMillis = maxTransactionLockRequestTimeoutMillis.load();
    if (maxTransactionLockMillis >= 0) {
        opCtx->lockState()->setMaxLockTimeout(Milliseconds(maxTransactionLockMillis));
    }

    // Save the RecoveryUnit from the new transaction and replace it with an empty one.
    _recoveryUnit = opCtx->releaseRecoveryUnit();
    opCtx->setRecoveryUnit(std::unique_ptr<RecoveryUnit>(
                               opCtx->getServiceContext()->getStorageEngine()->newRecoveryUnit()),
                           WriteUnitOfWork::RecoveryUnitState::kNotInUnitOfWork);
}

TransactionParticipant::OplogSlotReserver::~OplogSlotReserver() {
    if (MONGO_FAIL_POINT(hangBeforeReleasingTransactionOplogHole)) {
        log()
            << "transaction - hangBeforeReleasingTransactionOplogHole fail point enabled. Blocking "
               "until fail point is disabled.";
        MONGO_FAIL_POINT_PAUSE_WHILE_SET(hangBeforeReleasingTransactionOplogHole);
    }

    // If the constructor did not complete, we do not attempt to abort the units of work.
    if (_recoveryUnit) {
        // We should be at WUOW nesting level 1, only the top level WUOW for the oplog reservation
        // side transaction.
        _recoveryUnit->abortUnitOfWork();
        _locker->endWriteUnitOfWork();
        invariant(!_locker->inAWriteUnitOfWork());
    }

    // After releasing the oplog hole, the "all committed timestamp" can advance past
    // this oplog hole, if there are no other open holes. Check if we can advance the stable
    // timestamp any further since a majority write may be waiting on the stable timestamp to
    // advance beyond this oplog hole to acknowledge the write to the user.
    auto replCoord = repl::ReplicationCoordinator::get(_opCtx);
    replCoord->attemptToAdvanceStableTimestamp();
}

TransactionParticipant::TxnResources::TxnResources(OperationContext* opCtx, StashStyle stashStyle) {
    // We must lock the Client to change the Locker on the OperationContext.
    stdx::lock_guard<Client> lk(*opCtx->getClient());

    _ruState = opCtx->getWriteUnitOfWork()->release();
    opCtx->setWriteUnitOfWork(nullptr);

    _locker = opCtx->swapLockState(stdx::make_unique<LockerImpl>());
    // Inherit the locking setting from the original one.
    opCtx->lockState()->setShouldConflictWithSecondaryBatchApplication(
        _locker->shouldConflictWithSecondaryBatchApplication());
    if (stashStyle != StashStyle::kSideTransaction) {
        _locker->releaseTicket();
    }
    _locker->unsetThreadId();
    if (opCtx->getLogicalSessionId()) {
        _locker->setDebugInfo("lsid: " + opCtx->getLogicalSessionId()->toBSON().toString());
    }

    // On secondaries, we yield the locks for transactions.
    if (stashStyle == StashStyle::kSecondary) {
        _lockSnapshot = std::make_unique<Locker::LockSnapshot>();
        _locker->releaseWriteUnitOfWork(_lockSnapshot.get());
    }

    // This thread must still respect the transaction lock timeout, since it can prevent the
    // transaction from making progress.
    auto maxTransactionLockMillis = maxTransactionLockRequestTimeoutMillis.load();
    if (stashStyle != StashStyle::kSecondary && maxTransactionLockMillis >= 0) {
        opCtx->lockState()->setMaxLockTimeout(Milliseconds(maxTransactionLockMillis));
    }

    // On secondaries, max lock timeout must not be set.
    invariant(stashStyle != StashStyle::kSecondary || !opCtx->lockState()->hasMaxLockTimeout());

    _recoveryUnit = opCtx->releaseRecoveryUnit();
    opCtx->setRecoveryUnit(std::unique_ptr<RecoveryUnit>(
                               opCtx->getServiceContext()->getStorageEngine()->newRecoveryUnit()),
                           WriteUnitOfWork::RecoveryUnitState::kNotInUnitOfWork);

    _readConcernArgs = repl::ReadConcernArgs::get(opCtx);
}

TransactionParticipant::TxnResources::~TxnResources() {
    if (!_released && _recoveryUnit) {
        // This should only be reached when aborting a transaction that isn't active, i.e.
        // when starting a new transaction before completing an old one.  So we should
        // be at WUOW nesting level 1 (only the top level WriteUnitOfWork).
        _recoveryUnit->abortUnitOfWork();
        // If locks are not yielded, release them.
        if (!_lockSnapshot) {
            _locker->endWriteUnitOfWork();
        }
        invariant(!_locker->inAWriteUnitOfWork());
    }
}

void TransactionParticipant::TxnResources::release(OperationContext* opCtx) {
    // Perform operations that can fail the release before marking the TxnResources as released.

    // Restore locks if they are yielded.
    if (_lockSnapshot) {
        invariant(!_locker->isLocked());
        // opCtx is passed in to enable the restoration to be interrupted.
        _locker->restoreWriteUnitOfWork(opCtx, *_lockSnapshot);
        _lockSnapshot.reset(nullptr);
    }
    _locker->reacquireTicket(opCtx);

    invariant(!_released);
    _released = true;

    // It is necessary to lock the client to change the Locker on the OperationContext.
    stdx::lock_guard<Client> lk(*opCtx->getClient());
    invariant(opCtx->lockState()->getClientState() == Locker::ClientState::kInactive);
    // We intentionally do not capture the return value of swapLockState(), which is just an empty
    // locker. At the end of the operation, if the transaction is not complete, we will stash the
    // operation context's locker and replace it with a new empty locker.
    opCtx->swapLockState(std::move(_locker));
    opCtx->lockState()->updateThreadIdToCurrentThread();

    auto oldState = opCtx->setRecoveryUnit(std::move(_recoveryUnit),
                                           WriteUnitOfWork::RecoveryUnitState::kNotInUnitOfWork);
    invariant(oldState == WriteUnitOfWork::RecoveryUnitState::kNotInUnitOfWork,
              str::stream() << "RecoveryUnit state was " << oldState);

    opCtx->setWriteUnitOfWork(WriteUnitOfWork::createForSnapshotResume(opCtx, _ruState));

    auto& readConcernArgs = repl::ReadConcernArgs::get(opCtx);
    readConcernArgs = _readConcernArgs;
}

TransactionParticipant::SideTransactionBlock::SideTransactionBlock(OperationContext* opCtx)
    : _opCtx(opCtx) {
    if (_opCtx->getWriteUnitOfWork()) {
        _txnResources = TransactionParticipant::TxnResources(
            _opCtx, TxnResources::StashStyle::kSideTransaction);
    }
}

TransactionParticipant::SideTransactionBlock::~SideTransactionBlock() {
    if (_txnResources) {
        // Restore the transaction state onto '_opCtx'.
        _txnResources->release(_opCtx);
    }
}
void TransactionParticipant::_stashActiveTransaction(WithLock, OperationContext* opCtx) {
    if (_inShutdown) {
        return;
    }

    invariant(_activeTxnNumber == opCtx->getTxnNumber());
    {
        stdx::lock_guard<stdx::mutex> lm(_metricsMutex);
        auto tickSource = opCtx->getServiceContext()->getTickSource();
        _transactionMetricsObserver.onStash(ServerTransactionsMetrics::get(opCtx), tickSource);
        _transactionMetricsObserver.onTransactionOperation(
            opCtx->getClient(),
            CurOp::get(opCtx)->debug().additiveMetrics,
            CurOp::get(opCtx)->debug().storageStats);
    }

    invariant(!_txnResourceStash);
    auto stashStyle = opCtx->writesAreReplicated() ? TxnResources::StashStyle::kPrimary
                                                   : TxnResources::StashStyle::kSecondary;
    _txnResourceStash = TxnResources(opCtx, stashStyle);
}


void TransactionParticipant::stashTransactionResources(OperationContext* opCtx) {
    if (opCtx->getClient()->isInDirectClient()) {
        return;
    }

    invariant(opCtx->getTxnNumber());
    stdx::unique_lock<stdx::mutex> lg(_mutex);

    // Always check session's txnNumber, since it can be modified by migration, which does not
    // check out the session. We intentionally do not error if the transaction is aborted, since we
    // expect this function to be called at the end of the 'abortTransaction' command.
    _checkIsActiveTransaction(lg, *opCtx->getTxnNumber(), false);

    if (!_txnState.inMultiDocumentTransaction(lg)) {
        // Not in a multi-document transaction: nothing to do.
        return;
    }

    _stashActiveTransaction(lg, opCtx);
}

void TransactionParticipant::unstashTransactionResources(OperationContext* opCtx,
                                                         const std::string& cmdName) {
    invariant(!opCtx->getClient()->isInDirectClient());
    invariant(opCtx->getTxnNumber());

    {
        stdx::lock_guard<stdx::mutex> lg(_mutex);

        _checkValid(lg);
        _checkIsActiveTransaction(lg, *opCtx->getTxnNumber(), false);

        // If this is not a multi-document transaction, there is nothing to unstash.
        if (_txnState.isNone(lg)) {
            invariant(!_txnResourceStash);
            return;
        }

        _checkIsCommandValidWithTxnState(lg, *opCtx->getTxnNumber(), cmdName);

        if (_txnResourceStash) {
            // Transaction resources already exist for this transaction.  Transfer them from the
            // stash to the operation context.
            _txnResourceStash->release(opCtx);
            _txnResourceStash = boost::none;
            stdx::lock_guard<stdx::mutex> lm(_metricsMutex);
            _transactionMetricsObserver.onUnstash(ServerTransactionsMetrics::get(opCtx),
                                                  opCtx->getServiceContext()->getTickSource());
            return;
        }

        // If we have no transaction resources then we cannot be prepared. If we're not in progress,
        // we don't do anything else.
        invariant(!_txnState.isPrepared(lg));

        if (!_txnState.isInProgress(lg)) {
            // At this point we're either committed and this is a 'commitTransaction' command, or we
            // are in the process of committing.
            return;
        }

        // All locks of transactions must be acquired inside the global WUOW so that we can
        // yield and restore all locks on state transition. Otherwise, we'd have to remember
        // which locks are managed by WUOW.
        invariant(!opCtx->lockState()->isLocked());

        // Stashed transaction resources do not exist for this in-progress multi-document
        // transaction. Set up the transaction resources on the opCtx.
        opCtx->setWriteUnitOfWork(std::make_unique<WriteUnitOfWork>(opCtx));

        // If maxTransactionLockRequestTimeoutMillis is set, then we will ensure no
        // future lock request waits longer than maxTransactionLockRequestTimeoutMillis
        // to acquire a lock. This is to avoid deadlocks and minimize non-transaction
        // operation performance degradations.
        auto maxTransactionLockMillis = maxTransactionLockRequestTimeoutMillis.load();
        if (opCtx->writesAreReplicated() && maxTransactionLockMillis >= 0) {
            opCtx->lockState()->setMaxLockTimeout(Milliseconds(maxTransactionLockMillis));
        }

        // On secondaries, max lock timeout must not be set.
        invariant(opCtx->writesAreReplicated() || !opCtx->lockState()->hasMaxLockTimeout());
    }

    // Storage engine transactions may be started in a lazy manner. By explicitly
    // starting here we ensure that a point-in-time snapshot is established during the
    // first operation of a transaction.
    //
    // Active transactions are protected by the locking subsystem, so we must always hold at least a
    // Global intent lock before starting a transaction.  We pessimistically acquire an intent
    // exclusive lock here because we might be doing writes in this transaction, and it is currently
    // not deadlock-safe to upgrade IS to IX.
    Lock::GlobalLock(opCtx, MODE_IX);

    {
        // Set speculative execution.  This must be done after the global lock is acquired, because
        // we need to check that we are primary.
        stdx::lock_guard<stdx::mutex> lg(_mutex);
        const auto& readConcernArgs = repl::ReadConcernArgs::get(opCtx);
        // TODO(SERVER-38203): We cannot wait for write concern on secondaries, so we do not set the
        // speculative optime on secondaries either.  This means that reads done in transactions on
        // secondaries will not wait for the read snapshot to become majority-committed.
        repl::ReplicationCoordinator* replCoord =
            repl::ReplicationCoordinator::get(opCtx->getClient()->getServiceContext());
        if (replCoord->canAcceptWritesForDatabase(
                opCtx, NamespaceString::kSessionTransactionsTableNamespace.db())) {
            if (readConcernArgs.getArgsAtClusterTime()) {
                _setSpeculativeTransactionReadTimestamp(
                    lg, opCtx, readConcernArgs.getArgsAtClusterTime()->asTimestamp());
            } else {
                _setSpeculativeTransactionOpTime(
                    lg,
                    opCtx,
                    readConcernArgs.getOriginalLevel() ==
                            repl::ReadConcernLevel::kSnapshotReadConcern
                        ? SpeculativeTransactionOpTime::kAllCommitted
                        : SpeculativeTransactionOpTime::kLastApplied);
            }
        } else {
            opCtx->recoveryUnit()->preallocateSnapshot();
        }
    }

    // The Client lock must not be held when executing this failpoint as it will block currentOp
    // execution.
    if (MONGO_FAIL_POINT(hangAfterPreallocateSnapshot)) {
        CurOpFailpointHelpers::waitWhileFailPointEnabled(
            &hangAfterPreallocateSnapshot, opCtx, "hangAfterPreallocateSnapshot");
    }

    {
        stdx::lock_guard<stdx::mutex> lg(_mutex);
        stdx::lock_guard<stdx::mutex> lm(_metricsMutex);
        _transactionMetricsObserver.onUnstash(ServerTransactionsMetrics::get(opCtx),
                                              opCtx->getServiceContext()->getTickSource());
    }
}

void TransactionParticipant::refreshLocksForPreparedTransaction(OperationContext* opCtx,
                                                                bool yieldLocks) {
    // The opCtx will be used to swap locks, so it cannot hold any lock.
    invariant(!opCtx->lockState()->isRSTLLocked());
    invariant(!opCtx->lockState()->isLocked());

    stdx::unique_lock<stdx::mutex> lk(_mutex);

    // The node must have txn resource.
    invariant(_txnResourceStash);
    invariant(_txnState.isPrepared(lk));

    // Transfer the txn resource from the stash to the operation context.
    _txnResourceStash->release(opCtx);
    _txnResourceStash = boost::none;

    // Snapshot transactions don't conflict with PBWM lock on both primary and secondary.
    invariant(!opCtx->lockState()->shouldConflictWithSecondaryBatchApplication());

    // Transfer the txn resource back from the operation context to the stash.
    auto stashStyle =
        yieldLocks ? TxnResources::StashStyle::kSecondary : TxnResources::StashStyle::kPrimary;
    _txnResourceStash = TxnResources(opCtx, stashStyle);
}

Timestamp TransactionParticipant::prepareTransaction(OperationContext* opCtx,
                                                     boost::optional<repl::OpTime> prepareOptime) {
    stdx::unique_lock<stdx::mutex> lk(_mutex);

    // Always check session's txnNumber and '_txnState', since they can be modified by
    // session kill and migration, which do not check out the session.
    _checkIsActiveTransaction(lk, *opCtx->getTxnNumber(), true);

    auto abortGuard = makeGuard([&] {
        // Prepare transaction on secondaries should always succeed.
        invariant(!prepareOptime);

        if (lk.owns_lock()) {
            lk.unlock();
        }

        try {
            // This shouldn't cause deadlocks with other prepared txns, because the acquisition
            // of RSTL lock inside abortActiveTransaction will be no-op since we already have it.
            // This abortGuard gets dismissed before we release the RSTL while transitioning to
            // prepared.
            UninterruptibleLockGuard noInterrupt(opCtx->lockState());
            abortActiveTransaction(opCtx);
        } catch (...) {
            // It is illegal for aborting a prepared transaction to fail for any reason, so we crash
            // instead.
            severe() << "Caught exception during abort of prepared transaction "
                     << opCtx->getTxnNumber() << " on " << _sessionId().toBSON() << ": "
                     << exceptionToStatus();
            std::terminate();
        }
    });

    _txnState.transitionTo(lk, TransactionState::kPrepared);

    boost::optional<OplogSlotReserver> oplogSlotReserver;
    OplogSlot prepareOplogSlot;
    if (prepareOptime) {
        // On secondary, we just prepare the transaction and discard the buffered ops.
        prepareOplogSlot = OplogSlot(*prepareOptime, 0);
        _prepareOpTime = *prepareOptime;
    } else {
        // On primary, we reserve an optime, prepare the transaction and write the oplog entry.
        //
        // Reserve an optime for the 'prepareTimestamp'. This will create a hole in the oplog and
        // cause 'snapshot' and 'afterClusterTime' readers to block until this transaction is done
        // being prepared. When the OplogSlotReserver goes out of scope and is destroyed, the
        // storage-transaction it uses to keep the hole open will abort and the slot (and
        // corresponding oplog hole) will vanish.
        oplogSlotReserver.emplace(opCtx);
        prepareOplogSlot = oplogSlotReserver->getReservedOplogSlot();
        invariant(_prepareOpTime.isNull(),
                  str::stream() << "This transaction has already reserved a prepareOpTime at: "
                                << _prepareOpTime.toString());
        _prepareOpTime = prepareOplogSlot.opTime;

        if (MONGO_FAIL_POINT(hangAfterReservingPrepareTimestamp)) {
            // This log output is used in js tests so please leave it.
            log() << "transaction - hangAfterReservingPrepareTimestamp fail point "
                     "enabled. Blocking until fail point is disabled. Prepare OpTime: "
                  << prepareOplogSlot.opTime;
            MONGO_FAIL_POINT_PAUSE_WHILE_SET(hangAfterReservingPrepareTimestamp);
        }
    }
    opCtx->recoveryUnit()->setPrepareTimestamp(prepareOplogSlot.opTime.getTimestamp());
    opCtx->getWriteUnitOfWork()->prepare();

    // We need to unlock the session to run the opObserver onTransactionPrepare, which calls back
    // into the session.
    lk.unlock();
    opCtx->getServiceContext()->getOpObserver()->onTransactionPrepare(
        opCtx, prepareOplogSlot, retrieveCompletedTransactionOperations(opCtx));

    abortGuard.dismiss();

    invariant(!_oldestOplogEntryOpTime,
              str::stream() << "This transaction's oldest oplog entry Timestamp has already "
                            << "been set to: "
                            << _oldestOplogEntryOpTime->toString());
    // Keep track of the Timestamp from the first oplog entry written by this transaction.
    _oldestOplogEntryOpTime = prepareOplogSlot.opTime;

    // Maintain the OpTime of the oldest active oplog entry for this transaction. We currently
    // only write an oplog entry for an in progress transaction when it is in the prepare state
    // but this will change when we allow multiple oplog entries per transaction.
    {
        stdx::lock_guard<stdx::mutex> lm(_metricsMutex);
        const auto tickSource = getGlobalServiceContext()->getTickSource();
        _transactionMetricsObserver.onPrepare(ServerTransactionsMetrics::get(opCtx),
                                              *_oldestOplogEntryOpTime,
                                              tickSource->getTicks());
    }

    if (MONGO_FAIL_POINT(hangAfterSettingPrepareStartTime)) {
        log() << "transaction - hangAfterSettingPrepareStartTime fail point enabled. Blocking "
                 "until fail point is disabled.";
        MONGO_FAIL_POINT_PAUSE_WHILE_SET(hangAfterSettingPrepareStartTime);
    }

    // We unlock the RSTL to allow prepared transactions to survive state transitions. This should
    // be the last thing we do since a state transition may happen immediately after releasing the
    // RSTL.
    const bool unlocked = opCtx->lockState()->unlockRSTLforPrepare();
    invariant(unlocked);

    return prepareOplogSlot.opTime.getTimestamp();
}

void TransactionParticipant::addTransactionOperation(OperationContext* opCtx,
                                                     const repl::ReplOperation& operation) {
    stdx::lock_guard<stdx::mutex> lk(_mutex);

    // Always check _getSession()'s txnNumber and '_txnState', since they can be modified by session
    // kill and migration, which do not check out the session.
    _checkIsActiveTransaction(lk, *opCtx->getTxnNumber(), true);

    // Ensure that we only ever add operations to an in progress transaction.
    invariant(_txnState.isInProgress(lk), str::stream() << "Current state: " << _txnState);

    invariant(_autoCommit && !*_autoCommit && _activeTxnNumber != kUninitializedTxnNumber);
    invariant(opCtx->lockState()->inAWriteUnitOfWork());
    _transactionOperations.push_back(operation);
    _transactionOperationBytes += repl::OplogEntry::getReplOperationSize(operation);
    // _transactionOperationBytes is based on the in-memory size of the operation.  With overhead,
    // we expect the BSON size of the operation to be larger, so it's possible to make a transaction
    // just a bit too large and have it fail only in the commit.  It's still useful to fail early
    // when possible (e.g. to avoid exhausting server memory).
    uassert(ErrorCodes::TransactionTooLarge,
            str::stream() << "Total size of all transaction operations must be less than "
                          << BSONObjMaxInternalSize
                          << ". Actual size is "
                          << _transactionOperationBytes,
            _transactionOperationBytes <= BSONObjMaxInternalSize);
}

std::vector<repl::ReplOperation>& TransactionParticipant::retrieveCompletedTransactionOperations(
    OperationContext* opCtx) {
    stdx::lock_guard<stdx::mutex> lk(_mutex);

    // Always check session's txnNumber and '_txnState', since they can be modified by session kill
    // and migration, which do not check out the session.
    _checkIsActiveTransaction(lk, *opCtx->getTxnNumber(), true);

    // Ensure that we only ever retrieve a transaction's completed operations when in progress,
    // committing with prepare, or prepared.
    invariant(_txnState.isInSet(lk,
                                TransactionState::kInProgress |
                                    TransactionState::kCommittingWithPrepare |
                                    TransactionState::kPrepared),
              str::stream() << "Current state: " << _txnState);

    return _transactionOperations;
}

void TransactionParticipant::clearOperationsInMemory(OperationContext* opCtx) {
    stdx::lock_guard<stdx::mutex> lk(_mutex);

    // Always check session's txnNumber and '_txnState', since they can be modified by session kill
    // and migration, which do not check out the session.
    _checkIsActiveTransaction(lk, *opCtx->getTxnNumber(), true);

    // Ensure that we only ever end a transaction when committing with prepare or in progress.
    invariant(_txnState.isInSet(
                  lk, TransactionState::kCommittingWithPrepare | TransactionState::kInProgress),
              str::stream() << "Current state: " << _txnState);

    invariant(_autoCommit);
    _transactionOperationBytes = 0;
    _transactionOperations.clear();
}

void TransactionParticipant::commitUnpreparedTransaction(OperationContext* opCtx) {
    stdx::unique_lock<stdx::mutex> lk(_mutex);
    _checkIsActiveTransaction(lk, *opCtx->getTxnNumber(), true);

    uassert(ErrorCodes::InvalidOptions,
            "commitTransaction must provide commitTimestamp to prepared transaction.",
            !_txnState.isPrepared(lk));

    // TODO SERVER-37129: Remove this invariant once we allow transactions larger than 16MB.
    invariant(!_oldestOplogEntryOpTime,
              str::stream() << "The oldest oplog entry Timestamp should not have been set because "
                            << "this transaction is not prepared. But, it is currently "
                            << _oldestOplogEntryOpTime->toString());

    // We need to unlock the session to run the opObserver onTransactionCommit, which calls back
    // into the session.
    lk.unlock();
    auto opObserver = opCtx->getServiceContext()->getOpObserver();
    invariant(opObserver);
    opObserver->onTransactionCommit(
        opCtx, boost::none, boost::none, retrieveCompletedTransactionOperations(opCtx));

    clearOperationsInMemory(opCtx);

    lk.lock();
    _checkIsActiveTransaction(lk, *opCtx->getTxnNumber(), true);

    // The oplog entry is written in the same WUOW with the data change for unprepared transactions.
    // We can still consider the state is InProgress until now, since no externally visible changes
    // have been made yet by the commit operation. If anything throws before this point in the
    // function, entry point will abort the transaction.
    _txnState.transitionTo(lk, TransactionState::kCommittingWithoutPrepare);

    lk.unlock();
    _commitStorageTransaction(opCtx);
    lk.lock();
    _checkIsActiveTransaction(lk, *opCtx->getTxnNumber(), false);
    invariant(_txnState.isCommittingWithoutPrepare(lk),
              str::stream() << "Current State: " << _txnState);

    _finishCommitTransaction(lk, opCtx);
}

void TransactionParticipant::commitPreparedTransaction(
    OperationContext* opCtx,
    Timestamp commitTimestamp,
    boost::optional<repl::OpTime> commitOplogEntryOpTime) {
    // Re-acquire the RSTL to prevent state transitions while committing the transaction. When the
    // transaction was prepared, we dropped the RSTL. We do not need to reacquire the PBWM because
    // if we're not the primary we will uassert anyways.
    Lock::ResourceLock rstl(opCtx->lockState(), resourceIdReplicationStateTransitionLock, MODE_IX);
    if (opCtx->writesAreReplicated()) {
        auto replCoord = repl::ReplicationCoordinator::get(opCtx);
        uassert(ErrorCodes::NotMaster,
                "Not primary so we cannot commit a prepared transaction",
                replCoord->canAcceptWritesForDatabase(opCtx, "admin"));
    }

    stdx::unique_lock<stdx::mutex> lk(_mutex);
    _checkIsActiveTransaction(lk, *opCtx->getTxnNumber(), true);

    uassert(ErrorCodes::InvalidOptions,
            "commitTransaction cannot provide commitTimestamp to unprepared transaction.",
            _txnState.isPrepared(lk));
    uassert(
        ErrorCodes::InvalidOptions, "'commitTimestamp' cannot be null", !commitTimestamp.isNull());
    uassert(ErrorCodes::InvalidOptions,
            "'commitTimestamp' must be greater than the 'prepareTimestamp'",
            commitTimestamp > _prepareOpTime.getTimestamp());

    _txnState.transitionTo(lk, TransactionState::kCommittingWithPrepare);
    opCtx->recoveryUnit()->setCommitTimestamp(commitTimestamp);

    try {
        UninterruptibleLockGuard noInterrupt(opCtx->lockState());

        // On secondary, we generate a fake empty oplog slot, since it's not used by opObserver.
        OplogSlot commitOplogSlot;
        boost::optional<OplogSlotReserver> oplogSlotReserver;

        // On primary, we reserve an oplog slot before committing the transaction so that no
        // writes that are causally related to the transaction commit enter the oplog at a
        // timestamp earlier than the commit oplog entry.
        if (opCtx->writesAreReplicated()) {
            invariant(!commitOplogEntryOpTime);
            oplogSlotReserver.emplace(opCtx);
            commitOplogSlot = oplogSlotReserver->getReservedOplogSlot();
            invariant(commitOplogSlot.opTime.getTimestamp() >= commitTimestamp,
                      str::stream() << "Commit oplog entry must be greater than or equal to commit "
                                       "timestamp due to causal consistency. commit timestamp: "
                                    << commitTimestamp.toBSON()
                                    << ", commit oplog entry optime: "
                                    << commitOplogSlot.opTime.toBSON());
        } else {
            // We always expect a non-null commitOplogEntryOpTime to be passed in on secondaries
            // in order to set the finishOpTime.
            invariant(commitOplogEntryOpTime);
        }

        // We need to unlock the session to run the opObserver onTransactionCommit, which calls back
        // into the session. We also do not want to write to storage with the mutex locked.
        lk.unlock();
        _commitStorageTransaction(opCtx);

        auto opObserver = opCtx->getServiceContext()->getOpObserver();
        invariant(opObserver);

        {
            // Once the transaction is committed, the oplog entry must be written.
            UninterruptibleLockGuard lockGuard(opCtx->lockState());
            opObserver->onTransactionCommit(opCtx,
                                            commitOplogSlot,
                                            commitTimestamp,
                                            retrieveCompletedTransactionOperations(opCtx));
        }

        clearOperationsInMemory(opCtx);

        lk.lock();
        _checkIsActiveTransaction(lk, *opCtx->getTxnNumber(), true);

        // If we are committing a prepared transaction, then we must have already recorded this
        // transaction's oldest oplog entry optime.
        invariant(_oldestOplogEntryOpTime);
        // If commitOplogEntryOpTime is a nullopt, then we grab the OpTime from the commitOplogSlot
        // which will only be set if we are primary. Otherwise, the commitOplogEntryOpTime must have
        // been passed in during secondary oplog application.
        _finishOpTime = commitOplogEntryOpTime.value_or(commitOplogSlot.opTime);

        _finishCommitTransaction(lk, opCtx);
    } catch (...) {
        // It is illegal for committing a prepared transaction to fail for any reason, other than an
        // invalid command, so we crash instead.
        severe() << "Caught exception during commit of prepared transaction "
                 << opCtx->getTxnNumber() << " on " << _sessionId().toBSON() << ": "
                 << exceptionToStatus();
        std::terminate();
    }
}

void TransactionParticipant::_commitStorageTransaction(OperationContext* opCtx) try {
    invariant(opCtx->getWriteUnitOfWork());
    invariant(opCtx->lockState()->isRSTLLocked());
    opCtx->getWriteUnitOfWork()->commit();
    opCtx->setWriteUnitOfWork(nullptr);

    // We must clear the recovery unit and locker for the 'config.transactions' and oplog entry
    // writes.
    opCtx->setRecoveryUnit(std::unique_ptr<RecoveryUnit>(
                               opCtx->getServiceContext()->getStorageEngine()->newRecoveryUnit()),
                           WriteUnitOfWork::RecoveryUnitState::kNotInUnitOfWork);

    opCtx->lockState()->unsetMaxLockTimeout();
} catch (...) {
    // It is illegal for committing a storage-transaction to fail so we crash instead.
    severe() << "Caught exception during commit of storage-transaction " << opCtx->getTxnNumber()
             << " on " << _sessionId().toBSON() << ": " << exceptionToStatus();
    std::terminate();
}

void TransactionParticipant::_finishCommitTransaction(WithLock lk, OperationContext* opCtx) {
    // If no writes have been done, set the client optime forward to the read timestamp so waiting
    // for write concern will ensure all read data was committed.
    //
    // TODO(SERVER-34881): Once the default read concern is speculative majority, only set the
    // client optime forward if the original read concern level is "majority" or "snapshot".
    auto& clientInfo = repl::ReplClientInfo::forClient(opCtx->getClient());
    if (_speculativeTransactionReadOpTime > clientInfo.getLastOp()) {
        clientInfo.setLastOp(_speculativeTransactionReadOpTime);
    }
    const bool isCommittingWithPrepare = _txnState.isCommittingWithPrepare(lk);
    _txnState.transitionTo(lk, TransactionState::kCommitted);

    {
        stdx::lock_guard<stdx::mutex> lm(_metricsMutex);
        auto tickSource = opCtx->getServiceContext()->getTickSource();
        _transactionMetricsObserver.onCommit(ServerTransactionsMetrics::get(opCtx),
                                             tickSource,
                                             _oldestOplogEntryOpTime,
                                             _finishOpTime,
                                             &Top::get(getGlobalServiceContext()),
                                             isCommittingWithPrepare);
        _transactionMetricsObserver.onTransactionOperation(
            opCtx->getClient(),
            CurOp::get(opCtx)->debug().additiveMetrics,
            CurOp::get(opCtx)->debug().storageStats);
    }

    // We must clear the recovery unit and locker so any post-transaction writes can run without
    // transactional settings such as a read timestamp.
    _cleanUpTxnResourceOnOpCtx(lk, opCtx, TerminationCause::kCommitted);
}

void TransactionParticipant::shutdown() {
    stdx::lock_guard<stdx::mutex> lock(_mutex);

    _inShutdown = true;
    _txnResourceStash = boost::none;
}

void TransactionParticipant::abortArbitraryTransaction() {
    stdx::lock_guard<stdx::mutex> lock(_mutex);

    if (!_txnState.isInProgress(lock)) {
        // We do not want to abort transactions that are prepared unless we get an
        // 'abortTransaction' command.
        return;
    }

    _abortTransactionOnSession(lock);
}

bool TransactionParticipant::expired() const {
    stdx::lock_guard<stdx::mutex> lock(_mutex);

    return _txnState.isInProgress(lock) && _transactionExpireDate &&
        _transactionExpireDate < getGlobalServiceContext()->getPreciseClockSource()->now();
}

void TransactionParticipant::abortActiveTransaction(OperationContext* opCtx) {
    stdx::unique_lock<stdx::mutex> lock(_mutex);

    // Re-acquire the RSTL to prevent state transitions while aborting the transaction. If the
    // transaction was prepared then we dropped it on preparing the transaction. We do not need to
    // reacquire the PBWM because if we're not the primary we will uassert anyways.
    Lock::ResourceLock rstl(opCtx->lockState(), resourceIdReplicationStateTransitionLock, MODE_IX);
    if (_txnState.isPrepared(lock) && opCtx->writesAreReplicated()) {
        auto replCoord = repl::ReplicationCoordinator::get(opCtx);
        uassert(ErrorCodes::NotMaster,
                "Not primary so we cannot abort a prepared transaction",
                replCoord->canAcceptWritesForDatabase(opCtx, "admin"));
    }

    // This function shouldn't throw if the transaction is already aborted.
    _checkIsActiveTransaction(lock, *opCtx->getTxnNumber(), false);
    _abortActiveTransaction(
        std::move(lock), opCtx, TransactionState::kInProgress | TransactionState::kPrepared);
}

void TransactionParticipant::abortActiveUnpreparedOrStashPreparedTransaction(
    OperationContext* opCtx) try {
    stdx::unique_lock<stdx::mutex> lock(_mutex);
    if (_txnState.isInSet(lock, TransactionState::kNone | TransactionState::kCommitted)) {
        // If there is no active transaction, do nothing.
        return;
    }

    // We do this check to follow convention and maintain safety. If this were to throw we should
    // have returned in the check above. As a result, throwing here is fatal.
    _checkIsActiveTransaction(lock, *opCtx->getTxnNumber(), false);

    // Stash the transaction if it's in prepared state.
    if (_txnState.isInSet(lock, TransactionState::kPrepared)) {
        _stashActiveTransaction(lock, opCtx);
        return;
    }

    // TODO SERVER-37129: Remove this invariant once we allow transactions larger than 16MB.
    invariant(!_oldestOplogEntryOpTime,
              str::stream() << "The oldest oplog entry Timestamp should not have been set because "
                            << "this transaction is not prepared. But, it is currently "
                            << _oldestOplogEntryOpTime->toString());

    _abortActiveTransaction(std::move(lock), opCtx, TransactionState::kInProgress);
} catch (...) {
    // It is illegal for this to throw so we catch and log this here for diagnosability.
    severe() << "Caught exception during transaction " << opCtx->getTxnNumber()
             << " abort or stash on " << _sessionId().toBSON() << " in state " << _txnState << ": "
             << exceptionToStatus();
    std::terminate();
}

void TransactionParticipant::_abortActiveTransaction(stdx::unique_lock<stdx::mutex> lock,
                                                     OperationContext* opCtx,
                                                     TransactionState::StateSet expectedStates) {
    invariant(!_txnResourceStash);
    invariant(!_txnState.isCommittingWithPrepare(lock));

    if (!_txnState.isNone(lock)) {
        stdx::lock_guard<stdx::mutex> lm(_metricsMutex);
        _transactionMetricsObserver.onTransactionOperation(
            opCtx->getClient(),
            CurOp::get(opCtx)->debug().additiveMetrics,
            CurOp::get(opCtx)->debug().storageStats);
    }

    // We reserve an oplog slot before aborting the transaction so that no writes that are causally
    // related to the transaction abort enter the oplog at a timestamp earlier than the abort oplog
    // entry. On secondaries, we generate a fake empty oplog slot, since it's not used by the
    // OpObserver.
    boost::optional<OplogSlotReserver> oplogSlotReserver;
    boost::optional<OplogSlot> abortOplogSlot;
    if (_txnState.isPrepared(lock) && opCtx->writesAreReplicated()) {
        oplogSlotReserver.emplace(opCtx);
        abortOplogSlot = oplogSlotReserver->getReservedOplogSlot();
    }

    // Clean up the transaction resources on the opCtx even if the transaction resources on the
    // session were not aborted. This actually aborts the storage-transaction.
    _cleanUpTxnResourceOnOpCtx(lock, opCtx, TerminationCause::kAborted);

    // Write the abort oplog entry. This must be done after aborting the storage transaction, so
    // that the lock state is reset, and there is no max lock timeout on the locker. We need to
    // unlock the session to run the opObserver onTransactionAbort, which calls back into the
    // session.
    lock.unlock();

    auto opObserver = opCtx->getServiceContext()->getOpObserver();
    invariant(opObserver);
    opObserver->onTransactionAbort(opCtx, abortOplogSlot);

    lock.lock();
    // We do not check if the active transaction number is correct here because we handle it below.

    // Set the finishOpTime of this transaction if we have recorded this transaction's oldest oplog
    // entry optime.
    if (_oldestOplogEntryOpTime) {
        _finishOpTime = repl::ReplClientInfo::forClient(opCtx->getClient()).getLastOp();
    }

    // Only abort the transaction in session if it's in expected states.
    // When the state of active transaction on session is not expected, it means another
    // thread has already aborted the transaction on session.
    if (_txnState.isInSet(lock, expectedStates)) {
        invariant(opCtx->getTxnNumber() == _activeTxnNumber);
        _abortTransactionOnSession(lock);
    } else if (opCtx->getTxnNumber() == _activeTxnNumber) {
        if (_txnState.isNone(lock)) {
            // The active transaction is not a multi-document transaction.
            invariant(opCtx->getWriteUnitOfWork() == nullptr);
            return;
        }

        // Cannot abort these states unless they are specified in expectedStates explicitly.
        const auto unabortableStates = TransactionState::kPrepared  //
            | TransactionState::kCommittingWithPrepare              //
            | TransactionState::kCommittingWithoutPrepare           //
            | TransactionState::kCommitted;                         //
        invariant(!_txnState.isInSet(lock, unabortableStates),
                  str::stream() << "Cannot abort transaction in " << _txnState.toString());
    } else {
        // If _activeTxnNumber is higher than ours, it means the transaction is already aborted.
        invariant(_txnState.isInSet(lock,
                                    TransactionState::kNone |
                                        TransactionState::kAbortedWithoutPrepare |
                                        TransactionState::kAbortedWithPrepare));
    }
}

void TransactionParticipant::_abortTransactionOnSession(WithLock wl) {
    const auto tickSource = getGlobalServiceContext()->getTickSource();
    // If the transaction is stashed, then we have aborted an inactive transaction.
    if (_txnResourceStash) {
        // The transaction is stashed, so we abort the inactive transaction on session.
        {
            stdx::lock_guard<stdx::mutex> lm(_metricsMutex);
            _transactionMetricsObserver.onAbortInactive(
                ServerTransactionsMetrics::get(getGlobalServiceContext()),
                tickSource,
                _oldestOplogEntryOpTime,
                &Top::get(getGlobalServiceContext()));
        }
        _logSlowTransaction(wl,
                            &(_txnResourceStash->locker()->getLockerInfo(boost::none))->stats,
                            TerminationCause::kAborted,
                            _txnResourceStash->getReadConcernArgs());
    } else {
        stdx::lock_guard<stdx::mutex> lm(_metricsMutex);
        _transactionMetricsObserver.onAbortActive(
            ServerTransactionsMetrics::get(getGlobalServiceContext()),
            tickSource,
            _oldestOplogEntryOpTime,
            _finishOpTime,
            &Top::get(getGlobalServiceContext()),
            _txnState.isPrepared(lm));
    }

    const auto nextState = _txnState.isPrepared(wl) ? TransactionState::kAbortedWithPrepare
                                                    : TransactionState::kAbortedWithoutPrepare;
    _resetTransactionState(wl, nextState);
}

void TransactionParticipant::_cleanUpTxnResourceOnOpCtx(WithLock wl,
                                                        OperationContext* opCtx,
                                                        TerminationCause terminationCause) {
    // Log the transaction if its duration is longer than the slowMS command threshold.
    _logSlowTransaction(
        wl,
        &(opCtx->lockState()->getLockerInfo(CurOp::get(*opCtx)->getLockStatsBase()))->stats,
        terminationCause,
        repl::ReadConcernArgs::get(opCtx));

    // Reset the WUOW. We should be able to abort empty transactions that don't have WUOW.
    if (opCtx->getWriteUnitOfWork()) {
        invariant(opCtx->lockState()->isRSTLLocked());
        opCtx->setWriteUnitOfWork(nullptr);
    }

    // We must clear the recovery unit and locker so any post-transaction writes can run without
    // transactional settings such as a read timestamp.
    opCtx->setRecoveryUnit(std::unique_ptr<RecoveryUnit>(
                               opCtx->getServiceContext()->getStorageEngine()->newRecoveryUnit()),
                           WriteUnitOfWork::RecoveryUnitState::kNotInUnitOfWork);

    opCtx->lockState()->unsetMaxLockTimeout();
}

void TransactionParticipant::_checkIsActiveTransaction(WithLock wl,
                                                       const TxnNumber& requestTxnNumber,
                                                       bool checkAbort) const {
    uassert(ErrorCodes::ConflictingOperationInProgress,
            str::stream() << "Cannot perform operations on requested transaction "
                          << requestTxnNumber
                          << " on session "
                          << _sessionId()
                          << " because a different transaction "
                          << _activeTxnNumber
                          << " is now active.",
            requestTxnNumber == _activeTxnNumber);

    uassert(ErrorCodes::NoSuchTransaction,
            str::stream() << "Transaction " << _activeTxnNumber << " has been aborted.",
            !checkAbort || !_txnState.isAborted(wl));
}

void TransactionParticipant::_checkIsCommandValidWithTxnState(WithLock wl,
                                                              const TxnNumber& requestTxnNumber,
                                                              const std::string& cmdName) {
    // Throw NoSuchTransaction error instead of TransactionAborted error since this is the entry
    // point of transaction execution.
    uassert(ErrorCodes::NoSuchTransaction,
            str::stream() << "Transaction " << requestTxnNumber << " has been aborted.",
            !_txnState.isAborted(wl));

    // Cannot change committed transaction but allow retrying commitTransaction command.
    uassert(ErrorCodes::TransactionCommitted,
            str::stream() << "Transaction " << requestTxnNumber << " has been committed.",
            cmdName == "commitTransaction" || !_txnState.isCommitted(wl));

    // Disallow operations other than abort, prepare or commit on a prepared transaction
    uassert(ErrorCodes::PreparedTransactionInProgress,
            str::stream() << "Cannot call any operation other than abort, prepare or commit on"
                          << " a prepared transaction",
            !_txnState.isPrepared(wl) ||
                preparedTxnCmdWhitelist.find(cmdName) != preparedTxnCmdWhitelist.cend());
}

BSONObj TransactionParticipant::reportStashedState() const {
    BSONObjBuilder builder;
    reportStashedState(&builder);
    return builder.obj();
}

void TransactionParticipant::reportStashedState(BSONObjBuilder* builder) const {
    stdx::lock_guard<stdx::mutex> lm(_mutex);

    if (_txnResourceStash && _txnResourceStash->locker()) {
        if (auto lockerInfo = _txnResourceStash->locker()->getLockerInfo(boost::none)) {
            invariant(_activeTxnNumber != kUninitializedTxnNumber);
            builder->append("type", "idleSession");
            builder->append("host", getHostNameCachedAndPort());
            builder->append("desc", "inactive transaction");

            const auto& lastClientInfo =
                _transactionMetricsObserver.getSingleTransactionStats().getLastClientInfo();
            builder->append("client", lastClientInfo.clientHostAndPort);
            builder->append("connectionId", lastClientInfo.connectionId);
            builder->append("appName", lastClientInfo.appName);
            builder->append("clientMetadata", lastClientInfo.clientMetadata);

            {
                BSONObjBuilder lsid(builder->subobjStart("lsid"));
                _sessionId().serialize(&lsid);
            }

            BSONObjBuilder transactionBuilder;
            _reportTransactionStats(
                lm, &transactionBuilder, _txnResourceStash->getReadConcernArgs());

            builder->append("transaction", transactionBuilder.obj());
            builder->append("waitingForLock", false);
            builder->append("active", false);

            fillLockerInfo(*lockerInfo, *builder);
        }
    }
}

void TransactionParticipant::reportUnstashedState(OperationContext* opCtx,
                                                  BSONObjBuilder* builder) const {
    // This method may only take the metrics mutex, as it is called with the Client mutex held.  So
    // we cannot check the stashed state directly.  Instead, a transaction is considered unstashed
    // if it is not actually a transaction (retryable write, no stash used), or is active (not
    // stashed), or has ended (any stash would be cleared).

    stdx::lock_guard<stdx::mutex> lm(_metricsMutex);
    const auto& singleTransactionStats = _transactionMetricsObserver.getSingleTransactionStats();
    if (!singleTransactionStats.isForMultiDocumentTransaction() ||
        singleTransactionStats.isActive() || singleTransactionStats.isEnded()) {
        BSONObjBuilder transactionBuilder;
        _reportTransactionStats(lm, &transactionBuilder, repl::ReadConcernArgs::get(opCtx));
        builder->append("transaction", transactionBuilder.obj());
    }
}

std::string TransactionParticipant::TransactionState::toString(StateFlag state) {
    switch (state) {
        case TransactionParticipant::TransactionState::kNone:
            return "TxnState::None";
        case TransactionParticipant::TransactionState::kInProgress:
            return "TxnState::InProgress";
        case TransactionParticipant::TransactionState::kPrepared:
            return "TxnState::Prepared";
        case TransactionParticipant::TransactionState::kCommittingWithoutPrepare:
            return "TxnState::CommittingWithoutPrepare";
        case TransactionParticipant::TransactionState::kCommittingWithPrepare:
            return "TxnState::CommittingWithPrepare";
        case TransactionParticipant::TransactionState::kCommitted:
            return "TxnState::Committed";
        case TransactionParticipant::TransactionState::kAbortedWithoutPrepare:
            return "TxnState::AbortedWithoutPrepare";
        case TransactionParticipant::TransactionState::kAbortedWithPrepare:
            return "TxnState::AbortedAfterPrepare";
    }
    MONGO_UNREACHABLE;
}

bool TransactionParticipant::TransactionState::_isLegalTransition(StateFlag oldState,
                                                                  StateFlag newState) {
    switch (oldState) {
        case kNone:
            switch (newState) {
                case kNone:
                case kInProgress:
                    return true;
                default:
                    return false;
            }
            MONGO_UNREACHABLE;
        case kInProgress:
            switch (newState) {
                case kNone:
                case kPrepared:
                case kCommittingWithoutPrepare:
                case kAbortedWithoutPrepare:
                    return true;
                default:
                    return false;
            }
            MONGO_UNREACHABLE;
        case kPrepared:
            switch (newState) {
                case kCommittingWithPrepare:
                case kAbortedWithPrepare:
                    return true;
                default:
                    return false;
            }
            MONGO_UNREACHABLE;
        case kCommittingWithPrepare:
            switch (newState) {
                case kCommitted:
                    return true;
                default:
                    return false;
            }
            MONGO_UNREACHABLE;
        case kCommittingWithoutPrepare:
            switch (newState) {
                case kNone:
                case kCommitted:
                case kAbortedWithoutPrepare:
                    return true;
                default:
                    return false;
            }
            MONGO_UNREACHABLE;
        case kCommitted:
            switch (newState) {
                case kNone:
                    return true;
                default:
                    return false;
            }
            MONGO_UNREACHABLE;
        case kAbortedWithoutPrepare:
            switch (newState) {
                case kNone:
                case kInProgress:
                    return true;
                default:
                    return false;
            }
            MONGO_UNREACHABLE;
        case kAbortedWithPrepare:
            switch (newState) {
                case kNone:
                    return true;
                default:
                    return false;
            }
            MONGO_UNREACHABLE;
    }
    MONGO_UNREACHABLE;
}

void TransactionParticipant::TransactionState::transitionTo(WithLock,
                                                            StateFlag newState,
                                                            TransitionValidation shouldValidate) {
    if (shouldValidate == TransitionValidation::kValidateTransition) {
        invariant(TransactionState::_isLegalTransition(_state, newState),
                  str::stream() << "Current state: " << toString(_state)
                                << ", Illegal attempted next state: "
                                << toString(newState));
    }

    _state = newState;
}

void TransactionParticipant::_reportTransactionStats(WithLock wl,
                                                     BSONObjBuilder* builder,
                                                     repl::ReadConcernArgs readConcernArgs) const {
    const auto tickSource = getGlobalServiceContext()->getTickSource();
    _transactionMetricsObserver.getSingleTransactionStats().report(
        builder, readConcernArgs, tickSource, tickSource->getTicks());
}

std::string TransactionParticipant::_transactionInfoForLog(
    const SingleThreadedLockStats* lockStats,
    TerminationCause terminationCause,
    repl::ReadConcernArgs readConcernArgs) const {
    invariant(lockStats);

    StringBuilder s;

    // User specified transaction parameters.
    BSONObjBuilder parametersBuilder;

    BSONObjBuilder lsidBuilder(parametersBuilder.subobjStart("lsid"));
    _sessionId().serialize(&lsidBuilder);
    lsidBuilder.doneFast();

    parametersBuilder.append("txnNumber", _activeTxnNumber);
    parametersBuilder.append("autocommit", _autoCommit ? *_autoCommit : true);
    readConcernArgs.appendInfo(&parametersBuilder);

    s << "parameters:" << parametersBuilder.obj().toString() << ",";

    s << " readTimestamp:" << _speculativeTransactionReadOpTime.getTimestamp().toString() << ",";

    const auto& singleTransactionStats = _transactionMetricsObserver.getSingleTransactionStats();

    s << singleTransactionStats.getOpDebug()->additiveMetrics.report();

    std::string terminationCauseString =
        terminationCause == TerminationCause::kCommitted ? "committed" : "aborted";
    s << " terminationCause:" << terminationCauseString;

    auto tickSource = getGlobalServiceContext()->getTickSource();
    auto curTick = tickSource->getTicks();

    s << " timeActiveMicros:"
      << durationCount<Microseconds>(
             singleTransactionStats.getTimeActiveMicros(tickSource, curTick));
    s << " timeInactiveMicros:"
      << durationCount<Microseconds>(
             singleTransactionStats.getTimeInactiveMicros(tickSource, curTick));

    // Number of yields is always 0 in multi-document transactions, but it is included mainly to
    // match the format with other slow operation logging messages.
    s << " numYields:" << 0;
    // Aggregate lock statistics.

    BSONObjBuilder locks;
    lockStats->report(&locks);
    s << " locks:" << locks.obj().toString();

    if (singleTransactionStats.getOpDebug()->storageStats)
        s << " storage:" << singleTransactionStats.getOpDebug()->storageStats->toBSON().toString();

    // It is possible for a slow transaction to have aborted in the prepared state if an
    // exception was thrown before prepareTransaction succeeds.
    const auto totalPreparedDuration = durationCount<Microseconds>(
        singleTransactionStats.getPreparedDuration(tickSource, curTick));
    const bool txnWasPrepared = totalPreparedDuration > 0;
    s << " wasPrepared:" << txnWasPrepared;
    if (txnWasPrepared) {
        s << " totalPreparedDurationMicros:" << totalPreparedDuration;
        s << " prepareOpTime:" << _prepareOpTime.toString();
    }

    if (_oldestOplogEntryOpTime) {
        s << " oldestOplogEntryOpTime:" << _oldestOplogEntryOpTime->toString();
    }

    if (_finishOpTime) {
        s << " finishOpTime:" << _finishOpTime->toString();
    }

    // Total duration of the transaction.
    s << ", "
      << duration_cast<Milliseconds>(singleTransactionStats.getDuration(tickSource, curTick));

    return s.str();
}

void TransactionParticipant::_logSlowTransaction(WithLock wl,
                                                 const SingleThreadedLockStats* lockStats,
                                                 TerminationCause terminationCause,
                                                 repl::ReadConcernArgs readConcernArgs) {
    // Only log multi-document transactions.
    if (!_txnState.isNone(wl)) {
        const auto tickSource = getGlobalServiceContext()->getTickSource();
        // Log the transaction if its duration is longer than the slowMS command threshold.
        if (_transactionMetricsObserver.getSingleTransactionStats().getDuration(
                tickSource, tickSource->getTicks()) > Milliseconds(serverGlobalParams.slowMS)) {
            log(logger::LogComponent::kTransaction)
                << "transaction "
                << _transactionInfoForLog(lockStats, terminationCause, readConcernArgs);
        }
    }
}

void TransactionParticipant::_setNewTxnNumber(WithLock wl, const TxnNumber& txnNumber) {
    uassert(ErrorCodes::PreparedTransactionInProgress,
            "Cannot change transaction number while the session has a prepared transaction",
            !_txnState.isInSet(
                wl, TransactionState::kPrepared | TransactionState::kCommittingWithPrepare));

    LOG_FOR_TRANSACTION(4) << "New transaction started with txnNumber: " << txnNumber
                           << " on session with lsid " << _sessionId().getId();

    // Abort the existing transaction if it's not prepared, committed, or aborted.
    if (_txnState.isInProgress(wl)) {
        _abortTransactionOnSession(wl);
    }

    _activeTxnNumber = txnNumber;
    _lastWriteOpTime = repl::OpTime();

    // Reset the retryable writes state
    _resetRetryableWriteState(wl);

    // Reset the transactional state
    _resetTransactionState(wl, TransactionState::kNone);

    // Reset the transactions metrics
    stdx::lock_guard<stdx::mutex> lm(_metricsMutex);
    _transactionMetricsObserver.resetSingleTransactionStats(txnNumber);
}

void TransactionParticipant::refreshFromStorageIfNeeded() {
    const auto opCtx = _opCtx();
    invariant(!opCtx->getClient()->isInDirectClient());
    invariant(!opCtx->lockState()->isLocked());

    if (_isValid)
        return;

    auto activeTxnHistory = fetchActiveTransactionHistory(opCtx, _sessionId());

    stdx::lock_guard<stdx::mutex> lg(_mutex);

    const auto& lastTxnRecord = activeTxnHistory.lastTxnRecord;

    if (lastTxnRecord) {
        _activeTxnNumber = lastTxnRecord->getTxnNum();
        _lastWriteOpTime = lastTxnRecord->getLastWriteOpTime();
        _activeTxnCommittedStatements = std::move(activeTxnHistory.committedStatements);
        _hasIncompleteHistory = activeTxnHistory.hasIncompleteHistory;

        if (activeTxnHistory.transactionCommitted) {
            _txnState.transitionTo(
                lg,
                TransactionState::kCommitted,
                TransactionState::TransitionValidation::kRelaxTransitionValidation);
        }
    }

    _isValid = true;
}

void TransactionParticipant::onWriteOpCompletedOnPrimary(
    OperationContext* opCtx,
    TxnNumber txnNumber,
    std::vector<StmtId> stmtIdsWritten,
    const repl::OpTime& lastStmtIdWriteOpTime,
    Date_t lastStmtIdWriteDate,
    boost::optional<DurableTxnStateEnum> txnState) {
    invariant(opCtx->lockState()->inAWriteUnitOfWork());
    invariant(txnNumber == _activeTxnNumber);

    stdx::unique_lock<stdx::mutex> ul(_mutex);

    // Sanity check that we don't double-execute statements
    for (const auto stmtId : stmtIdsWritten) {
        const auto stmtOpTime = _checkStatementExecuted(stmtId);
        if (stmtOpTime) {
            fassertOnRepeatedExecution(
                _sessionId(), txnNumber, stmtId, *stmtOpTime, lastStmtIdWriteOpTime);
        }
    }

    const auto updateRequest =
        _makeUpdateRequest(lastStmtIdWriteOpTime, lastStmtIdWriteDate, txnState);

    ul.unlock();

    repl::UnreplicatedWritesBlock doNotReplicateWrites(opCtx);

    updateSessionEntry(opCtx, updateRequest);
    _registerUpdateCacheOnCommit(std::move(stmtIdsWritten), lastStmtIdWriteOpTime);
}

void TransactionParticipant::onMigrateCompletedOnPrimary(OperationContext* opCtx,
                                                         TxnNumber txnNumber,
                                                         std::vector<StmtId> stmtIdsWritten,
                                                         const repl::OpTime& lastStmtIdWriteOpTime,
                                                         Date_t oplogLastStmtIdWriteDate) {
    invariant(opCtx->lockState()->inAWriteUnitOfWork());
    invariant(txnNumber == _activeTxnNumber);

    stdx::unique_lock<stdx::mutex> ul(_mutex);

    _checkValid(ul);
    _checkIsActiveTransaction(ul, txnNumber);

    // We do not migrate transaction oplog entries so don't set the txn state
    const auto txnState = boost::none;
    const auto updateRequest =
        _makeUpdateRequest(lastStmtIdWriteOpTime, oplogLastStmtIdWriteDate, txnState);

    ul.unlock();

    repl::UnreplicatedWritesBlock doNotReplicateWrites(opCtx);

    updateSessionEntry(opCtx, updateRequest);
    _registerUpdateCacheOnCommit(std::move(stmtIdsWritten), lastStmtIdWriteOpTime);
}

void TransactionParticipant::_invalidate(WithLock) {
    _isValid = false;
    _activeTxnNumber = kUninitializedTxnNumber;
    _lastWriteOpTime = repl::OpTime();

    // Reset the transactions metrics.
    stdx::lock_guard<stdx::mutex> lm(_metricsMutex);
    _transactionMetricsObserver.resetSingleTransactionStats(_activeTxnNumber);
}

void TransactionParticipant::_resetRetryableWriteState(WithLock) {
    _activeTxnCommittedStatements.clear();
    _hasIncompleteHistory = false;
}

void TransactionParticipant::_resetTransactionState(WithLock wl,
                                                    TransactionState::StateFlag state) {
    // If we are transitioning to kNone, we are either starting a new transaction or aborting a
    // prepared transaction for rollback. In the latter case, we will need to relax the invariant
    // that prevents transitioning from kPrepared to kNone.
    if (_txnState.isPrepared(wl) && state == TransactionState::kNone) {
        _txnState.transitionTo(
            wl, state, TransactionState::TransitionValidation::kRelaxTransitionValidation);
    } else {
        _txnState.transitionTo(wl, state);
    }

    _transactionOperationBytes = 0;
    _transactionOperations.clear();
    _prepareOpTime = repl::OpTime();
    _oldestOplogEntryOpTime = boost::none;
    _finishOpTime = boost::none;
    _speculativeTransactionReadOpTime = repl::OpTime();
    _multikeyPathInfo.clear();
    _autoCommit = boost::none;

    // Release any locks held by this participant and abort the storage transaction.
    _txnResourceStash = boost::none;
}

void TransactionParticipant::invalidate() {
    stdx::lock_guard<stdx::mutex> lg(_mutex);

    uassert(ErrorCodes::PreparedTransactionInProgress,
            "Cannot invalidate prepared transaction",
            !_txnState.isInSet(
                lg, TransactionState::kPrepared | TransactionState::kCommittingWithPrepare));

    // Invalidate the session and clear both the retryable writes and transactional states on
    // this participant.
    _invalidate(lg);
    _resetRetryableWriteState(lg);
    _resetTransactionState(lg, TransactionState::kNone);
}

void TransactionParticipant::abortPreparedTransactionForRollback() {
    stdx::lock_guard<stdx::mutex> lg(_mutex);

    // Invalidate the session.
    _invalidate(lg);

    uassert(51030,
            str::stream() << "Cannot call abortPreparedTransactionForRollback on unprepared "
                          << "transaction.",
            _txnState.isPrepared(lg));

    // It should be safe to clear transactionOperationBytes and transactionOperations because
    // we only modify these variables when adding an operation to a transaction. Since this
    // transaction is already prepared, we cannot add more operations to it. We will have this
    // in the prepare oplog entry.
    // Both _finishOpTime and _oldestOplogEntryOpTime will be reset to boost::none. With a
    // prepared transaction, the latter is the same as the prepareOpTime.
    _resetTransactionState(lg, TransactionState::kNone);
}

repl::OpTime TransactionParticipant::getLastWriteOpTime() const {
    stdx::lock_guard<stdx::mutex> lg(_mutex);
    return _lastWriteOpTime;
}

boost::optional<repl::OplogEntry> TransactionParticipant::checkStatementExecuted(
    StmtId stmtId) const {
    const auto stmtTimestamp = _checkStatementExecuted(stmtId);

    if (!stmtTimestamp)
        return boost::none;

    TransactionHistoryIterator txnIter(*stmtTimestamp);
    while (txnIter.hasNext()) {
        const auto entry = txnIter.next(_opCtx());
        invariant(entry.getStatementId());
        if (*entry.getStatementId() == stmtId)
            return entry;
    }

    MONGO_UNREACHABLE;
}

bool TransactionParticipant::checkStatementExecutedNoOplogEntryFetch(StmtId stmtId) const {
    return bool(_checkStatementExecuted(stmtId));
}

void TransactionParticipant::_checkValid(WithLock) const {
    uassert(ErrorCodes::ConflictingOperationInProgress,
            str::stream() << "Session " << _sessionId()
                          << " was concurrently modified and the operation must be retried.",
            _isValid);
}

void TransactionParticipant::_checkIsActiveTransaction(WithLock, TxnNumber txnNumber) const {
    uassert(ErrorCodes::ConflictingOperationInProgress,
            str::stream() << "Cannot perform operations on transaction " << txnNumber
                          << " on session "
                          << _sessionId()
                          << " because a different transaction "
                          << _activeTxnNumber
                          << " is now active.",
            txnNumber == _activeTxnNumber);
}

boost::optional<repl::OpTime> TransactionParticipant::_checkStatementExecuted(StmtId stmtId) const {
    invariant(_isValid);

    const auto it = _activeTxnCommittedStatements.find(stmtId);
    if (it == _activeTxnCommittedStatements.end()) {
        uassert(ErrorCodes::IncompleteTransactionHistory,
                str::stream() << "Incomplete history detected for transaction " << _activeTxnNumber
                              << " on session "
                              << _sessionId(),
                !_hasIncompleteHistory);

        return boost::none;
    }

    return it->second;
}

UpdateRequest TransactionParticipant::_makeUpdateRequest(
    const repl::OpTime& newLastWriteOpTime,
    Date_t newLastWriteDate,
    boost::optional<DurableTxnStateEnum> newState) const {
    UpdateRequest updateRequest(NamespaceString::kSessionTransactionsTableNamespace);

    const auto updateBSON = [&] {
        SessionTxnRecord newTxnRecord;
        newTxnRecord.setSessionId(_sessionId());
        newTxnRecord.setTxnNum(_activeTxnNumber);
        newTxnRecord.setLastWriteOpTime(newLastWriteOpTime);
        newTxnRecord.setLastWriteDate(newLastWriteDate);
        newTxnRecord.setState(newState);
        return newTxnRecord.toBSON();
    }();
    updateRequest.setUpdates(updateBSON);
    updateRequest.setQuery(BSON(SessionTxnRecord::kSessionIdFieldName << _sessionId().toBSON()));
    updateRequest.setUpsert(true);

    return updateRequest;
}

void TransactionParticipant::_registerUpdateCacheOnCommit(
    std::vector<StmtId> stmtIdsWritten, const repl::OpTime& lastStmtIdWriteOpTime) {
    _opCtx()->recoveryUnit()->onCommit(
        [ this, stmtIdsWritten = std::move(stmtIdsWritten), lastStmtIdWriteOpTime ](
            boost::optional<Timestamp>) {
            invariant(_isValid);

            RetryableWritesStats::get(getGlobalServiceContext())
                ->incrementTransactionsCollectionWriteCount();

            stdx::lock_guard<stdx::mutex> lg(_mutex);

            // The cache of the last written record must always be advanced after a write so that
            // subsequent writes have the correct point to start from.
            _lastWriteOpTime = lastStmtIdWriteOpTime;

            for (const auto stmtId : stmtIdsWritten) {
                if (stmtId == kIncompleteHistoryStmtId) {
                    _hasIncompleteHistory = true;
                    continue;
                }

                const auto insertRes =
                    _activeTxnCommittedStatements.emplace(stmtId, lastStmtIdWriteOpTime);
                if (!insertRes.second) {
                    const auto& existingOpTime = insertRes.first->second;
                    fassertOnRepeatedExecution(_sessionId(),
                                               _activeTxnNumber,
                                               stmtId,
                                               existingOpTime,
                                               lastStmtIdWriteOpTime);
                }
            }
        });

    MONGO_FAIL_POINT_BLOCK(onPrimaryTransactionalWrite, customArgs) {
        const auto& data = customArgs.getData();

        const auto closeConnectionElem = data["closeConnection"];
        if (closeConnectionElem.eoo() || closeConnectionElem.Bool()) {
            _opCtx()->getClient()->session()->end();
        }

        const auto failBeforeCommitExceptionElem = data["failBeforeCommitExceptionCode"];
        if (!failBeforeCommitExceptionElem.eoo()) {
            const auto failureCode = ErrorCodes::Error(int(failBeforeCommitExceptionElem.Number()));
            uasserted(failureCode,
                      str::stream() << "Failing write for " << _sessionId() << ":"
                                    << _activeTxnNumber
                                    << " due to failpoint. The write must not be reflected.");
        }
    }
}

}  // namespace mongo
