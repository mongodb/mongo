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

#include <vector>

#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/ops/update.h"
#include "mongo/db/query/get_executor.h"
#include "mongo/stdx/memory.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {
namespace {

boost::optional<SessionTxnRecord> loadSessionRecord(OperationContext* opCtx,
                                                    const LogicalSessionId& sessionId) {
    DBDirectClient client(opCtx);
    Query sessionQuery(BSON(SessionTxnRecord::kSessionIdFieldName << sessionId.toBSON()));
    auto result = client.findOne(NamespaceString::kSessionTransactionsTableNamespace.ns(),
                                 sessionQuery,
                                 nullptr,
                                 DBClientCursor::QueryOptionLocal_forceOpQuery);  // SERVER-30318

    if (result.isEmpty()) {
        return boost::none;
    }

    IDLParserErrorContext ctx("parse latest txn record for session");
    return SessionTxnRecord::parse(ctx, result);
}

/**
 * Update the txnNum of the session record. Will create a new entry if the record with
 * corresponding sessionId does not exist.
 */
void updateSessionRecordTxnNum(OperationContext* opCtx,
                               const LogicalSessionId& sessionId,
                               const TxnNumber& txnNum) {
    repl::UnreplicatedWritesBlock doNotReplicateWrites(opCtx);
    Timestamp zeroTs;

    AutoGetCollection autoColl(opCtx, NamespaceString::kSessionTransactionsTableNamespace, MODE_IX);
    uassert(40526,
            str::stream() << "Unable to persist transaction state because the session transaction "
                             "collection is missing. This indicates that the "
                          << NamespaceString::kSessionTransactionsTableNamespace.ns()
                          << " collection has been manually deleted.",
            autoColl.getCollection() != nullptr);

    UpdateRequest updateRequest(NamespaceString::kSessionTransactionsTableNamespace);
    updateRequest.setQuery(BSON(SessionTxnRecord::kSessionIdFieldName << sessionId.toBSON()));
    updateRequest.setUpdates(BSON("$set" << BSON(SessionTxnRecord::kTxnNumFieldName
                                                 << txnNum
                                                 << SessionTxnRecord::kLastWriteOpTimeTsFieldName
                                                 << zeroTs)));
    updateRequest.setUpsert(true);

    auto updateResult = update(opCtx, autoColl.getDb(), updateRequest);
    uassert(40527,
            str::stream() << "Failed to update transaction progress for session " << sessionId,
            updateResult.numDocsModified >= 1 || !updateResult.upserted.isEmpty());
}

}  // namespace

Session::Session(LogicalSessionId sessionId) : _sessionId(std::move(sessionId)) {}

void Session::begin(OperationContext* opCtx, const TxnNumber& txnNumber) {
    invariant(!opCtx->lockState()->isLocked());
    invariant(!opCtx->lockState()->inAWriteUnitOfWork());

    if (!_txnRecord) {
        _txnRecord = loadSessionRecord(opCtx, _sessionId);

        // Previous read failed to retrieve the txn record, which means it does not exist yet,
        // so create a new entry.
        if (!_txnRecord) {
            updateSessionRecordTxnNum(opCtx, _sessionId, txnNumber);

            _txnRecord.emplace();
            _txnRecord->setSessionId(_sessionId);
            _txnRecord->setTxnNum(txnNumber);
            _txnRecord->setLastWriteOpTimeTs(Timestamp());

            return;
        }
    }

    uassert(40528,
            str::stream() << "cannot start transaction with id " << txnNumber << " on session "
                          << _sessionId
                          << " because transaction with id "
                          << _txnRecord->getTxnNum()
                          << " already started",
            _txnRecord->getTxnNum() <= txnNumber);

    if (txnNumber > _txnRecord->getTxnNum()) {
        updateSessionRecordTxnNum(opCtx, _sessionId, txnNumber);
        _txnRecord->setTxnNum(txnNumber);
        _txnRecord->setLastWriteOpTimeTs(Timestamp());
    }
}

void Session::saveTxnProgress(OperationContext* opCtx, Timestamp opTimeTs) {
    // Needs to be in the same write unit of work with the write for this result.
    invariant(opCtx->lockState()->inAWriteUnitOfWork());

    repl::UnreplicatedWritesBlock doNotReplicateWrites(opCtx);
    AutoGetCollection autoColl(opCtx, NamespaceString::kSessionTransactionsTableNamespace, MODE_IX);
    auto coll = autoColl.getCollection();

    uassert(40529,
            str::stream() << "Unable to persist transaction state because the session transaction "
                             "collection is missing. This indicates that the "
                          << NamespaceString::kSessionTransactionsTableNamespace.ns()
                          << " collection has been manually deleted.",
            coll);

    UpdateRequest updateRequest(NamespaceString::kSessionTransactionsTableNamespace);
    updateRequest.setQuery(BSON(SessionTxnRecord::kSessionIdFieldName
                                << _sessionId.toBSON()
                                << SessionTxnRecord::kTxnNumFieldName
                                << _txnRecord->getTxnNum()));
    updateRequest.setUpdates(
        BSON("$set" << BSON(SessionTxnRecord::kLastWriteOpTimeTsFieldName << opTimeTs)));
    updateRequest.setUpsert(false);

    auto updateResult = update(opCtx, autoColl.getDb(), updateRequest);
    uassert(40530,
            str::stream() << "Failed to update transaction progress for session " << _sessionId,
            updateResult.numDocsModified >= 1);

    _txnRecord->setLastWriteOpTimeTs(opTimeTs);
}

TxnNumber Session::getTxnNum() const {
    return _txnRecord->getTxnNum();
}

const Timestamp& Session::getLastWriteOpTimeTs() const {
    return _txnRecord->getLastWriteOpTimeTs();
}

TransactionHistoryIterator Session::getWriteHistory(OperationContext* opCtx) const {
    return TransactionHistoryIterator(getLastWriteOpTimeTs());
}

boost::optional<repl::OplogEntry> Session::checkStatementExecuted(OperationContext* opCtx,
                                                                  StmtId stmtId) {
    if (!opCtx->getTxnNumber()) {
        return boost::none;
    }

    auto it = getWriteHistory(opCtx);
    while (it.hasNext()) {
        auto entry = it.next(opCtx);
        if (entry.getStatementId() == stmtId) {
            return entry;
        }
    }

    return boost::none;
}

}  // namespace mongo
