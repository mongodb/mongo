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

#include <absl/container/node_hash_map.h>
#include <boost/none.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>
// IWYU pragma: no_include "ext/alloc_traits.h"
#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/config.h"  // IWYU pragma: keep
#include "mongo/db/auth/action_set.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/auth_name.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/parsed_privilege_gen.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/auth/umc_info_command_arg.h"
#include "mongo/db/auth/user_name.h"
#include "mongo/db/commands/user_management_commands_common.h"
#include "mongo/db/commands/user_management_commands_gen.h"
#include "mongo/db/multitenancy.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/tenant_id.h"
#include "mongo/stdx/unordered_set.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/database_name_util.h"
#include "mongo/util/str.h"

#include <cstddef>
#include <memory>
#include <vector>

namespace mongo {
namespace auth {
namespace {

Status checkAuthorizedToGrantPrivilege(AuthorizationSession* authzSession,
                                       const Privilege& privilege) {
    const ResourcePattern& resource = privilege.getResourcePattern();
    const auto& targetDb = resource.dbNameToMatch();
    if (resource.isDatabasePattern() || resource.isExactNamespacePattern()) {
        if (!authzSession->isAuthorizedForActionsOnResource(
                ResourcePattern::forDatabaseName(targetDb), ActionType::grantRole)) {
            return Status(ErrorCodes::Unauthorized,
                          str::stream() << "Not authorized to grant privileges on the "
                                        << targetDb.toStringForErrorMsg() << "database");
        }
    } else if (!authzSession->isAuthorizedForActionsOnResource(
                   ResourcePattern::forDatabaseName(DatabaseNameUtil::deserialize(
                       targetDb.tenantId(), "admin"_sd, SerializationContext::stateDefault())),
                   ActionType::grantRole)) {
        return Status(ErrorCodes::Unauthorized,
                      "To grant privileges affecting multiple databases or the cluster,"
                      " must be authorized to grant roles from the admin database");
    }
    return Status::OK();
}

}  // namespace

std::vector<RoleName> resolveRoleNames(const std::vector<RoleNameOrString>& possibleRoles,
                                       const DatabaseName& dbname) {
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
        if (!authzSession->isAuthorizedForActionsOnResource(
                ResourcePattern::forDatabaseName(roles[i].getDatabaseName()),
                ActionType::grantRole)) {
            return Status(ErrorCodes::Unauthorized,
                          str::stream() << "Not authorized to grant role: " << roles[i]);
        }
    }

    return Status::OK();
}

Status checkAuthorizedToGrantPrivileges(AuthorizationSession* authzSession,
                                        const boost::optional<TenantId>& tenantId,
                                        const std::vector<auth::ParsedPrivilege>& privileges) {
    for (const auto& pp : privileges) {
        auto privilege = Privilege::resolvePrivilegeWithTenant(tenantId, pp);
        auto status = checkAuthorizedToGrantPrivilege(authzSession, privilege);
        if (!status.isOK()) {
            return status;
        }
    }

    return Status::OK();
}

Status checkAuthorizedToRevokeRoles(AuthorizationSession* authzSession,
                                    const std::vector<RoleName>& roles) {
    for (size_t i = 0; i < roles.size(); ++i) {
        if (!authzSession->isAuthorizedForActionsOnResource(
                ResourcePattern::forDatabaseName(roles[i].getDatabaseName()),
                ActionType::revokeRole)) {
            return Status(ErrorCodes::Unauthorized,
                          str::stream() << "Not authorized to revoke role: " << roles[i]);
        }
    }
    return Status::OK();
}


Status checkAuthorizedToRevokePrivilege(AuthorizationSession* authzSession,
                                        const Privilege& privilege) {
    const ResourcePattern& resource = privilege.getResourcePattern();
    const auto& targetDb = resource.dbNameToMatch();
    if (resource.isDatabasePattern() || resource.isExactNamespacePattern()) {
        if (!authzSession->isAuthorizedForActionsOnResource(
                ResourcePattern::forDatabaseName(targetDb), ActionType::revokeRole)) {
            return Status(ErrorCodes::Unauthorized,
                          str::stream() << "Not authorized to revoke privileges on the "
                                        << targetDb.toStringForErrorMsg() << "database");
        }
    } else if (!authzSession->isAuthorizedForActionsOnResource(
                   ResourcePattern::forDatabaseName(DatabaseNameUtil::deserialize(
                       targetDb.tenantId(), "admin"_sd, SerializationContext::stateDefault())),
                   ActionType::revokeRole)) {
        return Status(ErrorCodes::Unauthorized,
                      "To revoke privileges affecting multiple databases or the cluster,"
                      " must be authorized to revoke roles from the admin database");
    }
    return Status::OK();
}

Status checkAuthorizedToRevokePrivileges(AuthorizationSession* authzSession,
                                         const boost::optional<TenantId>& tenantId,
                                         const std::vector<auth::ParsedPrivilege>& privileges) {
    for (const auto& pp : privileges) {
        auto privilege = Privilege::resolvePrivilegeWithTenant(tenantId, pp);
        auto status = checkAuthorizedToRevokePrivilege(authzSession, privilege);
        if (!status.isOK()) {
            return status;
        }
    }

    return Status::OK();
}

Status checkAuthorizedToSetRestrictions(AuthorizationSession* authzSession,
                                        bool hasAuthRestriction,
                                        const DatabaseName& dbname) {
    if (hasAuthRestriction) {
        if (!authzSession->isAuthorizedForActionsOnResource(
                ResourcePattern::forDatabaseName(dbname),
                ActionType::setAuthenticationRestriction)) {
            return Status(ErrorCodes::Unauthorized, "Unauthorized");
        }
    }

    return Status::OK();
}

bool isAuthorizedToChangeOwnPasswordAsUser(AuthorizationSession* authzSession,
                                           const UserName& userName) {
    return authzSession->isAuthorizedToChangeAsUser(userName, ActionType::changeOwnPassword);
}

bool isAuthorizedToChangeOwnCustomDataAsUser(AuthorizationSession* authzSession,
                                             const UserName& userName) {
    return authzSession->isAuthorizedToChangeAsUser(userName, ActionType::changeOwnCustomData);
}

void checkAuthForTypedCommand(OperationContext* opCtx, const CreateUserCommand& request) {
    const auto& dbname = request.getDbName();
    auto* as = AuthorizationSession::get(opCtx->getClient());

    uassert(ErrorCodes::Unauthorized,
            str::stream() << "Not authorized to create users on db: "
                          << dbname.toStringForErrorMsg(),
            as->isAuthorizedForActionsOnResource(ResourcePattern::forDatabaseName(dbname),
                                                 ActionType::createUser));

    auto resolvedRoles = resolveRoleNames(request.getRoles(), dbname);
    uassertStatusOK(checkAuthorizedToGrantRoles(as, resolvedRoles));

    uassertStatusOK(checkAuthorizedToSetRestrictions(
        as, request.getAuthenticationRestrictions() != boost::none, dbname));
}

void checkAuthForTypedCommand(OperationContext* opCtx, const UpdateUserCommand& request) {
    const auto& dbname = request.getDbName();
    auto* as = AuthorizationSession::get(opCtx->getClient());

    UserName userName(request.getCommandParameter(), dbname);
    uassert(ErrorCodes::Unauthorized,
            str::stream() << "Not authorized to change password of user: " << userName,
            (request.getPwd() == boost::none) ||
                isAuthorizedToChangeOwnPasswordAsUser(as, userName) ||
                as->isAuthorizedForActionsOnResource(ResourcePattern::forDatabaseName(dbname),
                                                     ActionType::changePassword));

    uassert(ErrorCodes::Unauthorized,
            str::stream() << "Not authorized to change customData of user: " << userName,
            (request.getCustomData() == boost::none) ||
                isAuthorizedToChangeOwnCustomDataAsUser(as, userName) ||
                as->isAuthorizedForActionsOnResource(ResourcePattern::forDatabaseName(dbname),
                                                     ActionType::changeCustomData));

    if (auto possibleRoles = request.getRoles()) {
        // You don't know what roles you might be revoking, so require the ability to
        // revoke any role in the system.
        uassert(
            ErrorCodes::Unauthorized,
            "In order to use updateUser to set roles array, must be "
            "authorized to revoke any role in the system",
            as->isAuthorizedForActionsOnResource(
                ResourcePattern::forAnyNormalResource(dbname.tenantId()), ActionType::revokeRole));

        auto resolvedRoles = resolveRoleNames(possibleRoles.value(), dbname);
        uassertStatusOK(checkAuthorizedToGrantRoles(as, resolvedRoles));
    }

    uassertStatusOK(checkAuthorizedToSetRestrictions(
        as, request.getAuthenticationRestrictions() != boost::none, dbname));
}

void checkAuthForTypedCommand(OperationContext* opCtx, const GrantRolesToUserCommand& request) {
    auto roles = resolveRoleNames(request.getRoles(), request.getDbName());
    auto* as = AuthorizationSession::get(opCtx->getClient());
    uassertStatusOK(checkAuthorizedToGrantRoles(as, roles));
}

void checkAuthForTypedCommand(OperationContext* opCtx, const CreateRoleCommand& request) {
    auto* as = AuthorizationSession::get(opCtx->getClient());
    const auto& dbname = request.getDbName();
    RoleName roleName(request.getCommandParameter(), dbname);

    uassert(ErrorCodes::Unauthorized,
            str::stream() << "Not authorized to create roles on db: "
                          << dbname.toStringForErrorMsg(),
            as->isAuthorizedToCreateRole(roleName));

    uassertStatusOK(checkAuthorizedToGrantRoles(as, resolveRoleNames(request.getRoles(), dbname)));
    uassertStatusOK(
        checkAuthorizedToGrantPrivileges(as, dbname.tenantId(), request.getPrivileges()));
    uassertStatusOK(checkAuthorizedToSetRestrictions(
        as, request.getAuthenticationRestrictions() != boost::none, dbname));
}

void checkAuthForTypedCommand(OperationContext* opCtx, const UpdateRoleCommand& request) {
    auto* as = AuthorizationSession::get(opCtx->getClient());
    const auto& dbname = request.getDbName();

    // You don't know what roles or privileges you might be revoking, so require the ability
    // to revoke any role (or privilege) in the system.
    uassert(ErrorCodes::Unauthorized,
            "updateRole command required the ability to revoke any role in the system",
            as->isAuthorizedForActionsOnResource(
                ResourcePattern::forAnyNormalResource(dbname.tenantId()), ActionType::revokeRole));

    if (auto roles = request.getRoles()) {
        auto resolvedRoles = resolveRoleNames(roles.value(), dbname);
        uassertStatusOK(checkAuthorizedToGrantRoles(as, resolvedRoles));
    }
    if (auto privs = request.getPrivileges()) {
        uassertStatusOK(checkAuthorizedToGrantPrivileges(as, dbname.tenantId(), privs.value()));
    }
    uassertStatusOK(checkAuthorizedToSetRestrictions(
        as, request.getAuthenticationRestrictions() != boost::none, dbname));
}

void checkAuthForTypedCommand(OperationContext* opCtx, const GrantRolesToRoleCommand& request) {
    auto rolesToRemove = resolveRoleNames(request.getRoles(), request.getDbName());
    auto* as = AuthorizationSession::get(opCtx->getClient());
    uassertStatusOK(checkAuthorizedToGrantRoles(as, rolesToRemove));
}

void checkAuthForTypedCommand(OperationContext* opCtx,
                              const GrantPrivilegesToRoleCommand& request) {
    auto* as = AuthorizationSession::get(opCtx->getClient());
    uassertStatusOK(checkAuthorizedToGrantPrivileges(
        as, request.getDbName().tenantId(), request.getPrivileges()));
}

void checkAuthForTypedCommand(OperationContext* opCtx, const DropUserCommand& request) {
    auto* as = AuthorizationSession::get(opCtx->getClient());
    UserName userName(request.getCommandParameter(), request.getDbName());

    uassert(
        ErrorCodes::Unauthorized,
        str::stream() << "Not authorized to drop users from the " << userName.getDB()
                      << " database",
        as->isAuthorizedForActionsOnResource(
            ResourcePattern::forDatabaseName(userName.getDatabaseName()), ActionType::dropUser));
}

void checkAuthForTypedCommand(OperationContext* opCtx, const DropRoleCommand& request) {
    const auto& dbname = request.getDbName();
    auto* as = AuthorizationSession::get(opCtx->getClient());

    uassert(ErrorCodes::Unauthorized,
            str::stream() << "Not authorized to drop roles from the "
                          << dbname.toStringForErrorMsg() << " database",
            as->isAuthorizedForActionsOnResource(ResourcePattern::forDatabaseName(dbname),
                                                 ActionType::dropRole));
}

void checkAuthForTypedCommand(OperationContext* opCtx,
                              const DropAllUsersFromDatabaseCommand& request) {
    const auto& dbname = request.getDbName();
    auto* as = AuthorizationSession::get(opCtx->getClient());
    uassert(ErrorCodes::Unauthorized,
            str::stream() << "Not authorized to drop users from the "
                          << dbname.toStringForErrorMsg() << " database",
            as->isAuthorizedForActionsOnResource(ResourcePattern::forDatabaseName(dbname),
                                                 ActionType::dropUser));
}

void checkAuthForTypedCommand(OperationContext* opCtx, const RevokeRolesFromUserCommand& request) {
    auto roles = resolveRoleNames(request.getRoles(), request.getDbName());
    auto* as = AuthorizationSession::get(opCtx->getClient());
    uassertStatusOK(checkAuthorizedToRevokeRoles(as, roles));
}

void checkAuthForTypedCommand(OperationContext* opCtx, const RevokeRolesFromRoleCommand& request) {
    auto rolesToRemove = resolveRoleNames(request.getRoles(), request.getDbName());
    auto* as = AuthorizationSession::get(opCtx->getClient());
    uassertStatusOK(checkAuthorizedToRevokeRoles(as, rolesToRemove));
}

void checkAuthForTypedCommand(OperationContext* opCtx, const UsersInfoCommand& request) {
    const auto& dbname = request.getDbName();
    const auto& arg = request.getCommandParameter();
    auto* as = AuthorizationSession::get(opCtx->getClient());

    if (arg.isAllOnCurrentDB()) {
        uassert(ErrorCodes::Unauthorized,
                str::stream() << "Not authorized to view users from the "
                              << dbname.toStringForErrorMsg() << " database",
                as->isAuthorizedForActionsOnResource(ResourcePattern::forDatabaseName(dbname),
                                                     ActionType::viewUser));
    } else if (arg.isAllForAllDBs()) {
        uassert(ErrorCodes::Unauthorized,
                str::stream() << "Not authorized to view users from all databases",
                as->isAuthorizedForActionsOnResource(
                    ResourcePattern::forClusterResource(dbname.tenantId()), ActionType::viewUser));
    } else {
        invariant(arg.isExact());
        auto activeTenant = getActiveTenant(opCtx);
        for (const auto& userName : arg.getElements(dbname)) {
            if (userName.tenantId() != boost::none) {
                // Only connection based cluster administrators may specify tenant in query.
                uassert(ErrorCodes::Unauthorized,
                        "May not specify tenant in usersInfo query",
                        !activeTenant &&
                            as->isAuthorizedForActionsOnResource(
                                ResourcePattern::forClusterResource(dbname.tenantId()),
                                ActionType::internal));
            }

            if (as->lookupUser(userName)) {
                // Can always view users you are logged in as.
                continue;
            }
            uassert(ErrorCodes::Unauthorized,
                    str::stream() << "Not authorized to view users from the "
                                  << dbname.toStringForErrorMsg() << " database",
                    as->isAuthorizedForActionsOnResource(
                        ResourcePattern::forDatabaseName(userName.getDatabaseName()),
                        ActionType::viewUser));
        }
    }
}

void checkAuthForTypedCommand(OperationContext* opCtx,
                              const RevokePrivilegesFromRoleCommand& request) {
    auto* as = AuthorizationSession::get(opCtx->getClient());
    uassertStatusOK(checkAuthorizedToRevokePrivileges(
        as, request.getDbName().tenantId(), request.getPrivileges()));
}

void checkAuthForTypedCommand(OperationContext* opCtx,
                              const DropAllRolesFromDatabaseCommand& request) {
    const auto& dbname = request.getDbName();
    auto* as = AuthorizationSession::get(opCtx->getClient());
    uassert(ErrorCodes::Unauthorized,
            str::stream() << "Not authorized to drop roles from the "
                          << dbname.toStringForErrorMsg() << " database",
            as->isAuthorizedForActionsOnResource(ResourcePattern::forDatabaseName(dbname),
                                                 ActionType::dropRole));
}

void checkAuthForTypedCommand(OperationContext* opCtx, const RolesInfoCommand& request) {
    const auto& dbname = request.getDbName();
    const auto& arg = request.getCommandParameter();
    auto* as = AuthorizationSession::get(opCtx->getClient());

    invariant(!arg.isAllForAllDBs());
    if (arg.isAllOnCurrentDB()) {
        uassert(ErrorCodes::Unauthorized,
                str::stream() << "Not authorized to view roles from the "
                              << dbname.toStringForErrorMsg() << " database",
                as->isAuthorizedForActionsOnResource(ResourcePattern::forDatabaseName(dbname),
                                                     ActionType::viewRole));
    } else {
        invariant(arg.isExact());
        auto roles = arg.getElements(dbname);
        for (const auto& role : roles) {
            if (as->isAuthenticatedAsUserWithRole(role)) {
                continue;  // Can always see roles that you are a member of
            }

            uassert(ErrorCodes::Unauthorized,
                    str::stream() << "Not authorized to view roles from the " << role.getDB()
                                  << " database",
                    as->isAuthorizedForActionsOnResource(
                        ResourcePattern::forDatabaseName(role.getDatabaseName()),
                        ActionType::viewRole));
        }
    }
}

void checkAuthForTypedCommand(OperationContext* opCtx, const InvalidateUserCacheCommand& request) {
    auto* as = AuthorizationSession::get(opCtx->getClient());
    uassert(ErrorCodes::Unauthorized,
            "Not authorized to invalidate user cache",
            as->isAuthorizedForActionsOnResource(
                ResourcePattern::forClusterResource(request.getDbName().tenantId()),
                ActionType::invalidateUserCache));
}

void checkAuthForTypedCommand(OperationContext* opCtx,
                              const GetUserCacheGenerationCommand& request) {
    auto* as = AuthorizationSession::get(opCtx->getClient());
    uassert(ErrorCodes::Unauthorized,
            "Not authorized to get cache generation",
            as->isAuthorizedForActionsOnResource(
                ResourcePattern::forClusterResource(request.getDbName().tenantId()),
                ActionType::internal));
}

void checkAuthForTypedCommand(OperationContext* opCtx,
                              const MergeAuthzCollectionsCommand& request) {
    auto* as = AuthorizationSession::get(opCtx->getClient());

    ActionSet actions;
    actions.addAction(ActionType::createUser);
    actions.addAction(ActionType::createRole);
    actions.addAction(ActionType::grantRole);
    actions.addAction(ActionType::revokeRole);
    if (request.getDrop()) {
        actions.addAction(ActionType::dropUser);
        actions.addAction(ActionType::dropRole);
    }

    auto tempUsersColl = request.getTempUsersCollection();
    if (!tempUsersColl.empty()) {
        auto tempUsersNS = NamespaceStringUtil::deserialize(
            request.getDbName().tenantId(), tempUsersColl, request.getSerializationContext());
        uassert(ErrorCodes::Unauthorized,
                "Not authorized to update user data using _mergeAuthzCollections a command",
                as->isAuthorizedForActionsOnResource(
                    ResourcePattern::forAnyNormalResource(tempUsersNS.tenantId()), actions));
        uassert(ErrorCodes::Unauthorized,
                str::stream() << "Not authorized to read " << tempUsersColl,
                as->isAuthorizedForActionsOnResource(
                    ResourcePattern::forExactNamespace(tempUsersNS), ActionType::find));
    }

    auto tempRolesColl = request.getTempRolesCollection();
    if (!tempRolesColl.empty()) {
        auto tempRolesNS = NamespaceStringUtil::deserialize(
            request.getDbName().tenantId(), tempRolesColl, request.getSerializationContext());
        uassert(ErrorCodes::Unauthorized,
                "Not authorized to update role data using _mergeAuthzCollections a command",
                as->isAuthorizedForActionsOnResource(
                    ResourcePattern::forAnyNormalResource(tempRolesNS.tenantId()), actions));
        uassert(ErrorCodes::Unauthorized,
                str::stream() << "Not authorized to read " << tempRolesColl,
                as->isAuthorizedForActionsOnResource(
                    ResourcePattern::forExactNamespace(tempRolesNS), ActionType::find));
    }
}

}  // namespace auth
}  // namespace mongo
