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
#include <memory>

#include "mongo/base/status.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/repl/multiapplier.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/oplog_applier.h"
#include "mongo/db/repl/oplog_buffer.h"
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/db/repl/replication_consistency_markers.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/stdx/functional.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/concurrency/thread_pool.h"

namespace mongo {

class Database;
class OperationContext;
struct MultikeyPathInfo;

namespace repl {
class ReplicationCoordinator;
class OpTime;

/**
 * Used for oplog application on a replica set secondary.
 * Primarily used to apply batches of operations fetched from a sync source during steady state
 * replication and initial sync.
 *
 * When used for steady state replication, runs a thread that reads batches of operations from
 * an oplog buffer (through the BackgroundSync interface) and applies the batch of operations.
 */
class SyncTail {
public:
    using MultiSyncApplyFunc =
        stdx::function<Status(OperationContext* opCtx,
                              MultiApplier::OperationPtrs* ops,
                              SyncTail* st,
                              WorkerMultikeyPathInfo* workerMultikeyPathInfo)>;

    /**
     * Maximum number of operations in each batch that can be applied using multiApply().
     */
    static AtomicInt32 replBatchLimitOperations;

    /**
     * Lower bound of batch limit size (in bytes) returned by calculateBatchLimitBytes().
     */
    static const unsigned int replBatchLimitBytes = 100 * 1024 * 1024;

    /**
     * Creates thread pool for writer tasks.
     */
    static std::unique_ptr<ThreadPool> makeWriterPool();
    static std::unique_ptr<ThreadPool> makeWriterPool(int threadCount);

    /**
     * Applies the operation that is in param o.
     * Functions for applying operations/commands and increment server status counters may
     * be overridden for testing.
     */
    static Status syncApply(OperationContext* opCtx,
                            const BSONObj& o,
                            OplogApplication::Mode oplogApplicationMode);

    /**
     *
     * Constructs a SyncTail.
     * During steady state replication, oplogApplication() obtains batches of operations to apply
     * from 'observer'. It is not required to provide 'observer' at construction if we do not plan
     * on using oplogApplication(). During the oplog application phase, the batch of operations is
     * distributed across writer threads in 'writerPool'. Each writer thread applies its own vector
     * of operations using 'func'. The writer thread pool is not owned by us.
     */
    SyncTail(OplogApplier::Observer* observer,
             ReplicationConsistencyMarkers* consistencyMarkers,
             StorageInterface* storageInterface,
             MultiSyncApplyFunc func,
             ThreadPool* writerPool,
             const OplogApplier::Options& options);
    SyncTail(OplogApplier::Observer* observer,
             ReplicationConsistencyMarkers* consistencyMarkers,
             StorageInterface* storageInterface,
             MultiSyncApplyFunc func,
             ThreadPool* writerPool);
    virtual ~SyncTail();

    /**
     * Returns options for oplog application.
     */
    const OplogApplier::Options& getOptions() const;

    /**
     * Runs oplog application in a loop until shutdown() is called.
     * Retrieves operations from the OplogBuffer in batches that will be applied in parallel using
     * multiApply().
     */
    void oplogApplication(OplogBuffer* oplogBuffer, ReplicationCoordinator* replCoord);

    /**
     * Shuts down oplogApplication() processing.
     */
    void shutdown();

    /**
     * Returns true if we are shutting down.
     */
    bool inShutdown() const;


    class OpQueue {
    public:
        OpQueue() : _bytes(0) {
            _batch.reserve(replBatchLimitOperations.load());
        }

        size_t getBytes() const {
            return _bytes;
        }
        size_t getCount() const {
            return _batch.size();
        }
        bool empty() const {
            return _batch.empty();
        }
        const OplogEntry& front() const {
            invariant(!_batch.empty());
            return _batch.front();
        }
        const OplogEntry& back() const {
            invariant(!_batch.empty());
            return _batch.back();
        }
        const std::vector<OplogEntry>& getBatch() const {
            return _batch;
        }

        void emplace_back(BSONObj obj) {
            invariant(!_mustShutdown);
            _bytes += obj.objsize();
            _batch.emplace_back(std::move(obj));
        }
        void pop_back() {
            _bytes -= back().getRawObjSizeBytes();
            _batch.pop_back();
        }

        /**
         * A batch with this set indicates that the upstream stages of the pipeline are shutdown and
         * no more batches will be coming.
         *
         * This can only happen with empty batches.
         *
         * TODO replace the empty object used to signal draining with this.
         */
        bool mustShutdown() const {
            return _mustShutdown;
        }
        void setMustShutdownFlag() {
            invariant(empty());
            _mustShutdown = true;
        }

        /**
         * Leaves this object in an unspecified state. Only assignment and destruction are valid.
         */
        std::vector<OplogEntry> releaseBatch() {
            return std::move(_batch);
        }

    private:
        std::vector<OplogEntry> _batch;
        size_t _bytes;
        bool _mustShutdown = false;
    };

    struct BatchLimits {
        size_t bytes = replBatchLimitBytes;
        size_t ops = replBatchLimitOperations.load();

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
    bool tryPopAndWaitForMore(OperationContext* opCtx,
                              OplogBuffer* oplogBuffer,
                              OpQueue* ops,
                              const BatchLimits& limits);

    /**
     * Fetch a single document referenced in the operation from the sync source.
     */
    virtual BSONObj getMissingDoc(OperationContext* opCtx, const OplogEntry& oplogEntry);

    /**
     * If an update fails, fetches the missing document and inserts it into the local collection.
     *
     * Returns true if the document was fetched and inserted successfully.
     */
    virtual bool fetchAndInsertMissingDocument(OperationContext* opCtx,
                                               const OplogEntry& oplogEntry);

    void setHostname(const std::string& hostname);

    /**
     * Applies a batch of oplog entries by writing the oplog entries to the local oplog and then
     * using a set of threads to apply the operations.
     *
     * If the batch application is successful, returns the optime of the last op applied, which
     * should be the last op in the batch.
     * Returns ErrorCodes::CannotApplyOplogWhilePrimary if the node has become primary.
     *
     * To provide crash resilience, this function will advance the persistent value of 'minValid'
     * to at least the last optime of the batch. If 'minValid' is already greater than or equal
     * to the last optime of this batch, it will not be updated.
     */
    StatusWith<OpTime> multiApply(OperationContext* opCtx, MultiApplier::Operations ops);

private:
    /**
     * Pops the operation at the front of the OplogBuffer.
     * Updates stats on BackgroundSync.
     */
    void _consume(OperationContext* opCtx, OplogBuffer* oplogBuffer);

    class OpQueueBatcher;

    std::string _hostname;

    OplogApplier::Observer* const _observer;
    ReplicationConsistencyMarkers* const _consistencyMarkers;
    StorageInterface* const _storageInterface;

    // Function to use during applyOps
    MultiSyncApplyFunc _applyFunc;

    // Pool of worker threads for writing ops to the databases.
    // Not owned by us.
    ThreadPool* const _writerPool;

    // Used to configure multiApply() behavior.
    const OplogApplier::Options _options;

    // Protects member data of SyncTail.
    mutable stdx::mutex _mutex;

    // Set to true if shutdown() has been called.
    bool _inShutdown = false;
};

// These free functions are used by the thread pool workers to write ops to the db.
// They consume the passed in OperationPtrs and callers should not make any assumptions about the
// state of the container after calling. However, these functions cannot modify the pointed-to
// operations because the OperationPtrs container contains const pointers.
Status multiSyncApply(OperationContext* opCtx,
                      MultiApplier::OperationPtrs* ops,
                      SyncTail* st,
                      WorkerMultikeyPathInfo* workerMultikeyPathInfo);

Status multiInitialSyncApply(OperationContext* opCtx,
                             MultiApplier::OperationPtrs* ops,
                             SyncTail* st,
                             WorkerMultikeyPathInfo* workerMultikeyPathInfo);

}  // namespace repl
}  // namespace mongo
