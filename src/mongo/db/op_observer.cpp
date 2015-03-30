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
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/global_environment_experiment.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/replication_coordinator_global.h"
#include "mongo/s/d_state.h"
#include "mongo/scripting/engine.h"

namespace mongo {

    void OpObserver::onCreateIndex(OperationContext* txn,
                                   const std::string& ns,
                                   BSONObj indexDoc,
                                   bool fromMigrate) {
        if (repl::getGlobalReplicationCoordinator()->isReplEnabled()) {
            repl::_logOp(txn, "i", ns.c_str(), indexDoc, nullptr, fromMigrate);
        }

        getGlobalAuthorizationManager()->logOp(txn, "i", ns.c_str(), indexDoc, nullptr);
        logOpForSharding(txn, "i", ns.c_str(), indexDoc, nullptr, fromMigrate);
        logOpForDbHash(txn, ns.c_str());
    }

    void OpObserver::onInsert(OperationContext* txn,
                              const std::string& ns,
                              BSONObj doc,
                              bool fromMigrate) {
        if ( repl::getGlobalReplicationCoordinator()->isReplEnabled() ) {
            repl::_logOp(txn, "i", ns.c_str(), doc, nullptr, fromMigrate);
        }

        getGlobalAuthorizationManager()->logOp(txn, "i", ns.c_str(), doc, nullptr);
        logOpForSharding(txn, "i", ns.c_str(), doc, nullptr, fromMigrate);
        logOpForDbHash(txn, ns.c_str());
        if ( strstr( ns.c_str(), ".system.js" ) ) {
            Scope::storedFuncMod(txn);
        }
    }

    void OpObserver::onUpdate(OperationContext* txn,
                              const std::string& ns,
                              const BSONObj& update,
                              BSONObj& criteria,
                              bool fromMigrate) {
        if ( repl::getGlobalReplicationCoordinator()->isReplEnabled() ) {
            repl::_logOp(txn, "u", ns.c_str(), update, &criteria, fromMigrate);
        }

        getGlobalAuthorizationManager()->logOp(txn, "u", ns.c_str(), update, &criteria);
        logOpForSharding(txn, "u", ns.c_str(), update, &criteria, fromMigrate);
        logOpForDbHash(txn, ns.c_str());
        if ( strstr( ns.c_str(), ".system.js" ) ) {
            Scope::storedFuncMod(txn);
        }
    }

    void OpObserver::onDelete(OperationContext* txn,
                              const std::string& ns,
                              const BSONObj& idDoc,
                              bool fromMigrate) {

        if ( repl::getGlobalReplicationCoordinator()->isReplEnabled() ) {
            repl::_logOp(txn, "d", ns.c_str(), idDoc, nullptr, fromMigrate);
        }

        getGlobalAuthorizationManager()->logOp(txn, "d", ns.c_str(), idDoc, nullptr);
        logOpForSharding(txn, "d", ns.c_str(), idDoc, nullptr, fromMigrate);
        logOpForDbHash(txn, ns.c_str());
        if ( strstr( ns.c_str(), ".system.js" ) ) {
            Scope::storedFuncMod(txn);
        }
    }

    void OpObserver::onOpMessage(OperationContext* txn, const BSONObj& msgObj) {
        if ( repl::getGlobalReplicationCoordinator()->isReplEnabled() ) {
            repl::_logOp(txn, "n", "", msgObj, nullptr, false);
        }
    }

    void OpObserver::onCreateCollection(OperationContext* txn,
                                        const NamespaceString& collectionName,
                                        const CollectionOptions& options) {
        std::string dbName = collectionName.db().toString() + ".$cmd";
        BSONObjBuilder b;
        b.append("create", collectionName.coll().toString());
        b.appendElements(options.toBSON());
        BSONObj cmdObj = b.obj();

        if ( repl::getGlobalReplicationCoordinator()->isReplEnabled() ) {
            repl::_logOp(txn, "c", dbName.c_str(), cmdObj, nullptr, false);
        }

        getGlobalAuthorizationManager()->logOp(txn, "c", dbName.c_str(), cmdObj, nullptr);
        logOpForDbHash(txn, dbName.c_str());
    }

    void OpObserver::onCollMod(OperationContext* txn,
                               const std::string& dbName,
                               const BSONObj& collModCmd) {
        if ( repl::getGlobalReplicationCoordinator()->isReplEnabled() ) {
            repl::_logOp(txn, "c", dbName.c_str(), collModCmd, nullptr, false);
        }

        getGlobalAuthorizationManager()->logOp(txn, "c", dbName.c_str(), collModCmd, nullptr);
        logOpForDbHash(txn, dbName.c_str());
    }

    void OpObserver::onDropDatabase(OperationContext* txn,
                                    const std::string& dbName) {
        BSONObj cmdObj = BSON("dropDatabase" << 1);

        if ( repl::getGlobalReplicationCoordinator()->isReplEnabled() ) {
            repl::_logOp(txn, "c", dbName.c_str(), cmdObj, nullptr, false);
        }

        getGlobalAuthorizationManager()->logOp(txn, "c", dbName.c_str(), cmdObj, nullptr);
        logOpForDbHash(txn, dbName.c_str());
    }

    void OpObserver::onDropCollection(OperationContext* txn,
                                      const NamespaceString& collectionName) {
        std::string dbName = collectionName.db().toString() + ".$cmd";
        BSONObj cmdObj = BSON("drop" << collectionName.coll().toString());

        if ( repl::getGlobalReplicationCoordinator()->isReplEnabled() ) {
            repl::_logOp(txn, "c", dbName.c_str(), cmdObj, nullptr, false);
        }

        getGlobalAuthorizationManager()->logOp(txn, "c", dbName.c_str(), cmdObj, nullptr);
        logOpForDbHash(txn, dbName.c_str());
    }

    void OpObserver::onDropIndex(OperationContext* txn,
                                 const std::string& dbName,
                                 const BSONObj& idxDescriptor) {
        if ( repl::getGlobalReplicationCoordinator()->isReplEnabled() ) {
            repl::_logOp(txn, "c", dbName.c_str(), idxDescriptor, nullptr, false);
        }

        getGlobalAuthorizationManager()->logOp(txn, "c", dbName.c_str(), idxDescriptor, nullptr);
        logOpForDbHash(txn, dbName.c_str());
    }

    void OpObserver::onRenameCollection(OperationContext* txn,
                                        const NamespaceString& fromCollection,
                                        const NamespaceString& toCollection,
                                        bool dropTarget,
                                        bool stayTemp) {
        std::string dbName = fromCollection.db().toString() + ".$cmd";
        BSONObj cmdObj = BSON("renameCollection" << fromCollection <<
                              "to" << toCollection <<
                              "stayTemp" << stayTemp <<
                              "dropTarget" << dropTarget);

        if ( repl::getGlobalReplicationCoordinator()->isReplEnabled() ) {
            repl::_logOp(txn, "c", dbName.c_str(), cmdObj, nullptr, false);
        }

        getGlobalAuthorizationManager()->logOp(txn, "c", dbName.c_str(), cmdObj, nullptr);
        logOpForDbHash(txn, dbName.c_str());
    }

    void OpObserver::onApplyOps(OperationContext* txn,
                                const std::string& dbName,
                                const BSONObj& applyOpCmd) {
        if ( repl::getGlobalReplicationCoordinator()->isReplEnabled() ) {
            repl::_logOp(txn, "c", dbName.c_str(), applyOpCmd, nullptr, false);
        }

        getGlobalAuthorizationManager()->logOp(txn, "c", dbName.c_str(), applyOpCmd, nullptr);
        logOpForDbHash(txn, dbName.c_str());
    }

    void OpObserver::onConvertToCapped(OperationContext* txn,
                                       const NamespaceString& collectionName,
                                       double size) {
        std::string dbName = collectionName.db().toString() + ".$cmd";
        BSONObj cmdObj = BSON("convertToCapped" << collectionName.coll() << "size" << size);

        if ( repl::getGlobalReplicationCoordinator()->isReplEnabled() ) {
            repl::_logOp(txn, "c", dbName.c_str(), cmdObj, nullptr, false);
        }

        getGlobalAuthorizationManager()->logOp(txn, "c", dbName.c_str(), cmdObj, nullptr);
        logOpForDbHash(txn, dbName.c_str());
    }

    void OpObserver::onEmptyCapped(OperationContext* txn, const NamespaceString& collectionName) {
        std::string dbName = collectionName.db().toString() + ".$cmd";
        BSONObj cmdObj = BSON("emptycapped" << collectionName.coll());

        if ( repl::getGlobalReplicationCoordinator()->isReplEnabled() ) {
            repl::_logOp(txn, "c", dbName.c_str(), cmdObj, nullptr, false);
        }

        getGlobalAuthorizationManager()->logOp(txn, "c", dbName.c_str(), cmdObj, nullptr);
        logOpForDbHash(txn, dbName.c_str());
    }

} // namespace mongo
