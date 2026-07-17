// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/index/index_access_method.h"
#include "mongo/db/index/multikey_paths.h"
#include "mongo/db/index_builds/duplicate_key_tracker.h"
#include "mongo/db/index_builds/index_builds_common.h"
#include "mongo/db/index_builds/side_writes_tracker.h"
#include "mongo/db/index_builds/skipped_record_tracker.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/record_id.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/shard_role/shard_catalog/index_catalog_entry.h"
#include "mongo/db/storage/key_string/key_string.h"
#include "mongo/db/storage/lazy_record_store.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/util/modules.h"

#include <cstdint>
#include <memory>
#include <mutex>
#include <utility>

#include <boost/optional/optional.hpp>

namespace [[MONGO_MOD_PUBLIC]] mongo {
class IndexBuildInterceptor {
public:
    using RetrySkippedRecordMode = SkippedRecordTracker::RetrySkippedRecordMode;
    using DrainYieldPolicy = SideWritesTracker::DrainYieldPolicy;

    enum class Op { kInsert, kDelete };

    /**
     * Indicates whether to record duplicate keys that have been inserted into the index. When set
     * to 'kNoTrack', inserted duplicate keys will be ignored. When set to 'kTrack', a subsequent
     * call to checkDuplicateKeyConstraints is required.
     */
    enum class TrackDuplicates { kNoTrack, kTrack };

    /**
     * Based on the create mode, it either creates temporary tables needed during an index build or
     * uses the existing temporary tables that were previously created.
     */
    IndexBuildInterceptor(OperationContext* opCtx,
                          const IndexBuildInfo& indexBuildInfo,
                          LazyRecordStore::CreateMode createMode,
                          bool unique);

    /**
     * Client writes that are concurrent with an index build will have their index updates written
     * to a temporary table. After the index table scan is complete, these updates will be applied
     * to the underlying index table.
     *
     * On success, `numKeysOut` if non-null will contain the number of keys added or removed.
     */
    Status sideWrite(OperationContext* opCtx,
                     const CollectionPtr& coll,
                     const IndexCatalogEntry* indexCatalogEntry,
                     const KeyStringSet& keys,
                     const KeyStringSet& multikeyMetadataKeys,
                     const MultikeyPaths& multikeyPaths,
                     Op op,
                     int64_t* numKeysOut);


    /**
     * Given a duplicate key, record the key for later verification by a call to
     * checkDuplicateKeyConstraints();
     */
    Status recordDuplicateKey(OperationContext* opCtx,
                              const CollectionPtr& coll,
                              const IndexCatalogEntry* indexCatalogEntry,
                              const key_string::View& key) const;

    /**
     * Returns Status::OK if all previously recorded duplicate key constraint violations have been
     * resolved for the index. Returns a DuplicateKey error if there are still duplicate key
     * constraint violations on the index.
     */
    Status checkDuplicateKeyConstraints(OperationContext* opCtx,
                                        const CollectionPtr&,
                                        const IndexCatalogEntry* indexCatalogEntry) const;

    /**
     * Drain the writes from the side writes table/tracker into the
     * index identified by `indexCatalogEntry`.
     */
    Status drainWritesIntoIndex(OperationContext* opCtx,
                                const CollectionPtr& coll,
                                const IndexCatalogEntry* indexCatalogEntry,
                                const InsertDeleteOptions& options,
                                TrackDuplicates trackDups,
                                DrainYieldPolicy drainYieldPolicy);

    [[MONGO_MOD_PRIVATE]] SkippedRecordTracker& getSkippedRecordTracker() {
        return _skippedRecordTracker;
    }

    [[MONGO_MOD_PRIVATE]] const SkippedRecordTracker& getSkippedRecordTracker() const {
        return _skippedRecordTracker;
    }

    /**
     * Returns true if any records were skipped. If this returns false, retrySkippedRecords() will
     * be a no-op.
     */
    bool hasAnySkippedRecords(OperationContext* opCtx) const {
        return !_skippedRecordTracker.areAllRecordsApplied(opCtx);
    }

    /**
     * By default, tries to generate keys and insert previously skipped records in the index. For
     * each record, if the new indexing attempt is successful, keys are written directly to the
     * index. Unsuccessful key generation or writes will return errors.
     *
     * The behaviour can be modified by specifying a RetrySkippedRecordMode.
     */
    Status retrySkippedRecords(
        OperationContext* opCtx,
        const CollectionPtr& collection,
        const IndexCatalogEntry* indexCatalogEntry,
        RetrySkippedRecordMode mode = RetrySkippedRecordMode::kKeyGenerationAndInsertion);

    /**
     * Returns whether there are no visible records remaining to be applied from the side writes
     * table.
     */
    bool areAllWritesApplied(OperationContext* opCtx) const;

    /**
     * Invariants that there are no visible records remaining to be applied from the side writes
     * table.
     */
    void invariantAllWritesApplied(OperationContext* opCtx) const;

    /**
     * When an index builder wants to commit, use this to retrieve any recorded multikey paths
     * that were tracked during the build.
     */
    boost::optional<MultikeyPaths> getMultikeyPaths() const;

    /**
     * Creates a ContainerSpiller from the _sorterTable.
     */
    IntegerKeyedContainer& getSorterContainer() {
        invariant(_sorterTable);
        auto& rs = _sorterTable->getTableOrThrow();
        return std::get<std::reference_wrapper<IntegerKeyedContainer>>(rs.getContainer()).get();
    }

    /**
     * Force-creates any backing tables still in deferred mode.
     */
    void createDeferredTables(OperationContext* opCtx) {
        _sideWritesTracker.createDeferredTable(opCtx);
        _skippedRecordTracker.createDeferredTable(opCtx);
        if (_duplicateKeyTracker) {
            _duplicateKeyTracker->createDeferredTable(opCtx);
        }
    }

    void dropTemporaryTables(OperationContext* opCtx, StorageEngine::DropTime dropTime) {
        if (_sorterTable) {
            _sorterTable->drop(opCtx, dropTime);
        }
        if (_duplicateKeyTracker) {
            _duplicateKeyTracker->dropTemporaryTable(opCtx, dropTime);
        }
        _skippedRecordTracker.dropTemporaryTable(opCtx, dropTime);
        _sideWritesTracker.dropTemporaryTable(opCtx, dropTime);
    }

private:
    using SideWriteRecord = std::pair<RecordId, BSONObj>;

    bool _checkAllWritesApplied(OperationContext* opCtx, bool fatal) const;

    // This temporary record store records all the index keys that we encounter upon collection
    // scan. We will use the _sorterTable for primary-driven index builds to replicate sorting and
    // inserting the sorted index keys into each node's index table.
    boost::optional<LazyRecordStore> _sorterTable;

    // This temporary record store records intercepted keys that will be written into the index by
    // calling drainWritesIntoIndex(). It is owned by the interceptor and dropped along with it.
    SideWritesTracker _sideWritesTracker;

    // Records RecordIds that have been skipped due to indexing errors.
    SkippedRecordTracker _skippedRecordTracker;

    std::unique_ptr<DuplicateKeyTracker> _duplicateKeyTracker;

    // Whether to skip the check the number of writes applied is equal to the number of writes
    // recorded. Resumable index builds do not preserve these counts, so we skip this check for
    // index builds that were resumed.
    const bool _skipNumAppliedCheck = false;

    mutable std::mutex _multikeyPathMutex;
    boost::optional<MultikeyPaths> _multikeyPaths;
};
}  // namespace mongo
