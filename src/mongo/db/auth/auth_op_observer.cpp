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

#include "mongo/db/auth/auth_op_observer.h"

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/audit.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/local_catalog/collection_options.h"
#include "mongo/db/op_observer/op_observer_util.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/db/rss/replicated_storage_service.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/decorable.h"
#include "mongo/util/namespace_string_util.h"

#include <set>
#include <utility>

#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kAccessControl

namespace mongo {

AuthOpObserver::AuthOpObserver() = default;

AuthOpObserver::~AuthOpObserver() = default;

void AuthOpObserver::onInserts(OperationContext* opCtx,
                               const CollectionPtr& coll,
                               std::vector<InsertStatement>::const_iterator first,
                               std::vector<InsertStatement>::const_iterator last,
                               const std::vector<RecordId>& recordIds,
                               std::vector<bool> fromMigrate,
                               bool defaultFromMigrate,
                               OpStateAccumulator* opAccumulator) {
    // This and all below accesses to AuthOpObserver should only happen
    // from a shard context.
    dassert(opCtx->getService()->role().has(ClusterRole::ShardServer));

    for (auto it = first; it != last; it++) {
        audit::logInsertOperation(opCtx->getClient(), coll->ns(), it->doc);
        AuthorizationManager::get(opCtx->getService())
            ->notifyDDLOperation(opCtx, "i", coll->ns(), it->doc, nullptr);
    }
}

void AuthOpObserver::onUpdate(OperationContext* opCtx,
                              const OplogUpdateEntryArgs& args,
                              OpStateAccumulator* opAccumulator) {
    if (args.updateArgs->update.isEmpty()) {
        return;
    }

    audit::logUpdateOperation(opCtx->getClient(), args.coll->ns(), args.updateArgs->updatedDoc);

    dassert(opCtx->getService()->role().has(ClusterRole::ShardServer));
    AuthorizationManager::get(opCtx->getService())
        ->notifyDDLOperation(
            opCtx, "u", args.coll->ns(), args.updateArgs->update, &args.updateArgs->criteria);
}

void AuthOpObserver::onDelete(OperationContext* opCtx,
                              const CollectionPtr& coll,
                              StmtId stmtId,
                              const BSONObj& doc,
                              const DocumentKey& documentKey,
                              const OplogDeleteEntryArgs& args,
                              OpStateAccumulator* opAccumulator) {
    audit::logRemoveOperation(opCtx->getClient(), coll->ns(), doc);
    // Extract the _id field from the document. If it does not have an _id, use the
    // document itself as the _id.
    auto documentId = doc["_id"] ? doc["_id"].wrap() : doc;
    invariant(!documentId.isEmpty());
    dassert(opCtx->getService()->role().has(ClusterRole::ShardServer));
    AuthorizationManager::get(opCtx->getService())
        ->notifyDDLOperation(opCtx, "d", coll->ns(), documentId, nullptr);
}

void AuthOpObserver::onCreateCollection(
    OperationContext* opCtx,
    const NamespaceString& collectionName,
    const CollectionOptions& options,
    const BSONObj& idIndex,
    const OplogSlot& createOpTime,
    const boost::optional<CreateCollCatalogIdentifier>& createCollCatalogIdentifier,
    bool fromMigrate,
    bool isTimeseries) {
    const auto cmdNss = collectionName.getCommandNS();

    const auto cmdObj =
        repl::MutableOplogEntry::makeCreateCollObject(collectionName, options, idIndex);

    BSONObj o2;
    if (createCollCatalogIdentifier.has_value() &&
        shouldReplicateLocalCatalogIdentifers(
            rss::ReplicatedStorageService::get(opCtx).getPersistenceProvider(),
            VersionContext::getDecoration(opCtx))) {
        o2 = repl::MutableOplogEntry::makeCreateCollObject2(
            createCollCatalogIdentifier->catalogId,
            createCollCatalogIdentifier->ident,
            createCollCatalogIdentifier->idIndexIdent,
            createCollCatalogIdentifier->directoryPerDB,
            createCollCatalogIdentifier->directoryForIndexes);
    }

    dassert(opCtx->getService()->role().has(ClusterRole::ShardServer));
    AuthorizationManager::get(opCtx->getService())
        ->notifyDDLOperation(opCtx, "c", cmdNss, cmdObj, &o2);
}

void AuthOpObserver::onCollMod(OperationContext* opCtx,
                               const NamespaceString& nss,
                               const UUID& uuid,
                               const BSONObj& collModCmd,
                               const CollectionOptions& oldCollOptions,
                               boost::optional<IndexCollModInfo> indexInfo,
                               bool isTimeseries) {
    const auto cmdNss = nss.getCommandNS();

    // Create the 'o' field object.
    const auto cmdObj = makeCollModCmdObj(collModCmd, oldCollOptions, indexInfo);

    dassert(opCtx->getService()->role().has(ClusterRole::ShardServer));
    AuthorizationManager::get(opCtx->getService())
        ->notifyDDLOperation(opCtx, "c", cmdNss, cmdObj, nullptr);
}

void AuthOpObserver::onDropDatabase(OperationContext* opCtx,
                                    const DatabaseName& dbName,
                                    bool markFromMigrate) {
    const NamespaceString cmdNss(NamespaceString::makeCommandNamespace(dbName));
    const auto cmdObj = BSON("dropDatabase" << 1);

    invariant(opCtx->getService()->role().has(ClusterRole::ShardServer));
    AuthorizationManager::get(opCtx->getService())
        ->notifyDDLOperation(opCtx, "c", cmdNss, cmdObj, nullptr);
}

repl::OpTime AuthOpObserver::onDropCollection(OperationContext* opCtx,
                                              const NamespaceString& collectionName,
                                              const UUID& uuid,
                                              std::uint64_t numRecords,
                                              bool markFromMigrate,
                                              bool isTimeseries) {
    const auto cmdNss = collectionName.getCommandNS();
    const auto cmdObj = BSON("drop" << collectionName.coll());

    dassert(opCtx->getService()->role().has(ClusterRole::ShardServer));
    AuthorizationManager::get(opCtx->getService())
        ->notifyDDLOperation(opCtx, "c", cmdNss, cmdObj, nullptr);

    return {};
}

void AuthOpObserver::onDropIndex(OperationContext* opCtx,
                                 const NamespaceString& nss,
                                 const UUID& uuid,
                                 const std::string& indexName,
                                 const BSONObj& indexInfo,
                                 bool isTimeseries) {
    const auto cmdNss = nss.getCommandNS();
    const auto cmdObj = BSON("dropIndexes" << nss.coll() << "index" << indexName);

    dassert(opCtx->getService()->role().has(ClusterRole::ShardServer));
    AuthorizationManager::get(opCtx->getService())
        ->notifyDDLOperation(opCtx, "c", cmdNss, cmdObj, &indexInfo);
}

void AuthOpObserver::postRenameCollection(OperationContext* const opCtx,
                                          const NamespaceString& fromCollection,
                                          const NamespaceString& toCollection,
                                          const UUID& uuid,
                                          const boost::optional<UUID>& dropTargetUUID,
                                          bool stayTemp) {
    const auto cmdNss = fromCollection.getCommandNS();
    const auto sc = SerializationContext::stateDefault();
    BSONObjBuilder builder;
    builder.append("renameCollection", NamespaceStringUtil::serialize(fromCollection, sc));
    builder.append("to", NamespaceStringUtil::serialize(toCollection, sc));
    builder.append("stayTemp", stayTemp);
    if (dropTargetUUID) {
        dropTargetUUID->appendToBuilder(&builder, "dropTarget");
    }

    const auto cmdObj = builder.done();

    dassert(opCtx->getService()->role().has(ClusterRole::ShardServer));
    AuthorizationManager::get(opCtx->getService())
        ->notifyDDLOperation(opCtx, "c", cmdNss, cmdObj, nullptr);
}

void AuthOpObserver::onRenameCollection(OperationContext* const opCtx,
                                        const NamespaceString& fromCollection,
                                        const NamespaceString& toCollection,
                                        const UUID& uuid,
                                        const boost::optional<UUID>& dropTargetUUID,
                                        std::uint64_t numRecords,
                                        bool stayTemp,
                                        bool markFromMigrate,
                                        bool isTimeseries) {
    postRenameCollection(opCtx, fromCollection, toCollection, uuid, dropTargetUUID, stayTemp);
}

void AuthOpObserver::onImportCollection(OperationContext* opCtx,
                                        const UUID& importUUID,
                                        const NamespaceString& nss,
                                        long long numRecords,
                                        long long dataSize,
                                        const BSONObj& catalogEntry,
                                        const BSONObj& storageMetadata,
                                        bool isDryRun,
                                        bool isTimeseries) {

    dassert(opCtx->getService()->role().has(ClusterRole::ShardServer));
    AuthorizationManager::get(opCtx->getService())
        ->notifyDDLOperation(opCtx, "m", nss, catalogEntry, &storageMetadata);
}

void AuthOpObserver::onReplicationRollback(OperationContext* opCtx,
                                           const RollbackObserverInfo& rbInfo) {
    // Invalidate any in-memory auth data if necessary.
    const auto& rollbackNamespaces = rbInfo.rollbackNamespaces;
    if (rollbackNamespaces.count(NamespaceString::kServerConfigurationNamespace) == 1 ||
        rollbackNamespaces.count(NamespaceString::kAdminUsersNamespace) == 1 ||
        rollbackNamespaces.count(NamespaceString::kAdminRolesNamespace) == 1) {
        AuthorizationManager::get(opCtx->getService())->invalidateUserCache();
    }
}


}  // namespace mongo
