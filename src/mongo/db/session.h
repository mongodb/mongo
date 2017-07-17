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

#pragma once

#include <boost/optional.hpp>

#include "mongo/base/disallow_copying.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/logical_session_id.h"
#include "mongo/db/session_txn_record_gen.h"
#include "mongo/db/transaction_history_iterator.h"

namespace mongo {

class OperationContext;

/**
 * Represents the current state of this transaction.
 *
 * Note: this class is not thread safe. This class assumes that it is the only entity that modifies
 * the session transaction document matching it's own sessionId.
 *
 * All of the modifications to underlying collection will not be replicated because there is no
 * straightforward way to make sure that the secondaries will get the oplog entry for BOTH the
 * actual write and the update to the sessions table in the same batch when it fetches the oplog
 * from the sync source. This can cause the secondaries to be in an inconsistent state that is
 * externally observable and can be really bad if enough secondaries are in this state that they
 * become primaries and start accepting writes.
 */
class Session {
    MONGO_DISALLOW_COPYING(Session);

public:
    explicit Session(LogicalSessionId sessionId);

    const LogicalSessionId& getSessionId() const {
        return _sessionId;
    }

    /**
     *  Load transaction state from storage if it hasn't.
     */
    void begin(OperationContext* opCtx, const TxnNumber& txnNumber);

    /**
     * Returns the history of writes that has happened on this transaction.
     */
    TransactionHistoryIterator getWriteHistory(OperationContext* opCtx) const;

    /**
     * Stores the result of a single write operation within this transaction.
     */
    void saveTxnProgress(OperationContext* opCtx, Timestamp opTime);

    /**
     * Note: can only be called after at least one successful execution of begin().
     */
    TxnNumber getTxnNum() const;

    /**
     * Note: can only be called after at least one successful execution of begin().
     */
    const Timestamp& getLastWriteOpTimeTs() const;

    /**
     * Returns the oplog entry with the given statementId, if it exists.
     */
    boost::optional<repl::OplogEntry> checkStatementExecuted(OperationContext* opCtx,
                                                             StmtId stmtId);

private:
    const LogicalSessionId _sessionId;

    boost::optional<SessionTxnRecord> _txnRecord;
};

}  // namespace mongo
