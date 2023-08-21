/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

/**
 * This module describes free functions for logging various operations of interest to a
 * party interested in generating logs of user activity in a MongoDB server instance.
 */

#pragma once

#include <boost/optional/optional.hpp>
#include <functional>
#include <set>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/mutable/document.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/auth/role_name.h"
#include "mongo/db/auth/user.h"
#include "mongo/db/auth/user_name.h"
#include "mongo/db/client.h"
#include "mongo/db/matcher/matchable.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/ops/write_ops.h"
#include "mongo/db/service_context.h"
#include "mongo/db/tenant_id.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/util/functional.h"

namespace mongo {
namespace audit {

class AuthenticateEvent;
class CommandInterface;
class AuditConfigDocument;

class AuditInterface {
    AuditInterface(const AuditInterface&) = delete;
    AuditInterface& operator=(const AuditInterface&) = delete;

public:
    static AuditInterface* get(ServiceContext* service);
    static void set(ServiceContext* service, std::unique_ptr<AuditInterface> interface);

    AuditInterface() = default;

    virtual ~AuditInterface() = default;

    /**
     * Logs the metadata for a client connection once it is finalized.
     */
    virtual void logClientMetadata(Client* client) const = 0;

    /**
     * Logs the result of an authentication attempt.
     */
    virtual void logAuthentication(Client* client, const AuthenticateEvent& event) const = 0;

    //
    // Authorization (authz) logging functions.
    //
    // These functions generate log messages describing the disposition of access control
    // checks.
    //

    /**
     * Logs the result of a command authorization check.
     */
    virtual void logCommandAuthzCheck(Client* client,
                                      const OpMsgRequest& cmdObj,
                                      const CommandInterface& command,
                                      ErrorCodes::Error result) const = 0;

    /**
     * Logs the result of an authorization check for a killCursors command.
     */
    virtual void logKillCursorsAuthzCheck(Client* client,
                                          const NamespaceString& ns,
                                          long long cursorId,
                                          ErrorCodes::Error result) const = 0;

    /**
     * Logs the result of a createUser command.
     */
    virtual void logCreateUser(Client* client,
                               const UserName& username,
                               bool password,
                               const BSONObj* customData,
                               const std::vector<RoleName>& roles,
                               const boost::optional<BSONArray>& restrictions) const = 0;

    /**
     * Logs the result of a dropUser command.
     */
    virtual void logDropUser(Client* client, const UserName& username) const = 0;

    /**
     * Logs the result of a dropAllUsersFromDatabase command.
     */
    virtual void logDropAllUsersFromDatabase(Client* client, const DatabaseName& dbname) const = 0;

    /**
     * Logs the result of a updateUser command.
     */
    virtual void logUpdateUser(Client* client,
                               const UserName& username,
                               bool password,
                               const BSONObj* customData,
                               const std::vector<RoleName>* roles,
                               const boost::optional<BSONArray>& restrictions) const = 0;

    /**
     * Logs the result of a grantRolesToUser command.
     */
    virtual void logGrantRolesToUser(Client* client,
                                     const UserName& username,
                                     const std::vector<RoleName>& roles) const = 0;

    /**
     * Logs the result of a revokeRolesFromUser command.
     */
    virtual void logRevokeRolesFromUser(Client* client,
                                        const UserName& username,
                                        const std::vector<RoleName>& roles) const = 0;

    /**
     * Logs the result of a createRole command.
     */
    virtual void logCreateRole(Client* client,
                               const RoleName& role,
                               const std::vector<RoleName>& roles,
                               const PrivilegeVector& privileges,
                               const boost::optional<BSONArray>& restrictions) const = 0;

    /**
     * Logs the result of a updateRole command.
     */
    virtual void logUpdateRole(Client* client,
                               const RoleName& role,
                               const std::vector<RoleName>* roles,
                               const PrivilegeVector* privileges,
                               const boost::optional<BSONArray>& restrictions) const = 0;

    /**
     * Logs the result of a dropRole command.
     */
    virtual void logDropRole(Client* client, const RoleName& role) const = 0;

    /**
     * Logs the result of a dropAllRolesForDatabase command.
     */
    virtual void logDropAllRolesFromDatabase(Client* client, const DatabaseName& dbname) const = 0;

    /**
     * Logs the result of a grantRolesToRole command.
     */
    virtual void logGrantRolesToRole(Client* client,
                                     const RoleName& role,
                                     const std::vector<RoleName>& roles) const = 0;

    /**
     * Logs the result of a revokeRolesFromRole command.
     */
    virtual void logRevokeRolesFromRole(Client* client,
                                        const RoleName& role,
                                        const std::vector<RoleName>& roles) const = 0;

    /**
     * Logs the result of a grantPrivilegesToRole command.
     */
    virtual void logGrantPrivilegesToRole(Client* client,
                                          const RoleName& role,
                                          const PrivilegeVector& privileges) const = 0;

    /**
     * Logs the result of a revokePrivilegesFromRole command.
     */
    virtual void logRevokePrivilegesFromRole(Client* client,
                                             const RoleName& role,
                                             const PrivilegeVector& privileges) const = 0;

    /**
     * Logs the result of a replSet(Re)config command.
     */
    virtual void logReplSetReconfig(Client* client,
                                    const BSONObj* oldConfig,
                                    const BSONObj* newConfig) const = 0;

    /**
     * Logs the result of an ApplicationMessage command.
     */
    virtual void logApplicationMessage(Client* client, StringData msg) const = 0;

    /**
     * Logs the options associated with a startup event.
     */
    virtual void logStartupOptions(Client* client, const BSONObj& startupOptions) const = 0;

    /**
     * Logs the result of a shutdown command.
     */
    virtual void logShutdown(Client* client) const = 0;

    /**
     * Logs the users authenticated to a session before and after a logout command.
     */
    virtual void logLogout(Client* client,
                           StringData reason,
                           const BSONArray& initialUsers,
                           const BSONArray& updatedUsers) const = 0;

    /**
     * Logs the result of a createIndex command.
     */
    virtual void logCreateIndex(Client* client,
                                const BSONObj* indexSpec,
                                StringData indexname,
                                const NamespaceString& nsname,
                                StringData indexBuildState,
                                ErrorCodes::Error result) const = 0;

    /**
     * Logs the result of a createCollection command.
     */
    virtual void logCreateCollection(Client* client, const NamespaceString& nsname) const = 0;

    /**
     * Logs the result of a createView command.
     */
    virtual void logCreateView(Client* client,
                               const NamespaceString& nsname,
                               StringData viewOn,
                               BSONArray pipeline,
                               ErrorCodes::Error code) const = 0;

    /**
     * Logs the result of an importCollection command.
     */
    virtual void logImportCollection(Client* client, const NamespaceString& nsname) const = 0;

    /**
     * Logs the result of a createDatabase command.
     */
    virtual void logCreateDatabase(Client* client, const DatabaseName& dbname) const = 0;


    /**
     * Logs the result of a dropIndex command.
     */
    virtual void logDropIndex(Client* client,
                              StringData indexname,
                              const NamespaceString& nsname) const = 0;

    /**
     * Logs the result of a dropCollection command on a collection.
     */
    virtual void logDropCollection(Client* client, const NamespaceString& nsname) const = 0;

    /**
     * Logs the result of a dropCollection command on a view.
     */
    virtual void logDropView(Client* client,
                             const NamespaceString& nsname,
                             StringData viewOn,
                             const std::vector<BSONObj>& pipeline,
                             ErrorCodes::Error code) const = 0;

    /**
     * Logs the result of a dropDatabase command.
     */
    virtual void logDropDatabase(Client* client, const DatabaseName& dbname) const = 0;

    /**
     * Logs a collection rename event.
     */
    virtual void logRenameCollection(Client* client,
                                     const NamespaceString& source,
                                     const NamespaceString& target) const = 0;

    /**
     * Logs the result of a enableSharding command.
     */
    virtual void logEnableSharding(Client* client, StringData dbname) const = 0;

    /**
     * Logs the result of a addShard command.
     */
    virtual void logAddShard(Client* client, StringData name, const std::string& servers) const = 0;

    /**
     * Logs the result of a removeShard command.
     */
    virtual void logRemoveShard(Client* client, StringData shardname) const = 0;

    /**
     * Logs the result of a shardCollection command.
     */
    virtual void logShardCollection(Client* client,
                                    StringData ns,
                                    const BSONObj& keyPattern,
                                    bool unique) const = 0;

    /**
     * Logs the result of a refineCollectionShardKey event.
     */
    virtual void logRefineCollectionShardKey(Client* client,
                                             StringData ns,
                                             const BSONObj& keyPattern) const = 0;

    /**
     * Logs an insert of a potentially security sensitive record.
     */
    virtual void logInsertOperation(Client* client,
                                    const NamespaceString& nss,
                                    const BSONObj& doc) const = 0;

    /**
     * Logs an update of a potentially security sensitive record.
     */
    virtual void logUpdateOperation(Client* client,
                                    const NamespaceString& nss,
                                    const BSONObj& doc) const = 0;

    /**
     * Logs a deletion of a potentially security sensitive record.
     */
    virtual void logRemoveOperation(Client* client,
                                    const NamespaceString& nss,
                                    const BSONObj& doc) const = 0;

    /**
     * Logs values of cluster server parameters requested via getClusterParameter.
     */
    virtual void logGetClusterParameter(
        Client* client,
        const stdx::variant<std::string, std::vector<std::string>>& requestedParameters) const = 0;

    /**
     * Logs old and new value of given tenant's cluster server parameter when it is updated via
     * setClusterParameter.
     */
    virtual void logSetClusterParameter(Client* client,
                                        const BSONObj& oldValue,
                                        const BSONObj& newValue,
                                        const boost::optional<TenantId>& tenantId) const = 0;

    /**
     * Logs old and new value of given tenant's cluster server parameter when it gets updated
     * in-memory in response to some on-disk change. This may be due to setClusterParameter or a
     * replication event such as rollback.
     */
    virtual void logUpdateCachedClusterParameter(
        Client* client,
        const BSONObj& oldValue,
        const BSONObj& newValue,
        const boost::optional<TenantId>& tenantId) const = 0;

    /**
     * Logs details of log file being rotated out to the file that is being rotated
     * in
     */
    virtual void logRotateLog(Client* client,
                              const Status& logStatus,
                              const std::vector<Status>& errors,
                              const std::string& suffix) const = 0;

    virtual void logConfigEvent(Client* client, const AuditConfigDocument& config) const = 0;


    /**
     * Base class of types representing events for writing to the audit log.
     */
    class AuditEvent : public MatchableDocument {
    public:
        using Serializer = std::function<void(BSONObjBuilder*)>;

        Date_t getTimestamp() const {
            return _ts;
        }

        BSONObj toBSON() const final {
            return _obj;
        }

        virtual StringData getTimestampFieldName() const = 0;

        ElementIterator* allocateIterator(const ElementPath* path) const final {
            if (_iteratorUsed) {
                return new BSONElementIterator(path, _obj);
            }

            _iteratorUsed = true;
            _iterator.reset(path, _obj);
            return &_iterator;
        }

        void releaseIterator(ElementIterator* iterator) const final {
            if (iterator == &_iterator) {
                _iteratorUsed = false;
            } else {
                delete iterator;
            }
        }

    private:
        AuditEvent& operator=(const AuditEvent&) = delete;

    protected:
        BSONObj _obj;
        mutable BSONElementIterator _iterator;
        mutable bool _iteratorUsed = false;

        Date_t _ts;
    };
};

struct TryLogEventParams {
    TryLogEventParams(Client* client,
                      ErrorCodes::Error code,
                      AuditInterface::AuditEvent::Serializer serializer)
        : client(client), code(code), serializer(serializer){};

    TryLogEventParams(Client* client,
                      ErrorCodes::Error code,
                      AuditInterface::AuditEvent::Serializer serializer,
                      boost::optional<TenantId> tenantId)
        : client(client), code(code), serializer(serializer), tenantId(tenantId){};

    Client* client;
    ErrorCodes::Error code;
    AuditInterface::AuditEvent::Serializer serializer;
    boost::optional<TenantId> tenantId;
};


class AuditNoOp : public AuditInterface {
public:
    AuditNoOp() = default;
    ~AuditNoOp() = default;

    void logClientMetadata(Client* client) const {};

    void logAuthentication(Client* client, const AuthenticateEvent& event) const {};

    void logCommandAuthzCheck(Client* client,
                              const OpMsgRequest& cmdObj,
                              const CommandInterface& command,
                              ErrorCodes::Error result) const {};

    void logKillCursorsAuthzCheck(Client* client,
                                  const NamespaceString& ns,
                                  long long cursorId,
                                  ErrorCodes::Error result) const {};

    void logCreateUser(Client* client,
                       const UserName& username,
                       bool password,
                       const BSONObj* customData,
                       const std::vector<RoleName>& roles,
                       const boost::optional<BSONArray>& restrictions) const {};

    void logDropUser(Client* client, const UserName& username) const {};

    void logDropAllUsersFromDatabase(Client* client, const DatabaseName& dbname) const {};

    void logUpdateUser(Client* client,
                       const UserName& username,
                       bool password,
                       const BSONObj* customData,
                       const std::vector<RoleName>* roles,
                       const boost::optional<BSONArray>& restrictions) const {};

    void logGrantRolesToUser(Client* client,
                             const UserName& username,
                             const std::vector<RoleName>& roles) const {};

    void logRevokeRolesFromUser(Client* client,
                                const UserName& username,
                                const std::vector<RoleName>& roles) const {};

    void logCreateRole(Client* client,
                       const RoleName& role,
                       const std::vector<RoleName>& roles,
                       const PrivilegeVector& privileges,
                       const boost::optional<BSONArray>& restrictions) const {};

    void logUpdateRole(Client* client,
                       const RoleName& role,
                       const std::vector<RoleName>* roles,
                       const PrivilegeVector* privileges,
                       const boost::optional<BSONArray>& restrictions) const {};

    void logDropRole(Client* client, const RoleName& role) const {};

    void logDropAllRolesFromDatabase(Client* client, const DatabaseName& dbname) const {};

    void logGrantRolesToRole(Client* client,
                             const RoleName& role,
                             const std::vector<RoleName>& roles) const {};

    void logRevokeRolesFromRole(Client* client,
                                const RoleName& role,
                                const std::vector<RoleName>& roles) const {};

    void logGrantPrivilegesToRole(Client* client,
                                  const RoleName& role,
                                  const PrivilegeVector& privileges) const {};

    void logRevokePrivilegesFromRole(Client* client,
                                     const RoleName& role,
                                     const PrivilegeVector& privileges) const {};

    void logReplSetReconfig(Client* client,
                            const BSONObj* oldConfig,
                            const BSONObj* newConfig) const {};

    void logApplicationMessage(Client* client, StringData msg) const {};

    void logStartupOptions(Client* client, const BSONObj& startupOptions) const {};

    void logShutdown(Client* client) const {};

    void logLogout(Client* client,
                   StringData reason,
                   const BSONArray& initialUsers,
                   const BSONArray& updatedUsers) const {};

    void logCreateIndex(Client* client,
                        const BSONObj* indexSpec,
                        StringData indexname,
                        const NamespaceString& nsname,
                        StringData indexBuildState,
                        ErrorCodes::Error result) const {};

    void logCreateCollection(Client* client, const NamespaceString& nsname) const {};

    void logCreateView(Client* client,
                       const NamespaceString& nsname,
                       StringData viewOn,
                       BSONArray pipeline,
                       ErrorCodes::Error code) const {};

    void logImportCollection(Client* client, const NamespaceString& nsname) const {};

    void logCreateDatabase(Client* client, const DatabaseName& dbname) const {};


    void logDropIndex(Client* client, StringData indexname, const NamespaceString& nsname) const {};

    void logDropCollection(Client* client, const NamespaceString& nsname) const {};

    void logDropView(Client* client,
                     const NamespaceString& nsname,
                     StringData viewOn,
                     const std::vector<BSONObj>& pipeline,
                     ErrorCodes::Error code) const {};

    void logDropDatabase(Client* client, const DatabaseName& dbname) const {};

    void logRenameCollection(Client* client,
                             const NamespaceString& source,
                             const NamespaceString& target) const {};

    void logEnableSharding(Client* client, StringData dbname) const {};

    void logAddShard(Client* client, StringData name, const std::string& servers) const {};

    void logRemoveShard(Client* client, StringData shardname) const {};

    void logShardCollection(Client* client,
                            StringData ns,
                            const BSONObj& keyPattern,
                            bool unique) const {};

    void logRefineCollectionShardKey(Client* client,
                                     StringData ns,
                                     const BSONObj& keyPattern) const {};

    void logInsertOperation(Client* client,
                            const NamespaceString& nss,
                            const BSONObj& doc) const {};

    void logUpdateOperation(Client* client,
                            const NamespaceString& nss,
                            const BSONObj& doc) const {};

    void logRemoveOperation(Client* client,
                            const NamespaceString& nss,
                            const BSONObj& doc) const {};

    void logGetClusterParameter(
        Client* client,
        const stdx::variant<std::string, std::vector<std::string>>& requestedParameters) const {};

    void logSetClusterParameter(Client* client,
                                const BSONObj& oldValue,
                                const BSONObj& newValue,
                                const boost::optional<TenantId>& tenantId) const {};

    void logUpdateCachedClusterParameter(Client* client,
                                         const BSONObj& oldValue,
                                         const BSONObj& newValue,
                                         const boost::optional<TenantId>& tenantId) const {};

    void logRotateLog(Client* client,
                      const Status& logStatus,
                      const std::vector<Status>& errors,
                      const std::string& suffix) const {};

    void logConfigEvent(Client* client, const AuditConfigDocument& config) const {};
};

}  // namespace audit
}  // namespace mongo
