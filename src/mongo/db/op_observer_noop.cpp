/**
*    Copyright (C) 2016 MongoDB Inc.
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

#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/database.h"
#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/catalog/namespace_uuid_cache.h"
#include "mongo/db/catalog/uuid_catalog.h"
#include "mongo/db/op_observer_noop.h"

namespace mongo {

void OpObserverNoop::onCreateIndex(
    OperationContext*, const NamespaceString&, OptionalCollectionUUID, BSONObj, bool) {}

void OpObserverNoop::onInserts(OperationContext*,
                               const NamespaceString&,
                               OptionalCollectionUUID,
                               std::vector<InsertStatement>::const_iterator,
                               std::vector<InsertStatement>::const_iterator,
                               bool) {}

void OpObserverNoop::onUpdate(OperationContext*, const OplogUpdateEntryArgs&) {}

CollectionShardingState::DeleteState OpObserverNoop::aboutToDelete(OperationContext*,
                                                                   const NamespaceString&,
                                                                   const BSONObj&) {
    return {};
}

void OpObserverNoop::onDelete(OperationContext*,
                              const NamespaceString&,
                              OptionalCollectionUUID,
                              StmtId stmtId,
                              CollectionShardingState::DeleteState,
                              bool) {}

void OpObserverNoop::onOpMessage(OperationContext*, const BSONObj&) {}

void OpObserverNoop::onCreateCollection(OperationContext* opCtx,
                                        Collection* coll,
                                        const NamespaceString& collectionName,
                                        const CollectionOptions& options,
                                        const BSONObj& idIndex) {
    if (options.uuid) {
        UUIDCatalog& catalog = UUIDCatalog::get(opCtx);
        catalog.onCreateCollection(opCtx, coll, options.uuid.get());
        NamespaceUUIDCache& cache = NamespaceUUIDCache::get(opCtx);
        opCtx->recoveryUnit()->onRollback(
            [&cache, collectionName]() { cache.evictNamespace(collectionName); });
    }
}

void OpObserverNoop::onCollMod(OperationContext*,
                               const NamespaceString&,
                               OptionalCollectionUUID,
                               const BSONObj&,
                               const CollectionOptions& oldCollOptions,
                               boost::optional<TTLCollModInfo> ttlInfo) {}

void OpObserverNoop::onDropDatabase(OperationContext*, const std::string&) {}

repl::OpTime OpObserverNoop::onDropCollection(OperationContext* opCtx,
                                              const NamespaceString& collectionName,
                                              OptionalCollectionUUID uuid) {
    // Evict namespace entry from the namespace/uuid cache if it exists.
    NamespaceUUIDCache& cache = NamespaceUUIDCache::get(opCtx);
    cache.evictNamespace(collectionName);

    // Remove collection from the uuid catalog.
    if (uuid) {
        UUIDCatalog& catalog = UUIDCatalog::get(opCtx);
        catalog.onDropCollection(opCtx, uuid.get());
    }

    return {};
}

void OpObserverNoop::onDropIndex(OperationContext*,
                                 const NamespaceString&,
                                 OptionalCollectionUUID,
                                 const std::string&,
                                 const BSONObj&) {}

void OpObserverNoop::onRenameCollection(OperationContext* opCtx,
                                        const NamespaceString& fromCollection,
                                        const NamespaceString& toCollection,
                                        OptionalCollectionUUID uuid,
                                        bool dropTarget,
                                        OptionalCollectionUUID dropTargetUUID,
                                        OptionalCollectionUUID dropSourceUUID,
                                        bool stayTemp) {
    // Evict namespace entry from the namespace/uuid cache if it exists.
    NamespaceUUIDCache& cache = NamespaceUUIDCache::get(opCtx);
    cache.evictNamespace(fromCollection);
    cache.evictNamespace(toCollection);
    opCtx->recoveryUnit()->onRollback(
        [&cache, toCollection]() { cache.evictNamespace(toCollection); });

    // Finally update the UUID Catalog.
    if (uuid) {
        auto db = dbHolder().get(opCtx, toCollection.db());
        auto newColl = db->getCollection(opCtx, toCollection);
        invariant(newColl);
        UUIDCatalog& catalog = UUIDCatalog::get(opCtx);
        catalog.onRenameCollection(opCtx, newColl, uuid.get());
    }
}

void OpObserverNoop::onApplyOps(OperationContext*, const std::string&, const BSONObj&) {}

void OpObserverNoop::onEmptyCapped(OperationContext*,
                                   const NamespaceString&,
                                   OptionalCollectionUUID) {}

}  // namespace mongo
