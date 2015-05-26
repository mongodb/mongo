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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kAccessControl

#include "mongo/platform/basic.h"

#include "mongo/db/auth/authz_manager_external_state_s.h"

#include <string>
#include <vector>

#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/authorization_manager_global.h"
#include "mongo/db/auth/authz_session_external_state_s.h"
#include "mongo/db/auth/user_name.h"
#include "mongo/db/commands.h"
#include "mongo/db/jsobj.h"
#include "mongo/s/catalog/catalog_manager.h"
#include "mongo/s/config.h"
#include "mongo/s/grid.h"
#include "mongo/stdx/memory.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

    AuthzManagerExternalStateMongos::AuthzManagerExternalStateMongos() = default;

    AuthzManagerExternalStateMongos::~AuthzManagerExternalStateMongos() = default;

    Status AuthzManagerExternalStateMongos::initialize(OperationContext* txn) {
        return Status::OK();
    }

    std::unique_ptr<AuthzSessionExternalState>
    AuthzManagerExternalStateMongos::makeAuthzSessionExternalState(
            AuthorizationManager* authzManager) {

        return stdx::make_unique<AuthzSessionExternalStateMongos>(authzManager);
    }

    Status AuthzManagerExternalStateMongos::getStoredAuthorizationVersion(
                                                OperationContext* txn, int* outVersion) {
        // Note: we are treating
        // { 'getParameter' : 1, <authSchemaVersionServerParameter> : 1 }
        // as a user management command since this is the *only* part of mongos
        // that runs this command
        BSONObj getParameterCmd = BSON("getParameter" << 1 <<
                                       authSchemaVersionServerParameter << 1);
        BSONObjBuilder builder;
        const bool ok = grid.catalogManager()->runUserManagementReadCommand("admin",
                                                                            getParameterCmd,
                                                                            &builder);
        BSONObj cmdResult = builder.obj();
        if (!ok) {
            return Command::getStatusFromCommandResult(cmdResult);
        }

        BSONElement versionElement = cmdResult[authSchemaVersionServerParameter];
        if (versionElement.eoo()) {
            return Status(ErrorCodes::UnknownError, "getParameter misbehaved.");
        }
        *outVersion = versionElement.numberInt();

        return Status::OK();
    }

    Status AuthzManagerExternalStateMongos::getUserDescription(
                    OperationContext* txn, const UserName& userName, BSONObj* result) {
        BSONObj usersInfoCmd = BSON("usersInfo" <<
                                    BSON_ARRAY(BSON(AuthorizationManager::USER_NAME_FIELD_NAME <<
                                                    userName.getUser() <<
                                                    AuthorizationManager::USER_DB_FIELD_NAME <<
                                                    userName.getDB())) <<
                                    "showPrivileges" << true <<
                                    "showCredentials" << true);
        BSONObjBuilder builder;
        const bool ok = grid.catalogManager()->runUserManagementReadCommand("admin",
                                                                            usersInfoCmd,
                                                                            &builder);
        BSONObj cmdResult = builder.obj();
        if (!ok) {
            return Command::getStatusFromCommandResult(cmdResult);
        }

        std::vector<BSONElement> foundUsers = cmdResult["users"].Array();
        if (foundUsers.size() == 0) {
            return Status(ErrorCodes::UserNotFound,
                          "User \"" + userName.toString() + "\" not found");
        }

        if (foundUsers.size() > 1) {
            return Status(ErrorCodes::UserDataInconsistent,
                          str::stream() << "Found multiple users on the \""
                                        << userName.getDB() << "\" database with name \""
                                        << userName.getUser() << "\"");
        }
        *result = foundUsers[0].Obj().getOwned();
        return Status::OK();
    }

    Status AuthzManagerExternalStateMongos::getRoleDescription(const RoleName& roleName,
                                                               bool showPrivileges,
                                                               BSONObj* result) {
        BSONObj rolesInfoCmd = BSON("rolesInfo" <<
                                    BSON_ARRAY(BSON(AuthorizationManager::ROLE_NAME_FIELD_NAME <<
                                                    roleName.getRole() <<
                                                    AuthorizationManager::ROLE_DB_FIELD_NAME <<
                                                    roleName.getDB())) <<
                                    "showPrivileges" << showPrivileges);
        BSONObjBuilder builder;
        const bool ok = grid.catalogManager()->runUserManagementReadCommand("admin",
                                                                           rolesInfoCmd,
                                                                           &builder);
        BSONObj cmdResult = builder.obj();
        if (!ok) {
            return Command::getStatusFromCommandResult(cmdResult);
        }

        std::vector<BSONElement> foundRoles = cmdResult["roles"].Array();
        if (foundRoles.size() == 0) {
            return Status(ErrorCodes::RoleNotFound,
                          "Role \"" + roleName.toString() + "\" not found");
        }

        if (foundRoles.size() > 1) {
            return Status(ErrorCodes::RoleDataInconsistent,
                          str::stream() << "Found multiple roles on the \""
                                        << roleName.getDB() << "\" database with name \""
                                        << roleName.getRole() << "\"");
        }
        *result = foundRoles[0].Obj().getOwned();
        return Status::OK();
    }

    Status AuthzManagerExternalStateMongos::getRoleDescriptionsForDB(const std::string dbname,
                                                                     bool showPrivileges,
                                                                     bool showBuiltinRoles,
                                                                     std::vector<BSONObj>* result) {
        BSONObj rolesInfoCmd = BSON("rolesInfo" << 1 <<
                                    "showPrivileges" << showPrivileges <<
                                    "showBuiltinRoles" << showBuiltinRoles);
        BSONObjBuilder builder;
        const bool ok = grid.catalogManager()->runUserManagementReadCommand(dbname,
                                                                            rolesInfoCmd,
                                                                            &builder);
        BSONObj cmdResult = builder.obj();
        if (!ok) {
            return Command::getStatusFromCommandResult(cmdResult);
        }
        for (BSONObjIterator it(cmdResult["roles"].Obj()); it.more(); it.next()) {
            result->push_back((*it).Obj().getOwned());
        }
        return Status::OK();
    }

    bool AuthzManagerExternalStateMongos::hasAnyPrivilegeDocuments(OperationContext* txn) {
        BSONObj usersInfoCmd = BSON("usersInfo" << 1);
        BSONObjBuilder builder;
        const bool ok = grid.catalogManager()->runUserManagementReadCommand("admin",
                                                                            usersInfoCmd,
                                                                            &builder);
        if (!ok) {
            // If we were unable to complete the query,
            // it's best to assume that there _are_ privilege documents.  This might happen
            // if the node contaning the users collection becomes transiently unavailable.
            // See SERVER-12616, for example.
            return true;
        }

        BSONObj cmdResult = builder.obj();
        std::vector<BSONElement> foundUsers = cmdResult["users"].Array();
        return foundUsers.size() > 0;
    }

} // namespace mongo
