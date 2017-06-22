/**
*    Copyright (C) 2012 10gen Inc.
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kAccessControl

#include "mongo/db/auth/user_document_parser.h"

#include <string>

#include "mongo/base/init.h"
#include "mongo/base/status.h"
#include "mongo/db/auth/address_restriction.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/privilege_parser.h"
#include "mongo/db/auth/user.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/namespace_string.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/stringutils.h"

namespace mongo {

namespace {
const std::string ADMIN_DBNAME = "admin";

const std::string ROLES_FIELD_NAME = "roles";
const std::string PRIVILEGES_FIELD_NAME = "inheritedPrivileges";
const std::string INHERITED_ROLES_FIELD_NAME = "inheritedRoles";
const std::string OTHER_DB_ROLES_FIELD_NAME = "otherDBRoles";
const std::string READONLY_FIELD_NAME = "readOnly";
const std::string CREDENTIALS_FIELD_NAME = "credentials";
const std::string ROLE_NAME_FIELD_NAME = "role";
const std::string ROLE_DB_FIELD_NAME = "db";
const std::string MONGODB_CR_CREDENTIAL_FIELD_NAME = "MONGODB-CR";
const std::string SCRAM_CREDENTIAL_FIELD_NAME = "SCRAM-SHA-1";
const std::string MONGODB_EXTERNAL_CREDENTIAL_FIELD_NAME = "external";
constexpr StringData AUTHENTICATION_RESTRICTIONS_FIELD_NAME = "authenticationRestrictions"_sd;
constexpr StringData INHERITED_AUTHENTICATION_RESTRICTIONS_FIELD_NAME =
    "inheritedAuthenticationRestrictions"_sd;

inline Status _badValue(const char* reason, int location) {
    return Status(ErrorCodes::BadValue, reason, location);
}

inline Status _badValue(const std::string& reason, int location) {
    return Status(ErrorCodes::BadValue, reason, location);
}

Status _checkV1RolesArray(const BSONElement& rolesElement) {
    if (rolesElement.type() != Array) {
        return _badValue("Role fields must be an array when present in system.users entries", 0);
    }
    for (BSONObjIterator iter(rolesElement.embeddedObject()); iter.more(); iter.next()) {
        BSONElement element = *iter;
        if (element.type() != String || element.valueStringData().empty()) {
            return _badValue("Roles must be non-empty strings.", 0);
        }
    }
    return Status::OK();
}
}  // namespace

std::string V1UserDocumentParser::extractUserNameFromUserDocument(const BSONObj& doc) const {
    return doc[AuthorizationManager::V1_USER_NAME_FIELD_NAME].str();
}

Status V1UserDocumentParser::initializeUserCredentialsFromUserDocument(
    User* user, const BSONObj& privDoc) const {
    User::CredentialData credentials;
    if (privDoc.hasField(AuthorizationManager::PASSWORD_FIELD_NAME)) {
        credentials.password = privDoc[AuthorizationManager::PASSWORD_FIELD_NAME].String();
        credentials.isExternal = false;
    } else if (privDoc.hasField(AuthorizationManager::V1_USER_SOURCE_FIELD_NAME)) {
        std::string userSource = privDoc[AuthorizationManager::V1_USER_SOURCE_FIELD_NAME].String();
        if (userSource != "$external") {
            return Status(ErrorCodes::UnsupportedFormat,
                          "Cannot extract credentials from user documents without a password "
                          "and with userSource != \"$external\"");
        } else {
            credentials.isExternal = true;
        }
    } else {
        return Status(ErrorCodes::UnsupportedFormat,
                      "Invalid user document: must have one of \"pwd\" and \"userSource\"");
    }

    user->setCredentials(credentials);
    return Status::OK();
}

static void _initializeUserRolesFromV0UserDocument(User* user,
                                                   const BSONObj& privDoc,
                                                   StringData dbname) {
    bool readOnly = privDoc["readOnly"].trueValue();
    if (dbname == "admin") {
        if (readOnly) {
            user->addRole(RoleName(RoleGraph::BUILTIN_ROLE_V0_ADMIN_READ, "admin"));
        } else {
            user->addRole(RoleName(RoleGraph::BUILTIN_ROLE_V0_ADMIN_READ_WRITE, "admin"));
        }
    } else {
        if (readOnly) {
            user->addRole(RoleName(RoleGraph::BUILTIN_ROLE_V0_READ, dbname));
        } else {
            user->addRole(RoleName(RoleGraph::BUILTIN_ROLE_V0_READ_WRITE, dbname));
        }
    }
}

Status _initializeUserRolesFromV1RolesArray(User* user,
                                            const BSONElement& rolesElement,
                                            StringData dbname) {
    static const char privilegesTypeMismatchMessage[] =
        "Roles in V1 user documents must be enumerated in an array of strings.";

    if (rolesElement.type() != Array)
        return Status(ErrorCodes::TypeMismatch, privilegesTypeMismatchMessage);

    for (BSONObjIterator iter(rolesElement.embeddedObject()); iter.more(); iter.next()) {
        BSONElement roleElement = *iter;
        if (roleElement.type() != String)
            return Status(ErrorCodes::TypeMismatch, privilegesTypeMismatchMessage);

        user->addRole(RoleName(roleElement.String(), dbname));
    }
    return Status::OK();
}

static Status _initializeUserRolesFromV1UserDocument(User* user,
                                                     const BSONObj& privDoc,
                                                     StringData dbname) {
    if (!privDoc[READONLY_FIELD_NAME].eoo()) {
        return Status(ErrorCodes::UnsupportedFormat,
                      "User documents may not contain both \"readonly\" and "
                      "\"roles\" fields");
    }

    Status status = _initializeUserRolesFromV1RolesArray(user, privDoc[ROLES_FIELD_NAME], dbname);
    if (!status.isOK()) {
        return status;
    }

    // If "dbname" is the admin database, handle the otherDBPrivileges field, which
    // grants privileges on databases other than "dbname".
    BSONElement otherDbPrivileges = privDoc[OTHER_DB_ROLES_FIELD_NAME];
    if (dbname == ADMIN_DBNAME) {
        switch (otherDbPrivileges.type()) {
            case EOO:
                break;
            case Object: {
                for (BSONObjIterator iter(otherDbPrivileges.embeddedObject()); iter.more();
                     iter.next()) {
                    BSONElement rolesElement = *iter;
                    status = _initializeUserRolesFromV1RolesArray(
                        user, rolesElement, rolesElement.fieldName());
                    if (!status.isOK())
                        return status;
                }
                break;
            }
            default:
                return Status(ErrorCodes::TypeMismatch,
                              "Field \"otherDBRoles\" must be an object, if present.");
        }
    } else if (!otherDbPrivileges.eoo()) {
        return Status(ErrorCodes::UnsupportedFormat,
                      "Only the admin database may contain a field called \"otherDBRoles\"");
    }

    return Status::OK();
}

Status V1UserDocumentParser::initializeUserRolesFromUserDocument(User* user,
                                                                 const BSONObj& privDoc,
                                                                 StringData dbname) const {
    if (!privDoc.hasField("roles")) {
        _initializeUserRolesFromV0UserDocument(user, privDoc, dbname);
    } else {
        return _initializeUserRolesFromV1UserDocument(user, privDoc, dbname);
    }
    return Status::OK();
}


Status _checkV2RolesArray(const BSONElement& rolesElement) {
    if (rolesElement.eoo()) {
        return _badValue("User document needs 'roles' field to be provided", 0);
    }
    if (rolesElement.type() != Array) {
        return _badValue("'roles' field must be an array", 0);
    }
    for (BSONObjIterator iter(rolesElement.embeddedObject()); iter.more(); iter.next()) {
        if ((*iter).type() != Object) {
            return _badValue("Elements in 'roles' array must objects", 0);
        }
        Status status = V2UserDocumentParser::checkValidRoleObject((*iter).Obj());
        if (!status.isOK())
            return status;
    }
    return Status::OK();
}

Status V2UserDocumentParser::checkValidUserDocument(const BSONObj& doc) const {
    BSONElement userElement = doc[AuthorizationManager::USER_NAME_FIELD_NAME];
    BSONElement userIDElement = doc[AuthorizationManager::USER_ID_FIELD_NAME];
    BSONElement userDBElement = doc[AuthorizationManager::USER_DB_FIELD_NAME];
    BSONElement credentialsElement = doc[CREDENTIALS_FIELD_NAME];
    BSONElement rolesElement = doc[ROLES_FIELD_NAME];

    // Validate the "user" element.
    if (userElement.type() != String)
        return _badValue("User document needs 'user' field to be a string", 0);
    if (userElement.valueStringData().empty())
        return _badValue("User document needs 'user' field to be non-empty", 0);

    // If we have an id field, make sure it is an OID
    if (!userIDElement.eoo() && (userIDElement.type() != BSONType::jstOID)) {
        return _badValue("User document 'userId' field must be an OID", 0);
    }

    // Validate the "db" element
    if (userDBElement.type() != String || userDBElement.valueStringData().empty()) {
        return _badValue("User document needs 'db' field to be a non-empty string", 0);
    }
    StringData userDBStr = userDBElement.valueStringData();
    if (!NamespaceString::validDBName(userDBStr, NamespaceString::DollarInDbNameBehavior::Allow) &&
        userDBStr != "$external") {
        return _badValue(mongoutils::str::stream() << "'" << userDBStr
                                                   << "' is not a valid value for the db field.",
                         0);
    }

    // Validate the "credentials" element
    if (credentialsElement.eoo()) {
        return _badValue("User document needs 'credentials' object", 0);
    }
    if (credentialsElement.type() != Object) {
        return _badValue("User document needs 'credentials' field to be an object", 0);
    }

    BSONObj credentialsObj = credentialsElement.Obj();
    if (credentialsObj.isEmpty()) {
        return _badValue("User document needs 'credentials' field to be a non-empty object", 0);
    }
    if (userDBStr == "$external") {
        BSONElement externalElement = credentialsObj[MONGODB_EXTERNAL_CREDENTIAL_FIELD_NAME];
        if (externalElement.eoo() || externalElement.type() != Bool || !externalElement.Bool()) {
            return _badValue(
                "User documents for users defined on '$external' must have "
                "'credentials' field set to {external: true}",
                0);
        }
    } else {
        BSONElement scramElement = credentialsObj[SCRAM_CREDENTIAL_FIELD_NAME];
        BSONElement mongoCRElement = credentialsObj[MONGODB_CR_CREDENTIAL_FIELD_NAME];

        if (!mongoCRElement.eoo()) {
            if (mongoCRElement.type() != String || mongoCRElement.valueStringData().empty()) {
                return _badValue(
                    "MONGODB-CR credential must to be a non-empty string"
                    ", if present",
                    0);
            }
        } else if (!scramElement.eoo()) {
            if (scramElement.type() != Object) {
                return _badValue("SCRAM credential must be an object, if present", 0);
            }
        } else {
            return _badValue(
                "User document must provide credentials for all "
                "non-external users",
                0);
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

std::string V2UserDocumentParser::extractUserNameFromUserDocument(const BSONObj& doc) const {
    return doc[AuthorizationManager::USER_NAME_FIELD_NAME].str();
}

boost::optional<OID> V2UserDocumentParser::extractUserIDFromUserDocument(const BSONObj& doc) const {
    BSONElement e = doc[AuthorizationManager::USER_ID_FIELD_NAME];
    if (e.type() == BSONType::EOO) {
        return boost::optional<OID>();
    }

    return e.OID();
}

Status V2UserDocumentParser::initializeUserCredentialsFromUserDocument(
    User* user, const BSONObj& privDoc) const {
    User::CredentialData credentials;
    std::string userDB = privDoc[AuthorizationManager::USER_DB_FIELD_NAME].String();
    BSONElement credentialsElement = privDoc[CREDENTIALS_FIELD_NAME];
    if (!credentialsElement.eoo()) {
        if (credentialsElement.type() != Object) {
            return Status(ErrorCodes::UnsupportedFormat,
                          "'credentials' field in user documents must be an object");
        }
        if (userDB == "$external") {
            BSONElement externalCredentialElement =
                credentialsElement.Obj()[MONGODB_EXTERNAL_CREDENTIAL_FIELD_NAME];
            if (!externalCredentialElement.eoo()) {
                if (externalCredentialElement.type() != Bool || !externalCredentialElement.Bool()) {
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
            BSONElement scramElement = credentialsElement.Obj()[SCRAM_CREDENTIAL_FIELD_NAME];
            BSONElement mongoCRCredentialElement =
                credentialsElement.Obj()[MONGODB_CR_CREDENTIAL_FIELD_NAME];

            if (scramElement.eoo() && mongoCRCredentialElement.eoo()) {
                return Status(ErrorCodes::UnsupportedFormat,
                              "User documents must provide credentials for SCRAM-SHA-1 "
                              "or MONGODB-CR authentication");
            }

            if (!scramElement.eoo()) {
                // We are asserting rather then returning errors since these
                // fields should have been prepopulated by the calling code.
                credentials.scram.iterationCount = scramElement.Obj()["iterationCount"].numberInt();
                uassert(17501,
                        "Invalid or missing SCRAM iteration count",
                        credentials.scram.iterationCount > 0);

                credentials.scram.salt = scramElement.Obj()["salt"].str();
                uassert(17502, "Missing SCRAM salt", !credentials.scram.salt.empty());

                credentials.scram.serverKey = scramElement["serverKey"].str();
                uassert(17503, "Missing SCRAM serverKey", !credentials.scram.serverKey.empty());

                credentials.scram.storedKey = scramElement["storedKey"].str();
                uassert(17504, "Missing SCRAM storedKey", !credentials.scram.storedKey.empty());
            }

            if (!mongoCRCredentialElement.eoo()) {
                if (mongoCRCredentialElement.type() != String ||
                    mongoCRCredentialElement.valueStringData().empty()) {
                    return Status(ErrorCodes::UnsupportedFormat,
                                  "MONGODB-CR credentials must be non-empty strings");
                } else {
                    credentials.password = mongoCRCredentialElement.String();
                    if (credentials.password.empty()) {
                        return Status(ErrorCodes::UnsupportedFormat,
                                      "User documents must provide authentication credentials");
                    }
                }
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

    if (roleNameElement->type() != String || roleNameElement->valueStringData().empty()) {
        return Status(ErrorCodes::UnsupportedFormat, "Role names must be non-empty strings");
    }
    if (roleSourceElement->type() != String || roleSourceElement->valueStringData().empty()) {
        return Status(ErrorCodes::UnsupportedFormat, "Role db must be non-empty strings");
    }

    return Status::OK();
}

Status V2UserDocumentParser::checkValidRoleObject(const BSONObj& roleObject) {
    BSONElement roleNameElement;
    BSONElement roleSourceElement;
    return _extractRoleDocumentElements(roleObject, &roleNameElement, &roleSourceElement);
}

Status V2UserDocumentParser::parseRoleName(const BSONObj& roleObject, RoleName* result) {
    BSONElement roleNameElement;
    BSONElement roleSourceElement;
    Status status = _extractRoleDocumentElements(roleObject, &roleNameElement, &roleSourceElement);
    if (!status.isOK())
        return status;
    *result = RoleName(roleNameElement.str(), roleSourceElement.str());
    return status;
}

Status V2UserDocumentParser::parseRoleVector(const BSONArray& rolesArray,
                                             std::vector<RoleName>* result) {
    std::vector<RoleName> roles;
    for (BSONObjIterator it(rolesArray); it.more(); it.next()) {
        if ((*it).type() != Object) {
            return Status(ErrorCodes::TypeMismatch, "Roles must be objects.");
        }
        RoleName role;
        Status status = parseRoleName((*it).Obj(), &role);
        if (!status.isOK())
            return status;
        roles.push_back(role);
    }
    std::swap(*result, roles);
    return Status::OK();
}

Status V2UserDocumentParser::initializeAuthenticationRestrictionsFromUserDocument(
    const BSONObj& privDoc, User* user) const {
    RestrictionDocuments::sequence_type restrictionVector;

    // Restrictions on the user
    const auto authenticationRestrictions = privDoc[AUTHENTICATION_RESTRICTIONS_FIELD_NAME];
    if (!authenticationRestrictions.eoo()) {
        if (authenticationRestrictions.type() != Array) {
            return Status(ErrorCodes::UnsupportedFormat,
                          "'authenticationRestrictions' field must be an array");
        }

        auto restrictions =
            parseAuthenticationRestriction(BSONArray(authenticationRestrictions.Obj()));
        if (!restrictions.isOK()) {
            return restrictions.getStatus();
        }

        restrictionVector.push_back(restrictions.getValue());
    }

    // Restrictions from roles
    const auto inherited = privDoc[INHERITED_AUTHENTICATION_RESTRICTIONS_FIELD_NAME];
    if (!inherited.eoo()) {
        if (inherited.type() != Array) {
            return Status(ErrorCodes::UnsupportedFormat,
                          "'inheritedAuthenticationRestrictions' field must be an array");
        }

        for (const auto& roleRestriction : BSONArray(inherited.Obj())) {
            if (roleRestriction.type() != Array) {
                return Status(ErrorCodes::UnsupportedFormat,
                              "'inheritedAuthenticationRestrictions' sub-fields must be arrays");
            }

            auto roleRestrictionDoc =
                parseAuthenticationRestriction(BSONArray(roleRestriction.Obj()));
            if (!roleRestrictionDoc.isOK()) {
                return roleRestrictionDoc.getStatus();
            }

            restrictionVector.push_back(roleRestrictionDoc.getValue());
        }
    }

    if (user) {
        user->setRestrictions(RestrictionDocuments(restrictionVector));
    }

    return Status::OK();
}

Status V2UserDocumentParser::initializeUserRolesFromUserDocument(const BSONObj& privDoc,
                                                                 User* user) const {
    BSONElement rolesElement = privDoc[ROLES_FIELD_NAME];

    if (rolesElement.type() != Array) {
        return Status(ErrorCodes::UnsupportedFormat,
                      "User document needs 'roles' field to be an array");
    }

    std::vector<RoleName> roles;
    for (BSONObjIterator it(rolesElement.Obj()); it.more(); it.next()) {
        if ((*it).type() != Object) {
            return Status(ErrorCodes::UnsupportedFormat,
                          "User document needs values in 'roles' array to be a sub-documents");
        }
        BSONObj roleObject = (*it).Obj();

        RoleName role;
        Status status = parseRoleName(roleObject, &role);
        if (!status.isOK()) {
            return status;
        }
        roles.push_back(role);
    }
    user->setRoles(makeRoleNameIteratorForContainer(roles));
    return Status::OK();
}

Status V2UserDocumentParser::initializeUserIndirectRolesFromUserDocument(const BSONObj& privDoc,
                                                                         User* user) const {
    BSONElement indirectRolesElement = privDoc[INHERITED_ROLES_FIELD_NAME];

    if (indirectRolesElement.type() != Array) {
        return Status(ErrorCodes::UnsupportedFormat,
                      "User document needs 'inheritedRoles' field to be an array");
    }

    std::vector<RoleName> indirectRoles;
    for (BSONObjIterator it(indirectRolesElement.Obj()); it.more(); it.next()) {
        if ((*it).type() != Object) {
            return Status(ErrorCodes::UnsupportedFormat,
                          "User document needs values in 'inheritedRoles'"
                          " array to be a sub-documents");
        }
        BSONObj indirectRoleObject = (*it).Obj();

        RoleName indirectRole;
        Status status = parseRoleName(indirectRoleObject, &indirectRole);
        if (!status.isOK()) {
            return status;
        }
        indirectRoles.push_back(indirectRole);
    }
    user->setIndirectRoles(makeRoleNameIteratorForContainer(indirectRoles));
    return Status::OK();
}

Status V2UserDocumentParser::initializeUserPrivilegesFromUserDocument(const BSONObj& doc,
                                                                      User* user) const {
    BSONElement privilegesElement = doc[PRIVILEGES_FIELD_NAME];
    if (privilegesElement.eoo())
        return Status::OK();
    if (privilegesElement.type() != Array) {
        return Status(ErrorCodes::UnsupportedFormat,
                      "User document 'inheritedPrivileges' element must be Array if present.");
    }
    PrivilegeVector privileges;
    std::string errmsg;
    for (BSONObjIterator it(privilegesElement.Obj()); it.more(); it.next()) {
        if ((*it).type() != Object) {
            warning() << "Wrong type of element in inheritedPrivileges array for "
                      << user->getName() << ": " << *it;
            continue;
        }
        Privilege privilege;
        ParsedPrivilege pp;
        if (!pp.parseBSON((*it).Obj(), &errmsg)) {
            warning() << "Could not parse privilege element in user document for "
                      << user->getName() << ": " << errmsg;
            continue;
        }
        std::vector<std::string> unrecognizedActions;
        Status status =
            ParsedPrivilege::parsedPrivilegeToPrivilege(pp, &privilege, &unrecognizedActions);
        if (!status.isOK()) {
            warning() << "Could not parse privilege element in user document for "
                      << user->getName() << causedBy(status);
            continue;
        }
        if (unrecognizedActions.size()) {
            std::string unrecognizedActionsString;
            joinStringDelim(unrecognizedActions, &unrecognizedActionsString, ',');
            warning() << "Encountered unrecognized actions \" " << unrecognizedActionsString
                      << "\" while parsing user document for " << user->getName();
        }
        privileges.push_back(privilege);
    }
    user->setPrivileges(privileges);
    return Status::OK();
}

}  // namespace mongo
