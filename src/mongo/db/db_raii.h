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
#include "mongo/db/views/view.h"
#include "mongo/util/timer.h"

namespace mongo {

class Collection;

/**
 * RAII-style class, which acquires a lock on the specified database in the requested mode and
 * obtains a reference to the database. Used as a shortcut for calls to dbHolder().get().
 *
 * It is guaranteed that the lock will be released when this object goes out of scope, therefore
 * the database reference returned by this class should not be retained.
 */
class AutoGetDb {
    MONGO_DISALLOW_COPYING(AutoGetDb);

public:
    AutoGetDb(OperationContext* txn, StringData ns, LockMode mode);

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
 * It is guaranteed that locks will be released when this object goes out of scope, therefore
 * the database and the collection references returned by this class should not be retained.
 */
class AutoGetCollection {
    MONGO_DISALLOW_COPYING(AutoGetCollection);

    enum class ViewMode;

public:
    AutoGetCollection(OperationContext* txn, const NamespaceString& nss, LockMode modeAll)
        : AutoGetCollection(txn, nss, modeAll, modeAll, ViewMode::kViewsForbidden) {}

    AutoGetCollection(OperationContext* txn,
                      const NamespaceString& nss,
                      LockMode modeDB,
                      LockMode modeColl)
        : AutoGetCollection(txn, nss, modeDB, modeColl, ViewMode::kViewsForbidden) {}

    /**
     * This constructor is inteded for internal use and should not be used outside this file.
     * AutoGetCollectionForRead and AutoGetCollectionOrViewForRead use ViewMode to determine whether
     * or not it is permissible to obtain a handle on a view namespace. Use another constructor or
     * another AutoGet class instead.
     */
    AutoGetCollection(OperationContext* txn,
                      const NamespaceString& nss,
                      LockMode modeDB,
                      LockMode modeColl,
                      ViewMode viewMode);

    Database* getDb() const {
        return _autoDb.getDb();
    }

    Collection* getCollection() const {
        return _coll;
    }

private:
    enum class ViewMode { kViewsPermitted, kViewsForbidden };

    const ViewMode _viewMode;
    const AutoGetDb _autoDb;
    const Lock::CollectionLock _collLock;
    Collection* const _coll;

    friend class AutoGetCollectionForRead;
    friend class AutoGetCollectionOrViewForRead;
};

/**
 * RAII-style class, which acquires a lock on the specified database in the requested mode and
 * obtains a reference to the database, creating it was non-existing. Used as a shortcut for
 * calls to dbHolder().openDb(), taking care of locking details. The requested mode must be
 * MODE_IX or MODE_X. If the database needs to be created, the lock will automatically be
 * reacquired as MODE_X.
 *
 * It is guaranteed that locks will be released when this object goes out of scope, therefore
 * the database reference returned by this class should not be retained.
 */
class AutoGetOrCreateDb {
    MONGO_DISALLOW_COPYING(AutoGetOrCreateDb);

public:
    AutoGetOrCreateDb(OperationContext* txn, StringData ns, LockMode mode);

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
    ScopedTransaction _transaction;
    Lock::DBLock _dbLock;  // not const, as we may need to relock for implicit create
    Database* _db;
    bool _justCreated;
};

/**
 * RAII-style class, which would acquire the appropritate hierarchy of locks for obtaining
 * a particular collection and would retrieve a reference to the collection. In addition, this
 * utility validates the shard version for the specified namespace and sets the current operation's
 * namespace for the duration while this object is alive.
 *
 * It is guaranteed that locks will be released when this object goes out of scope, therefore
 * database and collection references returned by this class should not be retained.
 */
class AutoGetCollectionForRead {
    MONGO_DISALLOW_COPYING(AutoGetCollectionForRead);

public:
    AutoGetCollectionForRead(OperationContext* txn, const std::string& ns)
        : AutoGetCollectionForRead(
              txn, NamespaceString(ns), AutoGetCollection::ViewMode::kViewsForbidden) {}

    AutoGetCollectionForRead(OperationContext* txn, const NamespaceString& nss)
        : AutoGetCollectionForRead(txn, nss, AutoGetCollection::ViewMode::kViewsForbidden) {}

    ~AutoGetCollectionForRead();

    Database* getDb() const {
        return _autoColl->getDb();
    }

    Collection* getCollection() const {
        return _autoColl->getCollection();
    }

private:
    void _ensureMajorityCommittedSnapshotIsValid(const NamespaceString& nss);

    const Timer _timer;
    OperationContext* const _txn;
    const ScopedTransaction _transaction;

protected:
    AutoGetCollectionForRead(OperationContext* txn,
                             const NamespaceString& nss,
                             AutoGetCollection::ViewMode viewMode);

    /**
     * This protected section must come after the private section because
     * AutoGetCollectionOrViewForRead needs access to _autoColl, but _autoColl must be initialized
     * after _transaction.
     */
    boost::optional<AutoGetCollection> _autoColl;
};

/**
 * RAII-style class for obtaining a collection or view for reading. The pointer to a view definition
 * is nullptr if it does not exist.
 */
class AutoGetCollectionOrViewForRead final : public AutoGetCollectionForRead {
    MONGO_DISALLOW_COPYING(AutoGetCollectionOrViewForRead);

public:
    AutoGetCollectionOrViewForRead(OperationContext* txn, const std::string& ns)
        : AutoGetCollectionOrViewForRead(txn, NamespaceString(ns)) {}

    AutoGetCollectionOrViewForRead(OperationContext* txn, const NamespaceString& nss);

    ViewDefinition* getView() const {
        return _view;
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
    ViewDefinition* _view;
};

/**
 * Opens the database that we want to use and sets the appropriate namespace on the
 * current operation.
 */
class OldClientContext {
    MONGO_DISALLOW_COPYING(OldClientContext);

public:
    /** this is probably what you want */
    OldClientContext(OperationContext* txn, const std::string& ns, bool doVersion = true);

    /**
     * Below still calls _finishInit, but assumes database has already been acquired
     * or just created.
     */
    OldClientContext(OperationContext* txn, const std::string& ns, Database* db, bool justCreated);

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
    OperationContext* _txn;

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
        return _c.db()->getCollection(_nss.ns());
    }

private:
    OperationContext* const _txn;
    const NamespaceString _nss;

    AutoGetOrCreateDb _autodb;
    Lock::CollectionLock _collk;
    OldClientContext _c;
    Collection* _collection;
};

}  // namespace mongo
