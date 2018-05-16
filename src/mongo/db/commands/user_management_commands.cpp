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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kAccessControl

#include "mongo/platform/basic.h"

#include "mongo/db/commands/user_management_commands.h"

#include <string>
#include <vector>

#include "mongo/base/status.h"
#include "mongo/bson/mutable/algorithm.h"
#include "mongo/bson/mutable/document.h"
#include "mongo/bson/mutable/element.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/client/dbclientinterface.h"
#include "mongo/config.h"
#include "mongo/crypto/mechanism_scram.h"
#include "mongo/db/audit.h"
#include "mongo/db/auth/action_set.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/address_restriction.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/authorization_manager_global.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/auth/privilege_parser.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/auth/sasl_options.h"
#include "mongo/db/auth/user.h"
#include "mongo/db/auth/user_document_parser.h"
#include "mongo/db/auth/user_management_commands_parser.h"
#include "mongo/db/client.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/run_aggregate.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/ops/write_ops.h"
#include "mongo/db/query/cursor_response.h"
#include "mongo/db/service_context.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/s/write_ops/batched_command_response.h"
#include "mongo/stdx/functional.h"
#include "mongo/stdx/mutex.h"
#include "mongo/stdx/unordered_set.h"
#include "mongo/util/icu.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/net/ssl_manager.h"
#include "mongo/util/password_digest.h"
#include "mongo/util/sequence_util.h"
#include "mongo/util/time_support.h"

namespace mongo {

using std::endl;
using std::string;
using std::stringstream;
using std::vector;

namespace {

// Used to obtain mutex that guards modifications to persistent authorization data
const auto getAuthzDataMutex = ServiceContext::declareDecoration<stdx::mutex>();

Status useDefaultCode(const Status& status, ErrorCodes::Error defaultCode) {
    if (status.code() != ErrorCodes::UnknownError)
        return status;
    return Status(defaultCode, status.reason());
}

BSONArray roleSetToBSONArray(const stdx::unordered_set<RoleName>& roles) {
    BSONArrayBuilder rolesArrayBuilder;
    for (stdx::unordered_set<RoleName>::const_iterator it = roles.begin(); it != roles.end();
         ++it) {
        const RoleName& role = *it;
        rolesArrayBuilder.append(BSON(AuthorizationManager::ROLE_NAME_FIELD_NAME
                                      << role.getRole()
                                      << AuthorizationManager::ROLE_DB_FIELD_NAME
                                      << role.getDB()));
    }
    return rolesArrayBuilder.arr();
}

BSONArray rolesVectorToBSONArray(const std::vector<RoleName>& roles) {
    BSONArrayBuilder rolesArrayBuilder;
    for (std::vector<RoleName>::const_iterator it = roles.begin(); it != roles.end(); ++it) {
        const RoleName& role = *it;
        rolesArrayBuilder.append(BSON(AuthorizationManager::ROLE_NAME_FIELD_NAME
                                      << role.getRole()
                                      << AuthorizationManager::ROLE_DB_FIELD_NAME
                                      << role.getDB()));
    }
    return rolesArrayBuilder.arr();
}

Status privilegeVectorToBSONArray(const PrivilegeVector& privileges, BSONArray* result) {
    BSONArrayBuilder arrBuilder;
    for (PrivilegeVector::const_iterator it = privileges.begin(); it != privileges.end(); ++it) {
        const Privilege& privilege = *it;

        ParsedPrivilege parsedPrivilege;
        std::string errmsg;
        if (!ParsedPrivilege::privilegeToParsedPrivilege(privilege, &parsedPrivilege, &errmsg)) {
            return Status(ErrorCodes::FailedToParse, errmsg);
        }
        if (!parsedPrivilege.isValid(&errmsg)) {
            return Status(ErrorCodes::FailedToParse, errmsg);
        }
        arrBuilder.append(parsedPrivilege.toBSON());
    }
    *result = arrBuilder.arr();
    return Status::OK();
}

/**
 * Used to get all current roles of the user identified by 'userName'.
 */
Status getCurrentUserRoles(OperationContext* opCtx,
                           AuthorizationManager* authzManager,
                           const UserName& userName,
                           stdx::unordered_set<RoleName>* roles) {
    User* user;
    authzManager->invalidateUserByName(userName);  // Need to make sure cache entry is up to date
    Status status = authzManager->acquireUser(opCtx, userName, &user);
    if (!status.isOK()) {
        return status;
    }
    RoleNameIterator rolesIt = user->getRoles();
    while (rolesIt.more()) {
        roles->insert(rolesIt.next());
    }
    authzManager->releaseUser(user);
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
                                   const std::vector<RoleName> rolesToAdd,
                                   AuthorizationManager* authzManager) {
    for (std::vector<RoleName>::const_iterator it = rolesToAdd.begin(); it != rolesToAdd.end();
         ++it) {
        const RoleName& roleToAdd = *it;
        if (roleToAdd == role) {
            return Status(ErrorCodes::InvalidRoleModification,
                          mongoutils::str::stream() << "Cannot grant role " << role.getFullName()
                                                    << " to itself.");
        }

        if (role.getDB() != "admin" && roleToAdd.getDB() != role.getDB()) {
            return Status(
                ErrorCodes::InvalidRoleModification,
                str::stream() << "Roles on the \'" << role.getDB()
                              << "\' database cannot be granted roles from other databases");
        }

        BSONObj roleToAddDoc;
        Status status = authzManager->getRoleDescription(opCtx, roleToAdd, &roleToAddDoc);
        if (status == ErrorCodes::RoleNotFound) {
            return Status(ErrorCodes::RoleNotFound,
                          "Cannot grant nonexistent role " + roleToAdd.toString());
        }
        if (!status.isOK()) {
            return status;
        }
        std::vector<RoleName> indirectRoles;
        status = auth::parseRoleNamesFromBSONArray(
            BSONArray(roleToAddDoc["inheritedRoles"].Obj()), role.getDB(), &indirectRoles);
        if (!status.isOK()) {
            return status;
        }

        if (sequenceContains(indirectRoles, role)) {
            return Status(
                ErrorCodes::InvalidRoleModification,
                mongoutils::str::stream() << "Granting " << roleToAdd.getFullName() << " to "
                                          << role.getFullName()
                                          << " would introduce a cycle in the role graph.");
        }
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
                          const stdx::function<void(const BSONObj&)>& resultProcessor) {
    try {
        DBDirectClient client(opCtx);
        client.query(resultProcessor, collectionName.ns(), query, &projection);
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
                            long long* nMatched) {
    try {
        DBDirectClient client(opCtx);

        BSONObj res;
        client.runCommand(collectionName.db().toString(),
                          [&] {
                              write_ops::Update updateOp(collectionName);
                              updateOp.setUpdates({[&] {
                                  write_ops::UpdateOpEntry entry;
                                  entry.setQ(query);
                                  entry.setU(updatePattern);
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
            *nMatched = response.getN();
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
    long long nMatched;
    Status status =
        updateAuthzDocuments(opCtx, collectionName, query, updatePattern, upsert, false, &nMatched);
    if (!status.isOK()) {
        return status;
    }
    dassert(nMatched == 1 || nMatched == 0);
    if (nMatched == 0) {
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
                            long long* numRemoved) {
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
        return Status(ErrorCodes::DuplicateKey,
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
    Status status = updateOneAuthzDocument(opCtx,
                                           AuthorizationManager::rolesCollectionNamespace,
                                           BSON(AuthorizationManager::ROLE_NAME_FIELD_NAME
                                                << role.getRole()
                                                << AuthorizationManager::ROLE_DB_FIELD_NAME
                                                << role.getDB()),
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
Status removeRoleDocuments(OperationContext* opCtx, const BSONObj& query, long long* numRemoved) {
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
        return Status(ErrorCodes::DuplicateKey,
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
    const auto status = updatePrivilegeDocument(opCtx,
                                                user,
                                                BSON(AuthorizationManager::USER_NAME_FIELD_NAME
                                                     << user.getUser()
                                                     << AuthorizationManager::USER_DB_FIELD_NAME
                                                     << user.getDB()),
                                                updateObj);
    return status;
}

/**
 * Removes users for the given database matching the given query.
 * Writes into *numRemoved the number of user documents that were modified.
 */
Status removePrivilegeDocuments(OperationContext* opCtx,
                                const BSONObj& query,
                                long long* numRemoved) {
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

/**
 * Returns Status::OK() if the current Auth schema version is at least the auth schema version
 * for the MongoDB 3.0 SCRAM auth mode.
 * Returns an error otherwise.
 */
Status requireWritableAuthSchema28SCRAM(OperationContext* opCtx,
                                        AuthorizationManager* authzManager) {
    int foundSchemaVersion;
    Status status = authzManager->getAuthorizationVersion(opCtx, &foundSchemaVersion);
    if (!status.isOK()) {
        return status;
    }

    if (foundSchemaVersion < AuthorizationManager::schemaVersion28SCRAM) {
        return Status(ErrorCodes::AuthSchemaIncompatible,
                      str::stream()
                          << "User and role management commands require auth data to have "
                          << "at least schema version "
                          << AuthorizationManager::schemaVersion28SCRAM
                          << " but found "
                          << foundSchemaVersion);
    }
    return writeAuthSchemaVersionIfNeeded(opCtx, authzManager, foundSchemaVersion);
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
Status requireReadableAuthSchema26Upgrade(OperationContext* opCtx,
                                          AuthorizationManager* authzManager) {
    int foundSchemaVersion;
    Status status = authzManager->getAuthorizationVersion(opCtx, &foundSchemaVersion);
    if (!status.isOK()) {
        return status;
    }

    if (foundSchemaVersion < AuthorizationManager::schemaVersion26Upgrade) {
        return Status(ErrorCodes::AuthSchemaIncompatible,
                      str::stream() << "The usersInfo and rolesInfo commands require auth data to "
                                    << "have at least schema version "
                                    << AuthorizationManager::schemaVersion26Upgrade
                                    << " but found "
                                    << foundSchemaVersion);
    }
    return Status::OK();
}

Status buildCredentials(BSONObjBuilder* builder, const auth::CreateOrUpdateUserArgs& args) {
    if (!args.hasPassword) {
        // Must be external user.
        builder->append("external", true);
        return Status::OK();
    }

    bool buildSCRAMSHA1 = false, buildSCRAMSHA256 = false;
    if (args.mechanisms.empty()) {
        buildSCRAMSHA1 = sequenceContains(saslGlobalParams.authenticationMechanisms, "SCRAM-SHA-1");
        buildSCRAMSHA256 =
            sequenceContains(saslGlobalParams.authenticationMechanisms, "SCRAM-SHA-256");
    } else {
        for (const auto& mech : args.mechanisms) {
            if (mech == "SCRAM-SHA-1") {
                buildSCRAMSHA1 = true;
            } else if (mech == "SCRAM-SHA-256") {
                buildSCRAMSHA256 = true;
            } else {
                return {ErrorCodes::BadValue,
                        str::stream() << "Unknown auth mechanism '" << mech << "'"};
            }

            if (!sequenceContains(saslGlobalParams.authenticationMechanisms, mech)) {
                return {ErrorCodes::BadValue,
                        str::stream() << mech << " not supported in authMechanisms"};
            }
        }
    }

    if (buildSCRAMSHA1) {
        // Add SCRAM-SHA-1 credentials.
        std::string hashedPwd;
        if (args.digestPassword) {
            hashedPwd = createPasswordDigest(args.userName.getUser(), args.password);
        } else {
            hashedPwd = args.password;
        }
        auto sha1Cred = scram::Secrets<SHA1Block>::generateCredentials(
            hashedPwd, saslGlobalParams.scramSHA1IterationCount.load());
        builder->append("SCRAM-SHA-1", sha1Cred);
    }

    if (buildSCRAMSHA256) {
        // FCV check is deferred till this point so that the suitability checks can be performed
        // regardless.
        const auto fcv = serverGlobalParams.featureCompatibility.getVersion();
        if (fcv < ServerGlobalParams::FeatureCompatibility::Version::kFullyUpgradedTo40) {
            buildSCRAMSHA256 = false;
        }
    }

    if (buildSCRAMSHA256) {
        if (!args.digestPassword) {
            return {ErrorCodes::BadValue, "Use of SCRAM-SHA-256 requires undigested passwords"};
        }
        const auto swPwd = saslPrep(args.password);
        if (!swPwd.isOK()) {
            return swPwd.getStatus();
        }
        auto sha256Cred = scram::Secrets<SHA256Block>::generateCredentials(
            swPwd.getValue(), saslGlobalParams.scramSHA256IterationCount.load());
        builder->append("SCRAM-SHA-256", sha256Cred);
    }

    return Status::OK();
}

Status trimCredentials(OperationContext* opCtx,
                       BSONObjBuilder* queryBuilder,
                       BSONObjBuilder* unsetBuilder,
                       const auth::CreateOrUpdateUserArgs& args) {
    auto* authzManager = AuthorizationManager::get(opCtx->getServiceContext());
    BSONObj userObj;
    const auto status = authzManager->getUserDescription(opCtx, args.userName, &userObj);
    if (!status.isOK()) {
        return status;
    }

    const auto& credsElem = userObj["credentials"];
    if (credsElem.eoo() || (credsElem.type() != Object)) {
        return {ErrorCodes::UnsupportedFormat,
                "Unable to trim credentials from a user document with no credentials"};
    }

    const auto& creds = credsElem.Obj();
    queryBuilder->append("credentials", creds);

    bool keepSCRAMSHA1 = false, keepSCRAMSHA256 = false;
    for (const auto& mech : args.mechanisms) {
        if (!creds.hasField(mech)) {
            return {ErrorCodes::BadValue,
                    "mechanisms field must be a subset of previously set mechanisms"};
        }
        if (mech == "SCRAM-SHA-1") {
            keepSCRAMSHA1 = true;
        } else if (mech == "SCRAM-SHA-256") {
            keepSCRAMSHA256 = true;
        }
    }
    if (!(keepSCRAMSHA1 || keepSCRAMSHA256)) {
        return {ErrorCodes::BadValue,
                "mechanisms field must contain at least one previously set known mechanism"};
    }

    if (!keepSCRAMSHA1) {
        unsetBuilder->append("credentials.SCRAM-SHA-1", "");
    }
    if (!keepSCRAMSHA256) {
        unsetBuilder->append("credentials.SCRAM-SHA-256", "");
    }

    return Status::OK();
}

}  // namespace


class CmdCreateUser : public BasicCommand {
public:
    CmdCreateUser() : BasicCommand("createUser") {}

    bool isUserManagementCommand() const override {
        return true;
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }

    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return true;
    }

    std::string help() const override {
        return "Adds a user to the system";
    }

    virtual Status checkAuthForCommand(Client* client,
                                       const std::string& dbname,
                                       const BSONObj& cmdObj) const {
        return auth::checkAuthForCreateUserCommand(client, dbname, cmdObj);
    }

    bool run(OperationContext* opCtx,
             const string& dbname,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) {
        auth::CreateOrUpdateUserArgs args;
        Status status = auth::parseCreateOrUpdateUserCommands(cmdObj, "createUser", dbname, &args);
        uassertStatusOK(status);

        if (args.userName.getDB() == "local") {
            uasserted(ErrorCodes::BadValue, "Cannot create users in the local database");
        }

        if (!args.hasPassword && args.userName.getDB() != "$external") {
            uasserted(ErrorCodes::BadValue,
                      "Must provide a 'pwd' field for all user documents, except those"
                      " with '$external' as the user's source db");
        }

        if ((args.hasPassword) && args.userName.getDB() == "$external") {
            uasserted(ErrorCodes::BadValue,
                      "Cannot set the password for users defined on the '$external' "
                      "database");
        }

        if (!args.hasRoles) {
            uasserted(ErrorCodes::BadValue, "\"createUser\" command requires a \"roles\" array");
        }

#ifdef MONGO_CONFIG_SSL
        if (args.userName.getDB() == "$external" && getSSLManager() &&
            getSSLManager()->getSSLConfiguration().isClusterMember(args.userName.getUser())) {
            uasserted(ErrorCodes::BadValue,
                      "Cannot create an x.509 user with a subjectname "
                      "that would be recognized as an internal "
                      "cluster member.");
        }
#endif

        BSONObjBuilder userObjBuilder;
        userObjBuilder.append(
            "_id", str::stream() << args.userName.getDB() << "." << args.userName.getUser());
        userObjBuilder.append(AuthorizationManager::USER_NAME_FIELD_NAME, args.userName.getUser());
        userObjBuilder.append(AuthorizationManager::USER_DB_FIELD_NAME, args.userName.getDB());

        ServiceContext* serviceContext = opCtx->getClient()->getServiceContext();
        AuthorizationManager* authzManager = AuthorizationManager::get(serviceContext);
        int authzVersion;
        status = authzManager->getAuthorizationVersion(opCtx, &authzVersion);
        uassertStatusOK(status);

        BSONObjBuilder credentialsBuilder(userObjBuilder.subobjStart("credentials"));
        status = buildCredentials(&credentialsBuilder, args);
        uassertStatusOK(status);
        credentialsBuilder.done();

        if (args.authenticationRestrictions && !args.authenticationRestrictions->isEmpty()) {
            credentialsBuilder.append("authenticationRestrictions",
                                      *args.authenticationRestrictions);
        }

        if (args.hasCustomData) {
            userObjBuilder.append("customData", args.customData);
        }

        userObjBuilder.append("roles", rolesVectorToBSONArray(args.roles));

        BSONObj userObj = userObjBuilder.obj();
        V2UserDocumentParser parser;
        status = parser.checkValidUserDocument(userObj);
        uassertStatusOK(status);

        stdx::lock_guard<stdx::mutex> lk(getAuthzDataMutex(serviceContext));

        status = requireWritableAuthSchema28SCRAM(opCtx, authzManager);
        uassertStatusOK(status);

        // Role existence has to be checked after acquiring the update lock
        for (size_t i = 0; i < args.roles.size(); ++i) {
            BSONObj ignored;
            status = authzManager->getRoleDescription(opCtx, args.roles[i], &ignored);
            uassertStatusOK(status);
        }

        audit::logCreateUser(Client::getCurrent(),
                             args.userName,
                             args.hasPassword,
                             args.hasCustomData ? &args.customData : NULL,
                             args.roles,
                             args.authenticationRestrictions);
        status = insertPrivilegeDocument(opCtx, userObj);
        uassertStatusOK(status);
        return true;
    }

    void redactForLogging(mutablebson::Document* cmdObj) const override {
        auth::redactPasswordData(cmdObj->root());
    }

} cmdCreateUser;

class CmdUpdateUser : public BasicCommand {
public:
    CmdUpdateUser() : BasicCommand("updateUser") {}

    bool isUserManagementCommand() const override {
        return true;
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }

    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return true;
    }

    std::string help() const override {
        return "Used to update a user, for example to change its password";
    }

    virtual Status checkAuthForCommand(Client* client,
                                       const std::string& dbname,
                                       const BSONObj& cmdObj) const {
        return auth::checkAuthForUpdateUserCommand(client, dbname, cmdObj);
    }

    bool run(OperationContext* opCtx,
             const string& dbname,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) {
        auth::CreateOrUpdateUserArgs args;
        Status status = auth::parseCreateOrUpdateUserCommands(cmdObj, "updateUser", dbname, &args);
        uassertStatusOK(status);

        if (!args.hasPassword && !args.hasCustomData && !args.hasRoles &&
            !args.authenticationRestrictions && args.mechanisms.empty()) {
            uasserted(ErrorCodes::BadValue,
                      "Must specify at least one field to update in updateUser");
        }

        if (args.hasPassword && args.userName.getDB() == "$external") {
            uasserted(ErrorCodes::BadValue,
                      "Cannot set the password for users defined on the '$external' "
                      "database");
        }

        BSONObjBuilder queryBuilder;
        queryBuilder.append(AuthorizationManager::USER_NAME_FIELD_NAME, args.userName.getUser());
        queryBuilder.append(AuthorizationManager::USER_DB_FIELD_NAME, args.userName.getDB());

        BSONObjBuilder updateSetBuilder;
        BSONObjBuilder updateUnsetBuilder;
        if (args.hasPassword) {
            BSONObjBuilder credentialsBuilder(updateSetBuilder.subobjStart("credentials"));
            status = buildCredentials(&credentialsBuilder, args);
            uassertStatusOK(status);
            credentialsBuilder.done();
        } else if (!args.mechanisms.empty()) {
            status = trimCredentials(opCtx, &queryBuilder, &updateUnsetBuilder, args);
            uassertStatusOK(status);
        }

        if (args.hasCustomData) {
            updateSetBuilder.append("customData", args.customData);
        }

        if (args.authenticationRestrictions) {
            if (args.authenticationRestrictions->isEmpty()) {
                updateUnsetBuilder.append("authenticationRestrictions", "");
            } else {
                auto swParsedRestrictions =
                    parseAuthenticationRestriction(*args.authenticationRestrictions);
                uassertStatusOK(swParsedRestrictions.getStatus());

                updateSetBuilder.append("authenticationRestrictions",
                                        *args.authenticationRestrictions);
            }
        }

        if (args.hasRoles) {
            updateSetBuilder.append("roles", rolesVectorToBSONArray(args.roles));
        }

        BSONObj updateSet = updateSetBuilder.done();
        BSONObj updateUnset = updateUnsetBuilder.done();
        BSONObjBuilder updateDocumentBuilder;
        if (!updateSet.isEmpty()) {
            updateDocumentBuilder << "$set" << updateSet;
        }
        if (!updateUnset.isEmpty()) {
            updateDocumentBuilder << "$unset" << updateUnset;
        }

        ServiceContext* serviceContext = opCtx->getClient()->getServiceContext();
        stdx::lock_guard<stdx::mutex> lk(getAuthzDataMutex(serviceContext));

        AuthorizationManager* authzManager = AuthorizationManager::get(serviceContext);
        status = requireWritableAuthSchema28SCRAM(opCtx, authzManager);
        uassertStatusOK(status);

        // Role existence has to be checked after acquiring the update lock
        if (args.hasRoles) {
            for (size_t i = 0; i < args.roles.size(); ++i) {
                BSONObj ignored;
                status = authzManager->getRoleDescription(opCtx, args.roles[i], &ignored);
                uassertStatusOK(status);
            }
        }

        audit::logUpdateUser(Client::getCurrent(),
                             args.userName,
                             args.hasPassword,
                             args.hasCustomData ? &args.customData : NULL,
                             args.hasRoles ? &args.roles : NULL,
                             args.authenticationRestrictions);

        status = updatePrivilegeDocument(
            opCtx, args.userName, queryBuilder.done(), updateDocumentBuilder.done());
        // Must invalidate even on bad status - what if the write succeeded but the GLE failed?
        authzManager->invalidateUserByName(args.userName);
        uassertStatusOK(status);
        return true;
    }

    void redactForLogging(mutablebson::Document* cmdObj) const override {
        auth::redactPasswordData(cmdObj->root());
    }

} cmdUpdateUser;

class CmdDropUser : public BasicCommand {
public:
    CmdDropUser() : BasicCommand("dropUser") {}

    bool isUserManagementCommand() const override {
        return true;
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }

    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return true;
    }

    std::string help() const override {
        return "Drops a single user.";
    }

    virtual Status checkAuthForCommand(Client* client,
                                       const std::string& dbname,
                                       const BSONObj& cmdObj) const {
        return auth::checkAuthForDropUserCommand(client, dbname, cmdObj);
    }

    bool run(OperationContext* opCtx,
             const string& dbname,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) {
        UserName userName;
        Status status = auth::parseAndValidateDropUserCommand(cmdObj, dbname, &userName);
        uassertStatusOK(status);

        ServiceContext* serviceContext = opCtx->getClient()->getServiceContext();
        stdx::lock_guard<stdx::mutex> lk(getAuthzDataMutex(serviceContext));
        AuthorizationManager* authzManager = AuthorizationManager::get(serviceContext);
        status = requireWritableAuthSchema28SCRAM(opCtx, authzManager);
        uassertStatusOK(status);

        audit::logDropUser(Client::getCurrent(), userName);

        long long nMatched;
        status = removePrivilegeDocuments(opCtx,
                                          BSON(AuthorizationManager::USER_NAME_FIELD_NAME
                                               << userName.getUser()
                                               << AuthorizationManager::USER_DB_FIELD_NAME
                                               << userName.getDB()),
                                          &nMatched);
        // Must invalidate even on bad status - what if the write succeeded but the GLE failed?
        authzManager->invalidateUserByName(userName);
        uassertStatusOK(status);

        if (nMatched == 0) {
            uasserted(ErrorCodes::UserNotFound,
                      str::stream() << "User '" << userName.getFullName() << "' not found");
        }

        return true;
    }

} cmdDropUser;

class CmdDropAllUsersFromDatabase : public BasicCommand {
public:
    CmdDropAllUsersFromDatabase() : BasicCommand("dropAllUsersFromDatabase") {}

    bool isUserManagementCommand() const override {
        return true;
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }

    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return true;
    }

    std::string help() const override {
        return "Drops all users for a single database.";
    }

    virtual Status checkAuthForCommand(Client* client,
                                       const std::string& dbname,
                                       const BSONObj& cmdObj) const {
        return auth::checkAuthForDropAllUsersFromDatabaseCommand(client, dbname);
    }

    bool run(OperationContext* opCtx,
             const string& dbname,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) {
        Status status = auth::parseAndValidateDropAllUsersFromDatabaseCommand(cmdObj, dbname);
        uassertStatusOK(status);
        ServiceContext* serviceContext = opCtx->getClient()->getServiceContext();
        stdx::lock_guard<stdx::mutex> lk(getAuthzDataMutex(serviceContext));

        AuthorizationManager* authzManager = AuthorizationManager::get(serviceContext);
        status = requireWritableAuthSchema28SCRAM(opCtx, authzManager);
        uassertStatusOK(status);

        audit::logDropAllUsersFromDatabase(Client::getCurrent(), dbname);

        long long numRemoved;
        status = removePrivilegeDocuments(
            opCtx, BSON(AuthorizationManager::USER_DB_FIELD_NAME << dbname), &numRemoved);
        // Must invalidate even on bad status - what if the write succeeded but the GLE failed?
        authzManager->invalidateUsersFromDB(dbname);
        uassertStatusOK(status);

        result.append("n", numRemoved);
        return true;
    }

} cmdDropAllUsersFromDatabase;

class CmdGrantRolesToUser : public BasicCommand {
public:
    CmdGrantRolesToUser() : BasicCommand("grantRolesToUser") {}

    bool isUserManagementCommand() const override {
        return true;
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }

    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return true;
    }

    std::string help() const override {
        return "Grants roles to a user.";
    }

    virtual Status checkAuthForCommand(Client* client,
                                       const std::string& dbname,
                                       const BSONObj& cmdObj) const {
        return auth::checkAuthForGrantRolesToUserCommand(client, dbname, cmdObj);
    }

    bool run(OperationContext* opCtx,
             const string& dbname,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) {
        std::string userNameString;
        std::vector<RoleName> roles;
        Status status = auth::parseRolePossessionManipulationCommands(
            cmdObj, "grantRolesToUser", dbname, &userNameString, &roles);
        uassertStatusOK(status);

        ServiceContext* serviceContext = opCtx->getClient()->getServiceContext();
        stdx::lock_guard<stdx::mutex> lk(getAuthzDataMutex(serviceContext));

        AuthorizationManager* authzManager = AuthorizationManager::get(serviceContext);
        status = requireWritableAuthSchema28SCRAM(opCtx, authzManager);
        uassertStatusOK(status);

        UserName userName(userNameString, dbname);
        stdx::unordered_set<RoleName> userRoles;
        status = getCurrentUserRoles(opCtx, authzManager, userName, &userRoles);
        uassertStatusOK(status);

        for (vector<RoleName>::iterator it = roles.begin(); it != roles.end(); ++it) {
            RoleName& roleName = *it;
            BSONObj roleDoc;
            status = authzManager->getRoleDescription(opCtx, roleName, &roleDoc);
            uassertStatusOK(status);

            userRoles.insert(roleName);
        }

        audit::logGrantRolesToUser(Client::getCurrent(), userName, roles);
        BSONArray newRolesBSONArray = roleSetToBSONArray(userRoles);
        status = updatePrivilegeDocument(
            opCtx, userName, BSON("$set" << BSON("roles" << newRolesBSONArray)));
        // Must invalidate even on bad status - what if the write succeeded but the GLE failed?
        authzManager->invalidateUserByName(userName);
        uassertStatusOK(status);
        return true;
    }

} cmdGrantRolesToUser;

class CmdRevokeRolesFromUser : public BasicCommand {
public:
    CmdRevokeRolesFromUser() : BasicCommand("revokeRolesFromUser") {}

    bool isUserManagementCommand() const override {
        return true;
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }

    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return true;
    }

    std::string help() const override {
        return "Revokes roles from a user.";
    }

    virtual Status checkAuthForCommand(Client* client,
                                       const std::string& dbname,
                                       const BSONObj& cmdObj) const {
        return auth::checkAuthForRevokeRolesFromUserCommand(client, dbname, cmdObj);
    }

    bool run(OperationContext* opCtx,
             const string& dbname,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) {
        std::string userNameString;
        std::vector<RoleName> roles;
        Status status = auth::parseRolePossessionManipulationCommands(
            cmdObj, "revokeRolesFromUser", dbname, &userNameString, &roles);
        uassertStatusOK(status);

        ServiceContext* serviceContext = opCtx->getClient()->getServiceContext();
        stdx::lock_guard<stdx::mutex> lk(getAuthzDataMutex(serviceContext));

        AuthorizationManager* authzManager = AuthorizationManager::get(serviceContext);
        status = requireWritableAuthSchema28SCRAM(opCtx, authzManager);
        uassertStatusOK(status);

        UserName userName(userNameString, dbname);
        stdx::unordered_set<RoleName> userRoles;
        status = getCurrentUserRoles(opCtx, authzManager, userName, &userRoles);
        uassertStatusOK(status);

        for (vector<RoleName>::iterator it = roles.begin(); it != roles.end(); ++it) {
            RoleName& roleName = *it;
            BSONObj roleDoc;
            status = authzManager->getRoleDescription(opCtx, roleName, &roleDoc);
            uassertStatusOK(status);

            userRoles.erase(roleName);
        }

        audit::logRevokeRolesFromUser(Client::getCurrent(), userName, roles);
        BSONArray newRolesBSONArray = roleSetToBSONArray(userRoles);
        status = updatePrivilegeDocument(
            opCtx, userName, BSON("$set" << BSON("roles" << newRolesBSONArray)));
        // Must invalidate even on bad status - what if the write succeeded but the GLE failed?
        authzManager->invalidateUserByName(userName);
        uassertStatusOK(status);
        return true;
    }

} cmdRevokeRolesFromUser;

class CmdUsersInfo : public BasicCommand {
public:
    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kOptIn;
    }

    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }

    CmdUsersInfo() : BasicCommand("usersInfo") {}

    std::string help() const override {
        return "Returns information about users.";
    }

    virtual Status checkAuthForCommand(Client* client,
                                       const std::string& dbname,
                                       const BSONObj& cmdObj) const {
        return auth::checkAuthForUsersInfoCommand(client, dbname, cmdObj);
    }

    bool run(OperationContext* opCtx,
             const string& dbname,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) {
        auth::UsersInfoArgs args;
        Status status = auth::parseUsersInfoCommand(cmdObj, dbname, &args);
        uassertStatusOK(status);

        status = requireReadableAuthSchema26Upgrade(opCtx, getGlobalAuthorizationManager());
        uassertStatusOK(status);

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
                status = getGlobalAuthorizationManager()->getUserDescription(
                    opCtx, args.userNames[i], &userDetails);
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

            BSONObjBuilder responseBuilder;
            AggregationRequest aggRequest(AuthorizationManager::usersCollectionNamespace,
                                          std::move(pipeline));
            Status status = runAggregate(opCtx,
                                         AuthorizationManager::usersCollectionNamespace,
                                         aggRequest,
                                         aggRequest.serializeToCommandObj().toBson(),
                                         responseBuilder);
            uassertStatusOK(status);

            CommandHelpers::appendSimpleCommandStatus(responseBuilder, true);
            auto swResponse = CursorResponse::parseFromBSON(responseBuilder.obj());
            uassertStatusOK(swResponse.getStatus());
            for (const BSONObj& obj : swResponse.getValue().getBatch()) {
                usersArrayBuilder.append(obj);
            }
        }
        result.append("users", usersArrayBuilder.arr());
        return true;
    }

} cmdUsersInfo;

class CmdCreateRole : public BasicCommand {
public:
    CmdCreateRole() : BasicCommand("createRole") {}

    bool isUserManagementCommand() const override {
        return true;
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }

    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return true;
    }

    std::string help() const override {
        return "Adds a role to the system";
    }

    virtual Status checkAuthForCommand(Client* client,
                                       const std::string& dbname,
                                       const BSONObj& cmdObj) const {
        return auth::checkAuthForCreateRoleCommand(client, dbname, cmdObj);
    }

    bool run(OperationContext* opCtx,
             const string& dbname,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) {
        auth::CreateOrUpdateRoleArgs args;
        Status status = auth::parseCreateOrUpdateRoleCommands(cmdObj, "createRole", dbname, &args);
        uassertStatusOK(status);

        if (args.roleName.getRole().empty()) {
            uasserted(ErrorCodes::BadValue, "Role name must be non-empty");
        }

        if (args.roleName.getDB() == "local") {
            uasserted(ErrorCodes::BadValue, "Cannot create roles in the local database");
        }

        if (args.roleName.getDB() == "$external") {
            uasserted(ErrorCodes::BadValue, "Cannot create roles in the $external database");
        }

        if (RoleGraph::isBuiltinRole(args.roleName)) {
            uasserted(ErrorCodes::BadValue,
                      "Cannot create roles with the same name as a built-in role");
        }

        if (!args.hasRoles) {
            uasserted(ErrorCodes::BadValue, "\"createRole\" command requires a \"roles\" array");
        }

        if (!args.hasPrivileges) {
            uasserted(ErrorCodes::BadValue,
                      "\"createRole\" command requires a \"privileges\" array");
        }

        BSONObjBuilder roleObjBuilder;

        roleObjBuilder.append(
            "_id", str::stream() << args.roleName.getDB() << "." << args.roleName.getRole());
        roleObjBuilder.append(AuthorizationManager::ROLE_NAME_FIELD_NAME, args.roleName.getRole());
        roleObjBuilder.append(AuthorizationManager::ROLE_DB_FIELD_NAME, args.roleName.getDB());

        BSONArray privileges;
        status = privilegeVectorToBSONArray(args.privileges, &privileges);
        uassertStatusOK(status);
        roleObjBuilder.append("privileges", privileges);

        roleObjBuilder.append("roles", rolesVectorToBSONArray(args.roles));

        if (args.authenticationRestrictions && !args.authenticationRestrictions->isEmpty()) {
            roleObjBuilder.append("authenticationRestrictions",
                                  args.authenticationRestrictions.get());
        }

        ServiceContext* serviceContext = opCtx->getClient()->getServiceContext();
        stdx::lock_guard<stdx::mutex> lk(getAuthzDataMutex(serviceContext));

        AuthorizationManager* authzManager = AuthorizationManager::get(serviceContext);
        status = requireWritableAuthSchema28SCRAM(opCtx, authzManager);
        uassertStatusOK(status);

        // Role existence has to be checked after acquiring the update lock
        status = checkOkayToGrantRolesToRole(opCtx, args.roleName, args.roles, authzManager);
        uassertStatusOK(status);

        status = checkOkayToGrantPrivilegesToRole(args.roleName, args.privileges);
        uassertStatusOK(status);

        audit::logCreateRole(Client::getCurrent(),
                             args.roleName,
                             args.roles,
                             args.privileges,
                             args.authenticationRestrictions);

        status = insertRoleDocument(opCtx, roleObjBuilder.done());
        uassertStatusOK(status);
        return true;
    }

} cmdCreateRole;

class CmdUpdateRole : public BasicCommand {
public:
    CmdUpdateRole() : BasicCommand("updateRole") {}

    bool isUserManagementCommand() const override {
        return true;
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }

    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return true;
    }

    std::string help() const override {
        return "Used to update a role";
    }

    virtual Status checkAuthForCommand(Client* client,
                                       const std::string& dbname,
                                       const BSONObj& cmdObj) const {
        return auth::checkAuthForUpdateRoleCommand(client, dbname, cmdObj);
    }

    bool run(OperationContext* opCtx,
             const string& dbname,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) {
        auth::CreateOrUpdateRoleArgs args;
        Status status = auth::parseCreateOrUpdateRoleCommands(cmdObj, "updateRole", dbname, &args);
        uassertStatusOK(status);

        if (!args.hasPrivileges && !args.hasRoles && !args.authenticationRestrictions) {
            uasserted(ErrorCodes::BadValue,
                      "Must specify at least one field to update in updateRole");
        }

        BSONObjBuilder updateSetBuilder;
        BSONObjBuilder updateUnsetBuilder;

        if (args.hasPrivileges) {
            BSONArray privileges;
            status = privilegeVectorToBSONArray(args.privileges, &privileges);
            uassertStatusOK(status);
            updateSetBuilder.append("privileges", privileges);
        }

        if (args.hasRoles) {
            updateSetBuilder.append("roles", rolesVectorToBSONArray(args.roles));
        }

        if (args.authenticationRestrictions) {
            if (args.authenticationRestrictions->isEmpty()) {
                updateUnsetBuilder.append("authenticationRestrictions", "");
            } else {
                updateSetBuilder.append("authenticationRestrictions",
                                        args.authenticationRestrictions.get());
            }
        }

        ServiceContext* serviceContext = opCtx->getClient()->getServiceContext();
        stdx::lock_guard<stdx::mutex> lk(getAuthzDataMutex(serviceContext));

        AuthorizationManager* authzManager = AuthorizationManager::get(serviceContext);
        status = requireWritableAuthSchema28SCRAM(opCtx, authzManager);
        uassertStatusOK(status);

        // Role existence has to be checked after acquiring the update lock
        BSONObj ignored;
        status = authzManager->getRoleDescription(opCtx, args.roleName, &ignored);
        uassertStatusOK(status);

        if (args.hasRoles) {
            status = checkOkayToGrantRolesToRole(opCtx, args.roleName, args.roles, authzManager);
            uassertStatusOK(status);
        }

        if (args.hasPrivileges) {
            status = checkOkayToGrantPrivilegesToRole(args.roleName, args.privileges);
            uassertStatusOK(status);
        }

        audit::logUpdateRole(Client::getCurrent(),
                             args.roleName,
                             args.hasRoles ? &args.roles : nullptr,
                             args.hasPrivileges ? &args.privileges : nullptr,
                             args.authenticationRestrictions);

        const auto updateSet = updateSetBuilder.obj();
        const auto updateUnset = updateUnsetBuilder.obj();
        BSONObjBuilder updateDocumentBuilder;
        if (!updateSet.isEmpty()) {
            updateDocumentBuilder.append("$set", updateSet);
        }
        if (!updateUnset.isEmpty()) {
            updateDocumentBuilder.append("$unset", updateUnset);
        }

        status = updateRoleDocument(opCtx, args.roleName, updateDocumentBuilder.obj());
        // Must invalidate even on bad status - what if the write succeeded but the GLE failed?
        authzManager->invalidateUserCache();
        uassertStatusOK(status);
        return true;
    }
} cmdUpdateRole;

class CmdGrantPrivilegesToRole : public BasicCommand {
public:
    CmdGrantPrivilegesToRole() : BasicCommand("grantPrivilegesToRole") {}

    bool isUserManagementCommand() const override {
        return true;
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }

    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return true;
    }

    std::string help() const override {
        return "Grants privileges to a role";
    }

    virtual Status checkAuthForCommand(Client* client,
                                       const std::string& dbname,
                                       const BSONObj& cmdObj) const {
        return auth::checkAuthForGrantPrivilegesToRoleCommand(client, dbname, cmdObj);
    }

    bool run(OperationContext* opCtx,
             const string& dbname,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) {

        RoleName roleName;
        PrivilegeVector privilegesToAdd;
        Status status = auth::parseAndValidateRolePrivilegeManipulationCommands(
            cmdObj, "grantPrivilegesToRole", dbname, &roleName, &privilegesToAdd);
        uassertStatusOK(status);

        ServiceContext* serviceContext = opCtx->getClient()->getServiceContext();
        stdx::lock_guard<stdx::mutex> lk(getAuthzDataMutex(serviceContext));

        AuthorizationManager* authzManager = AuthorizationManager::get(serviceContext);
        status = requireWritableAuthSchema28SCRAM(opCtx, authzManager);
        uassertStatusOK(status);

        if (RoleGraph::isBuiltinRole(roleName)) {
            uasserted(ErrorCodes::InvalidRoleModification,
                      str::stream() << roleName.getFullName()
                                    << " is a built-in role and cannot be modified.");
        }

        status = checkOkayToGrantPrivilegesToRole(roleName, privilegesToAdd);
        uassertStatusOK(status);

        BSONObj roleDoc;
        status = authzManager->getRoleDescription(opCtx,
                                                  roleName,
                                                  PrivilegeFormat::kShowSeparate,
                                                  AuthenticationRestrictionsFormat::kOmit,
                                                  &roleDoc);
        uassertStatusOK(status);

        PrivilegeVector privileges;
        status = auth::parseAndValidatePrivilegeArray(BSONArray(roleDoc["privileges"].Obj()),
                                                      &privileges);

        uassertStatusOK(status);

        for (PrivilegeVector::iterator it = privilegesToAdd.begin(); it != privilegesToAdd.end();
             ++it) {
            Privilege::addPrivilegeToPrivilegeVector(&privileges, *it);
        }

        // Build up update modifier object to $set privileges.
        mutablebson::Document updateObj;
        mutablebson::Element setElement = updateObj.makeElementObject("$set");
        status = updateObj.root().pushBack(setElement);
        uassertStatusOK(status);
        mutablebson::Element privilegesElement = updateObj.makeElementArray("privileges");
        status = setElement.pushBack(privilegesElement);
        uassertStatusOK(status);
        status = Privilege::getBSONForPrivileges(privileges, privilegesElement);
        uassertStatusOK(status);

        BSONObjBuilder updateBSONBuilder;
        updateObj.writeTo(&updateBSONBuilder);

        audit::logGrantPrivilegesToRole(Client::getCurrent(), roleName, privilegesToAdd);

        status = updateRoleDocument(opCtx, roleName, updateBSONBuilder.done());
        // Must invalidate even on bad status - what if the write succeeded but the GLE failed?
        authzManager->invalidateUserCache();
        uassertStatusOK(status);
        return true;
    }

} cmdGrantPrivilegesToRole;

class CmdRevokePrivilegesFromRole : public BasicCommand {
public:
    CmdRevokePrivilegesFromRole() : BasicCommand("revokePrivilegesFromRole") {}

    bool isUserManagementCommand() const override {
        return true;
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }

    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return true;
    }

    std::string help() const override {
        return "Revokes privileges from a role";
    }

    virtual Status checkAuthForCommand(Client* client,
                                       const std::string& dbname,
                                       const BSONObj& cmdObj) const {
        return auth::checkAuthForRevokePrivilegesFromRoleCommand(client, dbname, cmdObj);
    }

    bool run(OperationContext* opCtx,
             const string& dbname,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) {
        RoleName roleName;
        PrivilegeVector privilegesToRemove;
        Status status = auth::parseAndValidateRolePrivilegeManipulationCommands(
            cmdObj, "revokePrivilegesFromRole", dbname, &roleName, &privilegesToRemove);
        uassertStatusOK(status);

        ServiceContext* serviceContext = opCtx->getClient()->getServiceContext();
        stdx::lock_guard<stdx::mutex> lk(getAuthzDataMutex(serviceContext));

        AuthorizationManager* authzManager = AuthorizationManager::get(serviceContext);
        status = requireWritableAuthSchema28SCRAM(opCtx, authzManager);
        uassertStatusOK(status);

        if (RoleGraph::isBuiltinRole(roleName)) {
            uasserted(ErrorCodes::InvalidRoleModification,
                      str::stream() << roleName.getFullName()
                                    << " is a built-in role and cannot be modified.");
        }

        BSONObj roleDoc;
        status = authzManager->getRoleDescription(opCtx,
                                                  roleName,
                                                  PrivilegeFormat::kShowSeparate,
                                                  AuthenticationRestrictionsFormat::kOmit,
                                                  &roleDoc);
        uassertStatusOK(status);

        PrivilegeVector privileges;
        status = auth::parseAndValidatePrivilegeArray(BSONArray(roleDoc["privileges"].Obj()),
                                                      &privileges);
        uassertStatusOK(status);

        for (PrivilegeVector::iterator itToRm = privilegesToRemove.begin();
             itToRm != privilegesToRemove.end();
             ++itToRm) {
            for (PrivilegeVector::iterator curIt = privileges.begin(); curIt != privileges.end();
                 ++curIt) {
                if (curIt->getResourcePattern() == itToRm->getResourcePattern()) {
                    curIt->removeActions(itToRm->getActions());
                    if (curIt->getActions().empty()) {
                        privileges.erase(curIt);
                    }
                    break;
                }
            }
        }

        // Build up update modifier object to $set privileges.
        mutablebson::Document updateObj;
        mutablebson::Element setElement = updateObj.makeElementObject("$set");
        status = updateObj.root().pushBack(setElement);
        uassertStatusOK(status);
        mutablebson::Element privilegesElement = updateObj.makeElementArray("privileges");
        status = setElement.pushBack(privilegesElement);
        uassertStatusOK(status);
        status = Privilege::getBSONForPrivileges(privileges, privilegesElement);
        uassertStatusOK(status);

        audit::logRevokePrivilegesFromRole(Client::getCurrent(), roleName, privilegesToRemove);

        BSONObjBuilder updateBSONBuilder;
        updateObj.writeTo(&updateBSONBuilder);
        status = updateRoleDocument(opCtx, roleName, updateBSONBuilder.done());
        // Must invalidate even on bad status - what if the write succeeded but the GLE failed?
        authzManager->invalidateUserCache();
        uassertStatusOK(status);
        return true;
    }

} cmdRevokePrivilegesFromRole;

class CmdGrantRolesToRole : public BasicCommand {
public:
    CmdGrantRolesToRole() : BasicCommand("grantRolesToRole") {}

    bool isUserManagementCommand() const override {
        return true;
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }

    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return true;
    }

    std::string help() const override {
        return "Grants roles to another role.";
    }

    virtual Status checkAuthForCommand(Client* client,
                                       const std::string& dbname,
                                       const BSONObj& cmdObj) const {
        return auth::checkAuthForGrantRolesToRoleCommand(client, dbname, cmdObj);
    }

    bool run(OperationContext* opCtx,
             const string& dbname,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) {
        std::string roleNameString;
        std::vector<RoleName> rolesToAdd;
        Status status = auth::parseRolePossessionManipulationCommands(
            cmdObj, "grantRolesToRole", dbname, &roleNameString, &rolesToAdd);
        uassertStatusOK(status);

        RoleName roleName(roleNameString, dbname);
        if (RoleGraph::isBuiltinRole(roleName)) {
            uasserted(ErrorCodes::InvalidRoleModification,
                      str::stream() << roleName.getFullName()
                                    << " is a built-in role and cannot be modified.");
        }

        ServiceContext* serviceContext = opCtx->getClient()->getServiceContext();
        stdx::lock_guard<stdx::mutex> lk(getAuthzDataMutex(serviceContext));

        AuthorizationManager* authzManager = AuthorizationManager::get(serviceContext);
        status = requireWritableAuthSchema28SCRAM(opCtx, authzManager);
        uassertStatusOK(status);

        // Role existence has to be checked after acquiring the update lock
        BSONObj roleDoc;
        status = authzManager->getRoleDescription(opCtx, roleName, &roleDoc);
        uassertStatusOK(status);

        // Check for cycles
        status = checkOkayToGrantRolesToRole(opCtx, roleName, rolesToAdd, authzManager);
        uassertStatusOK(status);

        // Add new roles to existing roles
        std::vector<RoleName> directRoles;
        status = auth::parseRoleNamesFromBSONArray(
            BSONArray(roleDoc["roles"].Obj()), roleName.getDB(), &directRoles);
        uassertStatusOK(status);
        for (vector<RoleName>::iterator it = rolesToAdd.begin(); it != rolesToAdd.end(); ++it) {
            const RoleName& roleToAdd = *it;
            if (!sequenceContains(directRoles, roleToAdd))  // Don't double-add role
                directRoles.push_back(*it);
        }

        audit::logGrantRolesToRole(Client::getCurrent(), roleName, rolesToAdd);

        status = updateRoleDocument(
            opCtx, roleName, BSON("$set" << BSON("roles" << rolesVectorToBSONArray(directRoles))));
        // Must invalidate even on bad status - what if the write succeeded but the GLE failed?
        authzManager->invalidateUserCache();
        uassertStatusOK(status);
        return true;
    }

} cmdGrantRolesToRole;

class CmdRevokeRolesFromRole : public BasicCommand {
public:
    CmdRevokeRolesFromRole() : BasicCommand("revokeRolesFromRole") {}

    bool isUserManagementCommand() const override {
        return true;
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }

    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return true;
    }

    std::string help() const override {
        return "Revokes roles from another role.";
    }

    virtual Status checkAuthForCommand(Client* client,
                                       const std::string& dbname,
                                       const BSONObj& cmdObj) const {
        return auth::checkAuthForRevokeRolesFromRoleCommand(client, dbname, cmdObj);
    }

    bool run(OperationContext* opCtx,
             const string& dbname,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) {
        std::string roleNameString;
        std::vector<RoleName> rolesToRemove;
        Status status = auth::parseRolePossessionManipulationCommands(
            cmdObj, "revokeRolesFromRole", dbname, &roleNameString, &rolesToRemove);
        uassertStatusOK(status);

        ServiceContext* serviceContext = opCtx->getClient()->getServiceContext();
        stdx::lock_guard<stdx::mutex> lk(getAuthzDataMutex(serviceContext));

        AuthorizationManager* authzManager = AuthorizationManager::get(serviceContext);
        status = requireWritableAuthSchema28SCRAM(opCtx, authzManager);
        uassertStatusOK(status);

        RoleName roleName(roleNameString, dbname);
        if (RoleGraph::isBuiltinRole(roleName)) {
            uasserted(ErrorCodes::InvalidRoleModification,
                      str::stream() << roleName.getFullName()
                                    << " is a built-in role and cannot be modified.");
        }

        BSONObj roleDoc;
        status = authzManager->getRoleDescription(opCtx, roleName, &roleDoc);
        uassertStatusOK(status);

        std::vector<RoleName> roles;
        status = auth::parseRoleNamesFromBSONArray(
            BSONArray(roleDoc["roles"].Obj()), roleName.getDB(), &roles);
        uassertStatusOK(status);

        for (vector<RoleName>::const_iterator it = rolesToRemove.begin(); it != rolesToRemove.end();
             ++it) {
            vector<RoleName>::iterator itToRm = std::find(roles.begin(), roles.end(), *it);
            if (itToRm != roles.end()) {
                roles.erase(itToRm);
            }
        }

        audit::logRevokeRolesFromRole(Client::getCurrent(), roleName, rolesToRemove);

        status = updateRoleDocument(
            opCtx, roleName, BSON("$set" << BSON("roles" << rolesVectorToBSONArray(roles))));
        // Must invalidate even on bad status - what if the write succeeded but the GLE failed?
        authzManager->invalidateUserCache();
        uassertStatusOK(status);
        return true;
    }

} cmdRevokeRolesFromRole;

class CmdDropRole : public BasicCommand {
public:
    CmdDropRole() : BasicCommand("dropRole") {}

    bool isUserManagementCommand() const override {
        return true;
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }

    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return true;
    }

    std::string help() const override {
        return "Drops a single role.  Before deleting the role completely it must remove it "
               "from any users or roles that reference it.  If any errors occur in the middle "
               "of that process it's possible to be left in a state where the role has been "
               "removed from some user/roles but otherwise still exists.";
    }

    virtual Status checkAuthForCommand(Client* client,
                                       const std::string& dbname,
                                       const BSONObj& cmdObj) const {
        return auth::checkAuthForDropRoleCommand(client, dbname, cmdObj);
    }

    bool run(OperationContext* opCtx,
             const string& dbname,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) {
        RoleName roleName;
        Status status = auth::parseDropRoleCommand(cmdObj, dbname, &roleName);
        uassertStatusOK(status);

        ServiceContext* serviceContext = opCtx->getClient()->getServiceContext();
        stdx::lock_guard<stdx::mutex> lk(getAuthzDataMutex(serviceContext));

        AuthorizationManager* authzManager = AuthorizationManager::get(serviceContext);
        status = requireWritableAuthSchema28SCRAM(opCtx, authzManager);
        uassertStatusOK(status);

        if (RoleGraph::isBuiltinRole(roleName)) {
            uasserted(ErrorCodes::InvalidRoleModification,
                      str::stream() << roleName.getFullName()
                                    << " is a built-in role and cannot be modified.");
        }

        BSONObj roleDoc;
        status = authzManager->getRoleDescription(opCtx, roleName, &roleDoc);
        uassertStatusOK(status);

        // Remove this role from all users
        long long nMatched;
        status = updateAuthzDocuments(
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
            &nMatched);
        // Must invalidate even on bad status - what if the write succeeded but the GLE failed?
        authzManager->invalidateUserCache();
        if (!status.isOK()) {
            uassertStatusOK(useDefaultCode(status, ErrorCodes::UserModificationFailed)
                                .withContext(str::stream() << "Failed to remove role "
                                                           << roleName.getFullName()
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
            &nMatched);
        // Must invalidate even on bad status - what if the write succeeded but the GLE failed?
        authzManager->invalidateUserCache();
        if (!status.isOK()) {
            uassertStatusOK(
                useDefaultCode(status, ErrorCodes::RoleModificationFailed)
                    .withContext(
                        str::stream() << "Removed role " << roleName.getFullName()
                                      << " from all users but failed to remove from all roles"));
        }

        audit::logDropRole(Client::getCurrent(), roleName);
        // Finally, remove the actual role document
        status = removeRoleDocuments(opCtx,
                                     BSON(AuthorizationManager::ROLE_NAME_FIELD_NAME
                                          << roleName.getRole()
                                          << AuthorizationManager::ROLE_DB_FIELD_NAME
                                          << roleName.getDB()),
                                     &nMatched);
        // Must invalidate even on bad status - what if the write succeeded but the GLE failed?
        authzManager->invalidateUserCache();
        if (!status.isOK()) {
            uassertStatusOK(status.withContext(
                str::stream() << "Removed role " << roleName.getFullName()
                              << " from all users and roles but failed to actually delete"
                                 " the role itself"));
        }

        dassert(nMatched == 0 || nMatched == 1);
        if (nMatched == 0) {
            uasserted(ErrorCodes::RoleNotFound,
                      str::stream() << "Role '" << roleName.getFullName() << "' not found");
        }

        return true;
    }

} cmdDropRole;

class CmdDropAllRolesFromDatabase : public BasicCommand {
public:
    CmdDropAllRolesFromDatabase() : BasicCommand("dropAllRolesFromDatabase") {}

    bool isUserManagementCommand() const override {
        return true;
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }

    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return true;
    }

    std::string help() const override {
        return "Drops all roles from the given database.  Before deleting the roles completely "
               "it must remove them from any users or other roles that reference them.  If any "
               "errors occur in the middle of that process it's possible to be left in a state "
               "where the roles have been removed from some user/roles but otherwise still "
               "exist.";
    }

    virtual Status checkAuthForCommand(Client* client,
                                       const std::string& dbname,
                                       const BSONObj& cmdObj) const {
        return auth::checkAuthForDropAllRolesFromDatabaseCommand(client, dbname);
    }

    bool run(OperationContext* opCtx,
             const string& dbname,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) {
        Status status = auth::parseDropAllRolesFromDatabaseCommand(cmdObj, dbname);
        uassertStatusOK(status);

        ServiceContext* serviceContext = opCtx->getClient()->getServiceContext();
        stdx::lock_guard<stdx::mutex> lk(getAuthzDataMutex(serviceContext));

        AuthorizationManager* authzManager = AuthorizationManager::get(serviceContext);
        status = requireWritableAuthSchema28SCRAM(opCtx, authzManager);
        uassertStatusOK(status);

        // Remove these roles from all users
        long long nMatched;
        status = updateAuthzDocuments(
            opCtx,
            AuthorizationManager::usersCollectionNamespace,
            BSON("roles" << BSON(AuthorizationManager::ROLE_DB_FIELD_NAME << dbname)),
            BSON("$pull" << BSON("roles"
                                 << BSON(AuthorizationManager::ROLE_DB_FIELD_NAME << dbname))),
            false,
            true,
            &nMatched);
        // Must invalidate even on bad status - what if the write succeeded but the GLE failed?
        authzManager->invalidateUserCache();
        if (!status.isOK()) {
            uassertStatusOK(useDefaultCode(status, ErrorCodes::UserModificationFailed)
                                .withContext(str::stream() << "Failed to remove roles from \""
                                                           << dbname
                                                           << "\" db from all users"));
        }

        // Remove these roles from all other roles
        std::string sourceFieldName = str::stream() << "roles."
                                                    << AuthorizationManager::ROLE_DB_FIELD_NAME;
        status = updateAuthzDocuments(
            opCtx,
            AuthorizationManager::rolesCollectionNamespace,
            BSON(sourceFieldName << dbname),
            BSON("$pull" << BSON("roles"
                                 << BSON(AuthorizationManager::ROLE_DB_FIELD_NAME << dbname))),
            false,
            true,
            &nMatched);
        // Must invalidate even on bad status - what if the write succeeded but the GLE failed?
        authzManager->invalidateUserCache();
        if (!status.isOK()) {
            uassertStatusOK(useDefaultCode(status, ErrorCodes::RoleModificationFailed)
                                .withContext(str::stream() << "Failed to remove roles from \""
                                                           << dbname
                                                           << "\" db from all roles"));
        }

        audit::logDropAllRolesFromDatabase(Client::getCurrent(), dbname);
        // Finally, remove the actual role documents
        status = removeRoleDocuments(
            opCtx, BSON(AuthorizationManager::ROLE_DB_FIELD_NAME << dbname), &nMatched);
        // Must invalidate even on bad status - what if the write succeeded but the GLE failed?
        authzManager->invalidateUserCache();
        if (!status.isOK()) {
            uassertStatusOK(status.withContext(
                str::stream() << "Removed roles from \"" << dbname
                              << "\" db "
                                 " from all users and roles but failed to actually delete"
                                 " those roles themselves"));
        }

        result.append("n", nMatched);

        return true;
    }

} cmdDropAllRolesFromDatabase;

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
    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kOptIn;
    }

    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }

    CmdRolesInfo() : BasicCommand("rolesInfo") {}

    std::string help() const override {
        return "Returns information about roles.";
    }

    virtual Status checkAuthForCommand(Client* client,
                                       const std::string& dbname,
                                       const BSONObj& cmdObj) const {
        return auth::checkAuthForRolesInfoCommand(client, dbname, cmdObj);
    }

    bool run(OperationContext* opCtx,
             const string& dbname,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) {
        auth::RolesInfoArgs args;
        Status status = auth::parseRolesInfoCommand(cmdObj, dbname, &args);
        uassertStatusOK(status);

        status = requireReadableAuthSchema26Upgrade(opCtx, getGlobalAuthorizationManager());
        uassertStatusOK(status);

        if (args.allForDB) {
            std::vector<BSONObj> rolesDocs;
            status = getGlobalAuthorizationManager()->getRoleDescriptionsForDB(
                opCtx,
                dbname,
                args.privilegeFormat,
                args.authenticationRestrictionsFormat,
                args.showBuiltinRoles,
                &rolesDocs);
            uassertStatusOK(status);

            if (args.privilegeFormat == PrivilegeFormat::kShowAsUserFragment) {
                uasserted(ErrorCodes::IllegalOperation,
                          "Cannot get user fragment for all roles in a database");
            }
            BSONArrayBuilder rolesArrayBuilder;
            for (size_t i = 0; i < rolesDocs.size(); ++i) {
                rolesArrayBuilder.append(rolesDocs[i]);
            }
            result.append("roles", rolesArrayBuilder.arr());
        } else {
            BSONObj roleDetails;
            status = getGlobalAuthorizationManager()->getRolesDescription(
                opCtx,
                args.roleNames,
                args.privilegeFormat,
                args.authenticationRestrictionsFormat,
                &roleDetails);
            uassertStatusOK(status);

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
    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kAlways;
    }

    virtual bool adminOnly() const {
        return true;
    }

    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }

    CmdInvalidateUserCache() : BasicCommand("invalidateUserCache") {}

    std::string help() const override {
        return "Invalidates the in-memory cache of user information";
    }

    virtual Status checkAuthForCommand(Client* client,
                                       const std::string& dbname,
                                       const BSONObj& cmdObj) const {
        return auth::checkAuthForInvalidateUserCacheCommand(client);
    }

    bool run(OperationContext* opCtx,
             const string& dbname,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) {
        AuthorizationManager* authzManager = getGlobalAuthorizationManager();
        authzManager->invalidateUserCache();
        return true;
    }

} cmdInvalidateUserCache;

class CmdGetCacheGeneration : public BasicCommand {
public:
    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kAlways;
    }

    virtual bool adminOnly() const {
        return true;
    }

    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }

    CmdGetCacheGeneration() : BasicCommand("_getUserCacheGeneration") {}

    std::string help() const override {
        return "internal";
    }

    virtual Status checkAuthForCommand(Client* client,
                                       const std::string& dbname,
                                       const BSONObj& cmdObj) const {
        return auth::checkAuthForGetUserCacheGenerationCommand(client);
    }

    bool run(OperationContext* opCtx,
             const string& dbname,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) {
        AuthorizationManager* authzManager = getGlobalAuthorizationManager();
        result.append("cacheGeneration", authzManager->getCacheGeneration());
        return true;
    }

} CmdGetCacheGeneration;

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

    bool isUserManagementCommand() const override {
        return true;
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }

    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return true;
    }

    virtual bool adminOnly() const {
        return true;
    }

    std::string help() const override {
        return "Internal command used by mongorestore for updating user/role data";
    }

    virtual Status checkAuthForCommand(Client* client,
                                       const std::string& dbname,
                                       const BSONObj& cmdObj) const {
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
                                 userObj.hasField("customData") ? &customData : NULL,
                                 roles,
                                 authenticationRestrictions);
        } else {
            audit::logUpdateUser(Client::getCurrent(),
                                 userName,
                                 hasPwd,
                                 userObj.hasField("customData") ? &customData : NULL,
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
                warning() << "Could not update user " << userName
                          << " in _mergeAuthzCollections command: " << redact(status);
            }
        } else {
            auditCreateOrUpdateUser(userObj, true);
            Status status = insertPrivilegeDocument(opCtx, userObj);
            if (!status.isOK()) {
                // Match the behavior of mongorestore to continue on failure
                warning() << "Could not insert user " << userName
                          << " in _mergeAuthzCollections command: " << redact(status);
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
                warning() << "Could not update role " << roleName
                          << " in _mergeAuthzCollections command: " << redact(status);
            }
        } else {
            auditCreateOrUpdateRole(roleObj, true);
            Status status = insertRoleDocument(opCtx, roleObj);
            if (!status.isOK()) {
                // Match the behavior of mongorestore to continue on failure
                warning() << "Could not insert role " << roleName
                          << " in _mergeAuthzCollections command: " << redact(status);
            }
        }
        rolesToDrop->erase(roleName);
    }

    /**
     * Moves all user objects from usersCollName into admin.system.users.  If drop is true,
     * removes any users that were in admin.system.users but not in usersCollName.
     */
    Status processUsers(OperationContext* opCtx,
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
                                  << 1
                                  << AuthorizationManager::USER_DB_FIELD_NAME
                                  << 1);

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
            long long numRemoved;
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
    Status processRoles(OperationContext* opCtx,
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
                                  << 1
                                  << AuthorizationManager::ROLE_DB_FIELD_NAME
                                  << 1);

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
            long long numRemoved;
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
             const string& dbname,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) {
        auth::MergeAuthzCollectionsArgs args;
        Status status = auth::parseMergeAuthzCollectionsCommand(cmdObj, &args);
        uassertStatusOK(status);

        if (args.usersCollName.empty() && args.rolesCollName.empty()) {
            uasserted(ErrorCodes::BadValue,
                      "Must provide at least one of \"tempUsersCollection\" and "
                      "\"tempRolescollection\"");
        }

        ServiceContext* serviceContext = opCtx->getClient()->getServiceContext();
        stdx::lock_guard<stdx::mutex> lk(getAuthzDataMutex(serviceContext));

        AuthorizationManager* authzManager = AuthorizationManager::get(serviceContext);
        status = requireWritableAuthSchema28SCRAM(opCtx, authzManager);
        uassertStatusOK(status);

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

}  // namespace mongo
