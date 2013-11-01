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

#include <boost/bind.hpp>
#include <boost/function.hpp>
#include <string>
#include <vector>

#include "mongo/base/status.h"
#include "mongo/bson/mutable/algorithm.h"
#include "mongo/bson/mutable/document.h"
#include "mongo/bson/mutable/element.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/client/dbclientinterface.h"
#include "mongo/db/audit.h"
#include "mongo/db/auth/action_set.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authz_documents_update_guard.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/authorization_manager_global.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/auth/user.h"
#include "mongo/db/auth/user_document_parser.h"
#include "mongo/db/auth/user_management_commands_parser.h"
#include "mongo/db/commands.h"
#include "mongo/db/jsobj.h"
#include "mongo/platform/unordered_set.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/net/ssl_manager.h"
#include "mongo/util/sequence_util.h"

namespace mongo {

    namespace str = mongoutils::str;

    static void addStatus(const Status& status, BSONObjBuilder& builder) {
        builder.append("ok", status.isOK() ? 1.0: 0.0);
        if (!status.isOK())
            builder.append("code", status.code());
        if (!status.reason().empty())
            builder.append("errmsg", status.reason());
    }

    static void redactPasswordData(mutablebson::Element parent) {
        namespace mmb = mutablebson;
        const StringData pwdFieldName("pwd", StringData::LiteralTag());
        for (mmb::Element pwdElement = mmb::findFirstChildNamed(parent, pwdFieldName);
             pwdElement.ok();
             pwdElement = mmb::findElementNamed(pwdElement.rightSibling(), pwdFieldName)) {

            pwdElement.setValueString("xxx");
        }
    }

    static BSONArray roleSetToBSONArray(const unordered_set<RoleName>& roles) {
        BSONArrayBuilder rolesArrayBuilder;
        for (unordered_set<RoleName>::const_iterator it = roles.begin(); it != roles.end(); ++it) {
            const RoleName& role = *it;
            rolesArrayBuilder.append(
                    BSON(AuthorizationManager::ROLE_NAME_FIELD_NAME << role.getRole() <<
                         AuthorizationManager::ROLE_SOURCE_FIELD_NAME << role.getDB()));
        }
        return rolesArrayBuilder.arr();
    }

    static BSONArray rolesVectorToBSONArray(const std::vector<RoleName>& roles) {
        BSONArrayBuilder rolesArrayBuilder;
        for (std::vector<RoleName>::const_iterator it = roles.begin(); it != roles.end(); ++it) {
            const RoleName& role = *it;
            rolesArrayBuilder.append(
                    BSON(AuthorizationManager::ROLE_NAME_FIELD_NAME << role.getRole() <<
                         AuthorizationManager::ROLE_SOURCE_FIELD_NAME << role.getDB()));
        }
        return rolesArrayBuilder.arr();
    }

    static Status privilegeVectorToBSONArray(const PrivilegeVector& privileges, BSONArray* result) {
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

    static Status getCurrentUserRoles(AuthorizationManager* authzManager,
                                      const UserName& userName,
                                      unordered_set<RoleName>* roles) {
        User* user;
        Status status = authzManager->acquireUser(userName, &user);
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

    static Status checkAuthorizedToGrantRoles(AuthorizationSession* authzSession,
                                              const std::vector<RoleName>& roles) {
        for (size_t i = 0; i < roles.size(); ++i) {
            if (!authzSession->isAuthorizedToGrantRole(roles[i])) {
                return Status(ErrorCodes::Unauthorized,
                              str::stream() << "Not authorized to grant role: " <<
                                      roles[i].getFullName());
            }
        }
        return Status::OK();
    }

    static Status checkAuthorizedToRevokeRoles(AuthorizationSession* authzSession,
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

    static Status checkAuthorizedToGrantPrivileges(AuthorizationSession* authzSession,
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

    static Status checkAuthorizedToRevokePrivileges(AuthorizationSession* authzSession,
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

    /*
     * Checks that every role in "rolesToAdd" exists, and that adding each of those roles to "role"
     * will not result in a cycle to the role graph.
     */
    static Status checkOkayToGrantRolesToRole(const RoleName& role,
                                              const std::vector<RoleName> rolesToAdd,
                                              AuthorizationManager* authzManager) {
        for (vector<RoleName>::const_iterator it = rolesToAdd.begin();
                it != rolesToAdd.end(); ++it) {
            const RoleName& roleToAdd = *it;
            if (roleToAdd == role) {
                return Status(ErrorCodes::InvalidRoleModification,
                              mongoutils::str::stream() << "Cannot grant role " <<
                                      role.getFullName() << " to itself.");
            }
            BSONObj roleToAddDoc;
            Status status = authzManager->getRoleDescription(roleToAdd, &roleToAddDoc);
            if (status == ErrorCodes::RoleNotFound) {
                return Status(ErrorCodes::RoleNotFound,
                              "Cannot grant nonexistent role " + roleToAdd.toString());
            }
            if (!status.isOK()) {
                return status;
            }
            std::vector<RoleName> indirectRoles;
            status = auth::parseRoleNamesFromBSONArray(
                    BSONArray(roleToAddDoc["indirectRoles"].Obj()),
                    role.getDB(),
                    &indirectRoles);
            if (!status.isOK()) {
                return status;
            }

            if (sequenceContains(indirectRoles, role)) {
                return Status(ErrorCodes::InvalidRoleModification,
                              mongoutils::str::stream() << "Granting" <<
                                      roleToAdd.getFullName() << " to " << role.getFullName()
                                      << " would introduce a cycle in the role graph.");
            }
        }
        return Status::OK();
    }

    static Status requireAuthSchemaVersion26Final(AuthorizationManager* authzManager) {
        const int foundSchemaVersion = authzManager->getAuthorizationVersion();
        if (foundSchemaVersion != AuthorizationManager::schemaVersion26Final) {
            return Status(
                    ErrorCodes::AuthSchemaIncompatible,
                    str::stream() << "User and role management commands require auth data to have "
                    "schema version " << AuthorizationManager::schemaVersion26Final <<
                    " but found " << foundSchemaVersion);
        }
        return authzManager->writeAuthSchemaVersionIfNeeded();
    }

    static Status requireAuthSchemaVersion26UpgradeOrFinal(AuthorizationManager* authzManager) {
        const int foundSchemaVersion = authzManager->getAuthorizationVersion();
        if (foundSchemaVersion != AuthorizationManager::schemaVersion26Final &&
            foundSchemaVersion != AuthorizationManager::schemaVersion26Upgrade) {
            return Status(
                    ErrorCodes::AuthSchemaIncompatible,
                    str::stream() << "The usersInfo and rolesInfo commands require auth data to "
                    "have schema  version " << AuthorizationManager::schemaVersion26Final <<
                    " or " << AuthorizationManager::schemaVersion26Upgrade <<
                    " but found " << foundSchemaVersion);
        }
        return Status::OK();
    }

    class CmdCreateUser : public Command {
    public:

        CmdCreateUser() : Command("createUser") {}

        virtual bool logTheOp() {
            return false;
        }

        virtual bool slaveOk() const {
            return false;
        }

        virtual LockType locktype() const {
            return NONE;
        }

        virtual void help(stringstream& ss) const {
            ss << "Adds a user to the system" << endl;
        }

        virtual Status checkAuthForCommand(ClientBasic* client,
                                           const std::string& dbname,
                                           const BSONObj& cmdObj) {
            AuthorizationSession* authzSession = client->getAuthorizationSession();
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
                              str::stream() << "Not authorized to create users on db: " <<
                                      args.userName.getDB());
            }

            return checkAuthorizedToGrantRoles(authzSession, args.roles);
        }

        bool run(const string& dbname,
                 BSONObj& cmdObj,
                 int options,
                 string& errmsg,
                 BSONObjBuilder& result,
                 bool fromRepl) {
            auth::CreateOrUpdateUserArgs args;
            Status status = auth::parseCreateOrUpdateUserCommands(cmdObj,
                                                                  "createUser",
                                                                  dbname,
                                                                  &args);
            if (!status.isOK()) {
                addStatus(status, result);
                return false;
            }

            if (args.userName.getDB() == "local") {
                addStatus(Status(ErrorCodes::BadValue, "Cannot create users in the local database"),
                          result);
                return false;
            }

            if (!args.hasHashedPassword && args.userName.getDB() != "$external") {
                addStatus(Status(ErrorCodes::BadValue,
                                 "Must provide a 'pwd' field for all user documents, except those"
                                         " with '$external' as the user's source db"),
                          result);
                return false;
            }

            if (!args.hasRoles) {
                addStatus(Status(ErrorCodes::BadValue,
                                 "\"createUser\" command requires a \"roles\" array"),
                          result);
                return false;
            }

#ifdef MONGO_SSL
            if (args.userName.getDB() == "$external" &&
                getSSLManager() && 
                getSSLManager()->getServerSubjectName() == args.userName.getUser()) {
                addStatus(Status(ErrorCodes::BadValue,
                                 "Cannot create an x.509 user with the same "
                                 "subjectname as the server"),
                          result);
                return false;
            }
#endif
 
            BSONObjBuilder userObjBuilder;
            userObjBuilder.append("_id",
                                  str::stream() << args.userName.getDB() << "." <<
                                          args.userName.getUser());
            userObjBuilder.append(AuthorizationManager::USER_NAME_FIELD_NAME,
                                  args.userName.getUser());
            userObjBuilder.append(AuthorizationManager::USER_DB_FIELD_NAME,
                                  args.userName.getDB());
            if (args.hasHashedPassword) {
                userObjBuilder.append("credentials", BSON("MONGODB-CR" << args.hashedPassword));
            } else {
                // Must be an external user
                userObjBuilder.append("credentials", BSON("external" << true));
            }
            if (args.hasCustomData) {
                userObjBuilder.append("customData", args.customData);
            }
            userObjBuilder.append("roles", rolesVectorToBSONArray(args.roles));

            BSONObj userObj = userObjBuilder.obj();
            V2UserDocumentParser parser;
            status = parser.checkValidUserDocument(userObj);
            if (!status.isOK()) {
                addStatus(status, result);
                return false;
            }

            AuthorizationManager* authzManager = getGlobalAuthorizationManager();
            AuthzDocumentsUpdateGuard updateGuard(authzManager);
            if (!updateGuard.tryLock("Create user")) {
                addStatus(Status(ErrorCodes::LockBusy, "Could not lock auth data update lock."),
                          result);
                return false;
            }

            status = requireAuthSchemaVersion26Final(authzManager);
            if (!status.isOK()) {
                addStatus(status, result);
                return false;
            }

            // Role existence has to be checked after acquiring the update lock
            for (size_t i = 0; i < args.roles.size(); ++i) {
                BSONObj ignored;
                status = authzManager->getRoleDescription(args.roles[i], &ignored);
                if (!status.isOK()) {
                    addStatus(status, result);
                    return false;
                }
            }

            audit::logCreateUser(ClientBasic::getCurrent(),
                                 args.userName,
                                 args.hasHashedPassword,
                                 args.hasCustomData? &args.customData : NULL,
                                 args.roles);
            status = authzManager->insertPrivilegeDocument(dbname,
                                                           userObj,
                                                           args.writeConcern);
            if (!status.isOK()) {
                addStatus(status, result);
                return false;
            }
            return true;
        }

        virtual void redactForLogging(mutablebson::Document* cmdObj) {
            redactPasswordData(cmdObj->root());
        }

    } cmdCreateUser;

    class CmdUpdateUser : public Command {
    public:

        CmdUpdateUser() : Command("updateUser") {}

        virtual bool logTheOp() {
            return false;
        }

        virtual bool slaveOk() const {
            return false;
        }

        virtual LockType locktype() const {
            return NONE;
        }

        virtual void help(stringstream& ss) const {
            ss << "Used to update a user, for example to change its password" << endl;
        }

        virtual Status checkAuthForCommand(ClientBasic* client,
                                           const std::string& dbname,
                                           const BSONObj& cmdObj) {
            AuthorizationSession* authzSession = client->getAuthorizationSession();
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
                                  str::stream() << "Not authorized to change password of user: " <<
                                          args.userName.getFullName());
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

        bool run(const string& dbname,
                 BSONObj& cmdObj,
                 int options,
                 string& errmsg,
                 BSONObjBuilder& result,
                 bool fromRepl) {
            auth::CreateOrUpdateUserArgs args;
            Status status = auth::parseCreateOrUpdateUserCommands(cmdObj,
                                                                  "updateUser",
                                                                  dbname,
                                                                  &args);
            if (!status.isOK()) {
                addStatus(status, result);
                return false;
            }

            if (!args.hasHashedPassword && !args.hasCustomData && !args.hasRoles) {
                addStatus(Status(ErrorCodes::BadValue,
                                 "Must specify at least one field to update in updateUser"),
                          result);
                return false;
            }

            BSONObjBuilder updateSetBuilder;
            if (args.hasHashedPassword) {
                updateSetBuilder.append("credentials.MONGODB-CR", args.hashedPassword);
            }
            if (args.hasCustomData) {
                updateSetBuilder.append("customData", args.customData);
            }
            if (args.hasRoles) {
                updateSetBuilder.append("roles", rolesVectorToBSONArray(args.roles));
            }

            AuthorizationManager* authzManager = getGlobalAuthorizationManager();
            AuthzDocumentsUpdateGuard updateGuard(authzManager);
            if (!updateGuard.tryLock("Update user")) {
                addStatus(Status(ErrorCodes::LockBusy, "Could not lock auth data update lock."),
                          result);
                return false;
            }

            status = requireAuthSchemaVersion26Final(authzManager);
            if (!status.isOK()) {
                addStatus(status, result);
                return false;
            }


            // Role existence has to be checked after acquiring the update lock
            if (args.hasRoles) {
                for (size_t i = 0; i < args.roles.size(); ++i) {
                    BSONObj ignored;
                    status = authzManager->getRoleDescription(args.roles[i], &ignored);
                    if (!status.isOK()) {
                        addStatus(status, result);
                        return false;
                    }
                }
            }

            audit::logUpdateUser(ClientBasic::getCurrent(),
                                 args.userName,
                                 args.hasHashedPassword,
                                 args.hasCustomData? &args.customData : NULL,
                                 args.hasRoles? &args.roles : NULL);

            status = authzManager->updatePrivilegeDocument(args.userName,
                                                           BSON("$set" << updateSetBuilder.done()),
                                                           args.writeConcern);
            // Must invalidate even on bad status - what if the write succeeded but the GLE failed?
            authzManager->invalidateUserByName(args.userName);
            if (!status.isOK()) {
                addStatus(status, result);
                return false;
            }

            return true;
        }

        virtual void redactForLogging(mutablebson::Document* cmdObj) {
            redactPasswordData(cmdObj->root());
        }

    } cmdUpdateUser;

    class CmdDropUser : public Command {
    public:

        CmdDropUser() : Command("dropUser") {}

        virtual bool logTheOp() {
            return false;
        }

        virtual bool slaveOk() const {
            return false;
        }

        virtual LockType locktype() const {
            return NONE;
        }

        virtual void help(stringstream& ss) const {
            ss << "Drops a single user." << endl;
        }

        virtual Status checkAuthForCommand(ClientBasic* client,
                                           const std::string& dbname,
                                           const BSONObj& cmdObj) {
            AuthorizationSession* authzSession = client->getAuthorizationSession();
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
                              str::stream() << "Not authorized to drop users from the " <<
                                      userName.getDB() << " database");
            }
            return Status::OK();
        }

        bool run(const string& dbname,
                 BSONObj& cmdObj,
                 int options,
                 string& errmsg,
                 BSONObjBuilder& result,
                 bool fromRepl) {
            AuthorizationManager* authzManager = getGlobalAuthorizationManager();
            AuthzDocumentsUpdateGuard updateGuard(authzManager);
            if (!updateGuard.tryLock("Drop user")) {
                addStatus(Status(ErrorCodes::LockBusy, "Could not lock auth data update lock."),
                          result);
                return false;
            }

            Status status = requireAuthSchemaVersion26Final(authzManager);
            if (!status.isOK()) {
                addStatus(status, result);
                return false;
            }


            UserName userName;
            BSONObj writeConcern;
            status = auth::parseAndValidateDropUserCommand(cmdObj,
                                                           dbname,
                                                           &userName,
                                                           &writeConcern);
            if (!status.isOK()) {
                addStatus(status, result);
                return false;
            }

            int numUpdated;

            audit::logDropUser(ClientBasic::getCurrent(), userName);

            status = authzManager->removePrivilegeDocuments(
                    BSON(AuthorizationManager::USER_NAME_FIELD_NAME << userName.getUser() <<
                         AuthorizationManager::USER_DB_FIELD_NAME << userName.getDB()),
                    writeConcern,
                    &numUpdated);
            // Must invalidate even on bad status - what if the write succeeded but the GLE failed?
            authzManager->invalidateUserByName(userName);
            if (!status.isOK()) {
                addStatus(status, result);
                return false;
            }

            if (numUpdated == 0) {
                addStatus(Status(ErrorCodes::UserNotFound,
                                 str::stream() << "User '" << userName.getFullName() <<
                                         "' not found"),
                          result);
                return false;
            }

            return true;
        }

    } cmdDropUser;

    class CmdDropAllUsersFromDatabase : public Command {
    public:

        CmdDropAllUsersFromDatabase() : Command("dropAllUsersFromDatabase") {}

        virtual bool logTheOp() {
            return false;
        }

        virtual bool slaveOk() const {
            return false;
        }

        virtual LockType locktype() const {
            return NONE;
        }

        virtual void help(stringstream& ss) const {
            ss << "Drops all users for a single database." << endl;
        }

        virtual Status checkAuthForCommand(ClientBasic* client,
                                           const std::string& dbname,
                                           const BSONObj& cmdObj) {
            AuthorizationSession* authzSession = client->getAuthorizationSession();
            if (!authzSession->isAuthorizedForActionsOnResource(
                    ResourcePattern::forDatabaseName(dbname), ActionType::dropUser)) {
                return Status(ErrorCodes::Unauthorized,
                              str::stream() << "Not authorized to drop users from the " <<
                                      dbname << " database");
            }
            return Status::OK();
        }

        bool run(const string& dbname,
                 BSONObj& cmdObj,
                 int options,
                 string& errmsg,
                 BSONObjBuilder& result,
                 bool fromRepl) {
            AuthorizationManager* authzManager = getGlobalAuthorizationManager();
            AuthzDocumentsUpdateGuard updateGuard(authzManager);
            if (!updateGuard.tryLock("Drop all users from database")) {
                addStatus(Status(ErrorCodes::LockBusy, "Could not lock auth data update lock."),
                          result);
                return false;
            }

            Status status = requireAuthSchemaVersion26Final(authzManager);
            if (!status.isOK()) {
                addStatus(status, result);
                return false;
            }

            BSONObj writeConcern;
            status = auth::parseAndValidateDropAllUsersFromDatabaseCommand(cmdObj,
                                                                           dbname,
                                                                           &writeConcern);
            if (!status.isOK()) {
                addStatus(status, result);
                return false;
            }

            int numRemoved;

            audit::logDropAllUsersFromDatabase(ClientBasic::getCurrent(), dbname);

            status = authzManager->removePrivilegeDocuments(
                    BSON(AuthorizationManager::USER_DB_FIELD_NAME << dbname),
                    writeConcern,
                    &numRemoved);
            // Must invalidate even on bad status - what if the write succeeded but the GLE failed?
            authzManager->invalidateUsersFromDB(dbname);
            if (!status.isOK()) {
                addStatus(status, result);
                return false;
            }

            result.append("n", numRemoved);
            return true;
        }

    } cmdDropAllUsersFromDatabase;

    class CmdGrantRolesToUser: public Command {
    public:

        CmdGrantRolesToUser() : Command("grantRolesToUser") {}

        virtual bool logTheOp() {
            return false;
        }

        virtual bool slaveOk() const {
            return false;
        }

        virtual LockType locktype() const {
            return NONE;
        }

        virtual void help(stringstream& ss) const {
            ss << "Grants roles to a user." << endl;
        }

        virtual Status checkAuthForCommand(ClientBasic* client,
                                           const std::string& dbname,
                                           const BSONObj& cmdObj) {
            AuthorizationSession* authzSession = client->getAuthorizationSession();
            std::vector<RoleName> roles;
            std::string unusedUserNameString;
            BSONObj unusedWriteConcern;
            Status status = auth::parseRolePossessionManipulationCommands(cmdObj,
                                                                          "grantRolesToUser",
                                                                          "roles",
                                                                          dbname,
                                                                          &unusedUserNameString,
                                                                          &roles,
                                                                          &unusedWriteConcern);
            if (!status.isOK()) {
                return status;
            }

            return checkAuthorizedToGrantRoles(authzSession, roles);
        }

        bool run(const string& dbname,
                 BSONObj& cmdObj,
                 int options,
                 string& errmsg,
                 BSONObjBuilder& result,
                 bool fromRepl) {
            AuthorizationManager* authzManager = getGlobalAuthorizationManager();
            AuthzDocumentsUpdateGuard updateGuard(authzManager);
            if (!updateGuard.tryLock("Grant roles to user")) {
                addStatus(Status(ErrorCodes::LockBusy, "Could not lock auth data update lock."),
                          result);
                return false;
            }

            Status status = requireAuthSchemaVersion26Final(authzManager);
            if (!status.isOK()) {
                addStatus(status, result);
                return false;
            }

            std::string userNameString;
            std::vector<RoleName> roles;
            BSONObj writeConcern;
            status = auth::parseRolePossessionManipulationCommands(cmdObj,
                                                                   "grantRolesToUser",
                                                                   "roles",
                                                                   dbname,
                                                                   &userNameString,
                                                                   &roles,
                                                                   &writeConcern);
            if (!status.isOK()) {
                addStatus(status, result);
                return false;
            }

            UserName userName(userNameString, dbname);
            unordered_set<RoleName> userRoles;
            status = getCurrentUserRoles(authzManager, userName, &userRoles);
            if (!status.isOK()) {
                addStatus(status, result);
                return false;
            }

            for (vector<RoleName>::iterator it = roles.begin(); it != roles.end(); ++it) {
                RoleName& roleName = *it;
                BSONObj roleDoc;
                status = authzManager->getRoleDescription(roleName, &roleDoc);
                if (!status.isOK()) {
                    addStatus(status, result);
                    return false;
                }

                userRoles.insert(roleName);
            }

            audit::logGrantRolesToUser(ClientBasic::getCurrent(),
                                       userName,
                                       roles);
            BSONArray newRolesBSONArray = roleSetToBSONArray(userRoles);
            status = authzManager->updatePrivilegeDocument(
                    userName, BSON("$set" << BSON("roles" << newRolesBSONArray)), writeConcern);
            // Must invalidate even on bad status - what if the write succeeded but the GLE failed?
            authzManager->invalidateUserByName(userName);
            if (!status.isOK()) {
                addStatus(status, result);
                return false;
            }

            return true;
        }

    } cmdGrantRolesToUser;

    class CmdRevokeRolesFromUser: public Command {
    public:

        CmdRevokeRolesFromUser() : Command("revokeRolesFromUser") {}

        virtual bool logTheOp() {
            return false;
        }

        virtual bool slaveOk() const {
            return false;
        }

        virtual LockType locktype() const {
            return NONE;
        }

        virtual void help(stringstream& ss) const {
            ss << "Revokes roles from a user." << endl;
        }

        virtual Status checkAuthForCommand(ClientBasic* client,
                                           const std::string& dbname,
                                           const BSONObj& cmdObj) {
            AuthorizationSession* authzSession = client->getAuthorizationSession();
            std::vector<RoleName> roles;
            std::string unusedUserNameString;
            BSONObj unusedWriteConcern;
            Status status = auth::parseRolePossessionManipulationCommands(cmdObj,
                                                                          "revokeRolesFromUser",
                                                                          "roles",
                                                                          dbname,
                                                                          &unusedUserNameString,
                                                                          &roles,
                                                                          &unusedWriteConcern);
            if (!status.isOK()) {
                return status;
            }

            return checkAuthorizedToRevokeRoles(authzSession, roles);
        }

        bool run(const string& dbname,
                 BSONObj& cmdObj,
                 int options,
                 string& errmsg,
                 BSONObjBuilder& result,
                 bool fromRepl) {
            AuthorizationManager* authzManager = getGlobalAuthorizationManager();
            AuthzDocumentsUpdateGuard updateGuard(authzManager);
            if (!updateGuard.tryLock("Revoke roles from user")) {
                addStatus(Status(ErrorCodes::LockBusy, "Could not lock auth data update lock."),
                          result);
                return false;
            }

            Status status = requireAuthSchemaVersion26Final(authzManager);
            if (!status.isOK()) {
                addStatus(status, result);
                return false;
            }

            std::string userNameString;
            std::vector<RoleName> roles;
            BSONObj writeConcern;
            status = auth::parseRolePossessionManipulationCommands(cmdObj,
                                                                   "revokeRolesFromUser",
                                                                   "roles",
                                                                   dbname,
                                                                   &userNameString,
                                                                   &roles,
                                                                   &writeConcern);
            if (!status.isOK()) {
                addStatus(status, result);
                return false;
            }

            UserName userName(userNameString, dbname);
            unordered_set<RoleName> userRoles;
            status = getCurrentUserRoles(authzManager, userName, &userRoles);
            if (!status.isOK()) {
                addStatus(status, result);
                return false;
            }

            for (vector<RoleName>::iterator it = roles.begin(); it != roles.end(); ++it) {
                RoleName& roleName = *it;
                BSONObj roleDoc;
                status = authzManager->getRoleDescription(roleName, &roleDoc);
                if (!status.isOK()) {
                    addStatus(status, result);
                    return false;
                }

                userRoles.erase(roleName);
            }

            audit::logRevokeRolesFromUser(ClientBasic::getCurrent(),
                                          userName,
                                          roles);
            BSONArray newRolesBSONArray = roleSetToBSONArray(userRoles);
            status = authzManager->updatePrivilegeDocument(
                    userName, BSON("$set" << BSON("roles" << newRolesBSONArray)), writeConcern);
            // Must invalidate even on bad status - what if the write succeeded but the GLE failed?
            authzManager->invalidateUserByName(userName);
            if (!status.isOK()) {
                addStatus(status, result);
                return false;
            }

            return true;
        }

    } cmdRevokeRolesFromUser;

    class CmdUsersInfo: public Command {
    public:

        virtual bool logTheOp() {
            return false;
        }

        virtual bool slaveOk() const {
            return true;
        }

        virtual LockType locktype() const {
            return NONE;
        }

        CmdUsersInfo() : Command("usersInfo") {}

        virtual void help(stringstream& ss) const {
            ss << "Returns information about users." << endl;
        }

        virtual Status checkAuthForCommand(ClientBasic* client,
                                           const std::string& dbname,
                                           const BSONObj& cmdObj) {
            AuthorizationSession* authzSession = client->getAuthorizationSession();
            auth::UsersInfoArgs args;
            Status status = auth::parseUsersInfoCommand(cmdObj, dbname, &args);
            if (!status.isOK()) {
                return status;
            }

            if (args.allForDB) {
                if (!authzSession->isAuthorizedForActionsOnResource(
                        ResourcePattern::forDatabaseName(dbname), ActionType::viewUser)) {
                    return Status(ErrorCodes::Unauthorized,
                                  str::stream() << "Not authorized to view users from the " <<
                                          dbname << " database");
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
                                      str::stream() << "Not authorized to view users from the " <<
                                              dbname << " database");
                    }
                }
            }
            return Status::OK();
        }

        bool run(const string& dbname,
                 BSONObj& cmdObj,
                 int options,
                 string& errmsg,
                 BSONObjBuilder& result,
                 bool fromRepl) {

            auth::UsersInfoArgs args;
            Status status = auth::parseUsersInfoCommand(cmdObj, dbname, &args);
            if (!status.isOK()) {
                addStatus(status, result);
                return false;
            }

            status = requireAuthSchemaVersion26UpgradeOrFinal(getGlobalAuthorizationManager());
            if (!status.isOK()) {
                addStatus(status, result);
                return false;
            }

            if (args.allForDB && args.showPrivileges) {
                addStatus(Status(ErrorCodes::IllegalOperation,
                                 "Can only get privilege details on exact-match usersInfo "
                                 "queries."),
                          result);
                return false;
            }

            BSONArrayBuilder usersArrayBuilder;
            if (args.showPrivileges) {
                // If you want privileges you need to call getUserDescription on each user.
                for (size_t i = 0; i < args.userNames.size(); ++i) {
                    BSONObj userDetails;
                    status = getGlobalAuthorizationManager()->getUserDescription(
                            args.userNames[i], &userDetails);
                    if (status.code() == ErrorCodes::UserNotFound) {
                        continue;
                    }
                    if (!status.isOK()) {
                        addStatus(status, result);
                        return false;
                    }
                    if (!args.showCredentials) {
                        // getUserDescription always includes credentials, need to strip it out
                        BSONObjBuilder userWithoutCredentials(usersArrayBuilder.subobjStart());
                        for (BSONObjIterator it(userDetails);  it.more(); ) {
                            BSONElement e = it.next();
                            if (e.fieldNameStringData() != "credentials")
                                userWithoutCredentials.append(e);
                        }
                        userWithoutCredentials.doneFast();
                    } else {
                        usersArrayBuilder.append(userDetails);
                    }
                }
            } else {
                // If you don't need privileges, you can just do a regular query on system.users
                BSONObjBuilder queryBuilder;
                if (args.allForDB) {
                    queryBuilder.append(AuthorizationManager::USER_DB_FIELD_NAME, dbname);
                } else {
                    BSONArrayBuilder usersMatchArray;
                    for (size_t i = 0; i < args.userNames.size(); ++i) {
                        usersMatchArray.append(BSON(AuthorizationManager::USER_NAME_FIELD_NAME <<
                                                    args.userNames[i].getUser() <<
                                                    AuthorizationManager::USER_DB_FIELD_NAME <<
                                                    args.userNames[i].getDB()));
                    }
                    queryBuilder.append("$or", usersMatchArray.arr());

                }

                AuthorizationManager* authzManager = getGlobalAuthorizationManager();
                BSONObjBuilder projection;
                if (!args.showCredentials) {
                    projection.append("credentials", 0);
                }
                BSONArrayBuilder& (BSONArrayBuilder::* appendBSONObj) (const BSONObj&) =
                        &BSONArrayBuilder::append<BSONObj>;
                const boost::function<void(const BSONObj&)> function =
                        boost::bind(appendBSONObj, &usersArrayBuilder, _1);
                authzManager->queryAuthzDocument(AuthorizationManager::usersCollectionNamespace,
                                                 queryBuilder.done(),
                                                 projection.done(),
                                                 function);
            }
            result.append("users", usersArrayBuilder.arr());
            return true;
        }

    } cmdUsersInfo;

    class CmdCreateRole: public Command {
    public:

        CmdCreateRole() : Command("createRole") {}

        virtual bool logTheOp() {
            return false;
        }

        virtual bool slaveOk() const {
            return false;
        }

        virtual LockType locktype() const {
            return NONE;
        }

        virtual void help(stringstream& ss) const {
            ss << "Adds a role to the system" << endl;
        }

        virtual Status checkAuthForCommand(ClientBasic* client,
                                           const std::string& dbname,
                                           const BSONObj& cmdObj) {
            AuthorizationSession* authzSession = client->getAuthorizationSession();
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
                              str::stream() << "Not authorized to create roles on db: " <<
                                      args.roleName.getDB());
            }

            status = checkAuthorizedToGrantRoles(authzSession, args.roles);
            if (!status.isOK()) {
                return status;
            }

            return checkAuthorizedToGrantPrivileges(authzSession, args.privileges);
        }

        bool run(const string& dbname,
                 BSONObj& cmdObj,
                 int options,
                 string& errmsg,
                 BSONObjBuilder& result,
                 bool fromRepl) {
            auth::CreateOrUpdateRoleArgs args;
            Status status = auth::parseCreateOrUpdateRoleCommands(cmdObj,
                                                                  "createRole",
                                                                  dbname,
                                                                  &args);
            if (!status.isOK()) {
                addStatus(status, result);
                return false;
            }

            if (args.roleName.getDB() == "local") {
                addStatus(Status(ErrorCodes::BadValue, "Cannot create roles in the local database"),
                          result);
                return false;
            }

            if (!args.hasRoles) {
                addStatus(Status(ErrorCodes::BadValue,
                                 "\"createRole\" command requires a \"roles\" array"),
                          result);
                return false;
            }

            if (!args.hasPrivileges) {
                addStatus(Status(ErrorCodes::BadValue,
                                 "\"createRole\" command requires a \"privileges\" array"),
                          result);
                return false;
            }

            BSONObjBuilder roleObjBuilder;

            roleObjBuilder.append("_id", str::stream() << args.roleName.getDB() << "." <<
                                          args.roleName.getRole());
            roleObjBuilder.append(AuthorizationManager::ROLE_NAME_FIELD_NAME,
                                  args.roleName.getRole());
            roleObjBuilder.append(AuthorizationManager::ROLE_SOURCE_FIELD_NAME,
                                  args.roleName.getDB());

            BSONArray privileges;
            status = privilegeVectorToBSONArray(args.privileges, &privileges);
            if (!status.isOK()) {
                addStatus(status, result);
                return false;
            }
            roleObjBuilder.append("privileges", privileges);

            roleObjBuilder.append("roles", rolesVectorToBSONArray(args.roles));

            AuthorizationManager* authzManager = getGlobalAuthorizationManager();
            AuthzDocumentsUpdateGuard updateGuard(authzManager);
            if (!updateGuard.tryLock("Create role")) {
                addStatus(Status(ErrorCodes::LockBusy, "Could not lock auth data update lock."),
                          result);
                return false;
            }

            status = requireAuthSchemaVersion26Final(authzManager);
            if (!status.isOK()) {
                addStatus(status, result);
                return false;
            }

            // Role existence has to be checked after acquiring the update lock
            status = checkOkayToGrantRolesToRole(args.roleName, args.roles, authzManager);
            if (!status.isOK()) {
                addStatus(status, result);
                return false;
            }

            audit::logCreateRole(ClientBasic::getCurrent(),
                                 args.roleName,
                                 args.roles,
                                 args.privileges);

            status = authzManager->insertRoleDocument(roleObjBuilder.done(), args.writeConcern);
            if (!status.isOK()) {
                addStatus(status, result);
                return false;
            }
            return true;
        }

    } cmdCreateRole;

    class CmdUpdateRole: public Command {
    public:

        CmdUpdateRole() : Command("updateRole") {}

        virtual bool logTheOp() {
            return false;
        }

        virtual bool slaveOk() const {
            return false;
        }

        virtual LockType locktype() const {
            return NONE;
        }

        virtual void help(stringstream& ss) const {
            ss << "Used to update a role" << endl;
        }

        virtual Status checkAuthForCommand(ClientBasic* client,
                                           const std::string& dbname,
                                           const BSONObj& cmdObj) {
            AuthorizationSession* authzSession = client->getAuthorizationSession();
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

        bool run(const string& dbname,
                 BSONObj& cmdObj,
                 int options,
                 string& errmsg,
                 BSONObjBuilder& result,
                 bool fromRepl) {
            auth::CreateOrUpdateRoleArgs args;
            Status status = auth::parseCreateOrUpdateRoleCommands(cmdObj,
                                                                  "updateRole",
                                                                  dbname,
                                                                  &args);
            if (!status.isOK()) {
                addStatus(status, result);
                return false;
            }

            if (!args.hasPrivileges && !args.hasRoles) {
                addStatus(Status(ErrorCodes::BadValue,
                                 "Must specify at least one field to update in updateRole"),
                          result);
                return false;
            }

            BSONObjBuilder updateSetBuilder;

            if (args.hasPrivileges) {
                BSONArray privileges;
                status = privilegeVectorToBSONArray(args.privileges, &privileges);
                if (!status.isOK()) {
                    addStatus(status, result);
                    return false;
                }
                updateSetBuilder.append("privileges", privileges);
            }

            if (args.hasRoles) {
                updateSetBuilder.append("roles", rolesVectorToBSONArray(args.roles));
            }

            AuthorizationManager* authzManager = getGlobalAuthorizationManager();
            AuthzDocumentsUpdateGuard updateGuard(authzManager);
            if (!updateGuard.tryLock("Update role")) {
                addStatus(Status(ErrorCodes::LockBusy, "Could not lock auth data update lock."),
                          result);
                return false;
            }

            status = requireAuthSchemaVersion26Final(authzManager);
            if (!status.isOK()) {
                addStatus(status, result);
                return false;
            }

            // Role existence has to be checked after acquiring the update lock
            BSONObj ignored;
            status = authzManager->getRoleDescription(args.roleName, &ignored);
            if (!status.isOK()) {
                addStatus(status, result);
                return false;
            }

            if (args.hasRoles) {
                status = checkOkayToGrantRolesToRole(args.roleName, args.roles, authzManager);
                if (!status.isOK()) {
                    addStatus(status, result);
                    return false;
                }
            }

            audit::logUpdateRole(ClientBasic::getCurrent(),
                                 args.roleName,
                                 args.hasRoles? &args.roles : NULL,
                                 args.hasPrivileges? &args.privileges : NULL);

            status = authzManager->updateRoleDocument(args.roleName,
                                                      BSON("$set" << updateSetBuilder.done()),
                                                      args.writeConcern);
            if (!status.isOK()) {
                addStatus(status, result);
                return false;
            }

            return true;
        }
    } cmdUpdateRole;

    class CmdGrantPrivilegesToRole: public Command {
    public:

        CmdGrantPrivilegesToRole() : Command("grantPrivilegesToRole") {}

        virtual bool logTheOp() {
            return false;
        }

        virtual bool slaveOk() const {
            return false;
        }

        virtual LockType locktype() const {
            return NONE;
        }

        virtual void help(stringstream& ss) const {
            ss << "Grants privileges to a role" << endl;
        }

        virtual Status checkAuthForCommand(ClientBasic* client,
                                           const std::string& dbname,
                                           const BSONObj& cmdObj) {
            AuthorizationSession* authzSession = client->getAuthorizationSession();
            PrivilegeVector privileges;
            RoleName unusedRoleName;
            BSONObj unusedWriteConcern;
            Status status = auth::parseAndValidateRolePrivilegeManipulationCommands(
                    cmdObj,
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

        bool run(const string& dbname,
                 BSONObj& cmdObj,
                 int options,
                 string& errmsg,
                 BSONObjBuilder& result,
                 bool fromRepl) {
            AuthorizationManager* authzManager = getGlobalAuthorizationManager();
            AuthzDocumentsUpdateGuard updateGuard(authzManager);
            if (!updateGuard.tryLock("Grant privileges to role")) {
                addStatus(Status(ErrorCodes::LockBusy, "Could not lock auth data update lock."),
                          result);
                return false;
            }

            Status status = requireAuthSchemaVersion26Final(authzManager);
            if (!status.isOK()) {
                addStatus(status, result);
                return false;
            }

            RoleName roleName;
            PrivilegeVector privilegesToAdd;
            BSONObj writeConcern;
            status = auth::parseAndValidateRolePrivilegeManipulationCommands(
                    cmdObj,
                    "grantPrivilegesToRole",
                    dbname,
                    &roleName,
                    &privilegesToAdd,
                    &writeConcern);
            if (!status.isOK()) {
                addStatus(status, result);
                return false;
            }

            if (RoleGraph::isBuiltinRole(roleName)) {
                addStatus(Status(ErrorCodes::InvalidRoleModification,
                                 str::stream() << roleName.getFullName() <<
                                         " is a built-in role and cannot be modified."),
                          result);
                return false;
            }

            BSONObj roleDoc;
            status = authzManager->getRoleDescription(roleName, &roleDoc);
            if (!status.isOK()) {
                addStatus(status, result);
                return false;
            }

            PrivilegeVector privileges;
            status = auth::parseAndValidatePrivilegeArray(BSONArray(roleDoc["privileges"].Obj()),
                                                          &privileges);

            if (!status.isOK()) {
                addStatus(status, result);
                return false;
            }

            for (PrivilegeVector::iterator it = privilegesToAdd.begin();
                    it != privilegesToAdd.end(); ++it) {
                Privilege::addPrivilegeToPrivilegeVector(&privileges, *it);
            }

            // Build up update modifier object to $set privileges.
            mutablebson::Document updateObj;
            mutablebson::Element setElement = updateObj.makeElementObject("$set");
            status = updateObj.root().pushBack(setElement);
            if (!status.isOK()) {
                addStatus(status, result);
                return false;
            }
            mutablebson::Element privilegesElement = updateObj.makeElementArray("privileges");
            status = setElement.pushBack(privilegesElement);
            if (!status.isOK()) {
                addStatus(status, result);
                return false;
            }
            status = authzManager->getBSONForPrivileges(privileges, privilegesElement);
            if (!status.isOK()) {
                addStatus(status, result);
                return false;
            }

            BSONObjBuilder updateBSONBuilder;
            updateObj.writeTo(&updateBSONBuilder);

            audit::logGrantPrivilegesToRole(ClientBasic::getCurrent(),
                                            roleName,
                                            privilegesToAdd);

            status = authzManager->updateRoleDocument(
                    roleName,
                    updateBSONBuilder.done(),
                    writeConcern);
            if (!status.isOK()) {
                addStatus(status, result);
                return false;
            }

            return true;
        }

    } cmdGrantPrivilegesToRole;

    class CmdRevokePrivilegesFromRole: public Command {
    public:

        CmdRevokePrivilegesFromRole() : Command("revokePrivilegesFromRole") {}

        virtual bool logTheOp() {
            return false;
        }

        virtual bool slaveOk() const {
            return false;
        }

        virtual LockType locktype() const {
            return NONE;
        }

        virtual void help(stringstream& ss) const {
            ss << "Revokes privileges from a role" << endl;
        }

        virtual Status checkAuthForCommand(ClientBasic* client,
                                           const std::string& dbname,
                                           const BSONObj& cmdObj) {
            AuthorizationSession* authzSession = client->getAuthorizationSession();
            PrivilegeVector privileges;
            RoleName unusedRoleName;
            BSONObj unusedWriteConcern;
            Status status = auth::parseAndValidateRolePrivilegeManipulationCommands(
                    cmdObj,
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

        bool run(const string& dbname,
                 BSONObj& cmdObj,
                 int options,
                 string& errmsg,
                 BSONObjBuilder& result,
                 bool fromRepl) {
            AuthorizationManager* authzManager = getGlobalAuthorizationManager();
            AuthzDocumentsUpdateGuard updateGuard(authzManager);
            if (!updateGuard.tryLock("Revoke privileges from role")) {
                addStatus(Status(ErrorCodes::LockBusy, "Could not lock auth data update lock."),
                          result);
                return false;
            }

            Status status = requireAuthSchemaVersion26Final(authzManager);
            if (!status.isOK()) {
                addStatus(status, result);
                return false;
            }

            RoleName roleName;
            PrivilegeVector privilegesToRemove;
            BSONObj writeConcern;
            status = auth::parseAndValidateRolePrivilegeManipulationCommands(
                    cmdObj,
                    "revokePrivilegesFromRole",
                    dbname,
                    &roleName,
                    &privilegesToRemove,
                    &writeConcern);
            if (!status.isOK()) {
                addStatus(status, result);
                return false;
            }

            if (RoleGraph::isBuiltinRole(roleName)) {
                addStatus(Status(ErrorCodes::InvalidRoleModification,
                                 str::stream() << roleName.getFullName() <<
                                         " is a built-in role and cannot be modified."),
                          result);
                return false;
            }

            BSONObj roleDoc;
            status = authzManager->getRoleDescription(roleName, &roleDoc);
            if (!status.isOK()) {
                addStatus(status, result);
                return false;
            }

            PrivilegeVector privileges;
            status = auth::parseAndValidatePrivilegeArray(BSONArray(roleDoc["privileges"].Obj()),
                                                          &privileges);
            if (!status.isOK()) {
                addStatus(status, result);
                return false;
            }

            for (PrivilegeVector::iterator itToRm = privilegesToRemove.begin();
                    itToRm != privilegesToRemove.end(); ++itToRm) {
                for (PrivilegeVector::iterator curIt = privileges.begin();
                        curIt != privileges.end(); ++curIt) {
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
            if (!status.isOK()) {
                addStatus(status, result);
                return false;
            }
            mutablebson::Element privilegesElement = updateObj.makeElementArray("privileges");
            status = setElement.pushBack(privilegesElement);
            if (!status.isOK()) {
                addStatus(status, result);
                return false;
            }
            status = authzManager->getBSONForPrivileges(privileges, privilegesElement);
            if (!status.isOK()) {
                addStatus(status, result);
                return false;
            }

            audit::logRevokePrivilegesFromRole(ClientBasic::getCurrent(),
                                               roleName,
                                               privilegesToRemove);

            BSONObjBuilder updateBSONBuilder;
            updateObj.writeTo(&updateBSONBuilder);
            status = authzManager->updateRoleDocument(
                    roleName,
                    updateBSONBuilder.done(),
                    writeConcern);
            if (!status.isOK()) {
                addStatus(status, result);
                return false;
            }

            return true;
        }

    } cmdRevokePrivilegesFromRole;

    class CmdGrantRolesToRole: public Command {
    public:

        CmdGrantRolesToRole() : Command("grantRolesToRole") {}

        virtual bool logTheOp() {
            return false;
        }

        virtual bool slaveOk() const {
            return false;
        }

        virtual LockType locktype() const {
            return NONE;
        }

        virtual void help(stringstream& ss) const {
            ss << "Grants roles to another role." << endl;
        }

        virtual Status checkAuthForCommand(ClientBasic* client,
                                           const std::string& dbname,
                                           const BSONObj& cmdObj) {
            AuthorizationSession* authzSession = client->getAuthorizationSession();
            std::vector<RoleName> roles;
            std::string unusedUserNameString;
            BSONObj unusedWriteConcern;
            Status status = auth::parseRolePossessionManipulationCommands(cmdObj,
                                                                          "grantRolesToRole",
                                                                          "grantedRoles",
                                                                          dbname,
                                                                          &unusedUserNameString,
                                                                          &roles,
                                                                          &unusedWriteConcern);
            if (!status.isOK()) {
                return status;
            }

            return checkAuthorizedToGrantRoles(authzSession, roles);
        }

        bool run(const string& dbname,
                 BSONObj& cmdObj,
                 int options,
                 string& errmsg,
                 BSONObjBuilder& result,
                 bool fromRepl) {
            std::string roleNameString;
            std::vector<RoleName> rolesToAdd;
            BSONObj writeConcern;
            Status status = auth::parseRolePossessionManipulationCommands(
                    cmdObj,
                    "grantRolesToRole",
                    "grantedRoles",
                    dbname,
                    &roleNameString,
                    &rolesToAdd,
                    &writeConcern);
            if (!status.isOK()) {
                addStatus(status, result);
                return false;
            }

            RoleName roleName(roleNameString, dbname);
            if (RoleGraph::isBuiltinRole(roleName)) {
                addStatus(Status(ErrorCodes::InvalidRoleModification,
                                 str::stream() << roleName.getFullName() <<
                                         " is a built-in role and cannot be modified."),
                          result);
                return false;
            }

            AuthorizationManager* authzManager = getGlobalAuthorizationManager();
            AuthzDocumentsUpdateGuard updateGuard(authzManager);
            if (!updateGuard.tryLock("Grant roles to role")) {
                addStatus(Status(ErrorCodes::LockBusy, "Could not lock auth data update lock."),
                          result);
                return false;
            }

            status = requireAuthSchemaVersion26Final(authzManager);
            if (!status.isOK()) {
                addStatus(status, result);
                return false;
            }

            // Role existence has to be checked after acquiring the update lock
            BSONObj roleDoc;
            status = authzManager->getRoleDescription(roleName, &roleDoc);
            if (!status.isOK()) {
                addStatus(status, result);
                return false;
            }

            // Check for cycles
            status = checkOkayToGrantRolesToRole(roleName, rolesToAdd, authzManager);
            if (!status.isOK()) {
                addStatus(status, result);
                return false;
            }

            // Add new roles to existing roles
            std::vector<RoleName> directRoles;
            status = auth::parseRoleNamesFromBSONArray(BSONArray(roleDoc["roles"].Obj()),
                                                       roleName.getDB(),
                                                       &directRoles);
            if (!status.isOK()) {
                addStatus(status, result);
                return false;
            }
            for (vector<RoleName>::iterator it = rolesToAdd.begin(); it != rolesToAdd.end(); ++it) {
                const RoleName& roleToAdd = *it;
                if (!sequenceContains(directRoles, roleToAdd)) // Don't double-add role
                    directRoles.push_back(*it);
            }

            audit::logGrantRolesToRole(ClientBasic::getCurrent(),
                                       roleName,
                                       directRoles);

            status = authzManager->updateRoleDocument(
                    roleName,
                    BSON("$set" << BSON("roles" << rolesVectorToBSONArray(directRoles))),
                    writeConcern);
            if (!status.isOK()) {
                addStatus(status, result);
                return false;
            }
            return true;
        }

    } cmdGrantRolesToRole;

    class CmdRevokeRolesFromRole: public Command {
    public:

        CmdRevokeRolesFromRole() : Command("revokeRolesFromRole") {}

        virtual bool logTheOp() {
            return false;
        }

        virtual bool slaveOk() const {
            return false;
        }

        virtual LockType locktype() const {
            return NONE;
        }

        virtual void help(stringstream& ss) const {
            ss << "Revokes roles from another role." << endl;
        }

        virtual Status checkAuthForCommand(ClientBasic* client,
                                           const std::string& dbname,
                                           const BSONObj& cmdObj) {
            AuthorizationSession* authzSession = client->getAuthorizationSession();
            std::vector<RoleName> roles;
            std::string unusedUserNameString;
            BSONObj unusedWriteConcern;
            Status status = auth::parseRolePossessionManipulationCommands(cmdObj,
                                                                          "revokeRolesFromRole",
                                                                          "revokedRoles",
                                                                          dbname,
                                                                          &unusedUserNameString,
                                                                          &roles,
                                                                          &unusedWriteConcern);
            if (!status.isOK()) {
                return status;
            }

            return checkAuthorizedToRevokeRoles(authzSession, roles);
        }

        bool run(const string& dbname,
                 BSONObj& cmdObj,
                 int options,
                 string& errmsg,
                 BSONObjBuilder& result,
                 bool fromRepl) {
            AuthorizationManager* authzManager = getGlobalAuthorizationManager();
            AuthzDocumentsUpdateGuard updateGuard(authzManager);
            if (!updateGuard.tryLock("Revoke roles from role")) {
                addStatus(Status(ErrorCodes::LockBusy, "Could not lock auth data update lock."),
                          result);
                return false;
            }

            Status status = requireAuthSchemaVersion26Final(authzManager);
            if (!status.isOK()) {
                addStatus(status, result);
                return false;
            }

            std::string roleNameString;
            std::vector<RoleName> rolesToRemove;
            BSONObj writeConcern;
            status = auth::parseRolePossessionManipulationCommands(cmdObj,
                                                                   "revokeRolesFromRole",
                                                                   "revokedRoles",
                                                                   dbname,
                                                                   &roleNameString,
                                                                   &rolesToRemove,
                                                                   &writeConcern);
            if (!status.isOK()) {
                addStatus(status, result);
                return false;
            }

            RoleName roleName(roleNameString, dbname);
            if (RoleGraph::isBuiltinRole(roleName)) {
                addStatus(Status(ErrorCodes::InvalidRoleModification,
                                 str::stream() << roleName.getFullName() <<
                                         " is a built-in role and cannot be modified."),
                          result);
                return false;
            }

            BSONObj roleDoc;
            status = authzManager->getRoleDescription(roleName, &roleDoc);
            if (!status.isOK()) {
                addStatus(status, result);
                return false;
            }

            std::vector<RoleName> roles;
            status = auth::parseRoleNamesFromBSONArray(BSONArray(roleDoc["roles"].Obj()),
                                                       roleName.getDB(),
                                                       &roles);
            if (!status.isOK()) {
                addStatus(status, result);
                return false;
            }

            for (vector<RoleName>::const_iterator it = rolesToRemove.begin();
                    it != rolesToRemove.end(); ++it) {
                vector<RoleName>::iterator itToRm = std::find(roles.begin(), roles.end(), *it);
                if (itToRm != roles.end()) {
                    roles.erase(itToRm);
                }
            }

            audit::logRevokeRolesFromRole(ClientBasic::getCurrent(),
                                          roleName,
                                          roles);

            status = authzManager->updateRoleDocument(
                    roleName,
                    BSON("$set" << BSON("roles" << rolesVectorToBSONArray(roles))),
                    writeConcern);
            if (!status.isOK()) {
                addStatus(status, result);
                return false;
            }
            return true;
        }

    } cmdRevokeRolesFromRole;

    class CmdDropRole: public Command {
    public:

        CmdDropRole() : Command("dropRole") {}

        virtual bool logTheOp() {
            return false;
        }

        virtual bool slaveOk() const {
            return false;
        }

        virtual LockType locktype() const {
            return NONE;
        }

        virtual void help(stringstream& ss) const {
            ss << "Drops a single role.  Before deleting the role completely it must remove it "
                  "from any users or roles that reference it.  If any errors occur in the middle "
                  "of that process it's possible to be left in a state where the role has been "
                  "removed from some user/roles but otherwise still exists."<< endl;
        }

        virtual Status checkAuthForCommand(ClientBasic* client,
                                           const std::string& dbname,
                                           const BSONObj& cmdObj) {
            AuthorizationSession* authzSession = client->getAuthorizationSession();
            RoleName roleName;
            BSONObj unusedWriteConcern;
            Status status = auth::parseDropRoleCommand(cmdObj,
                                                         dbname,
                                                         &roleName,
                                                         &unusedWriteConcern);

            if (!authzSession->isAuthorizedForActionsOnResource(
                    ResourcePattern::forDatabaseName(roleName.getDB()), ActionType::dropRole)) {
                return Status(ErrorCodes::Unauthorized,
                              str::stream() << "Not authorized to drop roles from the " <<
                                      roleName.getDB() << " database");
            }
            return Status::OK();
        }

        bool run(const string& dbname,
                 BSONObj& cmdObj,
                 int options,
                 string& errmsg,
                 BSONObjBuilder& result,
                 bool fromRepl) {
            AuthorizationManager* authzManager = getGlobalAuthorizationManager();
            AuthzDocumentsUpdateGuard updateGuard(authzManager);
            if (!updateGuard.tryLock("Drop role")) {
                addStatus(Status(ErrorCodes::LockBusy, "Could not lock auth data update lock."),
                          result);
                return false;
            }

            Status status = requireAuthSchemaVersion26Final(authzManager);
            if (!status.isOK()) {
                addStatus(status, result);
                return false;
            }

            RoleName roleName;
            BSONObj writeConcern;
            status = auth::parseDropRoleCommand(cmdObj,
                                                dbname,
                                                &roleName,
                                                &writeConcern);
            if (!status.isOK()) {
                addStatus(status, result);
                return false;
            }

            if (RoleGraph::isBuiltinRole(roleName)) {
                addStatus(Status(ErrorCodes::InvalidRoleModification,
                                 str::stream() << roleName.getFullName() <<
                                         " is a built-in role and cannot be modified."),
                          result);
                return false;
            }

            BSONObj roleDoc;
            status = authzManager->getRoleDescription(roleName, &roleDoc);
            if (!status.isOK()) {
                addStatus(status, result);
                return false;
            }

            // Remove this role from all users
            int numUpdated;
            status = authzManager->updateAuthzDocuments(
                    NamespaceString("admin.system.users"),
                    BSON("roles" << BSON("$elemMatch" <<
                                         BSON(AuthorizationManager::ROLE_NAME_FIELD_NAME <<
                                              roleName.getRole() <<
                                              AuthorizationManager::ROLE_SOURCE_FIELD_NAME <<
                                              roleName.getDB()))),
                    BSON("$pull" << BSON("roles" <<
                                         BSON(AuthorizationManager::ROLE_NAME_FIELD_NAME <<
                                              roleName.getRole() <<
                                              AuthorizationManager::ROLE_SOURCE_FIELD_NAME <<
                                              roleName.getDB()))),
                    false,
                    true,
                    writeConcern,
                    &numUpdated);
            if (!status.isOK()) {
                ErrorCodes::Error code = status.code() == ErrorCodes::UnknownError ?
                        ErrorCodes::UserModificationFailed : status.code();
                addStatus(Status(code,
                                 str::stream() << "Failed to remove role " << roleName.getFullName()
                                         << " from all users: " << status.reason()),
                          result);
                return false;
            }

            // Remove this role from all other roles
            status = authzManager->updateAuthzDocuments(
                    NamespaceString("admin.system.roles"),
                    BSON("roles" << BSON("$elemMatch" <<
                                         BSON(AuthorizationManager::ROLE_NAME_FIELD_NAME <<
                                              roleName.getRole() <<
                                              AuthorizationManager::ROLE_SOURCE_FIELD_NAME <<
                                              roleName.getDB()))),
                    BSON("$pull" << BSON("roles" <<
                                         BSON(AuthorizationManager::ROLE_NAME_FIELD_NAME <<
                                              roleName.getRole() <<
                                              AuthorizationManager::ROLE_SOURCE_FIELD_NAME <<
                                              roleName.getDB()))),
                    false,
                    true,
                    writeConcern,
                    &numUpdated);
            if (!status.isOK()) {
                ErrorCodes::Error code = status.code() == ErrorCodes::UnknownError ?
                        ErrorCodes::RoleModificationFailed : status.code();
                addStatus(Status(code,
                                 str::stream() << "Removed role " << roleName.getFullName() <<
                                         " from all users but failed to remove from all roles: " <<
                                         status.reason()),
                          result);
                return false;
            }

            audit::logDropRole(ClientBasic::getCurrent(),
                               roleName);
            // Finally, remove the actual role document
            status = authzManager->removeRoleDocuments(
                    BSON(AuthorizationManager::ROLE_NAME_FIELD_NAME << roleName.getRole() <<
                         AuthorizationManager::ROLE_SOURCE_FIELD_NAME << roleName.getDB()),
                    writeConcern,
                    &numUpdated);
            if (!status.isOK()) {
                addStatus(Status(status.code(),
                                 str::stream() << "Removed role " << roleName.getFullName() <<
                                         " from all users and roles but failed to actually delete"
                                         " the role itself: " <<  status.reason()),
                          result);
                return false;
            }

            dassert(numUpdated == 0 || numUpdated == 1);
            if (numUpdated == 0) {
                addStatus(Status(ErrorCodes::RoleNotFound,
                                 str::stream() << "Role '" << roleName.getFullName() <<
                                         "' not found"),
                          result);
                return false;
            }

            return true;
        }

    } cmdDropRole;

    class CmdDropAllRolesFromDatabase: public Command {
    public:

        CmdDropAllRolesFromDatabase() : Command("dropAllRolesFromDatabase") {}

        virtual bool logTheOp() {
            return false;
        }

        virtual bool slaveOk() const {
            return false;
        }

        virtual LockType locktype() const {
            return NONE;
        }

        virtual void help(stringstream& ss) const {
            ss << "Drops all roles from the given database.  Before deleting the roles completely "
                  "it must remove them from any users or other roles that reference them.  If any "
                  "errors occur in the middle of that process it's possible to be left in a state "
                  "where the roles have been removed from some user/roles but otherwise still "
                  "exist." << endl;
        }

        virtual Status checkAuthForCommand(ClientBasic* client,
                                           const std::string& dbname,
                                           const BSONObj& cmdObj) {
            AuthorizationSession* authzSession = client->getAuthorizationSession();
            if (!authzSession->isAuthorizedForActionsOnResource(
                    ResourcePattern::forDatabaseName(dbname), ActionType::dropRole)) {
                return Status(ErrorCodes::Unauthorized,
                              str::stream() << "Not authorized to drop roles from the " <<
                                      dbname << " database");
            }
            return Status::OK();
        }

        bool run(const string& dbname,
                 BSONObj& cmdObj,
                 int options,
                 string& errmsg,
                 BSONObjBuilder& result,
                 bool fromRepl) {
            BSONObj writeConcern;
            Status status = auth::parseDropAllRolesFromDatabaseCommand(cmdObj,
                                                                    dbname,
                                                                    &writeConcern);
            if (!status.isOK()) {
                addStatus(status, result);
                return false;
            }

            AuthorizationManager* authzManager = getGlobalAuthorizationManager();
            AuthzDocumentsUpdateGuard updateGuard(authzManager);
            if (!updateGuard.tryLock("Drop roles from database")) {
                addStatus(Status(ErrorCodes::LockBusy, "Could not lock auth data update lock."),
                          result);
                return false;
            }

            status = requireAuthSchemaVersion26Final(authzManager);
            if (!status.isOK()) {
                addStatus(status, result);
                return false;
            }

            // Remove these roles from all users
            int numUpdated;
            status = authzManager->updateAuthzDocuments(
                    AuthorizationManager::usersCollectionNamespace,
                    BSON("roles" << BSON(AuthorizationManager::ROLE_SOURCE_FIELD_NAME << dbname)),
                    BSON("$pull" << BSON("roles" <<
                                         BSON(AuthorizationManager::ROLE_SOURCE_FIELD_NAME <<
                                              dbname))),
                    false,
                    true,
                    writeConcern,
                    &numUpdated);
            if (!status.isOK()) {
                ErrorCodes::Error code = status.code() == ErrorCodes::UnknownError ?
                        ErrorCodes::UserModificationFailed : status.code();
                addStatus(Status(code,
                                 str::stream() << "Failed to remove roles from \"" << dbname
                                         << "\" db from all users: " << status.reason()),
                          result);
                return false;
            }

            // Remove these roles from all other roles
            std::string sourceFieldName =
                    str::stream() << "roles." << AuthorizationManager::ROLE_SOURCE_FIELD_NAME;
            status = authzManager->updateAuthzDocuments(
                    AuthorizationManager::rolesCollectionNamespace,
                    BSON(sourceFieldName << dbname),
                    BSON("$pull" << BSON("roles" <<
                                         BSON(AuthorizationManager::ROLE_SOURCE_FIELD_NAME <<
                                              dbname))),
                    false,
                    true,
                    writeConcern,
                    &numUpdated);
            if (!status.isOK()) {
                ErrorCodes::Error code = status.code() == ErrorCodes::UnknownError ?
                        ErrorCodes::RoleModificationFailed : status.code();
                addStatus(Status(code,
                                 str::stream() << "Failed to remove roles from \"" << dbname
                                         << "\" db from all roles: " << status.reason()),
                          result);
                return false;
            }

            audit::logDropAllRolesFromDatabase(ClientBasic::getCurrent(), dbname);
            // Finally, remove the actual role documents
            status = authzManager->removeRoleDocuments(
                    BSON(AuthorizationManager::ROLE_SOURCE_FIELD_NAME << dbname),
                    writeConcern,
                    &numUpdated);
            if (!status.isOK()) {
                addStatus(Status(status.code(),
                                 str::stream() << "Removed roles from \"" << dbname << "\" db "
                                         " from all users and roles but failed to actually delete"
                                         " those roles themselves: " <<  status.reason()),
                          result);
                return false;
            }

            result.append("n", numUpdated);

            return true;
        }

    } cmdDropAllRolesFromDatabase;

    class CmdRolesInfo: public Command {
    public:

        virtual bool logTheOp() {
            return false;
        }

        virtual bool slaveOk() const {
            return true;
        }

        virtual LockType locktype() const {
            return NONE;
        }

        CmdRolesInfo() : Command("rolesInfo") {}

        virtual void help(stringstream& ss) const {
            ss << "Returns information about roles." << endl;
        }

        virtual Status checkAuthForCommand(ClientBasic* client,
                                           const std::string& dbname,
                                           const BSONObj& cmdObj) {
            AuthorizationSession* authzSession = client->getAuthorizationSession();
            std::vector<RoleName> roleNames;
            Status status = auth::parseRolesInfoCommand(cmdObj, dbname, &roleNames);
            if (!status.isOK()) {
                return status;
            }

            for (size_t i = 0; i < roleNames.size(); ++i) {
                if (authzSession->isAuthenticatedAsUserWithRole(roleNames[i])) {
                    continue; // Can always see roles that you are a member of
                }

                if (!authzSession->isAuthorizedForActionsOnResource(
                        ResourcePattern::forDatabaseName(roleNames[i].getDB()),
                        ActionType::viewRole)) {
                    return Status(ErrorCodes::Unauthorized,
                                  str::stream() << "Not authorized to view roles from the " <<
                                          dbname << " database");
                }
            }

            return Status::OK();
        }

        bool run(const string& dbname,
                 BSONObj& cmdObj,
                 int options,
                 string& errmsg,
                 BSONObjBuilder& result,
                 bool fromRepl) {

            std::vector<RoleName> roleNames;
            Status status = auth::parseRolesInfoCommand(cmdObj, dbname, &roleNames);
            if (!status.isOK()) {
                addStatus(status, result);
                return false;
            }

            status = requireAuthSchemaVersion26UpgradeOrFinal(getGlobalAuthorizationManager());
            if (!status.isOK()) {
                addStatus(status, result);
                return false;
            }

            BSONArrayBuilder rolesArrayBuilder;
            for (size_t i = 0; i < roleNames.size(); ++i) {
                BSONObj roleDetails;
                status = getGlobalAuthorizationManager()->getRoleDescription(
                        roleNames[i], &roleDetails);
                if (status.code() == ErrorCodes::RoleNotFound) {
                    continue;
                }
                if (!status.isOK()) {
                    addStatus(status, result);
                    return false;
                }
                rolesArrayBuilder.append(roleDetails);
            }
            result.append("roles", rolesArrayBuilder.arr());
            return true;
        }

    } cmdRolesInfo;

    class CmdInvalidateUserCache: public Command {
    public:

        virtual bool logTheOp() {
            return false;
        }

        virtual bool slaveOk() const {
            return true;
        }

        virtual LockType locktype() const {
            return NONE;
        }

        CmdInvalidateUserCache() : Command("invalidateUserCache") {}

        virtual void help(stringstream& ss) const {
            ss << "Invalidates the in-memory cache of user information" << endl;
        }

        virtual Status checkAuthForCommand(ClientBasic* client,
                                           const std::string& dbname,
                                           const BSONObj& cmdObj) {
            AuthorizationSession* authzSession = client->getAuthorizationSession();
            if (!authzSession->isAuthorizedForActionsOnResource(
                    ResourcePattern::forClusterResource(), ActionType::invalidateUserCache)) {
                return Status(ErrorCodes::Unauthorized, "Not authorized to invalidate user cache");
            }
            return Status::OK();
        }

        bool run(const string& dbname,
                 BSONObj& cmdObj,
                 int options,
                 string& errmsg,
                 BSONObjBuilder& result,
                 bool fromRepl) {

            AuthorizationManager* authzManager = getGlobalAuthorizationManager();
            authzManager->invalidateUserCache();
            return true;
        }

    } cmdInvalidateUserCache;
}
