/**
 *    Copyright (C) 2008 10gen Inc.
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

#include <deque>

#include "mongo/base/status.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/storage/mmap_v1/dur.h"
#include "mongo/stdx/functional.h"
#include "mongo/util/concurrency/old_thread_pool.h"

namespace mongo {

    class Database;
    class OperationContext;

namespace repl {
    class BackgroundSyncInterface;
    class ReplicationCoordinator;
    class OpTime;

    /**
     * "Normal" replica set syncing
     */
    class SyncTail {
    public:
        using MultiSyncApplyFunc =
            stdx::function<void (const std::vector<BSONObj>& ops, SyncTail* st)>;

        /**
         * Type of function that takes a non-command op and applies it locally.
         * Used for applying from an oplog.
         * Last boolean argument 'convertUpdateToUpsert' converts some updates to upserts for
         * idempotency reasons.
         * Returns failure status if the op was an update that could not be applied.
         */
        using ApplyOperationInLockFn =
            stdx::function<Status (OperationContext*, Database*, const BSONObj&, bool)>;

        /**
         * Type of function that takes a command op and applies it locally.
         * Used for applying from an oplog.
         * Returns failure status if the op that could not be applied.
         */
        using ApplyCommandInLockFn = stdx::function<Status (OperationContext*, const BSONObj&)>;

        /**
         * Type of function to increment "repl.apply.ops" server status metric.
         */
        using IncrementOpsAppliedStatsFn = stdx::function<void ()>;

        SyncTail(BackgroundSyncInterface *q, MultiSyncApplyFunc func);
        virtual ~SyncTail();

        /**
         * Applies the operation that is in param o.
         * Functions for applying operations/commands and increment server status counters may
         * be overridden for testing.
         */
        static Status syncApply(OperationContext* txn,
                                const BSONObj &o,
                                bool convertUpdateToUpsert,
                                ApplyOperationInLockFn applyOperationInLock,
                                ApplyCommandInLockFn applyCommandInLock,
                                IncrementOpsAppliedStatsFn incrementOpsAppliedStats);

        static Status syncApply(OperationContext* txn,
                                const BSONObj &o,
                                bool convertUpdateToUpsert);

        /**
         * Runs _applyOplogUntil(stopOpTime)
         */
        virtual void oplogApplication(OperationContext* txn, const OpTime& stopOpTime);

        void oplogApplication();
        bool peek(BSONObj* obj);

        class OpQueue {
        public:
            OpQueue() : _size(0) {}
            size_t getSize() const { return _size; }
            const std::deque<BSONObj>& getDeque() const { return _deque; }
            void push_back(BSONObj& op) {
                _deque.push_back(op);
                _size += op.objsize();
            }
            bool empty() const {
                return _deque.empty();
            }

            BSONObj back() const {
                invariant(!_deque.empty());
                return _deque.back();
            }

        private:
            std::deque<BSONObj> _deque;
            size_t _size;
        };

        // returns true if we should continue waiting for BSONObjs, false if we should
        // stop waiting and apply the queue we have.  Only returns false if !ops.empty().
        bool tryPopAndWaitForMore(OperationContext* txn,
                                  OpQueue* ops,
                                  ReplicationCoordinator* replCoord);

        /**
         * Fetch a single document referenced in the operation from the sync source.
         */
        virtual BSONObj getMissingDoc(OperationContext* txn, Database* db, const BSONObj& o);

        /**
         * If applyOperation_inlock should be called again after an update fails.
         */
        virtual bool shouldRetry(OperationContext* txn, const BSONObj& o);
        void setHostname(const std::string& hostname);

    protected:
        // Cap the batches using the limit on journal commits.
        // This works out to be 100 MB (64 bit) or 50 MB (32 bit)
        static const unsigned int replBatchLimitBytes = dur::UncommittedBytesLimit;
        static const int replBatchLimitSeconds = 1;
        static const unsigned int replBatchLimitOperations = 5000;

        // SyncTail base class always supports awaiting commit if any op has j:true flag
        // that indicates awaiting commit before updating last OpTime.
        virtual bool supportsWaitingUntilDurable() { return true; }

        // Prefetch and write a deque of operations, using the supplied function.
        // Initial Sync and Sync Tail each use a different function.
        // Returns the last OpTime applied.
        static OpTime multiApply(OperationContext* txn,
                                 const OpQueue& ops,
                                 OldThreadPool* prefetcherPool,
                                 OldThreadPool* writerPool,
                                 MultiSyncApplyFunc func,
                                 SyncTail* sync,
                                 bool supportsAwaitingCommit);

        /**
         * Applies oplog entries until reaching "endOpTime".
         *
         * NOTE:Will not transition or check states
         */
        void _applyOplogUntil(OperationContext* txn, const OpTime& endOpTime);

    private:
        std::string _hostname;

        BackgroundSyncInterface* _networkQueue;

        // Function to use during applyOps
        MultiSyncApplyFunc _applyFunc;

        void handleSlaveDelay(const BSONObj& op);

        // persistent pool of worker threads for writing ops to the databases
        OldThreadPool _writerPool;
        // persistent pool of worker threads for prefetching
        OldThreadPool _prefetcherPool;

    };

    // These free functions are used by the thread pool workers to write ops to the db.
    void multiSyncApply(const std::vector<BSONObj>& ops, SyncTail* st);
    void multiInitialSyncApply(const std::vector<BSONObj>& ops, SyncTail* st);

} // namespace repl
} // namespace mongo
