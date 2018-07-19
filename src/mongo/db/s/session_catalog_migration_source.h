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
#include <memory>

#include "mongo/base/disallow_copying.h"
#include "mongo/client/dbclientcursor.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/session_txn_record_gen.h"
#include "mongo/db/transaction_history_iterator.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/concurrency/with_lock.h"

namespace mongo {

class OperationContext;
class ScopedSession;
class ServiceContext;

/**
 * Provides facilities for extracting oplog entries of writes in a particular namespace that needs
 * to be migrated.
 *
 * This also ensures that oplog returned are majority committed. This is achieved by calling
 * waitForWriteConcern. However, waitForWriteConcern does not support waiting for opTimes of
 * previous terms. To get around this, the waitForWriteConcern is performed in two phases:
 *
 * During init() call phase:
 * 1. Scan the entire config.transactions and extract all the lastWriteOpTime.
 * 2. Insert a no-op oplog entry and wait for it to be majority committed.
 * 3. At this point any writes before should be majority committed (including all the oplog
 *    entries that the collected lastWriteOpTime points to). If the particular oplog with the
 *    opTime cannot be found: it either means that the oplog was truncated or rolled back.
 *
 * New writes/xfer mods phase oplog entries:
 * In this case, caller is responsible for calling waitForWriteConcern. If getLastFetchedOplog
 * returns shouldWaitForMajority == true, it should wait for the highest opTime it has got from
 * getLastFetchedOplog. It should also error if it detects a change of term within a batch since
 * it would be wrong to wait for the highest opTime in this case.
 */
class SessionCatalogMigrationSource {
    MONGO_DISALLOW_COPYING(SessionCatalogMigrationSource);

public:
    struct OplogResult {
        OplogResult(boost::optional<repl::OplogEntry> _oplog, bool _shouldWaitForMajority)
            : oplog(std::move(_oplog)), shouldWaitForMajority(_shouldWaitForMajority) {}

        // The oplog fetched.
        boost::optional<repl::OplogEntry> oplog;

        // If this is set to true, oplog returned is not confirmed to be majority committed,
        // so the caller has to explicitly wait for it to be committed to majority.
        bool shouldWaitForMajority = false;
    };

    SessionCatalogMigrationSource(OperationContext* opCtx, NamespaceString ns);

    /**
     * Returns true if there are more oplog entries to fetch at this moment. Note that new writes
     * can still continue to come in after this has returned false, so it can become true again.
     * Once this has returned false, this means that it has depleted the existing buffer so it
     * is a good time to enter the critical section.
     */
    bool hasMoreOplog();

    /**
     * Attempts to fetch the next oplog entry. Returns true if it was able to fetch anything.
     */
    bool fetchNextOplog(OperationContext* opCtx);

    /**
     * Returns the oplog document that was last fetched by the fetchNextOplog call.
     * Returns an empty object if there are no oplog to fetch.
     */
    OplogResult getLastFetchedOplog();

    /**
     * Remembers the oplog timestamp of a new write that just occurred.
     */
    void notifyNewWriteOpTime(repl::OpTime opTimestamp);

    /**
     * Returns the rollback ID recorded at the beginning of session migration.
     */
    int getRollbackIdAtInit() const {
        return _rollbackIdAtInit;
    }

private:
    /**
     * An iterator for extracting session write oplogs that need to be cloned during migration.
     */
    class SessionOplogIterator {
    public:
        SessionOplogIterator(SessionTxnRecord txnRecord, int expectedRollbackId);

        /**
          * Returns true if there are more oplog entries to fetch for this session.
          */
        bool hasNext() const;

        /**
         * Returns the next oplog write that happened in this session. If the oplog is lost
         * because the oplog rolled over, this will return a sentinel oplog entry instead with
         * type 'n' and o2 field set to Session::kDeadEndSentinel. This will also mean that
         * next subsequent calls to hasNext will return false.
         */
        repl::OplogEntry getNext(OperationContext* opCtx);

        BSONObj toBSON() const {
            return _record.toBSON();
        }

    private:
        const SessionTxnRecord _record;
        const int _initialRollbackId;
        std::unique_ptr<TransactionHistoryIterator> _writeHistoryIterator;
    };

    ///////////////////////////////////////////////////////////////////////////
    // Methods for extracting the oplog entries from session information.

    /**
     * If this returns false, it just means that there are no more oplog entry in the buffer that
     * needs to be moved over. However, there can still be new incoming operations that can add
     * new entries. Also see hasNewWrites.
     */
    bool _hasMoreOplogFromSessionCatalog();

    /**
     * Attempts to extract the next oplog document by following the oplog chain from the sessions
     * catalog. Returns true if a document was actually fetched.
     */
    bool _fetchNextOplogFromSessionCatalog(OperationContext* opCtx);

    /**
     * Returns the document that was last fetched by fetchNextOplogFromSessionCatalog.
     */
    repl::OplogEntry _getLastFetchedOplogFromSessionCatalog();

    /**
     * Extracts oplog information from the current writeHistoryIterator to _lastFetchedOplog. This
     * handles insert/update/delete/findAndModify oplog entries.
     *
     * Returns true if current writeHistoryIterator has any oplog entry.
     */
    bool _handleWriteHistory(WithLock, OperationContext* opCtx);

    ///////////////////////////////////////////////////////////////////////////
    // Methods for capturing and extracting oplog entries for new writes.

    /**
     * Returns true if there are oplog generated by new writes that needs to be fetched.
     */
    bool _hasNewWrites();

    /**
     * Attempts to fetch the next oplog entry from the new writes that was saved by saveNewWriteTS.
     * Returns true if there were documents that were retrieved.
     */
    bool _fetchNextNewWriteOplog(OperationContext* opCtx);

    /**
     * Returns the oplog that was last fetched by fetchNextNewWriteOplog.
     */
    repl::OplogEntry _getLastFetchedNewWriteOplog();

    // Namespace for which the migration is happening
    const NamespaceString _ns;

    // The rollback id just before migration started. This value is needed so that step-down
    // followed by step-up situations can be discovered.
    const int _rollbackIdAtInit;

    // Protects _sessionCatalogCursor, _sessionOplogIterators, _currentOplogIterator,
    // _lastFetchedOplogBuffer, _lastFetchedOplog
    stdx::mutex _sessionCloneMutex;

    // List of remaining session records that needs to be cloned.
    std::vector<std::unique_ptr<SessionOplogIterator>> _sessionOplogIterators;

    // Points to the current session record eing cloned.
    std::unique_ptr<SessionOplogIterator> _currentOplogIterator;

    // Used for temporarily storng oplog entries for operations that has more than one entry.
    // For example, findAndModify generates one for the actual operation and another for the
    // pre/post image.
    std::vector<repl::OplogEntry> _lastFetchedOplogBuffer;

    // Used to store the last fetched oplog. This enables calling get multiple times.
    boost::optional<repl::OplogEntry> _lastFetchedOplog;

    // Protects _newWriteTsList, _lastFetchedNewWriteOplog
    stdx::mutex _newOplogMutex;

    // Stores oplog opTime of new writes that are coming in.
    std::list<repl::OpTime> _newWriteOpTimeList;

    // Used to store the last fetched oplog from _newWriteTsList.
    boost::optional<repl::OplogEntry> _lastFetchedNewWriteOplog;
};

}  // namespace mongo
