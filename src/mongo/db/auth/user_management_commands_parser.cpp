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
#include "mongo/client/auth_helpers.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/auth/privilege_parser.h"
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
     */
    Status _extractRoleNamesFromBSONArray(const BSONArray& rolesArray,
                                          const std::string& dbname,
                                          const StringData& rolesFieldName,
                                          std::vector<RoleName>* parsedRoleNames) {
        for (BSONObjIterator it(rolesArray); it.more(); it.next()) {
            BSONElement element = *it;
            if (element.type() == String) {
                parsedRoleNames->push_back(RoleName(element.String(), dbname));
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

                parsedRoleNames->push_back(RoleName(roleNameString, roleSource));
            } else {
                return Status(ErrorCodes::BadValue,
                              mongoutils::str::stream() << "Values in \"" << rolesFieldName <<
                                      "\" array must be sub-documents or strings");
            }
        }
        return Status::OK();
    }

    Status _extractRoleDataFromBSONArray(const BSONElement& rolesElement,
                                         const std::string& dbname,
                                         std::vector<User::RoleData> *parsedRoleData) {
        for (BSONObjIterator it(rolesElement.Obj()); it.more(); it.next()) {
            BSONElement element = *it;
            if (element.type() == String) {
                RoleName roleName(element.String(), dbname);
                parsedRoleData->push_back(User::RoleData(roleName, true, false));
           } else if (element.type() == Object) {
               // Check that the role object is valid
               V2UserDocumentParser parser;
               BSONObj roleObj = element.Obj();
               Status status = parser.checkValidRoleObject(roleObj, true);
               if (!status.isOK()) {
                   return status;
               }

               std::string roleName;
               std::string roleSource;
               bool hasRole;
               bool canDelegate;
               status = bsonExtractStringField(roleObj, "name", &roleName);
               if (!status.isOK()) {
                   return status;
               }
               status = bsonExtractStringField(roleObj, "source", &roleSource);
               if (!status.isOK()) {
                   return status;
               }
               status = bsonExtractBooleanField(roleObj, "hasRole", &hasRole);
               if (!status.isOK()) {
                   return status;
               }
               status = bsonExtractBooleanField(roleObj, "canDelegate", &canDelegate);
               if (!status.isOK()) {
                   return status;
               }

               parsedRoleData->push_back(User::RoleData(RoleName(roleName, roleSource),
                                                        hasRole,
                                                        canDelegate));
           } else {
               return Status(ErrorCodes::UnsupportedFormat,
                             "Values in 'roles' array must be sub-documents or strings");
           }
       }

       return Status::OK();
    }

    Status parseRolePossessionManipulationCommands(const BSONObj& cmdObj,
                                                   const StringData& cmdName,
                                                   const StringData& rolesFieldName,
                                                   const std::string& dbname,
                                                   std::string* parsedName,
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
        *parsedName = userNameStr;

        BSONElement rolesElement;
        status = bsonExtractTypedField(cmdObj, "roles", Array, &rolesElement);
        if (!status.isOK()) {
            return status;
        }

        status = _extractRoleNamesFromBSONArray(BSONArray(rolesElement.Obj()),
                                                dbname,
                                                rolesFieldName,
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

    Status parseCreateOrUpdateUserCommands(const BSONObj& cmdObj,
                                           const StringData& cmdName,
                                           const std::string& dbname,
                                           CreateOrUpdateUserArgs* parsedArgs) {
        unordered_set<std::string> validFieldNames;
        validFieldNames.insert(cmdName.toString());
        validFieldNames.insert("customData");
        validFieldNames.insert("pwd");
        validFieldNames.insert("roles");
        validFieldNames.insert("writeConcern");

        Status status = _checkNoExtraFields(cmdObj, cmdName, validFieldNames);
        if (!status.isOK()) {
            return status;
        }

        status = _extractWriteConcern(cmdObj, &parsedArgs->writeConcern);
        if (!status.isOK()) {
            return status;
        }

        BSONObjBuilder userObjBuilder;

        // Parse user name
        std::string userName;
        status = bsonExtractStringField(cmdObj, cmdName, &userName);
        if (!status.isOK()) {
            return status;
        }

        parsedArgs->userName = UserName(userName, dbname);

        // Parse password
        if (cmdObj.hasField("pwd")) {
            std::string clearTextPassword;
            status = bsonExtractStringField(cmdObj, "pwd", &clearTextPassword);
            if (!status.isOK()) {
                return status;
            }

            parsedArgs->hashedPassword = auth::createPasswordDigest(userName, clearTextPassword);
            parsedArgs->hasHashedPassword = true;
        }

        // Parse custom data
        if (cmdObj.hasField("customData")) {
            BSONElement element;
            status = bsonExtractTypedField(cmdObj, "customData", Object, &element);
            if (!status.isOK()) {
                return status;
            }
            parsedArgs->customData = element.Obj();
            parsedArgs->hasCustomData = true;
        }

        // Parse roles
        if (cmdObj.hasField("roles")) {
            BSONElement rolesElement;
            status = bsonExtractTypedField(cmdObj, "roles", Array, &rolesElement);
            if (!status.isOK()) {
                return status;
            }
            status = _extractRoleDataFromBSONArray(rolesElement, dbname, &parsedArgs->roles);
            if (!status.isOK()) {
                return status;
            }
            parsedArgs->hasRoles = true;
        }

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

    Status parseAndValidateInfoCommands(const BSONObj& cmdObj,
                                        const StringData& cmdName,
                                        const std::string& dbname,
                                        bool* parsedAnyDB,
                                        BSONElement* parsedNameFilter) {
        unordered_set<std::string> validFieldNames;
        validFieldNames.insert(cmdName.toString());
        validFieldNames.insert("anyDB");
        validFieldNames.insert("writeConcern");

        Status status = _checkNoExtraFields(cmdObj, cmdName, validFieldNames);
        if (!status.isOK()) {
            return status;
        }

        if (cmdObj[cmdName].type() != String && cmdObj[cmdName].type() != RegEx) {
            return Status(ErrorCodes::BadValue,
                          mongoutils::str::stream() << "Argument to \"" << cmdName <<
                                  "\"command must be either a string or a regex");
        }
        *parsedNameFilter = cmdObj[cmdName];


        bool anyDB = false;
        if (cmdObj.hasField("anyDB")) {
            if (dbname == "admin") {
                Status status = bsonExtractBooleanField(cmdObj, "anyDB", &anyDB);
                if (!status.isOK()) {
                    return status;
                }
            } else {
                return Status(ErrorCodes::BadValue,
                              mongoutils::str::stream() << "\"anyDB\" argument to \"" << cmdName <<
                                      "\"command is only valid when run on the \"admin\" database");
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

            parsedPrivileges->push_back(privilege);
        }
        return Status::OK();
    }

    Status parseCreateOrUpdateRoleCommands(const BSONObj& cmdObj,
                                           const StringData& cmdName,
                                           const std::string& dbname,
                                           CreateOrUpdateRoleArgs* parsedArgs) {
        unordered_set<std::string> validFieldNames;
        validFieldNames.insert(cmdName.toString());
        validFieldNames.insert("privileges");
        validFieldNames.insert("roles");
        validFieldNames.insert("writeConcern");

        Status status = _checkNoExtraFields(cmdObj, cmdName, validFieldNames);
        if (!status.isOK()) {
            return status;
        }

        status = _extractWriteConcern(cmdObj, &parsedArgs->writeConcern);
        if (!status.isOK()) {
            return status;
        }

        std::string roleName;
        status = bsonExtractStringField(cmdObj, "createRole", &roleName);
        if (!status.isOK()) {
            return status;
        }
        parsedArgs->roleName = RoleName(roleName, dbname);

        // Parse privileges
        if (cmdObj.hasField("privileges")) {
            BSONElement privilegesElement;
            status = bsonExtractTypedField(cmdObj, "privileges", Array, &privilegesElement);
            if (!status.isOK()) {
                return status;
            }
            status = _parseAndValidatePrivilegeArray(BSONArray(privilegesElement.Obj()),
                                                     &parsedArgs->privileges);
            if (!status.isOK()) {
                return status;
            }
            parsedArgs->hasPrivileges = true;
        }

        // Parse roles
        if (cmdObj.hasField("roles")) {
            BSONElement rolesElement;
            status = bsonExtractTypedField(cmdObj, "roles", Array, &rolesElement);
            if (!status.isOK()) {
                return status;
            }
            status = _extractRoleNamesFromBSONArray(BSONArray(rolesElement.Obj()),
                                                    dbname,
                                                    "roles",
                                                    &parsedArgs->roles);
            if (!status.isOK()) {
                return status;
            }
            parsedArgs->hasRoles = true;
        }
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

} // namespace auth
} // namespace mongo
