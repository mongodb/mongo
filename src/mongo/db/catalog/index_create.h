/**
 *    Copyright (C) 2017 10gen Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include <memory>
#include <set>
#include <string>
#include <vector>

#include "mongo/base/disallow_copying.h"
#include "mongo/base/status.h"
#include "mongo/db/catalog/index_catalog.h"
#include "mongo/db/catalog/index_catalog_impl.h"
#include "mongo/db/index/index_access_method.h"
#include "mongo/db/record_id.h"
#include "mongo/stdx/functional.h"

namespace mongo {
class BackgroundOperation;
class BSONObj;
class Collection;
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
public:
    class Impl {
    public:
        virtual ~Impl() = 0;

        virtual void allowBackgroundBuilding() = 0;

        virtual void allowInterruption() = 0;

        virtual void ignoreUniqueConstraint() = 0;

        virtual void removeExistingIndexes(std::vector<BSONObj>* specs) const = 0;

        virtual StatusWith<std::vector<BSONObj>> init(const std::vector<BSONObj>& specs) = 0;

        virtual StatusWith<std::vector<BSONObj>> init(const BSONObj& spec) = 0;

        virtual Status insertAllDocumentsInCollection(std::set<RecordId>* dupsOut = NULL) = 0;

        virtual Status insert(const BSONObj& wholeDocument, const RecordId& loc) = 0;

        virtual Status doneInserting(std::set<RecordId>* dupsOut = NULL) = 0;

        virtual void commit(stdx::function<void(const BSONObj& spec)> onCreateFn) = 0;

        virtual void abortWithoutCleanup() = 0;

        virtual bool getBuildInBackground() const = 0;
    };

private:
    std::unique_ptr<Impl> _pimpl;

    // This structure exists to give us a customization point to decide how to force users of this
    // class to depend upon the corresponding `index_create.cpp` Translation Unit (TU).  All public
    // forwarding functions call `_impl(), and `_impl` creates an instance of this structure.
    struct TUHook {
        static void hook() noexcept;

        explicit inline TUHook() noexcept {
            if (kDebugBuild)
                this->hook();
        }
    };

    inline const Impl& _impl() const {
        TUHook{};
        return *this->_pimpl;
    }

    inline Impl& _impl() {
        TUHook{};
        return *this->_pimpl;
    }

public:
    static MONGO_DECLARE_SHIM((OperationContext * opCtx,
                               Collection* collection,
                               PrivateTo<MultiIndexBlock>)
                                  ->std::unique_ptr<Impl>) makeImpl;

    inline ~MultiIndexBlock() = default;

    /**
     * Neither pointer is owned.
     */
    inline explicit MultiIndexBlock(OperationContext* const opCtx, Collection* const collection)
        : _pimpl(makeImpl(opCtx, collection, PrivateCall<MultiIndexBlock>{})) {}

    /**
     * By default we ignore the 'background' flag in specs when building an index. If this is
     * called before init(), we will build the indexes in the background as long as *all* specs
     * call for background indexing. If any spec calls for foreground indexing all indexes will
     * be built in the foreground, as there is no concurrency benefit to building a subset of
     * indexes in the background, but there is a performance benefit to building all in the
     * foreground.
     */
    inline void allowBackgroundBuilding() {
        return this->_impl().allowBackgroundBuilding();
    }

    /**
     * Call this before init() to allow the index build to be interrupted.
     * This only affects builds using the insertAllDocumentsInCollection helper.
     */
    inline void allowInterruption() {
        return this->_impl().allowInterruption();
    }

    /**
     * By default we enforce the 'unique' flag in specs when building an index by failing.
     * If this is called before init(), we will ignore unique violations. This has no effect if
     * no specs are unique.
     *
     * If this is called, any dupsOut sets passed in will never be filled.
     */
    inline void ignoreUniqueConstraint() {
        return this->_impl().ignoreUniqueConstraint();
    }

    /**
     * Removes pre-existing indexes from 'specs'. If this isn't done, init() may fail with
     * IndexAlreadyExists.
     */
    inline void removeExistingIndexes(std::vector<BSONObj>* const specs) const {
        return this->_impl().removeExistingIndexes(specs);
    }

    /**
     * Prepares the index(es) for building and returns the canonicalized form of the requested index
     * specifications.
     *
     * Does not need to be called inside of a WriteUnitOfWork (but can be due to nesting).
     *
     * Requires holding an exclusive database lock.
     */
    inline StatusWith<std::vector<BSONObj>> init(const std::vector<BSONObj>& specs) {
        return this->_impl().init(specs);
    }

    inline StatusWith<std::vector<BSONObj>> init(const BSONObj& spec) {
        return this->_impl().init(spec);
    }

    /**
     * Inserts all documents in the Collection into the indexes and logs with timing info.
     *
     * This is a simplified replacement for insert and doneInserting. Do not call this if you
     * are calling either of them.
     *
     * If dupsOut is passed as non-NULL, violators of uniqueness constraints will be added to
     * the set rather than failing the build. Documents added to this set are not indexed, so
     * callers MUST either fail this index build or delete the documents from the collection.
     *
     * Can throw an exception if interrupted.
     *
     * Should not be called inside of a WriteUnitOfWork.
     */
    inline Status insertAllDocumentsInCollection(std::set<RecordId>* const dupsOut = nullptr) {
        return this->_impl().insertAllDocumentsInCollection(dupsOut);
    }

    /**
     * Call this after init() for each document in the collection.
     *
     * Do not call if you called insertAllDocumentsInCollection();
     *
     * Should be called inside of a WriteUnitOfWork.
     */
    inline Status insert(const BSONObj& wholeDocument, const RecordId& loc) {
        return this->_impl().insert(wholeDocument, loc);
    }

    /**
     * Call this after the last insert(). This gives the index builder a chance to do any
     * long-running operations in separate units of work from commit().
     *
     * Do not call if you called insertAllDocumentsInCollection();
     *
     * If dupsOut is passed as non-NULL, violators of uniqueness constraints will be added to
     * the set. Documents added to this set are not indexed, so callers MUST either fail this
     * index build or delete the documents from the collection.
     *
     * Should not be called inside of a WriteUnitOfWork.
     */
    inline Status doneInserting(std::set<RecordId>* const dupsOut = nullptr) {
        return this->_impl().doneInserting(dupsOut);
    }

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
    inline void commit(stdx::function<void(const BSONObj& spec)> onCreateFn = nullptr) {
        return this->_impl().commit(onCreateFn);
    }

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
     */
    inline void abortWithoutCleanup() {
        return this->_impl().abortWithoutCleanup();
    }

    inline bool getBuildInBackground() const {
        return this->_impl().getBuildInBackground();
    }
};
}  // namespace mongo
