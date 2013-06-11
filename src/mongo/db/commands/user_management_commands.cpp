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
#include "mongo/bson/util/bson_extract.h"
#include "mongo/db/auth/action_set.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/authorization_manager_global.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/commands.h"
#include "mongo/db/jsobj.h"

namespace mongo {

    void addStatus(const Status& status, BSONObjBuilder& builder) {
        builder.append("ok", status.isOK() ? 1.0: 0.0);
        if (!status.isOK())
            builder.append("code", status.code());
        if (!status.reason().empty())
            builder.append("errmsg", status.reason());
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

        // TODO: The bulk of the implementation of this will need to change once we're using the
        // new v2 authorization storage format.
        bool run(const string& dbname,
                 BSONObj& cmdObj,
                 int options,
                 string& errmsg,
                 BSONObjBuilder& result,
                 bool fromRepl) {
            std::string userName;
            std::string password;
            std::string userSource; // TODO: remove this.
            bool readOnly; // TODO: remove this.
            BSONElement extraData;
            BSONElement roles;

            if (cmdObj.hasField("pwd") && cmdObj.hasField("userSource")) {
                errmsg = "User objects can't have both 'pwd' and 'userSource'";
                return false;
            }

            if (!cmdObj.hasField("pwd") && !cmdObj.hasField("userSource")) {
                errmsg = "User objects must have one of 'pwd' and 'userSource'";
                return false;
            }

            if (cmdObj.hasField("roles") && cmdObj.hasField("readOnly")) {
                errmsg = "User objects can't have both 'roles' and 'readOnly'";
                return false;
            }

            Status status = bsonExtractStringField(cmdObj, "user", &userName);
            if (!status.isOK()) {
                addStatus(Status(ErrorCodes::UserModificationFailed,
                                 "\"user\" string not specified"),
                          result);
                return false;
            }

            status = bsonExtractStringFieldWithDefault(cmdObj, "pwd", "", &password);
            if (!status.isOK()) {
                addStatus(Status(ErrorCodes::UserModificationFailed,
                                 "Invalid \"pwd\" string"),
                          result);
                return false;
            }

            status = bsonExtractStringFieldWithDefault(cmdObj, "userSource", "", &userSource);
            if (!status.isOK()) {
                addStatus(Status(ErrorCodes::UserModificationFailed,
                                 "Invalid \"userSource\" string"),
                          result);
                return false;
            }

            status = bsonExtractBooleanFieldWithDefault(cmdObj, "readOnly", false, &readOnly);
            if (!status.isOK()) {
                addStatus(Status(ErrorCodes::UserModificationFailed,
                                 "Invalid \"readOnly\" boolean"),
                          result);
                return false;
            }

            if (cmdObj.hasField("extraData")) {
                status = bsonExtractField(cmdObj, "extraData", &extraData);
                if (!status.isOK()) {
                    addStatus(Status(ErrorCodes::UserModificationFailed,
                                     "Invalid \"extraData\" object"),
                              result);
                    return false;
                }
            }

            if (cmdObj.hasField("roles")) {
                status = bsonExtractField(cmdObj, "roles", &roles);
                if (!status.isOK()) {
                    addStatus(Status(ErrorCodes::UserModificationFailed,
                                     "Invalid \"roles\" array"),
                              result);
                    return false;
                }
            }

            BSONObjBuilder userObjBuilder;
            userObjBuilder.append("user", userName);
            if (cmdObj.hasField("pwd")) {
                // TODO: hash password once we're receiving plaintext passwords here.
                userObjBuilder.append("pwd", password);
            }

            if (cmdObj.hasField("userSource")) {
                userObjBuilder.append("userSource", userSource);
            }

            if (cmdObj.hasField("readOnly")) {
                userObjBuilder.append("readOnly", readOnly);
            }

            if (cmdObj.hasField("extraData")) {
                userObjBuilder.append("extraData", extraData);
            }

            if (cmdObj.hasField("roles")) {
                userObjBuilder.append(roles);
            }

            status = getGlobalAuthorizationManager()->insertPrivilegeDocument(dbname,
                                                                              userObjBuilder.obj());
            if (!status.isOK()) {
                addStatus(status, result);
                return false;
            }

            return true;
        }
    } cmdCreateUser;
}
