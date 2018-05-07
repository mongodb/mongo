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
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/db/session_txn_record_gen.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/util/concurrency/with_lock.h"

namespace mongo {

class OperationContext;
class UpdateRequest;

/**
 * A write through cache for the state of a particular session. All modifications to the underlying
 * session transactions collection must be performed through an object of this class.
 *
 * The cache state can be 'up-to-date' (it is in sync with the persistent contents) or 'needs
 * refresh' (in which case refreshFromStorageIfNeeded needs to be called in order to make it
 * up-to-date).
 */
class Session {
    MONGO_DISALLOW_COPYING(Session);

public:
    using CommittedStatementTimestampMap = stdx::unordered_map<StmtId, repl::OpTime>;

    static const BSONObj kDeadEndSentinel;

    explicit Session(LogicalSessionId sessionId);

    const LogicalSessionId& getSessionId() const {
        return _sessionId;
    }

    /**
     * Blocking method, which loads the transaction state from storage if it has been marked as
     * needing refresh.
     *
     * In order to avoid the possibility of deadlock, this method must not be called while holding a
     * lock.
     */
    void refreshFromStorageIfNeeded(OperationContext* opCtx);

    /**
     * Starts a new transaction on the session, must be called after refreshFromStorageIfNeeded has
     * been called. If an attempt is made to start a transaction with number less than the latest
     * transaction this session has seen, an exception will be thrown.
     *
     * Throws if the session has been invalidated or if an attempt is made to start a transaction
     * older than the active.
     *
     * In order to avoid the possibility of deadlock, this method must not be called while holding a
     * lock.
     */
    void beginTxn(OperationContext* opCtx, TxnNumber txnNumber);

    /**
     * Called after a write under the specified transaction completes while the node is a primary
     * and specifies the statement ids which were written. Must be called while the caller is still
     * in the write's WUOW. Updates the on-disk state of the session to match the specified
     * transaction/opTime and keeps the cached state in sync.
     *
     * Must only be called with the session checked-out.
     *
     * Throws if the session has been invalidated or the active transaction number doesn't match.
     */
    void onWriteOpCompletedOnPrimary(OperationContext* opCtx,
                                     TxnNumber txnNumber,
                                     std::vector<StmtId> stmtIdsWritten,
                                     const repl::OpTime& lastStmtIdWriteOpTime,
                                     Date_t lastStmtIdWriteDate);

    /**
     * Called after an entry for the specified session and transaction has been written to the oplog
     * during chunk migration, while the node is still primary. Must be called while the caller is
     * still in the oplog write's WUOW. Updates the on-disk state of the session to match the
     * specified transaction/opTime and keeps the cached state in sync.
     *
     * May be called concurrently with onWriteOpCompletedOnPrimary or onMigrateCompletedOnPrimary
     * and doesn't require the session to be checked-out.
     *
     * Throws if the session has been invalidated or the active transaction number is newer than the
     * one specified.
     */
    void onMigrateCompletedOnPrimary(OperationContext* opCtx,
                                     TxnNumber txnNumber,
                                     std::vector<StmtId> stmtIdsWritten,
                                     const repl::OpTime& lastStmtIdWriteOpTime,
                                     Date_t oplogLastStmtIdWriteDate);

    /**
     * Marks the session as requiring refresh. Used when the session state has been modified
     * externally, such as through a direct write to the transactions table.
     */
    void invalidate();

    /**
     * Returns the op time of the last committed write for this session and transaction. If no write
     * has completed yet, returns an empty timestamp.
     *
     * Throws if the session has been invalidated or the active transaction number doesn't match.
     */
    repl::OpTime getLastWriteOpTime(TxnNumber txnNumber) const;

    /**
     * Checks whether the given statementId for the specified transaction has already executed and
     * if so, returns the oplog entry which was generated by that write. If the statementId hasn't
     * executed, returns boost::none.
     *
     * Must only be called with the session checked-out.
     *
     * Throws if the session has been invalidated or the active transaction number doesn't match.
     */
    boost::optional<repl::OplogEntry> checkStatementExecuted(OperationContext* opCtx,
                                                             TxnNumber txnNumber,
                                                             StmtId stmtId) const;

    /**
     * Checks whether the given statementId for the specified transaction has already executed
     * without fetching the oplog entry which was generated by that write.
     *
     * Must only be called with the session checked-out.
     *
     * Throws if the session has been invalidated or the active transaction number doesn't match.
     */
    bool checkStatementExecutedNoOplogEntryFetch(TxnNumber txnNumber, StmtId stmtId) const;

    /**
     * Returns a new oplog entry if the given entry has transaction state embedded within in.
     * The new oplog entry will contain the operation needed to replicate the transaction
     * table.
     * Returns boost::none if the given oplog doesn't have any transaction state or does not
     * support update to the transaction table.
     */
    static boost::optional<repl::OplogEntry> createMatchingTransactionTableUpdate(
        const repl::OplogEntry& entry);

private:
    void _beginTxn(WithLock, TxnNumber txnNumber);

    void _checkValid(WithLock) const;

    void _checkIsActiveTransaction(WithLock, TxnNumber txnNumber) const;

    boost::optional<repl::OpTime> _checkStatementExecuted(WithLock,
                                                          TxnNumber txnNumber,
                                                          StmtId stmtId) const;

    // Returns the write date of the last committed write for this session and transaction. If no
    // write has completed yet, returns an empty date.
    //
    // Throws if the session has been invalidated or the active transaction number doesn't match.
    Date_t _getLastWriteDate(WithLock, TxnNumber txnNumber) const;

    UpdateRequest _makeUpdateRequest(WithLock,
                                     TxnNumber newTxnNumber,
                                     const repl::OpTime& newLastWriteTs,
                                     Date_t newLastWriteDate) const;

    void _registerUpdateCacheOnCommit(OperationContext* opCtx,
                                      TxnNumber newTxnNumber,
                                      std::vector<StmtId> stmtIdsWritten,
                                      const repl::OpTime& lastStmtIdWriteTs);

    const LogicalSessionId _sessionId;

    // Protects the member variables below.
    mutable stdx::mutex _mutex;

    // Specifies whether the session information needs to be refreshed from storage
    bool _isValid{false};

    // Counter, incremented with each call to invalidate in order to discern invalidations, which
    // happen during refresh
    int _numInvalidations{0};

    // Set to true if incomplete history is detected. For example, when the oplog to a write was
    // truncated because it was too old.
    bool _hasIncompleteHistory{false};

    // Caches what is known to be the last written transaction record for the session
    boost::optional<SessionTxnRecord> _lastWrittenSessionRecord;

    // Tracks the last seen txn number for the session and is always >= to the transaction number in
    // the last written txn record. When it is > than that in the last written txn record, this
    // means a new transaction has begun on the session, but it hasn't yet performed any writes.
    TxnNumber _activeTxnNumber{kUninitializedTxnNumber};

    // For the active txn, tracks which statement ids have been committed and at which oplog
    // opTime. Used for fast retryability check and retrieving the previous write's data without
    // having to scan through the oplog.
    CommittedStatementTimestampMap _activeTxnCommittedStatements;
};

}  // namespace mongo
