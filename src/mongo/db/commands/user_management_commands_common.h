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
                                       StringData dbname);

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

void checkAuthForTypedCommand(Client*, const CreateUserCommand&);
void checkAuthForTypedCommand(Client*, const UpdateUserCommand&);
void checkAuthForTypedCommand(Client*, const GrantRolesToUserCommand&);
void checkAuthForTypedCommand(Client*, const CreateRoleCommand&);
void checkAuthForTypedCommand(Client*, const UpdateRoleCommand&);
void checkAuthForTypedCommand(Client*, const GrantRolesToRoleCommand&);
void checkAuthForTypedCommand(Client*, const GrantPrivilegesToRoleCommand&);
void checkAuthForTypedCommand(Client*, const DropAllUsersFromDatabaseCommand&);
void checkAuthForTypedCommand(Client*, const RevokeRolesFromUserCommand&);
void checkAuthForTypedCommand(Client*, const RevokeRolesFromRoleCommand&);
void checkAuthForTypedCommand(Client*, const DropUserCommand&);
void checkAuthForTypedCommand(Client*, const DropRoleCommand&);
void checkAuthForTypedCommand(Client*, const RevokePrivilegesFromRoleCommand&);
void checkAuthForTypedCommand(Client*, const DropAllRolesFromDatabaseCommand&);
void checkAuthForTypedCommand(Client*, const UsersInfoCommand&);
void checkAuthForTypedCommand(Client*, const RolesInfoCommand&);
void checkAuthForTypedCommand(Client*, const InvalidateUserCacheCommand&);
void checkAuthForTypedCommand(Client*, const GetUserCacheGenerationCommand&);
void checkAuthForTypedCommand(Client*, const MergeAuthzCollectionsCommand&);

}  // namespace auth
}  // namespace mongo
