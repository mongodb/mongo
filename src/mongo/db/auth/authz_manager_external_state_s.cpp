/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/db/auth/authz_manager_external_state_s.h"

#include <string>
#include <vector>

#include "mongo/base/shim.h"
#include "mongo/db/auth/authz_session_external_state_s.h"
#include "mongo/db/auth/user_document_parser.h"
#include "mongo/db/auth/user_management_commands_parser.h"
#include "mongo/db/auth/user_name.h"
#include "mongo/db/multitenancy.h"
#include "mongo/db/operation_context.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/s/grid.h"
#include "mongo/util/net/ssl_types.h"
#include "mongo/util/str.h"

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

std::unique_ptr<AuthzSessionExternalState>
AuthzManagerExternalStateMongos::makeAuthzSessionExternalState(AuthorizationManager* authzManager) {
    return std::make_unique<AuthzSessionExternalStateMongos>(authzManager);
}

Status AuthzManagerExternalStateMongos::getStoredAuthorizationVersion(OperationContext* opCtx,
                                                                      int* outVersion) {
    // NOTE: We are treating the command "{ 'getParameter' : 1, 'authSchemaVersion' : 1 }" as a user
    // management command since this is the *only* part of mongos that runs this command.
    BSONObj getParameterCmd = BSON("getParameter" << 1 << "authSchemaVersion" << 1);
    BSONObjBuilder builder;
    const bool ok = Grid::get(opCtx)->catalogClient()->runUserManagementReadCommand(
        opCtx, "admin", getParameterCmd, &builder);
    BSONObj cmdResult = builder.obj();
    if (!ok) {
        return getStatusFromCommandResult(cmdResult);
    }

    BSONElement versionElement = cmdResult["authSchemaVersion"];
    if (versionElement.eoo()) {
        return Status(ErrorCodes::UnknownError, "getParameter misbehaved.");
    }
    *outVersion = versionElement.numberInt();

    return Status::OK();
}

StatusWith<User> AuthzManagerExternalStateMongos::getUserObject(OperationContext* opCtx,
                                                                const UserRequest& userReq) {
    // Marshalling to BSON and back is inevitable since the
    // source of truth is a system external to mongos.
    BSONObj userDoc;
    auto status = getUserDescription(opCtx, userReq, &userDoc);
    if (!status.isOK()) {
        return status;
    }

    User user(userReq.name);
    V2UserDocumentParser dp;
    dp.setTenantId(getActiveTenant(opCtx));
    status = dp.initializeUserFromUserDocument(userDoc, &user);
    if (!status.isOK()) {
        return status;
    }

    return std::move(user);
}

Status AuthzManagerExternalStateMongos::getUserDescription(OperationContext* opCtx,
                                                           const UserRequest& user,
                                                           BSONObj* result) {
    const UserName& userName = user.name;
    if (!user.roles) {
        BSONObj usersInfoCmd = BSON("usersInfo" << userName.toBSON(true /* serialize tenant */)
                                                << "showPrivileges" << true << "showCredentials"
                                                << true << "showAuthenticationRestrictions" << true
                                                << "showCustomData" << false);
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
                          str::stream() << "User \"" << userName << "\" not found");
        }

        if (foundUsers.size() > 1) {
            return Status(ErrorCodes::UserDataInconsistent,
                          str::stream()
                              << "Found multiple users on the \"" << userName.getDB()
                              << "\" database with name \"" << userName.getUser() << "\"");
        }
        *result = foundUsers[0].Obj().getOwned();
        return Status::OK();
    } else {
        // Obtain privilege information from the config servers for all roles acquired from the X509
        // certificate.
        BSONArrayBuilder userRolesBuilder;
        for (const RoleName& role : *user.roles) {
            userRolesBuilder.append(BSON(
                AuthorizationManager::ROLE_NAME_FIELD_NAME
                << role.getRole() << AuthorizationManager::ROLE_DB_FIELD_NAME << role.getDB()));
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
                "Received malformed response to request for X509 roles from config server");
        }

        *result =
            BSON("_id" << userName.getUser() << "user" << userName.getUser() << "db"
                       << userName.getDB() << "credentials" << BSON("external" << true) << "roles"
                       << BSONArray(cmdResult["roles"].Obj()) << "inheritedRoles"
                       << BSONArray(cmdResult["inheritedRoles"].Obj()) << "inheritedPrivileges"
                       << BSONArray(cmdResult["inheritedPrivileges"].Obj()));
        return Status::OK();
    }
}

Status AuthzManagerExternalStateMongos::rolesExist(OperationContext* opCtx,
                                                   const std::vector<RoleName>& roleNames) try {
    // Marshall role names into a set before querying so that we don't get a false-negative
    // from repeated roles only providing one result at the end.
    stdx::unordered_set<RoleName> roleNameSet(roleNames.cbegin(), roleNames.cend());

    BSONObjBuilder rolesInfoCmd;

    {
        BSONArrayBuilder rolesArray(rolesInfoCmd.subarrayStart("rolesInfo"));
        for (const auto& roleName : roleNameSet) {
            roleName.serializeToBSON(&rolesArray);
        }
        rolesArray.doneFast();
    }

    BSONObjBuilder resultBuilder;
    if (!Grid::get(opCtx)->catalogClient()->runUserManagementReadCommand(
            opCtx, "admin", rolesInfoCmd.obj(), &resultBuilder)) {
        return {ErrorCodes::OperationFailed, "Failed running rolesInfo command on mongod"};
    }

    auto result = resultBuilder.obj();
    auto cmdStatus = getStatusFromCommandResult(result);
    if (!cmdStatus.isOK()) {
        return {cmdStatus.code(),
                str::stream() << "Failed running rolesInfo command on mongod: "
                              << cmdStatus.reason()};
    }

    auto roles = result["roles"];
    if (roles.type() != Array) {
        return {ErrorCodes::OperationFailed,
                "Received invalid response from rolesInfo command on mongod"};
    }

    if (static_cast<std::size_t>(roles.Obj().nFields()) != roleNameSet.size()) {
        // One or more missing roles, cross out the ones that do exist, and return error.
        for (const auto& roleObj : roles.Obj()) {
            auto roleName = RoleName::parseFromBSON(roleObj);
            roleNameSet.erase(roleName);
        }

        return makeRoleNotFoundStatus(roleNameSet);
    }

    return Status::OK();
} catch (const AssertionException& ex) {
    return ex.toStatus();
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

namespace {

std::unique_ptr<AuthzManagerExternalState> authzManagerExternalStateCreateImpl() {
    return std::make_unique<AuthzManagerExternalStateMongos>();
}

auto authzManagerExternalStateCreateRegistration = MONGO_WEAK_FUNCTION_REGISTRATION(
    AuthzManagerExternalState::create, authzManagerExternalStateCreateImpl);

}  // namespace

}  // namespace mongo
