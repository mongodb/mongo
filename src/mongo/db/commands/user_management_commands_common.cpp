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

#include "mongo/db/commands/user_management_commands_common.h"

#include <string>
#include <vector>

#include "mongo/base/status.h"
#include "mongo/bson/mutable/algorithm.h"
#include "mongo/config.h"
#include "mongo/db/auth/action_set.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/auth/user.h"
#include "mongo/db/auth/user_management_commands_parser.h"
#include "mongo/db/commands/user_management_commands_gen.h"
#include "mongo/db/jsobj.h"
#include "mongo/util/sequence_util.h"
#include "mongo/util/str.h"

namespace mongo {
namespace auth {

std::vector<RoleName> resolveRoleNames(const std::vector<RoleNameOrString>& possibleRoles,
                                       StringData dbname) {
    // De-duplicate as we resolve names by using a set.
    stdx::unordered_set<RoleName> roles;
    for (const auto& possibleRole : possibleRoles) {
        roles.insert(possibleRole.getRoleName(dbname));
    }
    return std::vector<RoleName>(roles.cbegin(), roles.cend());
}

Status checkAuthorizedToGrantRoles(AuthorizationSession* authzSession,
                                   const std::vector<RoleName>& roles) {
    for (size_t i = 0; i < roles.size(); ++i) {
        if (!authzSession->isAuthorizedToGrantRole(roles[i])) {
            return Status(ErrorCodes::Unauthorized,
                          str::stream()
                              << "Not authorized to grant role: " << roles[i].getFullName());
        }
    }

    return Status::OK();
}

Status checkAuthorizedToGrantPrivileges(AuthorizationSession* authzSession,
                                        const PrivilegeVector& privileges) {
    for (PrivilegeVector::const_iterator it = privileges.begin(); it != privileges.end(); ++it) {
        Status status = authzSession->checkAuthorizedToGrantPrivilege(*it);
        if (!status.isOK()) {
            return status;
        }
    }

    return Status::OK();
}

Status checkAuthorizedToRevokeRoles(AuthorizationSession* authzSession,
                                    const std::vector<RoleName>& roles) {
    for (size_t i = 0; i < roles.size(); ++i) {
        if (!authzSession->isAuthorizedToRevokeRole(roles[i])) {
            return Status(ErrorCodes::Unauthorized,
                          str::stream()
                              << "Not authorized to revoke role: " << roles[i].getFullName());
        }
    }
    return Status::OK();
}

Status checkAuthorizedToRevokePrivileges(AuthorizationSession* authzSession,
                                         const PrivilegeVector& privileges) {
    for (PrivilegeVector::const_iterator it = privileges.begin(); it != privileges.end(); ++it) {
        Status status = authzSession->checkAuthorizedToRevokePrivilege(*it);
        if (!status.isOK()) {
            return status;
        }
    }

    return Status::OK();
}

Status checkAuthorizedToSetRestrictions(AuthorizationSession* authzSession,
                                        bool hasAuthRestriction,
                                        StringData dbname) {
    if (hasAuthRestriction) {
        if (!authzSession->isAuthorizedForActionsOnResource(
                ResourcePattern::forDatabaseName(dbname),
                ActionType::setAuthenticationRestriction)) {
            return Status(ErrorCodes::Unauthorized, "Unauthorized");
        }
    }

    return Status::OK();
}

void checkAuthForTypedCommand(Client* client, const CreateUserCommand& request) {
    const auto& dbname = request.getDbName();
    auto* as = AuthorizationSession::get(client);

    uassert(ErrorCodes::Unauthorized,
            str::stream() << "Not authorized to create users on db: " << dbname,
            as->isAuthorizedForActionsOnResource(ResourcePattern::forDatabaseName(dbname),
                                                 ActionType::createUser));

    auto resolvedRoles = resolveRoleNames(request.getRoles(), dbname);
    uassertStatusOK(checkAuthorizedToGrantRoles(as, resolvedRoles));

    uassertStatusOK(checkAuthorizedToSetRestrictions(
        as, request.getAuthenticationRestrictions() != boost::none, dbname));
}

void checkAuthForTypedCommand(Client* client, const UpdateUserCommand& request) {
    const auto& dbname = request.getDbName();
    auto* as = AuthorizationSession::get(client);

    UserName userName(request.getCommandParameter(), dbname);
    uassert(
        ErrorCodes::Unauthorized,
        str::stream() << "Not authorized to change password of user: " << userName.getFullName(),
        (request.getPwd() == boost::none) || as->isAuthorizedToChangeOwnPasswordAsUser(userName) ||
            as->isAuthorizedForActionsOnResource(ResourcePattern::forDatabaseName(dbname),
                                                 ActionType::changePassword));

    uassert(ErrorCodes::Unauthorized,
            str::stream() << "Not authorized to change customData of user: "
                          << userName.getFullName(),
            (request.getCustomData() == boost::none) ||
                as->isAuthorizedToChangeOwnCustomDataAsUser(userName) ||
                as->isAuthorizedForActionsOnResource(ResourcePattern::forDatabaseName(dbname),
                                                     ActionType::changeCustomData));

    if (auto possibleRoles = request.getRoles()) {
        // You don't know what roles you might be revoking, so require the ability to
        // revoke any role in the system.
        uassert(ErrorCodes::Unauthorized,
                "In order to use updateUser to set roles array, must be "
                "authorized to revoke any role in the system",
                as->isAuthorizedForActionsOnResource(ResourcePattern::forAnyNormalResource(),
                                                     ActionType::revokeRole));

        auto resolvedRoles = resolveRoleNames(possibleRoles.get(), dbname);
        uassertStatusOK(checkAuthorizedToGrantRoles(as, resolvedRoles));
    }

    uassertStatusOK(checkAuthorizedToSetRestrictions(
        as, request.getAuthenticationRestrictions() != boost::none, dbname));
}

void checkAuthForTypedCommand(Client* client, const GrantRolesToUserCommand& request) {
    auto roles = resolveRoleNames(request.getRoles(), request.getDbName());
    auto* as = AuthorizationSession::get(client);
    uassertStatusOK(checkAuthorizedToGrantRoles(as, roles));
}

void checkAuthForTypedCommand(Client* client, const CreateRoleCommand& request) {
    auto* as = AuthorizationSession::get(client);
    const auto& dbname = request.getDbName();
    RoleName roleName(request.getCommandParameter(), dbname);

    uassert(ErrorCodes::Unauthorized,
            str::stream() << "Not authorized to create roles on db: " << dbname,
            as->isAuthorizedToCreateRole(roleName));

    uassertStatusOK(checkAuthorizedToGrantRoles(as, resolveRoleNames(request.getRoles(), dbname)));
    uassertStatusOK(checkAuthorizedToGrantPrivileges(as, request.getPrivileges()));
    uassertStatusOK(checkAuthorizedToSetRestrictions(
        as, request.getAuthenticationRestrictions() != boost::none, dbname));
}

void checkAuthForTypedCommand(Client* client, const UpdateRoleCommand& request) {
    auto* as = AuthorizationSession::get(client);
    const auto& dbname = request.getDbName();

    // You don't know what roles or privileges you might be revoking, so require the ability
    // to revoke any role (or privilege) in the system.
    uassert(ErrorCodes::Unauthorized,
            "updateRole command required the ability to revoke any role in the system",
            as->isAuthorizedForActionsOnResource(ResourcePattern::forAnyNormalResource(),
                                                 ActionType::revokeRole));

    if (auto roles = request.getRoles()) {
        auto resolvedRoles = resolveRoleNames(roles.get(), dbname);
        uassertStatusOK(checkAuthorizedToGrantRoles(as, resolvedRoles));
    }
    if (auto privs = request.getPrivileges()) {
        uassertStatusOK(checkAuthorizedToGrantPrivileges(as, privs.get()));
    }
    uassertStatusOK(checkAuthorizedToSetRestrictions(
        as, request.getAuthenticationRestrictions() != boost::none, dbname));
}

void checkAuthForTypedCommand(Client* client, const GrantRolesToRoleCommand& request) {
    auto rolesToRemove = resolveRoleNames(request.getRoles(), request.getDbName());
    auto* as = AuthorizationSession::get(client);
    uassertStatusOK(checkAuthorizedToGrantRoles(as, rolesToRemove));
}

void checkAuthForTypedCommand(Client* client, const GrantPrivilegesToRoleCommand& request) {
    auto* as = AuthorizationSession::get(client);
    uassertStatusOK(checkAuthorizedToGrantPrivileges(as, request.getPrivileges()));
}

void checkAuthForTypedCommand(Client* client, const DropUserCommand& request) {
    auto* as = AuthorizationSession::get(client);
    UserName userName(request.getCommandParameter(), request.getDbName());

    uassert(ErrorCodes::Unauthorized,
            str::stream() << "Not authorized to drop users from the " << userName.getDB()
                          << " database",
            as->isAuthorizedForActionsOnResource(ResourcePattern::forDatabaseName(userName.getDB()),
                                                 ActionType::dropUser));
}

void checkAuthForTypedCommand(Client* client, const DropRoleCommand& request) {
    const auto& dbname = request.getDbName();
    auto* as = AuthorizationSession::get(client);

    uassert(ErrorCodes::Unauthorized,
            str::stream() << "Not authorized to drop roles from the " << dbname << " database",
            as->isAuthorizedForActionsOnResource(ResourcePattern::forDatabaseName(dbname),
                                                 ActionType::dropRole));
}

void checkAuthForTypedCommand(Client* client, const DropAllUsersFromDatabaseCommand& request) {
    const auto& dbname = request.getDbName();
    auto* as = AuthorizationSession::get(client);
    uassert(ErrorCodes::Unauthorized,
            str::stream() << "Not authorized to drop users from the " << dbname << " database",
            as->isAuthorizedForActionsOnResource(ResourcePattern::forDatabaseName(dbname),
                                                 ActionType::dropUser));
}

void checkAuthForTypedCommand(Client* client, const RevokeRolesFromUserCommand& request) {
    auto roles = resolveRoleNames(request.getRoles(), request.getDbName());
    auto* as = AuthorizationSession::get(client);
    uassertStatusOK(checkAuthorizedToRevokeRoles(as, roles));
}

void checkAuthForTypedCommand(Client* client, const RevokeRolesFromRoleCommand& request) {
    auto rolesToRemove = resolveRoleNames(request.getRoles(), request.getDbName());
    auto* as = AuthorizationSession::get(client);
    uassertStatusOK(checkAuthorizedToRevokeRoles(as, rolesToRemove));
}

Status checkAuthForUsersInfoCommand(Client* client,
                                    const std::string& dbname,
                                    const BSONObj& cmdObj) {
    AuthorizationSession* authzSession = AuthorizationSession::get(client);
    auth::UsersInfoArgs args;
    Status status = auth::parseUsersInfoCommand(cmdObj, dbname, &args);
    if (!status.isOK()) {
        return status;
    }

    if (args.target == auth::UsersInfoArgs::Target::kDB) {
        if (!authzSession->isAuthorizedForActionsOnResource(
                ResourcePattern::forDatabaseName(dbname), ActionType::viewUser)) {
            return Status(ErrorCodes::Unauthorized,
                          str::stream()
                              << "Not authorized to view users from the " << dbname << " database");
        }
    } else if (args.target == auth::UsersInfoArgs::Target::kGlobal) {
        if (!authzSession->isAuthorizedForActionsOnResource(ResourcePattern::forClusterResource(),
                                                            ActionType::viewUser)) {
            return Status(ErrorCodes::Unauthorized,
                          str::stream() << "Not authorized to view users from all"
                                        << " databases");
        }
    } else {
        for (size_t i = 0; i < args.userNames.size(); ++i) {
            if (authzSession->lookupUser(args.userNames[i])) {
                continue;  // Can always view users you are logged in as
            }
            if (!authzSession->isAuthorizedForActionsOnResource(
                    ResourcePattern::forDatabaseName(args.userNames[i].getDB()),
                    ActionType::viewUser)) {
                return Status(ErrorCodes::Unauthorized,
                              str::stream() << "Not authorized to view users from the " << dbname
                                            << " database");
            }
        }
    }
    return Status::OK();
}

void checkAuthForTypedCommand(Client* client, const RevokePrivilegesFromRoleCommand& request) {
    auto* as = AuthorizationSession::get(client);
    uassertStatusOK(checkAuthorizedToRevokePrivileges(as, request.getPrivileges()));
}

void checkAuthForTypedCommand(Client* client, const DropAllRolesFromDatabaseCommand& request) {
    const auto& dbname = request.getDbName();
    auto* as = AuthorizationSession::get(client);
    uassert(ErrorCodes::Unauthorized,
            str::stream() << "Not authorized to drop roles from the " << dbname << " database",
            as->isAuthorizedForActionsOnResource(ResourcePattern::forDatabaseName(dbname),
                                                 ActionType::dropRole));
}

Status checkAuthForRolesInfoCommand(Client* client,
                                    const std::string& dbname,
                                    const BSONObj& cmdObj) {
    AuthorizationSession* authzSession = AuthorizationSession::get(client);
    auth::RolesInfoArgs args;
    Status status = auth::parseRolesInfoCommand(cmdObj, dbname, &args);
    if (!status.isOK()) {
        return status;
    }

    if (args.allForDB) {
        if (!authzSession->isAuthorizedForActionsOnResource(
                ResourcePattern::forDatabaseName(dbname), ActionType::viewRole)) {
            return Status(ErrorCodes::Unauthorized,
                          str::stream()
                              << "Not authorized to view roles from the " << dbname << " database");
        }
    } else {
        for (size_t i = 0; i < args.roleNames.size(); ++i) {
            if (authzSession->isAuthenticatedAsUserWithRole(args.roleNames[i])) {
                continue;  // Can always see roles that you are a member of
            }

            if (!authzSession->isAuthorizedForActionsOnResource(
                    ResourcePattern::forDatabaseName(args.roleNames[i].getDB()),
                    ActionType::viewRole)) {
                return Status(ErrorCodes::Unauthorized,
                              str::stream() << "Not authorized to view roles from the "
                                            << args.roleNames[i].getDB() << " database");
            }
        }
    }

    return Status::OK();
}

Status checkAuthForInvalidateUserCacheCommand(Client* client) {
    AuthorizationSession* authzSession = AuthorizationSession::get(client);
    if (!authzSession->isAuthorizedForActionsOnResource(ResourcePattern::forClusterResource(),
                                                        ActionType::invalidateUserCache)) {
        return Status(ErrorCodes::Unauthorized, "Not authorized to invalidate user cache");
    }
    return Status::OK();
}

Status checkAuthForGetUserCacheGenerationCommand(Client* client) {
    AuthorizationSession* authzSession = AuthorizationSession::get(client);
    if (!authzSession->isAuthorizedForActionsOnResource(ResourcePattern::forClusterResource(),
                                                        ActionType::internal)) {
        return Status(ErrorCodes::Unauthorized, "Not authorized to get cache generation");
    }
    return Status::OK();
}

Status checkAuthForMergeAuthzCollectionsCommand(Client* client, const BSONObj& cmdObj) {
    auth::MergeAuthzCollectionsArgs args;
    Status status = auth::parseMergeAuthzCollectionsCommand(cmdObj, &args);
    if (!status.isOK()) {
        return status;
    }

    AuthorizationSession* authzSession = AuthorizationSession::get(client);
    ActionSet actions;
    actions.addAction(ActionType::createUser);
    actions.addAction(ActionType::createRole);
    actions.addAction(ActionType::grantRole);
    actions.addAction(ActionType::revokeRole);
    if (args.drop) {
        actions.addAction(ActionType::dropUser);
        actions.addAction(ActionType::dropRole);
    }
    if (!authzSession->isAuthorizedForActionsOnResource(ResourcePattern::forAnyNormalResource(),
                                                        actions)) {
        return Status(ErrorCodes::Unauthorized,
                      "Not authorized to update user/role data using _mergeAuthzCollections"
                      " command");
    }
    if (!args.usersCollName.empty() &&
        !authzSession->isAuthorizedForActionsOnResource(
            ResourcePattern::forExactNamespace(NamespaceString(args.usersCollName)),
            ActionType::find)) {
        return Status(ErrorCodes::Unauthorized,
                      str::stream() << "Not authorized to read " << args.usersCollName);
    }
    if (!args.rolesCollName.empty() &&
        !authzSession->isAuthorizedForActionsOnResource(
            ResourcePattern::forExactNamespace(NamespaceString(args.rolesCollName)),
            ActionType::find)) {
        return Status(ErrorCodes::Unauthorized,
                      str::stream() << "Not authorized to read " << args.rolesCollName);
    }
    return Status::OK();
}

}  // namespace auth
}  // namespace mongo
