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
            actions.addAction(ActionType::READ);
            readOnly = true;
        }
        else {
            actions.addAction(ActionType::READ_WRITE); // TODO: should this also add READ?
            actions.addAction(ActionType::DB_ADMIN);
            actions.addAction(ActionType::USER_ADMIN);
        }

        if (dbname == "admin" || dbname == "local") {
            // Make all basic actions available on all databases
            result->grantCapability(Capability("*", principal, actions));
            // Make server and cluster admin actions available on admin database.
            if (!readOnly) {
                actions.addAction(ActionType::SERVER_ADMIN);
                actions.addAction(ActionType::CLUSTER_ADMIN);
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
