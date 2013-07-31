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
#include "mongo/db/commands.h"
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

        struct CreateUserArgs {
            std::string userName;
            std::string clearTextPassword;
            std::string userSource; // TODO(spencer): remove this once we're using v2 user format
            bool readOnly; // TODO(spencer): remove this once we're using the new v2 user format
            BSONObj extraData; // Owned by the owner of the command object used to call createUser
            BSONArray roles; // Owned by the owner of the command object used to call createUser
            // Owned by the owner of the command object used to call createUser
            // TODO(spencer): remove otherDBRoles once we're using the new v2 user format
            BSONObj otherDBRoles;
            bool hasPassword;
            bool hasUserSource;
            bool hasReadOnly;
            bool hasExtraData;
            bool hasRoles;
            bool hasOtherDBRoles;
            CreateUserArgs() : readOnly(false), hasPassword(false), hasUserSource(false),
                    hasReadOnly(false), hasExtraData(false), hasRoles(false),
                    hasOtherDBRoles(false) {}
        };

        // TODO: The bulk of the implementation of this will need to change once we're using the
        // new v2 authorization storage format.
        bool run(const string& dbname,
                 BSONObj& cmdObj,
                 int options,
                 string& errmsg,
                 BSONObjBuilder& result,
                 bool fromRepl) {
            CreateUserArgs args;
            Status status = _parseAndValidateInput(dbname, cmdObj, &args);
            if (!status.isOK()) {
                addStatus(status, result);
                return false;
            }

            std::string password = DBClientWithCommands::createPasswordDigest(
                    args.userName, args.clearTextPassword);


            BSONObjBuilder userObjBuilder;
            userObjBuilder.append("_id", OID::gen());
            userObjBuilder.append("user", args.userName);
            if (args.hasPassword) {
                userObjBuilder.append("pwd", password);
            }

            if (args.hasUserSource) {
                userObjBuilder.append("userSource", args.userSource);
            }

            if (args.hasReadOnly) {
                userObjBuilder.append("readOnly", args.readOnly);
            }

            if (args.hasExtraData) {
                userObjBuilder.append("extraData", args.extraData);
            }

            if (args.hasRoles) {
                userObjBuilder.append("roles", args.roles);
            }

            if (args.hasOtherDBRoles) {
                userObjBuilder.append("otherDBRoles", args.otherDBRoles);
            }

            AuthorizationManager* authzManager = getGlobalAuthorizationManager();
            status = authzManager->insertPrivilegeDocument(dbname, userObjBuilder.obj());
            if (!status.isOK()) {
                addStatus(status, result);
                return false;
            }

            // Rebuild full user cache on every user modification.
            // TODO(spencer): Remove this once we update user cache on-demand for each user
            // modification.
            status = authzManager->initializeAllV1UserData();
            if (!status.isOK()) {
                addStatus(status, result);
                return false;
            }

            return true;
        }

        virtual void redactForLogging(mutablebson::Document* cmdObj) {
            redactPasswordData(cmdObj->root());
        }

    private:

        Status _parseAndValidateInput(const std::string& dbname,
                                      const BSONObj& cmdObj,
                                      CreateUserArgs* parsedArgs) const {
            unordered_set<std::string> validFieldNames;
            validFieldNames.insert("createUser");
            validFieldNames.insert("user");
            validFieldNames.insert("pwd");
            validFieldNames.insert("userSource");
            validFieldNames.insert("roles");
            validFieldNames.insert("readOnly");
            validFieldNames.insert("otherDBRoles");


            // Iterate through all fields in command object and make sure there are no unexpected
            // ones.
            for (BSONObjIterator iter(cmdObj); iter.more(); iter.next()) {
                StringData fieldName = (*iter).fieldNameStringData();
                if (!validFieldNames.count(fieldName.toString())) {
                    return Status(ErrorCodes::BadValue,
                                  mongoutils::str::stream() << "\"" << fieldName << "\" is not "
                                          "a valid argument to createUser");
                }
            }

            Status status = bsonExtractStringField(cmdObj, "user", &parsedArgs->userName);
            if (!status.isOK()) {
                return status;
            }

            if (cmdObj.hasField("pwd")) {
                parsedArgs->hasPassword = true;
                status = bsonExtractStringField(cmdObj, "pwd", &parsedArgs->clearTextPassword);
                if (!status.isOK()) {
                    return status;
                }
            }


            if (cmdObj.hasField("userSource")) {
                parsedArgs->hasUserSource = true;
                status = bsonExtractStringField(cmdObj, "userSource", &parsedArgs->userSource);
                if (!status.isOK()) {
                    return status;
                }
            }

            if (cmdObj.hasField("readOnly")) {
                parsedArgs->hasReadOnly = true;
                status = bsonExtractBooleanField(cmdObj, "readOnly", &parsedArgs->readOnly);
                if (!status.isOK()) {
                    return status;
                }
            }

            if (cmdObj.hasField("extraData")) {
                parsedArgs->hasExtraData = true;
                BSONElement element;
                status = bsonExtractTypedField(cmdObj, "extraData", Object, &element);
                if (!status.isOK()) {
                    return status;
                }
                parsedArgs->extraData = element.Obj();
            }

            if (cmdObj.hasField("roles")) {
                parsedArgs->hasRoles = true;
                BSONElement element;
                status = bsonExtractTypedField(cmdObj, "roles", Array, &element);
                if (!status.isOK()) {
                    return status;
                }
                parsedArgs->roles = BSONArray(element.Obj());
            }

            if (cmdObj.hasField("otherDBRoles")) {
                parsedArgs->hasOtherDBRoles = true;
                BSONElement element;
                status = bsonExtractTypedField(cmdObj, "otherDBRoles", Object, &element);
                if (!status.isOK()) {
                    return status;
                }
                parsedArgs->otherDBRoles = element.Obj();
            }

            if (parsedArgs->hasPassword && parsedArgs->hasUserSource) {
                return Status(ErrorCodes::BadValue,
                              "User objects can't have both 'pwd' and 'userSource'");
            }
            if (!parsedArgs->hasPassword && !parsedArgs->hasUserSource) {
                return Status(ErrorCodes::BadValue,
                              "User objects must have one of 'pwd' and 'userSource'");
            }
            if (parsedArgs->hasRoles && parsedArgs->hasReadOnly) {
                return Status(ErrorCodes::BadValue,
                              "User objects can't have both 'roles' and 'readOnly'");
            }

            // Prevent creating a __system user on the local database, and also prevent creating
            // privilege documents in other datbases for the __system@local user.
            // TODO(spencer): The second part will go away once we use the new V2 user doc format
            // as it doesn't have the same userSource notion.
            if (parsedArgs->userName == internalSecurity.user->getName().getUser() &&
                    ((!parsedArgs->hasUserSource && dbname == "local") ||
                            parsedArgs->userSource == "local")) {
                return Status(ErrorCodes::BadValue,
                              "Cannot create user document for the internal user");
            }

            return Status::OK();
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

        struct UpdateUserArgs {
            std::string userName;
            std::string clearTextPassword;
            BSONObj extraData; // Owned by the owner of the command object given to updateUser
            bool hasPassword;
            bool hasExtraData;
            UpdateUserArgs() : hasPassword(false), hasExtraData(false) {}
        };

        bool run(const string& dbname,
                 BSONObj& cmdObj,
                 int options,
                 string& errmsg,
                 BSONObjBuilder& result,
                 bool fromRepl) {
            UpdateUserArgs args;
            Status status = _parseAndValidateInput(cmdObj, &args);
            if (!status.isOK()) {
                addStatus(status, result);
                return false;
            }

            // TODO: This update will have to change once we're using the new v2 user
            // storage format.
            BSONObjBuilder setBuilder;
            if (args.hasPassword) {
                std::string password = DBClientWithCommands::createPasswordDigest(
                        args.userName, args.clearTextPassword);
                setBuilder.append("pwd", password);
            }
            if (args.hasExtraData) {
                setBuilder.append("extraData", args.extraData);
            }
            BSONObj updateObj = BSON("$set" << setBuilder.obj());

            AuthorizationManager* authzManager = getGlobalAuthorizationManager();
            status = authzManager->updatePrivilegeDocument(UserName(args.userName, dbname),
                                                           updateObj);

            if (!status.isOK()) {
                addStatus(status, result);
                return false;
            }

            // Rebuild full user cache on every user modification.
            // TODO(spencer): Remove this once we update user cache on-demand for each user
            // modification.
            status = authzManager->initializeAllV1UserData();
            if (!status.isOK()) {
                addStatus(status, result);
                return false;
            }

            return true;
        }

        virtual void redactForLogging(mutablebson::Document* cmdObj) {
            redactPasswordData(cmdObj->root());
        }

    private:

        Status _parseAndValidateInput(BSONObj cmdObj, UpdateUserArgs* parsedArgs) const {
            unordered_set<std::string> validFieldNames;
            validFieldNames.insert("updateUser");
            validFieldNames.insert("user");
            validFieldNames.insert("pwd");
            validFieldNames.insert("extraData");

            // Iterate through all fields in command object and make sure there are no
            // unexpected ones.
            for (BSONObjIterator iter(cmdObj); iter.more(); iter.next()) {
                StringData fieldName = (*iter).fieldNameStringData();
                if (!validFieldNames.count(fieldName.toString())) {
                    return Status(ErrorCodes::BadValue,
                                  mongoutils::str::stream() << "\"" << fieldName << "\" is not "
                                          "a valid argument to updateUser");
                }
            }

            Status status = bsonExtractStringField(cmdObj, "user", &parsedArgs->userName);
            if (!status.isOK()) {
                return status;
            }

            if (cmdObj.hasField("pwd")) {
                parsedArgs->hasPassword = true;
                status = bsonExtractStringField(cmdObj, "pwd", &parsedArgs->clearTextPassword);
                if (!status.isOK()) {
                    return status;
                }
            }

            if (cmdObj.hasField("extraData")) {
                parsedArgs->hasExtraData = true;
                BSONElement element;
                status = bsonExtractTypedField(cmdObj, "extraData", Object, &element);
                if (!status.isOK()) {
                    return status;
                }
                parsedArgs->extraData = element.Obj();
            }


            if (!parsedArgs->hasPassword && !parsedArgs->hasExtraData) {
                return Status(ErrorCodes::BadValue,
                              "Must specify at least one of 'pwd' and 'extraData'");
            }
            return Status::OK();
        }
    } cmdUpdateUser;
}
