/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include <memory>

#include "mongo/db/index/duplicate_key_tracker.h"
#include "mongo/db/index/index_access_method.h"
#include "mongo/db/index/multikey_paths.h"
#include "mongo/db/index/skipped_record_tracker.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/storage/temporary_record_store.h"
#include "mongo/platform/atomic_word.h"

namespace mongo {

class BSONObj;
class IndexAccessMethod;
class OperationContext;

class IndexBuildInterceptor {
public:
    /**
     * Determines if we will yield locks while draining the side tables.
     */
    enum class DrainYieldPolicy { kNoYield, kYield };

    enum class Op { kInsert, kDelete };

    /**
     * Indicates whether to record duplicate keys that have been inserted into the index. When set
     * to 'kNoTrack', inserted duplicate keys will be ignored. When set to 'kTrack', a subsequent
     * call to checkDuplicateKeyConstraints is required.
     */
    enum class TrackDuplicates { kNoTrack, kTrack };

    static bool typeCanFastpathMultikeyUpdates(IndexType type);

    /**
     * Creates a temporary table for writes during an index build. Additionally creates a temporary
     * table to store any duplicate key constraint violations found during the build, if the index
     * being built has uniqueness constraints.
     *
     * finalizeTemporaryTables() must be called before destruction to delete or keep the temporary
     * tables.
     */
    IndexBuildInterceptor(OperationContext* opCtx, IndexCatalogEntry* entry);

    /**
     * Finds the temporary table associated with storing writes during this index build. Only used
     * Only used when resuming an index build and the temporary table already exists on disk.
     * Additionally will find the tmeporary table associated with storing duplicate key constraint
     * violations found during the build, if the index being built has uniqueness constraints.
     *
     * finalizeTemporaryTable() must be called before destruction.
     */
    IndexBuildInterceptor(OperationContext* opCtx,
                          IndexCatalogEntry* entry,
                          StringData sideWritesIdent,
                          boost::optional<StringData> duplicateKeyTrackerIdent,
                          boost::optional<StringData> skippedRecordTrackerIdent);

    /**
     * Deletes or keeps the temporary side writes and duplicate key constraint violations tables.
     * Must be called before object destruction.
     */
    void finalizeTemporaryTables(OperationContext* opCtx,
                                 TemporaryRecordStore::FinalizationAction action);

    /**
     * Client writes that are concurrent with an index build will have their index updates written
     * to a temporary table. After the index table scan is complete, these updates will be applied
     * to the underlying index table.
     *
     * On success, `numKeysOut` if non-null will contain the number of keys added or removed.
     */
    Status sideWrite(OperationContext* opCtx,
                     const KeyStringSet& keys,
                     const KeyStringSet& multikeyMetadataKeys,
                     const MultikeyPaths& multikeyPaths,
                     RecordId loc,
                     Op op,
                     int64_t* const numKeysOut);

    /**
     * Given a duplicate key, record the key for later verification by a call to
     * checkDuplicateKeyConstraints();
     */
    Status recordDuplicateKey(OperationContext* opCtx, const KeyString::Value& key) const;

    /**
     * Returns Status::OK if all previously recorded duplicate key constraint violations have been
     * resolved for the index. Returns a DuplicateKey error if there are still duplicate key
     * constraint violations on the index.
     */
    Status checkDuplicateKeyConstraints(OperationContext* opCtx) const;


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
                                const InsertDeleteOptions& options,
                                TrackDuplicates trackDups,
                                DrainYieldPolicy drainYieldPolicy);

    SkippedRecordTracker* getSkippedRecordTracker() {
        return &_skippedRecordTracker;
    }

    const SkippedRecordTracker* getSkippedRecordTracker() const {
        return &_skippedRecordTracker;
    }

    /**
     * Tries to index previously skipped records. For each record, if the new indexing attempt is
     * successful, keys are written directly to the index. Unsuccessful key generation or writes
     * will return errors.
     */
    Status retrySkippedRecords(OperationContext* opCtx, const CollectionPtr& collection);

    /**
     * Returns 'true' if there are no visible records remaining to be applied from the side writes
     * table. Ensure that this returns 'true' when an index build is completed.
     */
    bool areAllWritesApplied(OperationContext* opCtx) const;

    /**
     * When an index builder wants to commit, use this to retrieve any recorded multikey paths
     * that were tracked during the build.
     */
    boost::optional<MultikeyPaths> getMultikeyPaths() const;

    std::string getSideWritesTableIdent() const {
        return _sideWritesTable->rs()->getIdent();
    }

    boost::optional<std::string> getDuplicateKeyTrackerTableIdent() const {
        return _duplicateKeyTracker ? boost::make_optional(_duplicateKeyTracker->getTableIdent())
                                    : boost::none;
    }

private:
    using SideWriteRecord = std::pair<RecordId, BSONObj>;


    void _initializeMultiKeyPaths(IndexCatalogEntry* entry);
    Status _applyWrite(OperationContext* opCtx,
                       const CollectionPtr& coll,
                       const BSONObj& doc,
                       const InsertDeleteOptions& options,
                       TrackDuplicates trackDups,
                       int64_t* const keysInserted,
                       int64_t* const keysDeleted);

    /**
     * Yield lock manager locks and abandon the current storage engine snapshot.
     */
    void _yield(OperationContext* opCtx);

    void _checkDrainPhaseFailPoint(OperationContext* opCtx,
                                   FailPoint* fp,
                                   long long iteration) const;

    // The entry for the index that is being built.
    IndexCatalogEntry* _indexCatalogEntry;

    // This temporary record store records intercepted keys that will be written into the index by
    // calling drainWritesIntoIndex(). It is owned by the interceptor and dropped along with it.
    std::unique_ptr<TemporaryRecordStore> _sideWritesTable;

    // Records RecordIds that have been skipped due to indexing errors.
    SkippedRecordTracker _skippedRecordTracker;

    std::unique_ptr<DuplicateKeyTracker> _duplicateKeyTracker;

    int64_t _numApplied{0};

    // This allows the counter to be used in a RecoveryUnit rollback handler where the
    // IndexBuildInterceptor is no longer available (e.g. due to index build cleanup). If there are
    // additional fields that have to be referenced in commit/rollback handlers, this counter should
    // be moved to a new IndexBuildsInterceptor::InternalState structure that will be managed as a
    // shared resource.
    std::shared_ptr<AtomicWord<long long>> _sideWritesCounter =
        std::make_shared<AtomicWord<long long>>(0);

    // Whether to skip the check the the number of writes applied is equal to the number of writes
    // recorded. Resumable index builds to not preserve these counts, so we skip this check for
    // index builds that were resumed.
    const bool _skipNumAppliedCheck = false;

    mutable Mutex _multikeyPathMutex =
        MONGO_MAKE_LATCH("IndexBuildInterceptor::_multikeyPathMutex");
    boost::optional<MultikeyPaths> _multikeyPaths;
};

}  // namespace mongo
