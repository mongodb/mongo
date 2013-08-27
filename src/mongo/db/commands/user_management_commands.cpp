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
*/

#include <string>
#include <vector>

#include "mongo/base/status.h"
#include "mongo/bson/mutable/algorithm.h"
#include "mongo/bson/mutable/document.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/client/dbclientinterface.h"
#include "mongo/db/auth/action_set.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/authorization_manager_global.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/auth/user.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/user_management_commands_parser.h"
#include "mongo/db/jsobj.h"
#include "mongo/platform/unordered_set.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

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

    class CmdCreateUser : public Command {
    public:

        virtual bool logTheOp() {
            return false;
        }

        virtual bool slaveOk() const {
            return false;
        }

        virtual LockType locktype() const {
            return NONE;
        }

        CmdCreateUser() : Command("createUser") {}

        virtual void help(stringstream& ss) const {
            ss << "Adds a user to the system" << endl;
        }

        virtual void addRequiredPrivileges(const std::string& dbname,
                                           const BSONObj& cmdObj,
                                           std::vector<Privilege>* out) {
            // TODO: update this with the new rules around user creation in 2.6.
            ActionSet actions;
            actions.addAction(ActionType::userAdmin);
            out->push_back(Privilege(dbname, actions));
        }

        bool run(const string& dbname,
                 BSONObj& cmdObj,
                 int options,
                 string& errmsg,
                 BSONObjBuilder& result,
                 bool fromRepl) {
            AuthorizationManager* authzManager = getGlobalAuthorizationManager();
            BSONObj userObj;
            Status status = auth::parseAndValidateCreateUserCommand(cmdObj,
                                                                    dbname,
                                                                    authzManager,
                                                                    &userObj);
            if (!status.isOK()) {
                addStatus(status, result);
                return false;
            }

            status = authzManager->insertPrivilegeDocument(dbname, userObj);
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

        virtual bool logTheOp() {
            return false;
        }

        virtual bool slaveOk() const {
            return false;
        }

        virtual LockType locktype() const {
            return NONE;
        }

        CmdUpdateUser() : Command("updateUser") {}

        virtual void help(stringstream& ss) const {
            ss << "Used to update a user, for example to change its password" << endl;
        }

        virtual void addRequiredPrivileges(const std::string& dbname,
                                           const BSONObj& cmdObj,
                                           std::vector<Privilege>* out) {
            // TODO: update this with the new rules around user creation in 2.6.
            ActionSet actions;
            actions.addAction(ActionType::userAdmin);
            out->push_back(Privilege(dbname, actions));
        }

        bool run(const string& dbname,
                 BSONObj& cmdObj,
                 int options,
                 string& errmsg,
                 BSONObjBuilder& result,
                 bool fromRepl) {
            AuthorizationManager* authzManager = getGlobalAuthorizationManager();
            BSONObj updateObj;
            UserName userName;
            Status status = auth::parseAndValidateUpdateUserCommand(cmdObj,
                                                                    dbname,
                                                                    authzManager,
                                                                    &updateObj,
                                                                    &userName);
            if (!status.isOK()) {
                addStatus(status, result);
                return false;
            }

            status = authzManager->updatePrivilegeDocument(userName, updateObj);
            if (!status.isOK()) {
                addStatus(status, result);
                return false;
            }

            authzManager->invalidateUserByName(userName);
            return true;
        }

        virtual void redactForLogging(mutablebson::Document* cmdObj) {
            redactPasswordData(cmdObj->root());
        }

    } cmdUpdateUser;

    class CmdRemoveUser : public Command {
    public:

        virtual bool logTheOp() {
            return false;
        }

        virtual bool slaveOk() const {
            return false;
        }

        virtual LockType locktype() const {
            return NONE;
        }

        CmdRemoveUser() : Command("removeUser") {}

        virtual void help(stringstream& ss) const {
            ss << "Removes a single user." << endl;
        }

        virtual void addRequiredPrivileges(const std::string& dbname,
                                           const BSONObj& cmdObj,
                                           std::vector<Privilege>* out) {
            // TODO: update this with the new rules around user creation in 2.6.
            ActionSet actions;
            actions.addAction(ActionType::userAdmin);
            out->push_back(Privilege(dbname, actions));
        }

        bool run(const string& dbname,
                 BSONObj& cmdObj,
                 int options,
                 string& errmsg,
                 BSONObjBuilder& result,
                 bool fromRepl) {
            std::string user;

            Status status = bsonExtractStringField(cmdObj, "removeUser", &user);
            if (!status.isOK()) {
                addStatus(status, result);
                return false;
            }

            int numUpdated;
            AuthorizationManager* authzManager = getGlobalAuthorizationManager();
            status = authzManager->removePrivilegeDocuments(
                    BSON(AuthorizationManager::USER_NAME_FIELD_NAME << user <<
                         AuthorizationManager::USER_SOURCE_FIELD_NAME << dbname),
                    &numUpdated);
            if (!status.isOK()) {
                addStatus(status, result);
                return false;
            }

            if (numUpdated == 0) {
                addStatus(Status(ErrorCodes::UserNotFound,
                                 mongoutils::str::stream() << "User '" << user << "@" <<
                                         dbname << "' not found"),
                          result);
                return false;
            }

            authzManager->invalidateUserByName(UserName(user, dbname));
            return true;
        }

    } cmdRemoveUser;

    class CmdRemoveUsersFromDatabase : public Command {
    public:

        virtual bool logTheOp() {
            return false;
        }

        virtual bool slaveOk() const {
            return false;
        }

        virtual LockType locktype() const {
            return NONE;
        }

        CmdRemoveUsersFromDatabase() : Command("removeUsersFromDatabase") {}

        virtual void help(stringstream& ss) const {
            ss << "Removes all users for a single database." << endl;
        }

        virtual void addRequiredPrivileges(const std::string& dbname,
                                           const BSONObj& cmdObj,
                                           std::vector<Privilege>* out) {
            // TODO: update this with the new rules around user creation in 2.6.
            ActionSet actions;
            actions.addAction(ActionType::userAdmin);
            out->push_back(Privilege(dbname, actions));
        }

        bool run(const string& dbname,
                 BSONObj& cmdObj,
                 int options,
                 string& errmsg,
                 BSONObjBuilder& result,
                 bool fromRepl) {
            int numRemoved;
            AuthorizationManager* authzManager = getGlobalAuthorizationManager();
            Status status = authzManager->removePrivilegeDocuments(
                    BSON(AuthorizationManager::USER_SOURCE_FIELD_NAME << dbname),
                    &numRemoved);
            if (!status.isOK()) {
                addStatus(status, result);
                return false;
            }

            result.append("n", numRemoved);

            authzManager->invalidateUsersFromDB(dbname);
            return true;
        }

    } cmdRemoveUsersFromDatabase;
}
