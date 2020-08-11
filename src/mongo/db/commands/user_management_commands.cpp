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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kAccessControl

#include "mongo/platform/basic.h"

#include <functional>
#include <string>
#include <vector>

#include "mongo/base/status.h"
#include "mongo/bson/mutable/algorithm.h"
#include "mongo/bson/mutable/document.h"
#include "mongo/bson/mutable/element.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/config.h"
#include "mongo/crypto/mechanism_scram.h"
#include "mongo/db/audit.h"
#include "mongo/db/auth/action_set.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/address_restriction.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/privilege_parser.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/auth/sasl_options.h"
#include "mongo/db/auth/user.h"
#include "mongo/db/auth/user_document_parser.h"
#include "mongo/db/auth/user_management_commands_parser.h"
#include "mongo/db/client.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/run_aggregate.h"
#include "mongo/db/commands/user_management_commands_common.h"
#include "mongo/db/commands/user_management_commands_gen.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/ops/write_ops.h"
#include "mongo/db/query/cursor_response.h"
#include "mongo/db/service_context.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/mutex.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/s/write_ops/batched_command_response.h"
#include "mongo/stdx/unordered_set.h"
#include "mongo/util/icu.h"
#include "mongo/util/net/ssl_manager.h"
#include "mongo/util/password_digest.h"
#include "mongo/util/sequence_util.h"
#include "mongo/util/str.h"
#include "mongo/util/time_support.h"
#include "mongo/util/uuid.h"

namespace mongo {
namespace {

Status useDefaultCode(const Status& status, ErrorCodes::Error defaultCode) {
    if (status.code() != ErrorCodes::UnknownError)
        return status;
    return Status(defaultCode, status.reason());
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
 * Used to get all current roles of the user identified by 'userName'.
 */
Status getCurrentUserRoles(OperationContext* opCtx,
                           AuthorizationManager* authzManager,
                           const UserName& userName,
                           stdx::unordered_set<RoleName>* roles) {
    auto swUser = authzManager->acquireUser(opCtx, userName);
    if (!swUser.isOK()) {
        return swUser.getStatus();
    }
    auto user = std::move(swUser.getValue());

    RoleNameIterator rolesIt = user->getRoles();
    while (rolesIt.more()) {
        roles->insert(rolesIt.next());
    }
    return Status::OK();
}

/**
 * Checks that every role in "rolesToAdd" exists, that adding each of those roles to "role"
 * will not result in a cycle to the role graph, and that every role being added comes from the
 * same database as the role it is being added to (or that the role being added to is from the
 * "admin" database.
 */
Status checkOkayToGrantRolesToRole(OperationContext* opCtx,
                                   const RoleName& role,
                                   const std::vector<RoleName>& rolesToAdd,
                                   AuthorizationManager* authzManager) {
    for (const auto& roleToAdd : rolesToAdd) {
        if (roleToAdd == role) {
            return {ErrorCodes::InvalidRoleModification,
                    str::stream() << "Cannot grant role " << role.getFullName() << " to itself."};
        }

        if (role.getDB() != "admin" && roleToAdd.getDB() != role.getDB()) {
            return {ErrorCodes::InvalidRoleModification,
                    str::stream() << "Roles on the \'" << role.getDB()
                                  << "\' database cannot be granted roles from other databases"};
        }
    }

    auto status = authzManager->rolesExist(opCtx, rolesToAdd);
    if (!status.isOK()) {
        return {status.code(),
                str::stream() << "Cannot grant roles to '" << role.toString()
                              << "': " << status.reason()};
    }

    auto swData = authzManager->resolveRoles(
        opCtx, rolesToAdd, AuthorizationManager::ResolveRoleOption::kRoles);
    if (!swData.isOK()) {
        return {swData.getStatus().code(),
                str::stream() << "Cannot grant roles to '" << role.toString()
                              << "': " << swData.getStatus().reason()};
    }

    if (sequenceContains(swData.getValue().roles.get(), role)) {
        return {ErrorCodes::InvalidRoleModification,
                str::stream() << "Granting roles to " << role.getFullName()
                              << " would introduce a cycle in the role graph"};
    }

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
            (resource.databaseToMatch() == role.getDB())) {
            continue;
        }

        return Status(ErrorCodes::InvalidRoleModification,
                      str::stream() << "Roles on the \'" << role.getDB()
                                    << "\' database cannot be granted privileges that target other "
                                       "databases or the cluster");
    }

    return Status::OK();
}

/**
 * Finds all documents matching "query" in "collectionName".  For each document returned,
 * calls the function resultProcessor on it.
 * Should only be called on collections with authorization documents in them
 * (ie admin.system.users and admin.system.roles).
 */
Status queryAuthzDocument(OperationContext* opCtx,
                          const NamespaceString& collectionName,
                          const BSONObj& query,
                          const BSONObj& projection,
                          const std::function<void(const BSONObj&)>& resultProcessor) {
    try {
        DBDirectClient client(opCtx);
        client.query(resultProcessor, collectionName, query, &projection);
        return Status::OK();
    } catch (const DBException& e) {
        return e.toStatus();
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
                           const NamespaceString& collectionName,
                           const BSONObj& document) {
    try {
        DBDirectClient client(opCtx);

        BSONObj res;
        client.runCommand(collectionName.db().toString(),
                          [&] {
                              write_ops::Insert insertOp(collectionName);
                              insertOp.setDocuments({document});
                              return insertOp.toBSON({});
                          }(),
                          res);

        BatchedCommandResponse response;
        std::string errmsg;
        if (!response.parseBSON(res, &errmsg)) {
            return Status(ErrorCodes::FailedToParse, errmsg);
        }
        return response.toStatus();
    } catch (const DBException& e) {
        return e.toStatus();
    }
}

/**
 * Updates documents matching "query" according to "updatePattern" in "collectionName".
 *
 * Should only be called on collections with authorization documents in them
 * (ie admin.system.users and admin.system.roles).
 */
Status updateAuthzDocuments(OperationContext* opCtx,
                            const NamespaceString& collectionName,
                            const BSONObj& query,
                            const BSONObj& updatePattern,
                            bool upsert,
                            bool multi,
                            std::int64_t* numMatched) {
    try {
        DBDirectClient client(opCtx);

        BSONObj res;
        client.runCommand(collectionName.db().toString(),
                          [&] {
                              write_ops::Update updateOp(collectionName);
                              updateOp.setUpdates({[&] {
                                  write_ops::UpdateOpEntry entry;
                                  entry.setQ(query);
                                  entry.setU(write_ops::UpdateModification::parseFromClassicUpdate(
                                      updatePattern));
                                  entry.setMulti(multi);
                                  entry.setUpsert(upsert);
                                  return entry;
                              }()});
                              return updateOp.toBSON({});
                          }(),
                          res);

        BatchedCommandResponse response;
        std::string errmsg;
        if (!response.parseBSON(res, &errmsg)) {
            return Status(ErrorCodes::FailedToParse, errmsg);
        }
        if (response.getOk()) {
            *numMatched = response.getN();
        }
        return response.toStatus();
    } catch (const DBException& e) {
        return e.toStatus();
    }
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
    std::int64_t numMatched;
    Status status = updateAuthzDocuments(
        opCtx, collectionName, query, updatePattern, upsert, false, &numMatched);
    if (!status.isOK()) {
        return status;
    }
    dassert(numMatched == 1 || numMatched == 0);
    if (numMatched == 0) {
        return Status(ErrorCodes::NoMatchingDocument, "No document found");
    }
    return Status::OK();
}

/**
 * Removes all documents matching "query" from "collectionName".
 *
 * Should only be called on collections with authorization documents in them
 * (ie admin.system.users and admin.system.roles).
 */
Status removeAuthzDocuments(OperationContext* opCtx,
                            const NamespaceString& collectionName,
                            const BSONObj& query,
                            std::int64_t* numRemoved) {
    try {
        DBDirectClient client(opCtx);

        BSONObj res;
        client.runCommand(collectionName.db().toString(),
                          [&] {
                              write_ops::Delete deleteOp(collectionName);
                              deleteOp.setDeletes({[&] {
                                  write_ops::DeleteOpEntry entry;
                                  entry.setQ(query);
                                  entry.setMulti(true);
                                  return entry;
                              }()});
                              return deleteOp.toBSON({});
                          }(),
                          res);

        BatchedCommandResponse response;
        std::string errmsg;
        if (!response.parseBSON(res, &errmsg)) {
            return Status(ErrorCodes::FailedToParse, errmsg);
        }
        if (response.getOk()) {
            *numRemoved = response.getN();
        }
        return response.toStatus();
    } catch (const DBException& e) {
        return e.toStatus();
    }
}

/**
 * Creates the given role object in the given database.
 */
Status insertRoleDocument(OperationContext* opCtx, const BSONObj& roleObj) {
    Status status =
        insertAuthzDocument(opCtx, AuthorizationManager::rolesCollectionNamespace, roleObj);
    if (status.isOK()) {
        return status;
    }
    if (status.code() == ErrorCodes::DuplicateKey) {
        std::string name = roleObj[AuthorizationManager::ROLE_NAME_FIELD_NAME].String();
        std::string source = roleObj[AuthorizationManager::ROLE_DB_FIELD_NAME].String();
        return Status(ErrorCodes::Error(51002),
                      str::stream() << "Role \"" << name << "@" << source << "\" already exists");
    }
    if (status.code() == ErrorCodes::UnknownError) {
        return Status(ErrorCodes::RoleModificationFailed, status.reason());
    }
    return status;
}

/**
 * Updates the given role object with the given update modifier.
 */
Status updateRoleDocument(OperationContext* opCtx, const RoleName& role, const BSONObj& updateObj) {
    Status status = updateOneAuthzDocument(
        opCtx,
        AuthorizationManager::rolesCollectionNamespace,
        BSON(AuthorizationManager::ROLE_NAME_FIELD_NAME
             << role.getRole() << AuthorizationManager::ROLE_DB_FIELD_NAME << role.getDB()),
        updateObj,
        false);
    if (status.isOK()) {
        return status;
    }
    if (status.code() == ErrorCodes::NoMatchingDocument) {
        return Status(ErrorCodes::RoleNotFound,
                      str::stream() << "Role " << role.getFullName() << " not found");
    }
    if (status.code() == ErrorCodes::UnknownError) {
        return Status(ErrorCodes::RoleModificationFailed, status.reason());
    }
    return status;
}

/**
 * Removes roles matching the given query.
 * Writes into *numRemoved the number of role documents that were modified.
 */
Status removeRoleDocuments(OperationContext* opCtx,
                           const BSONObj& query,
                           std::int64_t* numRemoved) {
    Status status = removeAuthzDocuments(
        opCtx, AuthorizationManager::rolesCollectionNamespace, query, numRemoved);
    if (status.code() == ErrorCodes::UnknownError) {
        return Status(ErrorCodes::RoleModificationFailed, status.reason());
    }
    return status;
}

/**
 * Creates the given user object in the given database.
 */
Status insertPrivilegeDocument(OperationContext* opCtx, const BSONObj& userObj) {
    Status status =
        insertAuthzDocument(opCtx, AuthorizationManager::usersCollectionNamespace, userObj);
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

    const auto status = updateOneAuthzDocument(
        opCtx, AuthorizationManager::usersCollectionNamespace, queryObj, updateObj, false);
    if (status.code() == ErrorCodes::UnknownError) {
        return {ErrorCodes::UserModificationFailed, status.reason()};
    }
    if (status.code() == ErrorCodes::NoMatchingDocument) {
        return {ErrorCodes::UserNotFound,
                str::stream() << "User " << user.getFullName() << " not found"};
    }
    return status;
}

/**
 * Convenience wrapper for above using only the UserName to match the original document.
 * Clarifies NoMatchingDocument result to reflect the user not existing.
 */
Status updatePrivilegeDocument(OperationContext* opCtx,
                               const UserName& user,
                               const BSONObj& updateObj) {
    const auto status = updatePrivilegeDocument(
        opCtx,
        user,
        BSON(AuthorizationManager::USER_NAME_FIELD_NAME
             << user.getUser() << AuthorizationManager::USER_DB_FIELD_NAME << user.getDB()),
        updateObj);
    return status;
}

/**
 * Removes users for the given database matching the given query.
 * Writes into *numRemoved the number of user documents that were modified.
 */
Status removePrivilegeDocuments(OperationContext* opCtx,
                                const BSONObj& query,
                                std::int64_t* numRemoved) {
    Status status = removeAuthzDocuments(
        opCtx, AuthorizationManager::usersCollectionNamespace, query, numRemoved);
    if (status.code() == ErrorCodes::UnknownError) {
        return Status(ErrorCodes::UserModificationFailed, status.reason());
    }
    return status;
}

/**
 * Updates the auth schema version document to reflect the current state of the system.
 * 'foundSchemaVersion' is the authSchemaVersion to update with.
 */
Status writeAuthSchemaVersionIfNeeded(OperationContext* opCtx,
                                      AuthorizationManager* authzManager,
                                      int foundSchemaVersion) {
    Status status = updateOneAuthzDocument(
        opCtx,
        AuthorizationManager::versionCollectionNamespace,
        AuthorizationManager::versionDocumentQuery,
        BSON("$set" << BSON(AuthorizationManager::schemaVersionFieldName << foundSchemaVersion)),
        true);  // upsert

    if (status == ErrorCodes::NoMatchingDocument) {  // SERVER-11492
        status = Status::OK();
    }

    return status;
}

class AuthzLockGuard {
    AuthzLockGuard(AuthzLockGuard&) = delete;
    AuthzLockGuard& operator=(AuthzLockGuard&) = delete;

public:
    enum InvalidationMode { kInvalidate, kReadOnly };

    AuthzLockGuard(OperationContext* opCtx, InvalidationMode mode)
        : _opCtx(opCtx),
          _authzManager(AuthorizationManager::get(_opCtx->getServiceContext())),
          _lock(_UMCMutexDecoration(opCtx->getServiceContext())),
          _mode(mode),
          _cacheGeneration(_authzManager->getCacheGeneration()) {}

    ~AuthzLockGuard() {
        if (!_lock.owns_lock() || _mode == kReadOnly) {
            return;
        }

        if (_authzManager->getCacheGeneration() == _cacheGeneration) {
            LOGV2_DEBUG(20509, 1, "User management command did not invalidate the user cache");
            _authzManager->invalidateUserCache(_opCtx);
        }
    }

    AuthzLockGuard(AuthzLockGuard&&) = default;
    AuthzLockGuard& operator=(AuthzLockGuard&&) = default;

private:
    static Decorable<ServiceContext>::Decoration<Mutex> _UMCMutexDecoration;

    OperationContext* _opCtx;
    AuthorizationManager* _authzManager;
    stdx::unique_lock<Latch> _lock;
    InvalidationMode _mode;
    OID _cacheGeneration;
};

Decorable<ServiceContext>::Decoration<Mutex> AuthzLockGuard::_UMCMutexDecoration =
    ServiceContext::declareDecoration<Mutex>();

/**
 * Returns Status::OK() if the current Auth schema version is at least the auth schema version
 * for the MongoDB 3.0 SCRAM auth mode.
 * Returns an error otherwise.
 */
StatusWith<AuthzLockGuard> requireWritableAuthSchema28SCRAM(OperationContext* opCtx,
                                                            AuthorizationManager* authzManager) {
    int foundSchemaVersion;
    // We take a MODE_X lock during writes because we want to be sure that we can read any pinned
    // user documents back out of the database after writing them during the user management
    // commands, and to ensure only one user management command is running at a time.
    AuthzLockGuard lk(opCtx, AuthzLockGuard::kInvalidate);
    Status status = authzManager->getAuthorizationVersion(opCtx, &foundSchemaVersion);
    if (!status.isOK()) {
        return status;
    }

    if (foundSchemaVersion < AuthorizationManager::schemaVersion28SCRAM) {
        return Status(ErrorCodes::AuthSchemaIncompatible,
                      str::stream()
                          << "User and role management commands require auth data to have "
                          << "at least schema version "
                          << AuthorizationManager::schemaVersion28SCRAM << " but found "
                          << foundSchemaVersion);
    }
    status = writeAuthSchemaVersionIfNeeded(opCtx, authzManager, foundSchemaVersion);
    if (!status.isOK()) {
        return status;
    }

    return std::move(lk);
}

/**
 * Returns Status::OK() if the current Auth schema version is at least the auth schema version
 * for MongoDB 2.6 during the upgrade process.
 * Returns an error otherwise.
 *
 * This method should only be called by READ-ONLY commands (usersInfo & rolesInfo)
 * because getAuthorizationVersion() will return the current max version without
 * reifying the authSchema setting in the admin database.
 *
 * If records are added thinking we're at one schema level, then the default is changed,
 * then the auth database would wind up in an inconsistent state.
 */
StatusWith<AuthzLockGuard> requireReadableAuthSchema26Upgrade(OperationContext* opCtx,
                                                              AuthorizationManager* authzManager) {
    int foundSchemaVersion;
    AuthzLockGuard lk(opCtx, AuthzLockGuard::kReadOnly);
    Status status = authzManager->getAuthorizationVersion(opCtx, &foundSchemaVersion);
    if (!status.isOK()) {
        return status;
    }

    if (foundSchemaVersion < AuthorizationManager::schemaVersion26Upgrade) {
        return Status(ErrorCodes::AuthSchemaIncompatible,
                      str::stream() << "The usersInfo and rolesInfo commands require auth data to "
                                    << "have at least schema version "
                                    << AuthorizationManager::schemaVersion26Upgrade << " but found "
                                    << foundSchemaVersion);
    }

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
            hashedPwd = password.toString();
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

void trimCredentials(OperationContext* opCtx,
                     const UserName& userName,
                     BSONObjBuilder* queryBuilder,
                     BSONObjBuilder* unsetBuilder,
                     const std::vector<StringData>& mechanisms) {
    auto* authzManager = AuthorizationManager::get(opCtx->getServiceContext());

    BSONObj userObj;
    uassertStatusOK(authzManager->getUserDescription(opCtx, userName, &userObj));

    const auto& credsElem = userObj["credentials"];
    uassert(ErrorCodes::UnsupportedFormat,
            "Unable to trim credentials from a user document with no credentials",
            credsElem.type() == Object);

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

template <typename T>
BSONArray vectorToBSON(const std::vector<T>& vec) {
    BSONArrayBuilder builder;
    for (const auto& val : vec) {
        builder.append(val.toBSON());
    }
    return builder.arr();
}

template <typename RequestT, typename ReplyT>
class CmdUMCTyped : public TypedCommand<CmdUMCTyped<RequestT, ReplyT>> {
public:
    using Request = RequestT;
    using Reply = ReplyT;
    using TC = TypedCommand<CmdUMCTyped<RequestT, ReplyT>>;

    class Invocation final : public TC::InvocationBase {
    public:
        using TC::InvocationBase::InvocationBase;
        using TC::InvocationBase::request;

        Reply typedRun(OperationContext* opCtx);

    private:
        bool supportsWriteConcern() const final {
            return true;
        }

        void doCheckAuthorization(OperationContext* opCtx) const final {
            auth::checkAuthForTypedCommand(opCtx->getClient(), request());
        }

        NamespaceString ns() const override {
            return NamespaceString(request().getDbName(), "");
        }
    };

    typename TC::AllowedOnSecondary secondaryAllowed(ServiceContext*) const final {
        return TC::AllowedOnSecondary::kNever;
    }
};

class CmdCreateUser : public CmdUMCTyped<CreateUserCommand, void> {
public:
    static constexpr StringData kPwdField = "pwd"_sd;

    StringData sensitiveFieldName() const final {
        return kPwdField;
    }
} cmdCreateUser;

template <>
void CmdUMCTyped<CreateUserCommand, void>::Invocation::typedRun(OperationContext* opCtx) {
    const auto& cmd = request();
    const auto& dbname = cmd.getDbName();

    // Validate input
    uassert(ErrorCodes::BadValue, "Cannot create users in the local database", dbname != "local");

    uassert(ErrorCodes::BadValue,
            "Username cannot contain NULL characters",
            cmd.getCommandParameter().find('\0') == std::string::npos);
    UserName userName(cmd.getCommandParameter(), dbname);

    uassert(ErrorCodes::BadValue,
            "Must provide a 'pwd' field for all user documents, except those"
            " with '$external' as the user's source db",
            (cmd.getPwd() != boost::none) || (dbname == "$external"));

    uassert(ErrorCodes::BadValue,
            "Cannot set the password for users defined on the '$external' database",
            (cmd.getPwd() == boost::none) || (dbname != "$external"));

    uassert(ErrorCodes::BadValue,
            "mechanisms field must not be empty",
            (cmd.getMechanisms() == boost::none) || !cmd.getMechanisms()->empty());

#ifdef MONGO_CONFIG_SSL
    auto configuration = opCtx->getClient()->session()->getSSLConfiguration();

    if ((dbname == "$external") && configuration &&
        configuration->isClusterMember(userName.getUser())) {
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
    userObjBuilder.append(AuthorizationManager::USER_NAME_FIELD_NAME, userName.getUser());
    userObjBuilder.append(AuthorizationManager::USER_DB_FIELD_NAME, userName.getDB());

    auto* serviceContext = opCtx->getClient()->getServiceContext();
    auto* authzManager = AuthorizationManager::get(serviceContext);

    auto lk = uassertStatusOK(requireWritableAuthSchema28SCRAM(opCtx, authzManager));

    int authzVersion;
    uassertStatusOK(authzManager->getAuthorizationVersion(opCtx, &authzVersion));

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
    uassertStatusOK(authzManager->rolesExist(opCtx, resolvedRoles));

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
    auto status = insertPrivilegeDocument(opCtx, userObj);
    authzManager->invalidateUserByName(opCtx, userName);
    uassertStatusOK(status);
}

class CmdUpdateUser : public CmdUMCTyped<UpdateUserCommand, void> {
public:
    static constexpr StringData kPwdField = "pwd"_sd;

    StringData sensitiveFieldName() const final {
        return kPwdField;
    }
} cmdUpdateUser;

template <>
void CmdUMCTyped<UpdateUserCommand, void>::Invocation::typedRun(OperationContext* opCtx) {
    const auto& cmd = request();
    const auto& dbname = cmd.getDbName();
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

    auto* serviceContext = opCtx->getClient()->getServiceContext();
    auto* authzManager = AuthorizationManager::get(serviceContext);

    auto lk = uassertStatusOK(requireWritableAuthSchema28SCRAM(opCtx, authzManager));

    // Role existence has to be checked after acquiring the update lock
    if (auto roles = cmd.getRoles()) {
        auto resolvedRoles = auth::resolveRoleNames(roles.get(), dbname);
        uassertStatusOK(authzManager->rolesExist(opCtx, resolvedRoles));
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
    authzManager->invalidateUserByName(opCtx, userName);
    uassertStatusOK(status);
}

CmdUMCTyped<DropUserCommand, void> cmdDropUser;
template <>
void CmdUMCTyped<DropUserCommand, void>::Invocation::typedRun(OperationContext* opCtx) {
    const auto& cmd = request();
    const auto& dbname = cmd.getDbName();
    UserName userName(cmd.getCommandParameter(), dbname);

    auto* serviceContext = opCtx->getClient()->getServiceContext();
    auto* authzManager = AuthorizationManager::get(serviceContext);
    auto lk = uassertStatusOK(requireWritableAuthSchema28SCRAM(opCtx, authzManager));

    audit::logDropUser(Client::getCurrent(), userName);

    std::int64_t numMatched;
    auto status = removePrivilegeDocuments(
        opCtx,
        BSON(AuthorizationManager::USER_NAME_FIELD_NAME
             << userName.getUser() << AuthorizationManager::USER_DB_FIELD_NAME << userName.getDB()),
        &numMatched);

    // Must invalidate even on bad status - what if the write succeeded but the GLE failed?
    authzManager->invalidateUserByName(opCtx, userName);
    uassertStatusOK(status);

    uassert(ErrorCodes::UserNotFound,
            str::stream() << "User '" << userName.getFullName() << "' not found",
            numMatched > 0);
}

CmdUMCTyped<DropAllUsersFromDatabaseCommand, DropAllUsersFromDatabaseReply>
    cmdDropAllUsersFromDatabase;
template <>
DropAllUsersFromDatabaseReply
CmdUMCTyped<DropAllUsersFromDatabaseCommand, DropAllUsersFromDatabaseReply>::Invocation::typedRun(
    OperationContext* opCtx) {
    const auto& cmd = request();
    const auto& dbname = cmd.getDbName();

    auto* client = opCtx->getClient();
    auto* serviceContext = client->getServiceContext();
    auto* authzManager = AuthorizationManager::get(serviceContext);
    auto lk = uassertStatusOK(requireWritableAuthSchema28SCRAM(opCtx, authzManager));

    audit::logDropAllUsersFromDatabase(client, dbname);

    std::int64_t numRemoved;
    auto status = removePrivilegeDocuments(
        opCtx, BSON(AuthorizationManager::USER_DB_FIELD_NAME << dbname), &numRemoved);

    // Must invalidate even on bad status - what if the write succeeded but the GLE failed?
    authzManager->invalidateUsersFromDB(opCtx, dbname);
    uassertStatusOK(status);

    DropAllUsersFromDatabaseReply reply;
    reply.setCount(numRemoved);
    return reply;
}

CmdUMCTyped<GrantRolesToUserCommand, void> cmdGrantRolesToUser;
template <>
void CmdUMCTyped<GrantRolesToUserCommand, void>::Invocation::typedRun(OperationContext* opCtx) {
    const auto& cmd = request();
    const auto& dbname = cmd.getDbName();
    UserName userName(cmd.getCommandParameter(), dbname);

    uassert(ErrorCodes::BadValue,
            "grantRolesToUser command requires a non-empty \"roles\" array",
            !cmd.getRoles().empty());

    auto* client = opCtx->getClient();
    auto* serviceContext = client->getServiceContext();
    auto* authzManager = AuthorizationManager::get(serviceContext);
    auto lk = uassertStatusOK(requireWritableAuthSchema28SCRAM(opCtx, authzManager));

    stdx::unordered_set<RoleName> userRoles;
    uassertStatusOK(getCurrentUserRoles(opCtx, authzManager, userName, &userRoles));

    auto resolvedRoleNames = auth::resolveRoleNames(cmd.getRoles(), dbname);
    uassertStatusOK(authzManager->rolesExist(opCtx, resolvedRoleNames));
    for (const auto& role : resolvedRoleNames) {
        userRoles.insert(role);
    }

    audit::logGrantRolesToUser(client, userName, resolvedRoleNames);
    auto newRolesBSONArray = containerToBSONArray(userRoles);
    auto status = updatePrivilegeDocument(
        opCtx, userName, BSON("$set" << BSON("roles" << newRolesBSONArray)));

    // Must invalidate even on bad status - what if the write succeeded but the GLE failed?
    authzManager->invalidateUserByName(opCtx, userName);
    uassertStatusOK(status);
}

CmdUMCTyped<RevokeRolesFromUserCommand, void> cmdRevokeRolesFromUser;
template <>
void CmdUMCTyped<RevokeRolesFromUserCommand, void>::Invocation::typedRun(OperationContext* opCtx) {
    const auto& cmd = request();
    const auto& dbname = cmd.getDbName();
    UserName userName(cmd.getCommandParameter(), dbname);

    uassert(ErrorCodes::BadValue,
            "revokeRolesFromUser command requires a non-empty \"roles\" array",
            !cmd.getRoles().empty());

    auto* client = opCtx->getClient();
    auto* serviceContext = client->getServiceContext();
    auto* authzManager = AuthorizationManager::get(serviceContext);
    auto lk = uassertStatusOK(requireWritableAuthSchema28SCRAM(opCtx, authzManager));

    stdx::unordered_set<RoleName> userRoles;
    uassertStatusOK(getCurrentUserRoles(opCtx, authzManager, userName, &userRoles));

    auto resolvedUserRoles = auth::resolveRoleNames(cmd.getRoles(), dbname);
    uassertStatusOK(authzManager->rolesExist(opCtx, resolvedUserRoles));
    for (const auto& role : resolvedUserRoles) {
        userRoles.erase(role);
    }

    audit::logRevokeRolesFromUser(client, userName, resolvedUserRoles);
    BSONArray newRolesBSONArray = containerToBSONArray(userRoles);
    auto status = updatePrivilegeDocument(
        opCtx, userName, BSON("$set" << BSON("roles" << newRolesBSONArray)));

    // Must invalidate even on bad status - what if the write succeeded but the GLE failed?
    authzManager->invalidateUserByName(opCtx, userName);
    uassertStatusOK(status);
}

class CmdUsersInfo : public BasicCommand {
public:
    CmdUsersInfo() : BasicCommand("usersInfo") {}

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kOptIn;
    }

    bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }

    std::string help() const override {
        return "Returns information about users.";
    }

    Status checkAuthForCommand(Client* client,
                               const std::string& dbname,
                               const BSONObj& cmdObj) const override {
        return auth::checkAuthForUsersInfoCommand(client, dbname, cmdObj);
    }

    bool run(OperationContext* opCtx,
             const std::string& dbname,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) override {
        auth::UsersInfoArgs args;
        Status status = auth::parseUsersInfoCommand(cmdObj, dbname, &args);
        uassertStatusOK(status);

        AuthorizationManager* authzManager = AuthorizationManager::get(opCtx->getServiceContext());
        auto lk = uassertStatusOK(requireReadableAuthSchema26Upgrade(opCtx, authzManager));

        if ((args.target != auth::UsersInfoArgs::Target::kExplicitUsers || args.filter) &&
            (args.showPrivileges ||
             args.authenticationRestrictionsFormat == AuthenticationRestrictionsFormat::kShow)) {
            uasserted(ErrorCodes::IllegalOperation,
                      "Privilege or restriction details require exact-match usersInfo "
                      "queries.");
        }

        BSONArrayBuilder usersArrayBuilder;
        if (args.target == auth::UsersInfoArgs::Target::kExplicitUsers &&
            (args.showPrivileges ||
             args.authenticationRestrictionsFormat == AuthenticationRestrictionsFormat::kShow)) {
            // If you want privileges or restrictions you need to call getUserDescription on each
            // user.
            for (size_t i = 0; i < args.userNames.size(); ++i) {
                BSONObj userDetails;
                status = authzManager->getUserDescription(opCtx, args.userNames[i], &userDetails);
                if (status.code() == ErrorCodes::UserNotFound) {
                    continue;
                }
                uassertStatusOK(status);

                // getUserDescription always includes credentials and restrictions, which may need
                // to be stripped out
                BSONObjBuilder strippedUser(usersArrayBuilder.subobjStart());
                for (const BSONElement& e : userDetails) {
                    if (e.fieldNameStringData() == "credentials") {
                        BSONArrayBuilder mechanismNamesBuilder;
                        BSONObj mechanismsObj = e.Obj();
                        for (const BSONElement& mechanismElement : mechanismsObj) {
                            mechanismNamesBuilder.append(mechanismElement.fieldNameStringData());
                        }
                        strippedUser.append("mechanisms", mechanismNamesBuilder.arr());

                        if (!args.showCredentials) {
                            continue;
                        }
                    }

                    if (e.fieldNameStringData() == "authenticationRestrictions" &&
                        args.authenticationRestrictionsFormat ==
                            AuthenticationRestrictionsFormat::kOmit) {
                        continue;
                    }

                    strippedUser.append(e);
                }
                strippedUser.doneFast();
            }
        } else {
            // If you don't need privileges, or authenticationRestrictions, you can just do a
            // regular query on system.users
            std::vector<BSONObj> pipeline;
            if (args.target == auth::UsersInfoArgs::Target::kGlobal) {
                // Leave the pipeline unconstrained, we want to return every user.
            } else if (args.target == auth::UsersInfoArgs::Target::kDB) {
                pipeline.push_back(
                    BSON("$match" << BSON(AuthorizationManager::USER_DB_FIELD_NAME << dbname)));
            } else {
                BSONArrayBuilder usersMatchArray;
                for (size_t i = 0; i < args.userNames.size(); ++i) {
                    usersMatchArray.append(BSON(AuthorizationManager::USER_NAME_FIELD_NAME
                                                << args.userNames[i].getUser()
                                                << AuthorizationManager::USER_DB_FIELD_NAME
                                                << args.userNames[i].getDB()));
                }
                pipeline.push_back(BSON("$match" << BSON("$or" << usersMatchArray.arr())));
            }
            // Order results by user field then db field, matching how UserNames are ordered
            pipeline.push_back(BSON("$sort" << BSON("user" << 1 << "db" << 1)));

            // Authentication restrictions are only rendered in the single user case.
            pipeline.push_back(BSON("$project" << BSON("authenticationRestrictions" << false)));

            // Rewrite the credentials object into an array of its fieldnames.
            pipeline.push_back(
                BSON("$addFields" << BSON("mechanisms"
                                          << BSON("$map" << BSON("input" << BSON("$objectToArray"
                                                                                 << "$credentials")
                                                                         << "as"
                                                                         << "cred"
                                                                         << "in"
                                                                         << "$$cred.k")))));

            // Remove credentials, they're not required in the output
            if (!args.showCredentials) {
                pipeline.push_back(BSON("$project" << BSON("credentials" << false)));
            }

            // Handle a user specified filter.
            if (args.filter) {
                pipeline.push_back(BSON("$match" << *args.filter));
            }

            DBDirectClient client(opCtx);

            rpc::OpMsgReplyBuilder replyBuilder;
            AggregationRequest aggRequest(AuthorizationManager::usersCollectionNamespace,
                                          std::move(pipeline));
            // Impose no cursor privilege requirements, as cursor is drained internally
            uassertStatusOK(runAggregate(opCtx,
                                         AuthorizationManager::usersCollectionNamespace,
                                         aggRequest,
                                         aggRequest.serializeToCommandObj().toBson(),
                                         PrivilegeVector(),
                                         &replyBuilder));
            auto bodyBuilder = replyBuilder.getBodyBuilder();
            CommandHelpers::appendSimpleCommandStatus(bodyBuilder, true);
            bodyBuilder.doneFast();
            auto response = CursorResponse::parseFromBSONThrowing(replyBuilder.releaseBody());
            DBClientCursor cursor(
                &client, response.getNSS(), response.getCursorId(), 0, 0, response.releaseBatch());

            while (cursor.more()) {
                usersArrayBuilder.append(cursor.next());
            }
        }
        result.append("users", usersArrayBuilder.arr());
        return true;
    }

} cmdUsersInfo;

CmdUMCTyped<CreateRoleCommand, void> cmdCreateRole;
template <>
void CmdUMCTyped<CreateRoleCommand, void>::Invocation::typedRun(OperationContext* opCtx) {
    const auto& cmd = request();
    const auto& dbname = cmd.getDbName();

    uassert(
        ErrorCodes::BadValue, "Role name must be non-empty", !cmd.getCommandParameter().empty());
    RoleName roleName(cmd.getCommandParameter(), dbname);

    uassert(ErrorCodes::BadValue, "Cannot create roles in the local database", dbname != "local");

    uassert(ErrorCodes::BadValue,
            "Cannot create roles in the $external database",
            dbname != "$external");

    uassert(ErrorCodes::BadValue,
            "Cannot create roles with the same name as a built-in role",
            !RoleGraph::isBuiltinRole(roleName));

    BSONObjBuilder roleObjBuilder;
    roleObjBuilder.append("_id", str::stream() << roleName.getDB() << "." << roleName.getRole());
    roleObjBuilder.append(AuthorizationManager::ROLE_NAME_FIELD_NAME, roleName.getRole());
    roleObjBuilder.append(AuthorizationManager::ROLE_DB_FIELD_NAME, roleName.getDB());

    BSONArray privileges;
    uassertStatusOK(privilegeVectorToBSONArray(cmd.getPrivileges(), &privileges));
    roleObjBuilder.append("privileges", privileges);

    auto resolvedRoleNames = auth::resolveRoleNames(cmd.getRoles(), dbname);
    roleObjBuilder.append("roles", containerToBSONArray(resolvedRoleNames));

    boost::optional<BSONArray> bsonAuthRestrictions;
    if (auto ar = cmd.getAuthenticationRestrictions(); ar && !ar->empty()) {
        bsonAuthRestrictions = vectorToBSON(ar.get());
        roleObjBuilder.append("authenticationRestrictions", bsonAuthRestrictions.get());
    }

    auto* client = opCtx->getClient();
    auto* serviceContext = client->getServiceContext();
    auto* authzManager = AuthorizationManager::get(serviceContext);
    auto lk = uassertStatusOK(requireWritableAuthSchema28SCRAM(opCtx, authzManager));

    // Role existence has to be checked after acquiring the update lock
    uassertStatusOK(checkOkayToGrantRolesToRole(opCtx, roleName, resolvedRoleNames, authzManager));
    uassertStatusOK(checkOkayToGrantPrivilegesToRole(roleName, cmd.getPrivileges()));

    audit::logCreateRole(
        client, roleName, resolvedRoleNames, cmd.getPrivileges(), bsonAuthRestrictions);

    uassertStatusOK(insertRoleDocument(opCtx, roleObjBuilder.done()));
}

CmdUMCTyped<UpdateRoleCommand, void> cmdUpdateRole;
template <>
void CmdUMCTyped<UpdateRoleCommand, void>::Invocation::typedRun(OperationContext* opCtx) {
    const auto& cmd = request();
    const auto& dbname = cmd.getDbName();
    RoleName roleName(cmd.getCommandParameter(), dbname);

    const bool hasRoles = cmd.getRoles() != boost::none;
    const bool hasPrivs = cmd.getPrivileges() != boost::none;
    const bool hasAuthRestrictions = cmd.getAuthenticationRestrictions() != boost::none;
    uassert(ErrorCodes::BadValue,
            "Must specify at least one field to update in updateRole",
            hasRoles || hasPrivs || hasAuthRestrictions);

    BSONObjBuilder updateSetBuilder;
    BSONObjBuilder updateUnsetBuilder;

    if (auto privs = cmd.getPrivileges()) {
        BSONArray privileges;
        uassertStatusOK(privilegeVectorToBSONArray(privs.get(), &privileges));
        updateSetBuilder.append("privileges", privileges);
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
    auto* serviceContext = client->getServiceContext();
    auto* authzManager = AuthorizationManager::get(serviceContext);
    auto lk = uassertStatusOK(requireWritableAuthSchema28SCRAM(opCtx, authzManager));

    // Role existence has to be checked after acquiring the update lock
    uassertStatusOK(authzManager->rolesExist(opCtx, {roleName}));

    if (optRoles) {
        uassertStatusOK(checkOkayToGrantRolesToRole(opCtx, roleName, *optRoles, authzManager));
    }

    auto privs = cmd.getPrivileges();
    if (privs) {
        uassertStatusOK(checkOkayToGrantPrivilegesToRole(roleName, privs.get()));
    }

    audit::logUpdateRole(
        client, roleName, optRoles ? &*optRoles : nullptr, privs ? &*privs : nullptr, authRest);

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
    authzManager->invalidateUserCache(opCtx);
    uassertStatusOK(status);
}

CmdUMCTyped<GrantPrivilegesToRoleCommand, void> cmdGrantPrivilegesToRole;
template <>
void CmdUMCTyped<GrantPrivilegesToRoleCommand, void>::Invocation::typedRun(
    OperationContext* opCtx) {
    const auto& cmd = request();
    const auto& dbname = cmd.getDbName();
    RoleName roleName(cmd.getCommandParameter(), dbname);

    uassert(ErrorCodes::BadValue,
            "grantPrivilegesToRole command requires a non-empty \"privileges\" array",
            !cmd.getPrivileges().empty());

    uassert(ErrorCodes::BadValue,
            str::stream() << roleName.getFullName() << " is a built-in role and cannot be modified",
            !RoleGraph::isBuiltinRole(roleName));

    auto* client = opCtx->getClient();
    auto* serviceContext = client->getServiceContext();
    auto* authzManager = AuthorizationManager::get(serviceContext);
    auto lk = uassertStatusOK(requireWritableAuthSchema28SCRAM(opCtx, authzManager));

    uassertStatusOK(checkOkayToGrantPrivilegesToRole(roleName, cmd.getPrivileges()));

    // Add additional privileges to existing set.
    auto data = uassertStatusOK(authzManager->resolveRoles(
        opCtx, {roleName}, AuthorizationManager::ResolveRoleOption::kDirectPrivileges));
    auto privileges = std::move(data.privileges.get());
    for (const auto& priv : cmd.getPrivileges()) {
        Privilege::addPrivilegeToPrivilegeVector(&privileges, priv);
    }

    // Build up update modifier object to $set privileges.
    mutablebson::Document updateObj;
    mutablebson::Element setElement = updateObj.makeElementObject("$set");
    uassertStatusOK(updateObj.root().pushBack(setElement));
    mutablebson::Element privilegesElement = updateObj.makeElementArray("privileges");
    uassertStatusOK(setElement.pushBack(privilegesElement));
    uassertStatusOK(Privilege::getBSONForPrivileges(privileges, privilegesElement));

    BSONObjBuilder updateBSONBuilder;
    updateObj.writeTo(&updateBSONBuilder);

    audit::logGrantPrivilegesToRole(client, roleName, cmd.getPrivileges());

    auto status = updateRoleDocument(opCtx, roleName, updateBSONBuilder.done());
    // Must invalidate even on bad status - what if the write succeeded but the GLE failed?
    authzManager->invalidateUserCache(opCtx);
    uassertStatusOK(status);
}

CmdUMCTyped<RevokePrivilegesFromRoleCommand, void> cmdRevokePrivilegesFromRole;
template <>
void CmdUMCTyped<RevokePrivilegesFromRoleCommand, void>::Invocation::typedRun(
    OperationContext* opCtx) {
    const auto& cmd = request();
    const auto& dbname = cmd.getDbName();
    RoleName roleName(cmd.getCommandParameter(), dbname);

    uassert(ErrorCodes::BadValue,
            "revokePrivilegesFromRole command requires a non-empty \"privileges\" array",
            !cmd.getPrivileges().empty());

    uassert(ErrorCodes::BadValue,
            str::stream() << roleName.getFullName() << " is a built-in role and cannot be modified",
            !RoleGraph::isBuiltinRole(roleName));

    auto* client = opCtx->getClient();
    auto* serviceContext = client->getServiceContext();
    auto* authzManager = AuthorizationManager::get(serviceContext);
    auto lk = uassertStatusOK(requireWritableAuthSchema28SCRAM(opCtx, authzManager));

    auto data = uassertStatusOK(authzManager->resolveRoles(
        opCtx, {roleName}, AuthorizationManager::ResolveRoleOption::kDirectPrivileges));
    auto privileges = std::move(data.privileges.get());
    for (const auto& rmPriv : cmd.getPrivileges()) {
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
    mutablebson::Document updateObj;
    mutablebson::Element setElement = updateObj.makeElementObject("$set");
    uassertStatusOK(updateObj.root().pushBack(setElement));
    mutablebson::Element privilegesElement = updateObj.makeElementArray("privileges");
    uassertStatusOK(setElement.pushBack(privilegesElement));
    uassertStatusOK(Privilege::getBSONForPrivileges(privileges, privilegesElement));

    audit::logRevokePrivilegesFromRole(client, roleName, cmd.getPrivileges());

    BSONObjBuilder updateBSONBuilder;
    updateObj.writeTo(&updateBSONBuilder);

    auto status = updateRoleDocument(opCtx, roleName, updateBSONBuilder.done());
    // Must invalidate even on bad status - what if the write succeeded but the GLE failed?
    authzManager->invalidateUserCache(opCtx);
    uassertStatusOK(status);
}

CmdUMCTyped<GrantRolesToRoleCommand, void> cmdGrantRolesToRole;
template <>
void CmdUMCTyped<GrantRolesToRoleCommand, void>::Invocation::typedRun(OperationContext* opCtx) {
    const auto& cmd = request();
    const auto& dbname = cmd.getDbName();
    RoleName roleName(cmd.getCommandParameter(), dbname);

    uassert(ErrorCodes::BadValue,
            "grantRolesToRole command requires a non-empty \"roles\" array",
            !cmd.getRoles().empty());

    uassert(ErrorCodes::BadValue,
            str::stream() << roleName.getFullName() << " is a built-in role and cannot be modified",
            !RoleGraph::isBuiltinRole(roleName));

    auto rolesToAdd = auth::resolveRoleNames(cmd.getRoles(), dbname);

    auto* client = opCtx->getClient();
    auto* serviceContext = client->getServiceContext();
    auto* authzManager = AuthorizationManager::get(serviceContext);
    auto lk = uassertStatusOK(requireWritableAuthSchema28SCRAM(opCtx, authzManager));

    // Check for cycles
    uassertStatusOK(checkOkayToGrantRolesToRole(opCtx, roleName, rolesToAdd, authzManager));

    // Add new roles to existing roles
    auto data = uassertStatusOK(authzManager->resolveRoles(
        opCtx, {roleName}, AuthorizationManager::ResolveRoleOption::kDirectRoles));
    auto directRoles = std::move(data.roles.get());
    directRoles.insert(rolesToAdd.cbegin(), rolesToAdd.cend());

    audit::logGrantRolesToRole(client, roleName, rolesToAdd);

    auto status = updateRoleDocument(
        opCtx, roleName, BSON("$set" << BSON("roles" << containerToBSONArray(directRoles))));
    // Must invalidate even on bad status - what if the write succeeded but the GLE failed?
    authzManager->invalidateUserCache(opCtx);
    uassertStatusOK(status);
}

CmdUMCTyped<RevokeRolesFromRoleCommand, void> cmdRevokeRolesFromRole;
template <>
void CmdUMCTyped<RevokeRolesFromRoleCommand, void>::Invocation::typedRun(OperationContext* opCtx) {
    const auto& cmd = request();
    const auto& dbname = cmd.getDbName();
    RoleName roleName(cmd.getCommandParameter(), dbname);

    uassert(ErrorCodes::BadValue,
            "revokeRolesFromRole command requires a non-empty \"roles\" array",
            !cmd.getRoles().empty());

    uassert(ErrorCodes::BadValue,
            str::stream() << roleName.getFullName() << " is a built-in role and cannot be modified",
            !RoleGraph::isBuiltinRole(roleName));

    auto rolesToRemove = auth::resolveRoleNames(cmd.getRoles(), dbname);

    auto* client = opCtx->getClient();
    auto* serviceContext = client->getServiceContext();
    auto* authzManager = AuthorizationManager::get(serviceContext);
    auto lk = uassertStatusOK(requireWritableAuthSchema28SCRAM(opCtx, authzManager));

    // Remove roles from existing set.
    auto data = uassertStatusOK(authzManager->resolveRoles(
        opCtx, {roleName}, AuthorizationManager::ResolveRoleOption::kDirectRoles));
    auto roles = std::move(data.roles.get());
    for (const auto& roleToRemove : rolesToRemove) {
        roles.erase(roleToRemove);
    }

    audit::logRevokeRolesFromRole(client, roleName, rolesToRemove);

    auto status = updateRoleDocument(
        opCtx, roleName, BSON("$set" << BSON("roles" << containerToBSONArray(roles))));
    // Must invalidate even on bad status - what if the write succeeded but the GLE failed?
    authzManager->invalidateUserCache(opCtx);
    uassertStatusOK(status);
}

CmdUMCTyped<DropRoleCommand, void> cmdDropRole;
template <>
void CmdUMCTyped<DropRoleCommand, void>::Invocation::typedRun(OperationContext* opCtx) {
    const auto& cmd = request();
    const auto& dbname = cmd.getDbName();
    RoleName roleName(cmd.getCommandParameter(), dbname);

    uassert(ErrorCodes::BadValue,
            str::stream() << roleName.getFullName() << " is a built-in role and cannot be modified",
            !RoleGraph::isBuiltinRole(roleName));

    auto* client = opCtx->getClient();
    auto* serviceContext = client->getServiceContext();
    auto* authzManager = AuthorizationManager::get(serviceContext);
    auto lk = uassertStatusOK(requireWritableAuthSchema28SCRAM(opCtx, authzManager));

    uassertStatusOK(authzManager->rolesExist(opCtx, {roleName}));

    // From here on, we always want to invalidate the user cache before returning.
    auto invalidateGuard = makeGuard([&] {
        try {
            authzManager->invalidateUserCache(opCtx);
        } catch (const AssertionException& ex) {
            LOGV2_WARNING(4907701, "Failed invalidating user cache", "exception"_attr = ex);
        }
    });

    // Remove this role from all users
    std::int64_t numMatched;
    auto status = updateAuthzDocuments(
        opCtx,
        AuthorizationManager::usersCollectionNamespace,
        BSON("roles" << BSON("$elemMatch" << BSON(AuthorizationManager::ROLE_NAME_FIELD_NAME
                                                  << roleName.getRole()
                                                  << AuthorizationManager::ROLE_DB_FIELD_NAME
                                                  << roleName.getDB()))),
        BSON("$pull" << BSON("roles" << BSON(AuthorizationManager::ROLE_NAME_FIELD_NAME
                                             << roleName.getRole()
                                             << AuthorizationManager::ROLE_DB_FIELD_NAME
                                             << roleName.getDB()))),
        false,
        true,
        &numMatched);
    if (!status.isOK()) {
        uassertStatusOK(useDefaultCode(status, ErrorCodes::UserModificationFailed)
                            .withContext(str::stream()
                                         << "Failed to remove role " << roleName.getFullName()
                                         << " from all users"));
    }

    // Remove this role from all other roles
    status = updateAuthzDocuments(
        opCtx,
        AuthorizationManager::rolesCollectionNamespace,
        BSON("roles" << BSON("$elemMatch" << BSON(AuthorizationManager::ROLE_NAME_FIELD_NAME
                                                  << roleName.getRole()
                                                  << AuthorizationManager::ROLE_DB_FIELD_NAME
                                                  << roleName.getDB()))),
        BSON("$pull" << BSON("roles" << BSON(AuthorizationManager::ROLE_NAME_FIELD_NAME
                                             << roleName.getRole()
                                             << AuthorizationManager::ROLE_DB_FIELD_NAME
                                             << roleName.getDB()))),
        false,
        true,
        &numMatched);
    if (!status.isOK()) {
        uassertStatusOK(useDefaultCode(status, ErrorCodes::RoleModificationFailed)
                            .withContext(str::stream()
                                         << "Removed role " << roleName.getFullName()
                                         << " from all users but failed to remove from all roles"));
    }

    audit::logDropRole(client, roleName);

    // Finally, remove the actual role document
    status = removeRoleDocuments(
        opCtx,
        BSON(AuthorizationManager::ROLE_NAME_FIELD_NAME
             << roleName.getRole() << AuthorizationManager::ROLE_DB_FIELD_NAME << roleName.getDB()),
        &numMatched);
    if (!status.isOK()) {
        uassertStatusOK(status.withContext(
            str::stream() << "Removed role " << roleName.getFullName()
                          << " from all users and roles but failed to actually delete"
                             " the role itself"));
    }

    dassert(numMatched == 0 || numMatched == 1);
    if (numMatched == 0) {
        uasserted(ErrorCodes::RoleNotFound,
                  str::stream() << "Role '" << roleName.getFullName() << "' not found");
    }
}

CmdUMCTyped<DropAllRolesFromDatabaseCommand, DropAllRolesFromDatabaseReply>
    cmdDropAllRolesFromDatabase;
template <>
DropAllRolesFromDatabaseReply
CmdUMCTyped<DropAllRolesFromDatabaseCommand, DropAllRolesFromDatabaseReply>::Invocation::typedRun(
    OperationContext* opCtx) {
    const auto& cmd = request();
    const auto& dbname = cmd.getDbName();

    auto* client = opCtx->getClient();
    auto* serviceContext = client->getServiceContext();
    auto* authzManager = AuthorizationManager::get(serviceContext);
    auto lk = uassertStatusOK(requireWritableAuthSchema28SCRAM(opCtx, authzManager));

    // From here on, we always want to invalidate the user cache before returning.
    auto invalidateGuard = makeGuard([opCtx, authzManager] {
        try {
            authzManager->invalidateUserCache(opCtx);
        } catch (const AssertionException& ex) {
            LOGV2_WARNING(4907700, "Failed invalidating user cache", "exception"_attr = ex);
        }
    });

    // Remove these roles from all users
    std::int64_t numMatched;
    auto status = updateAuthzDocuments(
        opCtx,
        AuthorizationManager::usersCollectionNamespace,
        BSON("roles" << BSON(AuthorizationManager::ROLE_DB_FIELD_NAME << dbname)),
        BSON("$pull" << BSON("roles" << BSON(AuthorizationManager::ROLE_DB_FIELD_NAME << dbname))),
        false,
        true,
        &numMatched);
    if (!status.isOK()) {
        uassertStatusOK(useDefaultCode(status, ErrorCodes::UserModificationFailed)
                            .withContext(str::stream() << "Failed to remove roles from \"" << dbname
                                                       << "\" db from all users"));
    }

    // Remove these roles from all other roles
    std::string sourceFieldName = str::stream()
        << "roles." << AuthorizationManager::ROLE_DB_FIELD_NAME;
    status = updateAuthzDocuments(
        opCtx,
        AuthorizationManager::rolesCollectionNamespace,
        BSON(sourceFieldName << dbname),
        BSON("$pull" << BSON("roles" << BSON(AuthorizationManager::ROLE_DB_FIELD_NAME << dbname))),
        false,
        true,
        &numMatched);
    if (!status.isOK()) {
        uassertStatusOK(useDefaultCode(status, ErrorCodes::RoleModificationFailed)
                            .withContext(str::stream() << "Failed to remove roles from \"" << dbname
                                                       << "\" db from all roles"));
    }

    audit::logDropAllRolesFromDatabase(Client::getCurrent(), dbname);
    // Finally, remove the actual role documents
    status = removeRoleDocuments(
        opCtx, BSON(AuthorizationManager::ROLE_DB_FIELD_NAME << dbname), &numMatched);
    if (!status.isOK()) {
        uassertStatusOK(status.withContext(
            str::stream() << "Removed roles from \"" << dbname
                          << "\" db "
                             " from all users and roles but failed to actually delete"
                             " those roles themselves"));
    }

    DropAllRolesFromDatabaseReply reply;
    reply.setCount(numMatched);
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
 *                    these roles. This format may change over time with changes to the auth
 *                    schema.
 */
class CmdRolesInfo : public BasicCommand {
public:
    CmdRolesInfo() : BasicCommand("rolesInfo") {}

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kOptIn;
    }

    bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }

    std::string help() const override {
        return "Returns information about roles.";
    }

    Status checkAuthForCommand(Client* client,
                               const std::string& dbname,
                               const BSONObj& cmdObj) const override {
        return auth::checkAuthForRolesInfoCommand(client, dbname, cmdObj);
    }

    bool run(OperationContext* opCtx,
             const std::string& dbname,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) override {
        auth::RolesInfoArgs args;
        uassertStatusOK(auth::parseRolesInfoCommand(cmdObj, dbname, &args));

        AuthorizationManager* authzManager = AuthorizationManager::get(opCtx->getServiceContext());
        auto lk = uassertStatusOK(requireReadableAuthSchema26Upgrade(opCtx, authzManager));

        if (args.allForDB) {
            if (args.privilegeFormat == PrivilegeFormat::kShowAsUserFragment) {
                uasserted(ErrorCodes::IllegalOperation,
                          "Cannot get user fragment for all roles in a database");
            }

            BSONArrayBuilder rolesBuilder(result.subarrayStart("roles"));
            uassertStatusOK(
                authzManager->getRoleDescriptionsForDB(opCtx,
                                                       dbname,
                                                       args.privilegeFormat,
                                                       args.authenticationRestrictionsFormat,
                                                       args.showBuiltinRoles,
                                                       &rolesBuilder));
        } else {
            BSONObj roleDetails;
            uassertStatusOK(authzManager->getRolesDescription(opCtx,
                                                              args.roleNames,
                                                              args.privilegeFormat,
                                                              args.authenticationRestrictionsFormat,
                                                              &roleDetails));

            if (args.privilegeFormat == PrivilegeFormat::kShowAsUserFragment) {
                result.append("userFragment", roleDetails);
            } else {
                result.append("roles", BSONArray(roleDetails));
            }
        }

        return true;
    }

} cmdRolesInfo;

class CmdInvalidateUserCache : public BasicCommand {
public:
    CmdInvalidateUserCache() : BasicCommand("invalidateUserCache") {}

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kAlways;
    }

    bool adminOnly() const override {
        return true;
    }

    bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }

    std::string help() const override {
        return "Invalidates the in-memory cache of user information";
    }

    Status checkAuthForCommand(Client* client,
                               const std::string& dbname,
                               const BSONObj& cmdObj) const override {
        return auth::checkAuthForInvalidateUserCacheCommand(client);
    }

    bool run(OperationContext* opCtx,
             const std::string& dbname,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) override {
        AuthorizationManager* authzManager = AuthorizationManager::get(opCtx->getServiceContext());
        auto lk = requireReadableAuthSchema26Upgrade(opCtx, authzManager);
        authzManager->invalidateUserCache(opCtx);
        return true;
    }

} cmdInvalidateUserCache;

class CmdGetCacheGeneration : public BasicCommand {
public:
    CmdGetCacheGeneration() : BasicCommand("_getUserCacheGeneration") {}

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kAlways;
    }

    bool adminOnly() const override {
        return true;
    }

    bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }

    std::string help() const override {
        return "internal";
    }

    Status checkAuthForCommand(Client* client,
                               const std::string& dbname,
                               const BSONObj& cmdObj) const override {
        return auth::checkAuthForGetUserCacheGenerationCommand(client);
    }

    bool run(OperationContext* opCtx,
             const std::string& dbname,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) override {
        uassert(ErrorCodes::IllegalOperation,
                "_getUserCacheGeneration can only be run on config servers",
                serverGlobalParams.clusterRole == ClusterRole::ConfigServer);
        AuthorizationManager* authzManager = AuthorizationManager::get(opCtx->getServiceContext());
        result.append("cacheGeneration", authzManager->getCacheGeneration());
        return true;
    }

} cmdGetCacheGeneration;

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
class CmdMergeAuthzCollections : public BasicCommand {
public:
    CmdMergeAuthzCollections() : BasicCommand("_mergeAuthzCollections") {}

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }

    bool supportsWriteConcern(const BSONObj& cmd) const override {
        return true;
    }

    bool adminOnly() const {
        return true;
    }

    std::string help() const override {
        return "Internal command used by mongorestore for updating user/role data";
    }

    Status checkAuthForCommand(Client* client,
                               const std::string& dbname,
                               const BSONObj& cmdObj) const override {
        return auth::checkAuthForMergeAuthzCollectionsCommand(client, cmdObj);
    }

    static UserName extractUserNameFromBSON(const BSONObj& userObj) {
        std::string name;
        std::string db;
        Status status =
            bsonExtractStringField(userObj, AuthorizationManager::USER_NAME_FIELD_NAME, &name);
        uassertStatusOK(status);
        status = bsonExtractStringField(userObj, AuthorizationManager::USER_DB_FIELD_NAME, &db);
        uassertStatusOK(status);
        return UserName(name, db);
    }

    static RoleName extractRoleNameFromBSON(const BSONObj& roleObj) {
        std::string name;
        std::string db;
        Status status =
            bsonExtractStringField(roleObj, AuthorizationManager::ROLE_NAME_FIELD_NAME, &name);
        uassertStatusOK(status);
        status = bsonExtractStringField(roleObj, AuthorizationManager::ROLE_DB_FIELD_NAME, &db);
        uassertStatusOK(status);
        return RoleName(name, db);
    }

    /**
     * Audits the fact that we are creating or updating the user described by userObj.
     */
    static void auditCreateOrUpdateUser(const BSONObj& userObj, bool create) {
        UserName userName = extractUserNameFromBSON(userObj);
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
     * Audits the fact that we are creating or updating the role described by roleObj.
     */
    static void auditCreateOrUpdateRole(const BSONObj& roleObj, bool create) {
        RoleName roleName = extractRoleNameFromBSON(roleObj);
        std::vector<RoleName> roles;
        std::vector<Privilege> privileges;
        uassertStatusOK(auth::parseRoleNamesFromBSONArray(
            BSONArray(roleObj["roles"].Obj()), roleName.getDB(), &roles));
        uassertStatusOK(auth::parseAndValidatePrivilegeArray(BSONArray(roleObj["privileges"].Obj()),
                                                             &privileges));

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
     * Designed to be used as a callback to be called on every user object in the result
     * set of a query over the tempUsersCollection provided to the command.  For each user
     * in the temp collection that is defined on the given db, adds that user to the actual
     * admin.system.users collection.
     * Also removes any users it encounters from the usersToDrop set.
     */
    static void addUser(OperationContext* opCtx,
                        AuthorizationManager* authzManager,
                        StringData db,
                        bool update,
                        stdx::unordered_set<UserName>* usersToDrop,
                        const BSONObj& userObj) {
        UserName userName = extractUserNameFromBSON(userObj);
        if (!db.empty() && userName.getDB() != db) {
            return;
        }

        if (update && usersToDrop->count(userName)) {
            auditCreateOrUpdateUser(userObj, false);
            Status status = updatePrivilegeDocument(opCtx, userName, userObj);
            if (!status.isOK()) {
                // Match the behavior of mongorestore to continue on failure
                LOGV2_WARNING(
                    20510,
                    "Could not update user {user} in _mergeAuthzCollections command: {error}",
                    "Could not update user during _mergeAuthzCollections command",
                    "user"_attr = userName,
                    "error"_attr = redact(status));
            }
        } else {
            auditCreateOrUpdateUser(userObj, true);
            Status status = insertPrivilegeDocument(opCtx, userObj);
            if (!status.isOK()) {
                // Match the behavior of mongorestore to continue on failure
                LOGV2_WARNING(
                    20511,
                    "Could not insert user {user} in _mergeAuthzCollections command: {error}",
                    "Could not insert user during _mergeAuthzCollections command",
                    "user"_attr = userName,
                    "error"_attr = redact(status));
            }
        }
        usersToDrop->erase(userName);
    }

    /**
     * Designed to be used as a callback to be called on every role object in the result
     * set of a query over the tempRolesCollection provided to the command.  For each role
     * in the temp collection that is defined on the given db, adds that role to the actual
     * admin.system.roles collection.
     * Also removes any roles it encounters from the rolesToDrop set.
     */
    static void addRole(OperationContext* opCtx,
                        AuthorizationManager* authzManager,
                        StringData db,
                        bool update,
                        stdx::unordered_set<RoleName>* rolesToDrop,
                        const BSONObj roleObj) {
        RoleName roleName = extractRoleNameFromBSON(roleObj);
        if (!db.empty() && roleName.getDB() != db) {
            return;
        }

        if (update && rolesToDrop->count(roleName)) {
            auditCreateOrUpdateRole(roleObj, false);
            Status status = updateRoleDocument(opCtx, roleName, roleObj);
            if (!status.isOK()) {
                // Match the behavior of mongorestore to continue on failure
                LOGV2_WARNING(
                    20512,
                    "Could not update role {role} in _mergeAuthzCollections command: {error}",
                    "Could not update role during _mergeAuthzCollections command",
                    "role"_attr = roleName,
                    "error"_attr = redact(status));
            }
        } else {
            auditCreateOrUpdateRole(roleObj, true);
            Status status = insertRoleDocument(opCtx, roleObj);
            if (!status.isOK()) {
                // Match the behavior of mongorestore to continue on failure
                LOGV2_WARNING(
                    20513,
                    "Could not insert role {role} in _mergeAuthzCollections command: {error}",
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
    static Status processUsers(OperationContext* opCtx,
                               AuthorizationManager* authzManager,
                               StringData usersCollName,
                               StringData db,
                               bool drop) {
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

            Status status =
                queryAuthzDocument(opCtx,
                                   AuthorizationManager::usersCollectionNamespace,
                                   query,
                                   fields,
                                   [&](const BSONObj& userObj) {
                                       usersToDrop.insert(extractUserNameFromBSON(userObj));
                                   });
            if (!status.isOK()) {
                return status;
            }
        }

        Status status = queryAuthzDocument(
            opCtx,
            NamespaceString(usersCollName),
            db.empty() ? BSONObj() : BSON(AuthorizationManager::USER_DB_FIELD_NAME << db),
            BSONObj(),
            [&](const BSONObj& userObj) {
                return addUser(opCtx, authzManager, db, drop, &usersToDrop, userObj);
            });
        if (!status.isOK()) {
            return status;
        }

        if (drop) {
            std::int64_t numRemoved;
            for (const UserName& userName : usersToDrop) {
                audit::logDropUser(Client::getCurrent(), userName);
                status = removePrivilegeDocuments(opCtx,
                                                  BSON(AuthorizationManager::USER_NAME_FIELD_NAME
                                                       << userName.getUser().toString()
                                                       << AuthorizationManager::USER_DB_FIELD_NAME
                                                       << userName.getDB().toString()),
                                                  &numRemoved);
                if (!status.isOK()) {
                    return status;
                }
                dassert(numRemoved == 1);
            }
        }

        return Status::OK();
    }

    /**
     * Moves all user objects from usersCollName into admin.system.users.  If drop is true,
     * removes any users that were in admin.system.users but not in usersCollName.
     */
    static Status processRoles(OperationContext* opCtx,
                               AuthorizationManager* authzManager,
                               StringData rolesCollName,
                               StringData db,
                               bool drop) {
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

            Status status =
                queryAuthzDocument(opCtx,
                                   AuthorizationManager::rolesCollectionNamespace,
                                   query,
                                   fields,
                                   [&](const BSONObj& roleObj) {
                                       return rolesToDrop.insert(extractRoleNameFromBSON(roleObj));
                                   });
            if (!status.isOK()) {
                return status;
            }
        }

        Status status = queryAuthzDocument(
            opCtx,
            NamespaceString(rolesCollName),
            db.empty() ? BSONObj() : BSON(AuthorizationManager::ROLE_DB_FIELD_NAME << db),
            BSONObj(),
            [&](const BSONObj& roleObj) {
                return addRole(opCtx, authzManager, db, drop, &rolesToDrop, roleObj);
            });
        if (!status.isOK()) {
            return status;
        }

        if (drop) {
            std::int64_t numRemoved;
            for (stdx::unordered_set<RoleName>::iterator it = rolesToDrop.begin();
                 it != rolesToDrop.end();
                 ++it) {
                const RoleName& roleName = *it;
                audit::logDropRole(Client::getCurrent(), roleName);
                status = removeRoleDocuments(opCtx,
                                             BSON(AuthorizationManager::ROLE_NAME_FIELD_NAME
                                                  << roleName.getRole().toString()
                                                  << AuthorizationManager::ROLE_DB_FIELD_NAME
                                                  << roleName.getDB().toString()),
                                             &numRemoved);
                if (!status.isOK()) {
                    return status;
                }
                dassert(numRemoved == 1);
            }
        }

        return Status::OK();
    }

    bool run(OperationContext* opCtx,
             const std::string& dbname,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) override {
        auth::MergeAuthzCollectionsArgs args;
        Status status = auth::parseMergeAuthzCollectionsCommand(cmdObj, &args);
        uassertStatusOK(status);

        if (args.usersCollName.empty() && args.rolesCollName.empty()) {
            uasserted(ErrorCodes::BadValue,
                      "Must provide at least one of \"tempUsersCollection\" and "
                      "\"tempRolescollection\"");
        }

        ServiceContext* serviceContext = opCtx->getClient()->getServiceContext();
        AuthorizationManager* authzManager = AuthorizationManager::get(serviceContext);

        auto lk = uassertStatusOK(requireWritableAuthSchema28SCRAM(opCtx, authzManager));
        // From here on, we always want to invalidate the user cache before returning.
        auto invalidateGuard = makeGuard([&] {
            try {
                authzManager->invalidateUserCache(opCtx);
            } catch (const DBException& e) {
                // Since this may be called after a uassert, we want to catch any uasserts
                // that come out of invalidating the user cache and explicitly append it to
                // the command response.
                CommandHelpers::appendCommandStatusNoThrow(result, e.toStatus());
            }
        });

        if (!args.usersCollName.empty()) {
            Status status =
                processUsers(opCtx, authzManager, args.usersCollName, args.db, args.drop);
            uassertStatusOK(status);
        }

        if (!args.rolesCollName.empty()) {
            Status status =
                processRoles(opCtx, authzManager, args.rolesCollName, args.db, args.drop);
            uassertStatusOK(status);
        }

        return true;
    }

} cmdMergeAuthzCollections;

}  // namespace
}  // namespace mongo
