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

#include "mongo/db/audit.h"

namespace mongo {
namespace audit {

#if !MONGO_ENTERPRISE_AUDIT

ImpersonatedClientAttrs::ImpersonatedClientAttrs(Client* client) {}

void logAuthentication(Client*, const AuthenticateEvent&) {}

void logCommandAuthzCheck(Client* client,
                          const OpMsgRequest& cmdObj,
                          const CommandInterface& command,
                          ErrorCodes::Error result) {}

void logDeleteAuthzCheck(Client* client,
                         const NamespaceString& ns,
                         const BSONObj& pattern,
                         ErrorCodes::Error result) {}

void logGetMoreAuthzCheck(Client* client,
                          const NamespaceString& ns,
                          long long cursorId,
                          ErrorCodes::Error result) {}

void logInsertAuthzCheck(Client* client,
                         const NamespaceString& ns,
                         const BSONObj& insertedObj,
                         ErrorCodes::Error result) {}

void logKillCursorsAuthzCheck(Client* client,
                              const NamespaceString& ns,
                              long long cursorId,
                              ErrorCodes::Error result) {}

void logQueryAuthzCheck(Client* client,
                        const NamespaceString& ns,
                        const BSONObj& query,
                        ErrorCodes::Error result) {}

void logUpdateAuthzCheck(Client* client,
                         const NamespaceString& ns,
                         const BSONObj& query,
                         const write_ops::UpdateModification& update,
                         bool isUpsert,
                         bool isMulti,
                         ErrorCodes::Error result) {}

void logCreateUser(Client* client,
                   const UserName& username,
                   bool password,
                   const BSONObj* customData,
                   const std::vector<RoleName>& roles,
                   const boost::optional<BSONArray>& restrictions) {}

void logDropUser(Client* client, const UserName& username) {}

void logDropAllUsersFromDatabase(Client* client, StringData dbname) {}

void logUpdateUser(Client* client,
                   const UserName& username,
                   bool password,
                   const BSONObj* customData,
                   const std::vector<RoleName>* roles,
                   const boost::optional<BSONArray>& restrictions) {}

void logGrantRolesToUser(Client* client,
                         const UserName& username,
                         const std::vector<RoleName>& roles) {}

void logRevokeRolesFromUser(Client* client,
                            const UserName& username,
                            const std::vector<RoleName>& roles) {}

void logCreateRole(Client* client,
                   const RoleName& role,
                   const std::vector<RoleName>& roles,
                   const PrivilegeVector& privileges,
                   const boost::optional<BSONArray>& restrictions) {}

void logUpdateRole(Client* client,
                   const RoleName& role,
                   const std::vector<RoleName>* roles,
                   const PrivilegeVector* privileges,
                   const boost::optional<BSONArray>& restrictions) {}

void logDropRole(Client* client, const RoleName& role) {}

void logDropAllRolesFromDatabase(Client* client, StringData dbname) {}

void logGrantRolesToRole(Client* client, const RoleName& role, const std::vector<RoleName>& roles) {
}

void logRevokeRolesFromRole(Client* client,
                            const RoleName& role,
                            const std::vector<RoleName>& roles) {}

void logGrantPrivilegesToRole(Client* client,
                              const RoleName& role,
                              const PrivilegeVector& privileges) {}

void logRevokePrivilegesFromRole(Client* client,
                                 const RoleName& role,
                                 const PrivilegeVector& privileges) {}

void logReplSetReconfig(Client* client, const BSONObj* oldConfig, const BSONObj* newConfig) {}

void logApplicationMessage(Client* client, StringData msg) {}

void logStartupOptions(Client* client, const BSONObj& startupOptions) {}

void logShutdown(Client* client) {}

void logLogout(Client* client,
               StringData reason,
               const BSONArray& initialUsers,
               const BSONArray& updatedUsers) {}

void logCreateIndex(Client* client,
                    const BSONObj* indexSpec,
                    StringData indexname,
                    const NamespaceString& nsname) {}

void logCreateCollection(Client* client, const NamespaceString& nsname) {}

void logCreateView(Client* client,
                   const NamespaceString& nsname,
                   StringData viewOn,
                   BSONArray pipeline,
                   ErrorCodes::Error code) {}

void logImportCollection(Client* client, const NamespaceString& nsname) {}

void logCreateDatabase(Client* client, StringData dbname) {}


void logDropIndex(Client* client, StringData indexname, const NamespaceString& nsname) {}

void logDropCollection(Client* client, const NamespaceString& nsname) {}

void logDropView(Client* client,
                 const NamespaceString& nsname,
                 StringData viewOn,
                 const std::vector<BSONObj>& pipeline,
                 ErrorCodes::Error code) {}

void logDropDatabase(Client* client, StringData dbname) {}

void logRenameCollection(Client* client,
                         const NamespaceString& source,
                         const NamespaceString& target) {}

void logEnableSharding(Client* client, StringData dbname) {}

void logAddShard(Client* client, StringData name, const std::string& servers, long long maxSize) {}

void logRemoveShard(Client* client, StringData shardname) {}

void logShardCollection(Client* client, StringData ns, const BSONObj& keyPattern, bool unique) {}

void logRefineCollectionShardKey(Client* client, StringData ns, const BSONObj& keyPattern) {}

void logInsertOperation(Client* client, const NamespaceString& nss, const BSONObj& doc) {}

void logUpdateOperation(Client* client, const NamespaceString& nss, const BSONObj& doc) {}

void logRemoveOperation(Client* client, const NamespaceString& nss, const BSONObj& doc) {}

#endif

}  // namespace audit
}  // namespace mongo
