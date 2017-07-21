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

#include "mongo/platform/basic.h"

#include "mongo/db/auth/user_management_commands_parser.h"

#include <algorithm>
#include <string>
#include <vector>

#include "mongo/base/status.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/address_restriction.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/auth/privilege_parser.h"
#include "mongo/db/auth/user_document_parser.h"
#include "mongo/db/auth/user_name.h"
#include "mongo/db/commands.h"
#include "mongo/db/jsobj.h"
#include "mongo/platform/unordered_set.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/password_digest.h"
#include "mongo/util/stringutils.h"

namespace mongo {
namespace auth {

using std::vector;

Status _checkNoExtraFields(const BSONObj& cmdObj,
                           StringData cmdName,
                           const unordered_set<std::string>& validFieldNames) {
    // Iterate through all fields in command object and make sure there are no unexpected
    // ones.
    for (BSONObjIterator iter(cmdObj); iter.more(); iter.next()) {
        StringData fieldName = (*iter).fieldNameStringData();
        if (!Command::isGenericArgument(fieldName) &&
            !validFieldNames.count(fieldName.toString())) {
            return Status(ErrorCodes::BadValue,
                          mongoutils::str::stream() << "\"" << fieldName << "\" is not "
                                                                            "a valid argument to "
                                                    << cmdName);
        }
    }
    return Status::OK();
}

// Extracts a UserName or RoleName object from a BSONElement.
template <typename Name>
Status _parseNameFromBSONElement(const BSONElement& element,
                                 StringData dbname,
                                 StringData nameFieldName,
                                 StringData sourceFieldName,
                                 Name* parsedName) {
    if (element.type() == String) {
        *parsedName = Name(element.String(), dbname);
    } else if (element.type() == Object) {
        BSONObj obj = element.Obj();

        std::string name;
        std::string source;
        Status status = bsonExtractStringField(obj, nameFieldName, &name);
        if (!status.isOK()) {
            return status;
        }
        status = bsonExtractStringField(obj, sourceFieldName, &source);
        if (!status.isOK()) {
            return status;
        }

        *parsedName = Name(name, source);
    } else {
        return Status(ErrorCodes::BadValue,
                      "User and role names must be either strings or objects");
    }
    return Status::OK();
}

// Extracts UserName or RoleName objects from a BSONArray of role/user names.
template <typename Name>
Status _parseNamesFromBSONArray(const BSONArray& array,
                                StringData dbname,
                                StringData nameFieldName,
                                StringData sourceFieldName,
                                std::vector<Name>* parsedNames) {
    for (BSONObjIterator it(array); it.more(); it.next()) {
        BSONElement element = *it;
        Name name;
        Status status =
            _parseNameFromBSONElement(element, dbname, nameFieldName, sourceFieldName, &name);
        if (!status.isOK()) {
            return status;
        }
        parsedNames->push_back(name);
    }
    return Status::OK();
}

Status parseUserNamesFromBSONArray(const BSONArray& usersArray,
                                   StringData dbname,
                                   std::vector<UserName>* parsedUserNames) {
    return _parseNamesFromBSONArray(usersArray,
                                    dbname,
                                    AuthorizationManager::USER_NAME_FIELD_NAME,
                                    AuthorizationManager::USER_DB_FIELD_NAME,
                                    parsedUserNames);
}

Status parseRoleNamesFromBSONArray(const BSONArray& rolesArray,
                                   StringData dbname,
                                   std::vector<RoleName>* parsedRoleNames) {
    return _parseNamesFromBSONArray(rolesArray,
                                    dbname,
                                    AuthorizationManager::ROLE_NAME_FIELD_NAME,
                                    AuthorizationManager::ROLE_DB_FIELD_NAME,
                                    parsedRoleNames);
}

Status parseRolePossessionManipulationCommands(const BSONObj& cmdObj,
                                               StringData cmdName,
                                               const std::string& dbname,
                                               std::string* parsedName,
                                               vector<RoleName>* parsedRoleNames) {
    unordered_set<std::string> validFieldNames;
    validFieldNames.insert(cmdName.toString());
    validFieldNames.insert("roles");

    Status status = _checkNoExtraFields(cmdObj, cmdName, validFieldNames);
    if (!status.isOK()) {
        return status;
    }

    status = bsonExtractStringField(cmdObj, cmdName, parsedName);
    if (!status.isOK()) {
        return status;
    }

    BSONElement rolesElement;
    status = bsonExtractTypedField(cmdObj, "roles", Array, &rolesElement);
    if (!status.isOK()) {
        return status;
    }

    status = parseRoleNamesFromBSONArray(BSONArray(rolesElement.Obj()), dbname, parsedRoleNames);
    if (!status.isOK()) {
        return status;
    }

    if (!parsedRoleNames->size()) {
        return Status(ErrorCodes::BadValue,
                      mongoutils::str::stream() << cmdName << " command requires a non-empty "
                                                              "\"roles\" array");
    }
    return Status::OK();
}

Status parseCreateOrUpdateUserCommands(const BSONObj& cmdObj,
                                       StringData cmdName,
                                       const std::string& dbname,
                                       CreateOrUpdateUserArgs* parsedArgs) {
    unordered_set<std::string> validFieldNames;
    validFieldNames.insert(cmdName.toString());
    validFieldNames.insert("customData");
    validFieldNames.insert("digestPassword");
    validFieldNames.insert("pwd");
    validFieldNames.insert("roles");
    validFieldNames.insert("authenticationRestrictions");

    Status status = _checkNoExtraFields(cmdObj, cmdName, validFieldNames);
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
    if (userName.find('\0') != std::string::npos) {
        return Status(ErrorCodes::BadValue, "Username cannot contain NULL characters");
    }

    parsedArgs->userName = UserName(userName, dbname);

    // Parse password
    if (cmdObj.hasField("pwd")) {
        std::string password;
        status = bsonExtractStringField(cmdObj, "pwd", &password);
        if (!status.isOK()) {
            return status;
        }
        if (password.empty()) {
            return Status(ErrorCodes::BadValue, "User passwords must not be empty");
        }

        bool digestPassword;  // True if the server should digest the password
        status =
            bsonExtractBooleanFieldWithDefault(cmdObj, "digestPassword", true, &digestPassword);
        if (!status.isOK()) {
            return status;
        }

        if (digestPassword) {
            parsedArgs->hashedPassword = mongo::createPasswordDigest(userName, password);
        } else {
            parsedArgs->hashedPassword = password;
        }
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

    // Parse authentication restrictions
    if (cmdObj.hasField("authenticationRestrictions")) {
        BSONElement element = cmdObj["authenticationRestrictions"];
        if (element.type() != Array) {
            return Status(ErrorCodes::BadValue, "authenticationRestrictions must be an array");
        }
        parsedArgs->authenticationRestrictions =
            BSONArray(cmdObj["authenticationRestrictions"].Obj());
    }

    // Parse roles
    if (cmdObj.hasField("roles")) {
        BSONElement rolesElement;
        status = bsonExtractTypedField(cmdObj, "roles", Array, &rolesElement);
        if (!status.isOK()) {
            return status;
        }
        status =
            parseRoleNamesFromBSONArray(BSONArray(rolesElement.Obj()), dbname, &parsedArgs->roles);
        if (!status.isOK()) {
            return status;
        }
        parsedArgs->hasRoles = true;
    }

    return Status::OK();
}

Status parseAndValidateDropUserCommand(const BSONObj& cmdObj,
                                       const std::string& dbname,
                                       UserName* parsedUserName) {
    unordered_set<std::string> validFieldNames;
    validFieldNames.insert("dropUser");

    Status status = _checkNoExtraFields(cmdObj, "dropUser", validFieldNames);
    if (!status.isOK()) {
        return status;
    }

    std::string user;
    status = bsonExtractStringField(cmdObj, "dropUser", &user);
    if (!status.isOK()) {
        return status;
    }

    *parsedUserName = UserName(user, dbname);
    return Status::OK();
}

Status parseFromDatabaseCommand(const BSONObj& cmdObj,
                                const std::string& dbname,
                                std::string command) {
    unordered_set<std::string> validFieldNames;
    validFieldNames.insert(command);

    Status status = _checkNoExtraFields(cmdObj, command, validFieldNames);
    if (!status.isOK()) {
        return status;
    }

    return Status::OK();
}
Status parseAndValidateDropAllUsersFromDatabaseCommand(const BSONObj& cmdObj,
                                                       const std::string& dbname) {
    return parseFromDatabaseCommand(cmdObj, dbname, "dropAllUsersFromDatabase");
}

Status parseUsersInfoCommand(const BSONObj& cmdObj, StringData dbname, UsersInfoArgs* parsedArgs) {
    unordered_set<std::string> validFieldNames;
    validFieldNames.insert("usersInfo");
    validFieldNames.insert("showPrivileges");
    validFieldNames.insert("showCredentials");

    Status status = _checkNoExtraFields(cmdObj, "usersInfo", validFieldNames);
    if (!status.isOK()) {
        return status;
    }

    if (cmdObj["usersInfo"].numberInt() == 1) {
        parsedArgs->allForDB = true;
    } else if (cmdObj["usersInfo"].type() == Array) {
        status = parseUserNamesFromBSONArray(
            BSONArray(cmdObj["usersInfo"].Obj()), dbname, &parsedArgs->userNames);
        if (!status.isOK()) {
            return status;
        }
        std::sort(parsedArgs->userNames.begin(), parsedArgs->userNames.end());
    } else {
        UserName name;
        status = _parseNameFromBSONElement(cmdObj["usersInfo"],
                                           dbname,
                                           AuthorizationManager::USER_NAME_FIELD_NAME,
                                           AuthorizationManager::USER_DB_FIELD_NAME,
                                           &name);
        if (!status.isOK()) {
            return status;
        }
        parsedArgs->userNames.push_back(name);
    }

    status = bsonExtractBooleanFieldWithDefault(
        cmdObj, "showPrivileges", false, &parsedArgs->showPrivileges);
    if (!status.isOK()) {
        return status;
    }
    status = bsonExtractBooleanFieldWithDefault(
        cmdObj, "showCredentials", false, &parsedArgs->showCredentials);
    if (!status.isOK()) {
        return status;
    }

    return Status::OK();
}

Status parseRolesInfoCommand(const BSONObj& cmdObj, StringData dbname, RolesInfoArgs* parsedArgs) {
    unordered_set<std::string> validFieldNames;
    validFieldNames.insert("rolesInfo");
    validFieldNames.insert("showPrivileges");
    validFieldNames.insert("showAuthenticationRestrictions");
    validFieldNames.insert("showBuiltinRoles");

    Status status = _checkNoExtraFields(cmdObj, "rolesInfo", validFieldNames);
    if (!status.isOK()) {
        return status;
    }

    if (cmdObj["rolesInfo"].numberInt() == 1) {
        parsedArgs->allForDB = true;
    } else if (cmdObj["rolesInfo"].type() == Array) {
        status = parseRoleNamesFromBSONArray(
            BSONArray(cmdObj["rolesInfo"].Obj()), dbname, &parsedArgs->roleNames);
        if (!status.isOK()) {
            return status;
        }
    } else {
        RoleName name;
        status = _parseNameFromBSONElement(cmdObj["rolesInfo"],
                                           dbname,
                                           AuthorizationManager::ROLE_NAME_FIELD_NAME,
                                           AuthorizationManager::ROLE_DB_FIELD_NAME,
                                           &name);
        if (!status.isOK()) {
            return status;
        }
        parsedArgs->roleNames.push_back(name);
    }

    BSONElement showPrivileges = cmdObj["showPrivileges"];
    if (showPrivileges.eoo()) {
        parsedArgs->privilegeFormat = PrivilegeFormat::kOmit;
    } else if (showPrivileges.isNumber() || showPrivileges.isBoolean()) {
        parsedArgs->privilegeFormat =
            showPrivileges.trueValue() ? PrivilegeFormat::kShowSeparate : PrivilegeFormat::kOmit;
    } else if (showPrivileges.type() == BSONType::String &&
               showPrivileges.String() == "asUserFragment") {
        parsedArgs->privilegeFormat = PrivilegeFormat::kShowAsUserFragment;
    } else {
        return Status(ErrorCodes::FailedToParse,
                      str::stream() << "Failed to parse 'showPrivileges'. 'showPrivileges' should "
                                       "either be a boolean or the string 'asUserFragment', given: "
                                    << showPrivileges.toString());
    }

    const auto showAuthenticationRestrictions = cmdObj["showAuthenticationRestrictions"];
    if (showAuthenticationRestrictions.eoo()) {
        parsedArgs->authenticationRestrictionsFormat = AuthenticationRestrictionsFormat::kOmit;
    } else if (parsedArgs->privilegeFormat == PrivilegeFormat::kShowAsUserFragment) {
        return Status(
            ErrorCodes::UnsupportedFormat,
            "showAuthenticationRestrictions may not be used with showPrivileges='asUserFragment'");
    } else {
        bool show;
        status = bsonExtractBooleanField(cmdObj, "showAuthenticationRestrictions", &show);
        if (!status.isOK()) {
            return status;
        }
        parsedArgs->authenticationRestrictionsFormat = show
            ? AuthenticationRestrictionsFormat::kShow
            : AuthenticationRestrictionsFormat::kOmit;
    }

    status = bsonExtractBooleanFieldWithDefault(
        cmdObj, "showBuiltinRoles", false, &parsedArgs->showBuiltinRoles);
    if (!status.isOK()) {
        return status;
    }

    return Status::OK();
}

/*
 * Validates that the given privilege BSONArray is valid.
 * If parsedPrivileges is not NULL, adds to it the privileges parsed out of the input BSONArray.
 */
Status parseAndValidatePrivilegeArray(const BSONArray& privileges,
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
        std::vector<std::string> unrecognizedActions;
        Status status = ParsedPrivilege::parsedPrivilegeToPrivilege(
            parsedPrivilege, &privilege, &unrecognizedActions);
        if (!status.isOK()) {
            return status;
        }
        if (unrecognizedActions.size()) {
            std::string unrecognizedActionsString;
            joinStringDelim(unrecognizedActions, &unrecognizedActionsString, ',');
            return Status(ErrorCodes::FailedToParse,
                          str::stream() << "Unrecognized action privilege strings: "
                                        << unrecognizedActionsString);
        }

        parsedPrivileges->push_back(privilege);
    }
    return Status::OK();
}

Status parseCreateOrUpdateRoleCommands(const BSONObj& cmdObj,
                                       StringData cmdName,
                                       const std::string& dbname,
                                       CreateOrUpdateRoleArgs* parsedArgs) {
    unordered_set<std::string> validFieldNames;
    validFieldNames.insert(cmdName.toString());
    validFieldNames.insert("privileges");
    validFieldNames.insert("roles");
    validFieldNames.insert("authenticationRestrictions");

    Status status = _checkNoExtraFields(cmdObj, cmdName, validFieldNames);
    if (!status.isOK()) {
        return status;
    }

    // Parse role name
    std::string roleName;
    status = bsonExtractStringField(cmdObj, cmdName, &roleName);
    if (!status.isOK()) {
        return status;
    }
    if (roleName.find('\0') != std::string::npos) {
        return Status(ErrorCodes::BadValue, "Role name cannot contain NULL characters");
    }

    parsedArgs->roleName = RoleName(roleName, dbname);

    // Parse privileges
    if (cmdObj.hasField("privileges")) {
        BSONElement privilegesElement;
        status = bsonExtractTypedField(cmdObj, "privileges", Array, &privilegesElement);
        if (!status.isOK()) {
            return status;
        }
        status = parseAndValidatePrivilegeArray(BSONArray(privilegesElement.Obj()),
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
        status =
            parseRoleNamesFromBSONArray(BSONArray(rolesElement.Obj()), dbname, &parsedArgs->roles);
        if (!status.isOK()) {
            return status;
        }
        parsedArgs->hasRoles = true;
    }

    // Parse restrictions
    if (cmdObj.hasField("authenticationRestrictions")) {
        BSONElement restrictionsElement;
        status = bsonExtractTypedField(
            cmdObj, "authenticationRestrictions", Array, &restrictionsElement);
        if (!status.isOK()) {
            return status;
        }
        auto restrictions = getRawAuthenticationRestrictions(BSONArray(restrictionsElement.Obj()));
        if (!restrictions.isOK()) {
            return restrictions.getStatus();
        }
        parsedArgs->authenticationRestrictions = restrictions.getValue();
    }

    return Status::OK();
}

Status parseAndValidateRolePrivilegeManipulationCommands(const BSONObj& cmdObj,
                                                         StringData cmdName,
                                                         const std::string& dbname,
                                                         RoleName* parsedRoleName,
                                                         PrivilegeVector* parsedPrivileges) {
    unordered_set<std::string> validFieldNames;
    validFieldNames.insert(cmdName.toString());
    validFieldNames.insert("privileges");

    Status status = _checkNoExtraFields(cmdObj, cmdName, validFieldNames);
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
    status = parseAndValidatePrivilegeArray(BSONArray(privilegesElement.Obj()), parsedPrivileges);
    if (!status.isOK()) {
        return status;
    }
    if (!parsedPrivileges->size()) {
        return Status(ErrorCodes::BadValue,
                      mongoutils::str::stream() << cmdName << " command requires a non-empty "
                                                              "\"privileges\" array");
    }

    return Status::OK();
}

Status parseDropRoleCommand(const BSONObj& cmdObj,
                            const std::string& dbname,
                            RoleName* parsedRoleName) {
    unordered_set<std::string> validFieldNames;
    validFieldNames.insert("dropRole");

    Status status = _checkNoExtraFields(cmdObj, "dropRole", validFieldNames);
    if (!status.isOK()) {
        return status;
    }

    std::string user;
    status = bsonExtractStringField(cmdObj, "dropRole", &user);
    if (!status.isOK()) {
        return status;
    }

    *parsedRoleName = RoleName(user, dbname);
    return Status::OK();
}

Status parseDropAllRolesFromDatabaseCommand(const BSONObj& cmdObj, const std::string& dbname) {
    return parseFromDatabaseCommand(cmdObj, dbname, "dropAllRolesFromDatabase");
}

Status parseMergeAuthzCollectionsCommand(const BSONObj& cmdObj,
                                         MergeAuthzCollectionsArgs* parsedArgs) {
    unordered_set<std::string> validFieldNames;
    validFieldNames.insert("_mergeAuthzCollections");
    validFieldNames.insert("tempUsersCollection");
    validFieldNames.insert("tempRolesCollection");
    validFieldNames.insert("db");
    validFieldNames.insert("drop");

    Status status = _checkNoExtraFields(cmdObj, "_mergeAuthzCollections", validFieldNames);
    if (!status.isOK()) {
        return status;
    }

    status = bsonExtractStringFieldWithDefault(
        cmdObj, "tempUsersCollection", "", &parsedArgs->usersCollName);
    if (!status.isOK()) {
        return status;
    }

    status = bsonExtractStringFieldWithDefault(
        cmdObj, "tempRolesCollection", "", &parsedArgs->rolesCollName);
    if (!status.isOK()) {
        return status;
    }

    status = bsonExtractStringField(cmdObj, "db", &parsedArgs->db);
    if (!status.isOK()) {
        if (status == ErrorCodes::NoSuchKey) {
            return Status(ErrorCodes::OutdatedClient,
                          "Missing \"db\" field for _mergeAuthzCollections command. This is "
                          "most likely due to running an outdated (pre-2.6.4) version of "
                          "mongorestore.");
        }
        return status;
    }

    status = bsonExtractBooleanFieldWithDefault(cmdObj, "drop", false, &parsedArgs->drop);
    if (!status.isOK()) {
        return status;
    }

    return Status::OK();
}

Status parseAuthSchemaUpgradeCommand(const BSONObj& cmdObj,
                                     const std::string& dbname,
                                     AuthSchemaUpgradeArgs* parsedArgs) {
    static const int minUpgradeSteps = 1;
    static const int maxUpgradeSteps = 2;

    unordered_set<std::string> validFieldNames;
    validFieldNames.insert("authSchemaUpgrade");
    validFieldNames.insert("maxSteps");
    validFieldNames.insert("upgradeShards");

    Status status = _checkNoExtraFields(cmdObj, "authSchemaUpgrade", validFieldNames);
    if (!status.isOK()) {
        return status;
    }

    status = bsonExtractBooleanFieldWithDefault(
        cmdObj, "upgradeShards", true, &parsedArgs->shouldUpgradeShards);
    if (!status.isOK()) {
        return status;
    }

    long long steps;
    status = bsonExtractIntegerFieldWithDefault(cmdObj, "maxSteps", maxUpgradeSteps, &steps);
    if (!status.isOK())
        return status;
    if (steps < minUpgradeSteps || steps > maxUpgradeSteps) {
        return Status(ErrorCodes::BadValue,
                      mongoutils::str::stream() << "Legal values for \"maxSteps\" are at least "
                                                << minUpgradeSteps
                                                << " and no more than "
                                                << maxUpgradeSteps
                                                << "; found "
                                                << steps);
    }
    parsedArgs->maxSteps = static_cast<int>(steps);

    return Status::OK();
}
}  // namespace auth
}  // namespace mongo
