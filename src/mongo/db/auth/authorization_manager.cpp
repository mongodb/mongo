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
#include "mongo/db/auth/acquired_privilege.h"
#include "mongo/db/auth/action_set.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/auth_external_state_impl.h"
#include "mongo/db/auth/principal.h"
#include "mongo/db/auth/principal_set.h"
#include "mongo/db/auth/privilege_set.h"
#include "mongo/db/client.h"
#include "mongo/db/namespacestring.h"
#include "mongo/db/security_common.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

    AuthInfo::AuthInfo() {
        user = "__system";
    }
    AuthInfo internalSecurity;

    const std::string AuthorizationManager::SERVER_RESOURCE_NAME = "$SERVER";
    const std::string AuthorizationManager::CLUSTER_RESOURCE_NAME = "$CLUSTER";

    namespace {
        Principal specialAdminPrincipal("special");
        const std::string ADMIN_DBNAME = "admin";
        const std::string LOCAL_DBNAME = "local";
        const std::string WILDCARD_DBNAME = "*";
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
        readRoleActions.addAction(ActionType::oldRead);
        readRoleActions.addAction(ActionType::collStats);
        readRoleActions.addAction(ActionType::dbStats);
        readRoleActions.addAction(ActionType::find);
        //TODO: should dbHash go here?

        // Read-write role
        readWriteRoleActions.addAllActionsFromSet(readRoleActions);
        // TODO: Remove OLD_WRITE once commands require the proper actions
        readWriteRoleActions.addAction(ActionType::oldWrite);
        readWriteRoleActions.addAction(ActionType::convertToCapped);
        readWriteRoleActions.addAction(ActionType::createCollection); // TODO: should db admin get this also?
        readWriteRoleActions.addAction(ActionType::dropCollection);
        readWriteRoleActions.addAction(ActionType::dropIndexes);
        readWriteRoleActions.addAction(ActionType::emptycapped);
        readWriteRoleActions.addAction(ActionType::ensureIndex);
        readWriteRoleActions.addAction(ActionType::insert);
        readWriteRoleActions.addAction(ActionType::remove);
        readWriteRoleActions.addAction(ActionType::update);

        // User admin role
        userAdminRoleActions.addAction(ActionType::userAdmin);

        // DB admin role
        dbAdminRoleActions.addAction(ActionType::clean);
        dbAdminRoleActions.addAction(ActionType::collMod);
        dbAdminRoleActions.addAction(ActionType::collStats);
        dbAdminRoleActions.addAction(ActionType::compact);
        dbAdminRoleActions.addAction(ActionType::convertToCapped);
        dbAdminRoleActions.addAction(ActionType::dbStats);
        dbAdminRoleActions.addAction(ActionType::dropCollection);
        dbAdminRoleActions.addAction(ActionType::reIndex); // TODO: Should readWrite have this also? This isn't consistent with ENSURE_INDEX and DROP_INDEXES
        dbAdminRoleActions.addAction(ActionType::renameCollection);
        dbAdminRoleActions.addAction(ActionType::validate);

        // Server admin role
        // TODO: should applyOps go here?
        serverAdminRoleActions.addAction(ActionType::closeAllDatabases);
        serverAdminRoleActions.addAction(ActionType::connPoolStats);
        serverAdminRoleActions.addAction(ActionType::connPoolSync);
        serverAdminRoleActions.addAction(ActionType::cpuProfiler);
        serverAdminRoleActions.addAction(ActionType::cursorInfo);
        serverAdminRoleActions.addAction(ActionType::diagLogging);
        serverAdminRoleActions.addAction(ActionType::fsync);
        serverAdminRoleActions.addAction(ActionType::getCmdLineOpts);
        serverAdminRoleActions.addAction(ActionType::getLog);
        serverAdminRoleActions.addAction(ActionType::getParameter);
        serverAdminRoleActions.addAction(ActionType::getShardMap);
        serverAdminRoleActions.addAction(ActionType::getShardVersion);
        serverAdminRoleActions.addAction(ActionType::hostInfo);
        serverAdminRoleActions.addAction(ActionType::listDatabases);
        serverAdminRoleActions.addAction(ActionType::logRotate);
        serverAdminRoleActions.addAction(ActionType::profile); // TODO: should this be dbAdmin?
        serverAdminRoleActions.addAction(ActionType::repairDatabase);
        serverAdminRoleActions.addAction(ActionType::replSetFreeze);
        serverAdminRoleActions.addAction(ActionType::replSetGetStatus);
        serverAdminRoleActions.addAction(ActionType::replSetInitiate);
        serverAdminRoleActions.addAction(ActionType::replSetMaintenance);
        serverAdminRoleActions.addAction(ActionType::replSetReconfig);
        serverAdminRoleActions.addAction(ActionType::replSetStepDown);
        serverAdminRoleActions.addAction(ActionType::replSetSyncFrom);
        serverAdminRoleActions.addAction(ActionType::resync);
        serverAdminRoleActions.addAction(ActionType::setParameter);
        serverAdminRoleActions.addAction(ActionType::shutdown);
        serverAdminRoleActions.addAction(ActionType::top);
        serverAdminRoleActions.addAction(ActionType::touch);

        // Cluster admin role
        clusterAdminRoleActions.addAction(ActionType::addShard);
        clusterAdminRoleActions.addAction(ActionType::dropDatabase); // TODO: Should there be a CREATE_DATABASE also?
        clusterAdminRoleActions.addAction(ActionType::enableSharding);
        clusterAdminRoleActions.addAction(ActionType::flushRouterConfig);
        clusterAdminRoleActions.addAction(ActionType::listShards);
        clusterAdminRoleActions.addAction(ActionType::moveChunk);
        clusterAdminRoleActions.addAction(ActionType::movePrimary);
        clusterAdminRoleActions.addAction(ActionType::netstat);
        clusterAdminRoleActions.addAction(ActionType::removeShard);
        clusterAdminRoleActions.addAction(ActionType::setShardVersion); // TODO: should this be internal?
        clusterAdminRoleActions.addAction(ActionType::shardCollection);
        clusterAdminRoleActions.addAction(ActionType::shardingState);
        clusterAdminRoleActions.addAction(ActionType::split);
        clusterAdminRoleActions.addAction(ActionType::splitChunk);
        clusterAdminRoleActions.addAction(ActionType::splitVector);
        clusterAdminRoleActions.addAction(ActionType::unsetSharding);

        return Status::OK();
    }

    AuthorizationManager::AuthorizationManager(AuthExternalState* externalState) :
            _initialized(false) {
        _externalState.reset(externalState);
    }

    AuthorizationManager::~AuthorizationManager(){}

    Status AuthorizationManager::initialize(DBClientBase* adminDBConnection) {
        if (_initialized) {
            // This should never happen.
            return Status(ErrorCodes::InternalError,
                          "AuthorizationManager already initialized!",
                          0);
        }
        Status status = _externalState->initialize(adminDBConnection);
        if (status == Status::OK()) {
            _initialized = true;
        }
        return status;
    }

    void AuthorizationManager::addAuthorizedPrincipal(Principal* principal) {
        _authenticatedPrincipals.add(principal);
    }

    Status AuthorizationManager::removeAuthorizedPrincipal(const Principal* principal) {
        return _authenticatedPrincipals.removeByName(principal->getName());
    }

    Principal* AuthorizationManager::lookupPrincipal(const std::string& name) const {
        return _authenticatedPrincipals.lookup(name);
    }

    Status AuthorizationManager::acquirePrivilege(const AcquiredPrivilege& privilege) {
        const std::string& userName = privilege.getPrincipal()->getName();
        if (!_authenticatedPrincipals.lookup(userName)) {
            return Status(ErrorCodes::UserNotFound,
                          mongoutils::str::stream()
                                  << "No authenticated principle found with name: "
                                  << userName,
                          0);
        }

        _acquiredPrivileges.grantPrivilege(privilege);

        return Status::OK();
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

    ActionSet AuthorizationManager::getActionsForOldStyleUser(const std::string& dbname,
                                                              bool readOnly) {
        ActionSet actions;
        if (readOnly) {
            actions.addAllActionsFromSet(readRoleActions);
        }
        else {
            actions.addAllActionsFromSet(readWriteRoleActions);
            actions.addAllActionsFromSet(dbAdminRoleActions);
            actions.addAllActionsFromSet(userAdminRoleActions);

            if (dbname == ADMIN_DBNAME || dbname == LOCAL_DBNAME) {
                actions.addAllActionsFromSet(serverAdminRoleActions);
                actions.addAllActionsFromSet(clusterAdminRoleActions);
            }
        }
        return actions;
    }

    Status AuthorizationManager::acquirePrivilegesFromPrivilegeDocument(
            const std::string& dbname, Principal* principal, const BSONObj& privilegeDocument) {
        if (!_authenticatedPrincipals.lookup(principal->getName())) {
            return Status(ErrorCodes::UserNotFound,
                          mongoutils::str::stream()
                                  << "No authenticated principle found with name: "
                                  << principal->getName(),
                          0);
        }
        if (principal->getName() == internalSecurity.user) {
            // Grant full access to internal user
            ActionSet allActions;
            allActions.addAllActions();
            AcquiredPrivilege privilege(Privilege(WILDCARD_DBNAME, allActions), principal);
            return acquirePrivilege(privilege);
        }
        return buildPrivilegeSet(dbname, principal, privilegeDocument, &_acquiredPrivileges);
    }

    Status AuthorizationManager::buildPrivilegeSet(const std::string& dbname,
                                                   Principal* principal,
                                                   const BSONObj& privilegeDocument,
                                                   PrivilegeSet* result) {
        if (!privilegeDocument.hasField("privileges")) {
            // Old-style (v2.2 and prior) privilege document
            return _buildPrivilegeSetFromOldStylePrivilegeDocument(dbname,
                                                                   principal,
                                                                   privilegeDocument,
                                                                   result);
        }
        else {
            return Status(ErrorCodes::UnsupportedFormat,
                          mongoutils::str::stream() << "Invalid privilege document received when "
                                  "trying to extract privileges: " << privilegeDocument,
                          0);
        }
    }

    Status AuthorizationManager::_buildPrivilegeSetFromOldStylePrivilegeDocument(
            const std::string& dbname,
            Principal* principal,
            const BSONObj& privilegeDocument,
            PrivilegeSet* result) {
        if (!(privilegeDocument.hasField("user") && privilegeDocument.hasField("pwd"))) {
            return Status(ErrorCodes::UnsupportedFormat,
                          mongoutils::str::stream() << "Invalid old-style privilege document "
                                  "received when trying to extract privileges: "
                                   << privilegeDocument,
                          0);
        }
        if (privilegeDocument["user"].str() != principal->getName()) {
            return Status(ErrorCodes::BadValue,
                          mongoutils::str::stream() << "Principal name from privilege document \""
                                  << privilegeDocument["user"].str()
                                  << "\" doesn't match name of provided Principal \""
                                  << principal->getName()
                                  << "\"",
                          0);
        }

        bool readOnly = privilegeDocument.hasField("readOnly") &&
                privilegeDocument["readOnly"].trueValue();
        ActionSet actions = getActionsForOldStyleUser(dbname, readOnly);
        std::string resourceName = (dbname == ADMIN_DBNAME || dbname == LOCAL_DBNAME) ?
                WILDCARD_DBNAME : dbname;
        result->grantPrivilege(AcquiredPrivilege(Privilege(resourceName, actions), principal));

        return Status::OK();
    }

    const Principal* AuthorizationManager::checkAuthorization(const std::string& resource,
                                                              ActionType action) const {
        massert(16470, "AuthorizationManager has not been initialized!", _initialized);

        if (_externalState->shouldIgnoreAuthChecks()) {
            return &specialAdminPrincipal;
        }

        const AcquiredPrivilege* privilege;
        privilege = _acquiredPrivileges.getPrivilegeForAction(nsToDatabase(resource), action);
        if (privilege) {
            return privilege->getPrincipal();
        }
        privilege = _acquiredPrivileges.getPrivilegeForAction(WILDCARD_DBNAME, action);
        if (privilege) {
            return privilege->getPrincipal();
        }

        return NULL; // Not authorized
    }

} // namespace mongo
