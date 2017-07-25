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

#include "mongo/db/audit.h"

#if !MONGO_ENTERPRISE_VERSION

void mongo::audit::logAuthentication(Client* client,
                                     StringData mechanism,
                                     const UserName& user,
                                     ErrorCodes::Error result) {}

void mongo::audit::logCommandAuthzCheck(Client* client,
                                        const OpMsgRequest& cmdObj,
                                        CommandInterface* command,
                                        ErrorCodes::Error result) {}

void mongo::audit::logDeleteAuthzCheck(Client* client,
                                       const NamespaceString& ns,
                                       const BSONObj& pattern,
                                       ErrorCodes::Error result) {}

void mongo::audit::logGetMoreAuthzCheck(Client* client,
                                        const NamespaceString& ns,
                                        long long cursorId,
                                        ErrorCodes::Error result) {}

void mongo::audit::logInsertAuthzCheck(Client* client,
                                       const NamespaceString& ns,
                                       const BSONObj& insertedObj,
                                       ErrorCodes::Error result) {}

void mongo::audit::logKillCursorsAuthzCheck(Client* client,
                                            const NamespaceString& ns,
                                            long long cursorId,
                                            ErrorCodes::Error result) {}

void mongo::audit::logQueryAuthzCheck(Client* client,
                                      const NamespaceString& ns,
                                      const BSONObj& query,
                                      ErrorCodes::Error result) {}

void mongo::audit::logUpdateAuthzCheck(Client* client,
                                       const NamespaceString& ns,
                                       const BSONObj& query,
                                       const BSONObj& updateObj,
                                       bool isUpsert,
                                       bool isMulti,
                                       ErrorCodes::Error result) {}

void mongo::audit::logCreateUser(Client* client,
                                 const UserName& username,
                                 bool password,
                                 const BSONObj* customData,
                                 const std::vector<RoleName>& roles) {}

void mongo::audit::logDropUser(Client* client, const UserName& username) {}

void mongo::audit::logDropAllUsersFromDatabase(Client* client, StringData dbname) {}

void mongo::audit::logUpdateUser(Client* client,
                                 const UserName& username,
                                 bool password,
                                 const BSONObj* customData,
                                 const std::vector<RoleName>* roles) {}

void mongo::audit::logGrantRolesToUser(Client* client,
                                       const UserName& username,
                                       const std::vector<RoleName>& roles) {}

void mongo::audit::logRevokeRolesFromUser(Client* client,
                                          const UserName& username,
                                          const std::vector<RoleName>& roles) {}

void mongo::audit::logCreateRole(Client* client,
                                 const RoleName& role,
                                 const std::vector<RoleName>& roles,
                                 const PrivilegeVector& privileges) {}

void mongo::audit::logUpdateRole(Client* client,
                                 const RoleName& role,
                                 const std::vector<RoleName>* roles,
                                 const PrivilegeVector* privileges) {}

void mongo::audit::logDropRole(Client* client, const RoleName& role) {}

void mongo::audit::logDropAllRolesFromDatabase(Client* client, StringData dbname) {}

void mongo::audit::logGrantRolesToRole(Client* client,
                                       const RoleName& role,
                                       const std::vector<RoleName>& roles) {}

void mongo::audit::logRevokeRolesFromRole(Client* client,
                                          const RoleName& role,
                                          const std::vector<RoleName>& roles) {}

void mongo::audit::logGrantPrivilegesToRole(Client* client,
                                            const RoleName& role,
                                            const PrivilegeVector& privileges) {}

void mongo::audit::logRevokePrivilegesFromRole(Client* client,
                                               const RoleName& role,
                                               const PrivilegeVector& privileges) {}

void mongo::audit::logReplSetReconfig(Client* client,
                                      const BSONObj* oldConfig,
                                      const BSONObj* newConfig) {}

void mongo::audit::logApplicationMessage(Client* client, StringData msg) {}

void mongo::audit::logShutdown(Client* client) {}

void mongo::audit::logCreateIndex(Client* client,
                                  const BSONObj* indexSpec,
                                  StringData indexname,
                                  StringData nsname) {}

void mongo::audit::logCreateCollection(Client* client, StringData nsname) {}

void mongo::audit::logCreateDatabase(Client* client, StringData dbname) {}


void mongo::audit::logDropIndex(Client* client, StringData indexname, StringData nsname) {}

void mongo::audit::logDropCollection(Client* client, StringData nsname) {}

void mongo::audit::logDropDatabase(Client* client, StringData dbname) {}

void mongo::audit::logRenameCollection(Client* client, StringData source, StringData target) {}

void mongo::audit::logEnableSharding(Client* client, StringData dbname) {}

void mongo::audit::logAddShard(Client* client,
                               StringData name,
                               const std::string& servers,
                               long long maxSize) {}

void mongo::audit::logRemoveShard(Client* client, StringData shardname) {}

void mongo::audit::logShardCollection(Client* client,
                                      StringData ns,
                                      const BSONObj& keyPattern,
                                      bool unique) {}

void mongo::audit::writeImpersonatedUsersToMetadata(OperationContext* opCtx,
                                                    BSONObjBuilder* metadata) {}

#endif
