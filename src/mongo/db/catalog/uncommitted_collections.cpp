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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kStorage

#include "mongo/platform/basic.h"

#include "mongo/db/catalog/collection_catalog.h"
#include "mongo/db/catalog/uncommitted_collections.h"
#include "mongo/db/storage/durable_catalog.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/log.h"

namespace mongo {
namespace {
const auto getUncommittedCollections =
    OperationContext::declareDecoration<UncommittedCollections>();
}  // namespace

UncommittedCollections& UncommittedCollections::get(OperationContext* opCtx) {
    return getUncommittedCollections(opCtx);
}

void UncommittedCollections::addToTxn(OperationContext* opCtx,
                                      std::unique_ptr<Collection> coll,
                                      Timestamp createTime) {
    auto collList = getUncommittedCollections(opCtx).getResources().lock();
    auto existingColl = collList->_collections.find(coll->uuid());
    uassert(31370,
            str::stream() << "collection already exists. ns: " << coll->ns(),
            existingColl == collList->_collections.end());

    auto nss = coll->ns();
    auto uuid = coll->uuid();
    auto collPtr = coll.get();
    collList->_collections[uuid] = std::move(coll);
    collList->_nssIndex.insert({nss, uuid});

    auto collListUnowned = getUncommittedCollections(opCtx).getResources();

    opCtx->recoveryUnit()->registerPreCommitHook(
        [collListUnowned, uuid, createTime](OperationContext* opCtx) {
            UncommittedCollections::commit(opCtx, uuid, createTime, collListUnowned.lock().get());
        });
    opCtx->recoveryUnit()->onCommit(
        [collListUnowned, collPtr, createTime](boost::optional<Timestamp> commitTs) {
            // Verify that the collection was given a minVisibleTimestamp equal to the transactions
            // commit timestamp.
            invariant(collPtr->getMinimumVisibleSnapshot() == createTime);
            UncommittedCollections::clear(collListUnowned.lock().get());
        });
    opCtx->recoveryUnit()->onRollback(
        [collListUnowned]() { UncommittedCollections::clear(collListUnowned.lock().get()); });
}

Collection* UncommittedCollections::getForTxn(OperationContext* opCtx,
                                              const NamespaceStringOrUUID& id) {
    if (id.nss()) {
        return getForTxn(opCtx, id.nss().get());
    } else {
        return getForTxn(opCtx, id.uuid().get());
    }
}

Collection* UncommittedCollections::getForTxn(OperationContext* opCtx, const NamespaceString& nss) {
    auto collList = getUncommittedCollections(opCtx).getResources().lock();
    auto it = collList->_nssIndex.find(nss);
    if (it == collList->_nssIndex.end()) {
        return nullptr;
    }

    return collList->_collections[it->second].get();
}

Collection* UncommittedCollections::getForTxn(OperationContext* opCtx, const UUID& uuid) {
    auto collList = getUncommittedCollections(opCtx).getResources().lock();
    auto it = collList->_collections.find(uuid);
    if (it == collList->_collections.end()) {
        return nullptr;
    }

    return it->second.get();
}

void UncommittedCollections::commit(OperationContext* opCtx,
                                    UUID uuid,
                                    Timestamp createTs,
                                    UncommittedCollectionsMap* map) {
    if (map->_collections.count(uuid) == 0) {
        return;
    }

    auto it = map->_collections.find(uuid);
    // Invariant that a collection is found.
    invariant(it->second.get(), uuid.toString());
    it->second->setMinimumVisibleSnapshot(createTs);

    auto nss = it->second->ns();
    CollectionCatalog::get(opCtx).registerCollection(uuid, &(it->second));
    map->_collections.erase(it);
    map->_nssIndex.erase(nss);
}

bool UncommittedCollections::hasExclusiveAccessToCollection(OperationContext* opCtx,
                                                            const NamespaceString& nss) const {
    if (opCtx->lockState()->isCollectionLockedForMode(nss, MODE_X)) {
        return true;
    }

    if (_resourcesPtr->_nssIndex.count(nss) == 1) {
        // If the collection is found in the local catalog, the appropriate locks must have already
        // been taken.
        invariant(opCtx->lockState()->isCollectionLockedForMode(nss, MODE_IX), nss.toString());
        return true;
    }

    return false;
}

}  // namespace mongo
