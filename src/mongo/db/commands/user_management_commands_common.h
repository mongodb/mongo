// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/auth/role_name.h"
#include "mongo/db/auth/role_name_or_string.h"
#include "mongo/db/auth/user_name.h"
#include "mongo/db/client.h"
#include "mongo/db/commands/user_management_commands_gen.h"
#include "mongo/db/database_name.h"
#include "mongo/db/exec/mutable_bson/element.h"
#include "mongo/util/modules.h"

#include <string>
#include <vector>

namespace mongo {

class AuthorizationSession;
struct BSONArray;
class BSONObj;
class Client;
class OperationContext;

namespace [[MONGO_MOD_PUBLIC]] auth {

/**
 * User management commands accept rolenames as either `{ role: 'x', db: 'y' }`
 * or as simply a string implying the role name and the dbname is inferred from the command.
 *
 * This method takes a vector of RoleNameOrString values parsed via IDL
 * and normalizes them to a vector of RoleNames using a passed dbname fallback.
 */
std::vector<RoleName> resolveRoleNames(const std::vector<RoleNameOrString>& possibleRoles,
                                       const DatabaseName& dbname);

//
// checkAuthorizedTo* methods
//

Status checkAuthorizedToGrantRoles(AuthorizationSession* authzSession,
                                   const std::vector<RoleName>& roles);

Status checkAuthorizedToGrantPrivileges(AuthorizationSession* authzSession,
                                        const PrivilegeVector& privileges);

Status checkAuthorizedToRevokeRoles(AuthorizationSession* authzSession,
                                    const std::vector<RoleName>& roles);

Status checkAuthorizedToRevokePrivileges(AuthorizationSession* authzSession,
                                         const PrivilegeVector& privileges);

//
// checkAuthFor*Command methods
//

void checkAuthForTypedCommand(OperationContext*, const CreateUserCommand&);
void checkAuthForTypedCommand(OperationContext*, const UpdateUserCommand&);
void checkAuthForTypedCommand(OperationContext*, const GrantRolesToUserCommand&);
void checkAuthForTypedCommand(OperationContext*, const CreateRoleCommand&);
void checkAuthForTypedCommand(OperationContext*, const UpdateRoleCommand&);
void checkAuthForTypedCommand(OperationContext*, const GrantRolesToRoleCommand&);
void checkAuthForTypedCommand(OperationContext*, const GrantPrivilegesToRoleCommand&);
void checkAuthForTypedCommand(OperationContext*, const DropAllUsersFromDatabaseCommand&);
void checkAuthForTypedCommand(OperationContext*, const RevokeRolesFromUserCommand&);
void checkAuthForTypedCommand(OperationContext*, const RevokeRolesFromRoleCommand&);
void checkAuthForTypedCommand(OperationContext*, const DropUserCommand&);
void checkAuthForTypedCommand(OperationContext*, const DropRoleCommand&);
void checkAuthForTypedCommand(OperationContext*, const RevokePrivilegesFromRoleCommand&);
void checkAuthForTypedCommand(OperationContext*, const DropAllRolesFromDatabaseCommand&);
void checkAuthForTypedCommand(OperationContext*, const UsersInfoCommand&);
void checkAuthForTypedCommand(OperationContext*, const RolesInfoCommand&);
void checkAuthForTypedCommand(OperationContext*, const InvalidateUserCacheCommand&);
void checkAuthForTypedCommand(OperationContext*, const GetUserCacheGenerationCommand&);
void checkAuthForTypedCommand(OperationContext*, const MergeAuthzCollectionsCommand&);

}  // namespace auth
}  // namespace mongo
