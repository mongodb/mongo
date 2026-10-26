// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/client.h"
#include "mongo/db/curop.h"
#include "mongo/db/index/index_access_method.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/db/shard_role/shard_catalog/collection.h"
#include "mongo/db/shard_role/shard_catalog/index_catalog.h"
#include "mongo/db/shard_role/shard_catalog/index_descriptor.h"
#include "mongo/db/storage/lazy_record_store.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/logv2/log.h"
#include "mongo/util/fail_point.h"

#include <string_view>

namespace mongo {

/**
 * The SideWritesTracker is responsible for buffering side writes during an index build. A temporary
 * table is created (unless resuming) for storing the documents.
 */
class SideWritesTracker {
public:
    /**
     * Determines if we will yield locks while draining the side tables.
     */
    enum class DrainYieldPolicy { kNoYield, kYield };

    /**
     * Identifies which phase of draining the build is performing.
     */
    enum class DrainPhase {
        // Draining before the build is ready to commit.
        kBeforeReadiness,
        // Draining after the build is ready to commit.
        kAfterReadiness,
        // Draining during commit of the build.
        kFinal,
    };

    /**
     * Invoked once per drained batch, inside the transaction that applies that batch's keys and
     * deletes its records.
     */
    using OnBatchAppliedFn = std::function<Status(OperationContext*)>;

    SideWritesTracker(OperationContext* opCtx,
                      std::string_view ident,
                      LazyRecordStore::CreateMode createMode)
        : _table(opCtx, ident, createMode) {
        _table.getOrCreateTable(opCtx);
    }

    /**
     * Buffer a side write of the docs in `toInsert`.
     */
    Status bufferSideWrite(OperationContext* opCtx,
                           const CollectionPtr& coll,
                           const IndexCatalogEntry* indexCatalogEntry,
                           const std::vector<BSONObj>& toInsert);

    /**
     * Force-creates the backing table if it was deferred and has not already been created.
     */
    void createDeferredTable(OperationContext* opCtx);

    /**
     * Captures and return the initial tail, including invisible and prepared records. Repeated
     * calls return the original boundary, even if the table changes. A null RecordId records an
     * initially empty table. The backing table must exist.
     */
    RecordId captureInitialDrainTail(OperationContext* opCtx);

    uint64_t count() const {
        return _counter->load();
    }

    /**
     * Performs a resumable scan on the side writes table, and either inserts or removes each key
     * from the underlying IndexAccessMethod. This will only insert as many records as are visible
     * in the current snapshot.
     *
     * Before readiness, prepare conflicts within the captured initial prefix roll back the batch
     * and propagate to the caller for an outside-lock wait. Conflicts beyond that prefix roll back
     * the batch and end the drain, leaving the remaining writes for later. After readiness, every
     * conflict rolls back and ends the attempt. The final drain throws on every conflict.
     *
     * Previously-committed batches remain applied; retries scan the remaining table from the start.
     */
    Status drainWritesIntoIndex(OperationContext* opCtx,
                                const CollectionPtr& coll,
                                const IndexCatalogEntry* indexCatalogEntry,
                                const InsertDeleteOptions& options,
                                const IndexAccessMethod::KeyHandlerFn& onDuplicateKeyFn,
                                const OnBatchAppliedFn& onBatchApplied,
                                DrainYieldPolicy drainYieldPolicy,
                                DrainPhase drainPhase);

    /**
     * Checks whether all writes have been applied.
     */
    bool checkAllWritesApplied(OperationContext* opCtx, bool fatal) const;

    uint64_t numApplied() const {
        return _numApplied;
    }

    /**
     * Cumulative count of index keys written to the index table by this tracker's drains, and
     * their total key string size in bytes. Both insertions and deletions are counted, since
     * both write to the index table.
     */
    struct DrainWriteStats {
        int64_t keysWritten = 0;
        int64_t bytesWritten = 0;
    };

    DrainWriteStats getNumKeysAndBytesWritten() const {
        return {_drainKeysWritten, _drainBytesWritten};
    }

    /**
     * Drops the temporary table. Requires a minimum timestamp to be provided, which acts as a lower
     * bound for the drop reaper, ensuring the table will stay alive until the oldest timestamp has
     * advanced past the drop time.
     */
    void dropTemporaryTable(OperationContext* opCtx, StorageEngine::DropTime dropTime) {
        _table.drop(opCtx, dropTime);
    }

private:
    /**
     * Yield lock manager locks and abandon the current storage engine snapshot.
     */
    void _yield(OperationContext* opCtx,
                const IndexCatalogEntry* indexCatalogEntry,
                const Yieldable* yieldable);

    void _checkDrainPhaseFailPoint(OperationContext* opCtx,
                                   const IndexCatalogEntry* indexCatalogEntry,
                                   FailPoint* fp,
                                   long long iteration) const;

    LazyRecordStore _table;
    boost::optional<RecordId> _initialDrainTail;

    // This allows the counter to be used in a RecoveryUnit rollback handler where the
    // IndexBuildInterceptor is no longer available (e.g. due to index build cleanup). If there
    // are additional fields that have to be referenced in commit/rollback handlers, this
    // counter should be moved to a new IndexBuildsInterceptor::InternalState structure that
    // will be managed as a shared resource.
    std::shared_ptr<Atomic<uint64_t>> _counter = std::make_shared<Atomic<uint64_t>>(0);

    uint64_t _numApplied{0};
    int64_t _drainKeysWritten{0};
    int64_t _drainBytesWritten{0};
};

}  // namespace mongo
