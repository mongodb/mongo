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


#include "mongo/db/auth/user_document_parser.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/init.h"  // IWYU pragma: keep
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/auth/address_restriction.h"
#include "mongo/db/auth/auth_name.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/parsed_privilege_gen.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/auth/restriction_set.h"
#include "mongo/db/auth/role_name.h"
#include "mongo/db/auth/user.h"
#include "mongo/db/auth/user_name.h"
#include "mongo/db/namespace_string.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <algorithm>
#include <array>
#include <iterator>
#include <string>
#include <vector>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kAccessControl


namespace mongo {

namespace {
constexpr StringData ADMIN_DBNAME = "admin"_sd;

constexpr StringData ROLES_FIELD_NAME = "roles"_sd;
constexpr StringData PRIVILEGES_FIELD_NAME = "inheritedPrivileges"_sd;
constexpr StringData INHERITED_ROLES_FIELD_NAME = "inheritedRoles"_sd;
constexpr StringData OTHER_DB_ROLES_FIELD_NAME = "otherDBRoles"_sd;
constexpr StringData READONLY_FIELD_NAME = "readOnly"_sd;
constexpr StringData CREDENTIALS_FIELD_NAME = "credentials"_sd;
constexpr StringData ROLE_NAME_FIELD_NAME = "role"_sd;
constexpr StringData ROLE_DB_FIELD_NAME = "db"_sd;
constexpr StringData SCRAMSHA1_CREDENTIAL_FIELD_NAME = "SCRAM-SHA-1"_sd;
constexpr StringData SCRAMSHA256_CREDENTIAL_FIELD_NAME = "SCRAM-SHA-256"_sd;
constexpr StringData MONGODB_EXTERNAL_CREDENTIAL_FIELD_NAME = "external"_sd;
constexpr StringData AUTHENTICATION_RESTRICTIONS_FIELD_NAME = "authenticationRestrictions"_sd;
constexpr StringData INHERITED_AUTHENTICATION_RESTRICTIONS_FIELD_NAME =
    "inheritedAuthenticationRestrictions"_sd;

inline Status _badValue(const char* reason) {
    return Status(ErrorCodes::BadValue, reason);
}

inline Status _badValue(const std::string& reason) {
    return Status(ErrorCodes::BadValue, reason);
}

template <typename Credentials>
bool parseSCRAMCredentials(const BSONElement& credentialsElement,
                           Credentials& scram,
                           StringData fieldName) {
    const auto scramElement = credentialsElement[fieldName];
    if (scramElement.eoo()) {
        return false;
    }

    // We are asserting rather then returning errors since these
    // fields should have been prepopulated by the calling code.
    scram.iterationCount = scramElement["iterationCount"].numberInt();
    uassert(17501,
            str::stream() << "Invalid or missing " << fieldName << " iteration count",
            scram.iterationCount > 0);

    scram.salt = scramElement["salt"].str();
    uassert(17502, str::stream() << "Missing " << fieldName << " salt", !scram.salt.empty());

    scram.serverKey = scramElement["serverKey"].str();
    uassert(
        17503, str::stream() << "Missing " << fieldName << " serverKey", !scram.serverKey.empty());

    scram.storedKey = scramElement["storedKey"].str();
    uassert(
        17504, str::stream() << "Missing " << fieldName << " storedKey", !scram.storedKey.empty());

    uassert(50684,
            str::stream() << "credential document " << fieldName << " failed validation",
            scram.isValid());
    return true;
}

Status _checkV2RolesArray(const BSONElement& rolesElement) try {
    if (rolesElement.eoo()) {
        return _badValue("User document needs 'roles' field to be provided");
    }
    if (rolesElement.type() != BSONType::array) {
        return _badValue("'roles' field must be an array");
    }
    for (const auto& elem : rolesElement.Array()) {
        uassert(ErrorCodes::UnsupportedFormat,
                "User document needs values in 'roles' array to be a sub-documents",
                elem.type() == BSONType::object);
        RoleName::parseFromBSONObj(elem.Obj());
    }
    return Status::OK();
} catch (const DBException& ex) {
    return ex.toStatus();
}

User::UserId extractUserIDFromUserDocument(const BSONObj& doc) {
    auto userId = doc[AuthorizationManager::USERID_FIELD_NAME];
    if (userId.isBinData(BinDataType::newUUID)) {
        auto id = userId.uuid();
        User::UserId ret;
        std::copy(id.begin(), id.end(), std::back_inserter(ret));
        return ret;
    }

    return User::UserId();
}

}  // namespace

Status V2UserDocumentParser::checkValidUserDocument(const BSONObj& doc) const {
    auto userIdElement = doc[AuthorizationManager::USERID_FIELD_NAME];
    auto userElement = doc[AuthorizationManager::USER_NAME_FIELD_NAME];
    auto userDBElement = doc[AuthorizationManager::USER_DB_FIELD_NAME];
    auto credentialsElement = doc[CREDENTIALS_FIELD_NAME];
    auto rolesElement = doc[ROLES_FIELD_NAME];

    // Validate the "userId" element.
    if (!userIdElement.eoo()) {
        if (!userIdElement.isBinData(BinDataType::newUUID)) {
            return _badValue("User document needs 'userId' field to be a UUID");
        }
    }

    // Validate the "user" element.
    if (userElement.type() != BSONType::string)
        return _badValue("User document needs 'user' field to be a string");
    if (userElement.valueStringData().empty())
        return _badValue("User document needs 'user' field to be non-empty");

    // Validate the "db" element
    if (userDBElement.type() != BSONType::string || userDBElement.valueStringData().empty()) {
        return _badValue("User document needs 'db' field to be a non-empty string");
    }
    StringData userDBStr = userDBElement.valueStringData();
    if (!DatabaseName::validDBName(userDBStr, DatabaseName::DollarInDbNameBehavior::Allow) &&
        userDBStr != "$external") {
        return _badValue(str::stream()
                         << "'" << userDBStr << "' is not a valid value for the db field.");
    }

    // Validate the "credentials" element
    if (credentialsElement.eoo()) {
        return _badValue("User document needs 'credentials' object");
    }
    if (credentialsElement.type() != BSONType::object) {
        return _badValue("User document needs 'credentials' field to be an object");
    }

    BSONObj credentialsObj = credentialsElement.Obj();
    if (credentialsObj.isEmpty()) {
        return _badValue("User document needs 'credentials' field to be a non-empty object");
    }
    if (userDBStr == "$external") {
        BSONElement externalElement = credentialsObj[MONGODB_EXTERNAL_CREDENTIAL_FIELD_NAME];
        if (externalElement.eoo() || externalElement.type() != BSONType::boolean ||
            !externalElement.Bool()) {
            return _badValue(
                "User documents for users defined on '$external' must have "
                "'credentials' field set to {external: true}");
        }
    } else {
        const auto validateScram = [&credentialsObj](const auto& fieldName) {
            auto scramElement = credentialsObj[fieldName];

            if (scramElement.eoo()) {
                return Status(ErrorCodes::NoSuchKey,
                              str::stream() << fieldName << " does not exist");
            }
            if (scramElement.type() != BSONType::object) {
                return _badValue(str::stream()
                                 << fieldName << " credential must be an object, if present");
            }
            return Status::OK();
        };

        auto sha1status = validateScram(SCRAMSHA1_CREDENTIAL_FIELD_NAME);
        if (!sha1status.isOK() && (sha1status.code() != ErrorCodes::NoSuchKey)) {
            return sha1status;
        }
        auto sha256status = validateScram(SCRAMSHA256_CREDENTIAL_FIELD_NAME);
        if (!sha256status.isOK() && (sha256status.code() != ErrorCodes::NoSuchKey)) {
            return sha256status;
        }

        if (!sha1status.isOK() && !sha256status.isOK()) {
            return _badValue(
                "User document must provide credentials for all "
                "non-external users");
        }
    }

    // Validate the "roles" element.
    Status status = _checkV2RolesArray(rolesElement);
    if (!status.isOK())
        return status;

    // Validate the "authenticationRestrictions" element.
    status = initializeAuthenticationRestrictionsFromUserDocument(doc, nullptr);
    if (!status.isOK()) {
        return status;
    }

    return Status::OK();
}

Status V2UserDocumentParser::initializeUserCredentialsFromUserDocument(
    User* user, const BSONObj& privDoc) const {
    User::CredentialData credentials;
    std::string userDB = privDoc[AuthorizationManager::USER_DB_FIELD_NAME].String();
    BSONElement credentialsElement = privDoc[CREDENTIALS_FIELD_NAME];
    if (!credentialsElement.eoo()) {
        if (credentialsElement.type() != BSONType::object) {
            return Status(ErrorCodes::UnsupportedFormat,
                          "'credentials' field in user documents must be an object");
        }
        if (userDB == "$external") {
            BSONElement externalCredentialElement =
                credentialsElement.Obj()[MONGODB_EXTERNAL_CREDENTIAL_FIELD_NAME];
            if (!externalCredentialElement.eoo()) {
                if (externalCredentialElement.type() != BSONType::boolean ||
                    !externalCredentialElement.Bool()) {
                    return Status(ErrorCodes::UnsupportedFormat,
                                  "'external' field in credentials object must be set to true");
                } else {
                    credentials.isExternal = true;
                }
            } else {
                return Status(ErrorCodes::UnsupportedFormat,
                              "User documents defined on '$external' must provide set "
                              "credentials to {external:true}");
            }
        } else {
            const bool haveSha1 = parseSCRAMCredentials(
                credentialsElement, credentials.scram_sha1, SCRAMSHA1_CREDENTIAL_FIELD_NAME);
            const bool haveSha256 = parseSCRAMCredentials(
                credentialsElement, credentials.scram_sha256, SCRAMSHA256_CREDENTIAL_FIELD_NAME);

            if (!haveSha1 && !haveSha256) {
                return Status(
                    ErrorCodes::UnsupportedFormat,
                    "User documents must provide credentials for SCRAM-SHA-1 and/or SCRAM-SHA-256");
            }

            credentials.isExternal = false;
        }
    } else {
        return Status(ErrorCodes::UnsupportedFormat,
                      "Cannot extract credentials from user documents without a "
                      "'credentials' field");
    }

    user->setCredentials(credentials);
    return Status::OK();
}

static Status _extractRoleDocumentElements(const BSONObj& roleObject,
                                           BSONElement* roleNameElement,
                                           BSONElement* roleSourceElement) {
    *roleNameElement = roleObject[ROLE_NAME_FIELD_NAME];
    *roleSourceElement = roleObject[ROLE_DB_FIELD_NAME];

    if (roleNameElement->type() != BSONType::string || roleNameElement->valueStringData().empty()) {
        return Status(ErrorCodes::UnsupportedFormat, "Role names must be non-empty strings");
    }
    if (roleSourceElement->type() != BSONType::string ||
        roleSourceElement->valueStringData().empty()) {
        return Status(ErrorCodes::UnsupportedFormat, "Role db must be non-empty strings");
    }

    return Status::OK();
}

Status V2UserDocumentParser::initializeAuthenticationRestrictionsFromUserDocument(
    const BSONObj& privDoc, User* user) const {

    // Restrictions on the user
    const auto authenticationRestrictions = privDoc[AUTHENTICATION_RESTRICTIONS_FIELD_NAME];
    if (!authenticationRestrictions.eoo()) {
        if (authenticationRestrictions.type() != BSONType::array) {
            return Status(ErrorCodes::UnsupportedFormat,
                          "'authenticationRestrictions' field must be an array");
        }

        auto restrictions =
            parseAuthenticationRestriction(BSONArray(authenticationRestrictions.Obj()));
        if (!restrictions.isOK()) {
            return restrictions.getStatus();
        }

        if (user) {
            user->setRestrictions(RestrictionDocuments({std::move(restrictions.getValue())}));
        }
    }

    // Restrictions from roles
    const auto inherited = privDoc[INHERITED_AUTHENTICATION_RESTRICTIONS_FIELD_NAME];
    if (!inherited.eoo()) {
        if (inherited.type() != BSONType::array) {
            return Status(ErrorCodes::UnsupportedFormat,
                          "'inheritedAuthenticationRestrictions' field must be an array");
        }

        RestrictionDocuments::sequence_type authRest;
        for (const auto& roleRestriction : BSONArray(inherited.Obj())) {
            if (roleRestriction.type() != BSONType::array) {
                return Status(ErrorCodes::UnsupportedFormat,
                              "'inheritedAuthenticationRestrictions' sub-fields must be arrays");
            }

            auto roleRestrictionDoc =
                parseAuthenticationRestriction(BSONArray(roleRestriction.Obj()));
            if (!roleRestrictionDoc.isOK()) {
                return roleRestrictionDoc.getStatus();
            }

            if (user) {
                authRest.push_back(std::move(roleRestrictionDoc.getValue()));
            }
        }

        if (user) {
            user->setIndirectRestrictions(RestrictionDocuments(std::move(authRest)));
        }
    }

    return Status::OK();
}

Status V2UserDocumentParser::initializeUserRolesFromUserDocument(const BSONObj& privDoc,
                                                                 User* user) const try {
    BSONElement rolesElement = privDoc[ROLES_FIELD_NAME];

    if (rolesElement.type() != BSONType::array) {
        return Status(ErrorCodes::UnsupportedFormat,
                      "User document needs 'roles' field to be an array");
    }

    auto rolesArray = rolesElement.Array();
    std::vector<RoleName> roles;
    std::transform(
        rolesArray.begin(), rolesArray.end(), std::back_inserter(roles), [this](const auto& elem) {
            uassert(ErrorCodes::UnsupportedFormat,
                    "User document needs values in 'roles' array to be a sub-documents",
                    elem.type() == BSONType::object);
            return RoleName::parseFromBSONObj(elem.Obj(), this->_tenant);
        });
    user->setRoles(makeRoleNameIteratorForContainer(roles));

    return Status::OK();
} catch (const DBException& ex) {
    return ex.toStatus();
}

Status V2UserDocumentParser::initializeUserIndirectRolesFromUserDocument(const BSONObj& privDoc,
                                                                         User* user) const try {
    BSONElement indirectRolesElement = privDoc[INHERITED_ROLES_FIELD_NAME];

    if (!indirectRolesElement) {
        return Status::OK();
    }

    if (indirectRolesElement.type() != BSONType::array) {
        return Status(ErrorCodes::UnsupportedFormat,
                      "User document needs 'inheritedRoles' field to be an array");
    }

    auto rolesArray = indirectRolesElement.Array();
    std::vector<RoleName> indirectRoles;
    std::transform(
        rolesArray.begin(),
        rolesArray.end(),
        std::back_inserter(indirectRoles),
        [this](const auto& elem) {
            uassert(ErrorCodes::UnsupportedFormat,
                    "User document needs values in 'inheritedRoles' array to be a sub-documents",
                    elem.type() == BSONType::object);
            return RoleName::parseFromBSONObj(elem.Obj(), this->_tenant);
        });
    user->setIndirectRoles(makeRoleNameIteratorForContainer(indirectRoles));

    return Status::OK();
} catch (const DBException& ex) {
    return ex.toStatus();
}

Status V2UserDocumentParser::initializeUserPrivilegesFromUserDocument(const BSONObj& doc,
                                                                      User* user) const try {
    BSONElement privilegesElement = doc[PRIVILEGES_FIELD_NAME];
    if (privilegesElement.eoo())
        return Status::OK();
    if (privilegesElement.type() != BSONType::array) {
        return Status(ErrorCodes::UnsupportedFormat,
                      "User document 'inheritedPrivileges' element must be Array if present.");
    }
    PrivilegeVector privileges;

    for (const auto& element : privilegesElement.Obj()) {
        if (element.type() != BSONType::object) {
            LOGV2_WARNING(23743,
                          "Wrong type of element in inheritedPrivileges array",
                          "user"_attr = user->getName(),
                          "element"_attr = element);
            continue;
        }

        auto pp = auth::ParsedPrivilege::parse(element.Obj(), IDLParserContext("userPrivilegeDoc"));
        std::vector<std::string> unrecognizedActions;
        auto privilege = Privilege::resolvePrivilegeWithTenant(
            user->getName().tenantId(), pp, &unrecognizedActions);
        if (unrecognizedActions.size()) {
            std::string unrecognizedActionsString;
            str::joinStringDelim(unrecognizedActions, &unrecognizedActionsString, ',');
            LOGV2_WARNING(23746,
                          "Encountered unrecognized actions while parsing user document",
                          "action"_attr = unrecognizedActionsString,
                          "user"_attr = user->getName());
        }
        privileges.push_back(privilege);
    }
    user->setPrivileges(privileges);
    return Status::OK();
} catch (const DBException& ex) {
    return ex.toStatus();
}

Status V2UserDocumentParser::initializeUserFromUserDocument(const BSONObj& privDoc,
                                                            User* user) const try {
    auto userName = privDoc[AuthorizationManager::USER_NAME_FIELD_NAME].str();
    uassert(ErrorCodes::BadValue,
            str::stream() << "User name from privilege document \"" << userName
                          << "\" doesn't match name of provided User \""
                          << user->getName().getUser() << "\"",
            userName == user->getName().getUser());

    user->setID(extractUserIDFromUserDocument(privDoc));
    uassertStatusOK(initializeUserCredentialsFromUserDocument(user, privDoc));
    uassertStatusOK(initializeUserRolesFromUserDocument(privDoc, user));
    uassertStatusOK(initializeUserIndirectRolesFromUserDocument(privDoc, user));
    uassertStatusOK(initializeUserPrivilegesFromUserDocument(privDoc, user));
    uassertStatusOK(initializeAuthenticationRestrictionsFromUserDocument(privDoc, user));

    return Status::OK();
} catch (const AssertionException& ex) {
    return ex.toStatus();
}

}  // namespace mongo
