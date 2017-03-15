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

#include <boost/optional.hpp>
#include <deque>

#include "mongo/base/status.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/repl/minvalid.h"
#include "mongo/db/repl/storage_interface.h"
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
    using MultiSyncApplyFunc = stdx::function<void(const std::vector<BSONObj>& ops, SyncTail* st)>;

    /**
     * Type of function that takes a non-command op and applies it locally.
     * Used for applying from an oplog.
     * Last boolean argument 'inSteadyStateReplication' converts some updates to upserts for
     * idempotency reasons.
     * Returns failure status if the op was an update that could not be applied.
     */
    using ApplyOperationInLockFn =
        stdx::function<Status(OperationContext*, Database*, const BSONObj&, bool)>;

    /**
     * Type of function that takes a command op and applies it locally.
     * Used for applying from an oplog.
     * Returns failure status if the op that could not be applied.
     */
    using ApplyCommandInLockFn = stdx::function<Status(OperationContext*, const BSONObj&, bool)>;

    /**
     * Type of function to increment "repl.apply.ops" server status metric.
     */
    using IncrementOpsAppliedStatsFn = stdx::function<void()>;

    SyncTail(BackgroundSyncInterface* q, MultiSyncApplyFunc func);
    virtual ~SyncTail();

    /**
     * Applies the operation that is in param o.
     * Functions for applying operations/commands and increment server status counters may
     * be overridden for testing.
     */
    static Status syncApply(OperationContext* txn,
                            const BSONObj& o,
                            bool inSteadyStateReplication,
                            ApplyOperationInLockFn applyOperationInLock,
                            ApplyCommandInLockFn applyCommandInLock,
                            IncrementOpsAppliedStatsFn incrementOpsAppliedStats);

    static Status syncApply(OperationContext* txn, const BSONObj& o, bool inSteadyStateReplication);

    void oplogApplication(StorageInterface* storageInterface);
    bool peek(BSONObj* obj);

    /**
     * A parsed oplog entry.
     *
     * This only includes the fields used by the code using this object at the time this was
     * written. As more code uses this, more fields should be added.
     *
     * All unowned members (such as StringDatas and BSONElements) point into the raw BSON.
     * All StringData members are guaranteed to be NUL terminated.
     */
    struct OplogEntry {
        explicit OplogEntry(const BSONObj& raw);

        BSONObj raw;  // Owned.

        StringData ns = "";
        StringData opType = "";

        BSONElement version;
        BSONElement o;
        BSONElement o2;
        BSONElement ts;
    };

    class OpQueue {
    public:
        OpQueue() : _size(0) {}
        size_t getSize() const {
            return _size;
        }
        const std::deque<OplogEntry>& getDeque() const {
            return _deque;
        }
        void push_back(OplogEntry&& op) {
            _size += op.raw.objsize();
            _deque.push_back(std::move(op));
        }
        bool empty() const {
            return _deque.empty();
        }

        const OplogEntry& back() const {
            invariant(!_deque.empty());
            return _deque.back();
        }

        const OplogEntry& front() const {
            invariant(!_deque.empty());
            return _deque.front();
        }

    private:
        std::deque<OplogEntry> _deque;
        size_t _size;
    };

    struct BatchLimits {
        size_t bytes = replBatchLimitBytes;
        size_t ops = replBatchLimitOperations;

        // If provided, the batch will not include any operations with timestamps after this point.
        // This is intended for implementing slaveDelay, so it should be some number of seconds
        // before now.
        boost::optional<Date_t> slaveDelayLatestTimestamp = {};
    };

    /**
     * Attempts to pop an OplogEntry off the BGSync queue and add it to ops.
     *
     * Returns true if the (possibly empty) batch in ops should be ended and a new one started.
     * If ops is empty on entry and nothing can be added yet, will wait up to a second before
     * returning true.
     */
    bool tryPopAndWaitForMore(OperationContext* txn, OpQueue* ops, const BatchLimits& limits);

    /**
     * Fetch a single document referenced in the operation from the sync source.
     */
    virtual BSONObj getMissingDoc(OperationContext* txn, Database* db, const BSONObj& o);

    /**
     * If applyOperation_inlock should be called again after an update fails.
     */
    virtual bool shouldRetry(OperationContext* txn, const BSONObj& o);
    void setHostname(const std::string& hostname);

    /**
     * This variable determines the number of writer threads SyncTail will have. It has a default
     * value, which varies based on architecture and can be overridden using the
     * "replWriterThreadCount" server parameter.
     */
    static int replWriterThreadCount;

protected:
    // Cap the batches to 50 MB for 32-bit systems and 100 MB for 64-bit systems.
    static const unsigned int replBatchLimitBytes =
        (sizeof(void*) == 4) ? 50 * 1024 * 1024 : 100 * 1024 * 1024;
    static const unsigned int replBatchLimitOperations = 5000;

    // Apply a batch of operations, using multiple threads.
    // If boundries is supplied, will update minValid document at begin and end of batch.
    // Returns the last OpTime applied during the apply batch, ops.end["ts"] basically.
    OpTime multiApply(OperationContext* txn, const OpQueue& ops);

private:
    class OpQueueBatcher;

    std::string _hostname;

    BackgroundSyncInterface* _networkQueue;

    // Function to use during applyOps
    MultiSyncApplyFunc _applyFunc;

    // persistent pool of worker threads for writing ops to the databases
    OldThreadPool _writerPool;
    // persistent pool of worker threads for prefetching
    OldThreadPool _prefetcherPool;
};

// These free functions are used by the thread pool workers to write ops to the db.
void multiSyncApply(const std::vector<BSONObj>& ops, SyncTail* st);
void multiInitialSyncApply(const std::vector<BSONObj>& ops, SyncTail* st);
Status multiInitialSyncApply_noAbort(OperationContext* txn,
                                     const std::vector<BSONObj>& ops,
                                     SyncTail* st);

}  // namespace repl
}  // namespace mongo
