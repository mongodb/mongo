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


#include "mongo/platform/basic.h"

#include "mongo/db/catalog/database_holder_impl.h"

#include "mongo/db/audit.h"
#include "mongo/db/catalog/collection_catalog.h"
#include "mongo/db/catalog/collection_impl.h"
#include "mongo/db/catalog/database_impl.h"
#include "mongo/db/concurrency/exception_util.h"
#include "mongo/db/index_builds_coordinator.h"
#include "mongo/db/op_observer/op_observer.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/db/stats/top.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/logv2/log.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage


namespace mongo {

Database* DatabaseHolderImpl::getDb(OperationContext* opCtx, const DatabaseName& dbName) const {
    uassert(
        13280,
        "invalid db name: " + dbName.db(),
        NamespaceString::validDBName(dbName.db(), NamespaceString::DollarInDbNameBehavior::Allow));

    invariant(opCtx->lockState()->isDbLockedForMode(dbName, MODE_IS) ||
              (dbName.db().compare("local") == 0 && opCtx->lockState()->isLocked()));

    stdx::lock_guard<SimpleMutex> lk(_m);
    DBs::const_iterator it = _dbs.find(dbName);
    if (it != _dbs.end()) {
        return it->second;
    }

    return nullptr;
}

bool DatabaseHolderImpl::dbExists(OperationContext* opCtx, const DatabaseName& dbName) const {
    uassert(
        6198702,
        "invalid db name: " + dbName.db(),
        NamespaceString::validDBName(dbName.db(), NamespaceString::DollarInDbNameBehavior::Allow));
    stdx::lock_guard<SimpleMutex> lk(_m);
    auto it = _dbs.find(dbName);
    return it != _dbs.end() && it->second != nullptr;
}

std::set<DatabaseName> DatabaseHolderImpl::_getNamesWithConflictingCasing_inlock(
    const DatabaseName& dbName) {
    std::set<DatabaseName> duplicates;

    for (const auto& nameAndPointer : _dbs) {
        // A name that's equal with case-insensitive match must be identical, or it's a duplicate.
        if (dbName.equalCaseInsensitive(nameAndPointer.first) && dbName != nameAndPointer.first)
            duplicates.insert(nameAndPointer.first);
    }
    return duplicates;
}

std::set<DatabaseName> DatabaseHolderImpl::getNamesWithConflictingCasing(
    const DatabaseName& dbName) {
    stdx::lock_guard<SimpleMutex> lk(_m);
    return _getNamesWithConflictingCasing_inlock(dbName);
}

std::vector<DatabaseName> DatabaseHolderImpl::getNames() {
    stdx::lock_guard<SimpleMutex> lk(_m);
    std::vector<DatabaseName> dbNames;
    for (const auto& nameAndPointer : _dbs) {
        dbNames.push_back(nameAndPointer.first);
    }
    return dbNames;
}

Database* DatabaseHolderImpl::openDb(OperationContext* opCtx,
                                     const DatabaseName& dbName,
                                     bool* justCreated) {
    uassert(
        6198701,
        "invalid db name: " + dbName.db(),
        NamespaceString::validDBName(dbName.db(), NamespaceString::DollarInDbNameBehavior::Allow));
    invariant(opCtx->lockState()->isDbLockedForMode(dbName, MODE_IX));

    if (justCreated)
        *justCreated = false;  // Until proven otherwise.

    stdx::unique_lock<SimpleMutex> lk(_m);

    // The following will insert a nullptr for dbname, which will treated the same as a non-
    // existant database by the get method, yet still counts in getNamesWithConflictingCasing.
    if (auto db = _dbs[dbName])
        return db;

    // We've inserted a nullptr entry for dbname: make sure to remove it on unsuccessful exit.
    ScopeGuard removeDbGuard([this, &lk, opCtx, dbName] {
        if (!lk.owns_lock())
            lk.lock();
        auto it = _dbs.find(dbName);
        // If someone else hasn't either already removed it or already set it successfully, remove.
        if (it != _dbs.end() && !it->second) {
            _dbs.erase(it);
        }

        // In case anyone else is trying to open the same DB simultaneously and waiting on our
        // result, we should notify them we failed and let them try in our place.
        _c.notify_all();
    });

    // Check casing in lock to avoid transient duplicates.
    auto duplicates = _getNamesWithConflictingCasing_inlock(dbName);
    uassert(ErrorCodes::DatabaseDifferCase,
            str::stream() << "db already exists with different case already have: ["
                          << (*duplicates.cbegin()) << "] trying to create [" << dbName.toString()
                          << "]",
            duplicates.empty());

    // Do the catalog lookup and database creation outside of the scoped lock, because these may
    // block.
    lk.unlock();

    if (CollectionCatalog::get(opCtx)->getAllCollectionUUIDsFromDb(dbName).empty()) {
        audit::logCreateDatabase(opCtx->getClient(), dbName.toString());
        if (justCreated)
            *justCreated = true;
    }

    std::unique_ptr<DatabaseImpl> newDb = std::make_unique<DatabaseImpl>(dbName);
    Status status = newDb->init(opCtx);
    while (!status.isOK()) {
        // If we get here, then initializing the database failed because another concurrent writer
        // already registered their own Database instance with the ViewCatalog. We need to wait for
        // them to finish.
        lk.lock();

        auto it = _dbs.find(dbName);
        if (it != _dbs.end() && it->second) {
            // Creating databases only requires a DB lock in MODE_IX. Thus databases can be created
            // concurrently. If this thread "lost the race", return the database object that was
            // persisted in the `_dbs` map.
            removeDbGuard.dismiss();
            return it->second;
        }

        // Consider using OperationContext::waitForConditionOrInterrupt if the logic here changes
        // in such a way that we can easily express it as a predicate for that function.
        _c.wait_for(lk, stdx::chrono::milliseconds(1));

        it = _dbs.find(dbName);
        if (it != _dbs.end() && it->second) {
            // As above, another writer finished successfully, return the persisted object.
            removeDbGuard.dismiss();
            return it->second;
        }

        lk.unlock();

        // Before we continue make sure we haven't been killed
        opCtx->checkForInterrupt();

        // At this point it's possible that the other writer just hasn't finished yet, or that they
        // failed. In either case, we should check and see if we can initialize the database now.
        status = newDb->init(opCtx);
    }

    // Finally replace our nullptr entry with the new Database pointer.
    removeDbGuard.dismiss();
    lk.lock();

    invariant(!_dbs[dbName]);
    auto* db = newDb.release();
    _dbs[dbName] = db;
    invariant(_getNamesWithConflictingCasing_inlock(dbName).empty());
    _c.notify_all();

    return db;
}

void DatabaseHolderImpl::dropDb(OperationContext* opCtx, Database* db) {
    invariant(db);

    // Store the name so we have if for after the db object is deleted
    auto name = db->name();

    LOGV2_DEBUG(20310, 1, "dropDatabase {name}", "name"_attr = name);

    invariant(opCtx->lockState()->isDbLockedForMode(name, MODE_X));

    auto catalog = CollectionCatalog::get(opCtx);
    for (auto collIt = catalog->begin(opCtx, name); collIt != catalog->end(opCtx); ++collIt) {
        auto coll = *collIt;
        if (!coll) {
            break;
        }

        // It is the caller's responsibility to ensure that no index builds are active in the
        // database.
        invariant(!coll->getIndexCatalog()->haveAnyIndexesInProgress(),
                  str::stream() << "An index is building on collection '" << coll->ns() << "'.");
    }

    audit::logDropDatabase(opCtx->getClient(), name.toString());

    auto const serviceContext = opCtx->getServiceContext();

    for (auto collIt = catalog->begin(opCtx, name); collIt != catalog->end(opCtx); ++collIt) {
        auto coll = *collIt;
        if (!coll) {
            break;
        }

        // The in-memory ViewCatalog gets cleared when opObserver::onDropCollection() is called for
        // the system.views collection. Since it is a replicated collection, this call occurs in
        // dropCollectionEvenIfSystem(). For standalones, `system.views` and the ViewCatalog are
        // dropped/cleared here.
        auto replCoord = repl::ReplicationCoordinator::get(opCtx);
        if (!replCoord->isReplEnabled() && coll->ns().isSystemDotViews()) {
            opCtx->getServiceContext()->getOpObserver()->onDropCollection(
                opCtx,
                coll->ns(),
                coll->uuid(),
                coll->numRecords(opCtx),
                OpObserver::CollectionDropType::kOnePhase);
        }

        Top::get(serviceContext).collectionDropped(coll->ns());
    }

    // Clean up the in-memory database state.
    CollectionCatalog::write(
        opCtx, [&](CollectionCatalog& catalog) { catalog.clearDatabaseProfileSettings(name); });
    close(opCtx, name);

    auto const storageEngine = serviceContext->getStorageEngine();
    writeConflictRetry(opCtx, "dropDatabase", name.toString(), [&] {
        storageEngine->dropDatabase(opCtx, name).transitional_ignore();
    });
}

void DatabaseHolderImpl::close(OperationContext* opCtx, const DatabaseName& dbName) {
    uassert(
        6198700,
        "invalid db name: " + dbName.db(),
        NamespaceString::validDBName(dbName.db(), NamespaceString::DollarInDbNameBehavior::Allow));
    invariant(opCtx->lockState()->isDbLockedForMode(dbName, MODE_X));

    stdx::lock_guard<SimpleMutex> lk(_m);

    DBs::const_iterator it = _dbs.find(dbName);
    if (it == _dbs.end()) {
        return;
    }
    auto db = it->second;

    LOGV2_DEBUG(20311, 2, "DatabaseHolder::close", "db"_attr = dbName);

    CollectionCatalog::write(
        opCtx, [&](CollectionCatalog& catalog) { catalog.onCloseDatabase(opCtx, dbName); });

    delete db;
    db = nullptr;

    _dbs.erase(it);

    auto* const storageEngine = opCtx->getServiceContext()->getStorageEngine();
    storageEngine->closeDatabase(opCtx, dbName).transitional_ignore();
}

void DatabaseHolderImpl::closeAll(OperationContext* opCtx) {
    invariant(opCtx->lockState()->isW());

    while (true) {
        std::vector<DatabaseName> dbs;
        {
            stdx::lock_guard<SimpleMutex> lk(_m);
            for (DBs::const_iterator i = _dbs.begin(); i != _dbs.end(); ++i) {
                // It is the caller's responsibility to ensure that no index builds are active in
                // the database.
                IndexBuildsCoordinator::get(opCtx)->assertNoBgOpInProgForDb(i->first.toString());
                dbs.push_back(i->first);
            }
        }

        if (dbs.empty()) {
            break;
        }

        for (const auto& name : dbs) {
            close(opCtx, name);
        }
    }
}

void DatabaseHolderImpl::setDbInfo(OperationContext* opCtx,
                                   const DatabaseName& dbName,
                                   const DatabaseType& dbInfo) {
    uassert(
        6420900,
        "Invalid database name: " + dbName.db(),
        NamespaceString::validDBName(dbName.db(), NamespaceString::DollarInDbNameBehavior::Allow));
    invariant(opCtx->lockState()->isDbLockedForMode(dbName, MODE_X));

    stdx::lock_guard<SimpleMutex> lk(_m);

    const auto it = _dbs.find(dbName);
    if (it == _dbs.end() || !it->second) {
        return;
    }

    LOGV2(6420901,
          "Setting this node's cached database info",
          "db"_attr = dbName.db(),
          "version"_attr = dbInfo.getVersion());

    auto db = static_cast<DatabaseImpl*>(it->second);
    db->_info.emplace(dbInfo);
}

void DatabaseHolderImpl::clearDbInfo(OperationContext* opCtx, const DatabaseName& dbName) {
    uassert(
        6420902,
        "Invalid database name: " + dbName.db(),
        NamespaceString::validDBName(dbName.db(), NamespaceString::DollarInDbNameBehavior::Allow));
    invariant(opCtx->lockState()->isDbLockedForMode(dbName, MODE_IX));

    stdx::lock_guard<SimpleMutex> lk(_m);

    const auto it = _dbs.find(dbName);
    if (it == _dbs.end() || !it->second) {
        return;
    }

    LOGV2(6420903, "Clearing this node's cached database info", "db"_attr = dbName.db());

    auto db = static_cast<DatabaseImpl*>(it->second);
    db->_info = boost::none;
}

boost::optional<DatabaseVersion> DatabaseHolderImpl::getDbVersion(
    OperationContext* opCtx, const DatabaseName& dbName) const {
    uassert(
        6420904,
        "Invalid database name: " + dbName.db(),
        NamespaceString::validDBName(dbName.db(), NamespaceString::DollarInDbNameBehavior::Allow));
    // The database should be required to be locked in IS mode, however this function is also called
    // by the `AutoGet*ForReadLockFree` constructor, which only holds the global lock in IS mode.

    stdx::lock_guard<SimpleMutex> lk(_m);

    const auto it = _dbs.find(dbName);
    if (it == _dbs.end() || !it->second) {
        return boost::none;
    }

    auto db = static_cast<DatabaseImpl*>(it->second);
    return db->_info ? boost::optional<DatabaseVersion>(db->_info->getVersion()) : boost::none;
}

boost::optional<ShardId> DatabaseHolderImpl::getDbPrimary(OperationContext* opCtx,
                                                          const DatabaseName& dbName) const {
    uassert(
        6420905,
        "Invalid database name: " + dbName.db(),
        NamespaceString::validDBName(dbName.db(), NamespaceString::DollarInDbNameBehavior::Allow));
    invariant(opCtx->lockState()->isDbLockedForMode(dbName, MODE_IS));

    stdx::lock_guard<SimpleMutex> lk(_m);

    const auto it = _dbs.find(dbName);
    if (it == _dbs.end() || !it->second) {
        return boost::none;
    }

    auto db = static_cast<DatabaseImpl*>(it->second);
    return db->_info ? boost::optional<ShardId>(db->_info->getPrimary()) : boost::none;
}

}  // namespace mongo
