
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

#include <iosfwd>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "mongo/base/disallow_copying.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/background.h"
#include "mongo/db/catalog/index_catalog.h"
#include "mongo/db/index/index_access_method.h"
#include "mongo/db/record_id.h"
#include "mongo/stdx/functional.h"
#include "mongo/stdx/mutex.h"

namespace mongo {
class Collection;
class MatchExpression;
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
    MONGO_DISALLOW_COPYING(MultiIndexBlock);

public:
    MultiIndexBlock(OperationContext* opCtx, Collection* collection);
    ~MultiIndexBlock();

    /**
     * By default we ignore the 'background' flag in specs when building an index. If this is
     * called before init(), we will build the indexes in the background as long as *all* specs
     * call for background indexing. If any spec calls for foreground indexing all indexes will
     * be built in the foreground, as there is no concurrency benefit to building a subset of
     * indexes in the background, but there is a performance benefit to building all in the
     * foreground.
     */
    void allowBackgroundBuilding();

    /**
     * Call this before init() to allow the index build to be interrupted.
     * This only affects builds using the insertAllDocumentsInCollection helper.
     */
    void allowInterruption();

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
     * Prepares the index(es) for building and returns the canonicalized form of the requested index
     * specifications.
     *
     * Does not need to be called inside of a WriteUnitOfWork (but can be due to nesting).
     *
     * Requires holding an exclusive database lock.
     */
    StatusWith<std::vector<BSONObj>> init(const std::vector<BSONObj>& specs);

    StatusWith<std::vector<BSONObj>> init(const BSONObj& spec);

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
    Status insertAllDocumentsInCollection();

    /**
     * Call this after init() for each document in the collection.
     *
     * Do not call if you called insertAllDocumentsInCollection();
     *
     * Should be called inside of a WriteUnitOfWork.
     */
    Status insert(const BSONObj& wholeDocument, const RecordId& loc);

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
    Status dumpInsertsFromBulk();
    Status dumpInsertsFromBulk(std::set<RecordId>* const dupRecords);

    /**
     * For background indexes using an IndexBuildInterceptor to capture inserts during a build,
     * drain these writes into the index. If intent locks are held on the collection, more writes
     * may come in after this drain completes. To ensure that all writes are completely drained
     * before calling commit(), stop writes on the collection by holding a S or X while calling this
     * method.
     *
     * Must not be in a WriteUnitOfWork.
     */
    Status drainBackgroundWritesIfNeeded();

    /**
     * Check any constraits that may have been temporarily violated during the index build for
     * background indexes using an IndexBuildInterceptor to capture writes. The caller is
     * responsible for ensuring that all writes on the collection are visible.
     *
     * Must not be in a WriteUnitOfWork.
     */
    Status checkConstraints();

    /**
     * Marks the index ready for use. Should only be called as the last method after
     * doneInserting() or insertAllDocumentsInCollection() return success.
     *
     * Should be called inside of a WriteUnitOfWork. If the index building is to be logOp'd,
     * logOp() should be called from the same unit of work as commit().
     *
     * `onCreateFn` will be called on each index before writes that mark the index as "ready".
     *
     * Requires holding an exclusive database lock.
     */
    Status commit();
    Status commit(stdx::function<void(const BSONObj& spec)> onCreateFn);

    /**
     * Returns true if this index builder was added to the index catalog successfully.
     * In addition to having commit() return without errors, the enclosing WUOW has to be committed
     * for the indexes to show up in the index catalog.
     */
    bool isCommitted() const;

    /**
     * Signals the index build to abort.
     *
     * In-progress inserts and commits will still run to completion. However, subsequent index build
     * operations will fail an IndexBuildAborted error.
     *
     * Aborts the uncommitted index build and prevents further inserts or commit attempts from
     * proceeding. On destruction, all traces of uncommitted index builds will be removed.
     *
     * If the index build has already been aborted (using abort() or abortWithoutCleanup()),
     * this function does nothing.
     *
     * If this index build has been committed successfully, this function has no effect.
     *
     * May be called from any thread.
     */
    void abort(StringData reason);

    /**
     * May be called at any time after construction but before a successful commit(). Suppresses
     * the default behavior on destruction of removing all traces of uncommitted index builds.
     *
     * The most common use of this is if the indexes were already dropped via some other
     * mechanism such as the whole collection being dropped. In that case, it would be invalid
     * to try to remove the indexes again. Also, replication uses this to ensure that indexes
     * that are being built on shutdown are resumed on startup.
     *
     * Do not use this unless you are really sure you need to.
     *
     * Does not matter whether it is called inside of a WriteUnitOfWork. Will not be rolled
     * back.
     *
     * Must be called from owning thread.
     */
    void abortWithoutCleanup();

    bool getBuildInBackground() const;

    /**
     * State transitions:
     *
     * Uninitialized --> Running --> Committed
     *       |              |           ^
     *       |              |           |
     *       \--------------+------> Aborted
     *
     * It is possible for abort() to skip intermediate states. For example, calling abort() when the
     * index build has not been initialized will transition from Uninitialized directly to Aborted.
     *
     * In the case where we are in the midst of committing the WUOW for a successful commit() call,
     * we may transition temporarily to Aborted before finally ending at Committed. See comments for
     * MultiIndexBlock::abort().
     *
     * For testing only. Callers should not have to query the state of the MultiIndexBlock directly.
     */
    enum class State { kUninitialized, kRunning, kCommitted, kAborted };
    State getState_forTest() const;

private:
    struct IndexToBuild {
        std::unique_ptr<IndexCatalog::IndexBuildBlockInterface> block;

        IndexAccessMethod* real = NULL;           // owned elsewhere
        const MatchExpression* filterExpression;  // might be NULL, owned elsewhere
        std::unique_ptr<IndexAccessMethod::BulkBuilder> bulk;

        InsertDeleteOptions options;
    };

    Status _dumpInsertsFromBulk(std::set<RecordId>* dupRecords,
                                std::vector<BSONObj>* dupKeysInserted);

    /**
     * Returns the current state.
     */
    State _getState() const;

    /**
     * Updates the current state to a non-Aborted state.
     */
    void _setState(State newState);

    /**
     * Updates the current state to Aborted with the given reason.
     */
    void _setStateToAbortedIfNotCommitted(StringData reason);

    /**
     * Updates CurOp's 'opDescription' field with the current state of this index build.
     */
    void _updateCurOpOpDescription(bool isBuildingPhaseComplete) const;

    std::vector<IndexToBuild> _indexes;

    std::unique_ptr<BackgroundOperation> _backgroundOperation;

    // Pointers not owned here and must outlive 'this'
    Collection* _collection;
    OperationContext* _opCtx;

    bool _buildInBackground = false;
    bool _allowInterruption = false;
    bool _ignoreUnique = false;

    bool _needToCleanup = true;

    // Protects member variables of this class declared below.
    mutable stdx::mutex _mutex;

    State _state = State::kUninitialized;
    std::string _abortReason;
};

// For unit tests that need to check MultiIndexBlock states.
// The ASSERT_*() macros use this function to print the value of 'state' when the predicate fails.
std::ostream& operator<<(std::ostream& os, const MultiIndexBlock::State& state);

}  // namespace mongo
