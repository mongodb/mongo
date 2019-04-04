/**
 *    Copyright (C) 2018-present MerizoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MerizoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.merizodb.com/licensing/server-side-public-license>.
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

#include "merizo/db/audit.h"

#if !MONGO_ENTERPRISE_AUDIT

void merizo::audit::logAuthentication(Client* client,
                                     StringData mechanism,
                                     const UserName& user,
                                     ErrorCodes::Error result) {}

void merizo::audit::logCommandAuthzCheck(Client* client,
                                        const OpMsgRequest& cmdObj,
                                        const CommandInterface& command,
                                        ErrorCodes::Error result) {}

void merizo::audit::logDeleteAuthzCheck(Client* client,
                                       const NamespaceString& ns,
                                       const BSONObj& pattern,
                                       ErrorCodes::Error result) {}

void merizo::audit::logGetMoreAuthzCheck(Client* client,
                                        const NamespaceString& ns,
                                        long long cursorId,
                                        ErrorCodes::Error result) {}

void merizo::audit::logInsertAuthzCheck(Client* client,
                                       const NamespaceString& ns,
                                       const BSONObj& insertedObj,
                                       ErrorCodes::Error result) {}

void merizo::audit::logKillCursorsAuthzCheck(Client* client,
                                            const NamespaceString& ns,
                                            long long cursorId,
                                            ErrorCodes::Error result) {}

void merizo::audit::logQueryAuthzCheck(Client* client,
                                      const NamespaceString& ns,
                                      const BSONObj& query,
                                      ErrorCodes::Error result) {}

void merizo::audit::logUpdateAuthzCheck(Client* client,
                                       const NamespaceString& ns,
                                       const BSONObj& query,
                                       const BSONObj& updateObj,
                                       bool isUpsert,
                                       bool isMulti,
                                       ErrorCodes::Error result) {}

void merizo::audit::logCreateUser(Client* client,
                                 const UserName& username,
                                 bool password,
                                 const BSONObj* customData,
                                 const std::vector<RoleName>& roles,
                                 const boost::optional<BSONArray>& restrictions) {}

void merizo::audit::logDropUser(Client* client, const UserName& username) {}

void merizo::audit::logDropAllUsersFromDatabase(Client* client, StringData dbname) {}

void merizo::audit::logUpdateUser(Client* client,
                                 const UserName& username,
                                 bool password,
                                 const BSONObj* customData,
                                 const std::vector<RoleName>* roles,
                                 const boost::optional<BSONArray>& restrictions) {}

void merizo::audit::logGrantRolesToUser(Client* client,
                                       const UserName& username,
                                       const std::vector<RoleName>& roles) {}

void merizo::audit::logRevokeRolesFromUser(Client* client,
                                          const UserName& username,
                                          const std::vector<RoleName>& roles) {}

void merizo::audit::logCreateRole(Client* client,
                                 const RoleName& role,
                                 const std::vector<RoleName>& roles,
                                 const PrivilegeVector& privileges,
                                 const boost::optional<BSONArray>& restrictions) {}

void merizo::audit::logUpdateRole(Client* client,
                                 const RoleName& role,
                                 const std::vector<RoleName>* roles,
                                 const PrivilegeVector* privileges,
                                 const boost::optional<BSONArray>& restrictions) {}

void merizo::audit::logDropRole(Client* client, const RoleName& role) {}

void merizo::audit::logDropAllRolesFromDatabase(Client* client, StringData dbname) {}

void merizo::audit::logGrantRolesToRole(Client* client,
                                       const RoleName& role,
                                       const std::vector<RoleName>& roles) {}

void merizo::audit::logRevokeRolesFromRole(Client* client,
                                          const RoleName& role,
                                          const std::vector<RoleName>& roles) {}

void merizo::audit::logGrantPrivilegesToRole(Client* client,
                                            const RoleName& role,
                                            const PrivilegeVector& privileges) {}

void merizo::audit::logRevokePrivilegesFromRole(Client* client,
                                               const RoleName& role,
                                               const PrivilegeVector& privileges) {}

void merizo::audit::logReplSetReconfig(Client* client,
                                      const BSONObj* oldConfig,
                                      const BSONObj* newConfig) {}

void merizo::audit::logApplicationMessage(Client* client, StringData msg) {}

void merizo::audit::logShutdown(Client* client) {}

void merizo::audit::logCreateIndex(Client* client,
                                  const BSONObj* indexSpec,
                                  StringData indexname,
                                  StringData nsname) {}

void merizo::audit::logCreateCollection(Client* client, StringData nsname) {}

void merizo::audit::logCreateDatabase(Client* client, StringData dbname) {}


void merizo::audit::logDropIndex(Client* client, StringData indexname, StringData nsname) {}

void merizo::audit::logDropCollection(Client* client, StringData nsname) {}

void merizo::audit::logDropDatabase(Client* client, StringData dbname) {}

void merizo::audit::logRenameCollection(Client* client, StringData source, StringData target) {}

void merizo::audit::logEnableSharding(Client* client, StringData dbname) {}

void merizo::audit::logAddShard(Client* client,
                               StringData name,
                               const std::string& servers,
                               long long maxSize) {}

void merizo::audit::logRemoveShard(Client* client, StringData shardname) {}

void merizo::audit::logShardCollection(Client* client,
                                      StringData ns,
                                      const BSONObj& keyPattern,
                                      bool unique) {}

#endif
