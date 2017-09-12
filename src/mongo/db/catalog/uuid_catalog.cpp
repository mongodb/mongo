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
#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kStorage

#include "mongo/platform/basic.h"

#include "uuid_catalog.h"

#include "mongo/db/catalog/database.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/util/log.h"
#include "mongo/util/uuid.h"

namespace mongo {
namespace {
const ServiceContext::Decoration<UUIDCatalog> getCatalog =
    ServiceContext::declareDecoration<UUIDCatalog>();
}  // namespace

UUIDCatalog& UUIDCatalog::get(ServiceContext* svcCtx) {
    return getCatalog(svcCtx);
}
UUIDCatalog& UUIDCatalog::get(OperationContext* opCtx) {
    return getCatalog(opCtx->getServiceContext());
}

void UUIDCatalog::onCreateCollection(OperationContext* opCtx,
                                     Collection* coll,
                                     CollectionUUID uuid) {
    removeUUIDCatalogEntry(uuid);
    registerUUIDCatalogEntry(uuid, coll);
    opCtx->recoveryUnit()->onRollback([this, uuid] { removeUUIDCatalogEntry(uuid); });
}

void UUIDCatalog::onDropCollection(OperationContext* opCtx, CollectionUUID uuid) {
    Collection* foundColl = removeUUIDCatalogEntry(uuid);
    opCtx->recoveryUnit()->onRollback(
        [this, foundColl, uuid] { registerUUIDCatalogEntry(uuid, foundColl); });
}

void UUIDCatalog::onRenameCollection(OperationContext* opCtx,
                                     GetNewCollectionFunction getNewCollection,
                                     CollectionUUID uuid) {
    Collection* oldColl = removeUUIDCatalogEntry(uuid);
    opCtx->recoveryUnit()->onCommit([this, getNewCollection, uuid] {
        // Reset current UUID entry in case some other operation updates the UUID catalog before the
        // WUOW is committed. registerUUIDCatalogEntry() is a no-op if there's an existing UUID
        // entry.
        removeUUIDCatalogEntry(uuid);
        auto newColl = getNewCollection();
        invariant(newColl);
        registerUUIDCatalogEntry(uuid, newColl);
    });
    opCtx->recoveryUnit()->onRollback([this, oldColl, uuid] {
        // Reset current UUID entry in case some other operation updates the UUID catalog before the
        // WUOW is rolled back. registerUUIDCatalogEntry() is a no-op if there's an existing UUID
        // entry.
        removeUUIDCatalogEntry(uuid);
        registerUUIDCatalogEntry(uuid, oldColl);
    });
}

void UUIDCatalog::onCloseDatabase(Database* db) {
    for (auto&& coll : *db) {
        if (coll->uuid()) {
            // While the collection does not actually get dropped, we're going to destroy the
            // Collection object, so for purposes of the UUIDCatalog it looks the same.
            removeUUIDCatalogEntry(coll->uuid().get());
        }
    }
}

Collection* UUIDCatalog::lookupCollectionByUUID(CollectionUUID uuid) const {
    stdx::lock_guard<stdx::mutex> lock(_catalogLock);
    auto foundIt = _catalog.find(uuid);
    return foundIt == _catalog.end() ? nullptr : foundIt->second;
}

NamespaceString UUIDCatalog::lookupNSSByUUID(CollectionUUID uuid) const {
    stdx::lock_guard<stdx::mutex> lock(_catalogLock);
    auto foundIt = _catalog.find(uuid);
    Collection* coll = foundIt == _catalog.end() ? nullptr : foundIt->second;
    return foundIt == _catalog.end() ? NamespaceString() : coll->ns();
}

void UUIDCatalog::registerUUIDCatalogEntry(CollectionUUID uuid, Collection* coll) {
    stdx::lock_guard<stdx::mutex> lock(_catalogLock);

    if (coll && !_catalog.count(uuid)) {
        // Invalidate this database's ordering, since we're adding a new UUID.
        _orderedCollections.erase(coll->ns().db());

        std::pair<CollectionUUID, Collection*> entry = std::make_pair(uuid, coll);
        LOG(2) << "registering collection " << coll->ns() << " with UUID " << uuid.toString();
        invariant(_catalog.insert(entry).second == true);
    }
}

Collection* UUIDCatalog::removeUUIDCatalogEntry(CollectionUUID uuid) {
    stdx::lock_guard<stdx::mutex> lock(_catalogLock);

    auto foundIt = _catalog.find(uuid);
    if (foundIt == _catalog.end())
        return nullptr;

    // Invalidate this database's ordering, since we're deleting a UUID.
    _orderedCollections.erase(foundIt->second->ns().db());

    auto foundCol = foundIt->second;
    LOG(2) << "unregistering collection " << foundCol->ns() << " with UUID " << uuid.toString();
    _catalog.erase(foundIt);
    return foundCol;
}

boost::optional<CollectionUUID> UUIDCatalog::prev(const StringData& db, CollectionUUID uuid) {
    stdx::lock_guard<stdx::mutex> lock(_catalogLock);
    const auto& ordering = _getOrdering_inlock(db, lock);
    auto current = std::lower_bound(ordering.cbegin(), ordering.cend(), uuid);

    // If the element does not appear, or is the first element.
    if (current == ordering.cend() || *current != uuid || current == ordering.cbegin()) {
        return boost::none;
    }

    return *(current - 1);
}

boost::optional<CollectionUUID> UUIDCatalog::next(const StringData& db, CollectionUUID uuid) {
    stdx::lock_guard<stdx::mutex> lock(_catalogLock);
    const auto& ordering = _getOrdering_inlock(db, lock);
    auto current = std::lower_bound(ordering.cbegin(), ordering.cend(), uuid);

    if (current == ordering.cend() || *current != uuid || current + 1 == ordering.cend()) {
        return boost::none;
    }

    return *(current + 1);
}

const std::vector<CollectionUUID>& UUIDCatalog::_getOrdering_inlock(
    const StringData& db, const stdx::lock_guard<stdx::mutex>&) {
    // If an ordering is already cached,
    auto it = _orderedCollections.find(db);
    if (it != _orderedCollections.end()) {
        // return it.
        return it->second;
    }

    // Otherwise, get all of the UUIDs for this database,
    auto& newOrdering = _orderedCollections[db];
    for (const auto& pair : _catalog) {
        if (pair.second->ns().db() == db) {
            newOrdering.push_back(pair.first);
        }
    }

    // and sort them.
    std::sort(newOrdering.begin(), newOrdering.end());

    return newOrdering;
}
}  // namespace mongo
