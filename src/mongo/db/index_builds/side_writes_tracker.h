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

    uint64_t count() const {
        return _counter->load();
    }

    /**
     * Performs a resumable scan on the side writes table, and either inserts or removes each key
     * from the underlying IndexAccessMethod. This will only insert as many records as are visible
     * in the current snapshot.
     *
     * This is resumable, so subsequent calls will start the scan at the record immediately
     * following the last inserted record from a previous call to drainWritesIntoIndex.
     */
    Status drainWritesIntoIndex(OperationContext* opCtx,
                                const CollectionPtr& coll,
                                const IndexCatalogEntry* indexCatalogEntry,
                                const InsertDeleteOptions& options,
                                const IndexAccessMethod::KeyHandlerFn& onDuplicateKeyFn,
                                DrainYieldPolicy drainYieldPolicy);

    /**
     * Check whether all writes have been applied. This will be true after a successful call to
     * `drainWritesIntoIndex()`.
     */
    bool checkAllWritesApplied(OperationContext* opCtx, bool fatal) const;

    uint64_t numApplied() const {
        return _numApplied;
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

    // This allows the counter to be used in a RecoveryUnit rollback handler where the
    // IndexBuildInterceptor is no longer available (e.g. due to index build cleanup). If there
    // are additional fields that have to be referenced in commit/rollback handlers, this
    // counter should be moved to a new IndexBuildsInterceptor::InternalState structure that
    // will be managed as a shared resource.
    std::shared_ptr<Atomic<uint64_t>> _counter = std::make_shared<Atomic<uint64_t>>(0);

    uint64_t _numApplied{0};
};

}  // namespace mongo
