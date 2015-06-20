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

/**
 * This module describes free functions for logging various operations of interest to a
 * party interested in generating logs of user activity in a MongoDB server instance.
 */

#pragma once

#include "mongo/base/error_codes.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/auth/user.h"

namespace mongo {

class AuthorizationSession;
class BSONObj;
class ClientBasic;
class Command;
class NamespaceString;
class ReplSetConfig;
class StringData;
class UserName;

namespace audit {

/**
 * Logs the result of an authentication attempt.
 */
void logAuthentication(ClientBasic* client,
                       StringData mechanism,
                       const UserName& user,
                       ErrorCodes::Error result);

//
// Authorization (authz) logging functions.
//
// These functions generate log messages describing the disposition of access control
// checks.
//

/**
 * Logs the result of a command authorization check.
 */
void logCommandAuthzCheck(ClientBasic* client,
                          const std::string& dbname,
                          const BSONObj& cmdObj,
                          Command* command,
                          ErrorCodes::Error result);

/**
 * Logs the result of an authorization check for an OP_DELETE wire protocol message.
 */
void logDeleteAuthzCheck(ClientBasic* client,
                         const NamespaceString& ns,
                         const BSONObj& pattern,
                         ErrorCodes::Error result);

/**
 * Logs the result of an authorization check for an OP_GET_MORE wire protocol message.
 */
void logGetMoreAuthzCheck(ClientBasic* client,
                          const NamespaceString& ns,
                          long long cursorId,
                          ErrorCodes::Error result);

/**
 * Logs the result of an authorization check for an OP_INSERT wire protocol message.
 */
void logInsertAuthzCheck(ClientBasic* client,
                         const NamespaceString& ns,
                         const BSONObj& insertedObj,
                         ErrorCodes::Error result);

/**
 * Logs the result of an authorization check for an OP_KILL_CURSORS wire protocol message.
 */
void logKillCursorsAuthzCheck(ClientBasic* client,
                              const NamespaceString& ns,
                              long long cursorId,
                              ErrorCodes::Error result);

/**
 * Logs the result of an authorization check for an OP_QUERY wire protocol message.
 */
void logQueryAuthzCheck(ClientBasic* client,
                        const NamespaceString& ns,
                        const BSONObj& query,
                        ErrorCodes::Error result);

/**
 * Logs the result of an authorization check for an OP_UPDATE wire protocol message.
 */
void logUpdateAuthzCheck(ClientBasic* client,
                         const NamespaceString& ns,
                         const BSONObj& query,
                         const BSONObj& updateObj,
                         bool isUpsert,
                         bool isMulti,
                         ErrorCodes::Error result);

/**
 * Logs the result of a createUser command.
 */
void logCreateUser(ClientBasic* client,
                   const UserName& username,
                   bool password,
                   const BSONObj* customData,
                   const std::vector<RoleName>& roles);

/**
 * Logs the result of a dropUser command.
 */
void logDropUser(ClientBasic* client, const UserName& username);

/**
 * Logs the result of a dropAllUsersFromDatabase command.
 */
void logDropAllUsersFromDatabase(ClientBasic* client, StringData dbname);

/**
 * Logs the result of a updateUser command.
 */
void logUpdateUser(ClientBasic* client,
                   const UserName& username,
                   bool password,
                   const BSONObj* customData,
                   const std::vector<RoleName>* roles);

/**
 * Logs the result of a grantRolesToUser command.
 */
void logGrantRolesToUser(ClientBasic* client,
                         const UserName& username,
                         const std::vector<RoleName>& roles);

/**
 * Logs the result of a revokeRolesFromUser command.
 */
void logRevokeRolesFromUser(ClientBasic* client,
                            const UserName& username,
                            const std::vector<RoleName>& roles);

/**
 * Logs the result of a createRole command.
 */
void logCreateRole(ClientBasic* client,
                   const RoleName& role,
                   const std::vector<RoleName>& roles,
                   const PrivilegeVector& privileges);

/**
 * Logs the result of a updateRole command.
 */
void logUpdateRole(ClientBasic* client,
                   const RoleName& role,
                   const std::vector<RoleName>* roles,
                   const PrivilegeVector* privileges);

/**
 * Logs the result of a dropRole command.
 */
void logDropRole(ClientBasic* client, const RoleName& role);

/**
 * Logs the result of a dropAllRolesForDatabase command.
 */
void logDropAllRolesFromDatabase(ClientBasic* client, StringData dbname);

/**
 * Logs the result of a grantRolesToRole command.
 */
void logGrantRolesToRole(ClientBasic* client,
                         const RoleName& role,
                         const std::vector<RoleName>& roles);

/**
 * Logs the result of a revokeRolesFromRole command.
 */
void logRevokeRolesFromRole(ClientBasic* client,
                            const RoleName& role,
                            const std::vector<RoleName>& roles);

/**
 * Logs the result of a grantPrivilegesToRole command.
 */
void logGrantPrivilegesToRole(ClientBasic* client,
                              const RoleName& role,
                              const PrivilegeVector& privileges);

/**
 * Logs the result of a revokePrivilegesFromRole command.
 */
void logRevokePrivilegesFromRole(ClientBasic* client,
                                 const RoleName& role,
                                 const PrivilegeVector& privileges);

/**
 * Logs the result of a replSet(Re)config command.
 */
void logReplSetReconfig(ClientBasic* client, const BSONObj* oldConfig, const BSONObj* newConfig);

/**
 * Logs the result of an ApplicationMessage command.
 */
void logApplicationMessage(ClientBasic* client, StringData msg);

/**
 * Logs the result of a shutdown command.
 */
void logShutdown(ClientBasic* client);

/**
 * Logs the result of a createIndex command.
 */
void logCreateIndex(ClientBasic* client,
                    const BSONObj* indexSpec,
                    StringData indexname,
                    StringData nsname);

/**
 * Logs the result of a createCollection command.
 */
void logCreateCollection(ClientBasic* client, StringData nsname);

/**
 * Logs the result of a createDatabase command.
 */
void logCreateDatabase(ClientBasic* client, StringData dbname);


/**
 * Logs the result of a dropIndex command.
 */
void logDropIndex(ClientBasic* client, StringData indexname, StringData nsname);

/**
 * Logs the result of a dropCollection command.
 */
void logDropCollection(ClientBasic* client, StringData nsname);

/**
 * Logs the result of a dropDatabase command.
 */
void logDropDatabase(ClientBasic* client, StringData dbname);

/**
 * Logs a collection rename event.
 */
void logRenameCollection(ClientBasic* client, StringData source, StringData target);

/**
 * Logs the result of a enableSharding command.
 */
void logEnableSharding(ClientBasic* client, StringData dbname);

/**
 * Logs the result of a addShard command.
 */
void logAddShard(ClientBasic* client,
                 StringData name,
                 const std::string& servers,
                 long long maxSize);

/**
 * Logs the result of a removeShard command.
 */
void logRemoveShard(ClientBasic* client, StringData shardname);

/**
 * Logs the result of a shardCollection command.
 */
void logShardCollection(ClientBasic* client, StringData ns, const BSONObj& keyPattern, bool unique);


/*
 * Appends an array of user/db pairs and an array of role/db pairs
 * to the provided metadata builder. The users and roles are extracted from the current client.
 * They are to be the impersonated users and roles for a Command run by an internal user.
 */
void writeImpersonatedUsersToMetadata(BSONObjBuilder* metadataBob);

/*
 * Looks for an 'impersonatedUsers' field.  This field is used by mongos to
 * transmit the usernames of the currently authenticated user when it runs commands
 * on a shard using internal user authentication.  Auditing uses this information
 * to properly ascribe users to actions.  This is necessary only for implicit actions that
 * mongos cannot properly audit itself; examples are implicit collection and database creation.
 * This function requires that the field is the last field in the bson object; it edits the
 * command BSON to efficiently remove the field before returning.
 *
 * cmdObj [in, out]: If any impersonated users field exists, it will be parsed and removed.
 * parsedUserNames [out]: populated with parsed usernames
 * fieldIsPresent [out]: true if impersonatedUsers field was present in the object
 */
void parseAndRemoveImpersonatedUsersField(BSONObj cmdObj,
                                          std::vector<UserName>* parsedUserNames,
                                          bool* fieldIsPresent);

/*
 * Looks for an 'impersonatedRoles' field.  This field is used by mongos to
 * transmit the roles of the currently authenticated user when it runs commands
 * on a shard using internal user authentication.  Auditing uses this information
 * to properly ascribe user roles to actions.  This is necessary only for implicit actions that
 * mongos cannot properly audit itself; examples are implicit collection and database creation.
 * This function requires that the field is the last field in the bson object; it edits the
 * command BSON to efficiently remove the field before returning.
 *
 * cmdObj [in, out]: If any impersonated roles field exists, it will be parsed and removed.
 * parsedRoleNames [out]: populated with parsed user rolenames
 * fieldIsPresent [out]: true if impersonatedRoles field was present in the object
 */
void parseAndRemoveImpersonatedRolesField(BSONObj cmdObj,
                                          std::vector<RoleName>* parsedRoleNames,
                                          bool* fieldIsPresent);

}  // namespace audit
}  // namespace mongo
