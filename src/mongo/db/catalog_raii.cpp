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

#include "mongo/platform/basic.h"

#include "mongo/db/catalog_raii.h"

#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/catalog/uuid_catalog.h"
#include "mongo/db/s/database_sharding_state.h"
#include "mongo/util/fail_point_service.h"

namespace mongo {
namespace {

MONGO_FAIL_POINT_DEFINE(setAutoGetCollectionWait);

void uassertLockTimeout(std::string resourceName,
                        LockMode lockMode,
                        Date_t deadline,
                        bool isLocked) {
    uassert(ErrorCodes::LockTimeout,
            str::stream() << "Failed to acquire " << modeName(lockMode) << " lock for "
                          << resourceName
                          << " since deadline "
                          << dateToISOStringLocal(deadline)
                          << " has passed.",
            isLocked);
}

}  // namespace

AutoGetDb::AutoGetDb(OperationContext* opCtx, StringData dbName, LockMode mode, Date_t deadline)
    : _dbLock(opCtx, dbName, mode, deadline), _db([&] {
          uassertLockTimeout(
              str::stream() << "database " << dbName, mode, deadline, _dbLock.isLocked());
          return DatabaseHolder::getDatabaseHolder().get(opCtx, dbName);
      }()) {
    if (_db) {
        DatabaseShardingState::get(_db).checkDbVersion(opCtx);
    }
}

AutoGetCollection::AutoGetCollection(OperationContext* opCtx,
                                     const NamespaceStringOrUUID& nsOrUUID,
                                     LockMode modeDB,
                                     LockMode modeColl,
                                     ViewMode viewMode,
                                     Date_t deadline)
    :  // The UUID to NamespaceString resolution is performed outside of any locks
      _resolvedNss(resolveNamespaceStringOrUUID(opCtx, nsOrUUID)),
      // The database locking is performed based on the resolved NamespaceString
      _autoDb(opCtx, _resolvedNss.db(), modeDB, deadline) {
    // In order to account for possible collection rename happening because the resolution from UUID
    // to NamespaceString was done outside of database lock, if UUID was specified we need to
    // re-resolve the _resolvedNss after acquiring the database lock so it has the correct value.
    //
    // Holding a database lock prevents collection renames, so this guarantees a stable UUID to
    // NamespaceString mapping.
    if (nsOrUUID.uuid())
        _resolvedNss = resolveNamespaceStringOrUUID(opCtx, nsOrUUID);

    _collLock.emplace(opCtx->lockState(), _resolvedNss.ns(), modeColl, deadline);
    uassertLockTimeout(str::stream() << "collection " << nsOrUUID.toString(),
                       modeColl,
                       deadline,
                       _collLock->isLocked());

    // Wait for a configured amount of time after acquiring locks if the failpoint is enabled
    MONGO_FAIL_POINT_BLOCK(setAutoGetCollectionWait, customWait) {
        const BSONObj& data = customWait.getData();
        sleepFor(Milliseconds(data["waitForMillis"].numberInt()));
    }

    Database* const db = _autoDb.getDb();
    invariant(!nsOrUUID.uuid() || db,
              str::stream() << "Database for " << _resolvedNss.ns()
                            << " disappeared after successufully resolving "
                            << nsOrUUID.toString());

    // If the database doesn't exists, we can't obtain a collection or check for views
    if (!db)
        return;

    _coll = db->getCollection(opCtx, _resolvedNss);
    invariant(!nsOrUUID.uuid() || _coll,
              str::stream() << "Collection for " << _resolvedNss.ns()
                            << " disappeared after successufully resolving "
                            << nsOrUUID.toString());

    if (_coll) {
        // Unlike read concern majority, read concern snapshot cannot yield and wait when there are
        // pending catalog changes. Instead, we must return an error in such situations. We ignore
        // this restriction for the oplog, since it never has pending catalog changes.
        auto readConcernLevel = repl::ReadConcernArgs::get(opCtx).getLevel();
        auto mySnapshot = opCtx->recoveryUnit()->getPointInTimeReadTimestamp();
        if (readConcernLevel == repl::ReadConcernLevel::kSnapshotReadConcern && mySnapshot &&
            _resolvedNss != NamespaceString::kRsOplogNamespace) {
            auto minSnapshot = _coll->getMinimumVisibleSnapshot();
            uassert(
                ErrorCodes::SnapshotUnavailable,
                str::stream() << "Unable to read from a snapshot due to pending collection catalog "
                                 "changes; please retry the operation. Snapshot timestamp is "
                              << mySnapshot->toString()
                              << ". Collection minimum is "
                              << minSnapshot->toString(),
                !minSnapshot || *mySnapshot >= *minSnapshot);
        }

        // If the collection exists, there is no need to check for views.
        return;
    }

    _view = db->getViewCatalog()->lookup(opCtx, _resolvedNss.ns());
    uassert(ErrorCodes::CommandNotSupportedOnView,
            str::stream() << "Namespace " << _resolvedNss.ns() << " is a view, not a collection",
            !_view || viewMode == kViewsPermitted);
}

NamespaceString AutoGetCollection::resolveNamespaceStringOrUUID(OperationContext* opCtx,
                                                                NamespaceStringOrUUID nsOrUUID) {
    if (nsOrUUID.nss())
        return *nsOrUUID.nss();

    UUIDCatalog& uuidCatalog = UUIDCatalog::get(opCtx);
    auto resolvedNss = uuidCatalog.lookupNSSByUUID(*nsOrUUID.uuid());

    uassert(ErrorCodes::NamespaceNotFound,
            str::stream() << "Unable to resolve " << nsOrUUID.toString(),
            resolvedNss.isValid());

    uassert(ErrorCodes::NamespaceNotFound,
            str::stream() << "UUID " << nsOrUUID.toString() << " specified in " << nsOrUUID.dbname()
                          << " resolved to a collection in a different database: "
                          << resolvedNss.toString(),
            resolvedNss.db() == nsOrUUID.dbname());

    return resolvedNss;
}

AutoGetOrCreateDb::AutoGetOrCreateDb(OperationContext* opCtx,
                                     StringData dbName,
                                     LockMode mode,
                                     Date_t deadline) {
    invariant(mode == MODE_IX || mode == MODE_X);

    _autoDb.emplace(opCtx, dbName, mode, deadline);
    _db = _autoDb->getDb();

    // If the database didn't exist, relock in MODE_X
    if (!_db) {
        if (mode != MODE_X) {
            _autoDb.emplace(opCtx, dbName, MODE_X, deadline);
        }

        _db = DatabaseHolder::getDatabaseHolder().openDb(opCtx, dbName, &_justCreated);
    }

    DatabaseShardingState::get(_db).checkDbVersion(opCtx);
}

}  // namespace mongo
