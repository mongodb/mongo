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

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/oid.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/bson/util/builder.h"
#include "mongo/bson/util/builder_fwd.h"
#include "mongo/client/dbclient_cursor.h"
#include "mongo/config.h"  // IWYU pragma: keep
#include "mongo/crypto/mechanism_scram.h"
#include "mongo/crypto/sha1_block.h"
#include "mongo/crypto/sha256_block.h"
#include "mongo/db/audit.h"
#include "mongo/db/auth/action_set.h"
#include "mongo/db/auth/address_restriction.h"
#include "mongo/db/auth/auth_name.h"
#include "mongo/db/auth/auth_options_gen.h"
#include "mongo/db/auth/authorization_backend_interface.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/builtin_roles.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/auth/privilege_format.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/auth/role_name.h"
#include "mongo/db/auth/sasl_options.h"
#include "mongo/db/auth/umc_info_command_arg.h"
#include "mongo/db/auth/user.h"
#include "mongo/db/auth/user_document_parser.h"
#include "mongo/db/auth/user_management_commands_parser.h"
#include "mongo/db/auth/user_name.h"
#include "mongo/db/client.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/query_cmd/run_aggregate.h"
#include "mongo/db/commands/test_commands_enabled.h"
#include "mongo/db/commands/user_management_commands_common.h"
#include "mongo/db/commands/user_management_commands_gen.h"
#include "mongo/db/database_name.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/dbmessage.h"
#include "mongo/db/local_executor.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/aggregate_command_gen.h"
#include "mongo/db/pipeline/aggregation_request_helper.h"
#include "mongo/db/query/client_cursor/cursor_response.h"
#include "mongo/db/query/compiler/physical_model/query_solution/query_solution.h"
#include "mongo/db/query/find_command.h"
#include "mongo/db/query/write_ops/write_ops.h"
#include "mongo/db/query/write_ops/write_ops_gen.h"
#include "mongo/db/query/write_ops/write_ops_parsers.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
#include "mongo/db/session/logical_session_id_gen.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/db/tenant_id.h"
#include "mongo/db/topology/cluster_role.h"
#include "mongo/db/transaction/transaction_api.h"
#include "mongo/db/transaction/transaction_participant_resource_yielder.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/rpc/factory.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/rpc/op_msg_rpc_impls.h"
#include "mongo/rpc/reply_interface.h"
#include "mongo/s/write_ops/batched_command_response.h"
#include "mongo/stdx/mutex.h"
#include "mongo/stdx/unordered_set.h"
#include "mongo/transport/service_entry_point.h"
#include "mongo/transport/session.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/database_name_util.h"
#include "mongo/util/decorable.h"
#include "mongo/util/duration.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/future.h"
#include "mongo/util/icu.h"
#include "mongo/util/namespace_string_util.h"
#include "mongo/util/net/ssl_manager.h"
#include "mongo/util/net/ssl_options.h"
#include "mongo/util/net/ssl_types.h"
#include "mongo/util/password_digest.h"
#include "mongo/util/read_through_cache.h"
#include "mongo/util/sequence_util.h"
#include "mongo/util/str.h"
#include "mongo/util/time_support.h"
#include "mongo/util/uuid.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include <absl/container/node_hash_set.h>
#include <boost/cstdint.hpp>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kAccessControl

namespace mongo {

class auth::AuthorizationBackendInterface::CmdUMCPassthrough {
public:
    static Status rolesExist(OperationContext* opCtx, const std::vector<RoleName>& roleNames) {
        return auth::AuthorizationBackendInterface::get(opCtx->getService())
            ->rolesExist(opCtx, roleNames);
    }

    static UsersInfoReply lookupUsers(OperationContext* opCtx, const UsersInfoCommand& cmd) {
        return auth::AuthorizationBackendInterface::get(opCtx->getService())
            ->lookupUsers(opCtx, cmd);
    }

    static StatusWith<ResolvedRoleData> resolveRoles(OperationContext* opCtx,
                                                     const std::vector<RoleName>& roleNames,
                                                     ResolveRoleOption option) {
        return auth::AuthorizationBackendInterface::get(opCtx->getService())
            ->resolveRoles(opCtx, roleNames, option);
    }

    static Status getUserDescription(OperationContext* opCtx,
                                     const UserRequest& user,
                                     BSONObj* result,
                                     const SharedUserAcquisitionStats& userAcquisitionStats) {
        return auth::AuthorizationBackendInterface::get(opCtx->getService())
            ->getUserDescription(opCtx, user, result, userAcquisitionStats);
    }

    static StatusWith<User> getUserObject(OperationContext* opCtx,
                                          const UserRequest& userReq,
                                          const SharedUserAcquisitionStats& userAcquisitionStats) {
        return auth::AuthorizationBackendInterface::get(opCtx->getService())
            ->getUserObject(opCtx, userReq, userAcquisitionStats);
    }

    static RolesInfoReply lookupRoles(OperationContext* opCtx, const RolesInfoCommand& cmd) {
        return auth::AuthorizationBackendInterface::get(opCtx->getService())
            ->lookupRoles(opCtx, cmd);
    }
};

using CmdUMCPassthrough = auth::AuthorizationBackendInterface::CmdUMCPassthrough;

namespace {

constexpr auto kOne = "1"_sd;

Status useDefaultCode(const Status& status, ErrorCodes::Error defaultCode) {
    if (status.code() == ErrorCodes::UnknownError) {
        return Status(defaultCode, status.reason());
    }

    return status;
}

template <typename T>
StatusWith<T> useDefaultCode(StatusWith<T>&& status, ErrorCodes::Error defaultCode) {
    if (!status.isOK()) {
        return useDefaultCode(status.getStatus(), defaultCode);
    }

    return std::move(status);
}

template <typename Container>
BSONArray containerToBSONArray(const Container& container) {
    BSONArrayBuilder arrayBuilder;
    for (const auto& item : container) {
        arrayBuilder.append(item.toBSON());
    }
    return arrayBuilder.arr();
}

Status privilegeVectorToBSONArray(const PrivilegeVector& privileges, BSONArray* result) {
    // privileges may come in with non-unique ResourcePatterns.
    // Make a local copy so that ActionSets are merged.
    PrivilegeVector uniquePrivileges;
    Privilege::addPrivilegesToPrivilegeVector(&uniquePrivileges, privileges);
    *result = containerToBSONArray(uniquePrivileges);
    return Status::OK();
}

/**
 * Checks that every privilege being granted targets just the database the role is from, or that
 * the role is from the "admin" db.
 */
Status checkOkayToGrantPrivilegesToRole(const RoleName& role, const PrivilegeVector& privileges) {
    if (role.getDB() == "admin") {
        return Status::OK();
    }

    for (PrivilegeVector::const_iterator it = privileges.begin(); it != privileges.end(); ++it) {
        const ResourcePattern& resource = (*it).getResourcePattern();
        if ((resource.isDatabasePattern() || resource.isExactNamespacePattern()) &&
            (resource.dbNameToMatch() == role.getDatabaseName())) {
            continue;
        }

        return Status(ErrorCodes::InvalidRoleModification,
                      str::stream() << "Roles on the \'" << role.getDB()
                                    << "\' database cannot be granted privileges that target other "
                                       "databases or the cluster");
    }

    return Status::OK();
}

NamespaceString usersNSS(const boost::optional<TenantId>& tenant) {
    if (tenant) {
        return NamespaceString::makeTenantUsersCollection(tenant);
    } else {
        return NamespaceString::kAdminUsersNamespace;
    }
}

NamespaceString rolesNSS(const boost::optional<TenantId>& tenant) {
    if (tenant) {
        return NamespaceString::makeTenantRolesCollection(tenant);
    } else {
        return NamespaceString::kAdminRolesNamespace;
    }
}

/**
 * Inserts "document" into "collectionName".
 * If there is a duplicate key error, returns a Status with code DuplicateKey.
 *
 * Should only be called on collections with authorization documents in them
 * (ie admin.system.users and admin.system.roles).
 */
Status insertAuthzDocument(OperationContext* opCtx,
                           const NamespaceString& nss,
                           const BSONObj& document) try {
    DBDirectClient client(opCtx);

    // The sc is used to control serialization behavior executed on the request in client.insert,
    // and tenantIds should not be prefixed on the $db field.  Indicating that the request received
    // a tenantId from something other than a prefix, in this case the nss, will prevent prefixing.
    const auto vts = auth::ValidatedTenancyScope::get(opCtx);
    auto sc = SerializationContext::stateCommandRequest(
        nss.tenantId() != boost::none, vts != boost::none && vts->isFromAtlasProxy());

    write_ops::checkWriteErrors(
        client.insert(write_ops::InsertCommandRequest(nss, {document}, sc)));
    return Status::OK();
} catch (const DBException& e) {
    return e.toStatus();
}

/**
 * Updates documents matching "query" according to "updatePattern" in "collectionName".
 *
 * Should only be called on collections with authorization documents in them
 * (ie admin.system.users and admin.system.roles).
 */
StatusWith<std::int64_t> updateAuthzDocuments(OperationContext* opCtx,
                                              const NamespaceString& nss,
                                              const BSONObj& query,
                                              const BSONObj& updatePattern,
                                              bool upsert,
                                              bool multi) try {
    DBDirectClient client(opCtx);

    // The sc is used to control serialization behavior executed on the request in client.update,
    // and tenantIds should not be prefixed on the $db field.  Indicating that the request received
    // a tenantId from something other than a prefix, in this case the nss, will prevent prefixing.
    const auto vts = auth::ValidatedTenancyScope::get(opCtx);
    auto sc = SerializationContext::stateCommandRequest(
        nss.tenantId() != boost::none, vts != boost::none && vts->isFromAtlasProxy());

    auto result = client.update([&] {
        write_ops::UpdateCommandRequest updateOp(nss, sc);
        updateOp.setUpdates({[&] {
            write_ops::UpdateOpEntry entry;
            entry.setQ(query);
            entry.setU(write_ops::UpdateModification::parseFromClassicUpdate(updatePattern));
            entry.setMulti(multi);
            entry.setUpsert(upsert);
            return entry;
        }()});
        return updateOp;
    }());

    write_ops::checkWriteErrors(result);
    return result.getN();
} catch (const DBException& e) {
    return e.toStatus();
}

/**
 * Update one document matching "query" according to "updatePattern" in "collectionName".
 *
 * If "upsert" is true and no document matches "query", inserts one using "query" as a
 * template.
 * If "upsert" is false and no document matches "query", return a Status with the code
 * NoMatchingDocument.  The Status message in that case is not very descriptive and should
 * not be displayed to the end user.
 *
 * Should only be called on collections with authorization documents in them
 * (ie admin.system.users and admin.system.roles).
 */
Status updateOneAuthzDocument(OperationContext* opCtx,
                              const NamespaceString& collectionName,
                              const BSONObj& query,
                              const BSONObj& updatePattern,
                              bool upsert) {
    auto swNumMatched =
        updateAuthzDocuments(opCtx, collectionName, query, updatePattern, upsert, false);
    if (!swNumMatched.isOK()) {
        return swNumMatched.getStatus();
    }

    auto numMatched = swNumMatched.getValue();
    dassert(numMatched == 1 || numMatched == 0);
    if (numMatched == 0) {
        return {ErrorCodes::NoMatchingDocument, "No document found"};
    }

    return Status::OK();
}

/**
 * Removes all documents matching "query" from "collectionName".
 *
 * Should only be called on collections with authorization documents in them
 * (ie admin.system.users and admin.system.roles).
 */
StatusWith<std::int64_t> removeAuthzDocuments(OperationContext* opCtx,
                                              const NamespaceString& nss,
                                              const BSONObj& query) try {
    DBDirectClient client(opCtx);

    // The sc is used to control serialization behavior executed on the request in client.remove,
    // and tenantIds should not be prefixed on the $db field.  Indicating that the request received
    // a tenantId from something other than a prefix, in this case the nss, will prevent prefixing.
    const auto vts = auth::ValidatedTenancyScope::get(opCtx);
    auto sc = SerializationContext::stateCommandRequest(
        nss.tenantId() != boost::none, vts != boost::none && vts->isFromAtlasProxy());

    auto result = client.remove([&] {
        write_ops::DeleteCommandRequest deleteOp(nss, sc);
        deleteOp.setDeletes({[&] {
            write_ops::DeleteOpEntry entry;
            entry.setQ(query);
            entry.setMulti(true);
            return entry;
        }()});
        return deleteOp;
    }());

    write_ops::checkWriteErrors(result);
    return result.getN();
} catch (const DBException& e) {
    return e.toStatus();
}

/**
 * Creates the given role object in the given database.
 */
Status insertRoleDocument(OperationContext* opCtx,
                          const BSONObj& roleObj,
                          const boost::optional<TenantId>& tenant) {
    auto status = insertAuthzDocument(opCtx, rolesNSS(tenant), roleObj);
    if (status.isOK()) {
        return status;
    }

    if (status.code() == ErrorCodes::DuplicateKey) {
        std::string name = roleObj[AuthorizationManager::ROLE_NAME_FIELD_NAME].String();
        std::string source = roleObj[AuthorizationManager::ROLE_DB_FIELD_NAME].String();
        return Status(ErrorCodes::Error(51002),
                      str::stream() << "Role \"" << name << "@" << source << "\" already exists");
    }

    return useDefaultCode(status, ErrorCodes::RoleModificationFailed);
}

/**
 * Updates the given role object with the given update modifier.
 */
Status updateRoleDocument(OperationContext* opCtx, const RoleName& role, const BSONObj& updateObj) {
    Status status = updateOneAuthzDocument(
        opCtx,
        rolesNSS(role.tenantId()),
        BSON(AuthorizationManager::ROLE_NAME_FIELD_NAME
             << role.getRole() << AuthorizationManager::ROLE_DB_FIELD_NAME << role.getDB()),
        updateObj,
        false);
    if (status.isOK()) {
        return status;
    }

    if (status.code() == ErrorCodes::NoMatchingDocument) {
        return Status(ErrorCodes::RoleNotFound, str::stream() << "Role " << role << " not found");
    }

    return useDefaultCode(status, ErrorCodes::RoleModificationFailed);
}

/**
 * Removes roles matching the given query.
 * Writes into *numRemoved the number of role documents that were modified.
 */
StatusWith<std::int64_t> removeRoleDocuments(OperationContext* opCtx,
                                             const BSONObj& query,
                                             const boost::optional<TenantId>& tenant) {
    return useDefaultCode(removeAuthzDocuments(opCtx, rolesNSS(tenant), query),
                          ErrorCodes::RoleModificationFailed);
}

/**
 * Creates the given user object in the given database.
 */
Status insertPrivilegeDocument(OperationContext* opCtx,
                               const BSONObj& userObj,
                               const boost::optional<TenantId>& tenant = boost::none) {
    auto nss = usersNSS(tenant);
    Status status = insertAuthzDocument(opCtx, nss, userObj);
    if (status.isOK()) {
        return status;
    }
    if (status.code() == ErrorCodes::DuplicateKey) {
        std::string name = userObj[AuthorizationManager::USER_NAME_FIELD_NAME].String();
        std::string source = userObj[AuthorizationManager::USER_DB_FIELD_NAME].String();
        return Status(ErrorCodes::Error(51003),
                      str::stream() << "User \"" << name << "@" << source << "\" already exists");
    }
    if (status.code() == ErrorCodes::UnknownError) {
        return Status(ErrorCodes::UserModificationFailed, status.reason());
    }
    return status;
}

/**
 * Updates the given user object with the given update modifier.
 */
Status updatePrivilegeDocument(OperationContext* opCtx,
                               const UserName& user,
                               const BSONObj& queryObj,
                               const BSONObj& updateObj) {
    // Minimum fields required for an update.
    dassert(queryObj.hasField(AuthorizationManager::USER_NAME_FIELD_NAME));
    dassert(queryObj.hasField(AuthorizationManager::USER_DB_FIELD_NAME));

    const auto status =
        updateOneAuthzDocument(opCtx, usersNSS(user.tenantId()), queryObj, updateObj, false);

    if (status.code() == ErrorCodes::NoMatchingDocument) {
        return {ErrorCodes::UserNotFound, str::stream() << "User " << user << " not found"};
    }

    return useDefaultCode(status, ErrorCodes::UserModificationFailed);
}

/**
 * Convenience wrapper for above using only the UserName to match the original document.
 * Clarifies NoMatchingDocument result to reflect the user not existing.
 */
Status updatePrivilegeDocument(OperationContext* opCtx,
                               const UserName& user,
                               const BSONObj& updateObj) {
    return updatePrivilegeDocument(
        opCtx,
        user,
        BSON(AuthorizationManager::USER_NAME_FIELD_NAME
             << user.getUser() << AuthorizationManager::USER_DB_FIELD_NAME << user.getDB()),
        updateObj);
}

/**
 * Removes users for the given database matching the given query.
 * Writes into *numRemoved the number of user documents that were modified.
 */
StatusWith<std::int64_t> removePrivilegeDocuments(OperationContext* opCtx,
                                                  const BSONObj& query,
                                                  const boost::optional<TenantId>& tenant) {
    return useDefaultCode(removeAuthzDocuments(opCtx, usersNSS(tenant), query),
                          ErrorCodes::UserModificationFailed);
}

class AuthzLockGuard {
    AuthzLockGuard(AuthzLockGuard&) = delete;
    AuthzLockGuard& operator=(AuthzLockGuard&) = delete;

public:
    enum InvalidationMode { kInvalidate, kReadOnly };

    AuthzLockGuard(OperationContext* opCtx, InvalidationMode mode)
        : _opCtx(opCtx),
          _authzManager(AuthorizationManager::get(opCtx->getService())),
          _lock(_UMCMutexDecoration(opCtx->getServiceContext())),
          _mode(mode),
          _cacheGeneration(_authzManager->getCacheGeneration()) {}

    ~AuthzLockGuard() {
        if (!_lock.owns_lock() || _mode == kReadOnly) {
            return;
        }

        if (_authzManager->getCacheGeneration() == _cacheGeneration) {
            LOGV2_DEBUG(20509, 1, "User management command did not invalidate the user cache");
            AuthorizationManager::get(_opCtx->getService())->invalidateUserCache();
        }
    }

    AuthzLockGuard(AuthzLockGuard&&) = default;
    AuthzLockGuard& operator=(AuthzLockGuard&&) = default;

private:
    static Decorable<ServiceContext>::Decoration<stdx::mutex> _UMCMutexDecoration;

    OperationContext* _opCtx;
    AuthorizationManager* _authzManager;
    stdx::unique_lock<stdx::mutex> _lock;
    InvalidationMode _mode;
    OID _cacheGeneration;
};

Decorable<ServiceContext>::Decoration<stdx::mutex> AuthzLockGuard::_UMCMutexDecoration =
    ServiceContext::declareDecoration<stdx::mutex>();

/**
 * When executing a UMC that requires writes, this function must be called so that a lock is taken
 * that invalidates the user cache when it is released.
 */
StatusWith<AuthzLockGuard> getWritableAuthzLock(OperationContext* opCtx) {
    AuthzLockGuard lk(opCtx, AuthzLockGuard::kInvalidate);
    return std::move(lk);
}

/**
 * When executing a read-only UMC, this function must be called so that it can be synchronized
 * without necessarily invalidating the user cache afterwards.
 */
StatusWith<AuthzLockGuard> getReadOnlyAuthzLock(OperationContext* opCtx) {
    AuthzLockGuard lk(opCtx, AuthzLockGuard::kReadOnly);
    return std::move(lk);
}

template <typename T>
void buildCredentials(BSONObjBuilder* builder, const UserName& userName, const T& cmd) {
    if (cmd.getPwd() == boost::none) {
        // Must be external user.
        builder->append("external", true);
        return;
    }

    bool buildSCRAMSHA1 = false, buildSCRAMSHA256 = false;
    if (auto mechanisms = cmd.getMechanisms(); mechanisms && !mechanisms->empty()) {
        for (const auto& mech : mechanisms.get()) {
            if (mech == "SCRAM-SHA-1") {
                buildSCRAMSHA1 = true;
            } else if (mech == "SCRAM-SHA-256") {
                buildSCRAMSHA256 = true;
            } else {
                uasserted(ErrorCodes::BadValue,
                          str::stream() << "Unknown auth mechanism '" << mech << "'");
            }

            uassert(ErrorCodes::BadValue,
                    str::stream() << mech << " not supported in authMechanisms",
                    sequenceContains(saslGlobalParams.authenticationMechanisms, mech));
        }

    } else {
        buildSCRAMSHA1 = sequenceContains(saslGlobalParams.authenticationMechanisms, "SCRAM-SHA-1");
        buildSCRAMSHA256 =
            sequenceContains(saslGlobalParams.authenticationMechanisms, "SCRAM-SHA-256");
    }

    auto password = cmd.getPwd().get();
    const bool digestPassword = cmd.getDigestPassword();

    if (buildSCRAMSHA1) {
        // Add SCRAM-SHA-1 credentials.
        std::string hashedPwd;
        if (digestPassword) {
            hashedPwd = createPasswordDigest(userName.getUser(), password);
        } else {
            hashedPwd = std::string{password};
        }
        auto sha1Cred = scram::Secrets<SHA1Block>::generateCredentials(
            hashedPwd, saslGlobalParams.scramSHA1IterationCount.load());
        builder->append("SCRAM-SHA-1", sha1Cred);
    }

    if (buildSCRAMSHA256) {
        uassert(ErrorCodes::BadValue,
                "Use of SCRAM-SHA-256 requires undigested passwords",
                digestPassword);

        auto prepPwd = uassertStatusOK(icuSaslPrep(password));
        auto sha256Cred = scram::Secrets<SHA256Block>::generateCredentials(
            prepPwd, saslGlobalParams.scramSHA256IterationCount.load());
        builder->append("SCRAM-SHA-256", sha256Cred);
    }
}

template <typename T>
BSONArray vectorToBSON(const std::vector<T>& vec) {
    BSONArrayBuilder builder;
    for (const auto& val : vec) {
        builder.append(val.toBSON());
    }
    return builder.arr();
}

MONGO_FAIL_POINT_DEFINE(umcTransaction);


/**
 * Generic API for a client that runs CRUD operations in a UMCTransaction's
 * callback lambda. The replica set impl falls through to TransactionClient
 * while the standalone uses its own client to issue the requested commands on
 * itself.
 */
class UMCTransactionClient {
public:
    UMCTransactionClient() = delete;

    UMCTransactionClient(StringData cmdName) : _cmdName(cmdName) {}

    virtual BatchedCommandResponse runCRUDOp(const BatchedCommandRequest& request,
                                             std::vector<StmtId> stmtIds) = 0;

protected:
    std::string _cmdName;
};

class UMCTransaction {
public:
    UMCTransaction() = delete;

    UMCTransaction(StringData cmdName) : _cmdName(cmdName) {}

    virtual void run(OperationContext* opCtx,
                     unique_function<Status(UMCTransactionClient&)> txnOpsCallback) = 0;


protected:
    std::string _cmdName;
};

/**
 * Wrapper class to handle "transactional" UMCs on standalones. Since transactions are
 * not supported on standalones, this is simply a way to sequentially run all of the CRUD ops
 * specified in txnOpsCallback with some retries built-in.
 */
class UMCTransactionStandalone : public UMCTransaction {
private:
    class UMCTransactionClientStandalone : public UMCTransactionClient {
    public:
        static constexpr StringData kAdminDB = "admin"_sd;

        explicit UMCTransactionClientStandalone(OperationContext* opCtx, StringData cmdName)
            : UMCTransactionClient(cmdName),
              _client(opCtx->getServiceContext()
                          ->getService(ClusterRole::ShardServer)
                          ->makeClient(std::string{cmdName})),
              _writeConcern(opCtx->getWriteConcern().toBSON().removeField(
                  ReadWriteConcernProvenanceBase::kSourceFieldName)) {
            _vts = auth::ValidatedTenancyScope::get(opCtx);
            const auto tenantId = _vts && _vts->hasTenantId()
                ? boost::optional<TenantId>(_vts->tenantId())
                : boost::none;
            const auto serializationContext = _vts
                ? SerializationContext::stateCommandRequest(_vts->hasTenantId(),
                                                            _vts->isFromAtlasProxy())
                : SerializationContext::stateCommandRequest();
            _dbName = DatabaseNameUtil::deserialize(tenantId, kAdminDB, serializationContext);
            auto* as = AuthorizationSession::get(_client.get());
            if (as) {
                as->grantInternalAuthorization();
            }
        }

        BatchedCommandResponse runCRUDOp(const BatchedCommandRequest& request,
                                         std::vector<StmtId> stmtIds) final {
            // Set a default apiVersion for all UMC commands and propagate the writeConcern
            // from the parent opCtx provided at construction.
            BSONObjBuilder cmdBuilder;
            request.serialize(&cmdBuilder);
            cmdBuilder.append("apiVersion", kOne);
            cmdBuilder.append(WriteConcernOptions::kWriteConcernField, _writeConcern);

            // Convert the command into a Message that can be sent directly to the shard
            // ServiceEntryPoint's request handler.
            auto requestMessage =
                OpMsgRequestBuilder::create(_vts, DatabaseName::kAdmin, cmdBuilder.obj())
                    .serialize();

            auto* serviceContext = _client->getServiceContext();
            auto* serviceEntryPoint =
                serviceContext->getService(ClusterRole::ShardServer)->getServiceEntryPoint();

            AlternativeClientRegion altClientRegion(_client);
            auto subOpCtx = serviceContext->makeOperationContext(Client::getCurrent());
            auth::ValidatedTenancyScope::set(subOpCtx.get(), _vts);
            auto responseMessage = serviceEntryPoint
                                       ->handleRequest(subOpCtx.get(),
                                                       requestMessage,
                                                       subOpCtx.get()->fastClockSource().now())
                                       .get()
                                       .response;

            auto replyObj = rpc::makeReply(&responseMessage)->getCommandReply().getOwned();
            uassertStatusOK(getStatusFromWriteCommandReply(replyObj));

            BatchedCommandResponse response;
            std::string errorMessage;
            uassert(ErrorCodes::FailedToParse,
                    errorMessage,
                    response.parseBSON(replyObj, &errorMessage));

            return response;
        }

    private:
        ServiceContext::UniqueClient _client;
        BSONObj _writeConcern;
        DatabaseName _dbName;
        boost::optional<auth::ValidatedTenancyScope> _vts;
    };

public:
    UMCTransactionStandalone(StringData cmdName) : UMCTransaction(cmdName) {}

    void run(OperationContext* opCtx,
             unique_function<Status(UMCTransactionClient&)> txnOpsCallback) final {
        // In practice this status never makes it to a return
        // since its populated with the return from txnOpsCallback(),
        // but guard against bit-rot by pre-populating a generic failure.
        Status status(ErrorCodes::OperationFailed, "Operation was never attempted");

        // Be more patient with our test runner.
        const int kMaxAttempts = getTestCommandsEnabled() ? 10 : 3;

        for (int tries = kMaxAttempts; tries > 0; tries--) {
            if (tries < kMaxAttempts) {
                // Emit log on all but the first attempt.
                LOGV2_DEBUG(5297200,
                            4,
                            "Retrying user management command transaction on standalone",
                            "command"_attr = _cmdName,
                            "reason"_attr = status);
            }

            // On standalones, simply run the callback inline without transactionalizing it.
            UMCTransactionClientStandalone umcTxnClient(opCtx, _cmdName);
            status = txnOpsCallback(umcTxnClient);

            // If we receive Status::OK() or a non-retryable error, break out of the loop so
            // we can assert on it being OK at the end of the method. Otherwise, allow the
            // loop to reiterate without incident if the max retry limit hasn't been reached yet.
            if (!shouldRetryTransaction(status)) {
                break;
            }
        }

        uassertStatusOK(status);
    }

private:
    bool shouldRetryTransaction(const Status& status) {
        return (status == ErrorCodes::LockTimeout) || (status == ErrorCodes::SnapshotUnavailable);
    }
};

/**
 * Wrapper class to manage UMC transactions on replica sets. It hooks into the transaction API
 * via SyncTransactionWithRetries.
 */
class UMCTransactionReplSet : public UMCTransaction {
private:
    class UMCTransactionClientReplSet : public UMCTransactionClient {
    public:
        explicit UMCTransactionClientReplSet(StringData cmdName,
                                             const txn_api::TransactionClient& client)
            : UMCTransactionClient(cmdName), _txnClient(client) {}
        UMCTransactionClientReplSet(const txn_api::TransactionClient&&) = delete;

        BatchedCommandResponse runCRUDOp(const BatchedCommandRequest& request,
                                         std::vector<StmtId> stmtIds) final {
            return _txnClient.runCRUDOpSync(request, stmtIds);
        }

    private:
        const txn_api::TransactionClient& _txnClient;
    };

public:
    UMCTransactionReplSet(StringData cmdName) : UMCTransaction(cmdName) {}

    void run(OperationContext* opCtx,
             unique_function<Status(UMCTransactionClient&)> txnOpsCallback) final {
        auto inlineExecutor = std::make_shared<executor::InlineExecutor>();
        auto sleepAndCleanupExecutor = getLocalExecutor(opCtx);

        // Constructing a SyncTransactionWithRetries causes it to store the write concern from the
        // supplied OperationContext and then wait for that write concern when running/committing
        // the transaction. If transaction successfully commits locally but fails due to a write
        // concern error, it throws, which subsequently can cause the writeConcern error to be
        // reported as a command-level error. That may not necessarily be true as the command's
        // writes may have been applied successfully, but just not yet replicated to enough nodes as
        // the requested write concern.

        // To avoid this, we swap in {w: 1} to the OperationContext's write concern so that it never
        // throws due to a failed write concern. Once the transaction completes, we will update the
        // OperationContext's Client's ReplClientInfo decoration to the latest system OpTime,
        // causing it to recognize that a write was performed which may need to be waited on for
        // replication to satisfy the requested write concern.
        auto originalWC = opCtx->getWriteConcern();
        ScopeGuard resetWCGuard([&] { opCtx->setWriteConcern(originalWC); });
        opCtx->setWriteConcern(WriteConcernOptions());
        auto txn = txn_api::SyncTransactionWithRetries(
            opCtx,
            sleepAndCleanupExecutor,
            TransactionParticipantResourceYielder::make("UMCTransaction"),
            inlineExecutor);
        const auto txnCallback = [this,
                                  &txnOpsCallback](const txn_api::TransactionClient& txnClient,
                                                   ExecutorPtr executor) {
            UMCTransactionClientReplSet umcTxnClient(_cmdName, txnClient);
            return SemiFuture<void>::makeReady(txnOpsCallback(umcTxnClient));
        };

        txn.run(opCtx, std::move(txnCallback));

        // SyncTransactionWithRetries advances opCtx's Client's lastProxyWriteTimestamp but leaves
        // its lastOpTime alone. Since all of the transaction's writes were conducted by a different
        // Client, opCtx's Client's lastOpTime must be updated to indicate that this Client was
        // responsible for some writes. Ideally, we would use the exact OpTime corresponding to the
        // no-op write performed when the transaction was committed, but since that is not yet
        // available, we set it to the latest opTime in the system right now. That opTime must be >=
        // commitOpTime, ensuring that the calling Client will wait at least until commitOpTime has
        // been replicated to enough nodes to satisfy its write concern.

        // TODO SERVER-98338 Change this to use ReplClientInfo::setLastOp(opTime) once
        // SyncTransactionWithRetries is able to report the OpTime of the transaction commit write.
        repl::ReplClientInfo::forClient(opCtx->getClient()).setLastOpToSystemLastOpTime(opCtx);
    }
};

void uassertNoUnrecognizedActions(const std::vector<std::string>& unrecognizedActions) {
    if (unrecognizedActions.empty()) {
        return;
    }

    // Dedupe
    std::set<StringData> actions;
    for (const auto& action : unrecognizedActions) {
        actions.insert(StringData{action});
    }

    StringBuilder sb;
    sb << "Unknown action type";
    if (actions.size() > 1) {
        sb << 's';
    }
    sb << " in privilege set:";
    for (const auto& action : actions) {
        sb << " '" << action << "',";
    }

    // Trim last comma off.
    auto msg = sb.str();
    msg.pop_back();

    uasserted(ErrorCodes::BadValue, msg);
}

enum class SupportTenantOption {
    kNever,
    kTestOnly,
    kAlways,
};

// Used by most UMC commands.
struct UMCStdParams {
    static constexpr bool adminOnly = false;
    static constexpr bool supportsWriteConcern = true;
    static constexpr auto allowedOnSecondary = BasicCommand::AllowedOnSecondary::kNever;
    static constexpr bool skipApiVersionCheck = false;
    static constexpr auto supportTenant = SupportTenantOption::kTestOnly;
};

// Used by {usersInfo:...} and {rolesInfo:...}
struct UMCInfoParams {
    static constexpr bool adminOnly = false;
    static constexpr bool supportsWriteConcern = false;
    static constexpr auto allowedOnSecondary = BasicCommand::AllowedOnSecondary::kOptIn;
    static constexpr bool skipApiVersionCheck = false;
    static constexpr auto supportTenant = SupportTenantOption::kAlways;
};

// Used by {invalidateUserCache:...}
struct UMCInvalidateUserCacheParams {
    static constexpr bool adminOnly = false;
    static constexpr bool supportsWriteConcern = false;
    static constexpr auto allowedOnSecondary = BasicCommand::AllowedOnSecondary::kAlways;
    static constexpr bool skipApiVersionCheck = false;
    static constexpr auto supportTenant = SupportTenantOption::kAlways;
};

// Used by {_getUserCacheGeneration:...}
struct UMCGetUserCacheGenParams {
    static constexpr bool adminOnly = true;
    static constexpr bool supportsWriteConcern = false;
    static constexpr auto allowedOnSecondary = BasicCommand::AllowedOnSecondary::kAlways;
    static constexpr bool skipApiVersionCheck = true;
    static constexpr auto supportTenant = SupportTenantOption::kNever;
};

template <typename T>
using HasGetCmdParamOp = std::remove_cv_t<decltype(std::declval<T>().getCommandParameter())>;
template <typename T>
constexpr bool hasGetCmdParamStringData =
    stdx::is_detected_exact_v<StringData, HasGetCmdParamOp, T>;


using ResolvedRoleData = auth::AuthorizationBackendInterface::ResolvedRoleData;
using ResolveRoleOption = auth::AuthorizationBackendInterface::ResolveRoleOption;

template <typename RequestT, typename Params = UMCStdParams>
class CmdUMCTyped : public TypedCommand<CmdUMCTyped<RequestT, Params>> {
public:
    using Request = RequestT;
    using Reply = typename RequestT::Reply;
    using TC = TypedCommand<CmdUMCTyped<RequestT, Params>>;

    class Invocation final : public TC::InvocationBase {
    public:
        using TC::InvocationBase::InvocationBase;
        using TC::InvocationBase::request;

        Reply typedRun(OperationContext* opCtx);

    private:
        bool supportsWriteConcern() const final {
            return Params::supportsWriteConcern;
        }

        void doCheckAuthorization(OperationContext* opCtx) const final {
            auth::checkAuthForTypedCommand(opCtx, request());
        }

        NamespaceString ns() const final {
            const auto& cmd = request();
            if constexpr (hasGetCmdParamStringData<RequestT>) {
                return NamespaceStringUtil::deserialize(cmd.getDbName(), cmd.getCommandParameter());
            }
            return NamespaceString(cmd.getDbName());
        }
    };

    bool skipApiVersionCheck() const final {
        return Params::skipApiVersionCheck;
    }

    bool adminOnly() const final {
        return Params::adminOnly;
    }

    typename TC::AllowedOnSecondary secondaryAllowed(ServiceContext*) const final {
        return Params::allowedOnSecondary;
    }

    bool allowedWithSecurityToken() const final {
        switch (Params::supportTenant) {
            case SupportTenantOption::kAlways:
                return true;
            case SupportTenantOption::kTestOnly:
                return getTestCommandsEnabled();
            case SupportTenantOption::kNever:
                return false;
        }

        MONGO_UNREACHABLE;
        return false;
    }

    // Since the user management commands do not affect user data, we should allow these commands
    // even if the user does not have the direct shard operations action type.
    bool shouldSkipDirectConnectionChecks() const final {
        return true;
    }
};

/**
 * Checks that every role in "rolesToAdd" exists, that adding each of those roles to "role"
 * will not result in a cycle to the role graph, and that every role being added comes from the
 * same database as the role it is being added to (or that the role being added to is from the
 * "admin" database.
 */
Status checkOkayToGrantRolesToRole(OperationContext* opCtx,
                                   const RoleName& role,
                                   const std::vector<RoleName>& rolesToAdd) {
    for (const auto& roleToAdd : rolesToAdd) {
        if (roleToAdd == role) {
            return {ErrorCodes::InvalidRoleModification,
                    str::stream() << "Cannot grant role " << role << " to itself."};
        }

        if (role.getDB() != "admin" && roleToAdd.getDB() != role.getDB()) {
            return {ErrorCodes::InvalidRoleModification,
                    str::stream() << "Roles on the \'" << role.getDB()
                                  << "\' database cannot be granted roles from other databases"};
        }
    }

    auto status = CmdUMCPassthrough::rolesExist(opCtx, rolesToAdd);
    if (!status.isOK()) {
        return {status.code(),
                str::stream() << "Cannot grant roles to '" << role << "': " << status.reason()};
    }

    auto swData = CmdUMCPassthrough::resolveRoles(
        opCtx, rolesToAdd, auth::AuthorizationBackendInterface::ResolveRoleOption::kRoles());
    if (!swData.isOK()) {
        return {swData.getStatus().code(),
                str::stream() << "Cannot grant roles to '" << role
                              << "': " << swData.getStatus().reason()};
    }

    if (sequenceContains(swData.getValue().roles.get(), role)) {
        return {ErrorCodes::InvalidRoleModification,
                str::stream() << "Granting roles to " << role
                              << " would introduce a cycle in the role graph"};
    }

    return Status::OK();
}

/**
 * Used to get all current roles of the user identified by 'userName'.
 */
Status getCurrentUserRoles(OperationContext* opCtx,
                           const UserName& userName,
                           stdx::unordered_set<RoleName>* roles) {

    auto userAcquisitionStats = CurOp::get(opCtx)->getUserAcquisitionStats();
    BSONObj result;
    std::unique_ptr<UserRequest> request =
        std::make_unique<UserRequestGeneral>(userName, boost::none);

    auto swUser = CmdUMCPassthrough::getUserObject(opCtx, *request.get(), userAcquisitionStats);

    if (!swUser.isOK()) {
        return swUser.getStatus();
    }

    User user(std::move(swUser.getValue()));

    RoleNameIterator rolesIt = user.getRoles();
    while (rolesIt.more()) {
        roles->insert(rolesIt.next());
    }
    return Status::OK();
}

void trimCredentials(OperationContext* opCtx,
                     const UserName& userName,
                     BSONObjBuilder* queryBuilder,
                     BSONObjBuilder* unsetBuilder,
                     const std::vector<StringData>& mechanisms) {
    BSONObj userObj;
    auto sharedAcquisitionStats = CurOp::get(opCtx)->getUserAcquisitionStats();
    auto userReq = std::make_unique<UserRequestGeneral>(userName, boost::none);
    uassertStatusOK(CmdUMCPassthrough::getUserDescription(
        opCtx, *userReq.get(), &userObj, sharedAcquisitionStats));

    const auto& credsElem = userObj["credentials"];
    uassert(ErrorCodes::UnsupportedFormat,
            "Unable to trim credentials from a user document with no credentials",
            credsElem.type() == BSONType::object);

    const auto& creds = credsElem.Obj();
    queryBuilder->append("credentials", creds);

    bool keepSCRAMSHA1 = false, keepSCRAMSHA256 = false;
    for (const auto& mech : mechanisms) {
        uassert(ErrorCodes::BadValue,
                "mechanisms field must be a subset of previously set mechanisms",
                creds.hasField(mech));

        if (mech == "SCRAM-SHA-1") {
            keepSCRAMSHA1 = true;
        } else if (mech == "SCRAM-SHA-256") {
            keepSCRAMSHA256 = true;
        }
    }

    uassert(ErrorCodes::BadValue,
            "mechanisms field must contain at least one previously set known mechanism",
            keepSCRAMSHA1 || keepSCRAMSHA256);

    if (!keepSCRAMSHA1) {
        unsetBuilder->append("credentials.SCRAM-SHA-1", "");
    }
    if (!keepSCRAMSHA256) {
        unsetBuilder->append("credentials.SCRAM-SHA-256", "");
    }
}

class CmdCreateUser : public CmdUMCTyped<CreateUserCommand> {
public:
    static constexpr StringData kPwdField = "pwd"_sd;

    std::set<StringData> sensitiveFieldNames() const final {
        return {kPwdField};
    }
};
MONGO_REGISTER_COMMAND(CmdCreateUser).forShard();

template <>
void CmdUMCTyped<CreateUserCommand>::Invocation::typedRun(OperationContext* opCtx) {
    const auto& cmd = request();
    auto dbname = cmd.getDbName();

    // Validate input
    uassert(ErrorCodes::BadValue,
            "Cannot create users in the local database",
            dbname != DatabaseName::kLocal);

    uassert(ErrorCodes::BadValue,
            "Username cannot contain NULL characters",
            cmd.getCommandParameter().find('\0') == std::string::npos);
    UserName userName(cmd.getCommandParameter(), dbname);

    const bool isExternal = dbname.isExternalDB();
    uassert(ErrorCodes::BadValue,
            "Must provide a 'pwd' field for all user documents, except those"
            " with '$external' as the user's source db",
            (cmd.getPwd() != boost::none) || isExternal);

    uassert(ErrorCodes::BadValue,
            "Cannot set the password for users defined on the '$external' database",
            (cmd.getPwd() == boost::none) || !isExternal);

    uassert(ErrorCodes::BadValue,
            "mechanisms field must not be empty",
            (cmd.getMechanisms() == boost::none) || !cmd.getMechanisms()->empty());

#ifdef MONGO_CONFIG_SSL
    // An internal caller of 'createUser' won't have a transport session bound to the client.
    // Instead, we should retrieve the SSL manager from the SSLManagerCoordinator.
    auto sslConfig = [&]() -> const SSLConfiguration* {
        const auto& session = opCtx->getClient()->session();
        if (session) {
            return session->getSSLConfiguration();
        }
        // If SSL is supported but disabled, the SSLManagerCoordinator will not exist. We should
        // return an empty pointer.
        const auto sslCoord = SSLManagerCoordinator::get();
        if (!sslCoord) {
            return nullptr;
        }
        return &sslCoord->getSSLManager()->getSSLConfiguration();
    }();
    if (isExternal && sslConfig && sslGlobalParams.clusterAuthX509ExtensionValue.empty() &&
        sslConfig->isClusterMember(userName.getUser(), boost::none /* clusterExtensionValue */)) {
        if (gEnforceUserClusterSeparation) {
            uasserted(ErrorCodes::BadValue,
                      "Cannot create an x.509 user with a subjectname that would be "
                      "recognized as an internal cluster member");
        } else {
            LOGV2(4593800,
                  "Creating user which would be considered a cluster member if clusterAuthMode "
                  "enabled X509 authentication",
                  "user"_attr = userName);
        }
    }
#endif

    // Synthesize a user document
    BSONObjBuilder userObjBuilder;
    userObjBuilder.append("_id", userName.getUnambiguousName());
    UUID::gen().appendToBuilder(&userObjBuilder, AuthorizationManager::USERID_FIELD_NAME);
    userName.appendToBSON(&userObjBuilder);

    auto lk = uassertStatusOK(getWritableAuthzLock(opCtx));

    BSONObjBuilder credentialsBuilder(userObjBuilder.subobjStart("credentials"));
    buildCredentials(&credentialsBuilder, userName, cmd);
    credentialsBuilder.done();

    if (auto ar = cmd.getAuthenticationRestrictions(); ar && !ar->empty()) {
        userObjBuilder.append("authenticationRestrictions", vectorToBSON(ar.get()));
    }

    if (auto customData = cmd.getCustomData(); customData) {
        userObjBuilder.append("customData", customData.get());
    }

    auto resolvedRoles = auth::resolveRoleNames(cmd.getRoles(), dbname);
    userObjBuilder.append("roles", vectorToBSON(resolvedRoles));
    BSONObj userObj = userObjBuilder.obj();

    // Validate contents
    uassertStatusOK(V2UserDocumentParser().checkValidUserDocument(userObj));

    // Role existence has to be checked after acquiring the update lock
    uassertStatusOK(CmdUMCPassthrough::rolesExist(opCtx, resolvedRoles));

    // Audit this event.
    auto optCustomData = cmd.getCustomData();
    BSONArray authRestrictionsArray;
    if (auto ar = cmd.getAuthenticationRestrictions()) {
        authRestrictionsArray = vectorToBSON(ar.get());
    }
    audit::logCreateUser(opCtx->getClient(),
                         userName,
                         cmd.getPwd() != boost::none,
                         optCustomData ? &(optCustomData.get()) : nullptr,
                         resolvedRoles,
                         authRestrictionsArray);

    // Must invalidate even on bad status
    auto status = insertPrivilegeDocument(opCtx, userObj, userName.tenantId());
    AuthorizationManager::get(opCtx->getService())->invalidateUserByName(userName);
    uassertStatusOK(status);
}

class CmdUpdateUser : public CmdUMCTyped<UpdateUserCommand> {
public:
    static constexpr StringData kPwdField = "pwd"_sd;

    std::set<StringData> sensitiveFieldNames() const final {
        return {kPwdField};
    }
};
MONGO_REGISTER_COMMAND(CmdUpdateUser).forShard();

template <>
void CmdUMCTyped<UpdateUserCommand>::Invocation::typedRun(OperationContext* opCtx) {
    const auto& cmd = request();
    auto dbname = cmd.getDbName();
    UserName userName(cmd.getCommandParameter(), dbname);

    uassert(ErrorCodes::BadValue,
            "mechanisms field must not be empty",
            (cmd.getMechanisms() == boost::none) || !cmd.getMechanisms()->empty());

    // Create the update filter (query) object.
    BSONObjBuilder queryBuilder;
    queryBuilder.append(AuthorizationManager::USER_NAME_FIELD_NAME, userName.getUser());
    queryBuilder.append(AuthorizationManager::USER_DB_FIELD_NAME, userName.getDB());

    // Create set/update mutators.
    BSONObjBuilder updateSetBuilder;
    BSONObjBuilder updateUnsetBuilder;

    if (auto pwd = cmd.getPwd()) {
        uassert(ErrorCodes::BadValue,
                "Cannot set the password for users defined on the '$external' database",
                userName.getDB() != "$external");

        BSONObjBuilder credentialsBuilder(updateSetBuilder.subobjStart("credentials"));
        buildCredentials(&credentialsBuilder, userName, cmd);
        credentialsBuilder.done();
    } else if (auto mechanisms = cmd.getMechanisms()) {
        trimCredentials(opCtx, userName, &queryBuilder, &updateUnsetBuilder, mechanisms.get());
    }

    if (auto customData = cmd.getCustomData()) {
        updateSetBuilder.append("customData", customData.get());
    }

    if (auto ar = cmd.getAuthenticationRestrictions()) {
        if (ar->empty()) {
            updateUnsetBuilder.append("authenticationRestrictions", "");
        } else {
            updateSetBuilder.append("authenticationRestrictions", vectorToBSON(ar.get()));
        }
    }

    boost::optional<std::vector<RoleName>> optResolvedRoles;
    if (auto roles = cmd.getRoles()) {
        optResolvedRoles = auth::resolveRoleNames(roles.get(), dbname);
        updateSetBuilder.append("roles", vectorToBSON(optResolvedRoles.get()));
    }

    BSONObj updateSet = updateSetBuilder.done();
    BSONObj updateUnset = updateUnsetBuilder.done();

    uassert(ErrorCodes::BadValue,
            "Must specify at least one field to update in updateUser",
            !updateSet.isEmpty() || !updateUnset.isEmpty());

    // Merge set/update builders into a single update document.
    BSONObjBuilder updateDocumentBuilder;
    if (!updateSet.isEmpty()) {
        updateDocumentBuilder.append("$set", updateSet);
    }
    if (!updateUnset.isEmpty()) {
        updateDocumentBuilder.append("$unset", updateUnset);
    }

    auto lk = uassertStatusOK(getWritableAuthzLock(opCtx));

    // Role existence has to be checked after acquiring the update lock
    if (auto roles = cmd.getRoles()) {
        auto resolvedRoles = auth::resolveRoleNames(roles.get(), dbname);
        uassertStatusOK(CmdUMCPassthrough::rolesExist(opCtx, resolvedRoles));
    }

    // Audit this event.
    auto optCustomData = cmd.getCustomData();
    BSONArray authRestrictions;
    if (auto ar = cmd.getAuthenticationRestrictions()) {
        authRestrictions = vectorToBSON(ar.get());
    }
    audit::logUpdateUser(opCtx->getClient(),
                         userName,
                         cmd.getPwd() != boost::none,
                         optCustomData ? &(optCustomData.get()) : nullptr,
                         optResolvedRoles ? &(optResolvedRoles.get()) : nullptr,
                         authRestrictions);

    auto status =
        updatePrivilegeDocument(opCtx, userName, queryBuilder.done(), updateDocumentBuilder.done());

    // Must invalidate even on bad status - what if the write succeeded but the GLE failed?
    AuthorizationManager::get(opCtx->getService())->invalidateUserByName(userName);
    uassertStatusOK(status);
}

MONGO_REGISTER_COMMAND(CmdUMCTyped<DropUserCommand>).forShard();
template <>
void CmdUMCTyped<DropUserCommand>::Invocation::typedRun(OperationContext* opCtx) {
    const auto& cmd = request();
    auto dbname = cmd.getDbName();
    UserName userName(cmd.getCommandParameter(), dbname);

    auto lk = uassertStatusOK(getWritableAuthzLock(opCtx));

    audit::logDropUser(Client::getCurrent(), userName);

    auto swNumMatched = removePrivilegeDocuments(
        opCtx,
        BSON(AuthorizationManager::USER_NAME_FIELD_NAME
             << userName.getUser() << AuthorizationManager::USER_DB_FIELD_NAME << userName.getDB()),
        userName.tenantId());

    // Must invalidate even on bad status - what if the write succeeded but the GLE failed?
    AuthorizationManager::get(opCtx->getService())->invalidateUserByName(userName);
    auto numMatched = uassertStatusOK(swNumMatched);

    uassert(ErrorCodes::UserNotFound,
            str::stream() << "User '" << userName << "' not found",
            numMatched > 0);
}

MONGO_REGISTER_COMMAND(CmdUMCTyped<DropAllUsersFromDatabaseCommand>).forShard();
template <>
DropAllUsersFromDatabaseReply CmdUMCTyped<DropAllUsersFromDatabaseCommand>::Invocation::typedRun(
    OperationContext* opCtx) {
    const auto& cmd = request();
    auto dbname = cmd.getDbName();

    auto* client = opCtx->getClient();
    auto lk = uassertStatusOK(getWritableAuthzLock(opCtx));

    audit::logDropAllUsersFromDatabase(client, dbname);

    auto swNumRemoved =
        removePrivilegeDocuments(opCtx,
                                 BSON(AuthorizationManager::USER_DB_FIELD_NAME
                                      << dbname.serializeWithoutTenantPrefix_UNSAFE()),
                                 dbname.tenantId());

    // Must invalidate even on bad status - what if the write succeeded but the GLE failed?
    AuthorizationManager::get(opCtx->getService())->invalidateUsersFromDB(dbname);

    DropAllUsersFromDatabaseReply reply;
    reply.setCount(uassertStatusOK(swNumRemoved));
    return reply;
}

MONGO_REGISTER_COMMAND(CmdUMCTyped<GrantRolesToUserCommand>).forShard();
template <>
void CmdUMCTyped<GrantRolesToUserCommand>::Invocation::typedRun(OperationContext* opCtx) {
    const auto& cmd = request();
    auto dbname = cmd.getDbName();
    UserName userName(cmd.getCommandParameter(), dbname);

    uassert(ErrorCodes::BadValue,
            "grantRolesToUser command requires a non-empty \"roles\" array",
            !cmd.getRoles().empty());

    auto* client = opCtx->getClient();
    auto lk = uassertStatusOK(getWritableAuthzLock(opCtx));

    stdx::unordered_set<RoleName> userRoles;
    uassertStatusOK(getCurrentUserRoles(opCtx, userName, &userRoles));

    auto resolvedRoleNames = auth::resolveRoleNames(cmd.getRoles(), dbname);
    uassertStatusOK(CmdUMCPassthrough::rolesExist(opCtx, resolvedRoleNames));
    for (const auto& role : resolvedRoleNames) {
        userRoles.insert(role);
    }

    audit::logGrantRolesToUser(client, userName, resolvedRoleNames);
    auto newRolesBSONArray = containerToBSONArray(userRoles);
    auto status = updatePrivilegeDocument(
        opCtx, userName, BSON("$set" << BSON("roles" << newRolesBSONArray)));

    // Must invalidate even on bad status - what if the write succeeded but the GLE failed?
    AuthorizationManager::get(opCtx->getService())->invalidateUserByName(userName);
    uassertStatusOK(status);
}

MONGO_REGISTER_COMMAND(CmdUMCTyped<RevokeRolesFromUserCommand>).forShard();
template <>
void CmdUMCTyped<RevokeRolesFromUserCommand>::Invocation::typedRun(OperationContext* opCtx) {
    const auto& cmd = request();
    auto dbname = cmd.getDbName();
    UserName userName(cmd.getCommandParameter(), dbname);

    uassert(ErrorCodes::BadValue,
            "revokeRolesFromUser command requires a non-empty \"roles\" array",
            !cmd.getRoles().empty());

    auto* client = opCtx->getClient();
    auto lk = uassertStatusOK(getWritableAuthzLock(opCtx));

    stdx::unordered_set<RoleName> userRoles;
    uassertStatusOK(getCurrentUserRoles(opCtx, userName, &userRoles));

    auto resolvedUserRoles = auth::resolveRoleNames(cmd.getRoles(), dbname);
    uassertStatusOK(CmdUMCPassthrough::rolesExist(opCtx, resolvedUserRoles));
    for (const auto& role : resolvedUserRoles) {
        userRoles.erase(role);
    }

    audit::logRevokeRolesFromUser(client, userName, resolvedUserRoles);
    BSONArray newRolesBSONArray = containerToBSONArray(userRoles);
    auto status = updatePrivilegeDocument(
        opCtx, userName, BSON("$set" << BSON("roles" << newRolesBSONArray)));

    // Must invalidate even on bad status - what if the write succeeded but the GLE failed?
    AuthorizationManager::get(opCtx->getService())->invalidateUserByName(userName);
    uassertStatusOK(status);
}

MONGO_REGISTER_COMMAND(CmdUMCTyped<UsersInfoCommand, UMCInfoParams>).forShard();
template <>
UsersInfoReply CmdUMCTyped<UsersInfoCommand, UMCInfoParams>::Invocation::typedRun(
    OperationContext* opCtx) {
    const auto& cmd = request();

    auto lk = uassertStatusOK(getReadOnlyAuthzLock(opCtx));
    return CmdUMCPassthrough::lookupUsers(opCtx, cmd);
}

MONGO_REGISTER_COMMAND(CmdUMCTyped<CreateRoleCommand>).forShard();
template <>
void CmdUMCTyped<CreateRoleCommand>::Invocation::typedRun(OperationContext* opCtx) {
    const auto& cmd = request();
    auto dbname = cmd.getDbName();

    uassert(
        ErrorCodes::BadValue, "Role name must be non-empty", !cmd.getCommandParameter().empty());
    RoleName roleName(cmd.getCommandParameter(), dbname);

    uassert(ErrorCodes::BadValue,
            "Cannot create roles in the local database",
            dbname != DatabaseName::kLocal);

    uassert(ErrorCodes::BadValue,
            "Cannot create roles in the $external database",
            !dbname.isExternalDB());

    uassert(ErrorCodes::BadValue,
            "Cannot create roles with the same name as a built-in role",
            !auth::isBuiltinRole(roleName));

    BSONObjBuilder roleObjBuilder;
    roleObjBuilder.append("_id", roleName.getUnambiguousName());
    roleObjBuilder.append(AuthorizationManager::ROLE_NAME_FIELD_NAME, roleName.getRole());
    roleObjBuilder.append(AuthorizationManager::ROLE_DB_FIELD_NAME, roleName.getDB());

    std::vector<std::string> unrecognizedActions;
    PrivilegeVector privileges = Privilege::privilegeVectorFromParsedPrivilegeVector(
        dbname.tenantId(), cmd.getPrivileges(), &unrecognizedActions);
    uassertNoUnrecognizedActions(unrecognizedActions);
    BSONArray privBSON;
    uassertStatusOK(privilegeVectorToBSONArray(privileges, &privBSON));
    roleObjBuilder.append("privileges", privBSON);

    auto resolvedRoleNames = auth::resolveRoleNames(cmd.getRoles(), dbname);
    roleObjBuilder.append("roles", containerToBSONArray(resolvedRoleNames));

    boost::optional<BSONArray> bsonAuthRestrictions;
    if (auto ar = cmd.getAuthenticationRestrictions(); ar && !ar->empty()) {
        bsonAuthRestrictions = vectorToBSON(ar.get());
        roleObjBuilder.append("authenticationRestrictions", bsonAuthRestrictions.get());
    }

    auto* client = opCtx->getClient();
    auto lk = uassertStatusOK(getWritableAuthzLock(opCtx));

    // Role existence has to be checked after acquiring the update lock
    uassertStatusOK(checkOkayToGrantRolesToRole(opCtx, roleName, resolvedRoleNames));
    uassertStatusOK(checkOkayToGrantPrivilegesToRole(roleName, privileges));

    audit::logCreateRole(client, roleName, resolvedRoleNames, privileges, bsonAuthRestrictions);

    uassertStatusOK(insertRoleDocument(opCtx, roleObjBuilder.done(), roleName.tenantId()));
}

MONGO_REGISTER_COMMAND(CmdUMCTyped<UpdateRoleCommand>).forShard();
template <>
void CmdUMCTyped<UpdateRoleCommand>::Invocation::typedRun(OperationContext* opCtx) {
    const auto& cmd = request();
    auto dbname = cmd.getDbName();
    RoleName roleName(cmd.getCommandParameter(), dbname);

    const bool hasRoles = cmd.getRoles() != boost::none;
    const bool hasPrivs = cmd.getPrivileges() != boost::none;
    const bool hasAuthRestrictions = cmd.getAuthenticationRestrictions() != boost::none;
    uassert(ErrorCodes::BadValue,
            "Must specify at least one field to update in updateRole",
            hasRoles || hasPrivs || hasAuthRestrictions);

    BSONObjBuilder updateSetBuilder;
    BSONObjBuilder updateUnsetBuilder;

    PrivilegeVector privileges;
    if (auto privs = cmd.getPrivileges()) {
        std::vector<std::string> unrecognizedActions;
        privileges = Privilege::privilegeVectorFromParsedPrivilegeVector(
            dbname.tenantId(), privs.get(), &unrecognizedActions);
        uassertNoUnrecognizedActions(unrecognizedActions);
        BSONArray privBSON;
        uassertStatusOK(privilegeVectorToBSONArray(privileges, &privBSON));
        updateSetBuilder.append("privileges", privBSON);
    }

    boost::optional<std::vector<RoleName>> optRoles;
    if (auto roles = cmd.getRoles()) {
        optRoles = auth::resolveRoleNames(roles.get(), dbname);
        updateSetBuilder.append("roles", containerToBSONArray(*optRoles));
    }

    BSONArray authRest;
    if (auto ar = cmd.getAuthenticationRestrictions()) {
        if (ar->empty()) {
            updateUnsetBuilder.append("authenticationRestrictions", "");
        } else {
            authRest = vectorToBSON(ar.get());
            updateSetBuilder.append("authenticationRestrictions", authRest);
        }
    }

    auto* client = opCtx->getClient();
    auto lk = uassertStatusOK(getWritableAuthzLock(opCtx));

    // Role existence has to be checked after acquiring the update lock
    uassertStatusOK(CmdUMCPassthrough::rolesExist(opCtx, {roleName}));

    if (optRoles) {
        uassertStatusOK(checkOkayToGrantRolesToRole(opCtx, roleName, *optRoles));
    }

    if (!privileges.empty()) {
        uassertStatusOK(checkOkayToGrantPrivilegesToRole(roleName, privileges));
    }

    audit::logUpdateRole(client,
                         roleName,
                         optRoles ? &*optRoles : nullptr,
                         hasPrivs ? &privileges : nullptr,
                         authRest);

    const auto updateSet = updateSetBuilder.obj();
    const auto updateUnset = updateUnsetBuilder.obj();
    BSONObjBuilder updateDocumentBuilder;
    if (!updateSet.isEmpty()) {
        updateDocumentBuilder.append("$set", updateSet);
    }
    if (!updateUnset.isEmpty()) {
        updateDocumentBuilder.append("$unset", updateUnset);
    }

    auto status = updateRoleDocument(opCtx, roleName, updateDocumentBuilder.obj());
    // Must invalidate even on bad status - what if the write succeeded but the GLE failed?
    AuthorizationManager::get(opCtx->getService())->invalidateUsersByTenant(dbname.tenantId());
    uassertStatusOK(status);
}

MONGO_REGISTER_COMMAND(CmdUMCTyped<GrantPrivilegesToRoleCommand>).forShard();
template <>
void CmdUMCTyped<GrantPrivilegesToRoleCommand>::Invocation::typedRun(OperationContext* opCtx) {
    const auto& cmd = request();
    auto dbname = cmd.getDbName();
    RoleName roleName(cmd.getCommandParameter(), dbname);

    uassert(ErrorCodes::BadValue,
            "grantPrivilegesToRole command requires a non-empty \"privileges\" array",
            !cmd.getPrivileges().empty());

    uassert(ErrorCodes::BadValue,
            str::stream() << roleName << " is a built-in role and cannot be modified",
            !auth::isBuiltinRole(roleName));

    auto lk = uassertStatusOK(getWritableAuthzLock(opCtx));

    std::vector<std::string> unrecognizedActions;
    PrivilegeVector newPrivileges = Privilege::privilegeVectorFromParsedPrivilegeVector(
        dbname.tenantId(), cmd.getPrivileges(), &unrecognizedActions);
    uassertNoUnrecognizedActions(unrecognizedActions);
    uassertStatusOK(checkOkayToGrantPrivilegesToRole(roleName, newPrivileges));

    // Add additional privileges to existing set.
    auto data = uassertStatusOK(CmdUMCPassthrough::resolveRoles(
        opCtx,
        {roleName},
        auth::AuthorizationBackendInterface::ResolveRoleOption::kPrivileges().setDirectOnly(
            true /* shouldEnable */)));
    auto privileges = std::move(data.privileges.get());
    for (const auto& priv : newPrivileges) {
        Privilege::addPrivilegeToPrivilegeVector(&privileges, priv);
    }

    // Build up update modifier object to $set privileges.
    BSONObj updateBSON = [&] {
        BSONObjBuilder updateBuilder;
        BSONObjBuilder updateSetBuilder(updateBuilder.subobjStart("$set"_sd));
        BSONArrayBuilder privilegeBuilder(updateSetBuilder.subarrayStart("privileges"_sd));
        Privilege::serializePrivilegeVector(privileges, &privilegeBuilder);
        privilegeBuilder.doneFast();
        updateSetBuilder.doneFast();
        return updateBuilder.obj();
    }();

    audit::logGrantPrivilegesToRole(opCtx->getClient(), roleName, newPrivileges);
    auto status = updateRoleDocument(opCtx, roleName, updateBSON);
    // Must invalidate even on bad status - what if the write succeeded but the GLE failed?
    AuthorizationManager::get(opCtx->getService())->invalidateUsersByTenant(dbname.tenantId());
    uassertStatusOK(status);
}

MONGO_REGISTER_COMMAND(CmdUMCTyped<RevokePrivilegesFromRoleCommand>).forShard();
template <>
void CmdUMCTyped<RevokePrivilegesFromRoleCommand>::Invocation::typedRun(OperationContext* opCtx) {
    const auto& cmd = request();
    auto dbname = cmd.getDbName();
    RoleName roleName(cmd.getCommandParameter(), dbname);

    uassert(ErrorCodes::BadValue,
            "revokePrivilegesFromRole command requires a non-empty \"privileges\" array",
            !cmd.getPrivileges().empty());

    uassert(ErrorCodes::BadValue,
            str::stream() << roleName << " is a built-in role and cannot be modified",
            !auth::isBuiltinRole(roleName));

    auto lk = uassertStatusOK(getWritableAuthzLock(opCtx));

    std::vector<std::string> unrecognizedActions;
    PrivilegeVector rmPrivs = Privilege::privilegeVectorFromParsedPrivilegeVector(
        dbname.tenantId(), cmd.getPrivileges(), &unrecognizedActions);
    uassertNoUnrecognizedActions(unrecognizedActions);

    auto data = uassertStatusOK(CmdUMCPassthrough::resolveRoles(
        opCtx,
        {roleName},
        auth::AuthorizationBackendInterface::ResolveRoleOption::kPrivileges().setDirectOnly(
            true /* shouldEnable */)));

    auto privileges = std::move(data.privileges.get());
    for (const auto& rmPriv : rmPrivs) {
        for (auto it = privileges.begin(); it != privileges.end(); ++it) {
            if (it->getResourcePattern() == rmPriv.getResourcePattern()) {
                it->removeActions(rmPriv.getActions());
                if (it->getActions().empty()) {
                    privileges.erase(it);
                    break;
                }
            }
        }
    }

    // Build up update modifier object to $set privileges.
    BSONObj updateBSON = [&] {
        BSONObjBuilder updateBuilder;
        BSONObjBuilder updateSetBuilder(updateBuilder.subobjStart("$set"_sd));
        BSONArrayBuilder privilegeBuilder(updateSetBuilder.subarrayStart("privileges"_sd));
        Privilege::serializePrivilegeVector(privileges, &privilegeBuilder);
        privilegeBuilder.doneFast();
        updateSetBuilder.doneFast();
        return updateBuilder.obj();
    }();

    audit::logRevokePrivilegesFromRole(opCtx->getClient(), roleName, rmPrivs);
    auto status = updateRoleDocument(opCtx, roleName, updateBSON);
    // Must invalidate even on bad status - what if the write succeeded but the GLE failed?
    AuthorizationManager::get(opCtx->getService())->invalidateUsersByTenant(dbname.tenantId());
    uassertStatusOK(status);
}

MONGO_REGISTER_COMMAND(CmdUMCTyped<GrantRolesToRoleCommand>).forShard();
template <>
void CmdUMCTyped<GrantRolesToRoleCommand>::Invocation::typedRun(OperationContext* opCtx) {
    const auto& cmd = request();
    auto dbname = cmd.getDbName();
    RoleName roleName(cmd.getCommandParameter(), dbname);

    uassert(ErrorCodes::BadValue,
            "grantRolesToRole command requires a non-empty \"roles\" array",
            !cmd.getRoles().empty());

    uassert(ErrorCodes::BadValue,
            str::stream() << roleName << " is a built-in role and cannot be modified",
            !auth::isBuiltinRole(roleName));

    auto rolesToAdd = auth::resolveRoleNames(cmd.getRoles(), dbname);

    auto* client = opCtx->getClient();
    auto lk = uassertStatusOK(getWritableAuthzLock(opCtx));

    // Check for cycles
    uassertStatusOK(checkOkayToGrantRolesToRole(opCtx, roleName, rolesToAdd));

    // Add new roles to existing roles
    auto data = uassertStatusOK(CmdUMCPassthrough::resolveRoles(
        opCtx,
        {roleName},
        auth::AuthorizationBackendInterface::ResolveRoleOption::kRoles().setDirectOnly(
            true /* shouldEnable */)));
    auto directRoles = std::move(data.roles.get());
    directRoles.insert(rolesToAdd.cbegin(), rolesToAdd.cend());

    audit::logGrantRolesToRole(client, roleName, rolesToAdd);

    auto status = updateRoleDocument(
        opCtx, roleName, BSON("$set" << BSON("roles" << containerToBSONArray(directRoles))));
    // Must invalidate even on bad status - what if the write succeeded but the GLE failed?
    AuthorizationManager::get(opCtx->getService())->invalidateUsersByTenant(dbname.tenantId());
    uassertStatusOK(status);
}

MONGO_REGISTER_COMMAND(CmdUMCTyped<RevokeRolesFromRoleCommand>).forShard();
template <>
void CmdUMCTyped<RevokeRolesFromRoleCommand>::Invocation::typedRun(OperationContext* opCtx) {
    const auto& cmd = request();
    auto dbname = cmd.getDbName();
    RoleName roleName(cmd.getCommandParameter(), dbname);

    uassert(ErrorCodes::BadValue,
            "revokeRolesFromRole command requires a non-empty \"roles\" array",
            !cmd.getRoles().empty());

    uassert(ErrorCodes::BadValue,
            str::stream() << roleName << " is a built-in role and cannot be modified",
            !auth::isBuiltinRole(roleName));

    auto rolesToRemove = auth::resolveRoleNames(cmd.getRoles(), dbname);
    auto lk = uassertStatusOK(getWritableAuthzLock(opCtx));

    // Remove roles from existing set.
    auto data = uassertStatusOK(CmdUMCPassthrough::resolveRoles(
        opCtx,
        {roleName},
        auth::AuthorizationBackendInterface::ResolveRoleOption::kRoles().setDirectOnly(
            true /* shouldEnable */)));
    auto roles = std::move(data.roles.get());
    for (const auto& roleToRemove : rolesToRemove) {
        roles.erase(roleToRemove);
    }

    audit::logRevokeRolesFromRole(opCtx->getClient(), roleName, rolesToRemove);

    auto status = updateRoleDocument(
        opCtx, roleName, BSON("$set" << BSON("roles" << containerToBSONArray(roles))));
    // Must invalidate even on bad status - what if the write succeeded but the GLE failed?
    AuthorizationManager::get(opCtx->getService())->invalidateUsersByTenant(dbname.tenantId());
    uassertStatusOK(status);
}

bool validNamespace(const NamespaceString& nss) {
    return (nss.isAdminDB());
}

write_ops::UpdateCommandRequest buildUpdateRequest(const NamespaceString& nss,
                                                   BSONObj query,
                                                   BSONObj update) {
    dassert(validNamespace(nss));
    write_ops::UpdateOpEntry entry;
    entry.setQ(query);
    entry.setU(write_ops::UpdateModification::parseFromClassicUpdate(update));
    entry.setMulti(true);
    write_ops::UpdateCommandRequest op(nss);
    op.setUpdates({entry});
    return op;
}

write_ops::DeleteCommandRequest buildDeleteRequest(const NamespaceString& nss, BSONObj query) {
    dassert(validNamespace(nss));
    write_ops::DeleteOpEntry entry;
    entry.setQ(query);
    entry.setMulti(true);
    write_ops::DeleteCommandRequest op(nss);
    op.setDeletes({entry});
    return op;
}

void handleUmcTransactionFailpoint() {
    auto fp = umcTransaction.scoped();
    if (fp.isActive()) {
        IDLParserContext ctx("umcTransaction");
        auto delay = UMCTransactionFailPoint::parse(fp.getData(), ctx).getCommitDelayMS();
        LOGV2(4993100,
              "Sleeping prior to committing UMC transaction",
              "duration"_attr = Milliseconds(delay));
        sleepmillis(delay);
    }
}

MONGO_REGISTER_COMMAND(CmdUMCTyped<DropRoleCommand>).forShard();
template <>
void CmdUMCTyped<DropRoleCommand>::Invocation::typedRun(OperationContext* opCtx) {
    const auto& cmd = request();
    auto dbname = cmd.getDbName();
    RoleName roleName(cmd.getCommandParameter(), dbname);

    uassert(ErrorCodes::BadValue,
            str::stream() << roleName << " is a built-in role and cannot be modified",
            !auth::isBuiltinRole(roleName));

    auto* client = opCtx->getClient();
    auto lk = uassertStatusOK(getWritableAuthzLock(opCtx));

    uassertStatusOK(CmdUMCPassthrough::rolesExist(opCtx, {roleName}));

    // From here on, we always want to invalidate the user cache before returning.
    ScopeGuard invalidateGuard([&] {
        try {
            AuthorizationManager::get(opCtx->getService())
                ->invalidateUsersByTenant(dbname.tenantId());
        } catch (const AssertionException& ex) {
            LOGV2_WARNING(4907701, "Failed invalidating user cache", "exception"_attr = ex);
        }
    });

    // Perform all update/delete operations on the users and roles collections in a transaction.
    auto dropRoleTxnFn = [&roleName, &dbname](UMCTransactionClient& umcTxnClient) {
        try {
            // Remove this role from any user documents that include it.
            const auto& updateUserDocsOp =
                buildUpdateRequest(usersNSS(dbname.tenantId()),
                                   BSON("roles" << BSON("$elemMatch" << roleName.toBSON())),
                                   BSON("$pull" << BSON("roles" << roleName.toBSON())));
            auto updateUserDocsStatus = umcTxnClient.runCRUDOp(updateUserDocsOp, {}).toStatus();
            uassertStatusOK(useDefaultCode(updateUserDocsStatus, ErrorCodes::UserModificationFailed)
                                .withContext(str::stream() << "Failed to remove role " << roleName
                                                           << " from all users"));

            // Remove this role from all other roles.
            const auto& updateRoleDocsOp =
                buildUpdateRequest(rolesNSS(dbname.tenantId()),
                                   BSON("roles" << BSON("$elemMatch" << roleName.toBSON())),
                                   BSON("$pull" << BSON("roles" << roleName.toBSON())));
            auto updateRoleDocsStatus = umcTxnClient.runCRUDOp(updateRoleDocsOp, {}).toStatus();
            uassertStatusOK(useDefaultCode(updateRoleDocsStatus, ErrorCodes::RoleModificationFailed)
                                .withContext(str::stream() << "Failed to remove role " << roleName
                                                           << " from all roles"));

            // Finally, remove the actual role document
            const auto& deleteRoleDocOp =
                buildDeleteRequest(rolesNSS(dbname.tenantId()), roleName.toBSON());
            auto deleteRoleDocStatus = umcTxnClient.runCRUDOp(deleteRoleDocOp, {}).toStatus();
            uassertStatusOK(
                useDefaultCode(deleteRoleDocStatus, ErrorCodes::RoleModificationFailed)
                    .withContext(str::stream() << "Failed to remove role " << roleName));
        } catch (const DBException& ex) {
            return ex.toStatus();
        }

        handleUmcTransactionFailpoint();
        return Status::OK();
    };

    // Audit the dropRole attempt before the transaction runs.
    audit::logDropRole(client, roleName);
    if (repl::ReplicationCoordinator::get(opCtx)->getSettings().isReplSet()) {
        UMCTransactionReplSet txn{"dropRole"};
        txn.run(opCtx, std::move(dropRoleTxnFn));
    } else {
        UMCTransactionStandalone txn{"dropRole"};
        txn.run(opCtx, std::move(dropRoleTxnFn));
    }
}

MONGO_REGISTER_COMMAND(CmdUMCTyped<DropAllRolesFromDatabaseCommand>).forShard();
template <>
DropAllRolesFromDatabaseReply CmdUMCTyped<DropAllRolesFromDatabaseCommand>::Invocation::typedRun(
    OperationContext* opCtx) {
    const auto& cmd = request();
    auto dbname = cmd.getDbName();

    auto* client = opCtx->getClient();
    auto* service = client->getService();
    auto* authzManager = AuthorizationManager::get(service);
    auto lk = uassertStatusOK(getWritableAuthzLock(opCtx));

    // From here on, we always want to invalidate the user cache before returning.
    ScopeGuard invalidateGuard([opCtx, authzManager, &dbname] {
        try {
            authzManager->invalidateUsersByTenant(dbname.tenantId());
        } catch (const AssertionException& ex) {
            LOGV2_WARNING(4907700, "Failed invalidating user cache", "exception"_attr = ex);
        }
    });

    DropAllRolesFromDatabaseReply reply;
    auto dropAllRolesFromDBTxnFn = [&dbname, &reply](UMCTransactionClient& umcTxnClient) {
        auto roleMatch = BSON(AuthorizationManager::ROLE_DB_FIELD_NAME
                              << dbname.serializeWithoutTenantPrefix_UNSAFE());
        auto rolesMatch = BSON("roles" << roleMatch);

        try {
            // Remove this role from any user documents that include it.
            const auto& updateUserDocsOp = buildUpdateRequest(
                usersNSS(dbname.tenantId()), rolesMatch, BSON("$pull" << rolesMatch));
            auto updateUserDocsStatus = umcTxnClient.runCRUDOp(updateUserDocsOp, {}).toStatus();
            uassertStatusOK(useDefaultCode(updateUserDocsStatus, ErrorCodes::UserModificationFailed)
                                .withContext(str::stream() << "Failed to remove roles from  \""
                                                           << dbname.toStringForErrorMsg()
                                                           << "\" db from all users"));

            // Remove this role from all other roles.
            const auto& updateRoleDocsOp = buildUpdateRequest(
                rolesNSS(dbname.tenantId()),
                BSON("roles" << BSON("roles.db" << dbname.serializeWithoutTenantPrefix_UNSAFE())),
                BSON("$pull" << rolesMatch));
            auto updateRoleDocsStatus = umcTxnClient.runCRUDOp(updateRoleDocsOp, {}).toStatus();
            uassertStatusOK(useDefaultCode(updateRoleDocsStatus, ErrorCodes::RoleModificationFailed)
                                .withContext(str::stream() << "Failed to remove roles from \""
                                                           << dbname.toStringForErrorMsg()
                                                           << "\" db from all roles"));

            // Finally, remove the actual role documents
            const auto& deleteRoleDocOp =
                buildDeleteRequest(rolesNSS(dbname.tenantId()), roleMatch);
            const auto& deleteRoleDocReply = umcTxnClient.runCRUDOp(deleteRoleDocOp, {});
            auto deleteRoleDocStatus = deleteRoleDocReply.toStatus();
            uassertStatusOK(
                useDefaultCode(deleteRoleDocStatus, ErrorCodes::RoleModificationFailed)
                    .withContext(str::stream()
                                 << "Removed roles from \"" << dbname.toStringForErrorMsg()
                                 << "\" db from all users and roles but failed to actually "
                                    "delete those roles themselves"));
            reply.setCount(deleteRoleDocReply.getN());
        } catch (const DBException& ex) {
            return ex.toStatus();
        }

        handleUmcTransactionFailpoint();
        return Status::OK();
    };

    // Audit the dropAllRolesFromDatabase attempt before the transaction runs.
    audit::logDropAllRolesFromDatabase(opCtx->getClient(), dbname);
    if (repl::ReplicationCoordinator::get(opCtx)->getSettings().isReplSet()) {
        UMCTransactionReplSet txn{"dropAllRolesFromDatabase"};
        txn.run(opCtx, std::move(dropAllRolesFromDBTxnFn));
    } else {
        UMCTransactionStandalone txn{"dropRole"};
        txn.run(opCtx, std::move(dropAllRolesFromDBTxnFn));
    }

    return reply;
}

/**
 * Provides information about one or more roles, the indirect roles they are members of, and
 * optionally the privileges they provide.
 *
 * This command accepts the following arguments:
 * rolesInfo:
 *   (String) Returns information about a single role on the current database.
 *   {role: (String), db: (String)} Returns information about a specified role, on a specific db
 *   (BooleanTrue) Returns information about all roles in this database
 *   [ //Zero or more of
 *     {role: (String), db: (String) ] Returns information about all specified roles
 *
 * showBuiltinRoles:
 *   (Boolean) If true, and rolesInfo == (BooleanTrue), include built-in roles from the database
 *
 * showPrivileges:
 *   (BooleanFalse) Do not show information about privileges
 *   (BooleanTrue) Attach all privileges inherited from roles to role descriptions
 *   "asUserFragment" Render results as a partial user document as-if a user existed which possessed
 *                    these roles. This format may change over time with changes to the auth schema.
 */
MONGO_REGISTER_COMMAND(CmdUMCTyped<RolesInfoCommand, UMCInfoParams>).forShard();
template <>
RolesInfoReply CmdUMCTyped<RolesInfoCommand, UMCInfoParams>::Invocation::typedRun(
    OperationContext* opCtx) {
    const auto& cmd = request();
    auto lk = uassertStatusOK(getReadOnlyAuthzLock(opCtx));
    return CmdUMCPassthrough::lookupRoles(opCtx, cmd);
}

MONGO_REGISTER_COMMAND(CmdUMCTyped<InvalidateUserCacheCommand, UMCInvalidateUserCacheParams>)
    .forShard();
template <>
void CmdUMCTyped<InvalidateUserCacheCommand, UMCInvalidateUserCacheParams>::Invocation::typedRun(
    OperationContext* opCtx) {
    auto lk = getReadOnlyAuthzLock(opCtx);
    AuthorizationManager::get(opCtx->getService())
        ->invalidateUsersByTenant(request().getDbName().tenantId());
}

MONGO_REGISTER_COMMAND(CmdUMCTyped<GetUserCacheGenerationCommand, UMCGetUserCacheGenParams>)
    .forShard();
template <>
GetUserCacheGenerationReply
CmdUMCTyped<GetUserCacheGenerationCommand, UMCGetUserCacheGenParams>::Invocation::typedRun(
    OperationContext* opCtx) {
    uassert(ErrorCodes::IllegalOperation,
            "_getUserCacheGeneration can only be run on config servers",
            serverGlobalParams.clusterRole.has(ClusterRole::ConfigServer));

    definition()->skipApiVersionCheck();
    GetUserCacheGenerationReply reply;
    auto* authzManager = AuthorizationManager::get(opCtx->getService());
    reply.setCacheGeneration(authzManager->getCacheGeneration());
    return reply;
}

/**
 * This command is used only by mongorestore to handle restoring users/roles.  We do this so
 * that mongorestore doesn't do direct inserts into the admin.system.users and
 * admin.system.roles, which would bypass the authzUpdateLock and allow multiple concurrent
 * modifications to users/roles.  What mongorestore now does instead is it inserts all user/role
 * definitions it wants to restore into temporary collections, then this command moves those
 * user/role definitions into their proper place in admin.system.users and admin.system.roles.
 * It either adds the users/roles to the existing ones or replaces the existing ones, depending
 * on whether the "drop" argument is true or false.
 */
class CmdMergeAuthzCollections : public TypedCommand<CmdMergeAuthzCollections> {
public:
    using Request = MergeAuthzCollectionsCommand;

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;
        void typedRun(OperationContext* opCtx);

    private:
        bool supportsWriteConcern() const final {
            return true;
        }

        void doCheckAuthorization(OperationContext* opCtx) const final {
            auth::checkAuthForTypedCommand(opCtx, request());
        }

        NamespaceString ns() const final {
            return NamespaceString(request().getDbName());
        }
    };

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const final {
        return AllowedOnSecondary::kNever;
    }

    bool adminOnly() const final {
        return true;
    }

    bool allowedWithSecurityToken() const final {
        // TODO (SERVER-TBD) Support mergeAuthzCollections in multitenancy
        return false;
    }
};
MONGO_REGISTER_COMMAND(CmdMergeAuthzCollections).forShard();

UserName _extractUserNameFromBSON(const BSONObj& userObj) {
    std::string name;
    std::string db;
    uassertStatusOK(
        bsonExtractStringField(userObj, AuthorizationManager::USER_NAME_FIELD_NAME, &name));
    uassertStatusOK(bsonExtractStringField(userObj, AuthorizationManager::USER_DB_FIELD_NAME, &db));
    return UserName(name, db);
}

RoleName _extractRoleNameFromBSON(const BSONObj& roleObj) {
    std::string name;
    std::string db;
    uassertStatusOK(
        bsonExtractStringField(roleObj, AuthorizationManager::ROLE_NAME_FIELD_NAME, &name));
    uassertStatusOK(bsonExtractStringField(roleObj, AuthorizationManager::ROLE_DB_FIELD_NAME, &db));
    return RoleName(name, db);
}

/**
 * Audits the fact that we are creating or updating the user described by userObj.
 */
void _auditCreateOrUpdateUser(const BSONObj& userObj, bool create) {
    UserName userName = _extractUserNameFromBSON(userObj);
    std::vector<RoleName> roles;
    uassertStatusOK(auth::parseRoleNamesFromBSONArray(
        BSONArray(userObj["roles"].Obj()), userName.getDB(), &roles));
    BSONObj customData;
    if (userObj.hasField("customData")) {
        customData = userObj["customData"].Obj();
    }

    boost::optional<BSONArray> authenticationRestrictions;
    if (userObj.hasField("authenticationRestrictions")) {
        auto r = getRawAuthenticationRestrictions(
            BSONArray(userObj["authenticationRestrictions"].Obj()));
        uassertStatusOK(r);
        authenticationRestrictions = r.getValue();
    }

    const bool hasPwd = userObj["credentials"].Obj().hasField("SCRAM-SHA-1") ||
        userObj["credentials"].Obj().hasField("SCRAM-SHA-256");
    if (create) {
        audit::logCreateUser(Client::getCurrent(),
                             userName,
                             hasPwd,
                             userObj.hasField("customData") ? &customData : nullptr,
                             roles,
                             authenticationRestrictions);
    } else {
        audit::logUpdateUser(Client::getCurrent(),
                             userName,
                             hasPwd,
                             userObj.hasField("customData") ? &customData : nullptr,
                             &roles,
                             authenticationRestrictions);
    }
}

/**
 * Designed to be used as a callback to be called on every user object in the result
 * set of a query over the tempUsersCollection provided to the command.  For each user
 * in the temp collection that is defined on the given db, adds that user to the actual
 * admin.system.users collection.
 * Also removes any users it encounters from the usersToDrop set.
 */
void _addUser(OperationContext* opCtx,
              AuthorizationManager* authzManager,
              StringData db,
              bool update,
              stdx::unordered_set<UserName>* usersToDrop,
              const BSONObj& userObj) {
    UserName userName = _extractUserNameFromBSON(userObj);
    if (!db.empty() && userName.getDB() != db) {
        return;
    }

    if (update && usersToDrop->count(userName)) {
        _auditCreateOrUpdateUser(userObj, false);
        Status status = updatePrivilegeDocument(opCtx, userName, userObj);
        if (!status.isOK()) {
            // Match the behavior of mongorestore to continue on failure
            LOGV2_WARNING(20510,
                          "Could not update user during _mergeAuthzCollections command",
                          "user"_attr = userName,
                          "error"_attr = redact(status));
        }
    } else {
        _auditCreateOrUpdateUser(userObj, true);
        Status status = insertPrivilegeDocument(opCtx, userObj);
        if (!status.isOK()) {
            // Match the behavior of mongorestore to continue on failure
            LOGV2_WARNING(20511,
                          "Could not insert user during _mergeAuthzCollections command",
                          "user"_attr = userName,
                          "error"_attr = redact(status));
        }
    }
    usersToDrop->erase(userName);
}


/**
 * Finds all documents matching "query" in "collectionName".  For each document returned,
 * calls the function resultProcessor on it.
 * Should only be called on collections with authorization documents in them
 * (ie admin.system.users and admin.system.roles).
 */
Status queryAuthzDocument(OperationContext* opCtx,
                          const NamespaceString& nss,
                          const BSONObj& query,
                          const BSONObj& projection,
                          const std::function<void(const BSONObj&)>& resultProcessor) try {
    DBDirectClient client(opCtx);

    // The sc is used to control serialization behavior executed on the request in client.find, and
    // tenantIds should not be prefixed on the $db field.  Indicating that the request received a
    // tenantId from something other than a prefix, in this case the nss, will prevent prefixing.
    const auto vts = auth::ValidatedTenancyScope::get(opCtx);
    auto sc = SerializationContext::stateCommandRequest(
        nss.tenantId() != boost::none, vts != boost::none && vts->isFromAtlasProxy());

    FindCommandRequest findRequest{nss, sc};
    findRequest.setFilter(query);
    findRequest.setProjection(projection);
    client.find(std::move(findRequest), resultProcessor);
    return Status::OK();
} catch (const DBException& e) {
    return e.toStatus();
}


/**
 * Moves all user objects from usersCollName into admin.system.users.  If drop is true,
 * removes any users that were in admin.system.users but not in usersCollName.
 */
void _processUsers(OperationContext* opCtx,
                   AuthorizationManager* authzManager,
                   StringData usersCollName,
                   StringData db,
                   const bool drop,
                   const boost::optional<TenantId>& tenantId) {
    // When the "drop" argument has been provided, we use this set to store the users
    // that are currently in the system, and remove from it as we encounter
    // same-named users in the collection we are restoring from.  Once we've fully
    // moved over the temp users collection into its final location, we drop
    // any users that previously existed there but weren't in the temp collection.
    // This is so that we can completely replace the system.users
    // collection with the users from the temp collection, without removing all
    // users at the beginning and thus potentially locking ourselves out by having
    // no users in the whole system for a time.
    stdx::unordered_set<UserName> usersToDrop;

    if (drop) {
        // Create map of the users currently in the DB
        BSONObj query =
            db.empty() ? BSONObj() : BSON(AuthorizationManager::USER_DB_FIELD_NAME << db);
        BSONObj fields = BSON(AuthorizationManager::USER_NAME_FIELD_NAME
                              << 1 << AuthorizationManager::USER_DB_FIELD_NAME << 1);

        uassertStatusOK(queryAuthzDocument(opCtx,
                                           NamespaceString::kAdminUsersNamespace,
                                           query,
                                           fields,
                                           [&](const BSONObj& userObj) {
                                               usersToDrop.insert(
                                                   _extractUserNameFromBSON(userObj));
                                           }));
    }

    uassertStatusOK(queryAuthzDocument(
        opCtx,
        NamespaceStringUtil::deserialize(
            tenantId, usersCollName, SerializationContext::stateDefault()),
        db.empty() ? BSONObj() : BSON(AuthorizationManager::USER_DB_FIELD_NAME << db),
        BSONObj(),
        [&](const BSONObj& userObj) {
            return _addUser(opCtx, authzManager, db, drop, &usersToDrop, userObj);
        }));

    if (drop) {
        for (const auto& userName : usersToDrop) {
            audit::logDropUser(opCtx->getClient(), userName);
            auto numRemoved = uassertStatusOK(
                removePrivilegeDocuments(opCtx, userName.toBSON(), userName.tenantId()));
            dassert(numRemoved == 1);
        }
    }
}

/**
 * Audits the fact that we are creating or updating the role described by roleObj.
 */
void _auditCreateOrUpdateRole(const BSONObj& roleObj, bool create) {
    RoleName roleName = _extractRoleNameFromBSON(roleObj);
    std::vector<RoleName> roles;
    std::vector<Privilege> privileges;
    uassertStatusOK(auth::parseRoleNamesFromBSONArray(
        BSONArray(roleObj["roles"].Obj()), roleName.getDB(), &roles));
    uassertStatusOK(
        auth::parseAndValidatePrivilegeArray(BSONArray(roleObj["privileges"].Obj()), &privileges));

    boost::optional<BSONArray> authenticationRestrictions;
    if (roleObj.hasField("authenticationRestrictions")) {
        auto r = getRawAuthenticationRestrictions(
            BSONArray(roleObj["authenticationRestrictions"].Obj()));
        uassertStatusOK(r);
        authenticationRestrictions = r.getValue();
    }

    if (create) {
        audit::logCreateRole(
            Client::getCurrent(), roleName, roles, privileges, authenticationRestrictions);
    } else {
        audit::logUpdateRole(
            Client::getCurrent(), roleName, &roles, &privileges, authenticationRestrictions);
    }
}

/**
 * Designed to be used as a callback to be called on every role object in the result
 * set of a query over the tempRolesCollection provided to the command.  For each role
 * in the temp collection that is defined on the given db, adds that role to the actual
 * admin.system.roles collection.
 * Also removes any roles it encounters from the rolesToDrop set.
 */
void _addRole(OperationContext* opCtx,
              AuthorizationManager* authzManager,
              StringData db,
              bool update,
              stdx::unordered_set<RoleName>* rolesToDrop,
              const BSONObj roleObj) {
    RoleName roleName = _extractRoleNameFromBSON(roleObj);
    if (!db.empty() && roleName.getDB() != db) {
        return;
    }

    if (update && rolesToDrop->count(roleName)) {
        _auditCreateOrUpdateRole(roleObj, false);
        Status status = updateRoleDocument(opCtx, roleName, roleObj);
        if (!status.isOK()) {
            // Match the behavior of mongorestore to continue on failure
            LOGV2_WARNING(20512,
                          "Could not update role during _mergeAuthzCollections command",
                          "role"_attr = roleName,
                          "error"_attr = redact(status));
        }
    } else {
        _auditCreateOrUpdateRole(roleObj, true);
        Status status = insertRoleDocument(opCtx, roleObj, roleName.tenantId());
        if (!status.isOK()) {
            // Match the behavior of mongorestore to continue on failure
            LOGV2_WARNING(20513,
                          "Could not insert role during _mergeAuthzCollections command",
                          "role"_attr = roleName,
                          "error"_attr = redact(status));
        }
    }
    rolesToDrop->erase(roleName);
}

/**
 * Moves all user objects from usersCollName into admin.system.users.  If drop is true,
 * removes any users that were in admin.system.users but not in usersCollName.
 */
void _processRoles(OperationContext* opCtx,
                   AuthorizationManager* authzManager,
                   StringData rolesCollName,
                   StringData db,
                   const bool drop,
                   const boost::optional<TenantId>& tenantId) {
    // When the "drop" argument has been provided, we use this set to store the roles
    // that are currently in the system, and remove from it as we encounter
    // same-named roles in the collection we are restoring from.  Once we've fully
    // moved over the temp roles collection into its final location, we drop
    // any roles that previously existed there but weren't in the temp collection.
    // This is so that we can completely replace the system.roles
    // collection with the roles from the temp collection, without removing all
    // roles at the beginning and thus potentially locking ourselves out.
    stdx::unordered_set<RoleName> rolesToDrop;

    if (drop) {
        // Create map of the roles currently in the DB
        BSONObj query =
            db.empty() ? BSONObj() : BSON(AuthorizationManager::ROLE_DB_FIELD_NAME << db);
        BSONObj fields = BSON(AuthorizationManager::ROLE_NAME_FIELD_NAME
                              << 1 << AuthorizationManager::ROLE_DB_FIELD_NAME << 1);

        uassertStatusOK(queryAuthzDocument(opCtx,
                                           NamespaceString::kAdminRolesNamespace,
                                           query,
                                           fields,
                                           [&](const BSONObj& roleObj) {
                                               return rolesToDrop.insert(
                                                   _extractRoleNameFromBSON(roleObj));
                                           }));
    }

    uassertStatusOK(queryAuthzDocument(
        opCtx,
        NamespaceStringUtil::deserialize(
            tenantId, rolesCollName, SerializationContext::stateDefault()),
        db.empty() ? BSONObj() : BSON(AuthorizationManager::ROLE_DB_FIELD_NAME << db),
        BSONObj(),
        [&](const BSONObj& roleObj) {
            return _addRole(opCtx, authzManager, db, drop, &rolesToDrop, roleObj);
        }));

    if (drop) {
        for (const auto& roleName : rolesToDrop) {
            audit::logDropRole(Client::getCurrent(), roleName);
            auto numRemoved =
                uassertStatusOK(removeRoleDocuments(opCtx, roleName.toBSON(), roleName.tenantId()));
            dassert(numRemoved == 1);
        }
    }
}

void CmdMergeAuthzCollections::Invocation::typedRun(OperationContext* opCtx) {
    const auto& cmd = request();
    const auto tempUsersColl = cmd.getTempUsersCollection();
    const auto tempRolesColl = cmd.getTempRolesCollection();

    uassert(ErrorCodes::BadValue,
            "Must provide at least one of \"tempUsersCollection\" and \"tempRolescollection\"",
            !tempUsersColl.empty() || !tempRolesColl.empty());

    auto* service = opCtx->getClient()->getService();
    auto* authzManager = AuthorizationManager::get(service);
    auto lk = uassertStatusOK(getWritableAuthzLock(opCtx));

    // From here on, we always want to invalidate the user cache before returning.
    ScopeGuard invalidateGuard([&] { authzManager->invalidateUserCache(); });
    const auto db = cmd.getDb();
    const bool drop = cmd.getDrop();
    const auto tenantId = cmd.getDbName().tenantId();

    if (!tempUsersColl.empty()) {
        _processUsers(opCtx, authzManager, tempUsersColl, db, drop, tenantId);
    }

    if (!tempRolesColl.empty()) {
        _processRoles(opCtx, authzManager, tempRolesColl, db, drop, tenantId);
    }
}

}  // namespace

}  // namespace mongo
