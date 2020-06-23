/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
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
#include "mongo/db/command_generic_argument.h"
#include "mongo/db/commands.h"
#include "mongo/db/jsobj.h"
#include "mongo/stdx/unordered_set.h"
#include "mongo/util/str.h"

namespace mongo {
namespace auth {

using std::vector;

Status _checkNoExtraFields(const BSONObj& cmdObj,
                           StringData cmdName,
                           const stdx::unordered_set<std::string>& validFieldNames) {
    // Iterate through all fields in command object and make sure there are no unexpected
    // ones.
    for (BSONObjIterator iter(cmdObj); iter.more(); iter.next()) {
        StringData fieldName = (*iter).fieldNameStringData();
        if (!isGenericArgument(fieldName) && !validFieldNames.count(fieldName.toString())) {
            return Status(ErrorCodes::BadValue,
                          str::stream() << "\"" << fieldName
                                        << "\" is not "
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

Status parseUsersInfoCommand(const BSONObj& cmdObj, StringData dbname, UsersInfoArgs* parsedArgs) {
    stdx::unordered_set<std::string> validFieldNames;
    validFieldNames.insert("usersInfo");
    validFieldNames.insert("showAuthenticationRestrictions");
    validFieldNames.insert("showPrivileges");
    validFieldNames.insert("showCredentials");
    validFieldNames.insert("filter");

    Status status = _checkNoExtraFields(cmdObj, "usersInfo", validFieldNames);
    if (!status.isOK()) {
        return status;
    }

    if (cmdObj["usersInfo"].numberInt() == 1) {
        parsedArgs->target = UsersInfoArgs::Target::kDB;
    } else if (cmdObj["usersInfo"].type() == Object &&
               cmdObj["usersInfo"].Obj().getBoolField("forAllDBs")) {
        parsedArgs->target = UsersInfoArgs::Target::kGlobal;
    } else if (cmdObj["usersInfo"].type() == Array) {
        parsedArgs->target = UsersInfoArgs::Target::kExplicitUsers;
        status = parseUserNamesFromBSONArray(
            BSONArray(cmdObj["usersInfo"].Obj()), dbname, &parsedArgs->userNames);
        if (!status.isOK()) {
            return status;
        }
        std::sort(parsedArgs->userNames.begin(), parsedArgs->userNames.end());
    } else {
        parsedArgs->target = UsersInfoArgs::Target::kExplicitUsers;
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

    const auto showAuthenticationRestrictions = cmdObj["showAuthenticationRestrictions"];
    if (showAuthenticationRestrictions.eoo()) {
        parsedArgs->authenticationRestrictionsFormat = AuthenticationRestrictionsFormat::kOmit;
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


    const auto filterObj = cmdObj["filter"];
    if (!filterObj.eoo()) {
        if (filterObj.type() != Object) {
            return Status(ErrorCodes::TypeMismatch, "filter must be an Object");
        }
        parsedArgs->filter = filterObj.Obj();
    }

    return Status::OK();
}

Status parseRolesInfoCommand(const BSONObj& cmdObj, StringData dbname, RolesInfoArgs* parsedArgs) {
    stdx::unordered_set<std::string> validFieldNames;
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
            str::joinStringDelim(unrecognizedActions, &unrecognizedActionsString, ',');
            return Status(ErrorCodes::FailedToParse,
                          str::stream() << "Unrecognized action privilege strings: "
                                        << unrecognizedActionsString);
        }

        parsedPrivileges->push_back(privilege);
    }
    return Status::OK();
}

Status parseMergeAuthzCollectionsCommand(const BSONObj& cmdObj,
                                         MergeAuthzCollectionsArgs* parsedArgs) {
    stdx::unordered_set<std::string> validFieldNames;
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

}  // namespace auth
}  // namespace mongo
