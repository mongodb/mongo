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

#pragma once

#include <string>
#include <vector>

#include "mongo/base/string_data.h"
#include "mongo/bson/mutable/element.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/auth/role_name.h"
#include "mongo/db/auth/user_name.h"
#include "mongo/db/commands/user_management_commands_gen.h"
#include "mongo/db/database_name.h"

namespace mongo {

class AuthorizationSession;
struct BSONArray;
class BSONObj;
class Client;
class OperationContext;

namespace auth {

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
