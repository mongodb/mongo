/**
*    Copyright (C) 2008-2014 MongoDB Inc.
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

#include "mongo/db/op_observer.h"

#include "mongo/db/auth/authorization_manager_global.h"
#include "mongo/db/catalog/collection_options.h"
#include "mongo/db/commands/dbhash.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/s/collection_sharding_state.h"
#include "mongo/scripting/engine.h"

namespace mongo {

using std::vector;

void OpObserver::onCreateIndex(OperationContext* txn,
                               const std::string& ns,
                               BSONObj indexDoc,
                               bool fromMigrate) {
    repl::logOp(txn, "i", ns.c_str(), indexDoc, nullptr, fromMigrate);
    AuthorizationManager::get(txn->getServiceContext())
        ->logOp(txn, "i", ns.c_str(), indexDoc, nullptr);

    auto css = CollectionShardingState::get(txn, ns);
    if (!fromMigrate) {
        css->onInsertOp(txn, indexDoc);
    }

    logOpForDbHash(txn, ns.c_str());
}

void OpObserver::onInserts(OperationContext* txn,
                           const NamespaceString& nss,
                           vector<BSONObj>::const_iterator begin,
                           vector<BSONObj>::const_iterator end,
                           bool fromMigrate) {
    repl::logOps(txn, "i", nss, begin, end, fromMigrate);

    auto css = CollectionShardingState::get(txn, nss.ns());
    const char* ns = nss.ns().c_str();

    for (auto it = begin; it != end; it++) {
        AuthorizationManager::get(txn->getServiceContext())->logOp(txn, "i", ns, *it, nullptr);
        if (!fromMigrate) {
            css->onInsertOp(txn, *it);
        }
    }

    logOpForDbHash(txn, ns);
    if (strstr(ns, ".system.js")) {
        Scope::storedFuncMod(txn);
    }
}

void OpObserver::onUpdate(OperationContext* txn, const OplogUpdateEntryArgs& args) {
    // Do not log a no-op operation; see SERVER-21738
    if (args.update.isEmpty()) {
        return;
    }

    repl::logOp(txn, "u", args.ns.c_str(), args.update, &args.criteria, args.fromMigrate);
    AuthorizationManager::get(txn->getServiceContext())
        ->logOp(txn, "u", args.ns.c_str(), args.update, &args.criteria);

    auto css = CollectionShardingState::get(txn, args.ns);
    if (!args.fromMigrate) {
        css->onUpdateOp(txn, args.updatedDoc);
    }

    logOpForDbHash(txn, args.ns.c_str());
    if (strstr(args.ns.c_str(), ".system.js")) {
        Scope::storedFuncMod(txn);
    }
}

OpObserver::DeleteState OpObserver::aboutToDelete(OperationContext* txn,
                                                  const NamespaceString& ns,
                                                  const BSONObj& doc) {
    OpObserver::DeleteState deleteState;
    BSONElement idElement = doc["_id"];
    if (!idElement.eoo()) {
        deleteState.idDoc = idElement.wrap();
    }

    auto css = CollectionShardingState::get(txn, ns.ns());
    deleteState.isMigrating = css->isDocumentInMigratingChunk(txn, doc);

    return deleteState;
}

void OpObserver::onDelete(OperationContext* txn,
                          const NamespaceString& ns,
                          OpObserver::DeleteState deleteState,
                          bool fromMigrate) {
    if (deleteState.idDoc.isEmpty())
        return;

    repl::logOp(txn, "d", ns.ns().c_str(), deleteState.idDoc, nullptr, fromMigrate);
    AuthorizationManager::get(txn->getServiceContext())
        ->logOp(txn, "d", ns.ns().c_str(), deleteState.idDoc, nullptr);

    auto css = CollectionShardingState::get(txn, ns.ns());
    if (!fromMigrate && deleteState.isMigrating) {
        css->onDeleteOp(txn, deleteState.idDoc);
    }

    logOpForDbHash(txn, ns.ns().c_str());
    if (ns.coll() == "system.js") {
        Scope::storedFuncMod(txn);
    }
}

void OpObserver::onOpMessage(OperationContext* txn, const BSONObj& msgObj) {
    repl::logOp(txn, "n", "", msgObj, nullptr, false);
}

void OpObserver::onCreateCollection(OperationContext* txn,
                                    const NamespaceString& collectionName,
                                    const CollectionOptions& options) {
    std::string dbName = collectionName.db().toString() + ".$cmd";
    BSONObjBuilder b;
    b.append("create", collectionName.coll().toString());
    b.appendElements(options.toBSON());
    BSONObj cmdObj = b.obj();

    if (!collectionName.isSystemDotProfile()) {
        // do not replicate system.profile modifications
        repl::logOp(txn, "c", dbName.c_str(), cmdObj, nullptr, false);
    }

    getGlobalAuthorizationManager()->logOp(txn, "c", dbName.c_str(), cmdObj, nullptr);
    logOpForDbHash(txn, dbName.c_str());
}

void OpObserver::onCollMod(OperationContext* txn,
                           const std::string& dbName,
                           const BSONObj& collModCmd) {
    BSONElement first = collModCmd.firstElement();
    std::string coll = first.valuestr();

    if (!NamespaceString(NamespaceString(dbName).db(), coll).isSystemDotProfile()) {
        // do not replicate system.profile modifications
        repl::logOp(txn, "c", dbName.c_str(), collModCmd, nullptr, false);
    }

    getGlobalAuthorizationManager()->logOp(txn, "c", dbName.c_str(), collModCmd, nullptr);
    logOpForDbHash(txn, dbName.c_str());
}

void OpObserver::onDropDatabase(OperationContext* txn, const std::string& dbName) {
    BSONObj cmdObj = BSON("dropDatabase" << 1);

    repl::logOp(txn, "c", dbName.c_str(), cmdObj, nullptr, false);

    getGlobalAuthorizationManager()->logOp(txn, "c", dbName.c_str(), cmdObj, nullptr);
    logOpForDbHash(txn, dbName.c_str());
}

void OpObserver::onDropCollection(OperationContext* txn, const NamespaceString& collectionName) {
    std::string dbName = collectionName.db().toString() + ".$cmd";
    BSONObj cmdObj = BSON("drop" << collectionName.coll().toString());

    if (!collectionName.isSystemDotProfile()) {
        // do not replicate system.profile modifications
        repl::logOp(txn, "c", dbName.c_str(), cmdObj, nullptr, false);
    }

    getGlobalAuthorizationManager()->logOp(txn, "c", dbName.c_str(), cmdObj, nullptr);
    logOpForDbHash(txn, dbName.c_str());
}

void OpObserver::onDropIndex(OperationContext* txn,
                             const std::string& dbName,
                             const BSONObj& idxDescriptor) {
    repl::logOp(txn, "c", dbName.c_str(), idxDescriptor, nullptr, false);

    getGlobalAuthorizationManager()->logOp(txn, "c", dbName.c_str(), idxDescriptor, nullptr);
    logOpForDbHash(txn, dbName.c_str());
}

void OpObserver::onRenameCollection(OperationContext* txn,
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

    repl::logOp(txn, "c", dbName.c_str(), cmdObj, nullptr, false);

    getGlobalAuthorizationManager()->logOp(txn, "c", dbName.c_str(), cmdObj, nullptr);
    logOpForDbHash(txn, dbName.c_str());
}

void OpObserver::onApplyOps(OperationContext* txn,
                            const std::string& dbName,
                            const BSONObj& applyOpCmd) {
    repl::logOp(txn, "c", dbName.c_str(), applyOpCmd, nullptr, false);

    getGlobalAuthorizationManager()->logOp(txn, "c", dbName.c_str(), applyOpCmd, nullptr);
    logOpForDbHash(txn, dbName.c_str());
}

void OpObserver::onConvertToCapped(OperationContext* txn,
                                   const NamespaceString& collectionName,
                                   double size) {
    std::string dbName = collectionName.db().toString() + ".$cmd";
    BSONObj cmdObj = BSON("convertToCapped" << collectionName.coll() << "size" << size);

    if (!collectionName.isSystemDotProfile()) {
        // do not replicate system.profile modifications
        repl::logOp(txn, "c", dbName.c_str(), cmdObj, nullptr, false);
    }

    getGlobalAuthorizationManager()->logOp(txn, "c", dbName.c_str(), cmdObj, nullptr);
    logOpForDbHash(txn, dbName.c_str());
}

void OpObserver::onEmptyCapped(OperationContext* txn, const NamespaceString& collectionName) {
    std::string dbName = collectionName.db().toString() + ".$cmd";
    BSONObj cmdObj = BSON("emptycapped" << collectionName.coll());

    if (!collectionName.isSystemDotProfile()) {
        // do not replicate system.profile modifications
        repl::logOp(txn, "c", dbName.c_str(), cmdObj, nullptr, false);
    }

    getGlobalAuthorizationManager()->logOp(txn, "c", dbName.c_str(), cmdObj, nullptr);
    logOpForDbHash(txn, dbName.c_str());
}

}  // namespace mongo
