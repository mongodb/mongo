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

#include "mongo/db/auth/user_management_commands_parser.h"

#include <string>
#include <vector>

#include "mongo/base/status.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/bson/mutable/document.h"
#include "mongo/bson/mutable/element.h"
#include "mongo/client/auth_helpers.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/auth/privilege_parser.h"
#include "mongo/db/auth/role_graph.h"
#include "mongo/db/auth/user_document_parser.h"
#include "mongo/db/auth/user_name.h"
#include "mongo/db/jsobj.h"
#include "mongo/platform/unordered_set.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {
namespace auth {

    /**
     * Writes into *writeConcern a BSONObj describing the parameters to getLastError to use for
     * the write confirmation.
     */
    Status _extractWriteConcern(const BSONObj& cmdObj, BSONObj* writeConcern) {
        BSONElement writeConcernElement;
        Status status = bsonExtractTypedField(cmdObj, "writeConcern", Object, &writeConcernElement);
        if (!status.isOK()) {
            if (status.code() == ErrorCodes::NoSuchKey) {
                *writeConcern = BSONObj();
                return Status::OK();
            }
            return status;
        }
        *writeConcern = writeConcernElement.Obj().getOwned();;
        return Status::OK();
    }

    Status _checkNoExtraFields(const BSONObj& cmdObj,
                              const StringData& cmdName,
                              const unordered_set<std::string>& validFieldNames) {
        // Iterate through all fields in command object and make sure there are no unexpected
        // ones.
        for (BSONObjIterator iter(cmdObj); iter.more(); iter.next()) {
            StringData fieldName = (*iter).fieldNameStringData();
            if (!validFieldNames.count(fieldName.toString())) {
                return Status(ErrorCodes::BadValue,
                              mongoutils::str::stream() << "\"" << fieldName << "\" is not "
                                      "a valid argument to " << cmdName);
            }
        }
        return Status::OK();
    }

    /**
     * Takes a BSONArray of name,source pair documents, parses that array and returns (via the
     * output param parsedRoleNames) a list of the role names in the input array.
     * Also validates the input array and returns a non-OK status if there is anything wrong.
     */
    Status _extractRoleNamesFromBSONArray(const BSONArray rolesArray,
                                         const std::string& dbname,
                                         AuthorizationManager* authzManager,
                                         std::vector<RoleName>* parsedRoleNames) {
        for (BSONObjIterator it(rolesArray); it.more(); it.next()) {
            BSONElement element = *it;
            if (element.type() == String) {
                RoleName roleName(element.String(), dbname);
                if (!authzManager->roleExists(roleName)) {
                    return Status(ErrorCodes::RoleNotFound,
                                  mongoutils::str::stream() << roleName.getFullName() <<
                                          " does not name an existing role");
                }
                parsedRoleNames->push_back(roleName);
            } else if (element.type() == Object) {
                BSONObj roleObj = element.Obj();

                std::string roleNameString;
                std::string roleSource;
                Status status = bsonExtractStringField(roleObj, "name", &roleNameString);
                if (!status.isOK()) {
                    return status;
                }
                status = bsonExtractStringField(roleObj, "source", &roleSource);
                if (!status.isOK()) {
                    return status;
                }

                RoleName roleName(roleNameString, roleSource);
                if (!authzManager->roleExists(roleName)) {
                    return Status(ErrorCodes::RoleNotFound,
                                  mongoutils::str::stream() << roleName.getFullName() <<
                                          " does not name an existing role");
                }
                parsedRoleNames->push_back(roleName);
            } else {
                return Status(ErrorCodes::UnsupportedFormat,
                              "Values in 'roles' array must be sub-documents or strings");
            }
        }
        return Status::OK();
    }

    Status parseUserRoleManipulationCommand(const BSONObj& cmdObj,
                                            const StringData& cmdName,
                                            const std::string& dbname,
                                            AuthorizationManager* authzManager,
                                            UserName* parsedUserName,
                                            vector<RoleName>* parsedRoleNames,
                                            BSONObj* parsedWriteConcern) {
        unordered_set<std::string> validFieldNames;
        validFieldNames.insert(cmdName.toString());
        validFieldNames.insert("roles");
        validFieldNames.insert("writeConcern");

        Status status = _checkNoExtraFields(cmdObj, cmdName, validFieldNames);
        if (!status.isOK()) {
            return status;
        }

        status = _extractWriteConcern(cmdObj, parsedWriteConcern);
        if (!status.isOK()) {
            return status;
        }

        std::string userNameStr;
        status = bsonExtractStringField(cmdObj, cmdName, &userNameStr);
        if (!status.isOK()) {
            return status;
        }
        *parsedUserName = UserName(userNameStr, dbname);

        BSONElement rolesElement;
        status = bsonExtractTypedField(cmdObj, "roles", Array, &rolesElement);
        if (!status.isOK()) {
            return status;
        }

        status = _extractRoleNamesFromBSONArray(BSONArray(rolesElement.Obj()),
                                                dbname,
                                                authzManager,
                                                parsedRoleNames);
        if (!status.isOK()) {
            return status;
        }

        if (!parsedRoleNames->size()) {
            return Status(ErrorCodes::BadValue,
                          mongoutils::str::stream() << cmdName << " command requires a non-empty" <<
                                  " roles array");
        }
        return Status::OK();
    }

    /**
     * Validates that the roles array described by rolesElement is valid.
     * Also returns a new roles array (via the modifiedRolesArray output param) where any roles
     * from the input array that were listed as strings have been expanded to a full role document.
     * If includePossessionBools is true then the expanded roles documents will have "hasRole"
     * and "canDelegate" boolean fields (in addition to the "name" and "source" fields which are
     * there either way).
     */
    Status _validateAndModifyRolesArray(const BSONElement& rolesElement,
                                        const std::string& dbname,
                                        AuthorizationManager* authzManager,
                                        bool includePossessionBools,
                                        BSONArray* modifiedRolesArray) {
        BSONArrayBuilder rolesBuilder;

        for (BSONObjIterator it(rolesElement.Obj()); it.more(); it.next()) {
            BSONElement element = *it;
            if (element.type() == String) {
                RoleName roleName(element.String(), dbname);
                if (!authzManager->roleExists(roleName)) {
                    return Status(ErrorCodes::RoleNotFound,
                                  mongoutils::str::stream() << roleName.toString() <<
                                  " does not name an existing role");
                }

                if (includePossessionBools) {
                    rolesBuilder.append(BSON("name" << element.String() <<
                                             "source" << dbname <<
                                             "hasRole" << true <<
                                             "canDelegate" << false));
                } else {
                    rolesBuilder.append(BSON("name" << element.String() <<
                                             "source" << dbname));
                }
            } else if (element.type() == Object) {
                // Check that the role object is valid
                V2UserDocumentParser parser;
                BSONObj roleObj = element.Obj();
                Status status = parser.checkValidRoleObject(roleObj, includePossessionBools);
                if (!status.isOK()) {
                    return status;
                }

                // Check that the role actually exists
                std::string roleNameString;
                std::string roleSource;
                status = bsonExtractStringField(roleObj, "name", &roleNameString);
                if (!status.isOK()) {
                    return status;
                }
                status = bsonExtractStringField(roleObj, "source", &roleSource);
                if (!status.isOK()) {
                    return status;
                }

                RoleName roleName(roleNameString, roleSource);
                if (!authzManager->roleExists(roleName)) {
                    return Status(ErrorCodes::RoleNotFound,
                                  mongoutils::str::stream() << roleName.toString() <<
                                  " does not name an existing role");
                }

                rolesBuilder.append(element);
            } else {
                return Status(ErrorCodes::UnsupportedFormat,
                              "Values in 'roles' array must be sub-documents or strings");
            }
        }

        *modifiedRolesArray = rolesBuilder.arr();
        return Status::OK();
    }

    Status parseAndValidateCreateUserCommand(const BSONObj& cmdObj,
                                             const std::string& dbname,
                                             AuthorizationManager* authzManager,
                                             BSONObj* parsedUserObj,
                                             BSONObj* parsedWriteConcern) {
        unordered_set<std::string> validFieldNames;
        validFieldNames.insert("createUser");
        validFieldNames.insert("customData");
        validFieldNames.insert("pwd");
        validFieldNames.insert("roles");
        validFieldNames.insert("writeConcern");

        Status status = _checkNoExtraFields(cmdObj, "createUser", validFieldNames);
        if (!status.isOK()) {
            return status;
        }

        status = _extractWriteConcern(cmdObj, parsedWriteConcern);
        if (!status.isOK()) {
            return status;
        }

        BSONObjBuilder userObjBuilder;

        // Parse user name
        std::string userName;
        status = bsonExtractStringField(cmdObj, "createUser", &userName);
        if (!status.isOK()) {
            return status;
        }

        // Prevent creating users in the local database
        if (dbname == "local") {
            return Status(ErrorCodes::BadValue, "Cannot create users in the local database");
        }

        userObjBuilder.append("_id", dbname + "." + userName);
        userObjBuilder.append(AuthorizationManager::USER_NAME_FIELD_NAME, userName);
        userObjBuilder.append(AuthorizationManager::USER_SOURCE_FIELD_NAME, dbname);


        // Parse password
        if (cmdObj.hasField("pwd")) {
            std::string clearTextPassword;
            status = bsonExtractStringField(cmdObj, "pwd", &clearTextPassword);
            if (!status.isOK()) {
                return status;
            }

            std::string password = auth::createPasswordDigest(userName, clearTextPassword);
            userObjBuilder.append("credentials", BSON("MONGODB-CR" << password));
        } else {
            if (dbname != "$external") {
                return Status(ErrorCodes::BadValue,
                              "Must provide a 'pwd' field for all user documents, except those"
                                      " with '$external' as the user's source");
            }
        }


        // Parse custom data
        if (cmdObj.hasField("customData")) {
            BSONElement element;
            status = bsonExtractTypedField(cmdObj, "customData", Object, &element);
            if (!status.isOK()) {
                return status;
            }
            userObjBuilder.append("customData", element.Obj());
        }

        // Parse roles
        if (cmdObj.hasField("roles")) {
            BSONElement rolesElement;
            status = bsonExtractTypedField(cmdObj, "roles", Array, &rolesElement);
            if (!status.isOK()) {
                return status;
            }
            BSONArray modifiedRolesArray;
            status = _validateAndModifyRolesArray(rolesElement,
                                                  dbname,
                                                  authzManager,
                                                  true,
                                                  &modifiedRolesArray);
            if (!status.isOK()) {
                return status;
            }

            userObjBuilder.append("roles", modifiedRolesArray);
        }

        *parsedUserObj = userObjBuilder.obj();

        // Make sure document to insert is valid
        V2UserDocumentParser parser;
        status = parser.checkValidUserDocument(*parsedUserObj);
        if (!status.isOK()) {
            return status;
        }

        return Status::OK();
    }


    Status parseAndValidateUpdateUserCommand(const BSONObj& cmdObj,
                                             const std::string& dbname,
                                             AuthorizationManager* authzManager,
                                             BSONObj* parsedUpdateObj,
                                             UserName* parsedUserName,
                                             BSONObj* parsedWriteConcern) {
        unordered_set<std::string> validFieldNames;
        validFieldNames.insert("updateUser");
        validFieldNames.insert("customData");
        validFieldNames.insert("pwd");
        validFieldNames.insert("roles");
        validFieldNames.insert("writeConcern");

        Status status = _checkNoExtraFields(cmdObj, "updateUser", validFieldNames);
        if (!status.isOK()) {
            return status;
        }

        status = _extractWriteConcern(cmdObj, parsedWriteConcern);
        if (!status.isOK()) {
            return status;
        }

        BSONObjBuilder updateSetBuilder;

        // Parse user name
        std::string userName;
        status = bsonExtractStringField(cmdObj, "updateUser", &userName);
        if (!status.isOK()) {
            return status;
        }
        *parsedUserName = UserName(userName, dbname);

        // Parse password
        if (cmdObj.hasField("pwd")) {
            std::string clearTextPassword;
            status = bsonExtractStringField(cmdObj, "pwd", &clearTextPassword);
            if (!status.isOK()) {
                return status;
            }

            std::string password = auth::createPasswordDigest(userName, clearTextPassword);
            updateSetBuilder.append("credentials.MONGODB-CR", password);
        }


        // Parse custom data
        if (cmdObj.hasField("customData")) {
            BSONElement element;
            status = bsonExtractTypedField(cmdObj, "customData", Object, &element);
            if (!status.isOK()) {
                return status;
            }
            updateSetBuilder.append("customData", element.Obj());
        }

        // Parse roles
        if (cmdObj.hasField("roles")) {
            BSONElement rolesElement;
            Status status = bsonExtractTypedField(cmdObj, "roles", Array, &rolesElement);
            if (!status.isOK()) {
                return status;
            }

            BSONArray modifiedRolesObj;
            status = _validateAndModifyRolesArray(rolesElement,
                                                  dbname,
                                                  authzManager,
                                                  true,
                                                  &modifiedRolesObj);
            if (!status.isOK()) {
                return status;
            }

            updateSetBuilder.append("roles", modifiedRolesObj);
        }

        BSONObj updateSet = updateSetBuilder.obj();
        if (updateSet.isEmpty()) {
            return Status(ErrorCodes::UserModificationFailed,
                          "Must specify at least one field to update in updateUser");
        }

        *parsedUpdateObj = BSON("$set" << updateSet);
        return Status::OK();
    }

    Status parseAndValidateRemoveUserCommand(const BSONObj& cmdObj,
                                             const std::string& dbname,
                                             UserName* parsedUserName,
                                             BSONObj* parsedWriteConcern) {
        unordered_set<std::string> validFieldNames;
        validFieldNames.insert("removeUser");
        validFieldNames.insert("writeConcern");

        Status status = _checkNoExtraFields(cmdObj, "removeUser", validFieldNames);
        if (!status.isOK()) {
            return status;
        }

        std::string user;
        status = bsonExtractStringField(cmdObj, "removeUser", &user);
        if (!status.isOK()) {
            return status;
        }

        status = _extractWriteConcern(cmdObj, parsedWriteConcern);
        if (!status.isOK()) {
            return status;
        }

        *parsedUserName = UserName(user, dbname);
        return Status::OK();
    }

    Status parseAndValidateRemoveUsersFromDatabaseCommand(const BSONObj& cmdObj,
                                                          const std::string& dbname,
                                                          BSONObj* parsedWriteConcern) {
        unordered_set<std::string> validFieldNames;
        validFieldNames.insert("removeUsersFromDatabase");
        validFieldNames.insert("writeConcern");

        Status status = _checkNoExtraFields(cmdObj, "removeUsersFromDatabase", validFieldNames);
        if (!status.isOK()) {
            return status;
        }

        status = _extractWriteConcern(cmdObj, parsedWriteConcern);
        if (!status.isOK()) {
            return status;
        }

        return Status::OK();
    }

    Status parseAndValidateUsersInfoCommand(const BSONObj& cmdObj,
                                            const std::string& dbname,
                                            bool* parsedAnyDB,
                                            BSONElement* parsedUsersFilter) {
        unordered_set<std::string> validFieldNames;
        validFieldNames.insert("usersInfo");
        validFieldNames.insert("anyDB");
        validFieldNames.insert("writeConcern");

        Status status = _checkNoExtraFields(cmdObj, "usersInfo", validFieldNames);
        if (!status.isOK()) {
            return status;
        }

        if (cmdObj["usersInfo"].type() != String && cmdObj["usersInfo"].type() != RegEx) {
            return Status(ErrorCodes::BadValue,
                          "Argument to userInfo command must be either a string or a regex");
        }
        *parsedUsersFilter = cmdObj["usersInfo"];


        bool anyDB = false;
        if (cmdObj.hasField("anyDB")) {
            if (dbname == "admin") {
                Status status = bsonExtractBooleanField(cmdObj, "anyDB", &anyDB);
                if (!status.isOK()) {
                    return status;
                }
            } else {
                return Status(ErrorCodes::BadValue,
                              "\"anyDB\" argument to usersInfo command is only valid when "
                                      "run on the \"admin\" database");
            }
        }
        *parsedAnyDB = anyDB;

        return Status::OK();
    }

    /*
     * Validates that the given privilege BSONArray is valid.
     * If parsedPrivileges is not NULL, adds to it the privileges parsed out of the input BSONArray.
     */
    Status _parseAndValidatePrivilegeArray(const BSONArray& privileges,
                                           PrivilegeVector* parsedPrivileges) {
        for (BSONObjIterator it(privileges); it.more(); it.next()) {
            BSONElement element = *it;
            if (element.type() != Object) {
                return Status(ErrorCodes::FailedToParse,
                              "Elements in privilege arrays must be objects");
            }

            ParsedPrivilege parsedPrivilege;
            std::string errmsg;
            if (!parsedPrivilege.parseBSON(element.Obj(), &errmsg)) {
                return Status(ErrorCodes::FailedToParse, errmsg);
            }
            if (!parsedPrivilege.isValid(&errmsg)) {
                return Status(ErrorCodes::FailedToParse, errmsg);
            }

            Privilege privilege;
            if (!ParsedPrivilege::parsedPrivilegeToPrivilege(parsedPrivilege, &privilege, &errmsg)) {
                return Status(ErrorCodes::FailedToParse, errmsg);
            }

            if (parsedPrivileges) {
                parsedPrivileges->push_back(privilege);
            }
        }
        return Status::OK();
    }

    Status parseAndValidateCreateRoleCommand(const BSONObj& cmdObj,
                                             const std::string& dbname,
                                             AuthorizationManager* authzManager,
                                             BSONObj* parsedRoleObj,
                                             BSONObj* parsedWriteConcern) {
        unordered_set<std::string> validFieldNames;
        validFieldNames.insert("createRole");
        validFieldNames.insert("privileges");
        validFieldNames.insert("roles");
        validFieldNames.insert("writeConcern");

        Status status = _checkNoExtraFields(cmdObj, "createRole", validFieldNames);
        if (!status.isOK()) {
            return status;
        }

        status = _extractWriteConcern(cmdObj, parsedWriteConcern);
        if (!status.isOK()) {
            return status;
        }

        BSONObjBuilder roleObjBuilder;

        // Parse role name
        std::string roleName;
        status = bsonExtractStringField(cmdObj, "createRole", &roleName);
        if (!status.isOK()) {
            return status;
        }

        // Prevent creating roles in the local database
        if (dbname == "local") {
            return Status(ErrorCodes::BadValue, "Cannot create roles in the local database");
        }

        roleObjBuilder.append("_id", dbname + "." + roleName);
        roleObjBuilder.append(AuthorizationManager::ROLE_NAME_FIELD_NAME, roleName);
        roleObjBuilder.append(AuthorizationManager::ROLE_SOURCE_FIELD_NAME, dbname);

        // Parse privileges
        BSONElement privilegesElement;
        status = bsonExtractTypedField(cmdObj, "privileges", Array, &privilegesElement);
        if (!status.isOK()) {
            return status;
        }
        status = _parseAndValidatePrivilegeArray(BSONArray(privilegesElement.Obj()), NULL);
        if (!status.isOK()) {
            return status;
        }
        roleObjBuilder.append(privilegesElement);

        // Parse roles
        BSONElement rolesElement;
        status = bsonExtractTypedField(cmdObj, "roles", Array, &rolesElement);
        if (!status.isOK()) {
            return status;
        }
        BSONArray modifiedRolesArray;
        status = _validateAndModifyRolesArray(rolesElement,
                                              dbname,
                                              authzManager,
                                              false,
                                              &modifiedRolesArray);
        if (!status.isOK()) {
            return status;
        }
        roleObjBuilder.append("roles", modifiedRolesArray);

        *parsedRoleObj = roleObjBuilder.obj();
        return Status::OK();
    }

    Status parseAndValidateRolePrivilegeManipulationCommands(const BSONObj& cmdObj,
                                                             const StringData& cmdName,
                                                             const std::string& dbname,
                                                             RoleName* parsedRoleName,
                                                             PrivilegeVector* parsedPrivileges,
                                                             BSONObj* parsedWriteConcern) {
        unordered_set<std::string> validFieldNames;
        validFieldNames.insert(cmdName.toString());
        validFieldNames.insert("privileges");
        validFieldNames.insert("writeConcern");

        Status status = _checkNoExtraFields(cmdObj, cmdName, validFieldNames);
        if (!status.isOK()) {
            return status;
        }

        status = _extractWriteConcern(cmdObj, parsedWriteConcern);
        if (!status.isOK()) {
            return status;
        }

        BSONObjBuilder roleObjBuilder;

        // Parse role name
        std::string roleName;
        status = bsonExtractStringField(cmdObj, cmdName, &roleName);
        if (!status.isOK()) {
            return status;
        }
        *parsedRoleName = RoleName(roleName, dbname);

        // Parse privileges
        BSONElement privilegesElement;
        status = bsonExtractTypedField(cmdObj, "privileges", Array, &privilegesElement);
        if (!status.isOK()) {
            return status;
        }
        status = _parseAndValidatePrivilegeArray(BSONArray(privilegesElement.Obj()),
                                                 parsedPrivileges);
        if (!status.isOK()) {
            return status;
        }

        return Status::OK();
    }

    Status getBSONForRole(RoleGraph* graph, const RoleName& roleName, mutablebson::Element result) {
        if (!graph->roleExists(roleName)) {
            return Status(ErrorCodes::RoleNotFound,
                          mongoutils::str::stream() << roleName.getFullName() <<
                                  "does not name an existing role");
        }
        std::string id = mongoutils::str::stream() << roleName.getDB() << "." << roleName.getRole();
        result.appendString("_id", id);
        result.appendString("name", roleName.getRole());
        result.appendString("source", roleName.getDB());

        // Build privileges array
        mutablebson::Element privilegesArrayElement =
                result.getDocument().makeElementArray("privileges");
        result.pushBack(privilegesArrayElement);
        const PrivilegeVector& privileges = graph->getDirectPrivileges(roleName);
        for (PrivilegeVector::const_iterator it = privileges.begin();
                it != privileges.end(); ++it) {
            std::string errmsg;
            ParsedPrivilege privilege;
            if (!ParsedPrivilege::privilegeToParsedPrivilege(*it, &privilege, &errmsg)) {
                return Status(ErrorCodes::BadValue, errmsg);
            }
            privilegesArrayElement.appendObject("privileges", privilege.toBSON());
        }

        // Build roles array
        mutablebson::Element rolesArrayElement = result.getDocument().makeElementArray("roles");
        result.pushBack(rolesArrayElement);
        RoleNameIterator nameIt = graph->getDirectSubordinates(roleName);
        while (nameIt.more()) {
            const RoleName& subRole = nameIt.next();
            mutablebson::Element roleObj = result.getDocument().makeElementObject("");
            roleObj.appendString("name", subRole.getRole());
            roleObj.appendString("source", subRole.getDB());
            rolesArrayElement.pushBack(roleObj);
        }

        return Status::OK();
    }

} // namespace auth
} // namespace mongo
