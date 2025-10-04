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

#include "mongo/db/auth/user_management_commands_parser.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/parsed_privilege_gen.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/auth/user_name.h"
#include "mongo/db/tenant_id.h"
#include "mongo/idl/command_generic_argument.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/stdx/unordered_set.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <string>
#include <vector>

#include <absl/container/node_hash_map.h>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>

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
        if (!isGenericArgument(fieldName) && !validFieldNames.count(std::string{fieldName})) {
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
    if (element.type() == BSONType::string) {
        *parsedName = Name(element.String(), dbname);
    } else if (element.type() == BSONType::object) {
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

/*
 * Validates that the given privilege BSONArray is valid.
 * If parsedPrivileges is not NULL, adds to it the privileges parsed out of the input BSONArray.
 */
Status parseAndValidatePrivilegeArray(const BSONArray& privileges,
                                      PrivilegeVector* parsedPrivileges) try {
    for (const auto& element : privileges) {
        if (element.type() != BSONType::object) {
            return Status(ErrorCodes::FailedToParse,
                          "Elements in privilege arrays must be objects");
        }

        auto parsedPrivilege =
            auth::ParsedPrivilege::parse(element.Obj(), IDLParserContext("privilege"));
        std::vector<std::string> unrecognizedActions;
        auto privilege = Privilege::resolvePrivilegeWithTenant(
            boost::none /* tenantId */, parsedPrivilege, &unrecognizedActions);
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
} catch (const DBException& ex) {
    return Status(ErrorCodes::FailedToParse, ex.toStatus().reason());
}

}  // namespace auth
}  // namespace mongo
