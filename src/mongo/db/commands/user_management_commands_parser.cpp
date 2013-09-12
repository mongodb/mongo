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

#include "mongo/db/commands/user_management_commands_parser.h"

#include <string>
#include <vector>

#include "mongo/base/status.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/client/auth_helpers.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/privilege_document_parser.h"
#include "mongo/db/auth/user_name.h"
#include "mongo/db/jsobj.h"
#include "mongo/platform/unordered_set.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {
namespace auth {

    Status extractWriteConcern(const BSONObj& cmdObj, BSONObj* writeConcern) {
        BSONElement writeConcernElement;
        Status status = bsonExtractTypedField(cmdObj, "writeConcern", Object, &writeConcernElement);
        if (!status.isOK()) {
            if (status.code() == ErrorCodes::NoSuchKey) {
                *writeConcern = BSONObj();
                return Status::OK();
            }
            return status;
        }
        *writeConcern = writeConcernElement.Obj();
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
        Status status = extractWriteConcern(cmdObj, parsedWriteConcern);
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
     */
    Status _validateAndModifyRolesArray(const BSONElement& rolesElement,
                                        const std::string& dbname,
                                        AuthorizationManager* authzManager,
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

                rolesBuilder.append(BSON("name" << element.String() <<
                                         "source" << dbname <<
                                         "hasRole" << true <<
                                         "canDelegate" << false));
            } else if (element.type() == Object) {
                // Check that the role object is valid
                V2PrivilegeDocumentParser parser;
                BSONObj roleObj = element.Obj();
                Status status = parser.checkValidRoleObject(roleObj);
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
                                             BSONObj* parsedUserObj) {
        unordered_set<std::string> validFieldNames;
        validFieldNames.insert("createUser");
        validFieldNames.insert("customData");
        validFieldNames.insert("pwd");
        validFieldNames.insert("roles");
        validFieldNames.insert("writeConcern");

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

        BSONObjBuilder userObjBuilder;
        userObjBuilder.append("_id", OID::gen());

        // Parse user name
        std::string userName;
        Status status = bsonExtractStringField(cmdObj, "createUser", &userName);
        if (!status.isOK()) {
            return status;
        }

        // Prevent creating users in the local database
        if (dbname == "local") {
            return Status(ErrorCodes::BadValue, "Cannot create users in the local database");
        }

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
                                                  &modifiedRolesArray);
            if (!status.isOK()) {
                return status;
            }

            userObjBuilder.append("roles", modifiedRolesArray);
        }

        *parsedUserObj = userObjBuilder.obj();

        // Make sure document to insert is valid
        V2PrivilegeDocumentParser parser;
        status = parser.checkValidPrivilegeDocument(*parsedUserObj);
        if (!status.isOK()) {
            return status;
        }

        return Status::OK();
    }


    Status parseAndValidateUpdateUserCommand(const BSONObj& cmdObj,
                                             const std::string& dbname,
                                             AuthorizationManager* authzManager,
                                             BSONObj* parsedUpdateObj,
                                             UserName* parsedUserName) {
        unordered_set<std::string> validFieldNames;
        validFieldNames.insert("updateUser");
        validFieldNames.insert("customData");
        validFieldNames.insert("pwd");
        validFieldNames.insert("roles");
        validFieldNames.insert("writeConcern");

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

        BSONObjBuilder updateSetBuilder;

        // Parse user name
        std::string userName;
        Status status = bsonExtractStringField(cmdObj, "updateUser", &userName);
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

} // namespace auth
} // namespace mongo
