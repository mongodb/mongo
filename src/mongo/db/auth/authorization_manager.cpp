/**
*    Copyright (C) 2012 10gen Inc.
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
*/

#include "mongo/db/auth/authorization_manager.h"

#include <string>

#include "mongo/base/init.h"
#include "mongo/base/status.h"
#include "mongo/client/dbclientinterface.h"
#include "mongo/db/auth/action_set.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/capability.h"
#include "mongo/db/auth/capability_set.h"
#include "mongo/db/auth/external_state_impl.h"
#include "mongo/db/auth/principal.h"
#include "mongo/db/auth/principal_set.h"
#include "mongo/db/client.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

    namespace {
        Principal specialAdminPrincipal("special");
    }

    // ActionSets for the various system roles.  These ActionSets contain all the actions that
    // a user of each system role is granted.
    ActionSet readRoleActions;
    ActionSet readWriteRoleActions;
    ActionSet userAdminRoleActions;
    ActionSet dbAdminRoleActions;
    ActionSet serverAdminRoleActions;
    ActionSet clusterAdminRoleActions;

    // This sets up the system role ActionSets.  This is what determines what actions each role
    // is authorized to perform
    MONGO_INITIALIZER(AuthorizationSystemRoles)(InitializerContext* context) {
        // Read role
        // TODO: Remove OLD_READ once commands require the proper actions
        readRoleActions.addAction(ActionType::OLD_READ);
        readRoleActions.addAction(ActionType::COLL_STATS);
        readRoleActions.addAction(ActionType::DB_STATS);
        readRoleActions.addAction(ActionType::FIND);

        // Read-write role
        readWriteRoleActions.addAllActionsFromSet(readRoleActions);
        // TODO: Remove OLD_WRITE once commands require the proper actions
        readWriteRoleActions.addAction(ActionType::OLD_WRITE);
        readWriteRoleActions.addAction(ActionType::CONVERT_TO_CAPPED);
        readWriteRoleActions.addAction(ActionType::CREATE_COLLECTION); // TODO: should db admin get this also?
        readWriteRoleActions.addAction(ActionType::DELETE);
        readWriteRoleActions.addAction(ActionType::DROP_COLLECTION);
        readWriteRoleActions.addAction(ActionType::DROP_INDEXES);
        readWriteRoleActions.addAction(ActionType::EMPTYCAPPED);
        readWriteRoleActions.addAction(ActionType::ENSURE_INDEX);
        readWriteRoleActions.addAction(ActionType::INSERT);
        readWriteRoleActions.addAction(ActionType::UPDATE);

        // User admin role
        userAdminRoleActions.addAction(ActionType::USER_ADMIN);

        // DB admin role
        dbAdminRoleActions.addAction(ActionType::CLEAN);
        dbAdminRoleActions.addAction(ActionType::COLL_MOD);
        dbAdminRoleActions.addAction(ActionType::COLL_STATS);
        dbAdminRoleActions.addAction(ActionType::COMPACT);
        dbAdminRoleActions.addAction(ActionType::CONVERT_TO_CAPPED);
        dbAdminRoleActions.addAction(ActionType::DB_STATS);
        dbAdminRoleActions.addAction(ActionType::DROP_COLLECTION);
        dbAdminRoleActions.addAction(ActionType::RE_INDEX); // TODO: Should readWrite have this also? This isn't consistent with ENSURE_INDEX and DROP_INDEXES
        dbAdminRoleActions.addAction(ActionType::RENAME_COLLECTION);
        dbAdminRoleActions.addAction(ActionType::VALIDATE);

        // Server admin role
        serverAdminRoleActions.addAction(ActionType::CLOSE_ALL_DATABASES);
        serverAdminRoleActions.addAction(ActionType::CONN_POOL_STATS);
        serverAdminRoleActions.addAction(ActionType::CONN_POOL_SYNC);
        serverAdminRoleActions.addAction(ActionType::CPU_PROFILER);
        serverAdminRoleActions.addAction(ActionType::CURSOR_INFO);
        serverAdminRoleActions.addAction(ActionType::DIAG_LOGGING);
        serverAdminRoleActions.addAction(ActionType::FSYNC);
        serverAdminRoleActions.addAction(ActionType::GET_CMD_LINE_OPTS);
        serverAdminRoleActions.addAction(ActionType::GET_LOG);
        serverAdminRoleActions.addAction(ActionType::GET_PARAMETER);
        serverAdminRoleActions.addAction(ActionType::GET_SHARD_MAP);
        serverAdminRoleActions.addAction(ActionType::GET_SHARD_VERSION);
        serverAdminRoleActions.addAction(ActionType::HOST_INFO);
        serverAdminRoleActions.addAction(ActionType::LIST_DATABASES);
        serverAdminRoleActions.addAction(ActionType::LOG_ROTATE);
        serverAdminRoleActions.addAction(ActionType::PROFILE);
        serverAdminRoleActions.addAction(ActionType::REPAIR_DATABASE);
        serverAdminRoleActions.addAction(ActionType::REPL_SET_FREEZE);
        serverAdminRoleActions.addAction(ActionType::REPL_SET_GET_STATUS);
        serverAdminRoleActions.addAction(ActionType::REPL_SET_INITIATE);
        serverAdminRoleActions.addAction(ActionType::REPL_SET_MAINTENANCE);
        serverAdminRoleActions.addAction(ActionType::REPL_SET_RECONFIG);
        serverAdminRoleActions.addAction(ActionType::REPL_SET_STEP_DOWN);
        serverAdminRoleActions.addAction(ActionType::REPL_SET_SYNC_FROM);
        serverAdminRoleActions.addAction(ActionType::RESYNC);
        serverAdminRoleActions.addAction(ActionType::SET_PARAMETER);
        serverAdminRoleActions.addAction(ActionType::SHUTDOWN);
        serverAdminRoleActions.addAction(ActionType::TOP);
        serverAdminRoleActions.addAction(ActionType::TOUCH);

        // Cluster admin role
        clusterAdminRoleActions.addAction(ActionType::ADD_SHARD);
        clusterAdminRoleActions.addAction(ActionType::DROP_DATABASE); // TODO: Should there be a CREATE_DATABASE also?
        clusterAdminRoleActions.addAction(ActionType::ENABLE_SHARDING);
        clusterAdminRoleActions.addAction(ActionType::FLUSH_ROUTER_CONFIG);
        clusterAdminRoleActions.addAction(ActionType::LIST_SHARDS);
        clusterAdminRoleActions.addAction(ActionType::MOVE_CHUNK);
        clusterAdminRoleActions.addAction(ActionType::MOVE_PRIMARY);
        clusterAdminRoleActions.addAction(ActionType::NETSTAT);
        clusterAdminRoleActions.addAction(ActionType::REMOVE_SHARD);
        clusterAdminRoleActions.addAction(ActionType::SET_SHARD_VERSION); // TODO: should this be internal?
        clusterAdminRoleActions.addAction(ActionType::SHARD_COLLECTION);
        clusterAdminRoleActions.addAction(ActionType::SHARDING_STATE);
        clusterAdminRoleActions.addAction(ActionType::SPLIT);
        clusterAdminRoleActions.addAction(ActionType::SPLIT_CHUNK);
        clusterAdminRoleActions.addAction(ActionType::SPLIT_VECTOR);
        clusterAdminRoleActions.addAction(ActionType::UNSET_SHARDING);

        return Status::OK();
    }

    AuthorizationManager::AuthorizationManager(ExternalState* externalState) {
        _externalState.reset(externalState);
    }

    AuthorizationManager::~AuthorizationManager(){}

    void AuthorizationManager::addAuthorizedPrincipal(Principal* principal) {
        _authenticatedPrincipals.add(principal);
    }

    Status AuthorizationManager::removeAuthorizedPrincipal(const Principal* principal) {
        return _authenticatedPrincipals.removeByName(principal->getName());
    }

    Status AuthorizationManager::acquireCapability(const Capability& capability) {
        const std::string& userName = capability.getPrincipal()->getName();
        if (!_authenticatedPrincipals.lookup(userName)) {
            return Status(ErrorCodes::UserNotFound,
                          mongoutils::str::stream()
                                  << "No authenticated principle found with name: "
                                  << userName,
                          0);
        }

        _aquiredCapabilities.grantCapability(capability);

        return Status::OK();
    }

    void AuthorizationManager::grantInternalAuthorization() {
        Principal* internalPrincipal = new Principal("__system");
        _authenticatedPrincipals.add(internalPrincipal);
        ActionSet allActions;
        allActions.addAllActions();
        Capability capability("*", internalPrincipal, allActions);
        Status status = acquireCapability(capability);
        verify (status == Status::OK());
    }

    Status AuthorizationManager::getPrivilegeDocument(DBClientBase* conn,
                                                      const std::string& dbname,
                                                      const std::string& userName,
                                                      BSONObj* result) {
        std::string usersNamespace = dbname + ".system.users";

        BSONObj userBSONObj;
        {
            BSONObj query = BSON("user" << userName);
            userBSONObj = conn->findOne(usersNamespace, query, 0, QueryOption_SlaveOk);
            if (userBSONObj.isEmpty()) {
                return Status(ErrorCodes::UserNotFound,
                              mongoutils::str::stream() << "No matching entry in "
                                                        << usersNamespace
                                                        << " found with name: "
                                                        << userName,
                              0);
            }
        }

        *result = userBSONObj.getOwned();
        return Status::OK();
    }

    bool AuthorizationManager::hasPrivilegeDocument(DBClientBase* conn, const std::string& dbname) {
        BSONObj result = conn->findOne(dbname + ".system.users", Query());
        return !result.isEmpty();
    }

    Status AuthorizationManager::buildCapabilitySet(const std::string& dbname,
                                                    Principal* principal,
                                                    const BSONObj& privilegeDocument,
                                                    CapabilitySet* result) {
        if (!privilegeDocument.hasField("privileges")) {
            // Old-style (v2.2 and prior) privilege document
            return _buildCapabilitySetFromOldStylePrivilegeDocument(dbname,
                                                                    principal,
                                                                    privilegeDocument,
                                                                    result);
        }
        else {
            return Status(ErrorCodes::UnsupportedFormat,
                          mongoutils::str::stream() << "Invalid privilege document received when"
                                  "trying to extract capabilities: " << privilegeDocument,
                          0);
        }
    }

    Status AuthorizationManager::_buildCapabilitySetFromOldStylePrivilegeDocument(
            const std::string& dbname,
            Principal* principal,
            const BSONObj& privilegeDocument,
            CapabilitySet* result) {
        if (!(privilegeDocument.hasField("user") && privilegeDocument.hasField("pwd"))) {
            return Status(ErrorCodes::UnsupportedFormat,
                          mongoutils::str::stream() << "Invalid old-style privilege document "
                                  "received when trying to extract capabilities: "
                                   << privilegeDocument,
                          0);
        }

        bool readOnly = false;
        ActionSet actions;
        if (privilegeDocument.hasField("readOnly") && privilegeDocument["readOnly"].trueValue()) {
            actions.addAllActionsFromSet(readRoleActions);
            readOnly = true;
        }
        else {
            actions.addAllActionsFromSet(readWriteRoleActions);
            actions.addAllActionsFromSet(dbAdminRoleActions);
            actions.addAllActionsFromSet(userAdminRoleActions);
        }

        if (dbname == "admin" || dbname == "local") {
            // Make all basic actions available on all databases
            result->grantCapability(Capability("*", principal, actions));
            // Make server and cluster admin actions available on admin database.
            if (!readOnly) {
                actions.addAllActionsFromSet(serverAdminRoleActions);
                actions.addAllActionsFromSet(clusterAdminRoleActions);
            }
        }

        result->grantCapability(Capability(dbname, principal, actions));

        return Status::OK();
    }

    const Principal* AuthorizationManager::checkAuthorization(const std::string& resource,
                                                              ActionType action) const {
        if (_externalState->shouldIgnoreAuthChecks()) {
            return &specialAdminPrincipal;
        }

        const Capability* capability;
        capability = _aquiredCapabilities.getCapabilityForAction(resource, action);
        if (capability) {
            return capability->getPrincipal();
        }
        capability = _aquiredCapabilities.getCapabilityForAction("*", action);
        if (capability) {
            return capability->getPrincipal();
        }

        return NULL; // Not authorized
    }

} // namespace mongo
