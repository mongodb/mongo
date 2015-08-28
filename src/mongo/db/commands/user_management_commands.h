/**
 *    Copyright (C) 2015 MongoDB Inc.
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

#pragma once

#include <string>
#include <vector>

#include "mongo/bson/mutable/element.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/auth/role_name.h"
#include "mongo/db/auth/user_name.h"

namespace mongo {

class AuthorizationManager;
class AuthorizationSession;
struct BSONArray;
class BSONObj;
class ClientBasic;
class OperationContext;

namespace auth {

/**
 * Looks for a field name "pwd" in the given BSONObj and if found replaces its contents with the
 * string "xxx" so that password data on the command object used in executing a user management
 * command isn't exposed in the logs.
 */
void redactPasswordData(mutablebson::Element parent);

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

Status checkAuthForCreateUserCommand(ClientBasic* client,
                                     const std::string& dbname,
                                     const BSONObj& cmdObj);

Status checkAuthForUpdateUserCommand(ClientBasic* client,
                                     const std::string& dbname,
                                     const BSONObj& cmdObj);

Status checkAuthForGrantRolesToUserCommand(ClientBasic* client,
                                           const std::string& dbname,
                                           const BSONObj& cmdObj);

Status checkAuthForCreateRoleCommand(ClientBasic* client,
                                     const std::string& dbname,
                                     const BSONObj& cmdObj);

Status checkAuthForUpdateRoleCommand(ClientBasic* client,
                                     const std::string& dbname,
                                     const BSONObj& cmdObj);

Status checkAuthForGrantRolesToRoleCommand(ClientBasic* client,
                                           const std::string& dbname,
                                           const BSONObj& cmdObj);

Status checkAuthForGrantPrivilegesToRoleCommand(ClientBasic* client,
                                                const std::string& dbname,
                                                const BSONObj& cmdObj);

Status checkAuthForDropAllUsersFromDatabaseCommand(ClientBasic* client, const std::string& dbname);

Status checkAuthForRevokeRolesFromUserCommand(ClientBasic* client,
                                              const std::string& dbname,
                                              const BSONObj& cmdObj);

Status checkAuthForRevokeRolesFromRoleCommand(ClientBasic* client,
                                              const std::string& dbname,
                                              const BSONObj& cmdObj);

Status checkAuthForDropUserCommand(ClientBasic* client,
                                   const std::string& dbname,
                                   const BSONObj& cmdObj);

Status checkAuthForDropRoleCommand(ClientBasic* client,
                                   const std::string& dbname,
                                   const BSONObj& cmdObj);


Status checkAuthForUsersInfoCommand(ClientBasic* client,
                                    const std::string& dbname,
                                    const BSONObj& cmdObj);

Status checkAuthForRevokePrivilegesFromRoleCommand(ClientBasic* client,
                                                   const std::string& dbname,
                                                   const BSONObj& cmdObj);

Status checkAuthForDropAllRolesFromDatabaseCommand(ClientBasic* client, const std::string& dbname);

Status checkAuthForRolesInfoCommand(ClientBasic* client,
                                    const std::string& dbname,
                                    const BSONObj& cmdObj);

Status checkAuthForInvalidateUserCacheCommand(ClientBasic* client);

Status checkAuthForGetUserCacheGenerationCommand(ClientBasic* client);

Status checkAuthForMergeAuthzCollectionsCommand(ClientBasic* client, const BSONObj& cmdObj);

Status checkAuthForAuthSchemaUpgradeCommand(ClientBasic* client);

}  // namespace auth
}  // namespace mongo
