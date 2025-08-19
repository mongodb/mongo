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

#include <boost/container/small_vector.hpp>
// IWYU pragma: no_include "boost/intrusive/detail/iterator.hpp"
#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/index/multikey_paths.h"
#include "mongo/db/index_builds/duplicate_key_tracker.h"
#include "mongo/db/index_builds/skipped_record_tracker.h"
#include "mongo/db/local_catalog/index_catalog_entry.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/record_id.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/storage/key_string/key_string.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/db/storage/temporary_record_store.h"
#include "mongo/db/yieldable.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/fail_point.h"

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/type_traits/decay.hpp>

namespace mongo {

class BSONObj;
class IndexAccessMethod;
struct IndexBuildInfo;
struct InsertDeleteOptions;
class OperationContext;

class IndexBuildInterceptor {
public:
    using RetrySkippedRecordMode = SkippedRecordTracker::RetrySkippedRecordMode;

    /**
     * Determines if we will yield locks while draining the side tables.
     */
    enum class DrainYieldPolicy { kNoYield, kYield };

    enum class Op { kInsert, kDelete, kUpdate };

    /**
     * Indicates whether to record duplicate keys that have been inserted into the index. When set
     * to 'kNoTrack', inserted duplicate keys will be ignored. When set to 'kTrack', a subsequent
     * call to checkDuplicateKeyConstraints is required.
     */
    enum class TrackDuplicates { kNoTrack, kTrack };

    /**
     * If 'resume' is false, creates temporary tables needed during an index build. If 'resume' is
     * true, uses the temporary tables that were previously created.
     */
    IndexBuildInterceptor(OperationContext* opCtx,
                          const IndexCatalogEntry* entry,
                          const IndexBuildInfo& indexBuildInfo,
                          bool resume);

    /**
     * Keeps the temporary side writes and duplicate key constraint violations tables.
     */
    void keepTemporaryTables();

    /**
     * Client writes that are concurrent with an index build will have their index updates written
     * to a temporary table. After the index table scan is complete, these updates will be applied
     * to the underlying index table.
     *
     * On success, `numKeysOut` if non-null will contain the number of keys added or removed.
     */
    Status sideWrite(OperationContext* opCtx,
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
                                TrackDuplicates trackDups,
                                DrainYieldPolicy drainYieldPolicy);

    SkippedRecordTracker* getSkippedRecordTracker() {
        return &_skippedRecordTracker;
    }

    const SkippedRecordTracker* getSkippedRecordTracker() const {
        return &_skippedRecordTracker;
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

    std::string getSideWritesTableIdent() const {
        return std::string{_sideWritesTable->rs()->getIdent()};
    }

    boost::optional<std::string> getDuplicateKeyTrackerTableIdent() const {
        return _duplicateKeyTracker ? boost::make_optional(_duplicateKeyTracker->getTableIdent())
                                    : boost::none;
    }

private:
    using SideWriteRecord = std::pair<RecordId, BSONObj>;

    Status _applyWrite(OperationContext* opCtx,
                       const CollectionPtr& coll,
                       const IndexCatalogEntry* indexCatalogEntry,
                       const BSONObj& doc,
                       const InsertDeleteOptions& options,
                       TrackDuplicates trackDups,
                       int64_t* keysInserted,
                       int64_t* keysDeleted);

    bool _checkAllWritesApplied(OperationContext* opCtx, bool fatal) const;

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

    Status _finishSideWrite(OperationContext* opCtx,
                            const IndexCatalogEntry* indexCatalogEntry,
                            const std::vector<BSONObj>& toInsert);

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

    mutable stdx::mutex _multikeyPathMutex;
    boost::optional<MultikeyPaths> _multikeyPaths;
};
}  // namespace mongo
