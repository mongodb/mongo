/**
 *    Copyright (C) 2017 MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
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

#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/ops/update.h"
#include "mongo/db/query/get_executor.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/transaction_history_iterator.h"
#include "mongo/stdx/memory.h"
#include "mongo/transport/transport_layer.h"
#include "mongo/util/fail_point_service.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {
namespace {

void fassertOnRepeatedExecution(OperationContext* opCtx,
                                const LogicalSessionId& lsid,
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
            fassertOnRepeatedExecution(opCtx,
                                       lsid,
                                       result.lastTxnRecord->getTxnNum(),
                                       *entry.getStatementId(),
                                       existingOpTime,
                                       entry.getOpTime());
        }
    }

    return result;
}

void updateSessionEntry(OperationContext* opCtx, const UpdateRequest& updateRequest) {
    AutoGetCollection autoColl(opCtx, NamespaceString::kSessionTransactionsTableNamespace, MODE_IX);

    uassert(40527,
            str::stream() << "Unable to persist transaction state because the session transaction "
                             "collection is missing. This indicates that the "
                          << NamespaceString::kSessionTransactionsTableNamespace.ns()
                          << " collection has been manually deleted.",
            autoColl.getCollection());

    try {
        const auto updateResult = update(opCtx, autoColl.getDb(), updateRequest);

        if (!updateResult.numDocsModified && updateResult.upserted.isEmpty()) {
            throw WriteConflictException();
        }
    } catch (const DBException& excep) {
        if (excep.code() == ErrorCodes::DuplicateKey) {
            // Duplicate key means that another thread already created the session this is trying
            // to upsert. Throw WriteCoflict to make it retry and check the current state again.
            throw WriteConflictException();
        }

        throw;
    }
}

// Failpoint which allows different failure actions to happen after each write. Supports the
// parameters below, which can be combined with each other (unless explicitly disallowed):
//
// closeConnection (bool, default = true): Closes the connection on which the write was executed.
// failBeforeCommitExceptionCode (int, default = not specified): If set, the specified exception
//      code will be thrown, which will cause the write to not commit; if not specified, the write
//      will be allowed to commit.
MONGO_FP_DECLARE(onPrimaryTransactionalWrite);

}  // namespace

const BSONObj Session::kDeadEndSentinel(BSON("$incompleteOplogHistory" << 1));

Session::Session(LogicalSessionId sessionId) : _sessionId(std::move(sessionId)) {}

void Session::refreshFromStorageIfNeeded(OperationContext* opCtx) {
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
            }

            break;
        }
    }
}

void Session::beginTxn(OperationContext* opCtx, TxnNumber txnNumber) {
    invariant(!opCtx->lockState()->isLocked());

    stdx::lock_guard<stdx::mutex> lg(_mutex);
    _beginTxn(lg, txnNumber);
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
                opCtx, _sessionId, txnNumber, stmtId, *stmtOpTime, lastStmtIdWriteOpTime);
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

void Session::updateSessionRecordOnSecondary(OperationContext* opCtx,
                                             const SessionTxnRecord& sessionTxnRecord) {
    invariant(!opCtx->lockState()->isLocked());

    writeConflictRetry(
        opCtx, "Update session txn", NamespaceString::kSessionTransactionsTableNamespace.ns(), [&] {
            UpdateRequest updateRequest(NamespaceString::kSessionTransactionsTableNamespace);
            updateRequest.setQuery(BSON(SessionTxnRecord::kSessionIdFieldName
                                        << sessionTxnRecord.getSessionId().toBSON()));
            updateRequest.setUpdates(sessionTxnRecord.toBSON());
            updateRequest.setUpsert(true);

            repl::UnreplicatedWritesBlock doNotReplicateWrites(opCtx);

            Lock::DBLock configDBLock(opCtx, NamespaceString::kConfigDb, MODE_IX);
            WriteUnitOfWork wuow(opCtx);
            updateSessionEntry(opCtx, updateRequest);
            wuow.commit();
        });
}

void Session::invalidate() {
    stdx::lock_guard<stdx::mutex> lg(_mutex);
    _isValid = false;
    _numInvalidations++;

    _lastWrittenSessionRecord.reset();

    _activeTxnNumber = kUninitializedTxnNumber;
    _activeTxnCommittedStatements.clear();
    _hasIncompleteHistory = false;
}

repl::OpTime Session::getLastWriteOpTime(TxnNumber txnNumber) const {
    stdx::lock_guard<stdx::mutex> lg(_mutex);
    _checkValid(lg);
    _checkIsActiveTransaction(lg, txnNumber);

    if (!_lastWrittenSessionRecord || _lastWrittenSessionRecord->getTxnNum() != txnNumber)
        return {};

    return _lastWrittenSessionRecord->getLastWriteOpTime();
}

boost::optional<repl::OplogEntry> Session::checkStatementExecuted(OperationContext* opCtx,
                                                                  TxnNumber txnNumber,
                                                                  StmtId stmtId) const {
    const auto stmtTimestamp = [&] {
        stdx::lock_guard<stdx::mutex> lg(_mutex);
        auto result = _checkStatementExecuted(lg, txnNumber, stmtId);

        if (!result) {
            uassert(ErrorCodes::IncompleteTransactionHistory,
                    str::stream() << "incomplete history detected for lsid: " << _sessionId.toBSON()
                                  << ", txnNum: "
                                  << txnNumber,
                    !_hasIncompleteHistory);
        }

        return result;
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

void Session::_beginTxn(WithLock wl, TxnNumber txnNumber) {
    _checkValid(wl);

    uassert(ErrorCodes::TransactionTooOld,
            str::stream() << "Cannot start transaction " << txnNumber << " on session "
                          << getSessionId()
                          << " because a newer transaction "
                          << _activeTxnNumber
                          << " has already started.",
            txnNumber >= _activeTxnNumber);

    // Check for continuing an existing transaction
    if (txnNumber == _activeTxnNumber)
        return;

    _activeTxnNumber = txnNumber;
    _activeTxnCommittedStatements.clear();
    _hasIncompleteHistory = false;
}

void Session::_checkValid(WithLock) const {
    uassert(ErrorCodes::ConflictingOperationInProgress,
            str::stream() << "Session " << getSessionId()
                          << " was concurrently modified and the operation must be retried.",
            _isValid);
}

void Session::_checkIsActiveTransaction(WithLock, TxnNumber txnNumber) const {
    uassert(ErrorCodes::ConflictingOperationInProgress,
            str::stream() << "Cannot perform retryability check for transaction " << txnNumber
                          << " on session "
                          << getSessionId()
                          << " because a different transaction "
                          << _activeTxnNumber
                          << " is now active.",
            txnNumber == _activeTxnNumber);
}

boost::optional<repl::OpTime> Session::_checkStatementExecuted(WithLock wl,
                                                               TxnNumber txnNumber,
                                                               StmtId stmtId) const {
    _checkValid(wl);
    _checkIsActiveTransaction(wl, txnNumber);

    const auto it = _activeTxnCommittedStatements.find(stmtId);
    if (it == _activeTxnCommittedStatements.end()) {
        return boost::none;
    }

    invariant(_lastWrittenSessionRecord);
    invariant(_lastWrittenSessionRecord->getTxnNum() == txnNumber);

    return it->second;
}

UpdateRequest Session::_makeUpdateRequest(WithLock,
                                          TxnNumber newTxnNumber,
                                          const repl::OpTime& newLastWriteOpTime,
                                          Date_t newLastWriteDate) const {
    UpdateRequest updateRequest(NamespaceString::kSessionTransactionsTableNamespace);

    if (_lastWrittenSessionRecord) {
        updateRequest.setQuery(BSON(SessionTxnRecord::kSessionIdFieldName
                                    << _sessionId.toBSON()
                                    << SessionTxnRecord::kTxnNumFieldName
                                    << _lastWrittenSessionRecord->getTxnNum()
                                    << SessionTxnRecord::kLastWriteOpTimeFieldName
                                    << _lastWrittenSessionRecord->getLastWriteOpTime()));
        updateRequest.setUpdates(BSON("$set" << BSON(SessionTxnRecord::kTxnNumFieldName
                                                     << newTxnNumber
                                                     << SessionTxnRecord::kLastWriteOpTimeFieldName
                                                     << newLastWriteOpTime
                                                     << SessionTxnRecord::kLastWriteDateFieldName
                                                     << newLastWriteDate)));
    } else {
        const auto updateBSON = [&] {
            SessionTxnRecord newTxnRecord;
            newTxnRecord.setSessionId(_sessionId);
            newTxnRecord.setTxnNum(newTxnNumber);
            newTxnRecord.setLastWriteOpTime(newLastWriteOpTime);
            newTxnRecord.setLastWriteDate(newLastWriteDate);
            return newTxnRecord.toBSON();
        }();

        updateRequest.setQuery(updateBSON);
        updateRequest.setUpdates(updateBSON);
        updateRequest.setUpsert(true);
    }

    return updateRequest;
}

void Session::_registerUpdateCacheOnCommit(OperationContext* opCtx,
                                           TxnNumber newTxnNumber,
                                           std::vector<StmtId> stmtIdsWritten,
                                           const repl::OpTime& lastStmtIdWriteOpTime) {
    opCtx->recoveryUnit()->onCommit([
        this,
        opCtx,
        newTxnNumber,
        stmtIdsWritten = std::move(stmtIdsWritten),
        lastStmtIdWriteOpTime
    ] {
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
            // This call is necessary in order to advance the txn number and reset the cached state
            // in the case where just before the storage transaction commits, the cache entry gets
            // invalidated and immediately refreshed while there were no writes for newTxnNumber
            // yet. In this case _activeTxnNumber will be less than newTxnNumber and we will fail to
            // update the cache even though the write was successful.
            _beginTxn(lg, newTxnNumber);
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
                    fassertOnRepeatedExecution(opCtx,
                                               _sessionId,
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
            auto transportSession = opCtx->getClient()->session();
            transportSession->getTransportLayer()->end(transportSession);
        }

        const auto failBeforeCommitExceptionElem = data["failBeforeCommitExceptionCode"];
        if (!failBeforeCommitExceptionElem.eoo()) {
            const auto failureCode =
                ErrorCodes::fromInt(int(failBeforeCommitExceptionElem.Number()));
            uasserted(failureCode,
                      str::stream() << "Failing write for " << _sessionId << ":" << newTxnNumber
                                    << " due to failpoint. The write must not be reflected.");
        }
    }
}

}  // namespace mongo
