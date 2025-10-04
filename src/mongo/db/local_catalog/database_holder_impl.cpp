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

#include "mongo/db/local_catalog/database_holder_impl.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/audit.h"
#include "mongo/db/index_builds/index_builds_coordinator.h"
#include "mongo/db/local_catalog/collection.h"
#include "mongo/db/local_catalog/collection_catalog.h"
#include "mongo/db/local_catalog/collection_catalog_helper.h"
#include "mongo/db/local_catalog/database_impl.h"
#include "mongo/db/local_catalog/index_catalog.h"
#include "mongo/db/local_catalog/lock_manager/exception_util.h"
#include "mongo/db/local_catalog/lock_manager/lock_manager_defs.h"
#include "mongo/db/local_catalog/shard_role_api/transaction_resources.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/op_observer/op_observer.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/profile_settings.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/service_context.h"
#include "mongo/db/stats/top.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/logv2/log.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/str.h"

#include <memory>
#include <mutex>
#include <string>
#include <utility>

#include <absl/container/node_hash_map.h>
#include <absl/meta/type_traits.h>
#include <boost/algorithm/string.hpp>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage

namespace mongo {

Database* DatabaseHolderImpl::getDb(OperationContext* opCtx, const DatabaseName& dbName) const {
    uassert(13280,
            "invalid db name: " + dbName.toStringForErrorMsg(),
            DatabaseName::isValid(dbName, DatabaseName::DollarInDbNameBehavior::Allow));

    invariant(shard_role_details::getLocker(opCtx)->isDbLockedForMode(dbName, MODE_IS) ||
              (dbName.isLocalDB() && shard_role_details::getLocker(opCtx)->isLocked()));

    stdx::lock_guard<stdx::mutex> lk(_m);

    auto it = _dbs.viewAll().find(dbName);
    if (it != _dbs.viewAll().end()) {
        return it->second.get();
    }

    return nullptr;
}

bool DatabaseHolderImpl::dbExists(OperationContext* opCtx, const DatabaseName& dbName) const {
    uassert(6198702,
            "invalid db name: " + dbName.toStringForErrorMsg(),
            DatabaseName::isValid(dbName, DatabaseName::DollarInDbNameBehavior::Allow));
    stdx::lock_guard<stdx::mutex> lk(_m);

    auto it = _dbs.viewAll().find(dbName);
    return it != _dbs.viewAll().end() && it->second != nullptr;
}

boost::optional<DatabaseName> DatabaseHolderImpl::_getNameWithConflictingCasing_inlock(
    const DatabaseName& dbName) {

    return _dbs.getAnyConflictingName(dbName);
}

boost::optional<DatabaseName> DatabaseHolderImpl::getNameWithConflictingCasing(
    const DatabaseName& dbName) {
    stdx::lock_guard<stdx::mutex> lk(_m);
    return _getNameWithConflictingCasing_inlock(dbName);
}

std::vector<DatabaseName> DatabaseHolderImpl::getNames() {
    stdx::lock_guard<stdx::mutex> lk(_m);
    std::vector<DatabaseName> dbNames;
    for (const auto& nameAndPointer : _dbs.viewAll()) {
        dbNames.push_back(nameAndPointer.first);
    }
    return dbNames;
}

Database* DatabaseHolderImpl::openDb(OperationContext* opCtx,
                                     const DatabaseName& dbName,
                                     bool* justCreated) {
    uassert(6198701,
            "invalid db name: " + dbName.toStringForErrorMsg(),
            DatabaseName::isValid(dbName, DatabaseName::DollarInDbNameBehavior::Allow));
    invariant(shard_role_details::getLocker(opCtx)->isDbLockedForMode(dbName, MODE_IX));

    if (justCreated)
        *justCreated = false;  // Until proven otherwise.

    stdx::unique_lock<stdx::mutex> lk(_m);

    // The following will insert a nullptr for dbname, which will treated the same as a non-
    // existant database by the get method, yet still counts in getNameWithConflictingCasing.
    if (auto db = _dbs.getOrCreate(dbName))
        return db;

    // We've inserted a nullptr entry for dbname: make sure to remove it on unsuccessful exit.
    ScopeGuard removeDbGuard([this, &lk, opCtx, dbName] {
        if (!lk.owns_lock())
            lk.lock();

        auto it = _dbs.viewAll().find(dbName);
        // If someone else hasn't either already removed it or already set it successfully, remove.
        if (it != _dbs.viewAll().end() && !it->second) {
            _dbs.erase(dbName);
        }
    });

    // Check casing in lock to avoid transient duplicates.
    auto duplicate = _getNameWithConflictingCasing_inlock(dbName);
    uassert(ErrorCodes::DatabaseDifferCase,
            str::stream() << "db already exists with different case already have: ["
                          << duplicate->toStringForErrorMsg() << "] trying to create ["
                          << dbName.toStringForErrorMsg() << "]",
            !duplicate);

    // Do the catalog lookup and database creation outside of the scoped lock, because these may
    // block.
    lk.unlock();

    if (CollectionCatalog::get(opCtx)->getAllCollectionUUIDsFromDb(dbName).empty()) {
        audit::logCreateDatabase(opCtx->getClient(), dbName);
        if (justCreated)
            *justCreated = true;
    }

    std::unique_ptr<DatabaseImpl> newDb = std::make_unique<DatabaseImpl>(dbName);
    newDb->init(opCtx);

    // Finally replace our nullptr entry with the new Database pointer.
    removeDbGuard.dismiss();
    lk.lock();

    auto it = _dbs.viewAll().find(dbName);
    invariant(it != _dbs.viewAll().end());
    if (it->second) {
        // Creating databases only requires a DB lock in MODE_IX, thus databases can be concurrently
        // created. If this thread lost the race, return the database object that was already
        // created.
        return it->second.get();
    }

    auto p = _dbs.upsert(dbName, std::move(newDb));
    return p.first;
}

void DatabaseHolderImpl::dropDb(OperationContext* opCtx, Database* db) {
    invariant(db);
    invariant(shard_role_details::getLocker(opCtx)->inAWriteUnitOfWork());

    // Store the name so we have if for after the db object is deleted
    auto name = db->name();

    LOGV2_DEBUG(20310, 1, "dropDatabase {name}", "name"_attr = name);

    invariant(shard_role_details::getLocker(opCtx)->isDbLockedForMode(name, MODE_X));

    auto catalog = CollectionCatalog::get(opCtx);
    for (auto&& coll : catalog->range(name)) {
        if (!coll) {
            break;
        }

        // It is the caller's responsibility to ensure that no index builds are active in the
        // database.
        invariant(!coll->getIndexCatalog()->haveAnyIndexesInProgress(),
                  str::stream() << "An index is building on collection '"
                                << coll->ns().toStringForErrorMsg() << "'.");
    }

    audit::logDropDatabase(opCtx->getClient(), name);

    for (auto&& coll : catalog->range(name)) {
        if (!coll) {
            break;
        }

        // The in-memory ViewCatalog gets cleared when opObserver::onDropCollection() is called for
        // the system.views collection. Since it is a replicated collection, this call occurs in
        // dropCollectionEvenIfSystem(). For standalones, `system.views` and the ViewCatalog are
        // dropped/cleared here.
        auto replCoord = repl::ReplicationCoordinator::get(opCtx);
        if (!replCoord->getSettings().isReplSet() && coll->ns().isSystemDotViews()) {
            opCtx->getServiceContext()->getOpObserver()->onDropCollection(
                opCtx,
                coll->ns(),
                coll->uuid(),
                coll->numRecords(opCtx),
                /*markFromMigrate=*/false);
        }

        Top::getDecoration(opCtx).collectionDropped(coll->ns());
    }

    // close() is called as part of the onCommit handler as it frees the memory pointed to by 'db'.
    // We need to keep this memory valid until the transaction successfully commits.
    shard_role_details::getRecoveryUnit(opCtx)->onCommit([this,
                                                          name = name](OperationContext* opCtx,
                                                                       boost::optional<Timestamp>) {
        close(opCtx, name);
        DatabaseProfileSettings::get(opCtx->getServiceContext()).clearDatabaseProfileSettings(name);
    });

    writeConflictRetry(opCtx, "dropDatabase", NamespaceString(name), [&] {
        catalog::dropDatabase(opCtx, name).transitional_ignore();
    });
}

void DatabaseHolderImpl::close(OperationContext* opCtx, const DatabaseName& dbName) {
    uassert(6198700,
            "invalid db name: " + dbName.toStringForErrorMsg(),
            DatabaseName::isValid(dbName, DatabaseName::DollarInDbNameBehavior::Allow));
    invariant(shard_role_details::getLocker(opCtx)->isDbLockedForMode(dbName, MODE_X));

    stdx::lock_guard<stdx::mutex> lk(_m);

    if (!_dbs.viewAll().contains(dbName)) {
        return;
    }

    LOGV2_DEBUG(20311, 2, "DatabaseHolder::close", logAttrs(dbName));

    CollectionCatalog::write(
        opCtx, [&](CollectionCatalog& catalog) { catalog.onCloseDatabase(opCtx, dbName); });

    _dbs.erase(dbName);
}

void DatabaseHolderImpl::closeAll(OperationContext* opCtx) {
    invariant(shard_role_details::getLocker(opCtx)->isW());

    while (true) {
        std::vector<DatabaseName> dbs;
        {
            stdx::lock_guard<stdx::mutex> lk(_m);
            for (auto i = _dbs.viewAll().begin(); i != _dbs.viewAll().end(); ++i) {
                // It is the caller's responsibility to ensure that no index builds are active in
                // the database.
                IndexBuildsCoordinator::get(opCtx)->assertNoBgOpInProgForDb(i->first);
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

DatabaseHolderImpl::DBsIndex::NormalizedDatabaseName DatabaseHolderImpl::DBsIndex::normalize(
    const DatabaseName& dbName) {
    std::string str = dbName.toStringForResourceId();
    boost::algorithm::to_lower(str);
    return str;
}

const DatabaseHolderImpl::DBsIndex::DBs& DatabaseHolderImpl::DBsIndex::viewAll() const {
    return _dbs;
}

// Return the Database already associated to a name. If there was no association, the class
// associates a null pointer to the Databasename in both maps
Database* DatabaseHolderImpl::DBsIndex::getOrCreate(const DatabaseName& dbName) {
    Database* result;
    auto it = _dbs.find(dbName);
    if (it != _dbs.end()) {
        // Existing entry. So, it was already registered in _normalizedDBs
        result = it->second.get();
    } else {
        // New entry. Update both collections
        auto insertIt = _dbs.insert(it, {dbName, nullptr});
        result = insertIt->second.get();
        _normalizedDBs.insert({normalize(dbName), dbName});
    }
    return result;
}

// Get the ownership of a Database with a given name. Update the associations for both maps
std::pair<Database*, bool> DatabaseHolderImpl::DBsIndex::upsert(const DatabaseName& dbName,
                                                                std::unique_ptr<Database> db) {
    auto [dbsIt, isNew] = _dbs.insert_or_assign(dbName, std::move(db));
    if (isNew) {  // New database name
        _normalizedDBs.insert({normalize(dbName), dbName});
    }
    return {dbsIt->second.get(), isNew};
}

void DatabaseHolderImpl::DBsIndex::erase(const DatabaseName& dbName) {
    NormalizedDatabaseName normalizedName = normalize(dbName);
    auto [begin, end] = _normalizedDBs.equal_range(normalizedName);
    for (auto dbsIt = begin; dbsIt != end; ++dbsIt) {
        if (dbsIt->second == dbName) {
            _normalizedDBs.erase(dbsIt);
            break;
        }
    }
    _dbs.erase(dbName);
}

// Check if there is any opened database with a name with the same name with a case
// insensitive search
boost::optional<DatabaseName> DatabaseHolderImpl::DBsIndex::getAnyConflictingName(
    const DatabaseName& dbName) const {
    NormalizedDatabaseName normalizedName = normalize(dbName);
    auto [begin, end] = _normalizedDBs.equal_range(normalizedName);
    for (auto dbsIt = begin; dbsIt != end; ++dbsIt) {
        if (dbName.equalCaseInsensitive(dbsIt->second) && dbName != dbsIt->second) {
            return dbsIt->second;
        }
    }
    return boost::none;
}

}  // namespace mongo
