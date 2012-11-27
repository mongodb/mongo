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
#include <vector>

#include "mongo/base/init.h"
#include "mongo/base/status.h"
#include "mongo/db/auth/acquired_privilege.h"
#include "mongo/db/auth/action_set.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/auth_external_state.h"
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
        Principal specialAdminPrincipal("special", "local");
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
    // Can only be performed by internal connections.  Nothing ever explicitly grants these actions,
    // but they're included when calling addAllActions on an ActionSet, which is how internal
    // connections are granted their privileges.
    ActionSet internalActions;

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
        dbAdminRoleActions.addAction(ActionType::profileEnable);
        dbAdminRoleActions.addAction(ActionType::profileRead);
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
        serverAdminRoleActions.addAction(ActionType::inprog);
        serverAdminRoleActions.addAction(ActionType::killop);
        serverAdminRoleActions.addAction(ActionType::listDatabases);
        serverAdminRoleActions.addAction(ActionType::logRotate);
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
        serverAdminRoleActions.addAction(ActionType::unlock);

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

        // Internal commands
        internalActions.addAction(ActionType::handshake);
        internalActions.addAction(ActionType::mapReduceShardedFinish);
        internalActions.addAction(ActionType::replSetElect);
        internalActions.addAction(ActionType::replSetFresh);
        internalActions.addAction(ActionType::replSetGetRBID);
        internalActions.addAction(ActionType::replSetHeartbeat);
        internalActions.addAction(ActionType::writebacklisten);
        internalActions.addAction(ActionType::writeBacksQueued);
        internalActions.addAction(ActionType::_migrateClone);
        internalActions.addAction(ActionType::_recvChunkAbort);
        internalActions.addAction(ActionType::_recvChunkCommit);
        internalActions.addAction(ActionType::_recvChunkStart);
        internalActions.addAction(ActionType::_recvChunkStatus);
        internalActions.addAction(ActionType::_transferMods);

        return Status::OK();
    }

    AuthorizationManager::AuthorizationManager(AuthExternalState* externalState) {
        _externalState.reset(externalState);
    }

    AuthorizationManager::~AuthorizationManager(){}

    void AuthorizationManager::addAuthorizedPrincipal(Principal* principal) {
        _authenticatedPrincipals.add(principal);
    }

    Principal* AuthorizationManager::lookupPrincipal(const std::string& name,
                                                     const std::string& userSource) {
        return _authenticatedPrincipals.lookup(name, userSource);
    }

    void AuthorizationManager::logoutDatabase(const std::string& dbname) {
        Principal* principal = _authenticatedPrincipals.lookupByDBName(dbname);
        _acquiredPrivileges.revokePrivilegesFromPrincipal(principal);
        _authenticatedPrincipals.removeByDBName(dbname);
    }

    Status AuthorizationManager::acquirePrivilege(const AcquiredPrivilege& privilege) {
        const Principal* principal = privilege.getPrincipal();
        if (!_authenticatedPrincipals.lookup(principal->getName(), principal->getDBName())) {
            return Status(ErrorCodes::UserNotFound,
                          mongoutils::str::stream()
                                  << "No authenticated principle found with name: "
                                  << principal->getName()
                                  << " from database "
                                  << principal->getDBName(),
                          0);
        }

        _acquiredPrivileges.grantPrivilege(privilege);

        return Status::OK();
    }

    void AuthorizationManager::grantInternalAuthorization(const std::string& principalName) {
        Principal* principal = new Principal(principalName, "local");
        ActionSet actions;
        actions.addAllActions();
        AcquiredPrivilege privilege(Privilege("*", actions), principal);

        addAuthorizedPrincipal(principal);
        Status status = acquirePrivilege(privilege);
        verify(status.isOK());
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
        if (!_authenticatedPrincipals.lookup(principal->getName(), dbname)) {
            return Status(ErrorCodes::UserNotFound,
                          mongoutils::str::stream()
                                  << "No authenticated principle found with name: "
                                  << principal->getName()
                                  << " from database "
                                  << principal->getDBName(),
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

    const Principal* AuthorizationManager::checkAuthorization(const std::string& resource,
                                                              ActionSet actions) const {

        if (_externalState->shouldIgnoreAuthChecks()) {
            return &specialAdminPrincipal;
        }

        const AcquiredPrivilege* privilege;
        privilege = _acquiredPrivileges.getPrivilegeForActions(nsToDatabase(resource), actions);
        if (privilege) {
            return privilege->getPrincipal();
        }
        privilege = _acquiredPrivileges.getPrivilegeForActions(WILDCARD_DBNAME, actions);
        if (privilege) {
            return privilege->getPrincipal();
        }

        return NULL; // Not authorized
    }

    Status AuthorizationManager::checkAuthForQuery(const std::string& ns) {
        NamespaceString namespaceString(ns);
        verify(!namespaceString.isCommand());
        if (namespaceString.coll == "system.users") {
            if (!checkAuthorization(ns, ActionType::userAdmin)) {
                return Status(ErrorCodes::Unauthorized,
                              mongoutils::str::stream() <<
                                      "unauthorized to read user information for database " <<
                                      namespaceString.db,
                              0);
            }
        }
        else if (namespaceString.coll == "system.profile") {
            if (!checkAuthorization(ns, ActionType::profileRead)) {
                return Status(ErrorCodes::Unauthorized,
                              mongoutils::str::stream() << "unauthorized to read " <<
                                      namespaceString.db << ".system.profile",
                              0);
            }
        }
        else {
            if (!checkAuthorization(ns, ActionType::find)) {
                return Status(ErrorCodes::Unauthorized,
                              mongoutils::str::stream() << "unauthorized for query on " << ns,
                              0);
            }
        }
        return Status::OK();
    }

    Status AuthorizationManager::checkAuthForInsert(const std::string& ns) {
        NamespaceString namespaceString(ns);
        if (namespaceString.coll == "system.users") {
            if (!checkAuthorization(ns, ActionType::userAdmin)) {
                return Status(ErrorCodes::Unauthorized,
                              mongoutils::str::stream() <<
                                      "unauthorized to create user for database " <<
                                      namespaceString.db,
                              0);
            }
        }
        else {
            if (!checkAuthorization(ns, ActionType::insert)) {
                return Status(ErrorCodes::Unauthorized,
                              mongoutils::str::stream() << "unauthorized for insert on " << ns,
                              0);
            }
        }
        return Status::OK();
    }

    Status AuthorizationManager::checkAuthForUpdate(const std::string& ns, bool upsert) {
        NamespaceString namespaceString(ns);
        if (namespaceString.coll == "system.users") {
            if (!checkAuthorization(ns, ActionType::userAdmin)) {
                return Status(ErrorCodes::Unauthorized,
                              mongoutils::str::stream() <<
                                      "not authorized to update user information for database " <<
                                      namespaceString.db,
                              0);
            }
        }
        else {
            if (!checkAuthorization(ns, ActionType::update)) {
                return Status(ErrorCodes::Unauthorized,
                              mongoutils::str::stream() << "not authorized for update on " << ns,
                              0);
            }
            if (upsert && !checkAuthorization(ns, ActionType::insert)) {
                return Status(ErrorCodes::Unauthorized,
                              mongoutils::str::stream() << "not authorized for upsert on " << ns,
                              0);
            }
        }
        return Status::OK();
    }

    Status AuthorizationManager::checkAuthForDelete(const std::string& ns) {
        NamespaceString namespaceString(ns);
        if (namespaceString.coll == "system.users") {
            if (!checkAuthorization(ns, ActionType::userAdmin)) {
                return Status(ErrorCodes::Unauthorized,
                              mongoutils::str::stream() <<
                                      "not authorized to remove user from database " <<
                                      namespaceString.db,
                              0);
            }
        }
        else {
            if (!checkAuthorization(ns, ActionType::remove)) {
                return Status(ErrorCodes::Unauthorized,
                              mongoutils::str::stream() << "not authorized to remove from " << ns,
                              0);
            }
        }
        return Status::OK();
    }

    Status AuthorizationManager::checkAuthForGetMore(const std::string& ns) {
        return checkAuthForQuery(ns);
    }

    Status AuthorizationManager::checkAuthForPrivileges(const vector<Privilege>& privileges) {
        for (std::vector<Privilege>::const_iterator it = privileges.begin();
                it != privileges.end(); ++it) {
            const Privilege& privilege = *it;
            ActionSet actions = privilege.getActions();
            if (!checkAuthorization(privilege.getResource(), privilege.getActions())) {
                return Status(ErrorCodes::Unauthorized, "unauthorized", 0);
            }
        }
        return Status::OK();
    }

} // namespace mongo
