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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kDefault

#include <memory>
#include <utility>

#include "mongo/platform/basic.h"

#include "mongo/base/init.h"
#include "mongo/base/string_data.h"
#include "mongo/db/audit.h"
#include "mongo/db/background.h"
#include "mongo/db/catalog/database.h"
#include "mongo/db/catalog/database_catalog_entry.h"
#include "mongo/db/catalog/database_holder_impl.h"
#include "mongo/db/catalog/namespace_uuid_cache.h"
#include "mongo/db/catalog/uuid_catalog.h"
#include "mongo/db/client.h"
#include "mongo/db/clientcursor.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/util/log.h"

namespace mongo {
extern thread_local int16_t localThreadId;

namespace {

const auto dbHolderStorage = ServiceContext::declareDecoration<DatabaseHolder>();

}  // namespace

MONGO_REGISTER_SHIM(DatabaseHolder::getDatabaseHolder)
()->DatabaseHolder& {
    return dbHolderStorage(getGlobalServiceContext());
}

MONGO_REGISTER_SHIM(DatabaseHolder::makeImpl)
(PrivateTo<DatabaseHolder>)->std::unique_ptr<DatabaseHolder::Impl> {
    return std::make_unique<DatabaseHolderImpl>();
}

using std::set;
using std::size_t;
using std::string;
using std::stringstream;

namespace {

StringData _todb(StringData ns) {
    size_t i = ns.find('.');
    if (i == std::string::npos) {
        uassert(13074, "db name can't be empty", ns.size());
        return ns;
    }

    uassert(13075, "db name can't be empty", i > 0);

    const StringData d = ns.substr(0, i);
    uassert(13280,
            "invalid db name: " + ns,
            NamespaceString::validDBName(d, NamespaceString::DollarInDbNameBehavior::Allow));

    return d;
}

}  // namespace

Database* DatabaseHolderImpl::get(OperationContext* opCtx, StringData ns) {
    const StringData db = _todb(ns);
    invariant(opCtx->lockState()->isDbLockedForMode(db, MODE_IS));

    auto id = static_cast<int16_t>(localThreadId + 1);
    // std::scoped_lock<std::mutex> lock{_dbMapMutexVector[id]};
    const auto& dbMap = _dbMapVector[id];
    if (auto iter = dbMap.find(db); iter != dbMap.end()) {
        return iter->second.get();
    }

    // https://www.mongodb.com/docs/manual/core/databases-and-collections/#create-a-database
    // MongoDB does not offer an explicit "createDatabase" command or API. Instead, if a database
    // does not exist, MongoDB automatically creates it when data is first stored in that database.
    // In MongoDB, the existence of a database is typically determined by the "Database *" in C++.
    // However, in Monograph, we verify the database's existence through the storage engine API,
    // which serves as our source of truth.
    bool existInStorageEngine =
        opCtx->getServiceContext()->getStorageEngine()->databaseExists(ns.toStringView());
    if (existInStorageEngine) {
        return openDb(opCtx, ns);
    } else {
        return nullptr;
    }
}

std::set<std::string> DatabaseHolderImpl::_getNamesWithConflictingCasing_inlock(
    StringData name) const {
    std::set<std::string> duplicates;
    auto id = static_cast<int16_t>(localThreadId + 1);

    for (const auto& [dbName, dbPtr] : _dbMapVector[id]) {
        // A name that's equal with case-insensitive match must be identical, or it's a duplicate.
        if (name.equalCaseInsensitive(dbName) && name != dbName) {
            duplicates.insert(dbName);
        }
    }
    return duplicates;
}

std::set<std::string> DatabaseHolderImpl::getNamesWithConflictingCasing(StringData name) {
    return _getNamesWithConflictingCasing_inlock(name);
}

Database* DatabaseHolderImpl::openDb(OperationContext* opCtx, StringData ns, bool* justCreated) {
    MONGO_LOG(1) << "DatabaseHolderImpl::openDb"
                 << ". ns: " << ns;
    const StringData dbName = _todb(ns);
    invariant(opCtx->lockState()->isDbLockedForMode(dbName, MODE_X));

    if (justCreated) {
        *justCreated = false;  // Until proven otherwise.
    }

    auto id = static_cast<int16_t>(localThreadId + 1);
    auto& dbMap = _dbMapVector[id];

    // std::scoped_lock<std::mutex> lock(_dbMapMutexVector[id]);
    // The following will insert a nullptr for dbname, which will treated the same as a non-
    // existant database by the get method, yet still counts in getNamesWithConflictingCasing.
    if (auto iter = dbMap.find(dbName); iter != dbMap.end()) {
        MONGO_LOG(1) << "DatabaseHolderImpl::openDb"
                     << ". ns: " << ns << " exists";
        return iter->second.get();
    }

    // Check casing in lock to avoid transient duplicates.
    auto duplicates = _getNamesWithConflictingCasing_inlock(dbName);
    uassert(ErrorCodes::DatabaseDifferCase,
            str::stream() << "db already exists with different case already have: ["
                          << *duplicates.cbegin() << "] trying to create [" << dbName.toString()
                          << "]",
            duplicates.empty());


    // Do the catalog lookup and database creation outside of the scoped lock, because these may
    // block. Only one thread can be inside this method for the same DB name, because of the
    // requirement for X-lock on the database when we enter. So there is no way we can insert
    // two different databases for the same name.

    // We've inserted a nullptr entry for dbname: make sure to remove it on unsuccessful exit.
    // auto removeDbGuard = MakeGuard([&dbMap, &dbname] {
    //     // if (!lk.owns_lock())
    //     //     lk.lock();
    //     dbMap.erase(dbname);
    // });

    MONGO_LOG(1) << "DatabaseHolderImpl::openDb"
                 << ". ns: " << ns << " create start";

    StorageEngine* storageEngine = getGlobalServiceContext()->getStorageEngine();
    DatabaseCatalogEntry* entry = storageEngine->getDatabaseCatalogEntry(opCtx, dbName);

    // if (!entry->exists()) {
    //     audit::logCreateDatabase(&cc(), dbName);
    // }

    // yield here
    auto newDb = std::make_unique<Database>(opCtx, dbName, entry);

    auto [iter, success] = dbMap.try_emplace(dbName.toString(), std::move(newDb));
    if (!success) {
        MONGO_LOG(1) << "Another coroutine created Database handler on this thread";
        return iter->second.get();
    }

    if (justCreated) {
        *justCreated = true;
    }
    MONGO_LOG(1) << "DatabaseHolderImpl::openDb"
                 << ". ns: " << ns << " done.";
    return iter->second.get();
}

namespace {
void evictDatabaseFromUUIDCatalog(OperationContext* opCtx, Database* db) {
    UUIDCatalog::get(opCtx).onCloseDatabase(opCtx, db);
    for (const auto& [name, coll] : db->collections(opCtx)) {
        NamespaceUUIDCache::get(opCtx).evictNamespace(coll->ns());
    }
}
}  // namespace

void DatabaseHolderImpl::close(OperationContext* opCtx, StringData ns, const std::string& reason) {
    invariant(opCtx->lockState()->isW());

    const StringData dbName = _todb(ns);

    auto id = static_cast<int16_t>(localThreadId + 1);
    {
        // std::scoped_lock<std::mutex> lock{_dbMapMutexVector[id]};
        auto& dbMap = _dbMapVector[id];

        auto it = dbMap.find(dbName);
        if (it != dbMap.end()) {
            auto db = it->second.get();
            repl::oplogCheckCloseDatabase(opCtx, db);
            evictDatabaseFromUUIDCatalog(opCtx, db);

            // only close once
            db->close(opCtx, reason);
            dbMap.erase(it);
        }
    }

    getGlobalServiceContext()
        ->getStorageEngine()
        ->closeDatabase(opCtx, dbName)
        .transitional_ignore();
}

void DatabaseHolderImpl::closeAll(OperationContext* opCtx, const std::string& reason) {
    invariant(opCtx->lockState()->isW());

    auto id = static_cast<int16_t>(localThreadId + 1);

    auto& dbMap = _dbMapVector[id];
    // std::scoped_lock<std::mutex> lock{_dbMapMutexVector[i]};
    for (auto& [dbName, dbPtr] : dbMap) {
        BackgroundOperation::assertNoBgOpInProgForDb(dbName);
        LOG(0) << "DatabaseHolder::closeAll name:" << dbName;
        repl::oplogCheckCloseDatabase(opCtx, dbPtr.get());
        evictDatabaseFromUUIDCatalog(opCtx, dbPtr.get());
        dbPtr->close(opCtx, reason);

        getGlobalServiceContext()
            ->getStorageEngine()
            ->closeDatabase(opCtx, dbName)
            .transitional_ignore();
    }
    dbMap.clear();
}
}  // namespace mongo
