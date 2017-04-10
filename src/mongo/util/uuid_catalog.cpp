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

#include "uuid_catalog.h"

#include "mongo/db/storage/recovery_unit.h"
#include "mongo/util/uuid.h"

namespace mongo {

const ServiceContext::Decoration<UUIDCatalog> UUIDCatalog::get =
    ServiceContext::declareDecoration<UUIDCatalog>();

void UUIDCatalog::onCreateCollection(OperationContext* opCtx,
                                     Collection* coll,
                                     CollectionUUID uuid) {
    _registerUUIDCatalogEntry(uuid, coll);
    opCtx->recoveryUnit()->onRollback([this, uuid] { _removeUUIDCatalogEntry(uuid); });
}

Collection* UUIDCatalog::lookupCollectionByUUID(CollectionUUID uuid) {
    stdx::lock_guard<stdx::mutex> lock(_catalogLock);
    Collection* foundCol = _catalog[uuid];
    return foundCol;
}

NamespaceString UUIDCatalog::lookupNSSByUUID(CollectionUUID uuid) {
    stdx::lock_guard<stdx::mutex> lock(_catalogLock);
    Collection* foundCol = _catalog[uuid];
    NamespaceString nss = foundCol ? foundCol->ns() : NamespaceString();
    return nss;
}

void UUIDCatalog::onDropCollection(OperationContext* opCtx, CollectionUUID uuid) {
    Collection* foundCol = _removeUUIDCatalogEntry(uuid);
    opCtx->recoveryUnit()->onRollback(
        [this, foundCol, uuid] { _registerUUIDCatalogEntry(uuid, foundCol); });
}

void UUIDCatalog::_registerUUIDCatalogEntry(CollectionUUID uuid, Collection* coll) {
    stdx::lock_guard<stdx::mutex> lock(_catalogLock);
    if (coll) {
        std::pair<CollectionUUID, Collection*> entry = std::make_pair(uuid, coll);
        invariant(_catalog.insert(entry).second == true);
    }
}

Collection* UUIDCatalog::_removeUUIDCatalogEntry(CollectionUUID uuid) {
    stdx::lock_guard<stdx::mutex> lock(_catalogLock);
    Collection* foundCol = _catalog[uuid];
    invariant(_catalog.erase(uuid) <= 1);
    return foundCol;
}
}  // namespace mongo
