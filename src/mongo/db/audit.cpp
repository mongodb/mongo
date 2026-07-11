// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/audit.h"

#include "mongo/db/audit_interface.h"
#include "mongo/db/service_context.h"
#include "mongo/util/assert_util.h"

#include <string_view>

#include <boost/optional/optional.hpp>

namespace mongo {
namespace audit {
std::function<void(OperationContext*)> initializeManager;
std::function<void(OpObserverRegistry*)> opObserverRegistrar;
std::function<void(ServiceContext*)> initializeSynchronizeJob;
std::function<void()> shutdownSynchronizeJob;

#if !MONGO_ENTERPRISE_AUDIT
void rotateAuditLog() {}
#endif

namespace {
const auto getAuditInterface = ServiceContext::declareDecoration<std::unique_ptr<AuditInterface>>();
ServiceContext::ConstructorActionRegisterer registerCreateNoopAudit{
    "initializeNoopAuditInterface", [](ServiceContext* svcCtx) {
        AuditInterface::set(svcCtx, std::make_unique<AuditNoOp>());
    }};
}  // namespace

AuditInterface* AuditInterface::get(ServiceContext* service) {
    return getAuditInterface(service).get();
}

void AuditInterface::set(ServiceContext* service, std::unique_ptr<AuditInterface> interface) {
    getAuditInterface(service) = std::move(interface);
}


void logClientMetadata(Client* client) {
    AuditInterface::get(client->getServiceContext())->logClientMetadata(client);
}

void logAuthentication(Client* client, const AuthenticateEvent& event) {
    AuditInterface::get(client->getServiceContext())->logAuthentication(client, event);
}

void logCommandAuthzCheck(Client* client,
                          const OpMsgRequest& cmdObj,
                          const CommandInterface& command,
                          ErrorCodes::Error result) {
    AuditInterface::get(client->getServiceContext())
        ->logCommandAuthzCheck(client, cmdObj, command, result);
}

void logKillCursorsAuthzCheck(Client* client,
                              const NamespaceString& ns,
                              long long cursorId,
                              ErrorCodes::Error result) {
    AuditInterface::get(client->getServiceContext())
        ->logKillCursorsAuthzCheck(client, ns, cursorId, result);
}

void logCreateUser(Client* client,
                   const UserName& username,
                   bool password,
                   const BSONObj* customData,
                   const std::vector<RoleName>& roles,
                   const boost::optional<BSONArray>& restrictions) {
    AuditInterface::get(client->getServiceContext())
        ->logCreateUser(client, username, password, customData, roles, restrictions);
}

void logDropUser(Client* client, const UserName& username) {
    AuditInterface::get(client->getServiceContext())->logDropUser(client, username);
}

void logDropAllUsersFromDatabase(Client* client, const DatabaseName& dbname) {
    AuditInterface::get(client->getServiceContext())->logDropAllUsersFromDatabase(client, dbname);
}

void logUpdateUser(Client* client,
                   const UserName& username,
                   bool password,
                   const BSONObj* customData,
                   const std::vector<RoleName>* roles,
                   const boost::optional<BSONArray>& restrictions) {
    AuditInterface::get(client->getServiceContext())
        ->logUpdateUser(client, username, password, customData, roles, restrictions);
}

void logGrantRolesToUser(Client* client,
                         const UserName& username,
                         const std::vector<RoleName>& roles) {
    AuditInterface::get(client->getServiceContext())->logGrantRolesToUser(client, username, roles);
}

void logRevokeRolesFromUser(Client* client,
                            const UserName& username,
                            const std::vector<RoleName>& roles) {
    AuditInterface::get(client->getServiceContext())
        ->logRevokeRolesFromUser(client, username, roles);
}

void logCreateRole(Client* client,
                   const RoleName& role,
                   const std::vector<RoleName>& roles,
                   const PrivilegeVector& privileges,
                   const boost::optional<BSONArray>& restrictions) {
    AuditInterface::get(client->getServiceContext())
        ->logCreateRole(client, role, roles, privileges, restrictions);
}

void logUpdateRole(Client* client,
                   const RoleName& role,
                   const std::vector<RoleName>* roles,
                   const PrivilegeVector* privileges,
                   const boost::optional<BSONArray>& restrictions) {
    AuditInterface::get(client->getServiceContext())
        ->logUpdateRole(client, role, roles, privileges, restrictions);
}

void logDropRole(Client* client, const RoleName& role) {
    AuditInterface::get(client->getServiceContext())->logDropRole(client, role);
}

void logDropAllRolesFromDatabase(Client* client, const DatabaseName& dbname) {
    AuditInterface::get(client->getServiceContext())->logDropAllRolesFromDatabase(client, dbname);
}

void logGrantRolesToRole(Client* client, const RoleName& role, const std::vector<RoleName>& roles) {
    AuditInterface::get(client->getServiceContext())->logGrantRolesToRole(client, role, roles);
}

void logRevokeRolesFromRole(Client* client,
                            const RoleName& role,
                            const std::vector<RoleName>& roles) {
    AuditInterface::get(client->getServiceContext())->logRevokeRolesFromRole(client, role, roles);
}

void logGrantPrivilegesToRole(Client* client,
                              const RoleName& role,
                              const PrivilegeVector& privileges) {
    AuditInterface::get(client->getServiceContext())
        ->logGrantPrivilegesToRole(client, role, privileges);
}

void logRevokePrivilegesFromRole(Client* client,
                                 const RoleName& role,
                                 const PrivilegeVector& privileges) {
    AuditInterface::get(client->getServiceContext())
        ->logRevokePrivilegesFromRole(client, role, privileges);
}

void logReplSetReconfig(Client* client, const BSONObj* oldConfig, const BSONObj* newConfig) {
    AuditInterface::get(client->getServiceContext())
        ->logReplSetReconfig(client, oldConfig, newConfig);
}

void logApplicationMessage(Client* client, std::string_view msg) {
    AuditInterface::get(client->getServiceContext())->logApplicationMessage(client, msg);
}

void logStartupOptions(Client* client, const BSONObj& startupOptions) {
    AuditInterface::get(client->getServiceContext())->logStartupOptions(client, startupOptions);
}

void logShutdown(Client* client) {
    AuditInterface::get(client->getServiceContext())->logShutdown(client);
}

void logLogout(Client* client,
               std::string_view reason,
               const BSONArray& initialUsers,
               const BSONArray& updatedUsers,
               const boost::optional<Date_t>& loginTime) {
    AuditInterface::get(client->getServiceContext())
        ->logLogout(client, reason, initialUsers, updatedUsers, loginTime);
}

void logCreateIndex(Client* client,
                    const BSONObj* indexSpec,
                    std::string_view indexname,
                    const NamespaceString& nsname,
                    std::string_view indexBuildState,
                    ErrorCodes::Error result) {
    AuditInterface::get(client->getServiceContext())
        ->logCreateIndex(client, indexSpec, indexname, nsname, indexBuildState, result);
}

void logCreateCollection(Client* client, const NamespaceString& nsname) {
    AuditInterface::get(client->getServiceContext())->logCreateCollection(client, nsname);
}

void logCreateView(Client* client,
                   const NamespaceString& nsname,
                   const NamespaceString& viewOn,
                   BSONArray pipeline,
                   ErrorCodes::Error code) {
    AuditInterface::get(client->getServiceContext())
        ->logCreateView(client, nsname, viewOn, pipeline, code);
}

void logImportCollection(Client* client, const NamespaceString& nsname) {
    AuditInterface::get(client->getServiceContext())->logImportCollection(client, nsname);
}

void logCreateDatabase(Client* client, const DatabaseName& dbname) {
    AuditInterface::get(client->getServiceContext())->logCreateDatabase(client, dbname);
}


void logDropIndex(Client* client, std::string_view indexname, const NamespaceString& nsname) {
    AuditInterface::get(client->getServiceContext())->logDropIndex(client, indexname, nsname);
}

void logDropCollection(Client* client, const NamespaceString& nsname) {
    AuditInterface::get(client->getServiceContext())->logDropCollection(client, nsname);
}

void logDropView(Client* client,
                 const NamespaceString& nsname,
                 const NamespaceString& viewOn,
                 const std::vector<BSONObj>& pipeline,
                 ErrorCodes::Error code) {
    AuditInterface::get(client->getServiceContext())
        ->logDropView(client, nsname, viewOn, pipeline, code);
}

void logDropDatabase(Client* client, const DatabaseName& dbname) {
    AuditInterface::get(client->getServiceContext())->logDropDatabase(client, dbname);
}

void logRenameCollection(Client* client,
                         const NamespaceString& source,
                         const NamespaceString& target) {
    AuditInterface::get(client->getServiceContext())->logRenameCollection(client, source, target);
}

void logEnableSharding(Client* client, std::string_view dbname) {
    AuditInterface::get(client->getServiceContext())->logEnableSharding(client, dbname);
}

void logAddShard(Client* client, std::string_view name, const std::string& servers) {
    AuditInterface::get(client->getServiceContext())->logAddShard(client, name, servers);
}

void logRemoveShard(Client* client, std::string_view shardname) {
    AuditInterface::get(client->getServiceContext())->logRemoveShard(client, shardname);
}

void logShardCollection(Client* client,
                        const NamespaceString& ns,
                        const BSONObj& keyPattern,
                        bool unique) {
    AuditInterface::get(client->getServiceContext())
        ->logShardCollection(client, ns, keyPattern, unique);
}

void logRefineCollectionShardKey(Client* client,
                                 const NamespaceString& ns,
                                 const BSONObj& keyPattern) {
    AuditInterface::get(client->getServiceContext())
        ->logRefineCollectionShardKey(client, ns, keyPattern);
}

void logInsertOperation(Client* client, const NamespaceString& nss, const BSONObj& doc) {
    AuditInterface::get(client->getServiceContext())->logInsertOperation(client, nss, doc);
}

void logUpdateOperation(Client* client, const NamespaceString& nss, const BSONObj& doc) {
    AuditInterface::get(client->getServiceContext())->logUpdateOperation(client, nss, doc);
}

void logRemoveOperation(Client* client, const NamespaceString& nss, const BSONObj& doc) {
    AuditInterface::get(client->getServiceContext())->logRemoveOperation(client, nss, doc);
}

void logGetClusterParameter(
    Client* client,
    const std::variant<std::string, std::vector<std::string>>& requestedParameters) {
    AuditInterface::get(client->getServiceContext())
        ->logGetClusterParameter(client, requestedParameters);
}

void logSetClusterParameter(Client* client,
                            const BSONObj& oldValue,
                            const BSONObj& newValue,
                            const boost::optional<TenantId>& tenantId) {
    AuditInterface::get(client->getServiceContext())
        ->logSetClusterParameter(client, oldValue, newValue, tenantId);
}

void logUpdateCachedClusterParameter(Client* client,
                                     const BSONObj& oldValue,
                                     const BSONObj& newValue,
                                     const boost::optional<TenantId>& tenantId) {
    AuditInterface::get(client->getServiceContext())
        ->logUpdateCachedClusterParameter(client, oldValue, newValue, tenantId);
}

void logRotateLog(Client* client,
                  const Status& logStatus,
                  const std::vector<Status>& errors,
                  const std::string& suffix) {
    // During startup, client hasn't been created. We get the serviceContext from the client when we
    // can
    if (client != nullptr) {
        AuditInterface::get(client->getServiceContext())
            ->logRotateLog(client, logStatus, errors, suffix);
    } else {
        AuditInterface::get(getGlobalServiceContext())
            ->logRotateLog(client, logStatus, errors, suffix);
    }
}

}  // namespace audit
}  // namespace mongo
