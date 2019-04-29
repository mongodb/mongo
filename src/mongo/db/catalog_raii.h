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
 * obtains a reference to the database. Used as a shortcut for calls to
 * DatabaseHolder::get(opCtx)->get().
 *
 * Use this when you want to do a database-level operation, like read a list of all collections, or
 * drop a collection.
 *
 * It is guaranteed that the lock will be released when this object goes out of scope, therefore
 * the database reference returned by this class should not be retained.
 */
class AutoGetDb {
    AutoGetDb(const AutoGetDb&) = delete;
    AutoGetDb& operator=(const AutoGetDb&) = delete;

public:
    AutoGetDb(OperationContext* opCtx,
              StringData dbName,
              LockMode mode,
              Date_t deadline = Date_t::max());

    /**
     * Returns nullptr if the database didn't exist.
     */
    Database* getDb() const {
        return _db;
    }

private:
    const Lock::DBLock _dbLock;
    Database* const _db;
};

/**
 * RAII-style class, which acquires a locks on the specified database and collection in the
 * requested modes and obtains references to both.
 *
 * NOTE: Throws NamespaceNotFound if the collection UUID cannot be resolved to a name.
 *
 * Any acquired locks may be released when this object goes out of scope, therefore the database
 * and the collection references returned by this class should not be retained.
 */
class AutoGetCollection {
    AutoGetCollection(const AutoGetCollection&) = delete;
    AutoGetCollection& operator=(const AutoGetCollection&) = delete;

public:
    enum ViewMode { kViewsPermitted, kViewsForbidden };

    AutoGetCollection(OperationContext* opCtx,
                      const NamespaceStringOrUUID& nsOrUUID,
                      LockMode modeAll,
                      ViewMode viewMode = kViewsForbidden,
                      Date_t deadline = Date_t::max())
        : AutoGetCollection(opCtx, nsOrUUID, modeAll, modeAll, viewMode, deadline) {}

    AutoGetCollection(OperationContext* opCtx,
                      const NamespaceStringOrUUID& nsOrUUID,
                      LockMode modeDB,
                      LockMode modeColl,
                      ViewMode viewMode = kViewsForbidden,
                      Date_t deadline = Date_t::max());

    /**
     * Without acquiring any locks resolves the given NamespaceStringOrUUID to an actual namespace.
     * Throws NamespaceNotFound if the collection UUID cannot be resolved to a name, or if the UUID
     * can be resolved, but the resulting collection is in the wrong database.
     */
    static NamespaceString resolveNamespaceStringOrUUID(OperationContext* opCtx,
                                                        NamespaceStringOrUUID nsOrUUID);

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

    /**
     * Returns nullptr if the view didn't exist.
     */
    ViewDefinition* getView() const {
        return _view.get();
    }

    /**
     * Returns the resolved namespace of the collection or view.
     */
    const NamespaceString& getNss() const {
        return _resolvedNss;
    }

private:
    AutoGetDb _autoDb;

    // If the object was instantiated with a UUID, contains the resolved namespace, otherwise it is
    // the same as the input namespace string
    NamespaceString _resolvedNss;

    // This field is boost::optional, because in the case of lookup by UUID, the collection lock
    // might need to be relocked for the correct namespace
    boost::optional<Lock::CollectionLock> _collLock;

    Collection* _coll = nullptr;
    std::shared_ptr<ViewDefinition> _view;
};

/**
 * RAII-style class, which acquires a lock on the specified database in the requested mode and
 * obtains a reference to the database, creating it was non-existing. Used as a shortcut for
 * calls to DatabaseHolder::get(opCtx)->openDb(), taking care of locking details. The
 * requested mode must be MODE_IX or MODE_X. If the database needs to be created, the lock will
 * automatically be reacquired as MODE_X.
 *
 * Use this when you are about to perform a write, and want to create the database if it doesn't
 * already exist.
 *
 * It is guaranteed that locks will be released when this object goes out of scope, therefore
 * the database reference returned by this class should not be retained.
 */
class AutoGetOrCreateDb {
    AutoGetOrCreateDb(const AutoGetOrCreateDb&) = delete;
    AutoGetOrCreateDb& operator=(const AutoGetOrCreateDb&) = delete;

public:
    AutoGetOrCreateDb(OperationContext* opCtx,
                      StringData dbName,
                      LockMode mode,
                      Date_t deadline = Date_t::max());

    Database* getDb() const {
        return _db;
    }

    bool justCreated() const {
        return _justCreated;
    }

private:
    boost::optional<AutoGetDb> _autoDb;

    Database* _db;
    bool _justCreated{false};
};

/**
 * RAII-style class. Hides changes to the CollectionCatalog for the life of the object, so that
 * calls to CollectionCatalog::lookupNSSByUUID will return results as before the RAII object was
 * instantiated.
 *
 * The caller must hold the global exclusive lock for the life of the instance.
 */
class ConcealCollectionCatalogChangesBlock {
    ConcealCollectionCatalogChangesBlock(const ConcealCollectionCatalogChangesBlock&) = delete;
    ConcealCollectionCatalogChangesBlock& operator=(const ConcealCollectionCatalogChangesBlock&) =
        delete;

public:
    /**
     * Conceals future CollectionCatalog changes and stashes a pointer to the opCtx for the
     * destructor to use.
     */
    ConcealCollectionCatalogChangesBlock(OperationContext* opCtx);

    /**
     * Reveals CollectionCatalog changes.
     */
    ~ConcealCollectionCatalogChangesBlock();

private:
    // Needed for the destructor to access the CollectionCatalog in order to call onOpenCatalog.
    OperationContext* _opCtx;
};

/**
 * RAII type to set and restore the timestamp read source on the recovery unit.
 *
 * Snapshot is abandoned in constructor and destructor, so it can only be used before
 * the recovery unit becomes active or when the existing snapshot is no longer needed.
 */
class ReadSourceScope {
public:
    ReadSourceScope(OperationContext* opCtx);
    ~ReadSourceScope();

private:
    OperationContext* _opCtx;
    RecoveryUnit::ReadSource _originalReadSource;
    Timestamp _originalReadTimestamp;
};

}  // namespace mongo
