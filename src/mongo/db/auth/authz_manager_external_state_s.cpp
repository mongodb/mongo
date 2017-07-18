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
#include "mongo/db/auth/user_document_parser.h"
#include "mongo/db/auth/user_management_commands_parser.h"
#include "mongo/db/auth/user_name.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/operation_context.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/s/catalog/sharding_catalog_client.h"
#include "mongo/s/grid.h"
#include "mongo/stdx/memory.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/stringutils.h"

namespace mongo {

namespace {

/**
 * Returns the top level field which is expected to be returned by rolesInfo.
 */
std::string rolesFieldName(PrivilegeFormat showPrivileges) {
    if (showPrivileges == PrivilegeFormat::kShowAsUserFragment) {
        return "userFragment";
    }
    return "roles";
}

/**
 * Attches a string representation of a PrivilegeFormat to the provided BSONObjBuilder.
 */
void addShowToBuilder(BSONObjBuilder* builder,
                      PrivilegeFormat showPrivileges,
                      AuthenticationRestrictionsFormat showRestrictions) {
    if (showPrivileges == PrivilegeFormat::kShowAsUserFragment) {
        builder->append("showPrivileges", "asUserfragment");
    } else {
        builder->append("showPrivileges", showPrivileges == PrivilegeFormat::kShowSeparate);
        builder->append("showAuthenticationRestrictions",
                        showRestrictions == AuthenticationRestrictionsFormat::kShow);
    }
}

}  // namespace

AuthzManagerExternalStateMongos::AuthzManagerExternalStateMongos() = default;

AuthzManagerExternalStateMongos::~AuthzManagerExternalStateMongos() = default;

Status AuthzManagerExternalStateMongos::initialize(OperationContext* opCtx) {
    return Status::OK();
}

std::unique_ptr<AuthzSessionExternalState>
AuthzManagerExternalStateMongos::makeAuthzSessionExternalState(AuthorizationManager* authzManager) {
    return stdx::make_unique<AuthzSessionExternalStateMongos>(authzManager);
}

Status AuthzManagerExternalStateMongos::getStoredAuthorizationVersion(OperationContext* opCtx,
                                                                      int* outVersion) {
    // Note: we are treating
    // { 'getParameter' : 1, <authSchemaVersionServerParameter> : 1 }
    // as a user management command since this is the *only* part of mongos
    // that runs this command
    BSONObj getParameterCmd = BSON("getParameter" << 1 << authSchemaVersionServerParameter << 1);
    BSONObjBuilder builder;
    const bool ok = Grid::get(opCtx)->catalogClient()->runUserManagementReadCommand(
        opCtx, "admin", getParameterCmd, &builder);
    BSONObj cmdResult = builder.obj();
    if (!ok) {
        return getStatusFromCommandResult(cmdResult);
    }

    BSONElement versionElement = cmdResult[authSchemaVersionServerParameter];
    if (versionElement.eoo()) {
        return Status(ErrorCodes::UnknownError, "getParameter misbehaved.");
    }
    *outVersion = versionElement.numberInt();

    return Status::OK();
}

Status AuthzManagerExternalStateMongos::getUserDescription(OperationContext* opCtx,
                                                           const UserName& userName,
                                                           BSONObj* result) {
    if (!shouldUseRolesFromConnection(opCtx, userName)) {
        BSONObj usersInfoCmd =
            BSON("usersInfo" << BSON_ARRAY(BSON(AuthorizationManager::USER_NAME_FIELD_NAME
                                                << userName.getUser()
                                                << AuthorizationManager::USER_DB_FIELD_NAME
                                                << userName.getDB()))
                             << "showPrivileges"
                             << true
                             << "showCredentials"
                             << true);
        BSONObjBuilder builder;
        const bool ok = Grid::get(opCtx)->catalogClient()->runUserManagementReadCommand(
            opCtx, "admin", usersInfoCmd, &builder);
        BSONObj cmdResult = builder.obj();
        if (!ok) {
            return getStatusFromCommandResult(cmdResult);
        }

        std::vector<BSONElement> foundUsers = cmdResult["users"].Array();
        if (foundUsers.size() == 0) {
            return Status(ErrorCodes::UserNotFound,
                          "User \"" + userName.toString() + "\" not found");
        }

        if (foundUsers.size() > 1) {
            return Status(ErrorCodes::UserDataInconsistent,
                          str::stream() << "Found multiple users on the \"" << userName.getDB()
                                        << "\" database with name \""
                                        << userName.getUser()
                                        << "\"");
        }
        *result = foundUsers[0].Obj().getOwned();
        return Status::OK();
    } else {
        // Obtain privilege information from the config servers for all roles acquired from the X509
        // certificate.
        BSONArrayBuilder userRolesBuilder;
        auto& sslPeerInfo = SSLPeerInfo::forSession(opCtx->getClient()->session());
        for (const RoleName& role : sslPeerInfo.roles) {
            userRolesBuilder.append(BSON(AuthorizationManager::ROLE_NAME_FIELD_NAME
                                         << role.getRole()
                                         << AuthorizationManager::ROLE_DB_FIELD_NAME
                                         << role.getDB()));
        }
        BSONArray providedRoles = userRolesBuilder.arr();

        BSONObj rolesInfoCmd = BSON("rolesInfo" << providedRoles << "showPrivileges"
                                                << "asUserFragment");

        BSONObjBuilder cmdResultBuilder;
        const bool cmdOk = Grid::get(opCtx)->catalogClient()->runUserManagementReadCommand(
            opCtx, "admin", rolesInfoCmd, &cmdResultBuilder);
        BSONObj cmdResult = cmdResultBuilder.obj();
        if (!cmdOk || !cmdResult["userFragment"].ok()) {
            return Status(ErrorCodes::FailedToParse,
                          "Unable to get resolved X509 roles from config server: " +
                              getStatusFromCommandResult(cmdResult).toString());
        }
        cmdResult = cmdResult["userFragment"].Obj().getOwned();
        BSONElement userRoles = cmdResult["roles"];
        BSONElement userInheritedRoles = cmdResult["inheritedRoles"];
        BSONElement userInheritedPrivileges = cmdResult["inheritedPrivileges"];

        if (userRoles.eoo() || userInheritedRoles.eoo() || userInheritedPrivileges.eoo() ||
            !userRoles.isABSONObj() || !userInheritedRoles.isABSONObj() ||
            !userInheritedPrivileges.isABSONObj()) {
            return Status(
                ErrorCodes::UserDataInconsistent,
                "Recieved malformed response to request for X509 roles from config server");
        }

        *result = BSON("_id" << userName.getUser() << "user" << userName.getUser() << "db"
                             << userName.getDB()
                             << "credentials"
                             << BSON("external" << true)
                             << "roles"
                             << BSONArray(cmdResult["roles"].Obj())
                             << "inheritedRoles"
                             << BSONArray(cmdResult["inheritedRoles"].Obj())
                             << "inheritedPrivileges"
                             << BSONArray(cmdResult["inheritedPrivileges"].Obj()));
        return Status::OK();
    }
}

Status AuthzManagerExternalStateMongos::getRoleDescription(
    OperationContext* opCtx,
    const RoleName& roleName,
    PrivilegeFormat showPrivileges,
    AuthenticationRestrictionsFormat showRestrictions,
    BSONObj* result) {
    BSONObjBuilder rolesInfoCmd;
    rolesInfoCmd.append("rolesInfo",
                        BSON_ARRAY(BSON(AuthorizationManager::ROLE_NAME_FIELD_NAME
                                        << roleName.getRole()
                                        << AuthorizationManager::ROLE_DB_FIELD_NAME
                                        << roleName.getDB())));
    addShowToBuilder(&rolesInfoCmd, showPrivileges, showRestrictions);

    BSONObjBuilder builder;
    const bool ok = Grid::get(opCtx)->catalogClient()->runUserManagementReadCommand(
        opCtx, "admin", rolesInfoCmd.obj(), &builder);
    BSONObj cmdResult = builder.obj();
    if (!ok) {
        return getStatusFromCommandResult(cmdResult);
    }

    std::vector<BSONElement> foundRoles = cmdResult[rolesFieldName(showPrivileges)].Array();
    if (foundRoles.size() == 0) {
        return Status(ErrorCodes::RoleNotFound, "Role \"" + roleName.toString() + "\" not found");
    }

    if (foundRoles.size() > 1) {
        return Status(ErrorCodes::RoleDataInconsistent,
                      str::stream() << "Found multiple roles on the \"" << roleName.getDB()
                                    << "\" database with name \""
                                    << roleName.getRole()
                                    << "\"");
    }
    *result = foundRoles[0].Obj().getOwned();
    return Status::OK();
}
Status AuthzManagerExternalStateMongos::getRolesDescription(
    OperationContext* opCtx,
    const std::vector<RoleName>& roles,
    PrivilegeFormat showPrivileges,
    AuthenticationRestrictionsFormat showRestrictions,
    BSONObj* result) {
    BSONArrayBuilder rolesInfoCmdArray;

    for (const RoleName& roleName : roles) {
        rolesInfoCmdArray << BSON(AuthorizationManager::ROLE_NAME_FIELD_NAME
                                  << roleName.getRole()
                                  << AuthorizationManager::ROLE_DB_FIELD_NAME
                                  << roleName.getDB());
    }

    BSONObjBuilder rolesInfoCmd;
    rolesInfoCmd.append("rolesInfo", rolesInfoCmdArray.arr());
    addShowToBuilder(&rolesInfoCmd, showPrivileges, showRestrictions);

    BSONObjBuilder builder;
    const bool ok = Grid::get(opCtx)->catalogClient()->runUserManagementReadCommand(
        opCtx, "admin", rolesInfoCmd.obj(), &builder);
    BSONObj cmdResult = builder.obj();
    if (!ok) {
        return getStatusFromCommandResult(cmdResult);
    }

    std::vector<BSONElement> foundRoles = cmdResult[rolesFieldName(showPrivileges)].Array();
    if (foundRoles.size() == 0) {
        return Status(ErrorCodes::RoleNotFound, "Roles not found");
    }

    *result = foundRoles[0].Obj().getOwned();

    return Status::OK();
}
Status AuthzManagerExternalStateMongos::getRoleDescriptionsForDB(
    OperationContext* opCtx,
    const std::string& dbname,
    PrivilegeFormat showPrivileges,
    AuthenticationRestrictionsFormat showRestrictions,
    bool showBuiltinRoles,
    std::vector<BSONObj>* result) {
    BSONObjBuilder rolesInfoCmd;
    rolesInfoCmd << "rolesInfo" << 1 << "showBuiltinRoles" << showBuiltinRoles;
    addShowToBuilder(&rolesInfoCmd, showPrivileges, showRestrictions);

    BSONObjBuilder builder;
    const bool ok = Grid::get(opCtx)->catalogClient()->runUserManagementReadCommand(
        opCtx, dbname, rolesInfoCmd.obj(), &builder);
    BSONObj cmdResult = builder.obj();
    if (!ok) {
        return getStatusFromCommandResult(cmdResult);
    }

    for (BSONObjIterator it(cmdResult[rolesFieldName(showPrivileges)].Obj()); it.more();
         it.next()) {
        result->push_back((*it).Obj().getOwned());
    }
    return Status::OK();
}

bool AuthzManagerExternalStateMongos::hasAnyPrivilegeDocuments(OperationContext* opCtx) {
    BSONObj usersInfoCmd = BSON("usersInfo" << 1);
    BSONObjBuilder userBuilder;
    bool ok = Grid::get(opCtx)->catalogClient()->runUserManagementReadCommand(
        opCtx, "admin", usersInfoCmd, &userBuilder);
    if (!ok) {
        // If we were unable to complete the query,
        // it's best to assume that there _are_ privilege documents.  This might happen
        // if the node contaning the users collection becomes transiently unavailable.
        // See SERVER-12616, for example.
        return true;
    }

    BSONObj cmdResult = userBuilder.obj();
    std::vector<BSONElement> foundUsers = cmdResult["users"].Array();
    if (foundUsers.size() > 0) {
        return true;
    }

    BSONObj rolesInfoCmd = BSON("rolesInfo" << 1);
    BSONObjBuilder roleBuilder;
    ok = Grid::get(opCtx)->catalogClient()->runUserManagementReadCommand(
        opCtx, "admin", rolesInfoCmd, &roleBuilder);
    if (!ok) {
        return true;
    }
    cmdResult = roleBuilder.obj();
    std::vector<BSONElement> foundRoles = cmdResult["roles"].Array();
    return foundRoles.size() > 0;
}

}  // namespace mongo
