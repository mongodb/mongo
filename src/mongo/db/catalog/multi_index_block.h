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

#include <functional>
#include <iosfwd>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/catalog/collection_options.h"
#include "mongo/db/catalog/index_build_block.h"
#include "mongo/db/catalog/index_catalog.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/index/index_access_method.h"
#include "mongo/db/index/index_build_interceptor.h"
#include "mongo/db/record_id.h"
#include "mongo/db/resumable_index_builds_gen.h"
#include "mongo/platform/mutex.h"
#include "mongo/util/fail_point.h"

namespace mongo {

extern FailPoint leaveIndexBuildUnfinishedForShutdown;

class Collection;
class CollectionPtr;
class MatchExpression;
class NamespaceString;
class OperationContext;
class ProgressMeterHolder;

/**
 * Builds one or more indexes.
 *
 * If any method other than insertSingleDocumentForInitialSyncOrRecovery() returns a not-ok Status,
 * this MultiIndexBlock should be considered failed and must be destroyed.
 *
 * If a MultiIndexBlock is destroyed before commit() or if commit() is rolled back, it will
 * clean up all traces of the indexes being constructed. MultiIndexBlocks should not be
 * destructed from inside of a WriteUnitOfWork as any cleanup needed should never be rolled back
 * (as it is itself essentially a form of rollback, you don't want to "rollback the rollback").
 */
class MultiIndexBlock {
    MultiIndexBlock(const MultiIndexBlock&) = delete;
    MultiIndexBlock& operator=(const MultiIndexBlock&) = delete;

public:
    MultiIndexBlock() = default;
    ~MultiIndexBlock();

    /**
     * When this is called:
     * For hybrid index builds, the index interceptor will not track duplicates.
     * For foreground index builds, the uniqueness constraint will be relaxed.
     */
    void ignoreUniqueConstraint();

    /**
     * Sets an index build UUID associated with the indexes for this builder. This call is required
     * for two-phase index builds.
     */
    void setTwoPhaseBuildUUID(UUID indexBuildUUID) {
        _buildUUID = indexBuildUUID;
    }

    /**
     * Prepares the index(es) for building and returns the canonicalized form of the requested index
     * specifications.
     *
     * Calls 'onInitFn' in the same WriteUnitOfWork as the 'ready: false' write to the index after
     * all indexes have been initialized. For callers that timestamp this write, use
     * 'makeTimestampedIndexOnInitFn', otherwise use 'kNoopOnInitFn'.
     *
     * Does not need to be called inside of a WriteUnitOfWork (but can be due to nesting).
     *
     * Requires holding an exclusive lock on the collection.
     */
    using OnInitFn = std::function<Status(std::vector<BSONObj>& specs)>;
    StatusWith<std::vector<BSONObj>> init(
        OperationContext* opCtx,
        CollectionWriter& collection,
        const std::vector<BSONObj>& specs,
        OnInitFn onInit,
        const boost::optional<ResumeIndexInfo>& resumeInfo = boost::none);
    StatusWith<std::vector<BSONObj>> init(OperationContext* opCtx,
                                          CollectionWriter& collection,
                                          const BSONObj& spec,
                                          OnInitFn onInit);
    StatusWith<std::vector<BSONObj>> initForResume(OperationContext* opCtx,
                                                   const CollectionPtr& collection,
                                                   const std::vector<BSONObj>& specs,
                                                   const ResumeIndexInfo& resumeInfo);

    /**
     * Not all index initializations need an OnInitFn, in particular index builds that do not need
     * to timestamp catalog writes. This is a no-op.
     */
    static OnInitFn kNoopOnInitFn;

    /**
     * Returns an OnInit function for initialization when this index build should be timestamped.
     * When called on primaries, this generates a new optime, writes a no-op oplog entry, and
     * timestamps the first catalog write. Does nothing on secondaries.
     */
    static OnInitFn makeTimestampedIndexOnInitFn(OperationContext* opCtx,
                                                 const CollectionPtr& coll);

    /**
     * Inserts all documents in the Collection into the indexes and logs with timing info.
     *
     * This is a replacement for calling both insertSingleDocumentForInitialSyncOrRecovery and
     * dumpInsertsFromBulk. Do not call this if you are calling either of them.
     *
     * Will fail if violators of uniqueness constraints exist.
     *
     * Can throw an exception if interrupted.
     *
     * Should not be called inside of a WriteUnitOfWork.
     */
    Status insertAllDocumentsInCollection(
        OperationContext* opCtx,
        const CollectionPtr& collection,
        const boost::optional<RecordId>& resumeAfterRecordId = boost::none);

    /**
     * Call this after init() for each document in the collection.
     *
     * Do not call if you called insertAllDocumentsInCollection();
     *
     * Should be called inside of a WriteUnitOfWork.
     *
     * 'saveCursorBeforeWrite' and 'restoreCursorAfterWrite' will only be called if an index
     * constraint violation is found and written out to the constraint violation side table. Any
     * open cursors held by the caller should be saved in 'saveCursorBeforeWrite' and restored in
     * 'saveCursorBeforeWrite'. Otherwise, the cursors may get reset unexpectedly because of an
     * internally handled WCE.
     */
    Status insertSingleDocumentForInitialSyncOrRecovery(
        OperationContext* opCtx,
        const CollectionPtr& collection,
        const BSONObj& wholeDocument,
        const RecordId& loc,
        const std::function<void()>& saveCursorBeforeWrite,
        const std::function<void()>& restoreCursorAfterWrite);

    /**
     * Call this after the last insertSingleDocumentForInitialSyncOrRecovery(). This gives the index
     * builder a chance to do any long-running operations in separate units of work from commit().
     *
     * Do not call if you called insertAllDocumentsInCollection();
     *
     * If 'onDuplicateRecord' is passed as non-NULL and duplicates are not allowed for the index,
     * violators of uniqueness constraints will be handled by 'onDuplicateRecord'.
     *
     * Should not be called inside of a WriteUnitOfWork.
     */
    Status dumpInsertsFromBulk(OperationContext* opCtx, const CollectionPtr& collection);
    Status dumpInsertsFromBulk(OperationContext* opCtx,
                               const CollectionPtr& collection,
                               const IndexAccessMethod::RecordIdHandlerFn& onDuplicateRecord);
    /**
     * For background indexes using an IndexBuildInterceptor to capture inserts during a build,
     * drain these writes into the index. If intent locks are held on the collection, more writes
     * may come in after this drain completes. To ensure that all writes are completely drained
     * before calling commit(), stop writes on the collection by holding a S or X while calling this
     * method.
     *
     * When 'readSource' is not kUnset, perform the drain by reading at the timestamp described by
     * the ReadSource.
     *
     * Must not be in a WriteUnitOfWork.
     */
    Status drainBackgroundWrites(OperationContext* opCtx,
                                 RecoveryUnit::ReadSource readSource,
                                 IndexBuildInterceptor::DrainYieldPolicy drainYieldPolicy);


    /**
     * Retries key generation and insertion for all records skipped during the collection scanning
     * phase.
     *
     * Index builds ignore key generation errors on secondaries. In steady-state replication, all
     * writes from the primary are eventually applied, so an index build should always succeed when
     * the primary commits. In two-phase index builds, a secondary may become primary in the middle
     * of an index build, so it must ensure that before it finishes, it has indexed all documents in
     * a collection, requiring a call to this function upon completion.
     */
    Status retrySkippedRecords(OperationContext* opCtx, const CollectionPtr& collection);

    /**
     * Check any constraits that may have been temporarily violated during the index build for
     * background indexes using an IndexBuildInterceptor to capture writes. The caller is
     * responsible for ensuring that all writes on the collection are visible.
     *
     * Must not be in a WriteUnitOfWork.
     */
    Status checkConstraints(OperationContext* opCtx, const CollectionPtr& collection);

    /**
     * Marks the index ready for use. Should only be called as the last method after
     * dumpInsertsFromBulk() or insertAllDocumentsInCollection() return success.
     *
     * Should be called inside of a WriteUnitOfWork. If the index building is to be logOp'd,
     * logOp() should be called from the same unit of work as commit().
     *
     * `onCreateEach` will be called after each index has been marked as "ready".
     * `onCommit` will be called after all indexes have been marked "ready".
     *
     * Requires holding an exclusive lock on the collection.
     */
    using OnCommitFn = std::function<void()>;
    using OnCreateEachFn = std::function<void(const BSONObj& spec)>;
    Status commit(OperationContext* opCtx,
                  Collection* collection,
                  OnCreateEachFn onCreateEach,
                  OnCommitFn onCommit);

    /**
     * Not all index commits need these functions, in particular index builds that do not need
     * to timestamp catalog writes. These are no-ops.
     */
    static OnCreateEachFn kNoopOnCreateEachFn;
    static OnCommitFn kNoopOnCommitFn;

    /**
     * Ensures the index build state is cleared correctly after index build failure.
     *
     * Must be called before object destruction if init() has been called; and safe to call if
     * init() has not been called.
     *
     * By only requiring this call after init(), we allow owners of the object to exit without
     * further handling if they never use the object.
     *
     * `onCleanUp` will be called after all indexes have been removed from the catalog.
     */
    using OnCleanUpFn = std::function<void()>;
    void abortIndexBuild(OperationContext* opCtx,
                         CollectionWriter& collection,
                         OnCleanUpFn onCleanUp) noexcept;

    /**
     * Not all index aborts need this function, in particular index builds that do not need
     * to timestamp catalog writes. This is a no-op.
     */
    static OnCleanUpFn kNoopOnCleanUpFn;

    /**
     * Returns an onCleanUp function for clean up when this index build should be timestamped. When
     * called on primaries, this generates a new optime, writes a no-op oplog entry, and timestamps
     * the catalog write to remove the index entry. Does nothing on secondaries.
     */
    static OnCleanUpFn makeTimestampedOnCleanUpFn(OperationContext* opCtx,
                                                  const CollectionPtr& coll);

    /**
     * May be called at any time after construction but before a successful commit(). Suppresses
     * the default behavior on destruction of removing all traces of uncommitted index builds. May
     * delete internal tables, but this is not transactional. Writes the resumable index build
     * state to disk if resumable index builds are supported.
     *
     * This should only be used during shutdown or rollback.
     */
    void abortWithoutCleanup(OperationContext* opCtx,
                             const CollectionPtr& collection,
                             bool isResumable);

    /**
     * Returns true if this build block supports background writes while building an index. This is
     * true for the kHybrid method.
     */
    bool isBackgroundBuilding() const;

    void setIndexBuildMethod(IndexBuildMethod indexBuildMethod);

    /**
     * Appends the current state information of the index build to the builder.
     */
    void appendBuildInfo(BSONObjBuilder* builder) const;

private:
    struct IndexToBuild {
        std::unique_ptr<IndexBuildBlock> block;

        IndexAccessMethod* real = nullptr;        // owned elsewhere
        const MatchExpression* filterExpression;  // might be NULL, owned elsewhere
        std::unique_ptr<IndexAccessMethod::BulkBuilder> bulk;

        InsertDeleteOptions options;
    };

    void _writeStateToDisk(OperationContext* opCtx, const CollectionPtr& collection) const;

    BSONObj _constructStateObject(OperationContext* opCtx, const CollectionPtr& collection) const;

    Status _failPointHangDuringBuild(OperationContext* opCtx,
                                     FailPoint* fp,
                                     StringData where,
                                     const BSONObj& doc,
                                     unsigned long long iteration) const;

    Status _insert(OperationContext* opCtx,
                   const CollectionPtr& collection,
                   const BSONObj& wholeDocument,
                   const RecordId& loc,
                   const std::function<void()>& saveCursorBeforeWrite,
                   const std::function<void()>& restoreCursorAfterWrite);

    /**
     * Performs a collection scan on the given collection and inserts the relevant index keys into
     * the external sorter.
     */
    void _doCollectionScan(OperationContext* opCtx,
                           const CollectionPtr& collection,
                           const boost::optional<RecordId>& resumeAfterRecordId,
                           ProgressMeterHolder* progress);

    // Is set during init() and ensures subsequent function calls act on the same Collection.
    boost::optional<UUID> _collectionUUID;

    std::vector<IndexToBuild> _indexes;

    IndexBuildMethod _method = IndexBuildMethod::kHybrid;

    bool _ignoreUnique = false;

    // True if one or more indexes being built are on time-series measurements.
    bool _containsIndexBuildOnTimeseriesMeasurement = false;

    // True if at least one bucket document contains mixed-schema data and
    // '_containsIndexBuildOnTimeseriesMeasurement=true'.
    bool _timeseriesBucketContainsMixedSchemaData = false;

    // Set to true when no work remains to be done, the object can safely destruct without leaving
    // incorrect state set anywhere.
    bool _buildIsCleanedUp = true;

    // A unique identifier associating this index build with a two-phase index build within a
    // replica set.
    boost::optional<UUID> _buildUUID;

    // The RecordId corresponding to the object most recently inserted using this MultiIndexBlock,
    // or boost::none if nothing has been inserted.
    boost::optional<RecordId> _lastRecordIdInserted;

    // The current phase of the index build.
    IndexBuildPhaseEnum _phase = IndexBuildPhaseEnum::kInitialized;
};
}  // namespace mongo
