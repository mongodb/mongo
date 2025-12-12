/**
 *    Copyright (C) 2025-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

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
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/db/storage/temporary_record_store.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/logv2/log.h"
#include "mongo/util/fail_point.h"

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

    SideWritesTracker(OperationContext* opCtx, std::string ident, bool resume)
        : _table([&]() {
              auto storageEngine = opCtx->getServiceContext()->getStorageEngine();
              if (resume) {
                  return storageEngine->makeTemporaryRecordStoreFromExistingIdent(
                      opCtx, ident, KeyFormat::Long);
              } else {
                  return storageEngine->makeTemporaryRecordStore(opCtx, ident, KeyFormat::Long);
              }
          }()) {}

    /**
     * Buffer a side write of the docs in `toInsert`.
     */
    Status bufferSideWrite(OperationContext* opCtx,
                           const CollectionPtr& coll,
                           const IndexCatalogEntry* indexCatalogEntry,
                           const std::vector<BSONObj>& toInsert);

    void keepTemporaryTable();

    std::string getTableIdent() const;

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

    std::unique_ptr<TemporaryRecordStore> _table;

    // This allows the counter to be used in a RecoveryUnit rollback handler where the
    // IndexBuildInterceptor is no longer available (e.g. due to index build cleanup). If there
    // are additional fields that have to be referenced in commit/rollback handlers, this
    // counter should be moved to a new IndexBuildsInterceptor::InternalState structure that
    // will be managed as a shared resource.
    std::shared_ptr<AtomicWord<uint64_t>> _counter = std::make_shared<AtomicWord<uint64_t>>(0);

    uint64_t _numApplied{0};
};

}  // namespace mongo
