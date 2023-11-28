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

#include <cstdint>
#include <shared_mutex>
#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kStorage

#include <atomic>
#include <cstddef>
#include <memory>
#include <mutex>
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

Database* DatabaseHolderImpl::get(OperationContext* opCtx, StringData ns) const {
    const StringData db = _todb(ns);
    invariant(opCtx->lockState()->isDbLockedForMode(db, MODE_IS));

    int16_t id = localThreadId + 1;
    // auto& mutex = (localThreadId == -1) ? _globalDBMapMutex :
    // _localDBMapMutexVector[localThreadId];
    std::scoped_lock<std::mutex> lock(_dbMapMutexVector[id]);
    const auto& dbMap = _dbMapVector[id];

    DBMap::const_iterator it = dbMap.find(db);
    if (it != dbMap.end()) {
        return it->second.get();
    }

    return nullptr;
}

std::set<std::string> DatabaseHolderImpl::_getNamesWithConflictingCasing_inlock(StringData name) {
    std::set<std::string> duplicates;
    int16_t id = localThreadId + 1;
    const auto& dbMap = _dbMapVector[id];
    for (const auto& nameAndPointer : dbMap) {
        // A name that's equal with case-insensitive match must be identical, or it's a duplicate.
        if (name.equalCaseInsensitive(nameAndPointer.first) && name != nameAndPointer.first) {
            duplicates.insert(nameAndPointer.first);
        }
    }
    return duplicates;
}


std::set<std::string> DatabaseHolderImpl::getNamesWithConflictingCasing(StringData name) {
    return _getNamesWithConflictingCasing_inlock(name);
}

Database* DatabaseHolderImpl::openDb(OperationContext* opCtx, StringData ns, bool* justCreated) {
    // _registerReadLock();
    const StringData dbName = _todb(ns);
    invariant(opCtx->lockState()->isDbLockedForMode(dbName, MODE_X));

    if (justCreated) {
        *justCreated = false;  // Until proven otherwise.
    }

    int16_t id = localThreadId + 1;

    {
        std::scoped_lock<std::mutex> lock(_dbMapMutexVector[id]);
        const auto& dbMap = _dbMapVector[id];
        // The following will insert a nullptr for dbname, which will treated the same as a non-
        // existant database by the get method, yet still counts in getNamesWithConflictingCasing.
        if (const auto& db = dbMap.find(dbName); db != dbMap.end()) {
            return db->second.get();
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
    }
    // We've inserted a nullptr entry for dbname: make sure to remove it on unsuccessful exit.
    // auto removeDbGuard = MakeGuard([&dbMap, &dbname] {
    //     // if (!lk.owns_lock())
    //     //     lk.lock();
    //     dbMap.erase(dbname);
    // });

    StorageEngine* storageEngine = getGlobalServiceContext()->getStorageEngine();
    DatabaseCatalogEntry* entry = storageEngine->getDatabaseCatalogEntry(opCtx, dbName);

    if (!entry->exists()) {
        audit::logCreateDatabase(&cc(), dbName);
        if (justCreated) {
            *justCreated = true;
        }
    }

    // Finally replace our nullptr entry with the new Database pointer.
    // auto newDb = stdx::make_unique<Database>(opCtx, dbname, entry);
    auto newDbPtr = std::make_shared<Database>(opCtx, dbName, entry);
    ;  // new Database(opCtx, dbName, entry);
    // auto ret = newDbPtr.get();

    // removeDbGuard.Dismiss();
    for (size_t i = 0; i < _dbMapVector.size(); ++i) {
        std::scoped_lock<std::mutex> lock(_dbMapMutexVector[i]);
        // newDbPtr = dbMap.try_emplace(dbName, opCtx, dbName, entry).first->second.get();
        // dbMap[i][dbName] = std::move(newDbPtr);
        _dbMapVector[i][dbName] = newDbPtr;
    }

    MONGO_LOG(0) << "new DB Ptr use count: " << newDbPtr.use_count();
    DEV {
        // ThreadlocalLock lk(_lockVector[localThreadId]);
        std::scoped_lock<std::mutex> lock(_dbMapMutexVector[id]);
        invariant(_getNamesWithConflictingCasing_inlock(dbName).empty());
    }

    return newDbPtr.get();
}

namespace {
void evictDatabaseFromUUIDCatalog(OperationContext* opCtx, Database* db) {
    UUIDCatalog::get(opCtx).onCloseDatabase(db);
    for (auto&& coll : *db) {
        NamespaceUUIDCache::get(opCtx).evictNamespace(coll->ns());
    }
}
}  // namespace

void DatabaseHolderImpl::close(OperationContext* opCtx, StringData ns, const std::string& reason) {
    invariant(opCtx->lockState()->isW());

    const StringData dbName = _todb(ns);

    int16_t id = localThreadId + 1;
    // first lock locally. do delete work
    {
        std::scoped_lock<std::mutex> lock(_dbMapMutexVector[id]);
        const auto& dbMap = _dbMapVector[id];

        DBMap::const_iterator it = dbMap.find(dbName);
        if (it != dbMap.end()) {
            auto db = it->second.get();
            repl::oplogCheckCloseDatabase(opCtx, db);
            evictDatabaseFromUUIDCatalog(opCtx, db);

            // only close and delete db pointer once
            db->close(opCtx, reason);
            // delete db;
            // db = nullptr;
        }
    }

    // the update all cache
    for (size_t i = 0; i < _dbMapVector.size(); ++i) {
        std::scoped_lock<std::mutex> lock(_dbMapMutexVector[i]);
        _dbMapVector[i].erase(dbName);
    }

    getGlobalServiceContext()
        ->getStorageEngine()
        ->closeDatabase(opCtx, dbName)
        .transitional_ignore();
}

void DatabaseHolderImpl::closeAll(OperationContext* opCtx, const std::string& reason) {
    invariant(opCtx->lockState()->isW());

    for (size_t i = 0; i < _dbMapVector.size(); ++i) {
        std::scoped_lock<std::mutex> lock(_dbMapMutexVector[i]);
        for (auto& [dbName, dbPtr] : _dbMapVector[i]) {
            BackgroundOperation::assertNoBgOpInProgForDb(dbName);
            LOG(1) << "DatabaseHolder::closeAll name:" << dbName;
            repl::oplogCheckCloseDatabase(opCtx, dbPtr.get());
            evictDatabaseFromUUIDCatalog(opCtx, dbPtr.get());
            dbPtr->close(opCtx, reason);

            getGlobalServiceContext()
                ->getStorageEngine()
                ->closeDatabase(opCtx, dbName)
                .transitional_ignore();
        }
        _dbMapVector[i].clear();
    }
}
}  // namespace mongo
