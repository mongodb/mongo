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

#include "mongo/db/auth/authorization_manager_global.h"
#include "mongo/db/catalog/collection_options.h"
#include "mongo/db/commands/dbhash.h"
#include "mongo/db/commands/feature_compatibility_version.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/s/collection_sharding_state.h"
#include "mongo/db/server_options.h"
#include "mongo/db/views/durable_view_catalog.h"
#include "mongo/scripting/engine.h"

namespace mongo {

void OpObserverImpl::onCreateIndex(OperationContext* opCtx,
                                   const std::string& ns,
                                   BSONObj indexDoc,
                                   bool fromMigrate) {
    repl::logOp(opCtx, "i", ns.c_str(), indexDoc, nullptr, fromMigrate);
    AuthorizationManager::get(opCtx->getServiceContext())
        ->logOp(opCtx, "i", ns.c_str(), indexDoc, nullptr);

    auto css = CollectionShardingState::get(opCtx, ns);
    if (!fromMigrate) {
        css->onInsertOp(opCtx, indexDoc);
    }

    logOpForDbHash(opCtx, ns.c_str());
}

void OpObserverImpl::onInserts(OperationContext* opCtx,
                               const NamespaceString& nss,
                               std::vector<BSONObj>::const_iterator begin,
                               std::vector<BSONObj>::const_iterator end,
                               bool fromMigrate) {
    repl::logOps(opCtx, "i", nss, begin, end, fromMigrate);

    auto css = CollectionShardingState::get(opCtx, nss.ns());
    const char* ns = nss.ns().c_str();

    for (auto it = begin; it != end; it++) {
        AuthorizationManager::get(opCtx->getServiceContext())->logOp(opCtx, "i", ns, *it, nullptr);
        if (!fromMigrate) {
            css->onInsertOp(opCtx, *it);
        }
    }

    if (nss.ns() == FeatureCompatibilityVersion::kCollection) {
        for (auto it = begin; it != end; it++) {
            FeatureCompatibilityVersion::onInsertOrUpdate(*it);
        }
    }

    logOpForDbHash(opCtx, ns);
    if (strstr(ns, ".system.js")) {
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

    repl::logOp(opCtx, "u", args.ns.c_str(), args.update, &args.criteria, args.fromMigrate);
    AuthorizationManager::get(opCtx->getServiceContext())
        ->logOp(opCtx, "u", args.ns.c_str(), args.update, &args.criteria);

    auto css = CollectionShardingState::get(opCtx, args.ns);
    if (!args.fromMigrate) {
        css->onUpdateOp(opCtx, args.updatedDoc);
    }

    logOpForDbHash(opCtx, args.ns.c_str());
    if (strstr(args.ns.c_str(), ".system.js")) {
        Scope::storedFuncMod(opCtx);
    }

    NamespaceString nss(args.ns);
    if (nss.coll() == DurableViewCatalog::viewsCollectionName()) {
        DurableViewCatalog::onExternalChange(opCtx, nss);
    }

    if (args.ns == FeatureCompatibilityVersion::kCollection) {
        FeatureCompatibilityVersion::onInsertOrUpdate(args.updatedDoc);
    }
}

CollectionShardingState::DeleteState OpObserverImpl::aboutToDelete(OperationContext* opCtx,
                                                                   const NamespaceString& ns,
                                                                   const BSONObj& doc) {
    CollectionShardingState::DeleteState deleteState;
    BSONElement idElement = doc["_id"];
    if (!idElement.eoo()) {
        deleteState.idDoc = idElement.wrap();
    }

    auto css = CollectionShardingState::get(opCtx, ns.ns());
    deleteState.isMigrating = css->isDocumentInMigratingChunk(opCtx, doc);

    return deleteState;
}

void OpObserverImpl::onDelete(OperationContext* opCtx,
                              const NamespaceString& ns,
                              CollectionShardingState::DeleteState deleteState,
                              bool fromMigrate) {
    if (deleteState.idDoc.isEmpty())
        return;

    repl::logOp(opCtx, "d", ns.ns().c_str(), deleteState.idDoc, nullptr, fromMigrate);
    AuthorizationManager::get(opCtx->getServiceContext())
        ->logOp(opCtx, "d", ns.ns().c_str(), deleteState.idDoc, nullptr);

    auto css = CollectionShardingState::get(opCtx, ns.ns());
    if (!fromMigrate) {
        css->onDeleteOp(opCtx, deleteState);
    }

    logOpForDbHash(opCtx, ns.ns().c_str());
    if (ns.coll() == "system.js") {
        Scope::storedFuncMod(opCtx);
    }
    if (ns.coll() == DurableViewCatalog::viewsCollectionName()) {
        DurableViewCatalog::onExternalChange(opCtx, ns);
    }
    if (ns.ns() == FeatureCompatibilityVersion::kCollection) {
        FeatureCompatibilityVersion::onDelete(deleteState.idDoc);
    }
}

void OpObserverImpl::onOpMessage(OperationContext* opCtx, const BSONObj& msgObj) {
    repl::logOp(opCtx, "n", "", msgObj, nullptr, false);
}

void OpObserverImpl::onCreateCollection(OperationContext* opCtx,
                                        const NamespaceString& collectionName,
                                        const CollectionOptions& options,
                                        const BSONObj& idIndex) {
    std::string dbName = collectionName.db().toString() + ".$cmd";
    BSONObjBuilder b;
    b.append("create", collectionName.coll().toString());
    b.appendElements(options.toBSON());

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
        repl::logOp(opCtx, "c", dbName.c_str(), cmdObj, nullptr, false);
    }

    getGlobalAuthorizationManager()->logOp(opCtx, "c", dbName.c_str(), cmdObj, nullptr);
    logOpForDbHash(opCtx, dbName.c_str());
}

void OpObserverImpl::onCollMod(OperationContext* opCtx,
                               const std::string& dbName,
                               const BSONObj& collModCmd) {
    BSONElement first = collModCmd.firstElement();
    std::string coll = first.valuestr();

    if (!NamespaceString(NamespaceString(dbName).db(), coll).isSystemDotProfile()) {
        // do not replicate system.profile modifications
        repl::logOp(opCtx, "c", dbName.c_str(), collModCmd, nullptr, false);
    }

    getGlobalAuthorizationManager()->logOp(opCtx, "c", dbName.c_str(), collModCmd, nullptr);
    logOpForDbHash(opCtx, dbName.c_str());
}

void OpObserverImpl::onDropDatabase(OperationContext* opCtx, const std::string& dbName) {
    BSONObj cmdObj = BSON("dropDatabase" << 1);

    repl::logOp(opCtx, "c", dbName.c_str(), cmdObj, nullptr, false);

    if (NamespaceString(dbName).db() == FeatureCompatibilityVersion::kDatabase) {
        FeatureCompatibilityVersion::onDropCollection();
    }

    getGlobalAuthorizationManager()->logOp(opCtx, "c", dbName.c_str(), cmdObj, nullptr);
    logOpForDbHash(opCtx, dbName.c_str());
}

void OpObserverImpl::onDropCollection(OperationContext* opCtx,
                                      const NamespaceString& collectionName) {
    std::string dbName = collectionName.db().toString() + ".$cmd";
    BSONObj cmdObj = BSON("drop" << collectionName.coll().toString());

    if (!collectionName.isSystemDotProfile()) {
        // do not replicate system.profile modifications
        repl::logOp(opCtx, "c", dbName.c_str(), cmdObj, nullptr, false);
    }

    if (collectionName.coll() == DurableViewCatalog::viewsCollectionName()) {
        DurableViewCatalog::onExternalChange(opCtx, collectionName);
    }

    if (collectionName.ns() == FeatureCompatibilityVersion::kCollection) {
        FeatureCompatibilityVersion::onDropCollection();
    }

    getGlobalAuthorizationManager()->logOp(opCtx, "c", dbName.c_str(), cmdObj, nullptr);

    auto css = CollectionShardingState::get(opCtx, collectionName);
    css->onDropCollection(opCtx, collectionName);

    logOpForDbHash(opCtx, dbName.c_str());
}

void OpObserverImpl::onDropIndex(OperationContext* opCtx,
                                 const NamespaceString& ns,
                                 const std::string& indexName,
                                 const BSONObj& indexInfo) {
    BSONObj cmdObj = BSON("dropIndexes" << ns.coll() << "index" << indexName);
    auto commandNS = ns.getCommandNS();
    repl::logOp(opCtx, "c", commandNS.c_str(), cmdObj, &indexInfo, false);

    getGlobalAuthorizationManager()->logOp(opCtx, "c", commandNS.c_str(), cmdObj, &indexInfo);
    logOpForDbHash(opCtx, commandNS.c_str());
}

void OpObserverImpl::onRenameCollection(OperationContext* opCtx,
                                        const NamespaceString& fromCollection,
                                        const NamespaceString& toCollection,
                                        bool dropTarget,
                                        bool stayTemp) {
    std::string dbName = fromCollection.db().toString() + ".$cmd";
    BSONObj cmdObj =
        BSON("renameCollection" << fromCollection.ns() << "to" << toCollection.ns() << "stayTemp"
                                << stayTemp
                                << "dropTarget"
                                << dropTarget);

    repl::logOp(opCtx, "c", dbName.c_str(), cmdObj, nullptr, false);
    if (fromCollection.coll() == DurableViewCatalog::viewsCollectionName() ||
        toCollection.coll() == DurableViewCatalog::viewsCollectionName()) {
        DurableViewCatalog::onExternalChange(
            opCtx, NamespaceString(DurableViewCatalog::viewsCollectionName()));
    }

    getGlobalAuthorizationManager()->logOp(opCtx, "c", dbName.c_str(), cmdObj, nullptr);
    logOpForDbHash(opCtx, dbName.c_str());
}

void OpObserverImpl::onApplyOps(OperationContext* opCtx,
                                const std::string& dbName,
                                const BSONObj& applyOpCmd) {
    repl::logOp(opCtx, "c", dbName.c_str(), applyOpCmd, nullptr, false);

    getGlobalAuthorizationManager()->logOp(opCtx, "c", dbName.c_str(), applyOpCmd, nullptr);
    logOpForDbHash(opCtx, dbName.c_str());
}

void OpObserverImpl::onConvertToCapped(OperationContext* opCtx,
                                       const NamespaceString& collectionName,
                                       double size) {
    std::string dbName = collectionName.db().toString() + ".$cmd";
    BSONObj cmdObj = BSON("convertToCapped" << collectionName.coll() << "size" << size);

    if (!collectionName.isSystemDotProfile()) {
        // do not replicate system.profile modifications
        repl::logOp(opCtx, "c", dbName.c_str(), cmdObj, nullptr, false);
    }

    getGlobalAuthorizationManager()->logOp(opCtx, "c", dbName.c_str(), cmdObj, nullptr);
    logOpForDbHash(opCtx, dbName.c_str());
}

void OpObserverImpl::onEmptyCapped(OperationContext* opCtx, const NamespaceString& collectionName) {
    std::string dbName = collectionName.db().toString() + ".$cmd";
    BSONObj cmdObj = BSON("emptycapped" << collectionName.coll());

    if (!collectionName.isSystemDotProfile()) {
        // do not replicate system.profile modifications
        repl::logOp(opCtx, "c", dbName.c_str(), cmdObj, nullptr, false);
    }

    getGlobalAuthorizationManager()->logOp(opCtx, "c", dbName.c_str(), cmdObj, nullptr);
    logOpForDbHash(opCtx, dbName.c_str());
}

}  // namespace mongo
