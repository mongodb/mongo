/**
 *    Copyright (C) 2014 MongoDB Inc.
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

#include <string>

#include "mongo/db/catalog/catalog_raii.h"
#include "mongo/db/stats/top.h"
#include "mongo/util/timer.h"

namespace mongo {

/**
 * RAII-style class which automatically tracks the operation namespace in CurrentOp and records the
 * operation via Top upon destruction.
 */
class AutoStatsTracker {
    MONGO_DISALLOW_COPYING(AutoStatsTracker);

public:
    /**
     * Sets the namespace of the CurOp object associated with 'opCtx' to be 'nss' and starts the
     * CurOp timer. 'lockType' describes which type of lock is held by this operation, and will be
     * used for reporting via Top. If 'dbProfilingLevel' is not given, this constructor will acquire
     * and then drop a database lock in order to determine the database's profiling level.
     */
    AutoStatsTracker(OperationContext* opCtx,
                     const NamespaceString& nss,
                     Top::LockType lockType,
                     boost::optional<int> dbProfilingLevel);

    /**
     * Records stats about the current operation via Top.
     */
    ~AutoStatsTracker();

private:
    OperationContext* _opCtx;
    Top::LockType _lockType;
};

/**
 * RAII-style class, which would acquire the appropriate hierarchy of locks for obtaining
 * a particular collection and would retrieve a reference to the collection. In addition, this
 * utility will ensure that the read will be performed against an appropriately committed snapshot
 * if the operation is using a readConcern of 'majority'.
 *
 * Use this when you want to read the contents of a collection, but you are not at the top-level of
 * some command. This will ensure your reads obey any requested readConcern, but will not update the
 * status of CurrentOp, or add a Top entry.
 *
 * It is guaranteed that locks will be released when this object goes out of scope, therefore
 * database and collection references returned by this class should not be retained.
 */
class AutoGetCollectionForRead {
    MONGO_DISALLOW_COPYING(AutoGetCollectionForRead);

public:
    AutoGetCollectionForRead(OperationContext* opCtx, const NamespaceString& nss)
        : AutoGetCollectionForRead(opCtx, nss, AutoGetCollection::ViewMode::kViewsForbidden) {}

    AutoGetCollectionForRead(OperationContext* opCtx, const StringData dbName, const UUID& uuid);

    /**
     * This constructor is intended for internal use and should not be used outside this file.
     * AutoGetCollectionForReadCommand and AutoGetCollectionOrViewForReadCommand use 'viewMode' to
     * determine whether or not it is permissible to obtain a handle on a view namespace. Use
     * another constructor or another 'AutoGet' class instead.
     */
    AutoGetCollectionForRead(OperationContext* opCtx,
                             const NamespaceString& nss,
                             AutoGetCollection::ViewMode viewMode);

    AutoGetCollectionForRead(OperationContext* opCtx,
                             const NamespaceString& nss,
                             AutoGetCollection::ViewMode viewMode,
                             Lock::DBLock lock);
    Database* getDb() const {
        if (!_autoColl) {
            return nullptr;
        }
        return _autoColl->getDb();
    }

    Collection* getCollection() const {
        if (!_autoColl) {
            return nullptr;
        }
        return _autoColl->getCollection();
    }

private:
    void _ensureMajorityCommittedSnapshotIsValid(const NamespaceString& nss,
                                                 OperationContext* opCtx);

    boost::optional<AutoGetCollection> _autoColl;
};

/**
 * RAII-style class, which would acquire the appropriate hierarchy of locks for obtaining
 * a particular collection and would retrieve a reference to the collection. In addition, this
 * utility validates the shard version for the specified namespace and sets the current operation's
 * namespace for the duration while this object is alive.
 *
 * Use this when you are a read-only command and you know that your target namespace is a collection
 * (not a view). In addition to ensuring your read obeys any requested readConcern, this will add a
 * Top entry upon destruction and ensure the CurrentOp object has the right namespace and has
 * started its timer.
 *
 * It is guaranteed that locks will be released when this object goes out of scope, therefore
 * database and collection references returned by this class should not be retained.
 */
class AutoGetCollectionForReadCommand {
    MONGO_DISALLOW_COPYING(AutoGetCollectionForReadCommand);

public:
    AutoGetCollectionForReadCommand(OperationContext* opCtx, const NamespaceString& nss)
        : AutoGetCollectionForReadCommand(
              opCtx, nss, AutoGetCollection::ViewMode::kViewsForbidden) {}

    AutoGetCollectionForReadCommand(OperationContext* opCtx,
                                    const NamespaceString& nss,
                                    Lock::DBLock lock)
        : AutoGetCollectionForReadCommand(
              opCtx, nss, AutoGetCollection::ViewMode::kViewsForbidden, std::move(lock)) {}

    AutoGetCollectionForReadCommand(OperationContext* opCtx,
                                    const StringData dbName,
                                    const UUID& uuid);

    Database* getDb() const {
        return _autoCollForRead->getDb();
    }

    Collection* getCollection() const {
        return _autoCollForRead->getCollection();
    }

protected:
    AutoGetCollectionForReadCommand(OperationContext* opCtx,
                                    const NamespaceString& nss,
                                    AutoGetCollection::ViewMode viewMode);

    AutoGetCollectionForReadCommand(OperationContext* opCtx,
                                    const NamespaceString& nss,
                                    AutoGetCollection::ViewMode viewMode,
                                    Lock::DBLock lock);

    // '_autoCollForRead' may need to be reset by AutoGetCollectionOrViewForReadCommand, so needs to
    // be a boost::optional.
    boost::optional<AutoGetCollectionForRead> _autoCollForRead;

    // This needs to be initialized after 'autoCollForRead', since we need to consult the Database
    // object to get the profiling level. Thus, it needs to be a boost::optional.
    boost::optional<AutoStatsTracker> _statsTracker;
};

/**
 * RAII-style class for obtaining a collection or view for reading. The pointer to a view definition
 * is nullptr if it does not exist.
 *
 * Use this when you are a read-only command, but have not yet determined if the namespace is a view
 * or a collection.
 */
class AutoGetCollectionOrViewForReadCommand final : public AutoGetCollectionForReadCommand {
    MONGO_DISALLOW_COPYING(AutoGetCollectionOrViewForReadCommand);

public:
    AutoGetCollectionOrViewForReadCommand(OperationContext* opCtx, const NamespaceString& nss);
    AutoGetCollectionOrViewForReadCommand(OperationContext* opCtx,
                                          const NamespaceString& nss,
                                          Lock::DBLock lock);

    ViewDefinition* getView() const {
        return _view.get();
    }

    /**
     * Unlock this view or collection and release all resources. After calling this function, it is
     * illegal to access this object's database, collection and view pointers.
     *
     * TODO(SERVER-24909): Consider having the constructor release locks instead, or otherwise
     * remove the need for this method.
     */
    void releaseLocksForView() noexcept;

private:
    std::shared_ptr<ViewDefinition> _view;
};

/**
 * Opens the database that we want to use and sets the appropriate namespace on the
 * current operation.
 */
class OldClientContext {
    MONGO_DISALLOW_COPYING(OldClientContext);

public:
    /** this is probably what you want */
    OldClientContext(OperationContext* opCtx, const std::string& ns, bool doVersion = true);

    /**
     * Below still calls _finishInit, but assumes database has already been acquired
     * or just created.
     */
    OldClientContext(OperationContext* opCtx,
                     const std::string& ns,
                     Database* db,
                     bool justCreated);

    ~OldClientContext();

    Database* db() const {
        return _db;
    }

    /** @return if the db was created by this OldClientContext */
    bool justCreated() const {
        return _justCreated;
    }

private:
    friend class CurOp;
    void _finishInit();
    void _checkNotStale() const;

    bool _justCreated;
    bool _doVersion;
    const std::string _ns;
    Database* _db;
    OperationContext* _opCtx;

    Timer _timer;
};

class OldClientWriteContext {
    MONGO_DISALLOW_COPYING(OldClientWriteContext);

public:
    OldClientWriteContext(OperationContext* opCtx, const std::string& ns);

    Database* db() const {
        return _c.db();
    }

    Collection* getCollection() const {
        return _c.db()->getCollection(_opCtx, _nss);
    }

private:
    OperationContext* const _opCtx;
    const NamespaceString _nss;

    AutoGetOrCreateDb _autodb;
    Lock::CollectionLock _collk;
    OldClientContext _c;
    Collection* _collection;
};

}  // namespace mongo
