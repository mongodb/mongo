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

#include "mongo/db/op_observer_impl.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/auth/authorization_manager_global.h"
#include "mongo/db/catalog/collection_catalog_entry.h"
#include "mongo/db/catalog/collection_options.h"
#include "mongo/db/catalog/database.h"
#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/catalog/namespace_uuid_cache.h"
#include "mongo/db/catalog/uuid_catalog.h"
#include "mongo/db/commands/feature_compatibility_version.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/replication_coordinator_global.h"
#include "mongo/db/s/collection_sharding_state.h"
#include "mongo/db/server_options.h"
#include "mongo/db/views/durable_view_catalog.h"
#include "mongo/scripting/engine.h"
#include "mongo/util/assert_util.h"

namespace mongo {
namespace {
// Return whether we're a master using master-slave replication.
bool isMasterSlave() {
    return repl::getGlobalReplicationCoordinator()->getReplicationMode() ==
        repl::ReplicationCoordinator::modeMasterSlave;
}
}  // namespace

void OpObserverImpl::onCreateIndex(OperationContext* opCtx,
                                   const NamespaceString& nss,
                                   OptionalCollectionUUID uuid,
                                   BSONObj indexDoc,
                                   bool fromMigrate) {
    NamespaceString systemIndexes{nss.getSystemIndexesCollection()};
    if (uuid && !isMasterSlave()) {
        BSONObjBuilder builder;
        builder.append("createIndexes", nss.coll());

        for (const auto& e : indexDoc) {
            if (e.fieldNameStringData() != "ns"_sd)
                builder.append(e);
        }
        repl::logOp(opCtx,
                    "c",
                    nss.getCommandNS(),
                    uuid,
                    builder.done(),
                    nullptr,
                    fromMigrate,
                    kUninitializedStmtId);
    } else {
        repl::logOp(
            opCtx, "i", systemIndexes, {}, indexDoc, nullptr, fromMigrate, kUninitializedStmtId);
    }
    AuthorizationManager::get(opCtx->getServiceContext())
        ->logOp(opCtx, "i", systemIndexes, indexDoc, nullptr);

    auto css = CollectionShardingState::get(opCtx, systemIndexes);
    if (!fromMigrate) {
        css->onInsertOp(opCtx, indexDoc);
    }
}

void OpObserverImpl::onInserts(OperationContext* opCtx,
                               const NamespaceString& nss,
                               OptionalCollectionUUID uuid,
                               std::vector<InsertStatement>::const_iterator begin,
                               std::vector<InsertStatement>::const_iterator end,
                               bool fromMigrate) {
    repl::logInsertOps(opCtx, nss, uuid, begin, end, fromMigrate);

    auto css = CollectionShardingState::get(opCtx, nss.ns());

    for (auto it = begin; it != end; it++) {
        AuthorizationManager::get(opCtx->getServiceContext())
            ->logOp(opCtx, "i", nss, it->doc, nullptr);
        if (!fromMigrate) {
            css->onInsertOp(opCtx, it->doc);
        }
    }

    if (nss.ns() == FeatureCompatibilityVersion::kCollection) {
        for (auto it = begin; it != end; it++) {
            FeatureCompatibilityVersion::onInsertOrUpdate(opCtx, it->doc);
        }
    }

    if (strstr(nss.ns().c_str(), ".system.js")) {
        Scope::storedFuncMod(opCtx);
    }
    if (nss.coll() == DurableViewCatalog::viewsCollectionName()) {
        DurableViewCatalog::onExternalChange(opCtx, nss);
    }
}

void OpObserverImpl::onUpdate(OperationContext* opCtx, const OplogUpdateEntryArgs& args) {
    // Do not log a no-op operation; see SERVER-21738
    if (args.update.isEmpty()) {
        return;
    }

    repl::logOp(opCtx,
                "u",
                args.nss,
                args.uuid,
                args.update,
                &args.criteria,
                args.fromMigrate,
                args.stmtId);
    AuthorizationManager::get(opCtx->getServiceContext())
        ->logOp(opCtx, "u", args.nss, args.update, &args.criteria);

    auto css = CollectionShardingState::get(opCtx, args.nss);
    if (!args.fromMigrate) {
        css->onUpdateOp(opCtx, args.criteria, args.update, args.updatedDoc);
    }

    if (strstr(args.nss.ns().c_str(), ".system.js")) {
        Scope::storedFuncMod(opCtx);
    }

    if (args.nss.coll() == DurableViewCatalog::viewsCollectionName()) {
        DurableViewCatalog::onExternalChange(opCtx, args.nss);
    }

    if (args.nss.ns() == FeatureCompatibilityVersion::kCollection) {
        FeatureCompatibilityVersion::onInsertOrUpdate(opCtx, args.updatedDoc);
    }
}

CollectionShardingState::DeleteState OpObserverImpl::aboutToDelete(OperationContext* opCtx,
                                                                   const NamespaceString& nss,
                                                                   const BSONObj& doc) {
    CollectionShardingState::DeleteState deleteState;
    BSONElement idElement = doc["_id"];
    if (!idElement.eoo()) {
        deleteState.idDoc = idElement.wrap();
    }

    auto css = CollectionShardingState::get(opCtx, nss.ns());
    deleteState.isMigrating = css->isDocumentInMigratingChunk(opCtx, doc);

    return deleteState;
}

void OpObserverImpl::onDelete(OperationContext* opCtx,
                              const NamespaceString& nss,
                              OptionalCollectionUUID uuid,
                              StmtId stmtId,
                              CollectionShardingState::DeleteState deleteState,
                              bool fromMigrate) {
    if (deleteState.idDoc.isEmpty())
        return;

    repl::logOp(opCtx, "d", nss, uuid, deleteState.idDoc, nullptr, fromMigrate, stmtId);
    AuthorizationManager::get(opCtx->getServiceContext())
        ->logOp(opCtx, "d", nss, deleteState.idDoc, nullptr);

    auto css = CollectionShardingState::get(opCtx, nss.ns());
    if (!fromMigrate) {
        css->onDeleteOp(opCtx, deleteState);
    }

    if (nss.coll() == "system.js") {
        Scope::storedFuncMod(opCtx);
    }
    if (nss.coll() == DurableViewCatalog::viewsCollectionName()) {
        DurableViewCatalog::onExternalChange(opCtx, nss);
    }
    if (nss.ns() == FeatureCompatibilityVersion::kCollection) {
        FeatureCompatibilityVersion::onDelete(opCtx, deleteState.idDoc);
    }
}

void OpObserverImpl::onOpMessage(OperationContext* opCtx, const BSONObj& msgObj) {
    repl::logOp(opCtx, "n", {}, {}, msgObj, nullptr, false, kUninitializedStmtId);
}

void OpObserverImpl::onCreateCollection(OperationContext* opCtx,
                                        Collection* coll,
                                        const NamespaceString& collectionName,
                                        const CollectionOptions& options,
                                        const BSONObj& idIndex) {
    const NamespaceString dbName = collectionName.getCommandNS();
    BSONObjBuilder b;
    b.append("create", collectionName.coll().toString());
    {
        // Don't store the UUID as part of the options, but instead only at the top level
        CollectionOptions optionsToStore = options;
        optionsToStore.uuid.reset();
        b.appendElements(optionsToStore.toBSON());
    }

    // Include the full _id index spec in the oplog for index versions >= 2.
    if (!idIndex.isEmpty()) {
        auto versionElem = idIndex[IndexDescriptor::kIndexVersionFieldName];
        invariant(versionElem.isNumber());
        if (IndexDescriptor::IndexVersion::kV2 <=
            static_cast<IndexDescriptor::IndexVersion>(versionElem.numberInt())) {
            b.append("idIndex", idIndex);
        }
    }

    BSONObj cmdObj = b.obj();

    if (!collectionName.isSystemDotProfile()) {
        // do not replicate system.profile modifications
        repl::logOp(opCtx, "c", dbName, options.uuid, cmdObj, nullptr, false, kUninitializedStmtId);
    }

    getGlobalAuthorizationManager()->logOp(opCtx, "c", dbName, cmdObj, nullptr);

    if (options.uuid) {
        UUIDCatalog& catalog = UUIDCatalog::get(opCtx);
        catalog.onCreateCollection(opCtx, coll, options.uuid.get());
        opCtx->recoveryUnit()->onRollback([opCtx, collectionName]() {
            NamespaceUUIDCache::get(opCtx).evictNamespace(collectionName);
        });
    }
}

namespace {
/**
 * Given a raw collMod command object and associated collection metadata, create and return the
 * object for the 'o' field of a collMod oplog entry. For TTL index updates, we make sure the oplog
 * entry always stores the index name, instead of a key pattern.
 */
BSONObj makeCollModCmdObj(const BSONObj& collModCmd,
                          const CollectionOptions& oldCollOptions,
                          boost::optional<TTLCollModInfo> ttlInfo) {
    BSONObjBuilder cmdObjBuilder;
    std::string ttlIndexFieldName = "index";

    // Add all fields from the original collMod command.
    for (auto elem : collModCmd) {
        // We normalize all TTL collMod oplog entry objects to use the index name, even if the
        // command used an index key pattern.
        if (elem.fieldNameStringData() == ttlIndexFieldName && ttlInfo) {
            BSONObjBuilder ttlIndexObjBuilder;
            ttlIndexObjBuilder.append("name", ttlInfo->indexName);
            ttlIndexObjBuilder.append("expireAfterSeconds",
                                      durationCount<Seconds>(ttlInfo->expireAfterSeconds));

            cmdObjBuilder.append(ttlIndexFieldName, ttlIndexObjBuilder.obj());
        } else {
            cmdObjBuilder.append(elem);
        }
    }

    return cmdObjBuilder.obj();
}
}

void OpObserverImpl::onCollMod(OperationContext* opCtx,
                               const NamespaceString& nss,
                               OptionalCollectionUUID uuid,
                               const BSONObj& collModCmd,
                               const CollectionOptions& oldCollOptions,
                               boost::optional<TTLCollModInfo> ttlInfo) {

    const NamespaceString cmdNss = nss.getCommandNS();

    // Create the 'o' field object.
    BSONObj cmdObj = makeCollModCmdObj(collModCmd, oldCollOptions, ttlInfo);

    // Create the 'o2' field object. We save the old collection metadata and TTL expiration.
    BSONObjBuilder o2Builder;
    o2Builder.append("collectionOptions_old", oldCollOptions.toBSON());
    if (ttlInfo) {
        auto oldExpireAfterSeconds = durationCount<Seconds>(ttlInfo->oldExpireAfterSeconds);
        o2Builder.append("expireAfterSeconds_old", oldExpireAfterSeconds);
    }

    const BSONObj o2Obj = o2Builder.obj();

    if (!nss.isSystemDotProfile()) {
        // do not replicate system.profile modifications
        repl::logOp(opCtx, "c", cmdNss, uuid, cmdObj, &o2Obj, false, kUninitializedStmtId);
    }

    getGlobalAuthorizationManager()->logOp(opCtx, "c", cmdNss, cmdObj, nullptr);

    // Make sure the UUID values in the Collection metadata, the Collection object,
    // and the UUID catalog are all present and equal if uuid exists and do not exist
    // if uuid does not exist.
    invariant(opCtx->lockState()->isDbLockedForMode(nss.db(), MODE_X));
    Database* db = dbHolder().get(opCtx, nss.db());
    // Some unit tests call the op observer on an unregistered Database.
    if (!db) {
        return;
    }
    Collection* coll = db->getCollection(opCtx, nss.ns());
    invariant(coll->uuid() == uuid);
    CollectionCatalogEntry* entry = coll->getCatalogEntry();
    invariant(entry->isEqualToMetadataUUID(opCtx, uuid));

    if (uuid) {
        UUIDCatalog& catalog = UUIDCatalog::get(opCtx->getServiceContext());
        Collection* catalogColl = catalog.lookupCollectionByUUID(uuid.get());
        invariant(catalogColl && catalogColl->uuid() == uuid);
    }
}

void OpObserverImpl::onDropDatabase(OperationContext* opCtx, const std::string& dbName) {
    BSONObj cmdObj = BSON("dropDatabase" << 1);
    const NamespaceString cmdNss{dbName, "$cmd"};

    repl::logOp(opCtx, "c", cmdNss, {}, cmdObj, nullptr, false, kUninitializedStmtId);

    if (dbName == FeatureCompatibilityVersion::kDatabase) {
        FeatureCompatibilityVersion::onDropCollection(opCtx);
    }

    NamespaceUUIDCache::get(opCtx).evictNamespacesInDatabase(dbName);

    getGlobalAuthorizationManager()->logOp(opCtx, "c", cmdNss, cmdObj, nullptr);
}

repl::OpTime OpObserverImpl::onDropCollection(OperationContext* opCtx,
                                              const NamespaceString& collectionName,
                                              OptionalCollectionUUID uuid) {
    const NamespaceString dbName = collectionName.getCommandNS();
    BSONObj cmdObj = BSON("drop" << collectionName.coll().toString());

    repl::OpTime dropOpTime;
    if (!collectionName.isSystemDotProfile()) {
        // do not replicate system.profile modifications
        dropOpTime =
            repl::logOp(opCtx, "c", dbName, uuid, cmdObj, nullptr, false, kUninitializedStmtId);
    }

    if (collectionName.coll() == DurableViewCatalog::viewsCollectionName()) {
        DurableViewCatalog::onExternalChange(opCtx, collectionName);
    }

    if (collectionName.ns() == FeatureCompatibilityVersion::kCollection) {
        FeatureCompatibilityVersion::onDropCollection(opCtx);
    }

    getGlobalAuthorizationManager()->logOp(opCtx, "c", dbName, cmdObj, nullptr);

    auto css = CollectionShardingState::get(opCtx, collectionName);
    css->onDropCollection(opCtx, collectionName);

    // Evict namespace entry from the namespace/uuid cache if it exists.
    NamespaceUUIDCache::get(opCtx).evictNamespace(collectionName);

    // Remove collection from the uuid catalog.
    if (uuid) {
        UUIDCatalog& catalog = UUIDCatalog::get(opCtx);
        catalog.onDropCollection(opCtx, uuid.get());
    }

    return dropOpTime;
}

void OpObserverImpl::onDropIndex(OperationContext* opCtx,
                                 const NamespaceString& nss,
                                 OptionalCollectionUUID uuid,
                                 const std::string& indexName,
                                 const BSONObj& indexInfo) {
    BSONObj cmdObj = BSON("dropIndexes" << nss.coll() << "index" << indexName);
    auto commandNS = nss.getCommandNS();
    repl::logOp(opCtx, "c", commandNS, uuid, cmdObj, &indexInfo, false, kUninitializedStmtId);

    getGlobalAuthorizationManager()->logOp(opCtx, "c", commandNS, cmdObj, &indexInfo);
}

void OpObserverImpl::onRenameCollection(OperationContext* opCtx,
                                        const NamespaceString& fromCollection,
                                        const NamespaceString& toCollection,
                                        OptionalCollectionUUID uuid,
                                        bool dropTarget,
                                        OptionalCollectionUUID dropTargetUUID,
                                        OptionalCollectionUUID dropSourceUUID,
                                        bool stayTemp) {
    const NamespaceString cmdNss = fromCollection.getCommandNS();
    BSONObjBuilder builder;
    builder.append("renameCollection", fromCollection.ns());
    builder.append("to", toCollection.ns());
    builder.append("stayTemp", stayTemp);
    if (dropTargetUUID && enableCollectionUUIDs && !isMasterSlave()) {
        dropTargetUUID->appendToBuilder(&builder, "dropTarget");
    } else {
        builder.append("dropTarget", dropTarget);
    }
    if (dropSourceUUID && enableCollectionUUIDs && !isMasterSlave()) {
        dropSourceUUID->appendToBuilder(&builder, "dropSource");
    }
    BSONObj cmdObj = builder.done();

    repl::logOp(opCtx, "c", cmdNss, uuid, cmdObj, nullptr, false, kUninitializedStmtId);
    if (fromCollection.coll() == DurableViewCatalog::viewsCollectionName() ||
        toCollection.coll() == DurableViewCatalog::viewsCollectionName()) {
        DurableViewCatalog::onExternalChange(
            opCtx, NamespaceString(DurableViewCatalog::viewsCollectionName()));
    }

    getGlobalAuthorizationManager()->logOp(opCtx, "c", cmdNss, cmdObj, nullptr);

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

void OpObserverImpl::onApplyOps(OperationContext* opCtx,
                                const std::string& dbName,
                                const BSONObj& applyOpCmd) {
    const NamespaceString cmdNss{dbName, "$cmd"};
    repl::logOp(opCtx, "c", cmdNss, {}, applyOpCmd, nullptr, false, kUninitializedStmtId);

    getGlobalAuthorizationManager()->logOp(opCtx, "c", cmdNss, applyOpCmd, nullptr);
}

void OpObserverImpl::onEmptyCapped(OperationContext* opCtx,
                                   const NamespaceString& collectionName,
                                   OptionalCollectionUUID uuid) {
    const NamespaceString cmdNss = collectionName.getCommandNS();
    BSONObj cmdObj = BSON("emptycapped" << collectionName.coll());

    if (!collectionName.isSystemDotProfile()) {
        // do not replicate system.profile modifications
        repl::logOp(opCtx, "c", cmdNss, uuid, cmdObj, nullptr, false, kUninitializedStmtId);
    }

    getGlobalAuthorizationManager()->logOp(opCtx, "c", cmdNss, cmdObj, nullptr);
}

}  // namespace mongo
