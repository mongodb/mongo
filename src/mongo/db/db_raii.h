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

#include "mongo/base/string_data.h"
#include "mongo/db/catalog/database.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/stats/top.h"
#include "mongo/db/views/view.h"
#include "mongo/util/timer.h"

namespace mongo {

class Collection;

/**
 * RAII-style class, which acquires a lock on the specified database in the requested mode and
 * obtains a reference to the database. Used as a shortcut for calls to dbHolder().get().
 *
 * Use this when you want to do a database-level operation, like read a list of all collections, or
 * drop a collection.
 *
 * It is guaranteed that the lock will be released when this object goes out of scope, therefore
 * the database reference returned by this class should not be retained.
 */
class AutoGetDb {
    MONGO_DISALLOW_COPYING(AutoGetDb);

public:
    AutoGetDb(OperationContext* opCtx, StringData ns, LockMode mode);

    Database* getDb() const {
        return _db;
    }

private:
    const Lock::DBLock _dbLock;
    Database* const _db;
};

/**
 * RAII-style class, which acquires a locks on the specified database and collection in the
 * requested mode and obtains references to both.
 *
 * Use this when you want to access something at the collection level, but do not want to do any of
 * the tasks associated with the 'ForRead' variants below. For example, you can use this to access a
 * Collection's CursorManager, or to remove a document.
 *
 * It is guaranteed that locks will be released when this object goes out of scope, therefore
 * the database and the collection references returned by this class should not be retained.
 */
class AutoGetCollection {
    MONGO_DISALLOW_COPYING(AutoGetCollection);

    enum class ViewMode;

public:
    AutoGetCollection(OperationContext* opCtx, const NamespaceString& nss, LockMode modeAll)
        : AutoGetCollection(opCtx, nss, modeAll, modeAll, ViewMode::kViewsForbidden) {}

    AutoGetCollection(OperationContext* opCtx,
                      const NamespaceString& nss,
                      LockMode modeDB,
                      LockMode modeColl)
        : AutoGetCollection(opCtx, nss, modeDB, modeColl, ViewMode::kViewsForbidden) {}

    /**
     * This constructor is intended for internal use and should not be used outside this file.
     * AutoGetCollectionForReadCommand and AutoGetCollectionOrViewForReadCommand use 'viewMode' to
     * determine whether or not it is permissible to obtain a handle on a view namespace. Use
     * another constructor or another 'AutoGet' class instead.
     */
    AutoGetCollection(OperationContext* opCtx,
                      const NamespaceString& nss,
                      LockMode modeDB,
                      LockMode modeColl,
                      ViewMode viewMode);

    /**
     * Returns nullptr if the database didn't exist.
     */
    Database* getDb() const {
        return _autoDb.getDb();
    }

    /**
     * Returns nullptr if the collection didn't exist.
     */
    Collection* getCollection() const {
        return _coll;
    }

private:
    enum class ViewMode { kViewsPermitted, kViewsForbidden };

    const ViewMode _viewMode;
    const AutoGetDb _autoDb;
    const Lock::CollectionLock _collLock;
    Collection* const _coll;

    friend class AutoGetCollectionOrView;
    friend class AutoGetCollectionForRead;
    friend class AutoGetCollectionForReadCommand;
    friend class AutoGetCollectionOrViewForReadCommand;
};

/**
 * RAII-style class which acquires the appropriate hierarchy of locks for a collection or
 * view. The pointer to a view definition is nullptr if it does not exist.
 *
 * Use this when you have not yet determined if the namespace is a view or a collection.
 * For example, you can use this to access a namespace's CursorManager.
 *
 * It is guaranteed that locks will be released when this object goes out of scope, therefore
 * the view returned by this class should not be retained.
 */
class AutoGetCollectionOrView {
    MONGO_DISALLOW_COPYING(AutoGetCollectionOrView);

public:
    AutoGetCollectionOrView(OperationContext* opCtx, const NamespaceString& nss, LockMode modeAll);

    /**
     * Returns nullptr if the database didn't exist.
     */
    Database* getDb() const {
        return _autoColl.getDb();
    }

    /**
     * Returns nullptr if the collection didn't exist.
     */
    Collection* getCollection() const {
        return _autoColl.getCollection();
    }

    /**
     * Returns nullptr if the view didn't exist.
     */
    ViewDefinition* getView() const {
        return _view.get();
    }

private:
    const AutoGetCollection _autoColl;
    std::shared_ptr<ViewDefinition> _view;
};

/**
 * RAII-style class, which acquires a lock on the specified database in the requested mode and
 * obtains a reference to the database, creating it was non-existing. Used as a shortcut for
 * calls to dbHolder().openDb(), taking care of locking details. The requested mode must be
 * MODE_IX or MODE_X. If the database needs to be created, the lock will automatically be
 * reacquired as MODE_X.
 *
 * Use this when you are about to perform a write, and want to create the database if it doesn't
 * already exist.
 *
 * It is guaranteed that locks will be released when this object goes out of scope, therefore
 * the database reference returned by this class should not be retained.
 */
class AutoGetOrCreateDb {
    MONGO_DISALLOW_COPYING(AutoGetOrCreateDb);

public:
    AutoGetOrCreateDb(OperationContext* opCtx, StringData ns, LockMode mode);

    Database* getDb() const {
        return _db;
    }

    bool justCreated() const {
        return _justCreated;
    }

    Lock::DBLock& lock() {
        return _dbLock;
    }

private:
    Lock::DBLock _dbLock;  // not const, as we may need to relock for implicit create
    Database* _db;
    bool _justCreated;
};

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

    /**
     * This constructor is intended for internal use and should not be used outside this file.
     * AutoGetCollectionForReadCommand and AutoGetCollectionOrViewForReadCommand use 'viewMode' to
     * determine whether or not it is permissible to obtain a handle on a view namespace. Use
     * another constructor or another 'AutoGet' class instead.
     */
    AutoGetCollectionForRead(OperationContext* opCtx,
                             const NamespaceString& nss,
                             AutoGetCollection::ViewMode viewMode);

    Database* getDb() const {
        return _autoColl->getDb();
    }

    Collection* getCollection() const {
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
