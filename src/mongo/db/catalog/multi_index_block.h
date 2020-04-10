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
#include "mongo/db/background.h"
#include "mongo/db/catalog/collection_options.h"
#include "mongo/db/catalog/index_build_block.h"
#include "mongo/db/catalog/index_catalog.h"
#include "mongo/db/index/index_access_method.h"
#include "mongo/db/index/index_build_interceptor.h"
#include "mongo/db/record_id.h"
#include "mongo/platform/mutex.h"
#include "mongo/util/fail_point.h"

namespace mongo {

extern FailPoint leaveIndexBuildUnfinishedForShutdown;

class Collection;
class MatchExpression;
class NamespaceString;
class OperationContext;

/**
 * Builds one or more indexes.
 *
 * If any method other than insert() returns a not-ok Status, this MultiIndexBlock should be
 * considered failed and must be destroyed.
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
     * By default we enforce the 'unique' flag in specs when building an index by failing.
     * If this is called before init(), we will ignore unique violations. This has no effect if
     * no specs are unique.
     *
     * If this is called, any 'dupRecords' set passed to dumpInsertsFromBulk() will never be
     * filled.
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
    StatusWith<std::vector<BSONObj>> init(OperationContext* opCtx,
                                          Collection* collection,
                                          const std::vector<BSONObj>& specs,
                                          OnInitFn onInit);
    StatusWith<std::vector<BSONObj>> init(OperationContext* opCtx,
                                          Collection* collection,
                                          const BSONObj& spec,
                                          OnInitFn onInit);

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
    static OnInitFn makeTimestampedIndexOnInitFn(OperationContext* opCtx, const Collection* coll);

    /**
     * Inserts all documents in the Collection into the indexes and logs with timing info.
     *
     * This is a simplified replacement for insert and doneInserting. Do not call this if you
     * are calling either of them.
     *
     * Will fail if violators of uniqueness constraints exist.
     *
     * Can throw an exception if interrupted.
     *
     * Should not be called inside of a WriteUnitOfWork.
     */
    Status insertAllDocumentsInCollection(OperationContext* opCtx, Collection* collection);

    /**
     * Call this after init() for each document in the collection.
     *
     * Do not call if you called insertAllDocumentsInCollection();
     *
     * Should be called inside of a WriteUnitOfWork.
     */
    Status insert(OperationContext* opCtx, const BSONObj& wholeDocument, const RecordId& loc);

    /**
     * Call this after the last insert(). This gives the index builder a chance to do any
     * long-running operations in separate units of work from commit().
     *
     * Do not call if you called insertAllDocumentsInCollection();
     *
     * If 'dupRecords' is passed as non-NULL and duplicates are not allowed for the index, violators
     * of uniqueness constraints will be added to the set. Records added to this set are not
     * indexed, so callers MUST either fail this index build or delete the documents from the
     * collection.
     *
     * Should not be called inside of a WriteUnitOfWork.
     */
    Status dumpInsertsFromBulk(OperationContext* opCtx);
    Status dumpInsertsFromBulk(OperationContext* opCtx, std::set<RecordId>* const dupRecords);

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
    Status retrySkippedRecords(OperationContext* opCtx, Collection* collection);

    /**
     * Check any constraits that may have been temporarily violated during the index build for
     * background indexes using an IndexBuildInterceptor to capture writes. The caller is
     * responsible for ensuring that all writes on the collection are visible.
     *
     * Must not be in a WriteUnitOfWork.
     */
    Status checkConstraints(OperationContext* opCtx);

    /**
     * Marks the index ready for use. Should only be called as the last method after
     * doneInserting() or insertAllDocumentsInCollection() return success.
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
                         Collection* collection,
                         OnCleanUpFn onCleanUp) noexcept;

    /**
     * Not all index aborts need this function, in particular index builds that do not need
     * to timestamp catalog writes. This is a no-op.
     */
    static OnCleanUpFn kNoopOnCleanUpFn;

    /**
     * May be called at any time after construction but before a successful commit(). Suppresses
     * the default behavior on destruction of removing all traces of uncommitted index builds. Does
     * not perform any storage engine writes. May delete internal tables, but this is not
     * transactional.
     *
     * This should only be used during shutdown or rollback.
     */
    void abortWithoutCleanup(OperationContext* opCtx);

    /**
     * Returns true if this build block supports background writes while building an index. This is
     * true for the kHybrid method.
     */
    bool isBackgroundBuilding() const;

    void setIndexBuildMethod(IndexBuildMethod indexBuildMethod);

private:
    struct IndexToBuild {
        std::unique_ptr<IndexBuildBlock> block;

        IndexAccessMethod* real = nullptr;        // owned elsewhere
        const MatchExpression* filterExpression;  // might be NULL, owned elsewhere
        std::unique_ptr<IndexAccessMethod::BulkBuilder> bulk;

        InsertDeleteOptions options;
    };

    // Is set during init() and ensures subsequent function calls act on the same Collection.
    boost::optional<UUID> _collectionUUID;

    std::vector<IndexToBuild> _indexes;

    std::unique_ptr<BackgroundOperation> _backgroundOperation;

    IndexBuildMethod _method = IndexBuildMethod::kHybrid;

    bool _ignoreUnique = false;

    // Set to true when no work remains to be done, the object can safely destruct without leaving
    // incorrect state set anywhere.
    bool _buildIsCleanedUp = true;

    // A unique identifier associating this index build with a two-phase index build within a
    // replica set.
    boost::optional<UUID> _buildUUID;
};
}  // namespace mongo
