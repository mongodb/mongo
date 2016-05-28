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

#pragma once

#include <string>
#include <vector>

#include "mongo/base/disallow_copying.h"
#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
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
    std::vector<RoleName> roles;
    BSONObj writeConcern;

    CreateOrUpdateUserArgs() : hasHashedPassword(false), hasCustomData(false), hasRoles(false) {}
};

/**
 * Takes a command object describing an invocation of the "createUser" or "updateUser" commands
 * (which command it is is specified in "cmdName") on the database "dbname", and parses out all
 * the arguments into the "parsedArgs" output param.
 */
Status parseCreateOrUpdateUserCommands(const BSONObj& cmdObj,
                                       StringData cmdName,
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
                                               StringData cmdName,
                                               const std::string& dbname,
                                               std::string* parsedName,
                                               std::vector<RoleName>* parsedRoleNames);

/**
 * Takes a command object describing an invocation of the "dropUser" command and parses out
 * the UserName of the user to be removed.
 * Also validates the input and returns a non-ok Status if there is anything wrong.
 */
Status parseAndValidateDropUserCommand(const BSONObj& cmdObj,
                                       const std::string& dbname,
                                       UserName* parsedUserName);

/**
 * Takes a command object describing an invocation of the "dropAllUsersFromDatabase" command and
 * parses out the write concern.
 * Also validates the input and returns a non-ok Status if there is anything wrong.
 */
Status parseAndValidateDropAllUsersFromDatabaseCommand(const BSONObj& cmdObj,
                                                       const std::string& dbname);

struct UsersInfoArgs {
    std::vector<UserName> userNames;
    bool allForDB;
    bool showPrivileges;
    bool showCredentials;
    UsersInfoArgs() : allForDB(false), showPrivileges(false), showCredentials(false) {}
};

/**
 * Takes a command object describing an invocation of the "usersInfo" command  and parses out
 * all the arguments into the "parsedArgs" output param.
 */
Status parseUsersInfoCommand(const BSONObj& cmdObj, StringData dbname, UsersInfoArgs* parsedArgs);

struct RolesInfoArgs {
    std::vector<RoleName> roleNames;
    bool allForDB;
    bool showPrivileges;
    bool showBuiltinRoles;
    RolesInfoArgs() : allForDB(false), showPrivileges(false), showBuiltinRoles(false) {}
};

/**
 * Takes a command object describing an invocation of the "rolesInfo" command  and parses out
 * the arguments into the "parsedArgs" output param.
 */
Status parseRolesInfoCommand(const BSONObj& cmdObj, StringData dbname, RolesInfoArgs* parsedArgs);

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
                                       StringData cmdName,
                                       const std::string& dbname,
                                       CreateOrUpdateRoleArgs* parsedArgs);

/**
 * Takes a command object describing an invocation of the "grantPrivilegesToRole" or
 * "revokePrivilegesFromRole" commands, and parses out the role name of the
 * role being modified, the privileges being granted or revoked, and the write concern to use.
 */
Status parseAndValidateRolePrivilegeManipulationCommands(const BSONObj& cmdObj,
                                                         StringData cmdName,
                                                         const std::string& dbname,
                                                         RoleName* parsedRoleName,
                                                         PrivilegeVector* parsedPrivileges);

/**
 * Takes a command object describing an invocation of the "dropRole" command and parses out
 * the RoleName of the role to be removed.
 */
Status parseDropRoleCommand(const BSONObj& cmdObj,
                            const std::string& dbname,
                            RoleName* parsedRoleName);

/**
 * Takes a command object describing an invocation of the "dropAllRolesFromDatabase" command and
 * parses out the write concern.
 */
Status parseDropAllRolesFromDatabaseCommand(const BSONObj& cmdObj, const std::string& dbname);

/**
 * Parses the privileges described in "privileges" into a vector of Privilege objects.
 * Returns Status::OK() upon successfully parsing all the elements of "privileges".
 */
Status parseAndValidatePrivilegeArray(const BSONArray& privileges,
                                      PrivilegeVector* parsedPrivileges);

/**
 * Takes a BSONArray of name,db pair documents, parses that array and returns (via the
 * output param parsedRoleNames) a list of the role names in the input array.
 * Performs syntactic validation of "rolesArray", only.
 */
Status parseRoleNamesFromBSONArray(const BSONArray& rolesArray,
                                   StringData dbname,
                                   std::vector<RoleName>* parsedRoleNames);

/**
 * Takes a BSONArray of name,db pair documents, parses that array and returns (via the
 * output param parsedUserNames) a list of the usernames in the input array.
 * Performs syntactic validation of "usersArray", only.
 */
Status parseUserNamesFromBSONArray(const BSONArray& usersArray,
                                   StringData dbname,
                                   std::vector<UserName>* parsedUserNames);

struct MergeAuthzCollectionsArgs {
    std::string usersCollName;
    std::string rolesCollName;
    std::string db;
    bool drop;
    BSONObj writeConcern;
    MergeAuthzCollectionsArgs() : drop(false) {}
};

/**
 * Takes a command object describing an invocation of the "_mergeAuthzCollections" command and
 * parses out the name of the temporary collections to use for user and role data, whether or
 * not to drop the existing users/roles, the database if this is a for a db-specific restore.
 * Returns ErrorCodes::OutdatedClient if the "db" field is missing, as that likely indicates
 * the command was sent by an outdated (pre 2.6.4) version of mongorestore.
 * Returns other codes indicating missing or incorrectly typed fields.
 */
Status parseMergeAuthzCollectionsCommand(const BSONObj& cmdObj,
                                         MergeAuthzCollectionsArgs* parsedArgs);

struct AuthSchemaUpgradeArgs {
    int maxSteps = 3;
    bool shouldUpgradeShards = true;
    BSONObj writeConcern;
};

/**
 * Takes a command object describing an invocation of the "authSchemaUpgrade" command and
 * parses out the write concern, maximum steps to take and whether or not shard servers should
 * also be upgraded, in the sharded deployment case.
 */
Status parseAuthSchemaUpgradeCommand(const BSONObj& cmdObj,
                                     const std::string& dbname,
                                     AuthSchemaUpgradeArgs* parsedArgs);
}  // namespace auth
}  // namespace mongo
