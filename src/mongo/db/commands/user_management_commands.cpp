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

#include <boost/thread/mutex.hpp>
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
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/authorization_manager_global.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/auth/sasl_options.h"
#include "mongo/db/auth/user.h"
#include "mongo/db/auth/user_document_parser.h"
#include "mongo/db/auth/user_management_commands_parser.h"
#include "mongo/db/client.h"
#include "mongo/db/commands.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/platform/unordered_set.h"
#include "mongo/stdx/functional.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/net/ssl_manager.h"
#include "mongo/util/sequence_util.h"
#include "mongo/util/time_support.h"

namespace mongo {

    namespace str = mongoutils::str;

    using std::endl;
    using std::string;
    using std::stringstream;
    using std::vector;

namespace {

    // Used to obtain mutex that guards modifications to persistent authorization data
    const auto getAuthzDataMutex = ServiceContext::declareDecoration<boost::timed_mutex>();

    const Seconds authzDataMutexAcquisitionTimeout{5};

} // namespace

    class CmdCreateUser : public Command {
    public:

        CmdCreateUser() : Command("createUser") {}

        virtual bool slaveOk() const {
            return false;
        }

        virtual bool isWriteCommandForConfigServer() const { return true; }

        virtual void help(stringstream& ss) const {
            ss << "Adds a user to the system" << endl;
        }

        virtual Status checkAuthForCommand(ClientBasic* client,
                                           const std::string& dbname,
                                           const BSONObj& cmdObj) {
            return auth::checkAuthForCreateUserCommand(client, dbname, cmdObj);
        }

        bool run(OperationContext* txn,
                 const string& dbname,
                 BSONObj& cmdObj,
                 int options,
                 string& errmsg,
                 BSONObjBuilder& result) {
            auth::CreateOrUpdateUserArgs args;
            Status status = auth::parseCreateOrUpdateUserCommands(cmdObj,
                                                                  "createUser",
                                                                  dbname,
                                                                  &args);
            if (!status.isOK()) {
                return appendCommandStatus(result, status);
            }

            if (args.userName.getDB() == "local") {
                return appendCommandStatus(
                        result,
                        Status(ErrorCodes::BadValue, "Cannot create users in the local database"));
            }

            if (!args.hasHashedPassword && args.userName.getDB() != "$external") {
                return appendCommandStatus(
                        result,
                        Status(ErrorCodes::BadValue,
                               "Must provide a 'pwd' field for all user documents, except those"
                               " with '$external' as the user's source db"));
            }

            if ((args.hasHashedPassword) &&
                 args.userName.getDB() == "$external") {
                return appendCommandStatus(
                        result,
                        Status(ErrorCodes::BadValue,
                               "Cannot set the password for users defined on the '$external' "
                                       "database"));
            }

            if (!args.hasRoles) {
                return appendCommandStatus(
                        result,
                        Status(ErrorCodes::BadValue,
                               "\"createUser\" command requires a \"roles\" array"));
            }

#ifdef MONGO_CONFIG_SSL
            if (args.userName.getDB() == "$external" &&
                getSSLManager() &&
                getSSLManager()->getSSLConfiguration()
                    .serverSubjectName == args.userName.getUser()) {
                return appendCommandStatus(
                        result,
                        Status(ErrorCodes::BadValue,
                               "Cannot create an x.509 user with the same "
                               "subjectname as the server"));
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

            ServiceContext* serviceContext = txn->getClient()->getServiceContext();
            AuthorizationManager* authzManager = AuthorizationManager::get(serviceContext);
            int authzVersion;
            status = authzManager->getAuthorizationVersion(txn, &authzVersion);
            if (!status.isOK()) {
                return appendCommandStatus(result, status);
            }

            BSONObjBuilder credentialsBuilder(userObjBuilder.subobjStart("credentials"));
            if (!args.hasHashedPassword) {
                // Must be an external user
                credentialsBuilder.append("external", true);
            }
            else {
                // Add SCRAM credentials for appropriate authSchemaVersions.
                if (authzVersion > AuthorizationManager::schemaVersion26Final) {
                    BSONObj scramCred = scram::generateCredentials(
                            args.hashedPassword,
                            saslGlobalParams.scramIterationCount);
                    credentialsBuilder.append("SCRAM-SHA-1", scramCred);
                }
                else { // Otherwise default to MONGODB-CR.
                    credentialsBuilder.append("MONGODB-CR", args.hashedPassword);
                }
            }
            credentialsBuilder.done();

            if (args.hasCustomData) {
                userObjBuilder.append("customData", args.customData);
            }
            userObjBuilder.append("roles", auth::rolesVectorToBSONArray(args.roles));

            BSONObj userObj = userObjBuilder.obj();
            V2UserDocumentParser parser;
            status = parser.checkValidUserDocument(userObj);
            if (!status.isOK()) {
                return appendCommandStatus(result, status);
            }

            boost::unique_lock<boost::timed_mutex> lk(getAuthzDataMutex(serviceContext),
                                                      authzDataMutexAcquisitionTimeout);
            if (!lk) {
                return appendCommandStatus(
                        result,
                        Status(ErrorCodes::LockBusy, "Could not lock auth data update lock."));
            }

            status = auth::requireAuthSchemaVersion26Final(txn, authzManager);
            if (!status.isOK()) {
                return appendCommandStatus(result, status);
            }

            // Role existence has to be checked after acquiring the update lock
            for (size_t i = 0; i < args.roles.size(); ++i) {
                BSONObj ignored;
                status = authzManager->getRoleDescription(args.roles[i], false, &ignored);
                if (!status.isOK()) {
                    return appendCommandStatus(result, status);
                }
            }

            audit::logCreateUser(ClientBasic::getCurrent(),
                                 args.userName,
                                 args.hasHashedPassword,
                                 args.hasCustomData? &args.customData : NULL,
                                 args.roles);
            status = authzManager->insertPrivilegeDocument(txn,
                                                           dbname,
                                                           userObj,
                                                           args.writeConcern);
            return appendCommandStatus(result, status);
        }

        virtual void redactForLogging(mutablebson::Document* cmdObj) {
            auth::redactPasswordData(cmdObj->root());
        }

    } cmdCreateUser;

    class CmdUpdateUser : public Command {
    public:

        CmdUpdateUser() : Command("updateUser") {}

        virtual bool slaveOk() const {
            return false;
        }

        virtual bool isWriteCommandForConfigServer() const { return true; }

        virtual void help(stringstream& ss) const {
            ss << "Used to update a user, for example to change its password" << endl;
        }

        virtual Status checkAuthForCommand(ClientBasic* client,
                                           const std::string& dbname,
                                           const BSONObj& cmdObj) {
            return auth::checkAuthForUpdateUserCommand(client, dbname, cmdObj);
        }

        bool run(OperationContext* txn, const string& dbname,
                 BSONObj& cmdObj,
                 int options,
                 string& errmsg,
                 BSONObjBuilder& result) {
            auth::CreateOrUpdateUserArgs args;
            Status status = auth::parseCreateOrUpdateUserCommands(cmdObj,
                                                                  "updateUser",
                                                                  dbname,
                                                                  &args);
            if (!status.isOK()) {
                return appendCommandStatus(result, status);
            }

            if (!args.hasHashedPassword && !args.hasCustomData && !args.hasRoles) {
                return appendCommandStatus(
                        result,
                        Status(ErrorCodes::BadValue,
                               "Must specify at least one field to update in updateUser"));
            }

            if (args.hasHashedPassword && args.userName.getDB() == "$external") {
                return appendCommandStatus(
                        result,
                        Status(ErrorCodes::BadValue,
                               "Cannot set the password for users defined on the '$external' "
                                       "database"));
            }

            BSONObjBuilder updateSetBuilder;
            if (args.hasHashedPassword) {
                BSONObjBuilder credentialsBuilder(updateSetBuilder.subobjStart("credentials"));

                AuthorizationManager* authzManager = getGlobalAuthorizationManager();
                int authzVersion;
                Status status = authzManager->getAuthorizationVersion(txn, &authzVersion);
                if (!status.isOK()) {
                    return appendCommandStatus(result, status);
                }

                // Add SCRAM credentials for appropriate authSchemaVersions
                if (authzVersion > AuthorizationManager::schemaVersion26Final) {
                    BSONObj scramCred = scram::generateCredentials(
                            args.hashedPassword,
                            saslGlobalParams.scramIterationCount);
                    credentialsBuilder.append("SCRAM-SHA-1",scramCred);
                }
                else { // Otherwise default to MONGODB-CR
                    credentialsBuilder.append("MONGODB-CR", args.hashedPassword);
                }
                credentialsBuilder.done();
            }
            if (args.hasCustomData) {
                updateSetBuilder.append("customData", args.customData);
            }
            if (args.hasRoles) {
                updateSetBuilder.append("roles", auth::rolesVectorToBSONArray(args.roles));
            }

            ServiceContext* serviceContext = txn->getClient()->getServiceContext();
            boost::unique_lock<boost::timed_mutex> lk(getAuthzDataMutex(serviceContext),
                                                      authzDataMutexAcquisitionTimeout);
            if (!lk) {
                return appendCommandStatus(
                        result,
                        Status(ErrorCodes::LockBusy, "Could not lock auth data update lock."));
            }

            AuthorizationManager* authzManager = AuthorizationManager::get(serviceContext);
            status = auth::requireAuthSchemaVersion26Final(txn, authzManager);
            if (!status.isOK()) {
                return appendCommandStatus(result, status);
            }


            // Role existence has to be checked after acquiring the update lock
            if (args.hasRoles) {
                for (size_t i = 0; i < args.roles.size(); ++i) {
                    BSONObj ignored;
                    status = authzManager->getRoleDescription(args.roles[i], false, &ignored);
                    if (!status.isOK()) {
                        return appendCommandStatus(result, status);
                    }
                }
            }

            audit::logUpdateUser(ClientBasic::getCurrent(),
                                 args.userName,
                                 args.hasHashedPassword,
                                 args.hasCustomData? &args.customData : NULL,
                                 args.hasRoles? &args.roles : NULL);

            status = authzManager->updatePrivilegeDocument(txn,
                                                           args.userName,
                                                           BSON("$set" << updateSetBuilder.done()),
                                                           args.writeConcern);
            // Must invalidate even on bad status - what if the write succeeded but the GLE failed?
            authzManager->invalidateUserByName(args.userName);
            return appendCommandStatus(result, status);
        }

        virtual void redactForLogging(mutablebson::Document* cmdObj) {
            auth::redactPasswordData(cmdObj->root());
        }

    } cmdUpdateUser;

    class CmdDropUser : public Command {
    public:

        CmdDropUser() : Command("dropUser") {}

        virtual bool slaveOk() const {
            return false;
        }

        virtual bool isWriteCommandForConfigServer() const { return true; }

        virtual void help(stringstream& ss) const {
            ss << "Drops a single user." << endl;
        }

        virtual Status checkAuthForCommand(ClientBasic* client,
                                           const std::string& dbname,
                                           const BSONObj& cmdObj) {
            return auth::checkAuthForDropUserCommand(client, dbname, cmdObj);
        }

        bool run(OperationContext* txn, const string& dbname,
                 BSONObj& cmdObj,
                 int options,
                 string& errmsg,
                 BSONObjBuilder& result) {
            ServiceContext* serviceContext = txn->getClient()->getServiceContext();
            boost::unique_lock<boost::timed_mutex> lk(getAuthzDataMutex(serviceContext),
                                                      authzDataMutexAcquisitionTimeout);
            if (!lk) {
                return appendCommandStatus(
                        result,
                        Status(ErrorCodes::LockBusy, "Could not lock auth data update lock."));
            }

            AuthorizationManager* authzManager = AuthorizationManager::get(serviceContext);
            Status status = auth::requireAuthSchemaVersion26Final(txn, authzManager);
            if (!status.isOK()) {
                return appendCommandStatus(result, status);
            }


            UserName userName;
            BSONObj writeConcern;
            status = auth::parseAndValidateDropUserCommand(cmdObj,
                                                           dbname,
                                                           &userName,
                                                           &writeConcern);
            if (!status.isOK()) {
                return appendCommandStatus(result, status);
            }

            int nMatched;

            audit::logDropUser(ClientBasic::getCurrent(), userName);

            status = authzManager->removePrivilegeDocuments(
                    txn,
                    BSON(AuthorizationManager::USER_NAME_FIELD_NAME << userName.getUser() <<
                         AuthorizationManager::USER_DB_FIELD_NAME << userName.getDB()),
                    writeConcern,
                    &nMatched);
            // Must invalidate even on bad status - what if the write succeeded but the GLE failed?
            authzManager->invalidateUserByName(userName);
            if (!status.isOK()) {
                return appendCommandStatus(result, status);
            }

            if (nMatched == 0) {
                return appendCommandStatus(
                        result,
                        Status(ErrorCodes::UserNotFound,
                               str::stream() << "User '" << userName.getFullName() <<
                               "' not found"));
            }

            return true;
        }

    } cmdDropUser;

    class CmdDropAllUsersFromDatabase : public Command {
    public:

        CmdDropAllUsersFromDatabase() : Command("dropAllUsersFromDatabase") {}

        virtual bool slaveOk() const {
            return false;
        }

        virtual bool isWriteCommandForConfigServer() const { return true; }

        virtual void help(stringstream& ss) const {
            ss << "Drops all users for a single database." << endl;
        }

        virtual Status checkAuthForCommand(ClientBasic* client,
                                           const std::string& dbname,
                                           const BSONObj& cmdObj) {
            return auth::checkAuthForDropAllUsersFromDatabaseCommand(client, dbname);
        }

        bool run(OperationContext* txn, const string& dbname,
                 BSONObj& cmdObj,
                 int options,
                 string& errmsg,
                 BSONObjBuilder& result) {
            ServiceContext* serviceContext = txn->getClient()->getServiceContext();
            boost::unique_lock<boost::timed_mutex> lk(getAuthzDataMutex(serviceContext),
                                                      authzDataMutexAcquisitionTimeout);
            if (!lk) {
                return appendCommandStatus(
                        result,
                        Status(ErrorCodes::LockBusy, "Could not lock auth data update lock."));
            }

            AuthorizationManager* authzManager = AuthorizationManager::get(serviceContext);
            Status status = auth::requireAuthSchemaVersion26Final(txn, authzManager);
            if (!status.isOK()) {
                return appendCommandStatus(result, status);
            }

            BSONObj writeConcern;
            status = auth::parseAndValidateDropAllUsersFromDatabaseCommand(cmdObj,
                                                                           dbname,
                                                                           &writeConcern);
            if (!status.isOK()) {
                return appendCommandStatus(result, status);
            }

            int numRemoved;

            audit::logDropAllUsersFromDatabase(ClientBasic::getCurrent(), dbname);

            status = authzManager->removePrivilegeDocuments(
                    txn,
                    BSON(AuthorizationManager::USER_DB_FIELD_NAME << dbname),
                    writeConcern,
                    &numRemoved);
            // Must invalidate even on bad status - what if the write succeeded but the GLE failed?
            authzManager->invalidateUsersFromDB(dbname);
            if (!status.isOK()) {
                return appendCommandStatus(result, status);
            }

            result.append("n", numRemoved);
            return true;
        }

    } cmdDropAllUsersFromDatabase;

    class CmdGrantRolesToUser: public Command {
    public:

        CmdGrantRolesToUser() : Command("grantRolesToUser") {}

        virtual bool slaveOk() const {
            return false;
        }

        virtual bool isWriteCommandForConfigServer() const { return true; }

        virtual void help(stringstream& ss) const {
            ss << "Grants roles to a user." << endl;
        }

        virtual Status checkAuthForCommand(ClientBasic* client,
                                           const std::string& dbname,
                                           const BSONObj& cmdObj) {
            return auth::checkAuthForGrantRolesToUserCommand(client, dbname, cmdObj);
        }

        bool run(OperationContext* txn, const string& dbname,
                 BSONObj& cmdObj,
                 int options,
                 string& errmsg,
                 BSONObjBuilder& result) {
            ServiceContext* serviceContext = txn->getClient()->getServiceContext();
            boost::unique_lock<boost::timed_mutex> lk(getAuthzDataMutex(serviceContext),
                                                      authzDataMutexAcquisitionTimeout);
            if (!lk) {
                return appendCommandStatus(
                        result,
                        Status(ErrorCodes::LockBusy, "Could not lock auth data update lock."));
            }

            AuthorizationManager* authzManager = AuthorizationManager::get(serviceContext);
            Status status = auth::requireAuthSchemaVersion26Final(txn, authzManager);
            if (!status.isOK()) {
                return appendCommandStatus(result, status);
            }

            std::string userNameString;
            std::vector<RoleName> roles;
            BSONObj writeConcern;
            status = auth::parseRolePossessionManipulationCommands(cmdObj,
                                                                   "grantRolesToUser",
                                                                   dbname,
                                                                   &userNameString,
                                                                   &roles,
                                                                   &writeConcern);
            if (!status.isOK()) {
                return appendCommandStatus(result, status);
            }

            UserName userName(userNameString, dbname);
            unordered_set<RoleName> userRoles;
            status = auth::getCurrentUserRoles(txn, authzManager, userName, &userRoles);
            if (!status.isOK()) {
                return appendCommandStatus(result, status);
            }

            for (vector<RoleName>::iterator it = roles.begin(); it != roles.end(); ++it) {
                RoleName& roleName = *it;
                BSONObj roleDoc;
                status = authzManager->getRoleDescription(roleName, false, &roleDoc);
                if (!status.isOK()) {
                    return appendCommandStatus(result, status);
                }

                userRoles.insert(roleName);
            }

            audit::logGrantRolesToUser(ClientBasic::getCurrent(),
                                       userName,
                                       roles);
            BSONArray newRolesBSONArray = auth::roleSetToBSONArray(userRoles);
            status = authzManager->updatePrivilegeDocument(
                    txn, userName, BSON("$set" << BSON("roles" << newRolesBSONArray)), writeConcern);
            // Must invalidate even on bad status - what if the write succeeded but the GLE failed?
            authzManager->invalidateUserByName(userName);
            return appendCommandStatus(result, status);
        }

    } cmdGrantRolesToUser;

    class CmdRevokeRolesFromUser: public Command {
    public:

        CmdRevokeRolesFromUser() : Command("revokeRolesFromUser") {}

        virtual bool slaveOk() const {
            return false;
        }

        virtual bool isWriteCommandForConfigServer() const { return true; }

        virtual void help(stringstream& ss) const {
            ss << "Revokes roles from a user." << endl;
        }

        virtual Status checkAuthForCommand(ClientBasic* client,
                                           const std::string& dbname,
                                           const BSONObj& cmdObj) {
            return auth::checkAuthForRevokeRolesFromUserCommand(client, dbname, cmdObj);
        }

        bool run(OperationContext* txn, const string& dbname,
                 BSONObj& cmdObj,
                 int options,
                 string& errmsg,
                 BSONObjBuilder& result) {
            ServiceContext* serviceContext = txn->getClient()->getServiceContext();
            boost::unique_lock<boost::timed_mutex> lk(getAuthzDataMutex(serviceContext),
                                                      authzDataMutexAcquisitionTimeout);
            if (!lk) {
                return appendCommandStatus(
                        result,
                        Status(ErrorCodes::LockBusy, "Could not lock auth data update lock."));
            }

            AuthorizationManager* authzManager = AuthorizationManager::get(serviceContext);
            Status status = auth::requireAuthSchemaVersion26Final(txn, authzManager);
            if (!status.isOK()) {
                return appendCommandStatus(result, status);
            }

            std::string userNameString;
            std::vector<RoleName> roles;
            BSONObj writeConcern;
            status = auth::parseRolePossessionManipulationCommands(cmdObj,
                                                                   "revokeRolesFromUser",
                                                                   dbname,
                                                                   &userNameString,
                                                                   &roles,
                                                                   &writeConcern);
            if (!status.isOK()) {
                return appendCommandStatus(result, status);
            }

            UserName userName(userNameString, dbname);
            unordered_set<RoleName> userRoles;
            status = auth::getCurrentUserRoles(txn, authzManager, userName, &userRoles);
            if (!status.isOK()) {
                return appendCommandStatus(result, status);
            }

            for (vector<RoleName>::iterator it = roles.begin(); it != roles.end(); ++it) {
                RoleName& roleName = *it;
                BSONObj roleDoc;
                status = authzManager->getRoleDescription(roleName, false, &roleDoc);
                if (!status.isOK()) {
                    return appendCommandStatus(result, status);
                }

                userRoles.erase(roleName);
            }

            audit::logRevokeRolesFromUser(ClientBasic::getCurrent(),
                                          userName,
                                          roles);
            BSONArray newRolesBSONArray = auth::roleSetToBSONArray(userRoles);
            status = authzManager->updatePrivilegeDocument(
                    txn, userName, BSON("$set" << BSON("roles" << newRolesBSONArray)), writeConcern);
            // Must invalidate even on bad status - what if the write succeeded but the GLE failed?
            authzManager->invalidateUserByName(userName);
            return appendCommandStatus(result, status);
        }

    } cmdRevokeRolesFromUser;

    class CmdUsersInfo: public Command {
    public:

        virtual bool slaveOk() const {
            return false;
        }

        virtual bool slaveOverrideOk() const {
            return true;
        }

        virtual bool isWriteCommandForConfigServer() const { return false; }

        CmdUsersInfo() : Command("usersInfo") {}

        virtual void help(stringstream& ss) const {
            ss << "Returns information about users." << endl;
        }

        virtual Status checkAuthForCommand(ClientBasic* client,
                                           const std::string& dbname,
                                           const BSONObj& cmdObj) {
            return auth::checkAuthForUsersInfoCommand(client, dbname, cmdObj);
        }

        bool run(OperationContext* txn, const string& dbname,
                 BSONObj& cmdObj,
                 int options,
                 string& errmsg,
                 BSONObjBuilder& result) {

            auth::UsersInfoArgs args;
            Status status = auth::parseUsersInfoCommand(cmdObj, dbname, &args);
            if (!status.isOK()) {
                return appendCommandStatus(result, status);
            }

            status =
                auth::requireAuthSchemaVersion26UpgradeOrFinal(txn,
                                                               getGlobalAuthorizationManager());
            if (!status.isOK()) {
                return appendCommandStatus(result, status);
            }

            if (args.allForDB && args.showPrivileges) {
                return appendCommandStatus(
                        result,
                        Status(ErrorCodes::IllegalOperation,
                               "Can only get privilege details on exact-match usersInfo "
                               "queries."));
            }

            BSONArrayBuilder usersArrayBuilder;
            if (args.showPrivileges) {
                // If you want privileges you need to call getUserDescription on each user.
                for (size_t i = 0; i < args.userNames.size(); ++i) {
                    BSONObj userDetails;
                    status = getGlobalAuthorizationManager()->getUserDescription(
                            txn, args.userNames[i], &userDetails);
                    if (status.code() == ErrorCodes::UserNotFound) {
                        continue;
                    }
                    if (!status.isOK()) {
                        return appendCommandStatus(result, status);
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
                const stdx::function<void(const BSONObj&)> function = stdx::bind(
                        auth::appendBSONObjToBSONArrayBuilder,
                        &usersArrayBuilder,
                        stdx::placeholders::_1);
                authzManager->queryAuthzDocument(txn,
                                                 AuthorizationManager::usersCollectionNamespace,
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

        virtual bool slaveOk() const {
            return false;
        }

        virtual bool isWriteCommandForConfigServer() const { return true; }

        virtual void help(stringstream& ss) const {
            ss << "Adds a role to the system" << endl;
        }

        virtual Status checkAuthForCommand(ClientBasic* client,
                                           const std::string& dbname,
                                           const BSONObj& cmdObj) {
            return auth::checkAuthForCreateRoleCommand(client, dbname, cmdObj);
        }

        bool run(OperationContext* txn, const string& dbname,
                 BSONObj& cmdObj,
                 int options,
                 string& errmsg,
                 BSONObjBuilder& result) {
            auth::CreateOrUpdateRoleArgs args;
            Status status = auth::parseCreateOrUpdateRoleCommands(cmdObj,
                                                                  "createRole",
                                                                  dbname,
                                                                  &args);
            if (!status.isOK()) {
                return appendCommandStatus(result, status);
            }

            if (args.roleName.getRole().empty()) {
                return appendCommandStatus(
                        result,
                        Status(ErrorCodes::BadValue, "Role name must be non-empty"));
            }

            if (args.roleName.getDB() == "local") {
                return appendCommandStatus(
                        result,
                        Status(ErrorCodes::BadValue, "Cannot create roles in the local database"));
            }

            if (args.roleName.getDB() == "$external") {
                return appendCommandStatus(
                        result,
                        Status(ErrorCodes::BadValue,
                               "Cannot create roles in the $external database"));
            }

            if (!args.hasRoles) {
                return appendCommandStatus(
                        result,
                        Status(ErrorCodes::BadValue,
                               "\"createRole\" command requires a \"roles\" array"));
            }

            if (!args.hasPrivileges) {
                return appendCommandStatus(
                        result,
                        Status(ErrorCodes::BadValue,
                               "\"createRole\" command requires a \"privileges\" array"));
            }

            BSONObjBuilder roleObjBuilder;

            roleObjBuilder.append("_id", str::stream() << args.roleName.getDB() << "." <<
                                          args.roleName.getRole());
            roleObjBuilder.append(AuthorizationManager::ROLE_NAME_FIELD_NAME,
                                  args.roleName.getRole());
            roleObjBuilder.append(AuthorizationManager::ROLE_DB_FIELD_NAME,
                                  args.roleName.getDB());

            BSONArray privileges;
            status = auth::privilegeVectorToBSONArray(args.privileges, &privileges);
            if (!status.isOK()) {
                return appendCommandStatus(result, status);
            }
            roleObjBuilder.append("privileges", privileges);

            roleObjBuilder.append("roles", auth::rolesVectorToBSONArray(args.roles));

            ServiceContext* serviceContext = txn->getClient()->getServiceContext();
            boost::unique_lock<boost::timed_mutex> lk(getAuthzDataMutex(serviceContext),
                                                      authzDataMutexAcquisitionTimeout);
            if (!lk) {
                return appendCommandStatus(
                        result,
                        Status(ErrorCodes::LockBusy, "Could not lock auth data update lock."));
            }

            AuthorizationManager* authzManager = AuthorizationManager::get(serviceContext);
            status = auth::requireAuthSchemaVersion26Final(txn, authzManager);
            if (!status.isOK()) {
                return appendCommandStatus(result, status);
            }

            // Role existence has to be checked after acquiring the update lock
            status = auth::checkOkayToGrantRolesToRole(args.roleName, args.roles, authzManager);
            if (!status.isOK()) {
                return appendCommandStatus(result, status);
            }

            status = auth::checkOkayToGrantPrivilegesToRole(args.roleName, args.privileges);
            if (!status.isOK()) {
                return appendCommandStatus(result, status);
            }

            audit::logCreateRole(ClientBasic::getCurrent(),
                                 args.roleName,
                                 args.roles,
                                 args.privileges);

            status = authzManager->insertRoleDocument(txn, roleObjBuilder.done(), args.writeConcern);
            return appendCommandStatus(result, status);
        }

    } cmdCreateRole;

    class CmdUpdateRole: public Command {
    public:

        CmdUpdateRole() : Command("updateRole") {}

        virtual bool slaveOk() const {
            return false;
        }

        virtual bool isWriteCommandForConfigServer() const { return true; }

        virtual void help(stringstream& ss) const {
            ss << "Used to update a role" << endl;
        }

        virtual Status checkAuthForCommand(ClientBasic* client,
                                           const std::string& dbname,
                                           const BSONObj& cmdObj) {
            return auth::checkAuthForUpdateRoleCommand(client, dbname, cmdObj);
        }

        bool run(OperationContext* txn, const string& dbname,
                 BSONObj& cmdObj,
                 int options,
                 string& errmsg,
                 BSONObjBuilder& result) {
            auth::CreateOrUpdateRoleArgs args;
            Status status = auth::parseCreateOrUpdateRoleCommands(cmdObj,
                                                                  "updateRole",
                                                                  dbname,
                                                                  &args);
            if (!status.isOK()) {
                return appendCommandStatus(result, status);
            }

            if (!args.hasPrivileges && !args.hasRoles) {
                return appendCommandStatus(
                        result,
                        Status(ErrorCodes::BadValue,
                               "Must specify at least one field to update in updateRole"));
            }

            BSONObjBuilder updateSetBuilder;

            if (args.hasPrivileges) {
                BSONArray privileges;
                status = auth::privilegeVectorToBSONArray(args.privileges, &privileges);
                if (!status.isOK()) {
                    return appendCommandStatus(result, status);
                }
                updateSetBuilder.append("privileges", privileges);
            }

            if (args.hasRoles) {
                updateSetBuilder.append("roles", auth::rolesVectorToBSONArray(args.roles));
            }

            ServiceContext* serviceContext = txn->getClient()->getServiceContext();
            boost::unique_lock<boost::timed_mutex> lk(getAuthzDataMutex(serviceContext),
                                                      authzDataMutexAcquisitionTimeout);
            if (!lk) {
                return appendCommandStatus(
                        result,
                        Status(ErrorCodes::LockBusy, "Could not lock auth data update lock."));
            }

            AuthorizationManager* authzManager = AuthorizationManager::get(serviceContext);
            status = auth::requireAuthSchemaVersion26Final(txn, authzManager);
            if (!status.isOK()) {
                return appendCommandStatus(result, status);
            }

            // Role existence has to be checked after acquiring the update lock
            BSONObj ignored;
            status = authzManager->getRoleDescription(args.roleName, false, &ignored);
            if (!status.isOK()) {
                return appendCommandStatus(result, status);
            }

            if (args.hasRoles) {
                status = auth::checkOkayToGrantRolesToRole(args.roleName, args.roles, authzManager);
                if (!status.isOK()) {
                    return appendCommandStatus(result, status);
                }
            }

            if (args.hasPrivileges) {
                status = auth::checkOkayToGrantPrivilegesToRole(args.roleName, args.privileges);
                if (!status.isOK()) {
                    return appendCommandStatus(result, status);
                }
            }

            audit::logUpdateRole(ClientBasic::getCurrent(),
                                 args.roleName,
                                 args.hasRoles? &args.roles : NULL,
                                 args.hasPrivileges? &args.privileges : NULL);

            status = authzManager->updateRoleDocument(txn,
                                                      args.roleName,
                                                      BSON("$set" << updateSetBuilder.done()),
                                                      args.writeConcern);
            // Must invalidate even on bad status - what if the write succeeded but the GLE failed?
            authzManager->invalidateUserCache();
            return appendCommandStatus(result, status);
        }
    } cmdUpdateRole;

    class CmdGrantPrivilegesToRole: public Command {
    public:

        CmdGrantPrivilegesToRole() : Command("grantPrivilegesToRole") {}

        virtual bool slaveOk() const {
            return false;
        }

        virtual bool isWriteCommandForConfigServer() const { return true; }

        virtual void help(stringstream& ss) const {
            ss << "Grants privileges to a role" << endl;
        }

        virtual Status checkAuthForCommand(ClientBasic* client,
                                           const std::string& dbname,
                                           const BSONObj& cmdObj) {
            return auth::checkAuthForGrantPrivilegesToRoleCommand(client, dbname, cmdObj);
        }

        bool run(OperationContext* txn, const string& dbname,
                 BSONObj& cmdObj,
                 int options,
                 string& errmsg,
                 BSONObjBuilder& result) {
            ServiceContext* serviceContext = txn->getClient()->getServiceContext();
            boost::unique_lock<boost::timed_mutex> lk(getAuthzDataMutex(serviceContext),
                                                      authzDataMutexAcquisitionTimeout);
            if (!lk) {
                return appendCommandStatus(
                        result,
                        Status(ErrorCodes::LockBusy, "Could not lock auth data update lock."));
            }

            AuthorizationManager* authzManager = AuthorizationManager::get(serviceContext);
            Status status = auth::requireAuthSchemaVersion26Final(txn, authzManager);
            if (!status.isOK()) {
                return appendCommandStatus(result, status);
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
                return appendCommandStatus(result, status);
            }

            if (RoleGraph::isBuiltinRole(roleName)) {
                return appendCommandStatus(
                        result,
                        Status(ErrorCodes::InvalidRoleModification,
                               str::stream() << roleName.getFullName() <<
                               " is a built-in role and cannot be modified."));
            }

            status = auth::checkOkayToGrantPrivilegesToRole(roleName, privilegesToAdd);
            if (!status.isOK()) {
                return appendCommandStatus(result, status);
            }

            BSONObj roleDoc;
            status = authzManager->getRoleDescription(roleName, true, &roleDoc);
            if (!status.isOK()) {
                return appendCommandStatus(result, status);
            }

            PrivilegeVector privileges;
            status = auth::parseAndValidatePrivilegeArray(BSONArray(roleDoc["privileges"].Obj()),
                                                          &privileges);

            if (!status.isOK()) {
                return appendCommandStatus(result, status);
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
                return appendCommandStatus(result, status);
            }
            mutablebson::Element privilegesElement = updateObj.makeElementArray("privileges");
            status = setElement.pushBack(privilegesElement);
            if (!status.isOK()) {
                return appendCommandStatus(result, status);
            }
            status = authzManager->getBSONForPrivileges(privileges, privilegesElement);
            if (!status.isOK()) {
                return appendCommandStatus(result, status);
            }

            BSONObjBuilder updateBSONBuilder;
            updateObj.writeTo(&updateBSONBuilder);

            audit::logGrantPrivilegesToRole(ClientBasic::getCurrent(),
                                            roleName,
                                            privilegesToAdd);

            status = authzManager->updateRoleDocument(
                    txn,
                    roleName,
                    updateBSONBuilder.done(),
                    writeConcern);
            // Must invalidate even on bad status - what if the write succeeded but the GLE failed?
            authzManager->invalidateUserCache();
            return appendCommandStatus(result, status);
        }

    } cmdGrantPrivilegesToRole;

    class CmdRevokePrivilegesFromRole: public Command {
    public:

        CmdRevokePrivilegesFromRole() : Command("revokePrivilegesFromRole") {}

        virtual bool slaveOk() const {
            return false;
        }

        virtual bool isWriteCommandForConfigServer() const { return true; }

        virtual void help(stringstream& ss) const {
            ss << "Revokes privileges from a role" << endl;
        }

        virtual Status checkAuthForCommand(ClientBasic* client,
                                           const std::string& dbname,
                                           const BSONObj& cmdObj) {
            return auth::checkAuthForRevokePrivilegesFromRoleCommand(client, dbname, cmdObj);
        }

        bool run(OperationContext* txn, const string& dbname,
                 BSONObj& cmdObj,
                 int options,
                 string& errmsg,
                 BSONObjBuilder& result) {
            ServiceContext* serviceContext = txn->getClient()->getServiceContext();
            boost::unique_lock<boost::timed_mutex> lk(getAuthzDataMutex(serviceContext),
                                                      authzDataMutexAcquisitionTimeout);
            if (!lk) {
                return appendCommandStatus(
                        result,
                        Status(ErrorCodes::LockBusy, "Could not lock auth data update lock."));
            }

            AuthorizationManager* authzManager = AuthorizationManager::get(serviceContext);
            Status status = auth::requireAuthSchemaVersion26Final(txn, authzManager);
            if (!status.isOK()) {
                return appendCommandStatus(result, status);
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
                return appendCommandStatus(result, status);
            }

            if (RoleGraph::isBuiltinRole(roleName)) {
                return appendCommandStatus(
                        result,
                        Status(ErrorCodes::InvalidRoleModification,
                               str::stream() << roleName.getFullName() <<
                               " is a built-in role and cannot be modified."));
            }

            BSONObj roleDoc;
            status = authzManager->getRoleDescription(roleName, true, &roleDoc);
            if (!status.isOK()) {
                return appendCommandStatus(result, status);
            }

            PrivilegeVector privileges;
            status = auth::parseAndValidatePrivilegeArray(BSONArray(roleDoc["privileges"].Obj()),
                                                          &privileges);
            if (!status.isOK()) {
                return appendCommandStatus(result, status);
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
                return appendCommandStatus(result, status);
            }
            mutablebson::Element privilegesElement = updateObj.makeElementArray("privileges");
            status = setElement.pushBack(privilegesElement);
            if (!status.isOK()) {
                return appendCommandStatus(result, status);
            }
            status = authzManager->getBSONForPrivileges(privileges, privilegesElement);
            if (!status.isOK()) {
                return appendCommandStatus(result, status);
            }

            audit::logRevokePrivilegesFromRole(ClientBasic::getCurrent(),
                                               roleName,
                                               privilegesToRemove);

            BSONObjBuilder updateBSONBuilder;
            updateObj.writeTo(&updateBSONBuilder);
            status = authzManager->updateRoleDocument(
                    txn,
                    roleName,
                    updateBSONBuilder.done(),
                    writeConcern);
            // Must invalidate even on bad status - what if the write succeeded but the GLE failed?
            authzManager->invalidateUserCache();
            return appendCommandStatus(result, status);
        }

    } cmdRevokePrivilegesFromRole;

    class CmdGrantRolesToRole: public Command {
    public:

        CmdGrantRolesToRole() : Command("grantRolesToRole") {}

        virtual bool slaveOk() const {
            return false;
        }

        virtual bool isWriteCommandForConfigServer() const { return true; }

        virtual void help(stringstream& ss) const {
            ss << "Grants roles to another role." << endl;
        }

        virtual Status checkAuthForCommand(ClientBasic* client,
                                           const std::string& dbname,
                                           const BSONObj& cmdObj) {
            return auth::checkAuthForGrantRolesToRoleCommand(client, dbname, cmdObj);
        }

        bool run(OperationContext* txn, const string& dbname,
                 BSONObj& cmdObj,
                 int options,
                 string& errmsg,
                 BSONObjBuilder& result) {
            std::string roleNameString;
            std::vector<RoleName> rolesToAdd;
            BSONObj writeConcern;
            Status status = auth::parseRolePossessionManipulationCommands(
                    cmdObj,
                    "grantRolesToRole",
                    dbname,
                    &roleNameString,
                    &rolesToAdd,
                    &writeConcern);
            if (!status.isOK()) {
                return appendCommandStatus(result, status);
            }

            RoleName roleName(roleNameString, dbname);
            if (RoleGraph::isBuiltinRole(roleName)) {
                return appendCommandStatus(
                        result,
                        Status(ErrorCodes::InvalidRoleModification,
                               str::stream() << roleName.getFullName() <<
                               " is a built-in role and cannot be modified."));
            }

            ServiceContext* serviceContext = txn->getClient()->getServiceContext();
            boost::unique_lock<boost::timed_mutex> lk(getAuthzDataMutex(serviceContext),
                                                      authzDataMutexAcquisitionTimeout);
            if (!lk) {
                return appendCommandStatus(
                        result,
                        Status(ErrorCodes::LockBusy, "Could not lock auth data update lock."));
            }

            AuthorizationManager* authzManager = AuthorizationManager::get(serviceContext);
            status = auth::requireAuthSchemaVersion26Final(txn, authzManager);
            if (!status.isOK()) {
                return appendCommandStatus(result, status);
            }

            // Role existence has to be checked after acquiring the update lock
            BSONObj roleDoc;
            status = authzManager->getRoleDescription(roleName, false, &roleDoc);
            if (!status.isOK()) {
                return appendCommandStatus(result, status);
            }

            // Check for cycles
            status = auth::checkOkayToGrantRolesToRole(roleName, rolesToAdd, authzManager);
            if (!status.isOK()) {
                return appendCommandStatus(result, status);
            }

            // Add new roles to existing roles
            std::vector<RoleName> directRoles;
            status = auth::parseRoleNamesFromBSONArray(BSONArray(roleDoc["roles"].Obj()),
                                                       roleName.getDB(),
                                                       &directRoles);
            if (!status.isOK()) {
                return appendCommandStatus(result, status);
            }
            for (vector<RoleName>::iterator it = rolesToAdd.begin(); it != rolesToAdd.end(); ++it) {
                const RoleName& roleToAdd = *it;
                if (!sequenceContains(directRoles, roleToAdd)) // Don't double-add role
                    directRoles.push_back(*it);
            }

            audit::logGrantRolesToRole(ClientBasic::getCurrent(),
                                       roleName,
                                       rolesToAdd);

            status = authzManager->updateRoleDocument(
                    txn,
                    roleName,
                    BSON("$set" << BSON("roles" << auth::rolesVectorToBSONArray(directRoles))),
                    writeConcern);
            // Must invalidate even on bad status - what if the write succeeded but the GLE failed?
            authzManager->invalidateUserCache();
            return appendCommandStatus(result, status);
        }

    } cmdGrantRolesToRole;

    class CmdRevokeRolesFromRole: public Command {
    public:

        CmdRevokeRolesFromRole() : Command("revokeRolesFromRole") {}

        virtual bool slaveOk() const {
            return false;
        }

        virtual bool isWriteCommandForConfigServer() const { return true; }

        virtual void help(stringstream& ss) const {
            ss << "Revokes roles from another role." << endl;
        }

        virtual Status checkAuthForCommand(ClientBasic* client,
                                           const std::string& dbname,
                                           const BSONObj& cmdObj) {
            return auth::checkAuthForRevokeRolesFromRoleCommand(client, dbname, cmdObj);
        }

        bool run(OperationContext* txn, const string& dbname,
                 BSONObj& cmdObj,
                 int options,
                 string& errmsg,
                 BSONObjBuilder& result) {
            ServiceContext* serviceContext = txn->getClient()->getServiceContext();
            boost::unique_lock<boost::timed_mutex> lk(getAuthzDataMutex(serviceContext),
                                                      authzDataMutexAcquisitionTimeout);
            if (!lk) {
                return appendCommandStatus(
                        result,
                        Status(ErrorCodes::LockBusy, "Could not lock auth data update lock."));
            }

            AuthorizationManager* authzManager = AuthorizationManager::get(serviceContext);
            Status status = auth::requireAuthSchemaVersion26Final(txn, authzManager);
            if (!status.isOK()) {
                return appendCommandStatus(result, status);
            }

            std::string roleNameString;
            std::vector<RoleName> rolesToRemove;
            BSONObj writeConcern;
            status = auth::parseRolePossessionManipulationCommands(cmdObj,
                                                                   "revokeRolesFromRole",
                                                                   dbname,
                                                                   &roleNameString,
                                                                   &rolesToRemove,
                                                                   &writeConcern);
            if (!status.isOK()) {
                return appendCommandStatus(result, status);
            }

            RoleName roleName(roleNameString, dbname);
            if (RoleGraph::isBuiltinRole(roleName)) {
                return appendCommandStatus(
                        result,
                        Status(ErrorCodes::InvalidRoleModification,
                               str::stream() << roleName.getFullName() <<
                               " is a built-in role and cannot be modified."));
            }

            BSONObj roleDoc;
            status = authzManager->getRoleDescription(roleName, false, &roleDoc);
            if (!status.isOK()) {
                return appendCommandStatus(result, status);
            }

            std::vector<RoleName> roles;
            status = auth::parseRoleNamesFromBSONArray(BSONArray(roleDoc["roles"].Obj()),
                                                       roleName.getDB(),
                                                       &roles);
            if (!status.isOK()) {
                return appendCommandStatus(result, status);
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
                                          rolesToRemove);

            status = authzManager->updateRoleDocument(
                    txn,
                    roleName,
                    BSON("$set" << BSON("roles" << auth::rolesVectorToBSONArray(roles))),
                    writeConcern);
            // Must invalidate even on bad status - what if the write succeeded but the GLE failed?
            authzManager->invalidateUserCache();
            return appendCommandStatus(result, status);
        }

    } cmdRevokeRolesFromRole;

    class CmdDropRole: public Command {
    public:

        CmdDropRole() : Command("dropRole") {}

        virtual bool slaveOk() const {
            return false;
        }

        virtual bool isWriteCommandForConfigServer() const { return true; }

        virtual void help(stringstream& ss) const {
            ss << "Drops a single role.  Before deleting the role completely it must remove it "
                  "from any users or roles that reference it.  If any errors occur in the middle "
                  "of that process it's possible to be left in a state where the role has been "
                  "removed from some user/roles but otherwise still exists."<< endl;
        }

        virtual Status checkAuthForCommand(ClientBasic* client,
                                           const std::string& dbname,
                                           const BSONObj& cmdObj) {
            return auth::checkAuthForDropRoleCommand(client, dbname, cmdObj);
        }

        bool run(OperationContext* txn, const string& dbname,
                 BSONObj& cmdObj,
                 int options,
                 string& errmsg,
                 BSONObjBuilder& result) {
            ServiceContext* serviceContext = txn->getClient()->getServiceContext();
            boost::unique_lock<boost::timed_mutex> lk(getAuthzDataMutex(serviceContext),
                                                      authzDataMutexAcquisitionTimeout);
            if (!lk) {
                return appendCommandStatus(
                        result,
                        Status(ErrorCodes::LockBusy, "Could not lock auth data update lock."));
            }

            AuthorizationManager* authzManager = AuthorizationManager::get(serviceContext);
            Status status = auth::requireAuthSchemaVersion26Final(txn, authzManager);
            if (!status.isOK()) {
                return appendCommandStatus(result, status);
            }

            RoleName roleName;
            BSONObj writeConcern;
            status = auth::parseDropRoleCommand(cmdObj,
                                                dbname,
                                                &roleName,
                                                &writeConcern);
            if (!status.isOK()) {
                return appendCommandStatus(result, status);
            }

            if (RoleGraph::isBuiltinRole(roleName)) {
                return appendCommandStatus(
                        result,
                        Status(ErrorCodes::InvalidRoleModification,
                               str::stream() << roleName.getFullName() <<
                               " is a built-in role and cannot be modified."));
            }

            BSONObj roleDoc;
            status = authzManager->getRoleDescription(roleName, false, &roleDoc);
            if (!status.isOK()) {
                return appendCommandStatus(result, status);
            }

            // Remove this role from all users
            int nMatched;
            status = authzManager->updateAuthzDocuments(
                    txn,
                    NamespaceString("admin.system.users"),
                    BSON("roles" << BSON("$elemMatch" <<
                                         BSON(AuthorizationManager::ROLE_NAME_FIELD_NAME <<
                                              roleName.getRole() <<
                                              AuthorizationManager::ROLE_DB_FIELD_NAME <<
                                              roleName.getDB()))),
                    BSON("$pull" << BSON("roles" <<
                                         BSON(AuthorizationManager::ROLE_NAME_FIELD_NAME <<
                                              roleName.getRole() <<
                                              AuthorizationManager::ROLE_DB_FIELD_NAME <<
                                              roleName.getDB()))),
                    false,
                    true,
                    writeConcern,
                    &nMatched);
            // Must invalidate even on bad status - what if the write succeeded but the GLE failed?
            authzManager->invalidateUserCache();
            if (!status.isOK()) {
                ErrorCodes::Error code = status.code() == ErrorCodes::UnknownError ?
                        ErrorCodes::UserModificationFailed : status.code();
                return appendCommandStatus(
                        result,
                        Status(code,
                               str::stream() << "Failed to remove role " << roleName.getFullName()
                               << " from all users: " << status.reason()));
            }

            // Remove this role from all other roles
            status = authzManager->updateAuthzDocuments(
                    txn,
                    NamespaceString("admin.system.roles"),
                    BSON("roles" << BSON("$elemMatch" <<
                                         BSON(AuthorizationManager::ROLE_NAME_FIELD_NAME <<
                                              roleName.getRole() <<
                                              AuthorizationManager::ROLE_DB_FIELD_NAME <<
                                              roleName.getDB()))),
                    BSON("$pull" << BSON("roles" <<
                                         BSON(AuthorizationManager::ROLE_NAME_FIELD_NAME <<
                                              roleName.getRole() <<
                                              AuthorizationManager::ROLE_DB_FIELD_NAME <<
                                              roleName.getDB()))),
                    false,
                    true,
                    writeConcern,
                    &nMatched);
            // Must invalidate even on bad status - what if the write succeeded but the GLE failed?
            authzManager->invalidateUserCache();
            if (!status.isOK()) {
                ErrorCodes::Error code = status.code() == ErrorCodes::UnknownError ?
                        ErrorCodes::RoleModificationFailed : status.code();
                return appendCommandStatus(
                        result,
                        Status(code,
                               str::stream() << "Removed role " << roleName.getFullName() <<
                               " from all users but failed to remove from all roles: " <<
                               status.reason()));
            }

            audit::logDropRole(ClientBasic::getCurrent(),
                               roleName);
            // Finally, remove the actual role document
            status = authzManager->removeRoleDocuments(
                    txn,
                    BSON(AuthorizationManager::ROLE_NAME_FIELD_NAME << roleName.getRole() <<
                         AuthorizationManager::ROLE_DB_FIELD_NAME << roleName.getDB()),
                    writeConcern,
                    &nMatched);
            // Must invalidate even on bad status - what if the write succeeded but the GLE failed?
            authzManager->invalidateUserCache();
            if (!status.isOK()) {
                return appendCommandStatus(
                        result,
                        Status(status.code(),
                               str::stream() << "Removed role " << roleName.getFullName() <<
                               " from all users and roles but failed to actually delete"
                               " the role itself: " <<  status.reason()));
            }

            dassert(nMatched == 0 || nMatched == 1);
            if (nMatched == 0) {
                return appendCommandStatus(
                        result,
                        Status(ErrorCodes::RoleNotFound,
                               str::stream() << "Role '" << roleName.getFullName() <<
                               "' not found"));
            }

            return true;
        }

    } cmdDropRole;

    class CmdDropAllRolesFromDatabase: public Command {
    public:

        CmdDropAllRolesFromDatabase() : Command("dropAllRolesFromDatabase") {}

        virtual bool slaveOk() const {
            return false;
        }

        virtual bool isWriteCommandForConfigServer() const { return true; }

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
            return auth::checkAuthForDropAllRolesFromDatabaseCommand(client, dbname);
        }

        bool run(OperationContext* txn, const string& dbname,
                 BSONObj& cmdObj,
                 int options,
                 string& errmsg,
                 BSONObjBuilder& result) {
            BSONObj writeConcern;
            Status status = auth::parseDropAllRolesFromDatabaseCommand(cmdObj,
                                                                    dbname,
                                                                    &writeConcern);
            if (!status.isOK()) {
                return appendCommandStatus(result, status);
            }

            ServiceContext* serviceContext = txn->getClient()->getServiceContext();
            boost::unique_lock<boost::timed_mutex> lk(getAuthzDataMutex(serviceContext),
                                                      authzDataMutexAcquisitionTimeout);
            if (!lk) {
                return appendCommandStatus(
                        result,
                        Status(ErrorCodes::LockBusy, "Could not lock auth data update lock."));
            }

            AuthorizationManager* authzManager = AuthorizationManager::get(serviceContext);
            status = auth::requireAuthSchemaVersion26Final(txn, authzManager);
            if (!status.isOK()) {
                return appendCommandStatus(result, status);
            }

            // Remove these roles from all users
            int nMatched;
            status = authzManager->updateAuthzDocuments(
                    txn,
                    AuthorizationManager::usersCollectionNamespace,
                    BSON("roles" << BSON(AuthorizationManager::ROLE_DB_FIELD_NAME << dbname)),
                    BSON("$pull" << BSON("roles" <<
                                         BSON(AuthorizationManager::ROLE_DB_FIELD_NAME <<
                                              dbname))),
                    false,
                    true,
                    writeConcern,
                    &nMatched);
            // Must invalidate even on bad status - what if the write succeeded but the GLE failed?
            authzManager->invalidateUserCache();
            if (!status.isOK()) {
                ErrorCodes::Error code = status.code() == ErrorCodes::UnknownError ?
                        ErrorCodes::UserModificationFailed : status.code();
                return appendCommandStatus(
                        result,
                        Status(code,
                               str::stream() << "Failed to remove roles from \"" << dbname
                               << "\" db from all users: " << status.reason()));
            }

            // Remove these roles from all other roles
            std::string sourceFieldName =
                    str::stream() << "roles." << AuthorizationManager::ROLE_DB_FIELD_NAME;
            status = authzManager->updateAuthzDocuments(
                    txn,
                    AuthorizationManager::rolesCollectionNamespace,
                    BSON(sourceFieldName << dbname),
                    BSON("$pull" << BSON("roles" <<
                                         BSON(AuthorizationManager::ROLE_DB_FIELD_NAME <<
                                              dbname))),
                    false,
                    true,
                    writeConcern,
                    &nMatched);
            // Must invalidate even on bad status - what if the write succeeded but the GLE failed?
            authzManager->invalidateUserCache();
            if (!status.isOK()) {
                ErrorCodes::Error code = status.code() == ErrorCodes::UnknownError ?
                        ErrorCodes::RoleModificationFailed : status.code();
                return appendCommandStatus(
                        result,
                        Status(code,
                               str::stream() << "Failed to remove roles from \"" << dbname
                               << "\" db from all roles: " << status.reason()));
            }

            audit::logDropAllRolesFromDatabase(ClientBasic::getCurrent(), dbname);
            // Finally, remove the actual role documents
            status = authzManager->removeRoleDocuments(
                    txn,
                    BSON(AuthorizationManager::ROLE_DB_FIELD_NAME << dbname),
                    writeConcern,
                    &nMatched);
            // Must invalidate even on bad status - what if the write succeeded but the GLE failed?
            authzManager->invalidateUserCache();
            if (!status.isOK()) {
                return appendCommandStatus(
                        result,
                        Status(status.code(),
                               str::stream() << "Removed roles from \"" << dbname << "\" db "
                               " from all users and roles but failed to actually delete"
                               " those roles themselves: " <<  status.reason()));
            }

            result.append("n", nMatched);

            return true;
        }

    } cmdDropAllRolesFromDatabase;

    class CmdRolesInfo: public Command {
    public:

        virtual bool slaveOk() const {
            return false;
        }

        virtual bool slaveOverrideOk() const {
            return true;
        }

        virtual bool isWriteCommandForConfigServer() const { return false; }

        CmdRolesInfo() : Command("rolesInfo") {}

        virtual void help(stringstream& ss) const {
            ss << "Returns information about roles." << endl;
        }

        virtual Status checkAuthForCommand(ClientBasic* client,
                                           const std::string& dbname,
                                           const BSONObj& cmdObj) {
            return auth::checkAuthForRolesInfoCommand(client, dbname, cmdObj);
        }

        bool run(OperationContext* txn, const string& dbname,
                 BSONObj& cmdObj,
                 int options,
                 string& errmsg,
                 BSONObjBuilder& result) {

            auth::RolesInfoArgs args;
            Status status = auth::parseRolesInfoCommand(cmdObj, dbname, &args);
            if (!status.isOK()) {
                return appendCommandStatus(result, status);
            }

            status =
                auth::requireAuthSchemaVersion26UpgradeOrFinal(txn,
                                                               getGlobalAuthorizationManager());
            if (!status.isOK()) {
                return appendCommandStatus(result, status);
            }

            BSONArrayBuilder rolesArrayBuilder;
            if (args.allForDB) {
                std::vector<BSONObj> rolesDocs;
                status = getGlobalAuthorizationManager()->getRoleDescriptionsForDB(
                        dbname, args.showPrivileges, args.showBuiltinRoles, &rolesDocs);
                if (!status.isOK()) {
                    return appendCommandStatus(result, status);
                }

                for (size_t i = 0; i < rolesDocs.size(); ++i) {
                    rolesArrayBuilder.append(rolesDocs[i]);
                }
            } else {
                for (size_t i = 0; i < args.roleNames.size(); ++i) {
                    BSONObj roleDetails;
                    status = getGlobalAuthorizationManager()->getRoleDescription(
                            args.roleNames[i], args.showPrivileges, &roleDetails);
                    if (status.code() == ErrorCodes::RoleNotFound) {
                        continue;
                    }
                    if (!status.isOK()) {
                        return appendCommandStatus(result, status);
                    }
                    rolesArrayBuilder.append(roleDetails);
                }
            }
            result.append("roles", rolesArrayBuilder.arr());
            return true;
        }

    } cmdRolesInfo;

    class CmdInvalidateUserCache: public Command {
    public:

        virtual bool slaveOk() const {
            return true;
        }

        virtual bool adminOnly() const {
            return true;
        }

        virtual bool isWriteCommandForConfigServer() const { return false; }

        CmdInvalidateUserCache() : Command("invalidateUserCache") {}

        virtual void help(stringstream& ss) const {
            ss << "Invalidates the in-memory cache of user information" << endl;
        }

        virtual Status checkAuthForCommand(ClientBasic* client,
                                           const std::string& dbname,
                                           const BSONObj& cmdObj) {
            return auth::checkAuthForInvalidateUserCacheCommand(client);
        }

        bool run(OperationContext* txn, const string& dbname,
                 BSONObj& cmdObj,
                 int options,
                 string& errmsg,
                 BSONObjBuilder& result) {

            AuthorizationManager* authzManager = getGlobalAuthorizationManager();
            authzManager->invalidateUserCache();
            return true;
        }

    } cmdInvalidateUserCache;

    class CmdGetCacheGeneration: public Command {
    public:

        virtual bool slaveOk() const {
            return true;
        }

        virtual bool adminOnly() const {
            return true;
        }

        virtual bool isWriteCommandForConfigServer() const { return false; }

        CmdGetCacheGeneration() : Command("_getUserCacheGeneration") {}

        virtual void help(stringstream& ss) const {
            ss << "internal" << endl;
        }

        virtual Status checkAuthForCommand(ClientBasic* client,
                                           const std::string& dbname,
                                           const BSONObj& cmdObj) {
            return auth::checkAuthForGetUserCacheGenerationCommand(client);
        }

        bool run(OperationContext* txn,
                 const string& dbname,
                 BSONObj& cmdObj,
                 int options,
                 string& errmsg,
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
    class CmdMergeAuthzCollections : public Command {
    public:

        CmdMergeAuthzCollections() : Command("_mergeAuthzCollections") {}

        virtual bool slaveOk() const {
            return false;
        }

        virtual bool isWriteCommandForConfigServer() const { return true; }

        virtual bool adminOnly() const {
            return true;
        }

        virtual void help(stringstream& ss) const {
            ss << "Internal command used by mongorestore for updating user/role data" << endl;
        }

        virtual Status checkAuthForCommand(ClientBasic* client,
                                           const std::string& dbname,
                                           const BSONObj& cmdObj) {
            return auth::checkAuthForMergeAuthzCollectionsCommand(client, cmdObj);
        }

        static UserName extractUserNameFromBSON(const BSONObj& userObj) {
            std::string name;
            std::string db;
            Status status = bsonExtractStringField(userObj,
                                                   AuthorizationManager::USER_NAME_FIELD_NAME,
                                                   &name);
            uassertStatusOK(status);
            status = bsonExtractStringField(userObj,
                                            AuthorizationManager::USER_DB_FIELD_NAME,
                                            &db);
            uassertStatusOK(status);
            return UserName(name, db);
        }

        /**
         * Extracts the UserName from the user document and adds it to set of existing users.
         * This function is written so it can used with stdx::bind over the result set of a query
         * on admin.system.users to add the user names of all existing users to the "usersToDrop"
         * set used in the command body.
         */
        static void extractAndInsertUserName(unordered_set<UserName>* existingUsers,
                                             const BSONObj& userObj) {

            UserName userName = extractUserNameFromBSON(userObj);
            existingUsers->insert(userName);
        }

        static RoleName extractRoleNameFromBSON(const BSONObj& roleObj) {
            std::string name;
            std::string db;
            Status status = bsonExtractStringField(roleObj,
                                                   AuthorizationManager::ROLE_NAME_FIELD_NAME,
                                                   &name);
            uassertStatusOK(status);
            status = bsonExtractStringField(roleObj,
                                            AuthorizationManager::ROLE_DB_FIELD_NAME,
                                            &db);
            uassertStatusOK(status);
            return RoleName(name, db);
        }

        /**
         * Extracts the RoleName from the role document and adds it to set of existing roles.
         * This function is written so it can used with stdx::bind over the result set of a query
         * on admin.system.roles to add the role names of all existing roles to the "rolesToDrop"
         * set used in the command body.
         */
        static void extractAndInsertRoleName(unordered_set<RoleName>* existingRoles,
                                             const BSONObj& roleObj) {
            RoleName roleName = extractRoleNameFromBSON(roleObj);
            existingRoles->insert(roleName);
        }

        /**
         * Audits the fact that we are creating or updating the user described by userObj.
         */
        static void auditCreateOrUpdateUser(const BSONObj& userObj, bool create) {
            UserName userName = extractUserNameFromBSON(userObj);
            std::vector<RoleName> roles;
            uassertStatusOK(auth::parseRoleNamesFromBSONArray(BSONArray(userObj["roles"].Obj()),
                                                              userName.getDB(),
                                                              &roles));
            BSONObj customData;
            if (userObj.hasField("customData")) {
                customData = userObj["customData"].Obj();
            }

            if (create) {
                audit::logCreateUser(ClientBasic::getCurrent(),
                                     userName,
                                     userObj["credentials"].Obj().hasField("MONGODB-CR"),
                                     userObj.hasField("customData") ? &customData : NULL,
                                     roles);
            } else {
                audit::logUpdateUser(ClientBasic::getCurrent(),
                                     userName,
                                     userObj["credentials"].Obj().hasField("MONGODB-CR"),
                                     userObj.hasField("customData") ? &customData : NULL,
                                     &roles);

            }
        }

        /**
         * Audits the fact that we are creating or updating the role described by roleObj.
         */
        static void auditCreateOrUpdateRole(const BSONObj& roleObj, bool create) {
            RoleName roleName = extractRoleNameFromBSON(roleObj);
            std::vector<RoleName> roles;
            std::vector<Privilege> privileges;
            uassertStatusOK(auth::parseRoleNamesFromBSONArray(BSONArray(roleObj["roles"].Obj()),
                                                              roleName.getDB(),
                                                              &roles));
            uassertStatusOK(auth::parseAndValidatePrivilegeArray(
                    BSONArray(roleObj["privileges"].Obj()), &privileges));
            if (create) {
                audit::logCreateRole(ClientBasic::getCurrent(), roleName, roles, privileges);
            } else {
                audit::logUpdateRole(ClientBasic::getCurrent(), roleName, &roles, &privileges);
            }
        }

        /**
         * Designed to be used with stdx::bind to be called on every user object in the result
         * set of a query over the tempUsersCollection provided to the command.  For each user
         * in the temp collection that is defined on the given db, adds that user to the actual
         * admin.system.users collection.
         * Also removes any users it encounters from the usersToDrop set.
         */
        static void addUser(OperationContext* txn,
                            AuthorizationManager* authzManager,
                            StringData db,
                            bool update,
                            const BSONObj& writeConcern,
                            unordered_set<UserName>* usersToDrop,
                            const BSONObj& userObj) {
            UserName userName = extractUserNameFromBSON(userObj);
            if (!db.empty() && userName.getDB() != db) {
                return;
            }

            if (update && usersToDrop->count(userName)) {
                auditCreateOrUpdateUser(userObj, false);
                Status status = authzManager->updatePrivilegeDocument(txn,
                                                                      userName,
                                                                      userObj,
                                                                      writeConcern);
                if (!status.isOK()) {
                    // Match the behavior of mongorestore to continue on failure
                    warning() << "Could not update user " << userName <<
                        " in _mergeAuthzCollections command: " << status << endl;
                }
            } else {
                auditCreateOrUpdateUser(userObj, true);
                Status status = authzManager->insertPrivilegeDocument(txn,
                                                                      userName.getDB().toString(),
                                                                      userObj,
                                                                      writeConcern);
                if (!status.isOK()) {
                    // Match the behavior of mongorestore to continue on failure
                    warning() << "Could not insert user " << userName <<
                        " in _mergeAuthzCollections command: " << status << endl;
                }
            }
            usersToDrop->erase(userName);
        }

        /**
         * Designed to be used with stdx::bind to be called on every role object in the result
         * set of a query over the tempRolesCollection provided to the command.  For each role
         * in the temp collection that is defined on the given db, adds that role to the actual
         * admin.system.roles collection.
         * Also removes any roles it encounters from the rolesToDrop set.
         */
        static void addRole(OperationContext* txn,
                            AuthorizationManager* authzManager,
                            StringData db,
                            bool update,
                            const BSONObj& writeConcern,
                            unordered_set<RoleName>* rolesToDrop,
                            const BSONObj roleObj) {
            RoleName roleName = extractRoleNameFromBSON(roleObj);
            if (!db.empty() && roleName.getDB() != db) {
                return;
            }

            if (update && rolesToDrop->count(roleName)) {
                auditCreateOrUpdateRole(roleObj, false);
                Status status = authzManager->updateRoleDocument(txn,
                                                                 roleName,
                                                                 roleObj,
                                                                 writeConcern);
                if (!status.isOK()) {
                    // Match the behavior of mongorestore to continue on failure
                    warning() << "Could not update role " << roleName <<
                        " in _mergeAuthzCollections command: " << status << endl;
                }
            } else {
                auditCreateOrUpdateRole(roleObj, true);
                Status status = authzManager->insertRoleDocument(txn, roleObj, writeConcern);
                if (!status.isOK()) {
                    // Match the behavior of mongorestore to continue on failure
                    warning() << "Could not insert role " << roleName <<
                        " in _mergeAuthzCollections command: " << status << endl;
                }
            }
            rolesToDrop->erase(roleName);
        }

        /**
         * Moves all user objects from usersCollName into admin.system.users.  If drop is true,
         * removes any users that were in admin.system.users but not in usersCollName.
         */
        Status processUsers(OperationContext* txn,
                            AuthorizationManager* authzManager,
                            StringData usersCollName,
                            StringData db,
                            bool drop,
                            const BSONObj& writeConcern) {
            // When the "drop" argument has been provided, we use this set to store the users
            // that are currently in the system, and remove from it as we encounter
            // same-named users in the collection we are restoring from.  Once we've fully
            // moved over the temp users collection into its final location, we drop
            // any users that previously existed there but weren't in the temp collection.
            // This is so that we can completely replace the system.users
            // collection with the users from the temp collection, without removing all
            // users at the beginning and thus potentially locking ourselves out by having
            // no users in the whole system for a time.
            unordered_set<UserName> usersToDrop;

            if (drop) {
                // Create map of the users currently in the DB
                BSONObj query = db.empty() ?
                        BSONObj() : BSON(AuthorizationManager::USER_DB_FIELD_NAME << db);
                BSONObj fields = BSON(AuthorizationManager::USER_NAME_FIELD_NAME << 1 <<
                                      AuthorizationManager::USER_DB_FIELD_NAME << 1);

                Status status = authzManager->queryAuthzDocument(
                        txn,
                        AuthorizationManager::usersCollectionNamespace,
                        query,
                        fields,
                        stdx::bind(&CmdMergeAuthzCollections::extractAndInsertUserName,
                                    &usersToDrop,
                                    stdx::placeholders::_1));
                if (!status.isOK()) {
                    return status;
                }
            }

            Status status = authzManager->queryAuthzDocument(
                    txn,
                    NamespaceString(usersCollName),
                    db.empty() ? BSONObj() : BSON(AuthorizationManager::USER_DB_FIELD_NAME << db),
                    BSONObj(),
                    stdx::bind(&CmdMergeAuthzCollections::addUser,
                                txn,
                                authzManager,
                                db,
                                drop,
                                writeConcern,
                                &usersToDrop,
                                stdx::placeholders::_1));
            if (!status.isOK()) {
                return status;
            }

            if (drop) {
                int numRemoved;
                for (unordered_set<UserName>::iterator it = usersToDrop.begin();
                        it != usersToDrop.end(); ++it) {
                    const UserName& userName = *it;
                    audit::logDropUser(ClientBasic::getCurrent(), userName);
                    status = authzManager->removePrivilegeDocuments(
                            txn,
                            BSON(AuthorizationManager::USER_NAME_FIELD_NAME <<
                                 userName.getUser().toString() <<
                                 AuthorizationManager::USER_DB_FIELD_NAME <<
                                 userName.getDB().toString()
                                 ),
                            writeConcern,
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
        Status processRoles(OperationContext* txn,
                            AuthorizationManager* authzManager,
                            StringData rolesCollName,
                            StringData db,
                            bool drop,
                            const BSONObj& writeConcern) {
            // When the "drop" argument has been provided, we use this set to store the roles
            // that are currently in the system, and remove from it as we encounter
            // same-named roles in the collection we are restoring from.  Once we've fully
            // moved over the temp roles collection into its final location, we drop
            // any roles that previously existed there but weren't in the temp collection.
            // This is so that we can completely replace the system.roles
            // collection with the roles from the temp collection, without removing all
            // roles at the beginning and thus potentially locking ourselves out.
            unordered_set<RoleName> rolesToDrop;

            if (drop) {
                // Create map of the roles currently in the DB
                BSONObj query = db.empty() ?
                        BSONObj() : BSON(AuthorizationManager::ROLE_DB_FIELD_NAME << db);
                BSONObj fields = BSON(AuthorizationManager::ROLE_NAME_FIELD_NAME << 1 <<
                                      AuthorizationManager::ROLE_DB_FIELD_NAME << 1);

                Status status = authzManager->queryAuthzDocument(
                        txn,
                        AuthorizationManager::rolesCollectionNamespace,
                        query,
                        fields,
                        stdx::bind(&CmdMergeAuthzCollections::extractAndInsertRoleName,
                                    &rolesToDrop,
                                    stdx::placeholders::_1));
                if (!status.isOK()) {
                    return status;
                }
            }

            Status status = authzManager->queryAuthzDocument(
                    txn,
                    NamespaceString(rolesCollName),
                    db.empty() ?
                            BSONObj() : BSON(AuthorizationManager::ROLE_DB_FIELD_NAME << db),
                    BSONObj(),
                    stdx::bind(&CmdMergeAuthzCollections::addRole,
                                txn,
                                authzManager,
                                db,
                                drop,
                                writeConcern,
                                &rolesToDrop,
                                stdx::placeholders::_1));
            if (!status.isOK()) {
                return status;
            }

            if (drop) {
                int numRemoved;
                for (unordered_set<RoleName>::iterator it = rolesToDrop.begin();
                        it != rolesToDrop.end(); ++it) {
                    const RoleName& roleName = *it;
                    audit::logDropRole(ClientBasic::getCurrent(), roleName);
                    status = authzManager->removeRoleDocuments(
                            txn,
                            BSON(AuthorizationManager::ROLE_NAME_FIELD_NAME <<
                                 roleName.getRole().toString() <<
                                 AuthorizationManager::ROLE_DB_FIELD_NAME <<
                                 roleName.getDB().toString()
                                 ),
                            writeConcern,
                            &numRemoved);
                    if (!status.isOK()) {
                        return status;
                    }
                    dassert(numRemoved == 1);
                }
            }

            return Status::OK();
        }

        bool run(OperationContext* txn, const string& dbname,
                 BSONObj& cmdObj,
                 int options,
                 string& errmsg,
                 BSONObjBuilder& result) {

            auth::MergeAuthzCollectionsArgs args;
            Status status = auth::parseMergeAuthzCollectionsCommand(cmdObj, &args);
            if (!status.isOK()) {
                return appendCommandStatus(result, status);
            }

            if (args.usersCollName.empty() && args.rolesCollName.empty()) {
                return appendCommandStatus(
                        result, Status(ErrorCodes::BadValue,
                                       "Must provide at least one of \"tempUsersCollection\" and "
                                       "\"tempRolescollection\""));
            }

            ServiceContext* serviceContext = txn->getClient()->getServiceContext();
            boost::unique_lock<boost::timed_mutex> lk(getAuthzDataMutex(serviceContext),
                                                      authzDataMutexAcquisitionTimeout);
            if (!lk) {
                return appendCommandStatus(
                        result,
                        Status(ErrorCodes::LockBusy, "Could not lock auth data update lock."));
            }

            AuthorizationManager* authzManager = AuthorizationManager::get(serviceContext);
            status = auth::requireAuthSchemaVersion26Final(txn, authzManager);
            if (!status.isOK()) {
                return appendCommandStatus(result, status);
            }

            if (!args.usersCollName.empty()) {
                Status status = processUsers(txn,
                                             authzManager,
                                             args.usersCollName,
                                             args.db,
                                             args.drop,
                                             args.writeConcern);
                if (!status.isOK()) {
                    return appendCommandStatus(result, status);
                }
            }

            if (!args.rolesCollName.empty()) {
                Status status = processRoles(txn,
                                             authzManager,
                                             args.rolesCollName,
                                             args.db,
                                             args.drop,
                                             args.writeConcern);
                if (!status.isOK()) {
                    return appendCommandStatus(result, status);
                }
            }

            return true;
        }

    } cmdMergeAuthzCollections;

} // namespace mongo
