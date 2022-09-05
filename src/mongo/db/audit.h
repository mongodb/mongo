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

/**
 * This module describes free functions for logging various operations of interest to a
 * party interested in generating logs of user activity in a MongoDB server instance.
 */

#pragma once

#include <functional>

#include "mongo/base/error_codes.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/auth/user.h"
#include "mongo/db/ops/write_ops.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/util/functional.h"

namespace mongo {

class AuthorizationSession;
class BSONObj;
class BSONObjBuilder;
class Client;
class NamespaceString;
class OperationContext;
class OpObserverRegistry;
class ServiceContext;
class StringData;
class UserName;

namespace mutablebson {
class Document;
}  // namespace mutablebson

namespace audit {

// AuditManager hooks.
extern std::function<void(OperationContext*)> initializeManager;
extern std::function<void(OpObserverRegistry*)> opObserverRegistrar;
extern std::function<void(ServiceContext*)> initializeSynchronizeJob;

/**
 * Struct that temporarily stores client information when an audit hook
 * executes on a separate thread with a new Client. In those cases, ImpersonatedClientAttrs
 * can bundle all relevant client attributes necessary for auditing and be safely
 * passed into the new thread, where the new Client will be loaded with the userNames and
 * roleNames stored in ImpersonatedClientAttrs.
 */
struct ImpersonatedClientAttrs {
    UserName userName;
    std::vector<RoleName> roleNames;

    ImpersonatedClientAttrs() = default;

    ImpersonatedClientAttrs(Client* client);
};

/**
 * Narrow API for the parts of mongo::Command used by the audit library.
 */
class CommandInterface {
public:
    virtual ~CommandInterface() = default;
    virtual std::set<StringData> sensitiveFieldNames() const = 0;
    virtual void snipForLogging(mutablebson::Document* cmdObj) const = 0;
    virtual StringData getName() const = 0;
    virtual NamespaceString ns() const = 0;
    virtual bool redactArgs() const = 0;
};

/**
 * Logs the metadata for a client connection once it is finalized.
 */
void logClientMetadata(Client* client);

/**
 * AuthenticateEvent is a opaque view into a finished authentication handshake.
 *
 * This object is only valid within its initial stack context.
 */
class AuthenticateEvent {
public:
    using Appender = unique_function<void(BSONObjBuilder*)>;

    AuthenticateEvent(StringData mechanism,
                      StringData db,
                      StringData user,
                      Appender appender,
                      ErrorCodes::Error result)
        : _mechanism(mechanism),
          _db(db),
          _user(user),
          _appender(std::move(appender)),
          _result(result) {}

    StringData getMechanism() const {
        return _mechanism;
    }

    StringData getDatabase() const {
        return _db;
    }

    StringData getUser() const {
        return _user;
    }

    ErrorCodes::Error getResult() const {
        return _result;
    }

    void appendExtraInfo(BSONObjBuilder* bob) const {
        _appender(bob);
    }

private:
    StringData _mechanism;
    StringData _db;
    StringData _user;

    Appender _appender;

    ErrorCodes::Error _result;
};

/**
 * Rotates the audit log in enterprise. Only to be called on startup.
 */
void rotateAuditLog();

/**
 * Logs the result of an authentication attempt.
 */
void logAuthentication(Client* client, const AuthenticateEvent& event);

//
// Authorization (authz) logging functions.
//
// These functions generate log messages describing the disposition of access control
// checks.
//

/**
 * Logs the result of a command authorization check.
 */
void logCommandAuthzCheck(Client* client,
                          const OpMsgRequest& cmdObj,
                          const CommandInterface& command,
                          ErrorCodes::Error result);

/**
 * Logs the result of an authorization check for a killCursors command.
 */
void logKillCursorsAuthzCheck(Client* client,
                              const NamespaceString& ns,
                              long long cursorId,
                              ErrorCodes::Error result);

/**
 * Logs the result of a createUser command.
 */
void logCreateUser(Client* client,
                   const UserName& username,
                   bool password,
                   const BSONObj* customData,
                   const std::vector<RoleName>& roles,
                   const boost::optional<BSONArray>& restrictions);

/**
 * Logs the result of a dropUser command.
 */
void logDropUser(Client* client, const UserName& username);

/**
 * Logs the result of a dropAllUsersFromDatabase command.
 */
void logDropAllUsersFromDatabase(Client* client, StringData dbname);

/**
 * Logs the result of a updateUser command.
 */
void logUpdateUser(Client* client,
                   const UserName& username,
                   bool password,
                   const BSONObj* customData,
                   const std::vector<RoleName>* roles,
                   const boost::optional<BSONArray>& restrictions);

/**
 * Logs the result of a grantRolesToUser command.
 */
void logGrantRolesToUser(Client* client,
                         const UserName& username,
                         const std::vector<RoleName>& roles);

/**
 * Logs the result of a revokeRolesFromUser command.
 */
void logRevokeRolesFromUser(Client* client,
                            const UserName& username,
                            const std::vector<RoleName>& roles);

/**
 * Logs the result of a createRole command.
 */
void logCreateRole(Client* client,
                   const RoleName& role,
                   const std::vector<RoleName>& roles,
                   const PrivilegeVector& privileges,
                   const boost::optional<BSONArray>& restrictions);

/**
 * Logs the result of a updateRole command.
 */
void logUpdateRole(Client* client,
                   const RoleName& role,
                   const std::vector<RoleName>* roles,
                   const PrivilegeVector* privileges,
                   const boost::optional<BSONArray>& restrictions);

/**
 * Logs the result of a dropRole command.
 */
void logDropRole(Client* client, const RoleName& role);

/**
 * Logs the result of a dropAllRolesForDatabase command.
 */
void logDropAllRolesFromDatabase(Client* client, StringData dbname);

/**
 * Logs the result of a grantRolesToRole command.
 */
void logGrantRolesToRole(Client* client, const RoleName& role, const std::vector<RoleName>& roles);

/**
 * Logs the result of a revokeRolesFromRole command.
 */
void logRevokeRolesFromRole(Client* client,
                            const RoleName& role,
                            const std::vector<RoleName>& roles);

/**
 * Logs the result of a grantPrivilegesToRole command.
 */
void logGrantPrivilegesToRole(Client* client,
                              const RoleName& role,
                              const PrivilegeVector& privileges);

/**
 * Logs the result of a revokePrivilegesFromRole command.
 */
void logRevokePrivilegesFromRole(Client* client,
                                 const RoleName& role,
                                 const PrivilegeVector& privileges);

/**
 * Logs the result of a replSet(Re)config command.
 */
void logReplSetReconfig(Client* client, const BSONObj* oldConfig, const BSONObj* newConfig);

/**
 * Logs the result of an ApplicationMessage command.
 */
void logApplicationMessage(Client* client, StringData msg);

/**
 * Logs the options associated with a startup event.
 */
void logStartupOptions(Client* client, const BSONObj& startupOptions);

/**
 * Logs the result of a shutdown command.
 */
void logShutdown(Client* client);

/**
 * Logs the users authenticated to a session before and after a logout command.
 */
void logLogout(Client* client,
               StringData reason,
               const BSONArray& initialUsers,
               const BSONArray& updatedUsers);

/**
 * Logs the result of a createIndex command.
 */
void logCreateIndex(Client* client,
                    const BSONObj* indexSpec,
                    StringData indexname,
                    const NamespaceString& nsname,
                    StringData indexBuildState,
                    ErrorCodes::Error result);

/**
 * Logs the result of a createCollection command.
 */
void logCreateCollection(Client* client, const NamespaceString& nsname);

/**
 * Logs the result of a createView command.
 */
void logCreateView(Client* client,
                   const NamespaceString& nsname,
                   StringData viewOn,
                   BSONArray pipeline,
                   ErrorCodes::Error code);

/**
 * Logs the result of an importCollection command.
 */
void logImportCollection(Client* client, const NamespaceString& nsname);

/**
 * Logs the result of a createDatabase command.
 */
void logCreateDatabase(Client* client, StringData dbname);


/**
 * Logs the result of a dropIndex command.
 */
void logDropIndex(Client* client, StringData indexname, const NamespaceString& nsname);

/**
 * Logs the result of a dropCollection command on a collection.
 */
void logDropCollection(Client* client, const NamespaceString& nsname);

/**
 * Logs the result of a dropCollection command on a view.
 */
void logDropView(Client* client,
                 const NamespaceString& nsname,
                 StringData viewOn,
                 const std::vector<BSONObj>& pipeline,
                 ErrorCodes::Error code);

/**
 * Logs the result of a dropDatabase command.
 */
void logDropDatabase(Client* client, StringData dbname);

/**
 * Logs a collection rename event.
 */
void logRenameCollection(Client* client,
                         const NamespaceString& source,
                         const NamespaceString& target);

/**
 * Logs the result of a enableSharding command.
 */
void logEnableSharding(Client* client, StringData dbname);

/**
 * Logs the result of a addShard command.
 */
void logAddShard(Client* client, StringData name, const std::string& servers);

/**
 * Logs the result of a removeShard command.
 */
void logRemoveShard(Client* client, StringData shardname);

/**
 * Logs the result of a shardCollection command.
 */
void logShardCollection(Client* client, StringData ns, const BSONObj& keyPattern, bool unique);

/**
 * Logs the result of a refineCollectionShardKey event.
 */
void logRefineCollectionShardKey(Client* client, StringData ns, const BSONObj& keyPattern);

/**
 * Logs an insert of a potentially security sensitive record.
 */
void logInsertOperation(Client* client, const NamespaceString& nss, const BSONObj& doc);

/**
 * Logs an update of a potentially security sensitive record.
 */
void logUpdateOperation(Client* client, const NamespaceString& nss, const BSONObj& doc);

/**
 * Logs a deletion of a potentially security sensitive record.
 */
void logRemoveOperation(Client* client, const NamespaceString& nss, const BSONObj& doc);

/**
 * Logs values of cluster server parameters requested via getClusterParameter.
 */
void logGetClusterParameter(
    Client* client,
    const stdx::variant<std::string, std::vector<std::string>>& requestedParameters);

/**
 * Logs old and new value of cluster server parameter when it is updated via setClusterParameter.
 */
void logSetClusterParameter(Client* client, const BSONObj& oldValue, const BSONObj& newValue);

/**
 * Logs old and new value of cluster server parameter when it gets updated in-memory in response to
 * some on-disk change. This may be due to setClusterParameter or a replication event such as
 * rollback.
 */
void logUpdateCachedClusterParameter(Client* client,
                                     const BSONObj& oldValue,
                                     const BSONObj& newValue);


}  // namespace audit
}  // namespace mongo
