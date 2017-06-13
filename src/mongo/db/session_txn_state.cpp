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

#include "mongo/platform/basic.h"

#include "mongo/db/db_raii.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/session_txn_state.h"

namespace mongo {

namespace {

auto getSessionTxnState = OperationContext::declareDecoration<SessionTxnState*>();

}  // unnamed namespace

const NamespaceString SessionTxnState::kConfigNS("admin.system.transactions");

SessionTxnState* SessionTxnState::get(OperationContext* opCtx) {
    return getSessionTxnState(opCtx);
}

void SessionTxnState::set(OperationContext* opCtx, SessionTxnState* txnState) {
    auto& sessionTxnState = getSessionTxnState(opCtx);
    sessionTxnState = txnState;
}

SessionTxnState::SessionTxnState(LogicalSessionId sessionId, TxnNumber txnNumber)
    : _sessionId(std::move(sessionId)), _txnNumber(std::move(txnNumber)) {}

void SessionTxnState::begin(OperationContext* opCtx) {
    if (!_isInitialized) {
        // load txn table state from storage
    }

    _isInitialized = true;
}

void SessionTxnState::addResultFromStorage(const SessionTxnRecord& txnRecord) {
    const auto& id = txnRecord.getId();
    invariant(id.getSessionId() == _sessionId);
    invariant(id.getTxnNum() == _txnNumber);

    _partialResults.insert(std::make_pair(id.getStmtId(), txnRecord.getResult()));
}

void SessionTxnState::storePartialResult(OperationContext* opCtx,
                                         StmtId stmtId,
                                         SingleWriteResult result,
                                         repl::OpTime opTime) {
    // Needs to be in the same write unit of work with the write for this result.
    invariant(opCtx->lockState()->inAWriteUnitOfWork());

    _partialResults.insert(std::make_pair(stmtId, std::move(result)));
    // TODO: update collection, then update _writeResults & _lastWriteOpTime
    // assert if cannot store -> could mean that a new txn started
}

const LogicalSessionId& SessionTxnState::getSessionId() const {
    return _sessionId;
}

const TxnNumber& SessionTxnState::getTxnNum() const {
    return _txnNumber;
}

const SessionTxnState::PartialResults& SessionTxnState::getPartialResults() const {
    return _partialResults;
}

void SessionTxnState::cleanUpOlderTransactions(OperationContext* opCtx) {
    // TODO: Remove all entries with txnNum < _txnNum && sessionId == _sessionId
}

}  // namespace mongo
