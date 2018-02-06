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

#include "mongo/db/catalog/catalog_raii.h"

#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/catalog/uuid_catalog.h"
#include "mongo/util/fail_point_service.h"

namespace mongo {
namespace {

MONGO_FP_DECLARE(setAutoGetCollectionWait);

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
    : _dbLock(opCtx, dbName, mode, deadline), _db(dbHolder().get(opCtx, dbName)) {
    uassertLockTimeout("database " + dbName, mode, deadline, _dbLock.isLocked());
}

AutoGetDb::AutoGetDb(OperationContext* opCtx, StringData dbName, Lock::DBLock dbLock)
    : _dbLock(std::move(dbLock)), _db(dbHolder().get(opCtx, dbName)) {
    uassert(ErrorCodes::LockTimeout,
            str::stream() << "Failed to acquire lock for '" << dbName << "'.",
            _dbLock.isLocked());
}

AutoGetCollection::AutoGetCollection(OperationContext* opCtx,
                                     const NamespaceStringOrUUID& nsOrUUID,
                                     LockMode modeDB,
                                     LockMode modeColl,
                                     ViewMode viewMode,
                                     Date_t deadline)
    : AutoGetCollection(opCtx,
                        nsOrUUID,
                        Lock::DBLock(opCtx, nsOrUUID.db(), modeDB, deadline),
                        modeColl,
                        viewMode,
                        deadline) {}

AutoGetCollection::AutoGetCollection(OperationContext* opCtx,
                                     const NamespaceStringOrUUID& nsOrUUID,
                                     Lock::DBLock dbLock,
                                     LockMode modeColl,
                                     ViewMode viewMode,
                                     Date_t deadline)
    : _autoDb(opCtx, nsOrUUID.db(), std::move(dbLock)),
      _nsAndLock([&]() -> NamespaceAndCollectionLock {
          if (nsOrUUID.nss()) {
              return {Lock::CollectionLock(
                          opCtx->lockState(), nsOrUUID.nss()->ns(), modeColl, deadline),
                      *nsOrUUID.nss()};
          } else {
              UUIDCatalog& catalog = UUIDCatalog::get(opCtx);
              auto resolvedNss = catalog.lookupNSSByUUID(nsOrUUID.dbAndUUID()->uuid);

              // If the collection UUID cannot be resolved, we can't obtain a collection or check
              // for vews
              uassert(ErrorCodes::NamespaceNotFound,
                      str::stream() << "Unable to resolve " << nsOrUUID.toString(),
                      resolvedNss.isValid());

              return {
                  Lock::CollectionLock(opCtx->lockState(), resolvedNss.ns(), modeColl, deadline),
                  std::move(resolvedNss)};
          }
      }()) {
    // Wait for a configured amount of time after acquiring locks if the failpoint is enabled
    MONGO_FAIL_POINT_BLOCK(setAutoGetCollectionWait, customWait) {
        const BSONObj& data = customWait.getData();
        sleepFor(Milliseconds(data["waitForMillis"].numberInt()));
    }

    uassertLockTimeout(
        "collection " + nsOrUUID.toString(), modeColl, deadline, _nsAndLock.lock.isLocked());

    Database* const db = _autoDb.getDb();

    // If the database doesn't exists, we can't obtain a collection or check for views
    if (!db)
        return;

    _coll = db->getCollection(opCtx, _nsAndLock.nss);

    // If the collection exists, there is no need to check for views and we must keep the collection
    // lock
    if (_coll) {
        return;
    }

    _view = db->getViewCatalog()->lookup(opCtx, _nsAndLock.nss.ns());
    uassert(ErrorCodes::CommandNotSupportedOnView,
            str::stream() << "Namespace " << _nsAndLock.nss.ns() << " is a view, not a collection",
            !_view || viewMode == kViewsPermitted);
}

AutoGetOrCreateDb::AutoGetOrCreateDb(OperationContext* opCtx,
                                     StringData dbName,
                                     LockMode mode,
                                     Date_t deadline)
    : _dbLock(opCtx, dbName, mode, deadline), _db(dbHolder().get(opCtx, dbName)) {
    invariant(mode == MODE_IX || mode == MODE_X);
    _justCreated = false;

    uassertLockTimeout("database " + dbName, mode, deadline, _dbLock.isLocked());

    // If the database didn't exist, relock in MODE_X
    if (_db == NULL) {
        if (mode != MODE_X) {
            _dbLock.relockWithMode(MODE_X);
        }

        _db = dbHolder().openDb(opCtx, dbName);
        _justCreated = true;
    }
}

}  // namespace mongo
