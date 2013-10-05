/**
*    Copyright (C) 2013 10gen Inc.
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

#include <string>
#include <vector>

#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/base/disallow_copying.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/auth/role_name.h"
#include "mongo/db/auth/user.h"
#include "mongo/db/auth/user_name.h"
#include "mongo/db/jsobj.h"

namespace mongo {
namespace auth {

    struct CreateOrUpdateUserArgs {
        UserName userName;
        bool hasHashedPassword;
        std::string hashedPassword;
        bool hasCustomData;
        BSONObj customData;
        bool hasRoles;
        std::vector<User::RoleData> roles;
        BSONObj writeConcern;
        CreateOrUpdateUserArgs() :
            hasHashedPassword(false), hasCustomData(false),  hasRoles(false) {}
    };

    /**
     * Takes a command object describing an invocation of the "createUser" or "updateUser" commands
     * (which command it is is specified in "cmdName") on the database "dbname", and parses out all
     * the arguments into the "parsedArgs" output param.
     */
    Status parseCreateOrUpdateUserCommands(const BSONObj& cmdObj,
                                           const StringData& cmdName,
                                           const std::string& dbname,
                                           CreateOrUpdateUserArgs* parsedArgs);

    /**
     * Takes a command object describing an invocation of one of "grantRolesToUser",
     * "revokeRolesFromUser", "grantDelegateRolesToUser", "revokeDelegateRolesFromUser",
     * "grantRolesToRole", and "revokeRolesFromRoles" (which command it is is specified in the
     * "cmdName" argument), and parses out (into the parsedName out param) the user/role name of
     * the user/roles being modified, the roles being granted or revoked, and the write concern to
     * use.
     */
    Status parseRolePossessionManipulationCommands(const BSONObj& cmdObj,
                                                   const StringData& cmdName,
                                                   const StringData& rolesFieldName,
                                                   const std::string& dbname,
                                                   std::string* parsedName,
                                                   vector<RoleName>* parsedRoleNames,
                                                   BSONObj* parsedWriteConcern);

    /**
     * Takes a command object describing an invocation of the "dropUser" command and parses out
     * the UserName of the user to be removed and the writeConcern.
     * Also validates the input and returns a non-ok Status if there is anything wrong.
     */
    Status parseAndValidateDropUserCommand(const BSONObj& cmdObj,
                                           const std::string& dbname,
                                           UserName* parsedUserName,
                                           BSONObj* parsedWriteConcern);

    /**
     * Takes a command object describing an invocation of the "dropUsersFromDatabase" command and
     * parses out the write concern.
     * Also validates the input and returns a non-ok Status if there is anything wrong.
     */
    Status parseAndValidateDropUsersFromDatabaseCommand(const BSONObj& cmdObj,
                                                        const std::string& dbname,
                                                        BSONObj* parsedWriteConcern);

    /**
     * Takes a command object describing an invocation of the "usersInfo" or "rolesInfo" commands
     * (which command it is is specified in the "cmdName" argument) and parses out a BSONElement
     * with the user/role name filter to be applied, as well as the anyDB boolean.
     * Also validates the input and returns a non-ok Status if there is anything wrong.
     */
    Status parseAndValidateInfoCommands(const BSONObj& cmdObj,
                                        const StringData& cmdName,
                                        const std::string& dbname,
                                        bool* parsedAnyDb,
                                        BSONElement* parsedNameFilter);

    struct CreateOrUpdateRoleArgs {
        RoleName roleName;
        bool hasRoles;
        std::vector<RoleName> roles;
        bool hasPrivileges;
        PrivilegeVector privileges;
        BSONObj writeConcern;
        CreateOrUpdateRoleArgs() : hasRoles(false), hasPrivileges(false) {}
    };

    /**
     * Takes a command object describing an invocation of the "createRole" or "updateRole" commands
     * (which command it is is specified in "cmdName") on the database "dbname", and parses out all
     * the arguments into the "parsedArgs" output param.
     */
    Status parseCreateOrUpdateRoleCommands(const BSONObj& cmdObj,
                                           const StringData& cmdName,
                                           const std::string& dbname,
                                           CreateOrUpdateRoleArgs* parsedArgs);

    /**
     * Takes a command object describing an invocation of the "grantPrivilegesToRole" or
     * "revokePrivilegesFromRole" commands, and parses out the role name of the
     * role being modified, the privileges being granted or revoked, and the write concern to use.
     */
    Status parseAndValidateRolePrivilegeManipulationCommands(const BSONObj& cmdObj,
                                                             const StringData& cmdName,
                                                             const std::string& dbname,
                                                             RoleName* parsedRoleName,
                                                             PrivilegeVector* parsedPrivileges,
                                                             BSONObj* parsedWriteConcern);

    /**
     * Takes a command object describing an invocation of the "dropRole" command and parses out
     * the RoleName of the role to be removed and the writeConcern.
     */
    Status parseDropRoleCommand(const BSONObj& cmdObj,
                                const std::string& dbname,
                                RoleName* parsedRoleName,
                                BSONObj* parsedWriteConcern);

    /**
     * Takes a command object describing an invocation of the "dropRolesFromDatabase" command and
     * parses out the write concern.
     */
    Status parseDropRolesFromDatabaseCommand(const BSONObj& cmdObj,
                                             const std::string& dbname,
                                             BSONObj* parsedWriteConcern);
    /**
     * Parses the privileges described in "privileges" into a vector of Privilege objects.
     * Returns Status::OK() upon successfully parsing all the elements of "privileges".
     */
    Status parseAndValidatePrivilegeArray(const BSONArray& privileges,
                                          PrivilegeVector* parsedPrivileges);

    /**
     * Takes a BSONArray of name,source pair documents, parses that array and returns (via the
     * output param parsedRoleNames) a list of the role names in the input array.
     * Performs syntactic validation of "rolesArray", only.
     */
    Status parseRoleNamesFromBSONArray(const BSONArray& rolesArray,
                                       const StringData& dbname,
                                       const StringData& rolesFieldName,
                                       std::vector<RoleName>* parsedRoleNames);

} // namespace auth
} // namespace mongo
