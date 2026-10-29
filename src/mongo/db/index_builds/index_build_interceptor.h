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
#include "mongo/util/string_map.h"

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

    // Field names of the multikey state a side write record carries.
    static constexpr std::string_view kSideWriteMultikeyFieldName = "multikey";
    static constexpr std::string_view kSideWriteMultikeyPathsFieldName = "multikeyPaths";

    enum class Op { kInsert, kDelete, kMultikey };

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
     * Invoked, inside the transaction draining a batch, when that batch recovered multikey state
     * from its records. It is handed everything this interceptor knows so far. The records that
     * carried that state are deleted by the same transaction, so a caller that needs it to outlive
     * this node has to persist it here; pass an empty function to drop it instead.
     */
    using OnMultikeyPathsRecoveredFn =
        std::function<Status(OperationContext*, const MultikeyPaths&)>;

    /**
     * Drain the writes from the side writes table/tracker into the
     * index identified by `indexCatalogEntry`.
     */
    Status drainWritesIntoIndex(OperationContext* opCtx,
                                const CollectionPtr& coll,
                                const IndexCatalogEntry* indexCatalogEntry,
                                const InsertDeleteOptions& options,
                                const OnMultikeyPathsRecoveredFn& onMultikeyPathsRecovered,
                                TrackDuplicates trackDups,
                                DrainYieldPolicy drainYieldPolicy);

    /**
     * Returns the cumulative number of keys and key bytes this interceptor's drains have written to
     * the index table.
     */
    SideWritesTracker::DrainWriteStats getNumKeysAndBytesWritten() const {
        return _sideWritesTracker.getNumKeysAndBytesWritten();
    }

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
        const OnMultikeyPathsRecoveredFn& onMultikeyPathsRecovered,
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
     * Records multikey paths recovered from a side write that this node's drain applied.
     */
    void recordDrainedMultikeyPaths(const MultikeyPaths& multikeyPaths);

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

    /**
     * Merges 'multikeyPaths' into '_multikeyPaths'
     */
    void _mergeMultikeyPaths(const MultikeyPaths& multikeyPaths);

    // Set when a drained record hands multikey state back, cleared once that state has been
    // reported to the drain's OnMultikeyPathsRecoveredFn.
    bool _multikeyPathsRecovered = false;

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

namespace index_builds {

/**
 * Interceptors created before the index builds that will own them exist, keyed by index ident.
 *
 * A step-up creates these for the builds it is about to resume, so writes accepted before those
 * builds set themselves up are recorded. An entry is only meaningful while this node is primary
 * and the build it belongs to has yet to adopt it.
 *
 * A build adopts its entry while setting up, which erases it. Anything not adopted is cleared
 * when the step-up task that created it ends, by which point no build will.
 */
class PendingInterceptors {
public:
    /** Returns the pending interceptor for 'indexIdent', or null. */
    std::shared_ptr<IndexBuildInterceptor> find(std::string_view indexIdent) const;

    bool contains(std::string_view indexIdent) const;

    void add(std::string_view indexIdent, std::shared_ptr<IndexBuildInterceptor> interceptor);

    void erase(std::string_view indexIdent);

    void clear();

private:
    mutable std::mutex _mutex;
    StringMap<std::shared_ptr<IndexBuildInterceptor>> _interceptors;
};

PendingInterceptors& getPendingInterceptors(ServiceContext* svcCtx);

}  // namespace index_builds

}  // namespace mongo
