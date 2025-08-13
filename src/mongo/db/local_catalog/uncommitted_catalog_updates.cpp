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

#include "uncommitted_catalog_updates.h"

#include "mongo/db/local_catalog/collection_catalog.h"
#include "mongo/db/local_catalog/shard_role_api/transaction_resources.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/util/decorable.h"

#include <algorithm>
#include <utility>

#include <absl/container/node_hash_map.h>
#include <boost/container/small_vector.hpp>
#include <boost/container/vector.hpp>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

namespace {
const RecoveryUnit::Snapshot::Decoration<UncommittedCatalogUpdates> getUncommittedCatalogUpdates =
    RecoveryUnit::Snapshot::declareDecoration<UncommittedCatalogUpdates>();

const RecoveryUnit::Snapshot::Decoration<OpenedCollections> getOpenedCollections =
    RecoveryUnit::Snapshot::declareDecoration<OpenedCollections>();
}  // namespace

UncommittedCatalogUpdates& UncommittedCatalogUpdates::get(OperationContext* opCtx) {
    return getUncommittedCatalogUpdates(shard_role_details::getRecoveryUnit(opCtx)->getSnapshot());
}

UncommittedCatalogUpdates::CollectionLookupResult UncommittedCatalogUpdates::lookupCollection(
    OperationContext* opCtx, UUID uuid) {
    auto& entries = UncommittedCatalogUpdates::get(opCtx)._entries;

    // Perform a reverse search so we find the most recent entry affecting this uuid.
    auto it = std::find_if(entries.rbegin(), entries.rend(), [uuid](auto&& entry) {
        // Renamed actions don't have UUIDs.
        if (entry.action == Entry::Action::kRenamedCollection) {
            return false;
        }

        return entry.uuid() == uuid;
    });

    if (it == entries.rend()) {
        return {false, nullptr, false};
    }
    return {true, it->collection, (it->action == Entry::Action::kCreatedCollection)};
}

UncommittedCatalogUpdates::CollectionLookupResult UncommittedCatalogUpdates::lookupCollection(
    OperationContext* opCtx, const NamespaceString& nss) {
    auto& entries = UncommittedCatalogUpdates::get(opCtx)._entries;
    // Perform a reverse search so we find most recent entry affecting this namespace.
    auto it = std::find_if(entries.rbegin(), entries.rend(), [&nss](auto&& entry) {
        return entry.nss == nss && isCollectionEntry(entry);
    });
    if (it == entries.rend()) {
        return {false, nullptr, false};
    }

    return {true, it->collection, (it->action == Entry::Action::kCreatedCollection)};
}

boost::optional<const ViewsForDatabase&> UncommittedCatalogUpdates::getViewsForDatabase(
    const DatabaseName& dbName) const {
    // Perform a reverse search so we find most recent entry affecting this namespace.
    auto it = std::find_if(_entries.rbegin(), _entries.rend(), [&](auto&& entry) {
        return entry.nss.dbName() == dbName && entry.viewsForDb;
    });
    if (it == _entries.rend()) {
        return boost::none;
    }
    return {*it->viewsForDb};
}

void UncommittedCatalogUpdates::createCollection(OperationContext* opCtx,
                                                 std::shared_ptr<Collection> coll) {
    _createCollection(opCtx, coll, UncommittedCatalogUpdates::Entry::Action::kCreatedCollection);
}

void UncommittedCatalogUpdates::recreateCollection(OperationContext* opCtx,
                                                   std::shared_ptr<Collection> coll) {
    _createCollection(opCtx, coll, UncommittedCatalogUpdates::Entry::Action::kRecreatedCollection);
}

void UncommittedCatalogUpdates::_createCollection(OperationContext* opCtx,
                                                  std::shared_ptr<Collection> coll,
                                                  Entry::Action action) {
    const auto& nss = coll->ns();
    auto uuid = coll->uuid();
    _entries.push_back({action, coll, nss, uuid});
}

void UncommittedCatalogUpdates::writableCollection(std::shared_ptr<Collection> collection) {
    const auto& ns = collection->ns();
    _entries.push_back({Entry::Action::kWritableCollection, std::move(collection), ns});
}

void UncommittedCatalogUpdates::renameCollection(const Collection* collection,
                                                 const NamespaceString& from) {
    auto it = std::find_if(_entries.rbegin(), _entries.rend(), [collection](auto&& entry) {
        return entry.collection.get() == collection;
    });
    invariant(it != _entries.rend());
    it->nss = collection->ns();
    _entries.push_back({Entry::Action::kRenamedCollection, nullptr, from, boost::none, it->nss});
}

void UncommittedCatalogUpdates::dropCollection(const Collection* collection) {
    auto it =
        std::find_if(_entries.rbegin(), _entries.rend(), [uuid = collection->uuid()](auto&& entry) {
            return entry.uuid() == uuid;
        });
    if (it == _entries.rend()) {
        // An entry with this uuid was not found so add a new entry.
        Entry entry;
        entry.action = Entry::Action::kDroppedCollection;
        entry.nss = collection->ns();
        entry.externalUUID = collection->uuid();
        _entries.push_back(std::move(entry));
        return;
    }

    if (it->action == Entry::Action::kRecreatedCollection) {
        // Scenario: i. drop nss  ->  ii. recreate nss  ->  iii. drop nss again
        //
        // In this case, we don't have to create another drop entry (iii), we can simply delete
        // entry (ii) such that subsequent lookups will find the previous drop (i).
        _entries.erase(it.base());
        return;
    }

    // If the entry doesn't have a Collection pointer, no further action is needed.
    if (!it->collection) {
        return;
    }

    // Transform the found entry into a dropped entry.
    invariant(it->collection.get() == collection);
    it->action = Entry::Action::kDroppedCollection;
    it->externalUUID = it->collection->uuid();
    it->collection = nullptr;
}

void UncommittedCatalogUpdates::replaceViewsForDatabase(const DatabaseName& dbName,
                                                        ViewsForDatabase&& vfdb) {
    _entries.push_back({Entry::Action::kReplacedViewsForDatabase,
                        nullptr,
                        NamespaceString{dbName},
                        boost::none,
                        {},
                        std::move(vfdb)});
}

void UncommittedCatalogUpdates::addView(OperationContext* opCtx, const NamespaceString& nss) {
    shard_role_details::getRecoveryUnit(opCtx)->registerPreCommitHook(
        [nss](OperationContext* opCtx) {
            CollectionCatalog::write(opCtx, [opCtx, nss](CollectionCatalog& catalog) {
                catalog.registerUncommittedView(opCtx, nss);
            });
        });
    shard_role_details::getRecoveryUnit(opCtx)->onRollback([nss](OperationContext* opCtx) {
        CollectionCatalog::write(
            opCtx, [&](CollectionCatalog& catalog) { catalog.deregisterUncommittedView(nss); });
    });
    _entries.push_back({Entry::Action::kAddViewResource, nullptr, nss});
}

void UncommittedCatalogUpdates::removeView(const NamespaceString& nss) {
    _entries.push_back({Entry::Action::kRemoveViewResource, nullptr, nss});
}

const std::vector<UncommittedCatalogUpdates::Entry>& UncommittedCatalogUpdates::entries() const {
    return _entries;
}

std::vector<UncommittedCatalogUpdates::Entry> UncommittedCatalogUpdates::releaseEntries() {
    std::vector<Entry> ret;
    std::swap(ret, _entries);
    return ret;
}

void UncommittedCatalogUpdates::setIgnoreExternalViewChanges(const DatabaseName& dbName,
                                                             bool value) {
    if (value) {
        _ignoreExternalViewChanges.emplace(dbName);
    } else {
        _ignoreExternalViewChanges.erase(dbName);
    }
}

bool UncommittedCatalogUpdates::shouldIgnoreExternalViewChanges(const DatabaseName& dbName) const {
    return _ignoreExternalViewChanges.contains(dbName);
}

bool UncommittedCatalogUpdates::isCreatedCollection(OperationContext* opCtx,
                                                    const NamespaceString& nss) {
    const auto& lookupResult = lookupCollection(opCtx, nss);
    return lookupResult.newColl;
}

OpenedCollections& OpenedCollections::get(OperationContext* opCtx) {
    return getOpenedCollections(shard_role_details::getRecoveryUnit(opCtx)->getSnapshot());
}

boost::optional<std::shared_ptr<const Collection>> OpenedCollections::lookupByNamespace(
    const NamespaceString& ns) const {
    auto it = std::find_if(_collections.begin(), _collections.end(), [&ns](const auto& entry) {
        if (!entry.nss)
            return false;

        return entry.nss.value() == ns;
    });
    if (it != _collections.end()) {
        return it->collection;
    }
    return boost::none;
}

boost::optional<std::shared_ptr<const Collection>> OpenedCollections::lookupByUUID(
    UUID uuid) const {
    auto it = std::find_if(_collections.begin(), _collections.end(), [&uuid](const auto& entry) {
        if (!entry.uuid)
            return false;

        return entry.uuid.value() == uuid;
    });
    if (it != _collections.end()) {
        return it->collection;
    }
    return boost::none;
}

void OpenedCollections::store(std::shared_ptr<const Collection> coll,
                              boost::optional<NamespaceString> nss,
                              boost::optional<UUID> uuid) {
    if (coll) {
        invariant(nss == coll->ns());
        invariant(uuid == coll->uuid());
    }
    _collections.push_back({std::move(coll), nss, uuid});
}

}  // namespace mongo
