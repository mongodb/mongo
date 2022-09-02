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
std::function<void(OperationContext*)> initializeManager;
std::function<void(OpObserverRegistry*)> opObserverRegistrar;
std::function<void(ServiceContext*)> initializeSynchronizeJob;

#if !MONGO_ENTERPRISE_AUDIT

ImpersonatedClientAttrs::ImpersonatedClientAttrs(Client* client) {}

void rotateAuditLog() {}

void logClientMetadata(Client* client) {
    invariant(client);
}

void logAuthentication(Client* client, const AuthenticateEvent&) {
    invariant(client);
}

void logCommandAuthzCheck(Client* client,
                          const OpMsgRequest& cmdObj,
                          const CommandInterface& command,
                          ErrorCodes::Error result) {
    invariant(client);
}

void logKillCursorsAuthzCheck(Client* client,
                              const NamespaceString& ns,
                              long long cursorId,
                              ErrorCodes::Error result) {
    invariant(client);
}

void logCreateUser(Client* client,
                   const UserName& username,
                   bool password,
                   const BSONObj* customData,
                   const std::vector<RoleName>& roles,
                   const boost::optional<BSONArray>& restrictions) {
    invariant(client);
}

void logDropUser(Client* client, const UserName& username) {
    invariant(client);
}

void logDropAllUsersFromDatabase(Client* client, StringData dbname) {
    invariant(client);
}

void logUpdateUser(Client* client,
                   const UserName& username,
                   bool password,
                   const BSONObj* customData,
                   const std::vector<RoleName>* roles,
                   const boost::optional<BSONArray>& restrictions) {
    invariant(client);
}

void logGrantRolesToUser(Client* client,
                         const UserName& username,
                         const std::vector<RoleName>& roles) {
    invariant(client);
}

void logRevokeRolesFromUser(Client* client,
                            const UserName& username,
                            const std::vector<RoleName>& roles) {
    invariant(client);
}

void logCreateRole(Client* client,
                   const RoleName& role,
                   const std::vector<RoleName>& roles,
                   const PrivilegeVector& privileges,
                   const boost::optional<BSONArray>& restrictions) {
    invariant(client);
}

void logUpdateRole(Client* client,
                   const RoleName& role,
                   const std::vector<RoleName>* roles,
                   const PrivilegeVector* privileges,
                   const boost::optional<BSONArray>& restrictions) {
    invariant(client);
}

void logDropRole(Client* client, const RoleName& role) {
    invariant(client);
}

void logDropAllRolesFromDatabase(Client* client, StringData dbname) {
    invariant(client);
}

void logGrantRolesToRole(Client* client, const RoleName& role, const std::vector<RoleName>& roles) {
}

void logRevokeRolesFromRole(Client* client,
                            const RoleName& role,
                            const std::vector<RoleName>& roles) {
    invariant(client);
}

void logGrantPrivilegesToRole(Client* client,
                              const RoleName& role,
                              const PrivilegeVector& privileges) {
    invariant(client);
}

void logRevokePrivilegesFromRole(Client* client,
                                 const RoleName& role,
                                 const PrivilegeVector& privileges) {
    invariant(client);
}

void logReplSetReconfig(Client* client, const BSONObj* oldConfig, const BSONObj* newConfig) {
    invariant(client);
}

void logApplicationMessage(Client* client, StringData msg) {
    invariant(client);
}

void logStartupOptions(Client* client, const BSONObj& startupOptions) {
    invariant(client);
}

void logShutdown(Client* client) {
    invariant(client);
}

void logLogout(Client* client,
               StringData reason,
               const BSONArray& initialUsers,
               const BSONArray& updatedUsers) {
    invariant(client);
}

void logCreateIndex(Client* client,
                    const BSONObj* indexSpec,
                    StringData indexname,
                    const NamespaceString& nsname,
                    StringData indexBuildState,
                    ErrorCodes::Error result) {
    invariant(client);
}

void logCreateCollection(Client* client, const NamespaceString& nsname) {
    invariant(client);
}

void logCreateView(Client* client,
                   const NamespaceString& nsname,
                   StringData viewOn,
                   BSONArray pipeline,
                   ErrorCodes::Error code) {
    invariant(client);
}

void logImportCollection(Client* client, const NamespaceString& nsname) {
    invariant(client);
}

void logCreateDatabase(Client* client, StringData dbname) {
    invariant(client);
}


void logDropIndex(Client* client, StringData indexname, const NamespaceString& nsname) {
    invariant(client);
}

void logDropCollection(Client* client, const NamespaceString& nsname) {
    invariant(client);
}

void logDropView(Client* client,
                 const NamespaceString& nsname,
                 StringData viewOn,
                 const std::vector<BSONObj>& pipeline,
                 ErrorCodes::Error code) {
    invariant(client);
}

void logDropDatabase(Client* client, StringData dbname) {
    invariant(client);
}

void logRenameCollection(Client* client,
                         const NamespaceString& source,
                         const NamespaceString& target) {
    invariant(client);
}

void logEnableSharding(Client* client, StringData dbname) {
    invariant(client);
}

void logAddShard(Client* client, StringData name, const std::string& servers, long long maxSize) {
    invariant(client);
}

void logRemoveShard(Client* client, StringData shardname) {
    invariant(client);
}

void logShardCollection(Client* client, StringData ns, const BSONObj& keyPattern, bool unique) {
    invariant(client);
}

void logRefineCollectionShardKey(Client* client, StringData ns, const BSONObj& keyPattern) {
    invariant(client);
}

void logInsertOperation(Client* client, const NamespaceString& nss, const BSONObj& doc) {
    invariant(client);
}

void logUpdateOperation(Client* client, const NamespaceString& nss, const BSONObj& doc) {
    invariant(client);
}

void logRemoveOperation(Client* client, const NamespaceString& nss, const BSONObj& doc) {
    invariant(client);
}

void logGetClusterParameter(
    Client* client,
    const stdx::variant<std::string, std::vector<std::string>>& requestedParameters) {
    invariant(client);
}

void logSetClusterParameter(Client* client, const BSONObj& oldValue, const BSONObj& newValue) {
    invariant(client);
}

void logUpdateCachedClusterParameter(Client* client,
                                     const BSONObj& oldValue,
                                     const BSONObj& newValue) {
    invariant(client);
}

#endif

}  // namespace audit
}  // namespace mongo
