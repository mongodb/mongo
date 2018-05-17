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
#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/log.h"
#include "mongo/util/uuid.h"

namespace mongo {
namespace {
const ServiceContext::Decoration<UUIDCatalog> getCatalog =
    ServiceContext::declareDecoration<UUIDCatalog>();
}  // namespace

void UUIDCatalogObserver::onCreateCollection(OperationContext* opCtx,
                                             Collection* coll,
                                             const NamespaceString& collectionName,
                                             const CollectionOptions& options,
                                             const BSONObj& idIndex) {
    if (!options.uuid)
        return;
    UUIDCatalog& catalog = UUIDCatalog::get(opCtx);
    catalog.onCreateCollection(opCtx, coll, options.uuid.get());
}

void UUIDCatalogObserver::onCollMod(OperationContext* opCtx,
                                    const NamespaceString& nss,
                                    OptionalCollectionUUID uuid,
                                    const BSONObj& collModCmd,
                                    const CollectionOptions& oldCollOptions,
                                    boost::optional<TTLCollModInfo> ttlInfo) {
    if (!uuid)
        return;
    UUIDCatalog& catalog = UUIDCatalog::get(opCtx);
    Collection* catalogColl = catalog.lookupCollectionByUUID(uuid.get());
    invariant(
        catalogColl->uuid() == uuid,
        str::stream() << (uuid ? uuid->toString() : "<no uuid>") << ","
                      << (catalogColl->uuid() ? catalogColl->uuid()->toString() : "<no uuid>"));
}

repl::OpTime UUIDCatalogObserver::onDropCollection(OperationContext* opCtx,
                                                   const NamespaceString& collectionName,
                                                   OptionalCollectionUUID uuid) {

    if (!uuid)
        return {};
    UUIDCatalog& catalog = UUIDCatalog::get(opCtx);
    catalog.onDropCollection(opCtx, uuid.get());
    return {};
}

void UUIDCatalogObserver::onRenameCollection(OperationContext* opCtx,
                                             const NamespaceString& fromCollection,
                                             const NamespaceString& toCollection,
                                             OptionalCollectionUUID uuid,
                                             OptionalCollectionUUID dropTargetUUID,
                                             bool stayTemp) {

    if (!uuid)
        return;
    auto db = DatabaseHolder::getDatabaseHolder().get(opCtx, toCollection.db());
    auto newColl = db->getCollection(opCtx, toCollection);
    invariant(newColl);
    UUIDCatalog& catalog = UUIDCatalog::get(opCtx);
    catalog.onRenameCollection(opCtx, newColl, uuid.get());
}

repl::OpTime UUIDCatalogObserver::preRenameCollection(OperationContext* opCtx,
                                                      const NamespaceString& fromCollection,
                                                      const NamespaceString& toCollection,
                                                      OptionalCollectionUUID uuid,
                                                      OptionalCollectionUUID dropTargetUUID,
                                                      bool stayTemp) {
    return {};
}

void UUIDCatalogObserver::postRenameCollection(OperationContext* opCtx,
                                               const NamespaceString& fromCollection,
                                               const NamespaceString& toCollection,
                                               OptionalCollectionUUID uuid,
                                               OptionalCollectionUUID dropTargetUUID,
                                               bool stayTemp) {
    // postRenameCollection and onRenameCollection are semantically equivalent from the perspective
    // of the UUIDCatalogObserver.
    onRenameCollection(opCtx, fromCollection, toCollection, uuid, dropTargetUUID, stayTemp);
}

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
                                     Collection* coll,
                                     CollectionUUID uuid) {
    invariant(coll);
    Collection* oldColl = replaceUUIDCatalogEntry(uuid, coll);
    opCtx->recoveryUnit()->onRollback(
        [this, oldColl, uuid] { replaceUUIDCatalogEntry(uuid, oldColl); });
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

void UUIDCatalog::onCloseCatalog() {
    stdx::lock_guard<stdx::mutex> lock(_catalogLock);
    invariant(!_shadowCatalog);
    _shadowCatalog.emplace();
    for (auto entry : _catalog)
        _shadowCatalog->insert({entry.first, entry.second->ns()});
}

void UUIDCatalog::onOpenCatalog() {
    stdx::lock_guard<stdx::mutex> lock(_catalogLock);
    invariant(_shadowCatalog);
    _shadowCatalog.reset();
}

Collection* UUIDCatalog::lookupCollectionByUUID(CollectionUUID uuid) const {
    stdx::lock_guard<stdx::mutex> lock(_catalogLock);
    auto foundIt = _catalog.find(uuid);
    return foundIt == _catalog.end() ? nullptr : foundIt->second;
}

NamespaceString UUIDCatalog::lookupNSSByUUID(CollectionUUID uuid) const {
    stdx::lock_guard<stdx::mutex> lock(_catalogLock);
    auto foundIt = _catalog.find(uuid);
    if (foundIt != _catalog.end())
        return foundIt->second->ns();

    // Only in the case that the catalog is closed and a UUID is currently unknown, resolve it
    // using the pre-close state. This ensures that any tasks reloading the catalog can see their
    // own updates.
    if (_shadowCatalog) {
        auto shadowIt = _shadowCatalog->find(uuid);
        if (shadowIt != _shadowCatalog->end())
            return shadowIt->second;
    }
    return NamespaceString();
}

Collection* UUIDCatalog::replaceUUIDCatalogEntry(CollectionUUID uuid, Collection* coll) {
    stdx::lock_guard<stdx::mutex> lock(_catalogLock);
    invariant(coll);

    auto foundIt = _catalog.find(uuid);
    invariant(foundIt != _catalog.end());
    // Invalidate the source database's ordering, since we're deleting a UUID.
    _orderedCollections.erase(foundIt->second->ns().db());

    Collection* oldColl = foundIt->second;
    LOG(2) << "unregistering collection " << oldColl->ns() << " with UUID " << uuid.toString();
    _catalog.erase(foundIt);

    // Invalidate the destination database's ordering, since we're adding a new UUID.
    _orderedCollections.erase(coll->ns().db());

    std::pair<CollectionUUID, Collection*> entry = std::make_pair(uuid, coll);
    LOG(2) << "registering collection " << coll->ns() << " with UUID " << uuid.toString();
    invariant(_catalog.insert(entry).second == true);
    return oldColl;
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
