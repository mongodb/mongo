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

#include <absl/container/node_hash_set.h>
#include <boost/move/utility_core.hpp>
#include <boost/optional.hpp>
#include <cstddef>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <boost/optional/optional.hpp>

#include "mongo/base/shim.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/auth/auth_name.h"
#include "mongo/db/auth/authorization_client_handle.h"
#include "mongo/db/auth/authz_manager_external_state_s.h"
#include "mongo/db/auth/authz_session_external_state.h"
#include "mongo/db/auth/authz_session_external_state_s.h"
#include "mongo/db/auth/user_document_parser.h"
#include "mongo/db/auth/user_name.h"
#include "mongo/db/commands/user_management_commands_gen.h"
#include "mongo/db/multitenancy.h"
#include "mongo/db/operation_context.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/s/catalog/sharding_catalog_client.h"
#include "mongo/s/grid.h"
#include "mongo/stdx/unordered_set.h"
#include "mongo/util/assert_util.h"
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
    const auto& swResponse = AuthorizationClientHandle::get(opCtx->getService())
                                 ->sendGetStoredAuthorizationVersionRequest(opCtx);

    if (!swResponse.isOK()) {
        return swResponse.getStatus();
    }

    *outVersion = swResponse.getValue().version;

    return Status::OK();
}

StatusWith<User> AuthzManagerExternalStateMongos::getUserObject(
    OperationContext* opCtx,
    const UserRequest& userReq,
    const SharedUserAcquisitionStats& userAcquisitionStats) {
    // Marshalling to BSON and back is inevitable since the
    // source of truth is a system external to mongos.
    BSONObj userDoc;
    auto status = getUserDescription(opCtx, userReq, &userDoc, userAcquisitionStats);
    if (!status.isOK()) {
        return status;
    }

    auto swReq = userReq.clone();
    if (!swReq.isOK()) {
        return swReq.getStatus();
    }

    User user(std::move(swReq.getValue()));
    V2UserDocumentParser dp;
    dp.setTenantId(getActiveTenant(opCtx));
    status = dp.initializeUserFromUserDocument(userDoc, &user);
    if (!status.isOK()) {
        return status;
    }

    return std::move(user);
}

Status AuthzManagerExternalStateMongos::getUserDescription(
    OperationContext* opCtx,
    const UserRequest& userReq,
    BSONObj* result,
    const SharedUserAcquisitionStats& userAcquisitionStats) {
    const auto& userName = userReq.getUserName();

    if (!userReq.getRoles()) {
        UsersInfoCommand usersInfoCmd(auth::UsersInfoCommandArg(userReq.getUserName()));
        usersInfoCmd.setShowPrivileges(true);
        usersInfoCmd.setShowCredentials(true);
        usersInfoCmd.setShowAuthenticationRestrictions(true);
        usersInfoCmd.setShowCustomData(false);

        const auto& usersInfoReply =
            AuthorizationClientHandle::get(opCtx->getService())
                ->sendUsersInfoRequest(opCtx, DatabaseName::kAdmin, std::move(usersInfoCmd));

        if (!usersInfoReply.isOK()) {
            return usersInfoReply.getStatus();
        }

        const auto& foundUsers = usersInfoReply.getValue().getUsers();

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
        *result = foundUsers[0].getOwned();
        return Status::OK();
    } else {
        // Obtain privilege information from the config servers for all roles acquired from the X509
        // certificate or jwt token.
        const auto& rolesArr = *userReq.getRoles();
        auth::RolesInfoCommandArg::Multiple roleNames(rolesArr.begin(), rolesArr.end());

        RolesInfoCommand rolesInfoCmd{auth::RolesInfoCommandArg{roleNames}};
        rolesInfoCmd.setShowPrivileges(
            auth::ParsedPrivilegeFormat(PrivilegeFormat::kShowAsUserFragment));

        const auto& rolesInfoResponse =
            AuthorizationClientHandle::get(opCtx->getService())
                ->sendRolesInfoRequest(opCtx, DatabaseName::kAdmin, std::move(rolesInfoCmd));

        if (!rolesInfoResponse.isOK()) {
            return Status(ErrorCodes::FailedToParse,
                          "Unable to get resolved X509 roles from config server: " +
                              rolesInfoResponse.getStatus().reason());
        }
        const auto& optUserFragment = rolesInfoResponse.getValue().getUserFragment();

        if (!optUserFragment) {
            return Status(ErrorCodes::FailedToParse, "Unable to get user from config server.");
        }

        const auto& cmdResult = *optUserFragment;

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
    auth::RolesInfoCommandArg::Multiple roleNamesCopy(roleNames.begin(), roleNames.end());
    RolesInfoCommand rolesInfoCmd{auth::RolesInfoCommandArg{roleNamesCopy}};

    const auto& rolesInfoResponse =
        AuthorizationClientHandle::get(opCtx->getService())
            ->sendRolesInfoRequest(opCtx, DatabaseName::kAdmin, std::move(rolesInfoCmd));

    if (!rolesInfoResponse.isOK()) {
        return {rolesInfoResponse.getStatus().code(),
                str::stream() << "Failed running rolesInfo command on mongod: "
                              << rolesInfoResponse.getStatus().reason()};
    }

    const auto& optRoles = rolesInfoResponse.getValue().getRoles();
    if (!optRoles) {
        return {ErrorCodes::OperationFailed,
                "Received invalid response from rolesInfo command on mongod"};
    }

    const auto& roles = *optRoles;
    stdx::unordered_set<RoleName> roleNamesSet(roleNames.begin(), roleNames.end());

    if (roles.size() != roleNamesSet.size()) {
        // One or more missing roles, cross out the ones that do exist, and return error.
        for (const auto& roleObj : roles) {
            auto roleName = RoleName::parseFromBSONObj(roleObj);
            roleNamesSet.erase(roleName);
        }

        return makeRoleNotFoundStatus(roleNamesSet);
    }

    return Status::OK();
} catch (const AssertionException& ex) {
    return ex.toStatus();
}

bool AuthzManagerExternalStateMongos::hasAnyPrivilegeDocuments(OperationContext* opCtx) {
    UsersInfoCommand usersInfoCmd{auth::UsersInfoCommandArg{}};

    const auto& swUsersReply =
        AuthorizationClientHandle::get(opCtx->getService())
            ->sendUsersInfoRequest(opCtx, DatabaseName::kAdmin, std::move(usersInfoCmd));

    if (!swUsersReply.isOK()) {
        // If we were unable to complete the query,
        // it's best to assume that there _are_ privilege documents.  This might happen
        // if the node contaning the users collection becomes transiently unavailable.
        // See SERVER-12616, for example.
        return true;
    }

    const auto& usersInfoReply = swUsersReply.getValue();
    const auto& foundUsers = usersInfoReply.getUsers();

    if (foundUsers.size() > 0) {
        return true;
    }

    RolesInfoCommand rolesInfoCmd{auth::RolesInfoCommandArg{}};

    const auto& swRolesReply =
        AuthorizationClientHandle::get(opCtx->getService())
            ->sendRolesInfoRequest(opCtx, DatabaseName::kAdmin, std::move(rolesInfoCmd));

    if (!swRolesReply.isOK()) {
        return true;
    }

    const auto& rolesInfoResponse = swRolesReply.getValue();
    const auto& foundRoles = rolesInfoResponse.getRoles();

    if (!foundRoles || foundRoles->size() == 0) {
        return false;
    }

    return true;
}

namespace {

std::unique_ptr<AuthzManagerExternalState> authzManagerExternalStateCreateImpl() {
    return std::make_unique<AuthzManagerExternalStateMongos>();
}

auto authzManagerExternalStateCreateRegistration = MONGO_WEAK_FUNCTION_REGISTRATION(
    AuthzManagerExternalState::create, authzManagerExternalStateCreateImpl);

}  // namespace

}  // namespace mongo
