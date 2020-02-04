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

#include "mongo/platform/basic.h"

#include "mongo/db/auth/auth_op_observer.h"

#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/catalog/collection_options.h"
#include "mongo/db/op_observer_util.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/db/s/collection_sharding_state.h"

namespace mongo {

namespace {

const auto documentKeyDecoration = OperationContext::declareDecoration<BSONObj>();

}  // namespace

AuthOpObserver::AuthOpObserver() = default;

AuthOpObserver::~AuthOpObserver() = default;

void AuthOpObserver::onInserts(OperationContext* opCtx,
                               const NamespaceString& nss,
                               OptionalCollectionUUID uuid,
                               std::vector<InsertStatement>::const_iterator first,
                               std::vector<InsertStatement>::const_iterator last,
                               bool fromMigrate) {
    for (auto it = first; it != last; it++) {
        AuthorizationManager::get(opCtx->getServiceContext())
            ->logOp(opCtx, "i", nss, it->doc, nullptr);
    }
}

void AuthOpObserver::onUpdate(OperationContext* opCtx, const OplogUpdateEntryArgs& args) {
    if (args.updateArgs.update.isEmpty()) {
        return;
    }
    AuthorizationManager::get(opCtx->getServiceContext())
        ->logOp(opCtx, "u", args.nss, args.updateArgs.update, &args.updateArgs.criteria);
}

BSONObj AuthOpObserver::getDocumentKey(OperationContext* opCtx,
                                       NamespaceString const& nss,
                                       BSONObj const& doc) {
    const auto collDesc =
        CollectionShardingState::get(opCtx, nss)->getCollectionDescription_DEPRECATED();
    return collDesc.extractDocumentKey(doc).getOwned();
}

void AuthOpObserver::aboutToDelete(OperationContext* opCtx,
                                   NamespaceString const& nss,
                                   BSONObj const& doc) {
    documentKeyDecoration(opCtx) = getDocumentKey(opCtx, nss, doc);
}

void AuthOpObserver::onDelete(OperationContext* opCtx,
                              const NamespaceString& nss,
                              OptionalCollectionUUID uuid,
                              StmtId stmtId,
                              bool fromMigrate,
                              const boost::optional<BSONObj>& deletedDoc) {
    auto& documentKey = documentKeyDecoration(opCtx);
    invariant(!documentKey.isEmpty());
    AuthorizationManager::get(opCtx->getServiceContext())
        ->logOp(opCtx, "d", nss, documentKey, nullptr);
}

void AuthOpObserver::onCreateCollection(OperationContext* opCtx,
                                        Collection* coll,
                                        const NamespaceString& collectionName,
                                        const CollectionOptions& options,
                                        const BSONObj& idIndex,
                                        const OplogSlot& createOpTime) {
    const auto cmdNss = collectionName.getCommandNS();

    const auto cmdObj =
        repl::MutableOplogEntry::makeCreateCollCmdObj(collectionName, options, idIndex);

    AuthorizationManager::get(opCtx->getServiceContext())
        ->logOp(opCtx, "c", cmdNss, cmdObj, nullptr);
}

void AuthOpObserver::onCollMod(OperationContext* opCtx,
                               const NamespaceString& nss,
                               OptionalCollectionUUID uuid,
                               const BSONObj& collModCmd,
                               const CollectionOptions& oldCollOptions,
                               boost::optional<IndexCollModInfo> indexInfo) {
    const auto cmdNss = nss.getCommandNS();

    // Create the 'o' field object.
    const auto cmdObj = makeCollModCmdObj(collModCmd, oldCollOptions, indexInfo);

    AuthorizationManager::get(opCtx->getServiceContext())
        ->logOp(opCtx, "c", cmdNss, cmdObj, nullptr);
}

void AuthOpObserver::onDropDatabase(OperationContext* opCtx, const std::string& dbName) {
    const NamespaceString cmdNss{dbName, "$cmd"};
    const auto cmdObj = BSON("dropDatabase" << 1);

    AuthorizationManager::get(opCtx->getServiceContext())
        ->logOp(opCtx, "c", cmdNss, cmdObj, nullptr);
}

repl::OpTime AuthOpObserver::onDropCollection(OperationContext* opCtx,
                                              const NamespaceString& collectionName,
                                              OptionalCollectionUUID uuid,
                                              std::uint64_t numRecords,
                                              const CollectionDropType dropType) {
    const auto cmdNss = collectionName.getCommandNS();
    const auto cmdObj = BSON("drop" << collectionName.coll());

    AuthorizationManager::get(opCtx->getServiceContext())
        ->logOp(opCtx, "c", cmdNss, cmdObj, nullptr);

    return {};
}

void AuthOpObserver::onDropIndex(OperationContext* opCtx,
                                 const NamespaceString& nss,
                                 OptionalCollectionUUID uuid,
                                 const std::string& indexName,
                                 const BSONObj& indexInfo) {
    const auto cmdNss = nss.getCommandNS();
    const auto cmdObj = BSON("dropIndexes" << nss.coll() << "index" << indexName);

    AuthorizationManager::get(opCtx->getServiceContext())
        ->logOp(opCtx, "c", cmdNss, cmdObj, &indexInfo);
}

void AuthOpObserver::postRenameCollection(OperationContext* const opCtx,
                                          const NamespaceString& fromCollection,
                                          const NamespaceString& toCollection,
                                          OptionalCollectionUUID uuid,
                                          OptionalCollectionUUID dropTargetUUID,
                                          bool stayTemp) {
    const auto cmdNss = fromCollection.getCommandNS();

    BSONObjBuilder builder;
    builder.append("renameCollection", fromCollection.ns());
    builder.append("to", toCollection.ns());
    builder.append("stayTemp", stayTemp);
    if (dropTargetUUID) {
        dropTargetUUID->appendToBuilder(&builder, "dropTarget");
    }

    const auto cmdObj = builder.done();

    AuthorizationManager::get(opCtx->getServiceContext())
        ->logOp(opCtx, "c", cmdNss, cmdObj, nullptr);
}

void AuthOpObserver::onRenameCollection(OperationContext* const opCtx,
                                        const NamespaceString& fromCollection,
                                        const NamespaceString& toCollection,
                                        OptionalCollectionUUID uuid,
                                        OptionalCollectionUUID dropTargetUUID,
                                        std::uint64_t numRecords,
                                        bool stayTemp) {
    postRenameCollection(opCtx, fromCollection, toCollection, uuid, dropTargetUUID, stayTemp);
}

void AuthOpObserver::onApplyOps(OperationContext* opCtx,
                                const std::string& dbName,
                                const BSONObj& applyOpCmd) {
    const NamespaceString cmdNss{dbName, "$cmd"};

    AuthorizationManager::get(opCtx->getServiceContext())
        ->logOp(opCtx, "c", cmdNss, applyOpCmd, nullptr);
}

void AuthOpObserver::onEmptyCapped(OperationContext* opCtx,
                                   const NamespaceString& collectionName,
                                   OptionalCollectionUUID uuid) {
    const auto cmdNss = collectionName.getCommandNS();
    const auto cmdObj = BSON("emptycapped" << collectionName.coll());

    AuthorizationManager::get(opCtx->getServiceContext())
        ->logOp(opCtx, "c", cmdNss, cmdObj, nullptr);
}

void AuthOpObserver::onReplicationRollback(OperationContext* opCtx,
                                           const RollbackObserverInfo& rbInfo) {
    // Invalidate any in-memory auth data if necessary.
    const auto& rollbackNamespaces = rbInfo.rollbackNamespaces;
    if (rollbackNamespaces.count(AuthorizationManager::versionCollectionNamespace) == 1 ||
        rollbackNamespaces.count(AuthorizationManager::usersCollectionNamespace) == 1 ||
        rollbackNamespaces.count(AuthorizationManager::rolesCollectionNamespace) == 1) {
        AuthorizationManager::get(opCtx->getServiceContext())->invalidateUserCache(opCtx);
    }
}


}  // namespace mongo
