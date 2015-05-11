/**
 *    Copyright (C) 2015 MongoDB Inc.
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
#include "mongo/config.h"
#include "mongo/db/auth/action_set.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/auth/user.h"
#include "mongo/db/auth/user_management_commands_parser.h"
#include "mongo/db/jsobj.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/sequence_util.h"

namespace mongo {
namespace auth {

    void redactPasswordData(mutablebson::Element parent) {
        namespace mmb = mutablebson;
        const StringData pwdFieldName("pwd", StringData::LiteralTag());
        for (mmb::Element pwdElement = mmb::findFirstChildNamed(parent, pwdFieldName);
             pwdElement.ok();
             pwdElement = mmb::findElementNamed(pwdElement.rightSibling(), pwdFieldName)) {

            pwdElement.setValueString("xxx");
        }
    }

    BSONArray roleSetToBSONArray(const unordered_set<RoleName>& roles) {
        BSONArrayBuilder rolesArrayBuilder;
        for (unordered_set<RoleName>::const_iterator it = roles.begin(); it != roles.end(); ++it) {
            const RoleName& role = *it;
            rolesArrayBuilder.append(
                    BSON(AuthorizationManager::ROLE_NAME_FIELD_NAME << role.getRole() <<
                         AuthorizationManager::ROLE_DB_FIELD_NAME << role.getDB()));
        }
        return rolesArrayBuilder.arr();
    }

    BSONArray rolesVectorToBSONArray(const std::vector<RoleName>& roles) {
        BSONArrayBuilder rolesArrayBuilder;
        for (std::vector<RoleName>::const_iterator it = roles.begin(); it != roles.end(); ++it) {
            const RoleName& role = *it;
            rolesArrayBuilder.append(
                    BSON(AuthorizationManager::ROLE_NAME_FIELD_NAME << role.getRole() <<
                         AuthorizationManager::ROLE_DB_FIELD_NAME << role.getDB()));
        }
        return rolesArrayBuilder.arr();
    }

    Status privilegeVectorToBSONArray(const PrivilegeVector& privileges, BSONArray* result) {
        BSONArrayBuilder arrBuilder;
        for (PrivilegeVector::const_iterator it = privileges.begin();
                it != privileges.end(); ++it) {
            const Privilege& privilege = *it;

            ParsedPrivilege parsedPrivilege;
            std::string errmsg;
            if (!ParsedPrivilege::privilegeToParsedPrivilege(privilege,
                                                             &parsedPrivilege,
                                                             &errmsg)) {
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

    Status getCurrentUserRoles(OperationContext* txn,
                               AuthorizationManager* authzManager,
                               const UserName& userName,
                               unordered_set<RoleName>* roles) {
        User* user;
        authzManager->invalidateUserByName(userName); // Need to make sure cache entry is up to date
        Status status = authzManager->acquireUser(txn, userName, &user);
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

    Status checkOkayToGrantRolesToRole(const RoleName& role,
                                       const std::vector<RoleName> rolesToAdd,
                                       AuthorizationManager* authzManager) {
        for (std::vector<RoleName>::const_iterator it = rolesToAdd.begin();
                it != rolesToAdd.end(); ++it) {
            const RoleName& roleToAdd = *it;
            if (roleToAdd == role) {
                return Status(ErrorCodes::InvalidRoleModification,
                              mongoutils::str::stream() << "Cannot grant role " <<
                                      role.getFullName() << " to itself.");
            }

            if (role.getDB() != "admin" && roleToAdd.getDB() != role.getDB()) {
                return Status(ErrorCodes::InvalidRoleModification,
                              str::stream() << "Roles on the \'" << role.getDB() <<
                                      "\' database cannot be granted roles from other databases");
            }

            BSONObj roleToAddDoc;
            Status status = authzManager->getRoleDescription(roleToAdd, false, &roleToAddDoc);
            if (status == ErrorCodes::RoleNotFound) {
                return Status(ErrorCodes::RoleNotFound,
                              "Cannot grant nonexistent role " + roleToAdd.toString());
            }
            if (!status.isOK()) {
                return status;
            }
            std::vector<RoleName> indirectRoles;
            status = auth::parseRoleNamesFromBSONArray(
                    BSONArray(roleToAddDoc["inheritedRoles"].Obj()),
                    role.getDB(),
                    &indirectRoles);
            if (!status.isOK()) {
                return status;
            }

            if (sequenceContains(indirectRoles, role)) {
                return Status(ErrorCodes::InvalidRoleModification,
                              mongoutils::str::stream() << "Granting " <<
                                      roleToAdd.getFullName() << " to " << role.getFullName()
                                      << " would introduce a cycle in the role graph.");
            }
        }
        return Status::OK();
    }

    Status checkOkayToGrantPrivilegesToRole(const RoleName& role,
                                            const PrivilegeVector& privileges) {
        if (role.getDB() == "admin") {
            return Status::OK();
        }

        for (PrivilegeVector::const_iterator it = privileges.begin();
                it != privileges.end(); ++it) {
            const ResourcePattern& resource = (*it).getResourcePattern();
            if ((resource.isDatabasePattern() || resource.isExactNamespacePattern()) &&
                    (resource.databaseToMatch() == role.getDB())) {
                continue;
            }

            return Status(ErrorCodes::InvalidRoleModification,
                          str::stream() << "Roles on the \'" << role.getDB() <<
                                  "\' database cannot be granted privileges that target other "
                                  "databases or the cluster");
        }

        return Status::OK();
    }

    Status requireAuthSchemaVersion26Final(OperationContext* txn,
                                           AuthorizationManager* authzManager) {
        int foundSchemaVersion;
        Status status = authzManager->getAuthorizationVersion(txn, &foundSchemaVersion);
        if (!status.isOK()) {
            return status;
        }

        if (foundSchemaVersion < AuthorizationManager::schemaVersion26Final) {
            return Status(
                    ErrorCodes::AuthSchemaIncompatible,
                    str::stream() << "User and role management commands require auth data to have "
                    "at least schema version " << AuthorizationManager::schemaVersion26Final <<
                    " but found " << foundSchemaVersion);
        }
        return authzManager->writeAuthSchemaVersionIfNeeded(txn, foundSchemaVersion);
    }

    Status requireAuthSchemaVersion26UpgradeOrFinal(OperationContext* txn,
                                                    AuthorizationManager* authzManager) {
        int foundSchemaVersion;
        Status status = authzManager->getAuthorizationVersion(txn, &foundSchemaVersion);
        if (!status.isOK()) {
            return status;
        }

        if (foundSchemaVersion < AuthorizationManager::schemaVersion26Upgrade) {
            return Status(
                    ErrorCodes::AuthSchemaIncompatible,
                    str::stream() << "The usersInfo and rolesInfo commands require auth data to "
                    "have at least schema version " <<
                    AuthorizationManager::schemaVersion26Upgrade <<
                    " but found " << foundSchemaVersion);
        }
        return Status::OK();
    }

    void appendBSONObjToBSONArrayBuilder(BSONArrayBuilder* array, const BSONObj& obj) {
        array->append(obj);
    }

    Status checkAuthorizedToGrantRoles(AuthorizationSession* authzSession,
                                       const std::vector<RoleName>& roles) {
        for (size_t i = 0; i < roles.size(); ++i) {
            if (!authzSession->isAuthorizedToGrantRole(roles[i])) {
                return Status(ErrorCodes::Unauthorized,
                              str::stream() << "Not authorized to grant role: "
                                            << roles[i].getFullName());
            }
        }

        return Status::OK();
    }

    Status checkAuthorizedToGrantPrivileges(AuthorizationSession* authzSession,
                                            const PrivilegeVector& privileges) {
        for (PrivilegeVector::const_iterator it = privileges.begin();
             it != privileges.end(); ++it) {
            Status status = authzSession->checkAuthorizedToGrantPrivilege(*it);
            if (!status.isOK()) {
                return status;
            }
        }

        return Status::OK();
    }

    Status checkAuthorizedToRevokeRoles(AuthorizationSession* authzSession,
                                        const std::vector<RoleName>& roles) {
        for (size_t i = 0; i < roles.size(); ++i) {
            if (!authzSession->isAuthorizedToRevokeRole(roles[i])) {
                return Status(ErrorCodes::Unauthorized,
                              str::stream() << "Not authorized to revoke role: " <<
                                      roles[i].getFullName());
            }
        }
        return Status::OK();
    }

    Status checkAuthorizedToRevokePrivileges(AuthorizationSession* authzSession,
                                             const PrivilegeVector& privileges) {
        for (PrivilegeVector::const_iterator it = privileges.begin();
             it != privileges.end(); ++it) {
            Status status = authzSession->checkAuthorizedToRevokePrivilege(*it);
            if (!status.isOK()) {
                return status;
            }
        }

        return Status::OK();
    }

    Status checkAuthForCreateUserCommand(ClientBasic* client,
                                         const std::string& dbname,
                                         const BSONObj& cmdObj) {
        AuthorizationSession* authzSession = AuthorizationSession::get(client);
        auth::CreateOrUpdateUserArgs args;
        Status status = auth::parseCreateOrUpdateUserCommands(cmdObj,
                                                              "createUser",
                                                              dbname,
                                                              &args);
        if (!status.isOK()) {
            return status;
        }

        if (!authzSession->isAuthorizedForActionsOnResource(
               ResourcePattern::forDatabaseName(args.userName.getDB()),
               ActionType::createUser)) {
            return Status(ErrorCodes::Unauthorized,
                          str::stream() << "Not authorized to create users on db: "
                                        << args.userName.getDB());
        }

        return checkAuthorizedToGrantRoles(authzSession, args.roles);
    }

    Status checkAuthForUpdateUserCommand(ClientBasic* client,
                                         const std::string& dbname,
                                         const BSONObj& cmdObj) {
        AuthorizationSession* authzSession = AuthorizationSession::get(client);
        auth::CreateOrUpdateUserArgs args;
        Status status = auth::parseCreateOrUpdateUserCommands(cmdObj,
                                                              "updateUser",
                                                              dbname,
                                                              &args);
        if (!status.isOK()) {
            return status;
        }

        if (args.hasHashedPassword) {
            if (!authzSession->isAuthorizedToChangeOwnPasswordAsUser(args.userName) &&
                !authzSession->isAuthorizedForActionsOnResource(
                    ResourcePattern::forDatabaseName(args.userName.getDB()),
                    ActionType::changePassword)) {
                return Status(ErrorCodes::Unauthorized,
                              str::stream() << "Not authorized to change password of user: "
                                            << args.userName.getFullName());
            }
        }

        if (args.hasCustomData) {
            if (!authzSession->isAuthorizedToChangeOwnCustomDataAsUser(args.userName) &&
                !authzSession->isAuthorizedForActionsOnResource(
                    ResourcePattern::forDatabaseName(args.userName.getDB()),
                    ActionType::changeCustomData)) {
                return Status(ErrorCodes::Unauthorized,
                              str::stream() << "Not authorized to change customData of user: "
                                            << args.userName.getFullName());
            }
        }

        if (args.hasRoles) {
            // You don't know what roles you might be revoking, so require the ability to
            // revoke any role in the system.
            if (!authzSession->isAuthorizedForActionsOnResource(
                    ResourcePattern::forAnyNormalResource(), ActionType::revokeRole)) {
                return Status(ErrorCodes::Unauthorized,
                              "In order to use updateUser to set roles array, must be "
                              "authorized to revoke any role in the system");
            }

            return checkAuthorizedToGrantRoles(authzSession, args.roles);
        }

        return Status::OK();
    }

    Status checkAuthForGrantRolesToUserCommand(ClientBasic* client,
                                               const std::string& dbname,
                                               const BSONObj& cmdObj) {
        AuthorizationSession* authzSession = AuthorizationSession::get(client);
        std::vector<RoleName> roles;
        std::string unusedUserNameString;
        BSONObj unusedWriteConcern;
        Status status = auth::parseRolePossessionManipulationCommands(cmdObj,
                                                                      "grantRolesToUser",
                                                                      dbname,
                                                                      &unusedUserNameString,
                                                                      &roles,
                                                                      &unusedWriteConcern);
        if (!status.isOK()) {
            return status;
        }

        return checkAuthorizedToGrantRoles(authzSession, roles);
    }

    Status checkAuthForCreateRoleCommand(ClientBasic* client,
                                         const std::string& dbname,
                                         const BSONObj& cmdObj) {
        AuthorizationSession* authzSession = AuthorizationSession::get(client);
        auth::CreateOrUpdateRoleArgs args;
        Status status = auth::parseCreateOrUpdateRoleCommands(cmdObj,
                                                              "createRole",
                                                              dbname,
                                                              &args);
        if (!status.isOK()) {
            return status;
        }

        if (!authzSession->isAuthorizedForActionsOnResource(
               ResourcePattern::forDatabaseName(args.roleName.getDB()),
               ActionType::createRole)) {
            return Status(ErrorCodes::Unauthorized,
                          str::stream() << "Not authorized to create roles on db: "
                                        << args.roleName.getDB());
        }

        status = checkAuthorizedToGrantRoles(authzSession, args.roles);
        if (!status.isOK()) {
            return status;
        }

        return checkAuthorizedToGrantPrivileges(authzSession, args.privileges);
    }

    Status checkAuthForUpdateRoleCommand(ClientBasic* client,
                                         const std::string& dbname,
                                         const BSONObj& cmdObj) {
        AuthorizationSession* authzSession = AuthorizationSession::get(client);
        auth::CreateOrUpdateRoleArgs args;
        Status status = auth::parseCreateOrUpdateRoleCommands(cmdObj,
                                                              "updateRole",
                                                              dbname,
                                                              &args);
        if (!status.isOK()) {
            return status;
        }

        // You don't know what roles or privileges you might be revoking, so require the ability
        // to revoke any role (or privilege) in the system.
        if (!authzSession->isAuthorizedForActionsOnResource(
                ResourcePattern::forAnyNormalResource(), ActionType::revokeRole)) {
            return Status(ErrorCodes::Unauthorized,
                          "updateRole command required the ability to revoke any role in the "
                          "system");
        }

        status = checkAuthorizedToGrantRoles(authzSession, args.roles);
        if (!status.isOK()) {
            return status;
        }

        return checkAuthorizedToGrantPrivileges(authzSession, args.privileges);
    }

    Status checkAuthForGrantRolesToRoleCommand(ClientBasic* client,
                                               const std::string& dbname,
                                               const BSONObj& cmdObj) {
        AuthorizationSession* authzSession = AuthorizationSession::get(client);
        std::vector<RoleName> roles;
        std::string unusedUserNameString;
        BSONObj unusedWriteConcern;
        Status status = auth::parseRolePossessionManipulationCommands(cmdObj,
                                                                      "grantRolesToRole",
                                                                      dbname,
                                                                      &unusedUserNameString,
                                                                      &roles,
                                                                      &unusedWriteConcern);
        if (!status.isOK()) {
            return status;
        }

        return checkAuthorizedToGrantRoles(authzSession, roles);
    }

    Status checkAuthForGrantPrivilegesToRoleCommand(ClientBasic* client,
                                                    const std::string& dbname,
                                                    const BSONObj& cmdObj) {
        AuthorizationSession* authzSession = AuthorizationSession::get(client);
        PrivilegeVector privileges;
        RoleName unusedRoleName;
        BSONObj unusedWriteConcern;
        Status status =
            auth::parseAndValidateRolePrivilegeManipulationCommands(cmdObj,
                                                                    "grantPrivilegesToRole",
                                                                    dbname,
                                                                    &unusedRoleName,
                                                                    &privileges,
                                                                    &unusedWriteConcern);
        if (!status.isOK()) {
            return status;
        }

        return checkAuthorizedToGrantPrivileges(authzSession, privileges);
    }

    Status checkAuthForDropUserCommand(ClientBasic* client,
                                       const std::string& dbname,
                                       const BSONObj& cmdObj) {
        AuthorizationSession* authzSession = AuthorizationSession::get(client);
        UserName userName;
        BSONObj unusedWriteConcern;
        Status status = auth::parseAndValidateDropUserCommand(cmdObj,
                                                              dbname,
                                                              &userName,
                                                              &unusedWriteConcern);
        if (!status.isOK()) {
            return status;
        }

        if (!authzSession->isAuthorizedForActionsOnResource(
               ResourcePattern::forDatabaseName(userName.getDB()), ActionType::dropUser)) {
            return Status(ErrorCodes::Unauthorized,
                          str::stream() << "Not authorized to drop users from the "
                                        << userName.getDB() << " database");
        }
        return Status::OK();
    }

    Status checkAuthForDropRoleCommand(ClientBasic* client,
                                       const std::string& dbname,
                                       const BSONObj& cmdObj) {
        AuthorizationSession* authzSession = AuthorizationSession::get(client);
        RoleName roleName;
        BSONObj unusedWriteConcern;
        Status status = auth::parseDropRoleCommand(cmdObj,
                                                   dbname,
                                                   &roleName,
                                                   &unusedWriteConcern);
        if (!status.isOK()) {
            return status;
        }

        if (!authzSession->isAuthorizedForActionsOnResource(
               ResourcePattern::forDatabaseName(roleName.getDB()), ActionType::dropRole)) {
            return Status(ErrorCodes::Unauthorized,
                          str::stream() << "Not authorized to drop roles from the "
                                        << roleName.getDB() << " database");
        }
        return Status::OK();
    }

    Status checkAuthForDropAllUsersFromDatabaseCommand(ClientBasic* client,
                                                       const std::string& dbname) {
        AuthorizationSession* authzSession = AuthorizationSession::get(client);
        if (!authzSession->isAuthorizedForActionsOnResource(
               ResourcePattern::forDatabaseName(dbname), ActionType::dropUser)) {
            return Status(ErrorCodes::Unauthorized,
                          str::stream() << "Not authorized to drop users from the "
                                        << dbname << " database");
        }
        return Status::OK();
    }

    Status checkAuthForRevokeRolesFromUserCommand(ClientBasic* client,
                                                  const std::string& dbname,
                                                  const BSONObj& cmdObj) {
        AuthorizationSession* authzSession = AuthorizationSession::get(client);
        std::vector<RoleName> roles;
        std::string unusedUserNameString;
        BSONObj unusedWriteConcern;
        Status status = auth::parseRolePossessionManipulationCommands(cmdObj,
                                                                      "revokeRolesFromUser",
                                                                      dbname,
                                                                      &unusedUserNameString,
                                                                      &roles,
                                                                      &unusedWriteConcern);
        if (!status.isOK()) {
            return status;
        }

        return checkAuthorizedToRevokeRoles(authzSession, roles);
    }

   Status checkAuthForRevokeRolesFromRoleCommand(ClientBasic* client,
                                                 const std::string& dbname,
                                                 const BSONObj& cmdObj) {
        AuthorizationSession* authzSession = AuthorizationSession::get(client);
        std::vector<RoleName> roles;
        std::string unusedUserNameString;
        BSONObj unusedWriteConcern;
        Status status = auth::parseRolePossessionManipulationCommands(cmdObj,
                                                                      "revokeRolesFromRole",
                                                                      dbname,
                                                                      &unusedUserNameString,
                                                                      &roles,
                                                                      &unusedWriteConcern);
        if (!status.isOK()) {
            return status;
        }

        return checkAuthorizedToRevokeRoles(authzSession, roles);
    }

    Status checkAuthForUsersInfoCommand(ClientBasic* client,
                                        const std::string& dbname,
                                        const BSONObj& cmdObj) {
        AuthorizationSession* authzSession = AuthorizationSession::get(client);
        auth::UsersInfoArgs args;
        Status status = auth::parseUsersInfoCommand(cmdObj, dbname, &args);
        if (!status.isOK()) {
            return status;
        }

        if (args.allForDB) {
            if (!authzSession->isAuthorizedForActionsOnResource(
                   ResourcePattern::forDatabaseName(dbname), ActionType::viewUser)) {
                return Status(ErrorCodes::Unauthorized,
                              str::stream() << "Not authorized to view users from the "
                                            << dbname << " database");
            }
        } else {
            for (size_t i = 0; i < args.userNames.size(); ++i) {
                if (authzSession->lookupUser(args.userNames[i])) {
                    continue; // Can always view users you are logged in as
                }
                if (!authzSession->isAuthorizedForActionsOnResource(
                       ResourcePattern::forDatabaseName(args.userNames[i].getDB()),
                       ActionType::viewUser)) {
                    return Status(ErrorCodes::Unauthorized,
                                  str::stream() << "Not authorized to view users from the "
                                                << dbname << " database");
                }
            }
        }
        return Status::OK();
    }

    Status checkAuthForRevokePrivilegesFromRoleCommand(ClientBasic* client,
                                                       const std::string& dbname,
                                                       const BSONObj& cmdObj) {
        AuthorizationSession* authzSession = AuthorizationSession::get(client);
        PrivilegeVector privileges;
        RoleName unusedRoleName;
        BSONObj unusedWriteConcern;
        Status status =
            auth::parseAndValidateRolePrivilegeManipulationCommands(cmdObj,
                                                                    "revokePrivilegesFromRole",
                                                                    dbname,
                                                                    &unusedRoleName,
                                                                    &privileges,
                                                                    &unusedWriteConcern);
        if (!status.isOK()) {
            return status;
        }

        return checkAuthorizedToRevokePrivileges(authzSession, privileges);
    }

    Status checkAuthForDropAllRolesFromDatabaseCommand(ClientBasic* client,
                                                       const std::string& dbname) {
        AuthorizationSession* authzSession = AuthorizationSession::get(client);
        if (!authzSession->isAuthorizedForActionsOnResource(
               ResourcePattern::forDatabaseName(dbname), ActionType::dropRole)) {
            return Status(ErrorCodes::Unauthorized,
                          str::stream() << "Not authorized to drop roles from the "
                                        << dbname << " database");
        }
        return Status::OK();
    }

    Status checkAuthForRolesInfoCommand(ClientBasic* client,
                                        const std::string& dbname,
                                        const BSONObj& cmdObj) {
        AuthorizationSession* authzSession = AuthorizationSession::get(client);
        auth::RolesInfoArgs args;
        Status status = auth::parseRolesInfoCommand(cmdObj, dbname, &args);
        if (!status.isOK()) {
            return status;
        }

        if (args.allForDB) {
            if (!authzSession->isAuthorizedForActionsOnResource(
                   ResourcePattern::forDatabaseName(dbname), ActionType::viewRole)) {
                return Status(ErrorCodes::Unauthorized,
                              str::stream() << "Not authorized to view roles from the "
                                            << dbname << " database");
            }
        } else {
            for (size_t i = 0; i < args.roleNames.size(); ++i) {
                if (authzSession->isAuthenticatedAsUserWithRole(args.roleNames[i])) {
                    continue; // Can always see roles that you are a member of
                }

                if (!authzSession->isAuthorizedForActionsOnResource(
                       ResourcePattern::forDatabaseName(args.roleNames[i].getDB()),
                       ActionType::viewRole)) {
                    return Status(ErrorCodes::Unauthorized,
                                  str::stream() << "Not authorized to view roles from the "
                                                << args.roleNames[i].getDB() << " database");
                }
            }
        }

        return Status::OK();
    }

    Status checkAuthForInvalidateUserCacheCommand(ClientBasic* client) {
        AuthorizationSession* authzSession = AuthorizationSession::get(client);
        if (!authzSession->isAuthorizedForActionsOnResource(
               ResourcePattern::forClusterResource(), ActionType::invalidateUserCache)) {
            return Status(ErrorCodes::Unauthorized, "Not authorized to invalidate user cache");
        }
        return Status::OK();
    }

    Status checkAuthForGetUserCacheGenerationCommand(ClientBasic* client) {
        AuthorizationSession* authzSession = AuthorizationSession::get(client);
        if (!authzSession->isAuthorizedForActionsOnResource(
               ResourcePattern::forClusterResource(), ActionType::internal)) {
            return Status(ErrorCodes::Unauthorized, "Not authorized to get cache generation");
        }
        return Status::OK();
    }

    Status checkAuthForMergeAuthzCollectionsCommand(ClientBasic* client,
                                                    const BSONObj& cmdObj) {
        auth::MergeAuthzCollectionsArgs args;
        Status status = auth::parseMergeAuthzCollectionsCommand(cmdObj, &args);
        if (!status.isOK()) {
            return status;
        }

        AuthorizationSession* authzSession = AuthorizationSession::get(client);
        ActionSet actions;
        actions.addAction(ActionType::createUser);
        actions.addAction(ActionType::createRole);
        actions.addAction(ActionType::grantRole);
        actions.addAction(ActionType::revokeRole);
        if (args.drop) {
            actions.addAction(ActionType::dropUser);
            actions.addAction(ActionType::dropRole);
        }
        if (!authzSession->isAuthorizedForActionsOnResource(
               ResourcePattern::forAnyNormalResource(), actions)) {
            return Status(ErrorCodes::Unauthorized,
                          "Not authorized to update user/role data using _mergeAuthzCollections"
                          " command");
        }
        if (!args.usersCollName.empty() &&
            !authzSession->isAuthorizedForActionsOnResource(
                ResourcePattern::forExactNamespace(NamespaceString(args.usersCollName)),
                ActionType::find)) {
            return Status(ErrorCodes::Unauthorized,
                          str::stream() << "Not authorized to read "
                                        << args.usersCollName);
        }
        if (!args.rolesCollName.empty() &&
            !authzSession->isAuthorizedForActionsOnResource(
               ResourcePattern::forExactNamespace(NamespaceString(args.rolesCollName)),
               ActionType::find)) {
            return Status(ErrorCodes::Unauthorized,
                          str::stream() << "Not authorized to read "
                                        << args.rolesCollName);
        }
        return Status::OK();
    }


} // namespace auth
} // namespace mongo
