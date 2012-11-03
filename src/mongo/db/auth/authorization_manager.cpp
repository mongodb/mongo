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
        readRoleActions.addAction(ActionType::oldRead);
        readRoleActions.addAction(ActionType::collStats);
        readRoleActions.addAction(ActionType::dbStats);
        readRoleActions.addAction(ActionType::find);

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
        serverAdminRoleActions.addAction(ActionType::profile);
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
