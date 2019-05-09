
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

#include "mongo/platform/basic.h"

#include "mongo/db/session.h"

#include "mongo/db/catalog/index_catalog.h"
#include "mongo/db/commands/feature_compatibility_version_documentation.h"
#include "mongo/db/commands/test_commands_enabled.h"
#include "mongo/db/concurrency/lock_state.h"
#include "mongo/db/concurrency/locker.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/curop_failpoint_helpers.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/index/index_access_method.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/ops/update.h"
#include "mongo/db/query/get_executor.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/retryable_writes_stats.h"
#include "mongo/db/server_options.h"
#include "mongo/db/server_parameters.h"
#include "mongo/db/server_transactions_metrics.h"
#include "mongo/db/stats/fill_locker_info.h"
#include "mongo/db/stats/top.h"
#include "mongo/db/transaction_history_iterator.h"
#include "mongo/stdx/memory.h"
#include "mongo/transport/transport_layer.h"
#include "mongo/util/fail_point_service.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"
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
    Session::CommittedStatementTimestampMap committedStatements;
    bool transactionCommitted{false};
    bool hasIncompleteHistory{false};
};

ActiveTransactionHistory fetchActiveTransactionHistory(OperationContext* opCtx,
                                                       const LogicalSessionId& lsid) {
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
                invariant(entry.getObject2()->woCompare(Session::kDeadEndSentinel) == 0);
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

            // applyOps oplog entry marks the commit of a transaction.
            if (entry.isCommand() &&
                entry.getCommandType() == repl::OplogEntry::CommandType::kApplyOps) {
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

    auto indexAccess = collection->getIndexCatalog()->getIndex(idIndex);
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
            opCtx, InsertStatement(updateRequest.getUpdates()), nullptr, true, false);

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

    OplogUpdateEntryArgs args;
    args.nss = NamespaceString::kSessionTransactionsTableNamespace;
    args.uuid = collection->uuid();
    args.update = updateRequest.getUpdates();
    args.criteria = toUpdateIdDoc;
    args.fromMigrate = false;

    collection->updateDocument(opCtx,
                               recordId,
                               Snapshotted<BSONObj>(startingSnapshotId, originalDoc),
                               updateRequest.getUpdates(),
                               true,   // enforceQuota
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

// Failpoint which will pause an operation just after allocating a point-in-time storage engine
// transaction.
MONGO_FAIL_POINT_DEFINE(hangAfterPreallocateSnapshot);
}  // namespace

const BSONObj Session::kDeadEndSentinel(BSON("$incompleteOplogHistory" << 1));

Session::Session(LogicalSessionId sessionId) : _sessionId(std::move(sessionId)) {}

void Session::setCurrentOperation(OperationContext* currentOperation) {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    invariant(!_currentOperation);
    _currentOperation = currentOperation;
}

void Session::clearCurrentOperation() {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    invariant(_currentOperation);
    _currentOperation = nullptr;
}

void Session::refreshFromStorageIfNeeded(OperationContext* opCtx) {
    if (opCtx->getClient()->isInDirectClient()) {
        return;
    }

    invariant(!opCtx->lockState()->isLocked());
    invariant(repl::ReadConcernArgs::get(opCtx).getLevel() ==
              repl::ReadConcernLevel::kLocalReadConcern);

    stdx::unique_lock<stdx::mutex> ul(_mutex);

    while (!_isValid) {
        const int numInvalidations = _numInvalidations;

        ul.unlock();

        auto activeTxnHistory = fetchActiveTransactionHistory(opCtx, _sessionId);

        ul.lock();

        // Protect against concurrent refreshes or invalidations
        if (!_isValid && _numInvalidations == numInvalidations) {
            _isValid = true;
            _lastWrittenSessionRecord = std::move(activeTxnHistory.lastTxnRecord);

            if (_lastWrittenSessionRecord) {
                _activeTxnNumber = _lastWrittenSessionRecord->getTxnNum();
                _activeTxnCommittedStatements = std::move(activeTxnHistory.committedStatements);
                _hasIncompleteHistory = activeTxnHistory.hasIncompleteHistory;
                if (activeTxnHistory.transactionCommitted) {
                    _txnState = MultiDocumentTransactionState::kCommitted;
                }
            }

            break;
        }
    }
}

void Session::beginOrContinueTxn(OperationContext* opCtx,
                                 TxnNumber txnNumber,
                                 boost::optional<bool> autocommit,
                                 boost::optional<bool> startTransaction,
                                 StringData dbName,
                                 StringData cmdName) {
    if (opCtx->getClient()->isInDirectClient()) {
        return;
    }

    invariant(!opCtx->lockState()->isLocked());

    stdx::lock_guard<stdx::mutex> lg(_mutex);
    _beginOrContinueTxn(lg, txnNumber, autocommit, startTransaction);
}

void Session::beginOrContinueTxnOnMigration(OperationContext* opCtx, TxnNumber txnNumber) {
    invariant(!opCtx->getClient()->isInDirectClient());
    invariant(!opCtx->lockState()->isLocked());

    stdx::lock_guard<stdx::mutex> lg(_mutex);
    _beginOrContinueTxnOnMigration(lg, txnNumber);
}

void Session::_setSpeculativeTransactionOpTime(WithLock,
                                               OperationContext* opCtx,
                                               SpeculativeTransactionOpTime opTimeChoice) {
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
    stdx::lock_guard<stdx::mutex> ls(_statsMutex);
    _singleTransactionStats.setReadTimestamp(readTimestamp);
}

void Session::onWriteOpCompletedOnPrimary(OperationContext* opCtx,
                                          TxnNumber txnNumber,
                                          std::vector<StmtId> stmtIdsWritten,
                                          const repl::OpTime& lastStmtIdWriteOpTime,
                                          Date_t lastStmtIdWriteDate) {
    invariant(opCtx->lockState()->inAWriteUnitOfWork());

    stdx::unique_lock<stdx::mutex> ul(_mutex);

    // Sanity check that we don't double-execute statements
    for (const auto stmtId : stmtIdsWritten) {
        const auto stmtOpTime = _checkStatementExecuted(ul, txnNumber, stmtId);
        if (stmtOpTime) {
            fassertOnRepeatedExecution(
                _sessionId, txnNumber, stmtId, *stmtOpTime, lastStmtIdWriteOpTime);
        }
    }

    const auto updateRequest =
        _makeUpdateRequest(ul, txnNumber, lastStmtIdWriteOpTime, lastStmtIdWriteDate);

    ul.unlock();

    repl::UnreplicatedWritesBlock doNotReplicateWrites(opCtx);

    updateSessionEntry(opCtx, updateRequest);
    _registerUpdateCacheOnCommit(
        opCtx, txnNumber, std::move(stmtIdsWritten), lastStmtIdWriteOpTime);
}

bool Session::onMigrateBeginOnPrimary(OperationContext* opCtx, TxnNumber txnNumber, StmtId stmtId) {
    beginOrContinueTxnOnMigration(opCtx, txnNumber);

    try {
        if (checkStatementExecuted(opCtx, txnNumber, stmtId)) {
            return false;
        }
    } catch (const DBException& ex) {
        // If the transaction chain is incomplete because oplog was truncated, just ignore the
        // incoming oplog and don't attempt to 'patch up' the missing pieces.
        if (ex.code() == ErrorCodes::IncompleteTransactionHistory) {
            return false;
        }

        if (stmtId == kIncompleteHistoryStmtId) {
            return false;
        }

        throw;
    }

    return true;
}

void Session::onMigrateCompletedOnPrimary(OperationContext* opCtx,
                                          TxnNumber txnNumber,
                                          std::vector<StmtId> stmtIdsWritten,
                                          const repl::OpTime& lastStmtIdWriteOpTime,
                                          Date_t oplogLastStmtIdWriteDate) {
    invariant(opCtx->lockState()->inAWriteUnitOfWork());

    stdx::unique_lock<stdx::mutex> ul(_mutex);

    _checkValid(ul);
    _checkIsActiveTransaction(ul, txnNumber, false);

    // We do not migrate transaction oplog entries.
    const auto updateRequest =
        _makeUpdateRequest(ul, txnNumber, lastStmtIdWriteOpTime, oplogLastStmtIdWriteDate);

    ul.unlock();

    repl::UnreplicatedWritesBlock doNotReplicateWrites(opCtx);

    updateSessionEntry(opCtx, updateRequest);
    _registerUpdateCacheOnCommit(
        opCtx, txnNumber, std::move(stmtIdsWritten), lastStmtIdWriteOpTime);
}

void Session::invalidate() {
    stdx::lock_guard<stdx::mutex> lg(_mutex);
    _isValid = false;
    _numInvalidations++;

    _lastWrittenSessionRecord.reset();

    _activeTxnNumber = kUninitializedTxnNumber;
    _activeTxnCommittedStatements.clear();
    _speculativeTransactionReadOpTime = repl::OpTime();
    _hasIncompleteHistory = false;
}

repl::OpTime Session::getLastWriteOpTime(TxnNumber txnNumber) const {
    stdx::lock_guard<stdx::mutex> lg(_mutex);
    _checkValid(lg);
    _checkIsActiveTransaction(lg, txnNumber, false);

    if (!_lastWrittenSessionRecord || _lastWrittenSessionRecord->getTxnNum() != txnNumber)
        return {};

    return _lastWrittenSessionRecord->getLastWriteOpTime();
}

boost::optional<repl::OplogEntry> Session::checkStatementExecuted(OperationContext* opCtx,
                                                                  TxnNumber txnNumber,
                                                                  StmtId stmtId) const {
    const auto stmtTimestamp = [&] {
        stdx::lock_guard<stdx::mutex> lg(_mutex);
        return _checkStatementExecuted(lg, txnNumber, stmtId);
    }();

    if (!stmtTimestamp)
        return boost::none;

    TransactionHistoryIterator txnIter(*stmtTimestamp);
    while (txnIter.hasNext()) {
        const auto entry = txnIter.next(opCtx);
        invariant(entry.getStatementId());
        if (*entry.getStatementId() == stmtId)
            return entry;
    }

    MONGO_UNREACHABLE;
}

bool Session::checkStatementExecutedNoOplogEntryFetch(TxnNumber txnNumber, StmtId stmtId) const {
    stdx::lock_guard<stdx::mutex> lg(_mutex);
    return bool(_checkStatementExecuted(lg, txnNumber, stmtId));
}

void Session::_beginOrContinueTxn(WithLock wl,
                                  TxnNumber txnNumber,
                                  boost::optional<bool> autocommit,
                                  boost::optional<bool> startTransaction) {

    // Check whether the session information needs to be refreshed from disk.
    _checkValid(wl);

    // Check if the given transaction number is valid for this session. The transaction number must
    // be >= the active transaction number.
    _checkTxnValid(wl, txnNumber);

    //
    // Continue an active transaction.
    //
    if (txnNumber == _activeTxnNumber) {

        // It is never valid to specify 'startTransaction' on an active transaction.
        uassert(ErrorCodes::ConflictingOperationInProgress,
                str::stream() << "Cannot specify 'startTransaction' on transaction " << txnNumber
                              << " since it is already in progress.",
                startTransaction == boost::none);

        // Continue a retryable write.
        if (_txnState == MultiDocumentTransactionState::kNone) {
            uassert(ErrorCodes::InvalidOptions,
                    "Cannot specify 'autocommit' on an operation not inside a multi-statement "
                    "transaction.",
                    autocommit == boost::none);
            return;
        }

        // Continue a multi-statement transaction. In this case, it is required that
        // autocommit=false be given as an argument on the request. Retryable writes will have
        // _autocommit=true, so that is why we verify that _autocommit=false here.
        if (!_autocommit) {
            uassert(
                ErrorCodes::InvalidOptions,
                "Must specify autocommit=false on all operations of a multi-statement transaction.",
                autocommit == boost::optional<bool>(false));
            if (_txnState == MultiDocumentTransactionState::kInProgress && !_txnResourceStash) {
                // This indicates that the first command in the transaction failed but did not
                // implicitly abort the transaction. It is not safe to continue the transaction, in
                // particular because we have not saved the readConcern from the first statement of
                // the transaction. Mark the transaction as active here, since
                // _abortTransaction() will assume we are aborting an active transaction since there
                // are no stashed resources.
                _singleTransactionStats.setActive(curTimeMicros64());
                ServerTransactionsMetrics::get(getGlobalServiceContext())->incrementCurrentActive();
                ServerTransactionsMetrics::get(getGlobalServiceContext())
                    ->decrementCurrentInactive();
                _abortTransaction(wl);
                uasserted(ErrorCodes::NoSuchTransaction,
                          str::stream() << "Transaction " << txnNumber
                                        << " has been aborted because an earlier command in this "
                                           "transaction failed.");
            }
        }
        return;
    }

    //
    // Start a new transaction.
    //
    // At this point, the given transaction number must be > _activeTxnNumber. Existence of an
    // 'autocommit' field means we interpret this operation as part of a multi-document transaction.
    invariant(txnNumber > _activeTxnNumber);
    if (autocommit) {
        // Start a multi-document transaction.
        invariant(*autocommit == false);
        uassert(ErrorCodes::NoSuchTransaction,
                str::stream() << "Given transaction number " << txnNumber
                              << " does not match any in-progress transactions.",
                startTransaction != boost::none);

        // Check for FCV 4.0. The presence of an autocommit field distiguishes this as a
        // multi-statement transaction vs a retryable write.
        uassert(
            50773,
            str::stream() << "Transactions are only supported in featureCompatibilityVersion 4.0. "
                          << "See "
                          << feature_compatibility_version_documentation::kCompatibilityLink
                          << " for more information.",
            (serverGlobalParams.featureCompatibility.isVersionInitialized() &&
             serverGlobalParams.featureCompatibility.getVersion() ==
                 ServerGlobalParams::FeatureCompatibility::Version::kFullyUpgradedTo40));

        _setActiveTxn(wl, txnNumber);
        _autocommit = false;
        _txnState = MultiDocumentTransactionState::kInProgress;

        const auto now = curTimeMicros64();
        _transactionExpireDate = Date_t::fromMillisSinceEpoch(now / 1000) +
            Seconds{transactionLifetimeLimitSeconds.load()};
        // Tracks various transactions metrics.
        {
            stdx::lock_guard<stdx::mutex> ls(_statsMutex);
            _singleTransactionStats.setStartTime(now);
            _singleTransactionStats.setExpireDate(*_transactionExpireDate);
            _singleTransactionStats.setAutoCommit(autocommit);
        }
        ServerTransactionsMetrics::get(getGlobalServiceContext())->incrementTotalStarted();
        // The transaction is considered open here and stays inactive until its first unstash event.
        ServerTransactionsMetrics::get(getGlobalServiceContext())->incrementCurrentOpen();
        ServerTransactionsMetrics::get(getGlobalServiceContext())->incrementCurrentInactive();
    } else {
        // Execute a retryable write.
        invariant(startTransaction == boost::none);
        _setActiveTxn(wl, txnNumber);
        _autocommit = true;
        _txnState = MultiDocumentTransactionState::kNone;
    }

    invariant(_transactionOperations.empty());
}

void Session::_checkTxnValid(WithLock, TxnNumber txnNumber) const {
    uassert(ErrorCodes::TransactionTooOld,
            str::stream() << "Cannot start transaction " << txnNumber << " on session "
                          << getSessionId()
                          << " because a newer transaction "
                          << _activeTxnNumber
                          << " has already started.",
            txnNumber >= _activeTxnNumber);
}

Session::TxnResources::TxnResources(OperationContext* opCtx, bool keepTicket) {
    stdx::lock_guard<Client> lk(*opCtx->getClient());
    _ruState = opCtx->getWriteUnitOfWork()->release();
    opCtx->setWriteUnitOfWork(nullptr);

    _locker = opCtx->swapLockState(stdx::make_unique<DefaultLockerImpl>());
    if (!keepTicket) {
        _locker->releaseTicket();
    }
    _locker->unsetThreadId();

    // This thread must still respect the transaction lock timeout, since it can prevent the
    // transaction from making progress.
    auto maxTransactionLockMillis = maxTransactionLockRequestTimeoutMillis.load();
    if (maxTransactionLockMillis >= 0) {
        opCtx->lockState()->setMaxLockTimeout(Milliseconds(maxTransactionLockMillis));
    }

    _recoveryUnit = std::unique_ptr<RecoveryUnit>(opCtx->releaseRecoveryUnit());
    opCtx->setRecoveryUnit(opCtx->getServiceContext()->getStorageEngine()->newRecoveryUnit(),
                           WriteUnitOfWork::RecoveryUnitState::kNotInUnitOfWork);

    _readConcernArgs = repl::ReadConcernArgs::get(opCtx);
}

Session::TxnResources::~TxnResources() {
    if (!_released && _recoveryUnit) {
        // This should only be reached when aborting a transaction that isn't active, i.e.
        // when starting a new transaction before completing an old one.  So we should
        // be at WUOW nesting level 1 (only the top level WriteUnitOfWork).
        _recoveryUnit->abortUnitOfWork();
        _locker->endWriteUnitOfWork();
        invariant(!_locker->inAWriteUnitOfWork());
    }
}

void Session::TxnResources::release(OperationContext* opCtx) {
    // Perform operations that can fail the release before marking the TxnResources as released.
    _locker->reacquireTicket(opCtx);

    invariant(!_released);
    _released = true;

    // We must take the client lock before changing the locker.
    stdx::lock_guard<Client> lk(*opCtx->getClient());
    // We intentionally do not capture the return value of swapLockState(), which is just an empty
    // locker. At the end of the operation, if the transaction is not complete, we will stash the
    // operation context's locker and replace it with a new empty locker.
    invariant(opCtx->lockState()->getClientState() == Locker::ClientState::kInactive);
    opCtx->swapLockState(std::move(_locker));
    opCtx->lockState()->updateThreadIdToCurrentThread();

    opCtx->setRecoveryUnit(_recoveryUnit.release(),
                           WriteUnitOfWork::RecoveryUnitState::kNotInUnitOfWork);

    opCtx->setWriteUnitOfWork(WriteUnitOfWork::createForSnapshotResume(opCtx, _ruState));

    auto& readConcernArgs = repl::ReadConcernArgs::get(opCtx);
    readConcernArgs = _readConcernArgs;
}

void Session::stashTransactionResources(OperationContext* opCtx) {
    if (opCtx->getClient()->isInDirectClient()) {
        return;
    }

    invariant(opCtx->getTxnNumber());
    invariant(!isMMAPV1());
    stdx::unique_lock<stdx::mutex> lg(_mutex);

    // Always check '_activeTxnNumber', since it can be modified by migration, which does not
    // check out the session. We intentionally do not error if _txnState=kAborted, since we
    // expect this function to be called at the end of the 'abortTransaction' command.
    _checkIsActiveTransaction(lg, *opCtx->getTxnNumber(), false);

    if (_txnState != MultiDocumentTransactionState::kInProgress) {
        // Not in a multi-document transaction: nothing to do.
        return;
    }

    {
        stdx::lock_guard<stdx::mutex> ls(_statsMutex);
        if (_singleTransactionStats.isActive()) {
            _singleTransactionStats.setInactive(curTimeMicros64());
        }

        // Add the latest operation stats to the aggregate OpDebug object stored in the
        // SingleTransactionStats instance on the Session.
        _singleTransactionStats.getOpDebug()->additiveMetrics.add(
            CurOp::get(opCtx)->debug().additiveMetrics);

        // If there are valid storage statistics for this operation, put those in the
        // SingleTransactionStats instance either by creating a new storageStats instance or by
        // adding into an existing storageStats instance stored in SingleTransactionStats.
        std::shared_ptr<StorageStats> storageStats =
            opCtx->recoveryUnit()->getOperationStatistics();
        if (storageStats) {
            CurOp::get(opCtx)->debug().storageStats = storageStats;
            if (!_singleTransactionStats.getOpDebug()->storageStats) {
                _singleTransactionStats.getOpDebug()->storageStats = storageStats->getCopy();
            } else {
                *_singleTransactionStats.getOpDebug()->storageStats += *storageStats;
            }
        }

        // Update the LastClientInfo object stored in the SingleTransactionStats instance on the
        // Session with this Client's information. This is the last client that ran a transaction
        // operation on the Session.
        _singleTransactionStats.updateLastClientInfo(opCtx->getClient());
    }

    invariant(!_txnResourceStash);
    _txnResourceStash = TxnResources(opCtx);

    // We accept possible slight inaccuracies in these counters from non-atomicity.
    ServerTransactionsMetrics::get(opCtx)->decrementCurrentActive();
    ServerTransactionsMetrics::get(opCtx)->incrementCurrentInactive();
}

void Session::unstashTransactionResources(OperationContext* opCtx, const std::string& cmdName) {
    if (opCtx->getClient()->isInDirectClient()) {
        return;
    }

    invariant(opCtx->getTxnNumber());

    // If the storage engine is mmapv1, it is not safe to lock both the Client and the Session
    // mutex. This is fine because mmapv1 does not support transactions.
    if (isMMAPV1()) {
        return;
    }

    {
        stdx::lock_guard<stdx::mutex> lg(_mutex);

        // Always check '_activeTxnNumber' and '_txnState', since they can be modified by session
        // kill and migration, which do not check out the session.
        _checkIsActiveTransaction(lg, *opCtx->getTxnNumber(), false);
        // Throw NoSuchTransaction error instead of TransactionAborted error since this is the entry
        // point of transaction execution.
        uassert(ErrorCodes::NoSuchTransaction,
                str::stream() << "Transaction " << *opCtx->getTxnNumber() << " has been aborted.",
                _txnState != MultiDocumentTransactionState::kAborted);

        // Cannot change committed transaction but allow retrying commitTransaction command.
        uassert(ErrorCodes::TransactionCommitted,
                str::stream() << "Transaction " << *opCtx->getTxnNumber() << " has been committed.",
                cmdName == "commitTransaction" ||
                    _txnState != MultiDocumentTransactionState::kCommitted);

        if (_txnResourceStash) {
            // Transaction resources already exist for this transaction.  Transfer them from the
            // stash to the operation context.
            invariant(_txnState != MultiDocumentTransactionState::kNone);

            auto& readConcernArgs = repl::ReadConcernArgs::get(opCtx);
            uassert(ErrorCodes::InvalidOptions,
                    "Only the first command in a transaction may specify a readConcern",
                    readConcernArgs.isEmpty());
            _txnResourceStash->release(opCtx);
            _txnResourceStash = boost::none;
            // Set the starting active time for this transaction.
            if (_txnState == MultiDocumentTransactionState::kInProgress) {
                stdx::lock_guard<stdx::mutex> ls(_statsMutex);
                _singleTransactionStats.setActive(curTimeMicros64());
            }
            // We accept possible slight inaccuracies in these counters from non-atomicity.
            ServerTransactionsMetrics::get(opCtx)->incrementCurrentActive();
            ServerTransactionsMetrics::get(opCtx)->decrementCurrentInactive();
            return;
        }

        // Stashed transaction resources do not exist for this transaction.
        if (_txnState != MultiDocumentTransactionState::kInProgress) {
            return;
        }

        // Set speculative execution.
        const auto& readConcernArgs = repl::ReadConcernArgs::get(opCtx);
        const bool speculative =
            readConcernArgs.getLevel() == repl::ReadConcernLevel::kSnapshotReadConcern &&
            !readConcernArgs.getArgsAtClusterTime();
        // Only set speculative on primary.
        if (opCtx->writesAreReplicated() && speculative) {
            _setSpeculativeTransactionOpTime(lg,
                                             opCtx,
                                             readConcernArgs.getOriginalLevel() ==
                                                     repl::ReadConcernLevel::kSnapshotReadConcern
                                                 ? SpeculativeTransactionOpTime::kAllCommitted
                                                 : SpeculativeTransactionOpTime::kLastApplied);
        }

        // If this is a multi-document transaction, set up the transaction resources on the opCtx.
        opCtx->setWriteUnitOfWork(std::make_unique<WriteUnitOfWork>(opCtx));

        // If maxTransactionLockRequestTimeoutMillis is set, then we will ensure no
        // future lock request waits longer than maxTransactionLockRequestTimeoutMillis
        // to acquire a lock. This is to avoid deadlocks and minimize non-transaction
        // operation performance degradations.
        auto maxTransactionLockMillis = maxTransactionLockRequestTimeoutMillis.load();
        if (maxTransactionLockMillis >= 0) {
            opCtx->lockState()->setMaxLockTimeout(Milliseconds(maxTransactionLockMillis));
        }
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
    opCtx->recoveryUnit()->preallocateSnapshot();

    // The Client lock must not be held when executing this failpoint as it will block currentOp
    // execution.
    if (MONGO_FAIL_POINT(hangAfterPreallocateSnapshot)) {
        CurOpFailpointHelpers::waitWhileFailPointEnabled(
            &hangAfterPreallocateSnapshot, opCtx, "hangAfterPreallocateSnapshot");
    }

    {
        stdx::lock_guard<stdx::mutex> lg(_mutex);
        stdx::lock_guard<stdx::mutex> ls(_statsMutex);
        _singleTransactionStats.setActive(curTimeMicros64());
        ServerTransactionsMetrics::get(getGlobalServiceContext())->incrementCurrentActive();
        ServerTransactionsMetrics::get(getGlobalServiceContext())->decrementCurrentInactive();
    }
}

void Session::abortArbitraryTransaction() {
    stdx::lock_guard<stdx::mutex> lock(_mutex);

    if (_txnState != MultiDocumentTransactionState::kInProgress) {
        return;
    }

    _abortTransaction(lock);
}

void Session::abortArbitraryTransactionIfExpired() {
    stdx::lock_guard<stdx::mutex> lock(_mutex);
    if (_txnState != MultiDocumentTransactionState::kInProgress || !_transactionExpireDate ||
        _transactionExpireDate >= Date_t::now()) {
        return;
    }

    if (_currentOperation) {
        // If an operation is still running for this transaction when it expires, kill the currently
        // running operation.
        stdx::lock_guard<Client> clientLock(*_currentOperation->getClient());
        getGlobalServiceContext()->killOperation(_currentOperation, ErrorCodes::ExceededTimeLimit);
    }

    // Log after killing the current operation because jstests may wait to see this log message to
    // imply that the operation has been killed.
    log() << "Aborting transaction with txnNumber " << _activeTxnNumber << " on session with lsid "
          << _sessionId.getId()
          << " because it has been running for longer than 'transactionLifetimeLimitSeconds'";

    _abortTransaction(lock);
}

void Session::abortActiveTransaction(OperationContext* opCtx) {
    stdx::lock_guard<stdx::mutex> lock(_mutex);

    invariant(!_txnResourceStash);
    if (_txnState != MultiDocumentTransactionState::kInProgress) {
        return;
    }

    _abortTransaction(lock);
    {
        stdx::lock_guard<Client> clientLock(*opCtx->getClient());
        // Abort the WUOW. We should be able to abort empty transactions that don't have WUOW.
        if (opCtx->getWriteUnitOfWork()) {
            opCtx->setWriteUnitOfWork(nullptr);
        }
        // We must clear the recovery unit and locker so any post-transaction writes can run without
        // transactional settings such as a read timestamp.
        opCtx->setRecoveryUnit(opCtx->getServiceContext()->getStorageEngine()->newRecoveryUnit(),
                               WriteUnitOfWork::RecoveryUnitState::kNotInUnitOfWork);
        opCtx->lockState()->unsetMaxLockTimeout();
    }
    {
        stdx::lock_guard<stdx::mutex> ls(_statsMutex);
        // Add the latest operation stats to the aggregate OpDebug object stored in the
        // SingleTransactionStats instance on the Session.
        _singleTransactionStats.getOpDebug()->additiveMetrics.add(
            CurOp::get(opCtx)->debug().additiveMetrics);

        // If there are valid storage statistics for this operation, put those in the
        // SingleTransactionStats instance either by creating a new storageStats instance or by
        // adding into an existing storageStats instance stored in SingleTransactionStats.
        std::shared_ptr<StorageStats> storageStats =
            opCtx->recoveryUnit()->getOperationStatistics();
        if (storageStats) {
            CurOp::get(opCtx)->debug().storageStats = storageStats;
            if (!_singleTransactionStats.getOpDebug()->storageStats) {
                _singleTransactionStats.getOpDebug()->storageStats = storageStats->getCopy();
            } else {
                *_singleTransactionStats.getOpDebug()->storageStats += *storageStats;
            }
        }

        // Update the LastClientInfo object stored in the SingleTransactionStats instance on the
        // Session with this Client's information.
        _singleTransactionStats.updateLastClientInfo(opCtx->getClient());
    }

    // Log the transaction if its duration is longer than the slowMS command threshold.
    _logSlowTransaction(
        lock,
        &(opCtx->lockState()->getLockerInfo(CurOp::get(opCtx)->getLockStatsBase()))->stats,
        MultiDocumentTransactionState::kAborted,
        repl::ReadConcernArgs::get(opCtx));
}

void Session::_abortTransaction(WithLock wl) {
    // TODO SERVER-33432 Disallow aborting committed transaction after we implement implicit abort.
    // A transaction in kCommitting state will either commit or abort for storage-layer reasons; it
    // is too late to abort externally.
    if (_txnState == MultiDocumentTransactionState::kCommitting ||
        _txnState == MultiDocumentTransactionState::kCommitted) {
        return;
    }

    auto curTime = curTimeMicros64();
    if (_txnState == MultiDocumentTransactionState::kInProgress) {
        stdx::lock_guard<stdx::mutex> ls(_statsMutex);
        _singleTransactionStats.setEndTime(curTime);
        if (_singleTransactionStats.isActive()) {
            _singleTransactionStats.setInactive(curTime);
        }
    }

    // If the transaction is stashed, then we have aborted an inactive transaction.
    if (_txnResourceStash) {
        _logSlowTransaction(wl,
                            &(_txnResourceStash->locker()->getLockerInfo(boost::none))->stats,
                            MultiDocumentTransactionState::kAborted,
                            _txnResourceStash->getReadConcernArgs());
        ServerTransactionsMetrics::get(getGlobalServiceContext())->decrementCurrentInactive();
    } else {
        ServerTransactionsMetrics::get(getGlobalServiceContext())->decrementCurrentActive();
    }
    _txnResourceStash = boost::none;
    _transactionOperationBytes = 0;
    _transactionOperations.clear();
    _txnState = MultiDocumentTransactionState::kAborted;
    _speculativeTransactionReadOpTime = repl::OpTime();
    ServerTransactionsMetrics::get(getGlobalServiceContext())->incrementTotalAborted();
    ServerTransactionsMetrics::get(getGlobalServiceContext())->decrementCurrentOpen();

    Top::get(getGlobalServiceContext())
        .incrementGlobalTransactionLatencyStats(_singleTransactionStats.getDuration(curTime));
}

void Session::_beginOrContinueTxnOnMigration(WithLock wl, TxnNumber txnNumber) {
    _checkValid(wl);
    _checkTxnValid(wl, txnNumber);

    // Check for continuing an existing transaction
    if (txnNumber == _activeTxnNumber)
        return;

    _setActiveTxn(wl, txnNumber);
}

void Session::_setActiveTxn(WithLock wl, TxnNumber txnNumber) {
    // Abort the existing transaction if it's not committed or aborted.
    if (_txnState == MultiDocumentTransactionState::kInProgress) {
        _abortTransaction(wl);
    }
    _activeTxnNumber = txnNumber;
    _activeTxnCommittedStatements.clear();
    _hasIncompleteHistory = false;
    _txnState = MultiDocumentTransactionState::kNone;
    {
        stdx::lock_guard<stdx::mutex> ls(_statsMutex);
        _singleTransactionStats = SingleTransactionStats(txnNumber);
    }
    _speculativeTransactionReadOpTime = repl::OpTime();
    _multikeyPathInfo.clear();
}

void Session::addTransactionOperation(OperationContext* opCtx,
                                      const repl::ReplOperation& operation) {
    stdx::lock_guard<stdx::mutex> lk(_mutex);

    // Always check '_activeTxnNumber' and '_txnState', since they can be modified by session kill
    // and migration, which do not check out the session.
    _checkIsActiveTransaction(lk, *opCtx->getTxnNumber(), true);

    invariant(_txnState == MultiDocumentTransactionState::kInProgress);
    invariant(!_autocommit && _activeTxnNumber != kUninitializedTxnNumber);
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

std::vector<repl::ReplOperation> Session::endTransactionAndRetrieveOperations(
    OperationContext* opCtx) {
    stdx::lock_guard<stdx::mutex> lk(_mutex);

    // Always check '_activeTxnNumber' and '_txnState', since they can be modified by session kill
    // and migration, which do not check out the session.
    _checkIsActiveTransaction(lk, *opCtx->getTxnNumber(), true);

    invariant(!_autocommit);
    _transactionOperationBytes = 0;
    return std::move(_transactionOperations);
}

void Session::commitTransaction(OperationContext* opCtx) {
    stdx::unique_lock<stdx::mutex> lk(_mutex);

    // Always check '_activeTxnNumber' and '_txnState', since they can be modified by session kill
    // and migration, which do not check out the session.
    _checkIsActiveTransaction(lk, *opCtx->getTxnNumber(), true);

    invariant(_txnState != MultiDocumentTransactionState::kCommitted);
    _commitTransaction(std::move(lk), opCtx);
}

void Session::_commitTransaction(stdx::unique_lock<stdx::mutex> lk, OperationContext* opCtx) {
    invariant(_txnState == MultiDocumentTransactionState::kInProgress);
    const size_t operationCount = _transactionOperations.size();
    const size_t oplogOperationBytes = _transactionOperationBytes;

    // We need to unlock the session to run the opObserver onTransactionCommit, which calls back
    // into the session.
    lk.unlock();
    auto opObserver = opCtx->getServiceContext()->getOpObserver();
    invariant(opObserver);
    opObserver->onTransactionCommit(opCtx);
    lk.lock();

    // It's possible some other thread aborted the transaction (e.g. through killSession) while the
    // opObserver was running.  If that happened, the commit should be reported as failed.
    uassert(ErrorCodes::NoSuchTransaction,
            str::stream() << "Transaction " << opCtx->getTxnNumber()
                          << " aborted while attempting to commit",
            _txnState == MultiDocumentTransactionState::kInProgress &&
                _activeTxnNumber == opCtx->getTxnNumber());
    _txnState = MultiDocumentTransactionState::kCommitting;
    bool committed = false;
    ON_BLOCK_EXIT([this, &committed, opCtx]() {
        // If we're still "committing", the recovery unit failed to commit, and the lock is not
        // held.  We can't safely use _txnState here, as it is protected by the lock.
        if (!committed) {
            stdx::lock_guard<stdx::mutex> lk(_mutex);
            opCtx->setWriteUnitOfWork(nullptr);
            // Make sure the transaction didn't change because of chunk migration.
            if (opCtx->getTxnNumber() == _activeTxnNumber) {
                _txnState = MultiDocumentTransactionState::kAborted;
                ServerTransactionsMetrics::get(getGlobalServiceContext())->decrementCurrentActive();
                // After the transaction has been aborted, we must update the end time and mark it
                // as inactive.
                auto curTime = curTimeMicros64();
                ServerTransactionsMetrics::get(opCtx)->incrementTotalAborted();
                ServerTransactionsMetrics::get(opCtx)->decrementCurrentOpen();
                {
                    stdx::lock_guard<stdx::mutex> ls(_statsMutex);
                    _singleTransactionStats.setEndTime(curTime);
                    if (_singleTransactionStats.isActive()) {
                        _singleTransactionStats.setInactive(curTime);
                    }
                    // Add the latest operation stats to the aggregate OpDebug object stored in the
                    // SingleTransactionStats instance on the Session.
                    _singleTransactionStats.getOpDebug()->additiveMetrics.add(
                        CurOp::get(opCtx)->debug().additiveMetrics);

                    // If there are valid storage statistics for this operation, put those in the
                    // SingleTransactionStats instance either by creating a new storageStats
                    // instance or by adding into an existing storageStats instance stored in
                    // SingleTransactionStats.
                    std::shared_ptr<StorageStats> storageStats =
                        opCtx->recoveryUnit()->getOperationStatistics();
                    if (storageStats) {
                        CurOp::get(opCtx)->debug().storageStats = storageStats;
                        if (!_singleTransactionStats.getOpDebug()->storageStats) {
                            _singleTransactionStats.getOpDebug()->storageStats =
                                storageStats->getCopy();
                        } else {
                            *_singleTransactionStats.getOpDebug()->storageStats += *storageStats;
                        }
                    }

                    // Update the LastClientInfo object stored in the SingleTransactionStats
                    // instance on the Session with this Client's information.
                    _singleTransactionStats.updateLastClientInfo(opCtx->getClient());
                }

                // Log the transaction if its duration is longer than the slowMS command threshold.
                _logSlowTransaction(
                    lk,
                    &(opCtx->lockState()->getLockerInfo(CurOp::get(opCtx)->getLockStatsBase()))
                         ->stats,
                    MultiDocumentTransactionState::kAborted,
                    repl::ReadConcernArgs::get(opCtx));
            }
        }
        // We must clear the recovery unit and locker so any post-transaction writes can run without
        // transactional settings such as a read timestamp.
        opCtx->setRecoveryUnit(opCtx->getServiceContext()->getStorageEngine()->newRecoveryUnit(),
                               WriteUnitOfWork::RecoveryUnitState::kNotInUnitOfWork);
        opCtx->lockState()->unsetMaxLockTimeout();
        _commitcv.notify_all();
    });
    lk.unlock();
    opCtx->getWriteUnitOfWork()->commit();
    opCtx->setWriteUnitOfWork(nullptr);
    committed = true;
    lk.lock();
    auto& clientInfo = repl::ReplClientInfo::forClient(opCtx->getClient());
    // If no writes have been done, set the client optime forward to the read timestamp so waiting
    // for write concern will ensure all read data was committed.
    //
    // TODO(SERVER-34881): Once the default read concern is speculative majority, only set the
    // client optime forward if the original read concern level is "majority" or "snapshot".
    if (_speculativeTransactionReadOpTime > clientInfo.getLastOp()) {
        clientInfo.setLastOp(_speculativeTransactionReadOpTime);
    }
    _txnState = MultiDocumentTransactionState::kCommitted;
    // After the transaction has been committed, we must update the end time and mark it as
    // inactive.
    ServerTransactionsMetrics::get(opCtx)->incrementTotalCommitted();
    ServerTransactionsMetrics::get(opCtx)->decrementCurrentOpen();
    ServerTransactionsMetrics::get(opCtx)->decrementCurrentActive();
    ServerTransactionsMetrics::get(opCtx)->updateLastTransaction(
        operationCount,
        oplogOperationBytes,
        opCtx->getWriteConcern().usedDefault ? BSONObj() : opCtx->getWriteConcern().toBSON());
    auto curTime = curTimeMicros64();
    Top::get(getGlobalServiceContext())
        .incrementGlobalTransactionLatencyStats(_singleTransactionStats.getDuration(curTime));

    {
        stdx::lock_guard<stdx::mutex> ls(_statsMutex);
        _singleTransactionStats.setEndTime(curTime);
        if (_singleTransactionStats.isActive()) {
            _singleTransactionStats.setInactive(curTime);
        }

        // Add the latest operation stats to the aggregate OpDebug object stored in the
        // SingleTransactionStats instance on the Session.
        _singleTransactionStats.getOpDebug()->additiveMetrics.add(
            CurOp::get(opCtx)->debug().additiveMetrics);

        // If there are valid storage statistics for this operation, put those in the
        // SingleTransactionStats instance either by creating a new storageStats instance or by
        // adding into an existing storageStats instance stored in SingleTransactionStats.
        std::shared_ptr<StorageStats> storageStats =
            opCtx->recoveryUnit()->getOperationStatistics();
        if (storageStats) {
            CurOp::get(opCtx)->debug().storageStats = storageStats;
            if (!_singleTransactionStats.getOpDebug()->storageStats) {
                _singleTransactionStats.getOpDebug()->storageStats = storageStats->getCopy();
            } else {
                *_singleTransactionStats.getOpDebug()->storageStats += *storageStats;
            }
        }

        // Update the LastClientInfo object stored in the SingleTransactionStats instance on the
        // Session with this Client's information.
        _singleTransactionStats.updateLastClientInfo(opCtx->getClient());
    }

    // Log the transaction if its duration is longer than the slowMS command threshold.
    _logSlowTransaction(
        lk,
        &(opCtx->lockState()->getLockerInfo(CurOp::get(opCtx)->getLockStatsBase()))->stats,
        MultiDocumentTransactionState::kCommitted,
        repl::ReadConcernArgs::get(opCtx));
}

BSONObj Session::reportStashedState() const {
    BSONObjBuilder builder;
    reportStashedState(&builder);
    return builder.obj();
}

void Session::reportStashedState(BSONObjBuilder* builder) const {
    stdx::lock_guard<stdx::mutex> ls(_mutex);

    if (_txnResourceStash && _txnResourceStash->locker()) {
        if (auto lockerInfo = _txnResourceStash->locker()->getLockerInfo(boost::none)) {
            invariant(_activeTxnNumber != kUninitializedTxnNumber);
            builder->append("host", getHostNameCachedAndPort());
            builder->append("desc", "inactive transaction");
            auto lastClientInfo = _singleTransactionStats.getLastClientInfo();
            builder->append("client", lastClientInfo.clientHostAndPort);
            builder->append("connectionId", lastClientInfo.connectionId);
            builder->append("appName", lastClientInfo.appName);
            builder->append("clientMetadata", lastClientInfo.clientMetadata);
            {
                BSONObjBuilder lsid(builder->subobjStart("lsid"));
                getSessionId().serialize(&lsid);
            }
            BSONObjBuilder transactionBuilder;
            _reportTransactionStats(
                ls, &transactionBuilder, _txnResourceStash->getReadConcernArgs());
            builder->append("transaction", transactionBuilder.obj());
            builder->append("waitingForLock", false);
            builder->append("active", false);
            fillLockerInfo(*lockerInfo, *builder);
        }
    }
}

void Session::reportUnstashedState(repl::ReadConcernArgs readConcernArgs,
                                   BSONObjBuilder* builder) const {
    stdx::lock_guard<stdx::mutex> ls(_statsMutex);

    // This method may only take the stats mutex, as it is called with the Client mutex held.  So we
    // cannot check the stashed state directly.  Instead, a transaction is considered unstashed if
    // it isnot actually a transaction (retryable write, no stash used), or is active (not stashed)
    if (!_singleTransactionStats.isForMultiDocumentTransaction() ||
        _singleTransactionStats.isActive() || _singleTransactionStats.isEnded()) {
        BSONObjBuilder transactionBuilder;
        _reportTransactionStats(ls, &transactionBuilder, readConcernArgs);
        builder->append("transaction", transactionBuilder.obj());
    }
}

void Session::_reportTransactionStats(WithLock wl,
                                      BSONObjBuilder* builder,
                                      repl::ReadConcernArgs readConcernArgs) const {
    _singleTransactionStats.report(builder, readConcernArgs);
}

std::string Session::_transactionInfoForLog(const SingleThreadedLockStats* lockStats,
                                            MultiDocumentTransactionState terminationCause,
                                            repl::ReadConcernArgs readConcernArgs) {
    invariant(lockStats);
    invariant(terminationCause == MultiDocumentTransactionState::kCommitted ||
              terminationCause == MultiDocumentTransactionState::kAborted);

    StringBuilder s;

    // User specified transaction parameters.
    BSONObjBuilder parametersBuilder;
    BSONObjBuilder lsidBuilder(parametersBuilder.subobjStart("lsid"));
    _sessionId.serialize(&lsidBuilder);
    lsidBuilder.doneFast();
    parametersBuilder.append("txnNumber", _activeTxnNumber);
    parametersBuilder.append("autocommit", _autocommit);
    readConcernArgs.appendInfo(&parametersBuilder);
    s << "parameters:" << parametersBuilder.obj().toString() << ",";

    s << " readTimestamp:" << _speculativeTransactionReadOpTime.getTimestamp().toString() << ",";

    s << _singleTransactionStats.getOpDebug()->additiveMetrics.report();

    std::string terminationCauseString =
        terminationCause == MultiDocumentTransactionState::kCommitted ? "committed" : "aborted";
    s << " terminationCause:" << terminationCauseString;

    auto curTime = curTimeMicros64();
    s << " timeActiveMicros:"
      << durationCount<Microseconds>(_singleTransactionStats.getTimeActiveMicros(curTime));
    s << " timeInactiveMicros:"
      << durationCount<Microseconds>(_singleTransactionStats.getTimeInactiveMicros(curTime));

    // Number of yields is always 0 in multi-document transactions, but it is included mainly to
    // match the format with other slow operation logging messages.
    s << " numYields:" << 0;

    // Aggregate lock statistics.
    BSONObjBuilder locks;
    lockStats->report(&locks);
    s << " locks:" << locks.obj().toString();

    if (_singleTransactionStats.getOpDebug()->storageStats)
        s << " storage:" << _singleTransactionStats.getOpDebug()->storageStats->toBSON().toString();

    // Total duration of the transaction.
    s << " "
      << Milliseconds{static_cast<long long>(_singleTransactionStats.getDuration(curTime)) / 1000};

    return s.str();
}

void Session::_logSlowTransaction(WithLock wl,
                                  const SingleThreadedLockStats* lockStats,
                                  MultiDocumentTransactionState terminationCause,
                                  repl::ReadConcernArgs readConcernArgs) {
    // Only log multi-document transactions.
    if (_txnState != MultiDocumentTransactionState::kNone) {
        // Log the transaction if its duration is longer than the slowMS command threshold.
        if (shouldLog(logger::LogComponent::kTransaction, logger::LogSeverity::Debug(1)) ||
            _singleTransactionStats.getDuration(curTimeMicros64()) >
                serverGlobalParams.slowMS * 1000ULL) {
            log(logger::LogComponent::kTransaction)
                << "transaction "
                << _transactionInfoForLog(lockStats, terminationCause, readConcernArgs);
        }
    }
}

void Session::_checkValid(WithLock) const {
    uassert(ErrorCodes::ConflictingOperationInProgress,
            str::stream() << "Session " << getSessionId()
                          << " was concurrently modified and the operation must be retried.",
            _isValid);
}

void Session::_checkIsActiveTransaction(WithLock, TxnNumber txnNumber, bool checkAbort) const {
    uassert(ErrorCodes::ConflictingOperationInProgress,
            str::stream() << "Cannot perform operations on transaction " << txnNumber
                          << " on session "
                          << getSessionId()
                          << " because a different transaction "
                          << _activeTxnNumber
                          << " is now active.",
            txnNumber == _activeTxnNumber);

    uassert(ErrorCodes::NoSuchTransaction,
            str::stream() << "Transaction " << txnNumber << " has been aborted.",
            !checkAbort || _txnState != MultiDocumentTransactionState::kAborted);
}

boost::optional<repl::OpTime> Session::_checkStatementExecuted(WithLock wl,
                                                               TxnNumber txnNumber,
                                                               StmtId stmtId) const {
    _checkValid(wl);
    _checkIsActiveTransaction(wl, txnNumber, false);
    // Retries are not detected for multi-document transactions.
    if (_txnState == MultiDocumentTransactionState::kInProgress)
        return boost::none;

    const auto it = _activeTxnCommittedStatements.find(stmtId);
    if (it == _activeTxnCommittedStatements.end()) {
        uassert(ErrorCodes::IncompleteTransactionHistory,
                str::stream() << "Incomplete history detected for transaction " << txnNumber
                              << " on session "
                              << _sessionId.toBSON(),
                !_hasIncompleteHistory);

        return boost::none;
    }

    invariant(_lastWrittenSessionRecord);
    invariant(_lastWrittenSessionRecord->getTxnNum() == txnNumber);

    return it->second;
}

Date_t Session::_getLastWriteDate(WithLock wl, TxnNumber txnNumber) const {
    _checkValid(wl);
    _checkIsActiveTransaction(wl, txnNumber, false);

    if (!_lastWrittenSessionRecord || _lastWrittenSessionRecord->getTxnNum() != txnNumber)
        return {};

    return _lastWrittenSessionRecord->getLastWriteDate();
}

UpdateRequest Session::_makeUpdateRequest(WithLock,
                                          TxnNumber newTxnNumber,
                                          const repl::OpTime& newLastWriteOpTime,
                                          Date_t newLastWriteDate) const {
    UpdateRequest updateRequest(NamespaceString::kSessionTransactionsTableNamespace);

    const auto updateBSON = [&] {
        SessionTxnRecord newTxnRecord;
        newTxnRecord.setSessionId(_sessionId);
        newTxnRecord.setTxnNum(newTxnNumber);
        newTxnRecord.setLastWriteOpTime(newLastWriteOpTime);
        newTxnRecord.setLastWriteDate(newLastWriteDate);
        return newTxnRecord.toBSON();
    }();
    updateRequest.setUpdates(updateBSON);
    updateRequest.setQuery(BSON(SessionTxnRecord::kSessionIdFieldName << _sessionId.toBSON()));
    updateRequest.setUpsert(true);

    return updateRequest;
}

void Session::_registerUpdateCacheOnCommit(OperationContext* opCtx,
                                           TxnNumber newTxnNumber,
                                           std::vector<StmtId> stmtIdsWritten,
                                           const repl::OpTime& lastStmtIdWriteOpTime) {
    opCtx->recoveryUnit()->onCommit(
        [ this, newTxnNumber, stmtIdsWritten = std::move(stmtIdsWritten), lastStmtIdWriteOpTime ](
            boost::optional<Timestamp>) {
            RetryableWritesStats::get(getGlobalServiceContext())
                ->incrementTransactionsCollectionWriteCount();

            stdx::lock_guard<stdx::mutex> lg(_mutex);

            if (!_isValid)
                return;

            // The cache of the last written record must always be advanced after a write so that
            // subsequent writes have the correct point to start from.
            if (!_lastWrittenSessionRecord) {
                _lastWrittenSessionRecord.emplace();

                _lastWrittenSessionRecord->setSessionId(_sessionId);
                _lastWrittenSessionRecord->setTxnNum(newTxnNumber);
                _lastWrittenSessionRecord->setLastWriteOpTime(lastStmtIdWriteOpTime);
            } else {
                if (newTxnNumber > _lastWrittenSessionRecord->getTxnNum())
                    _lastWrittenSessionRecord->setTxnNum(newTxnNumber);

                if (lastStmtIdWriteOpTime > _lastWrittenSessionRecord->getLastWriteOpTime())
                    _lastWrittenSessionRecord->setLastWriteOpTime(lastStmtIdWriteOpTime);
            }

            if (newTxnNumber > _activeTxnNumber) {
                // This call is necessary in order to advance the txn number and reset the cached
                // state in the case where just before the storage transaction commits, the cache
                // entry gets invalidated and immediately refreshed while there were no writes for
                // newTxnNumber yet. In this case _activeTxnNumber will be less than newTxnNumber
                // and we will fail to update the cache even though the write was successful.
                _beginOrContinueTxn(lg, newTxnNumber, boost::none, boost::none);
            }

            if (newTxnNumber == _activeTxnNumber) {
                for (const auto stmtId : stmtIdsWritten) {
                    if (stmtId == kIncompleteHistoryStmtId) {
                        _hasIncompleteHistory = true;
                        continue;
                    }

                    const auto insertRes =
                        _activeTxnCommittedStatements.emplace(stmtId, lastStmtIdWriteOpTime);
                    if (!insertRes.second) {
                        const auto& existingOpTime = insertRes.first->second;
                        fassertOnRepeatedExecution(_sessionId,
                                                   newTxnNumber,
                                                   stmtId,
                                                   existingOpTime,
                                                   lastStmtIdWriteOpTime);
                    }
                }
            }
        });

    MONGO_FAIL_POINT_BLOCK(onPrimaryTransactionalWrite, customArgs) {
        const auto& data = customArgs.getData();

        const auto closeConnectionElem = data["closeConnection"];
        if (closeConnectionElem.eoo() || closeConnectionElem.Bool()) {
            opCtx->getClient()->session()->end();
        }

        const auto failBeforeCommitExceptionElem = data["failBeforeCommitExceptionCode"];
        if (!failBeforeCommitExceptionElem.eoo()) {
            const auto failureCode = ErrorCodes::Error(int(failBeforeCommitExceptionElem.Number()));
            uasserted(failureCode,
                      str::stream() << "Failing write for " << _sessionId << ":" << newTxnNumber
                                    << " due to failpoint. The write must not be reflected.");
        }
    }
}

boost::optional<repl::OplogEntry> Session::createMatchingTransactionTableUpdate(
    const repl::OplogEntry& entry) {
    auto sessionInfo = entry.getOperationSessionInfo();
    if (!sessionInfo.getTxnNumber()) {
        return boost::none;
    }

    invariant(sessionInfo.getSessionId());
    invariant(entry.getWallClockTime());

    const auto updateBSON = [&] {
        SessionTxnRecord newTxnRecord;
        newTxnRecord.setSessionId(*sessionInfo.getSessionId());
        newTxnRecord.setTxnNum(*sessionInfo.getTxnNumber());
        newTxnRecord.setLastWriteOpTime(entry.getOpTime());
        newTxnRecord.setLastWriteDate(*entry.getWallClockTime());
        return newTxnRecord.toBSON();
    }();

    return repl::OplogEntry(
        entry.getOpTime(),
        0,  // hash
        repl::OpTypeEnum::kUpdate,
        NamespaceString::kSessionTransactionsTableNamespace,
        boost::none,  // uuid
        false,        // fromMigrate
        repl::OplogEntry::kOplogVersion,
        updateBSON,
        BSON(SessionTxnRecord::kSessionIdFieldName << sessionInfo.getSessionId()->toBSON()),
        {},    // sessionInfo
        true,  // upsert
        *entry.getWallClockTime(),
        boost::none,  // statementId
        boost::none,  // prevWriteOpTime
        boost::none,  // preImangeOpTime
        boost::none   // postImageOpTime
        );
}

}  // namespace mongo
