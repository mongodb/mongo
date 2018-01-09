/**
 *    Copyright (C) 2017 MongoDB Inc.
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

#include "mongo/base/string_data.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/database.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/views/view.h"

namespace mongo {

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
    AutoGetDb(OperationContext* opCtx, StringData ns, Lock::DBLock lock);

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

public:
    enum class ViewMode { kViewsPermitted, kViewsForbidden };

    AutoGetCollection(OperationContext* opCtx, const NamespaceString& nss, LockMode modeAll)
        : AutoGetCollection(opCtx, nss, modeAll, modeAll, ViewMode::kViewsForbidden) {}

    AutoGetCollection(OperationContext* opCtx,
                      const NamespaceString& nss,
                      LockMode modeDB,
                      LockMode modeColl)
        : AutoGetCollection(opCtx, nss, modeDB, modeColl, ViewMode::kViewsForbidden) {}

    AutoGetCollection(OperationContext* opCtx,
                      const NamespaceString& nss,
                      const UUID& uuid,
                      LockMode modeAll);

    AutoGetCollection(OperationContext* opCtx,
                      const NamespaceString& nss,
                      LockMode modeColl,
                      ViewMode viewMode,
                      Lock::DBLock lock);

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
    const ViewMode _viewMode;
    const AutoGetDb _autoDb;
    const Lock::CollectionLock _collLock;

    Collection* const _coll;
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

}  // namespace mongo
