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

#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/db/session_transaction_table.h"
#include "mongo/db/session_txn_state_holder.h"
#include "mongo/stdx/memory.h"

namespace mongo {

SessionTxnStateHolder::SessionTxnStateHolder(std::unique_ptr<SessionTxnState> txnState)
    : _sessionId(txnState->getSessionId()), _txnState(std::move(txnState)) {}

TxnStateAccessToken SessionTxnStateHolder::getTransactionState(OperationContext* opCtx,
                                                               TxnNumber txnNum) {
    stdx::unique_lock<stdx::mutex> lk(_mutex);

    while (!_txnState) {
        opCtx->waitForConditionOrInterrupt(_txnStateAvailableCV, lk);
    }

    if (txnNum < _txnState->getTxnNum()) {
        // uassert
    }

    if (txnNum > _txnState->getTxnNum()) {
        _txnState = stdx::make_unique<SessionTxnState>(_sessionId, txnNum);
    }

    return TxnStateAccessToken(opCtx, this, std::move(_txnState));
}

void SessionTxnStateHolder::finishTxn(std::unique_ptr<SessionTxnState> txnState) {
    invariant(txnState);
    invariant(_sessionId == txnState->getSessionId());

    stdx::lock_guard<stdx::mutex> lk(_mutex);
    _txnState = std::move(txnState);
    _txnStateAvailableCV.notify_all();
}

const LogicalSessionId& SessionTxnStateHolder::getSessionId() const {
    return _sessionId;
}

TxnStateAccessToken::TxnStateAccessToken(OperationContext* opCtx,
                                         SessionTxnStateHolder* holder,
                                         std::unique_ptr<SessionTxnState> txnState)
    : _opCtx(opCtx), _holder(holder), _txnState(std::move(txnState)) {
    invariant(_opCtx);
    invariant(_holder);
    SessionTxnState::set(_opCtx, _txnState.get());
}

TxnStateAccessToken::~TxnStateAccessToken() {
    SessionTxnState::set(_opCtx, nullptr);
    _holder->finishTxn(std::move(_txnState));
}

SessionTxnState* TxnStateAccessToken::get() {
    return _txnState.get();
}

}  // namespace mongo
