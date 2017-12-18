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

}  // namespace

AutoGetDb::AutoGetDb(OperationContext* opCtx, StringData ns, LockMode mode)
    : _dbLock(opCtx, ns, mode), _db(dbHolder().get(opCtx, ns)) {}

AutoGetDb::AutoGetDb(OperationContext* opCtx, StringData ns, Lock::DBLock lock)
    : _dbLock(std::move(lock)), _db(dbHolder().get(opCtx, ns)) {}

AutoGetCollection::AutoGetCollection(OperationContext* opCtx,
                                     const NamespaceString& nss,
                                     const UUID& uuid,
                                     LockMode modeAll)
    : _viewMode(ViewMode::kViewsForbidden),
      _autoDb(opCtx, nss.db(), Lock::DBLock(opCtx, nss.db(), modeAll)),
      _collLock(opCtx->lockState(), nss.ns(), modeAll),
      _coll(UUIDCatalog::get(opCtx).lookupCollectionByUUID(uuid)) {
    // Wait for a configured amount of time after acquiring locks if the failpoint is enabled
    MONGO_FAIL_POINT_BLOCK(setAutoGetCollectionWait, customWait) {
        const BSONObj& data = customWait.getData();
        sleepFor(Milliseconds(data["waitForMillis"].numberInt()));
    }
}

AutoGetCollection::AutoGetCollection(OperationContext* opCtx,
                                     const NamespaceString& nss,
                                     LockMode modeColl,
                                     ViewMode viewMode,
                                     Lock::DBLock lock)
    : _viewMode(viewMode),
      _autoDb(opCtx, nss.db(), std::move(lock)),
      _collLock(opCtx->lockState(), nss.ns(), modeColl),
      _coll(_autoDb.getDb() ? _autoDb.getDb()->getCollection(opCtx, nss) : nullptr) {
    Database* const db = _autoDb.getDb();

    // If the database exists, but not the collection, check for views
    if (_viewMode == ViewMode::kViewsForbidden && db && !_coll &&
        db->getViewCatalog()->lookup(opCtx, nss.ns())) {
        uasserted(ErrorCodes::CommandNotSupportedOnView,
                  str::stream() << "Namespace " << nss.ns() << " is a view, not a collection");
    }

    // Wait for a configured amount of time after acquiring locks if the failpoint is enabled
    MONGO_FAIL_POINT_BLOCK(setAutoGetCollectionWait, customWait) {
        const BSONObj& data = customWait.getData();
        sleepFor(Milliseconds(data["waitForMillis"].numberInt()));
    }
}

AutoGetCollection::AutoGetCollection(OperationContext* opCtx,
                                     const NamespaceString& nss,
                                     LockMode modeDB,
                                     LockMode modeColl,
                                     ViewMode viewMode)
    : AutoGetCollection(opCtx, nss, modeColl, viewMode, Lock::DBLock(opCtx, nss.db(), modeDB)) {}

AutoGetCollectionOrView::AutoGetCollectionOrView(OperationContext* opCtx,
                                                 const NamespaceString& nss,
                                                 LockMode modeAll)
    : _autoColl(opCtx, nss, modeAll, modeAll, AutoGetCollection::ViewMode::kViewsPermitted),
      _view(_autoColl.getDb() && !_autoColl.getCollection()
                ? _autoColl.getDb()->getViewCatalog()->lookup(opCtx, nss.ns())
                : nullptr) {}

AutoGetOrCreateDb::AutoGetOrCreateDb(OperationContext* opCtx, StringData ns, LockMode mode)
    : _dbLock(opCtx, ns, mode), _db(dbHolder().get(opCtx, ns)) {
    invariant(mode == MODE_IX || mode == MODE_X);
    _justCreated = false;

    // If the database didn't exist, relock in MODE_X
    if (_db == NULL) {
        if (mode != MODE_X) {
            _dbLock.relockWithMode(MODE_X);
        }

        _db = dbHolder().openDb(opCtx, ns);
        _justCreated = true;
    }
}

}  // namespace mongo
