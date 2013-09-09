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

#include "mongo/db/auth/privilege_document_parser.h"

#include <string>

#include "mongo/base/init.h"
#include "mongo/base/status.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/user.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/namespace_string.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

namespace {
    const std::string ADMIN_DBNAME = "admin";

    const std::string ROLES_FIELD_NAME = "roles";
    const std::string OTHER_DB_ROLES_FIELD_NAME = "otherDBRoles";
    const std::string READONLY_FIELD_NAME = "readOnly";
    const std::string CREDENTIALS_FIELD_NAME = "credentials";
    const std::string ROLE_NAME_FIELD_NAME = "name";
    const std::string ROLE_SOURCE_FIELD_NAME = "source";
    const std::string ROLE_CAN_DELEGATE_FIELD_NAME = "canDelegate";
    const std::string ROLE_HAS_ROLE_FIELD_NAME = "hasRole";
    const std::string MONGODB_CR_CREDENTIAL_FIELD_NAME = "MONGODB-CR";
}  // namespace

    static inline Status _badValue(const char* reason, int location) {
        return Status(ErrorCodes::BadValue, reason, location);
    }

    static inline Status _badValue(const std::string& reason, int location) {
        return Status(ErrorCodes::BadValue, reason, location);
    }

    static inline StringData makeStringDataFromBSONElement(const BSONElement& element) {
        return StringData(element.valuestr(), element.valuestrsize() - 1);
    }

    Status _checkV1RolesArray(const BSONElement& rolesElement) {
        if (rolesElement.type() != Array) {
            return _badValue("Role fields must be an array when present in system.users entries",
                             0);
        }
        for (BSONObjIterator iter(rolesElement.embeddedObject()); iter.more(); iter.next()) {
            BSONElement element = *iter;
            if (element.type() != String || makeStringDataFromBSONElement(element).empty()) {
                return _badValue("Roles must be non-empty strings.", 0);
            }
        }
        return Status::OK();
    }

    std::string V1PrivilegeDocumentParser::extractUserNameFromPrivilegeDocument(
            const BSONObj& doc) const {
        return doc[AuthorizationManager::V1_USER_NAME_FIELD_NAME].str();
    }

    Status V1PrivilegeDocumentParser::initializeUserCredentialsFromPrivilegeDocument(
            User* user, const BSONObj& privDoc) const {
        User::CredentialData credentials;
        if (privDoc.hasField(AuthorizationManager::PASSWORD_FIELD_NAME)) {
            credentials.password = privDoc[AuthorizationManager::PASSWORD_FIELD_NAME].String();
            credentials.isExternal = false;
        }
        else if (privDoc.hasField(AuthorizationManager::V1_USER_SOURCE_FIELD_NAME)) {
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

    void _initializeUserRolesFromV0PrivilegeDocument(
            User* user, const BSONObj& privDoc, const StringData& dbname) {
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
                                                const StringData& dbname) {
        static const char privilegesTypeMismatchMessage[] =
                "Roles in V1 user documents must be enumerated in an array of strings.";

        if (dbname == AuthorizationManager::WILDCARD_RESOURCE_NAME) {
            return Status(ErrorCodes::BadValue,
                          AuthorizationManager::WILDCARD_RESOURCE_NAME +
                                  " is an invalid database name.");
        }

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

    Status _initializeUserRolesFromV1PrivilegeDocument(
                User* user, const BSONObj& privDoc, const StringData& dbname) {

        if (!privDoc[READONLY_FIELD_NAME].eoo()) {
            return Status(ErrorCodes::UnsupportedFormat,
                          "Privilege documents may not contain both \"readonly\" and "
                          "\"roles\" fields");
        }

        Status status = _initializeUserRolesFromV1RolesArray(user,
                                                             privDoc[ROLES_FIELD_NAME],
                                                             dbname);
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
                for (BSONObjIterator iter(otherDbPrivileges.embeddedObject());
                     iter.more(); iter.next()) {

                    BSONElement rolesElement = *iter;
                    status = _initializeUserRolesFromV1RolesArray(user,
                                                                  rolesElement,
                                                                  rolesElement.fieldName());
                    if (!status.isOK())
                        return status;
                }
                break;
            }
            default:
                return Status(ErrorCodes::TypeMismatch,
                              "Field \"otherDBRoles\" must be an object, if present.");
            }
        }
        else if (!otherDbPrivileges.eoo()) {
            return Status(ErrorCodes::UnsupportedFormat,
                          "Only the admin database may contain a field called \"otherDBRoles\"");
        }

        return Status::OK();
    }

    Status V1PrivilegeDocumentParser::initializeUserRolesFromPrivilegeDocument(
            User* user, const BSONObj& privDoc, const StringData& dbname) const {
        if (!privDoc.hasField("roles")) {
            _initializeUserRolesFromV0PrivilegeDocument(user, privDoc, dbname);
        } else {
            return _initializeUserRolesFromV1PrivilegeDocument(user, privDoc, dbname);
        }
        // TODO(spencer): dassert that if you have a V0 or V1 privilege document that the _version
        // of the system is 1.
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
            BSONObj roleObj = (*iter).Obj();
            BSONElement nameElement = roleObj[ROLE_NAME_FIELD_NAME];
            BSONElement sourceElement = roleObj[ROLE_SOURCE_FIELD_NAME];
            BSONElement canDelegateElement = roleObj[ROLE_CAN_DELEGATE_FIELD_NAME];
            BSONElement hasRoleElement = roleObj[ROLE_HAS_ROLE_FIELD_NAME];

            if (nameElement.type() != String ||
                    makeStringDataFromBSONElement(nameElement).empty()) {
                return _badValue("Entries in 'roles' array need 'name' field to be a non-empty "
                                         "string",
                                 0);
            }
            if (sourceElement.type() != String ||
                    makeStringDataFromBSONElement(sourceElement).empty()) {
                return _badValue("Entries in 'roles' array need 'source' field to be a non-empty "
                                         "string",
                                 0);
            }
            if (canDelegateElement.type() != Bool) {
                return _badValue("Entries in 'roles' array need a 'canDelegate' boolean field",
                                 0);
            }
            if (hasRoleElement.type() != Bool) {
                return _badValue("Entries in 'roles' array need a 'canDelegate' boolean field",
                                 0);
            }
            if (!canDelegateElement.Bool() && !hasRoleElement.Bool()) {
                return _badValue("At least one of 'canDelegate' and 'hasRole' must be true for "
                                         "every role in the 'roles' array",
                                 0);
            }
        }
        return Status::OK();
    }

    Status V2PrivilegeDocumentParser::checkValidPrivilegeDocument(const BSONObj& doc) const {
        BSONElement userElement = doc[AuthorizationManager::USER_NAME_FIELD_NAME];
        BSONElement userSourceElement = doc[AuthorizationManager::USER_SOURCE_FIELD_NAME];
        BSONElement credentialsElement = doc[CREDENTIALS_FIELD_NAME];
        BSONElement rolesElement = doc[ROLES_FIELD_NAME];

        // Validate the "user" element.
        if (userElement.type() != String)
            return _badValue("User document needs 'user' field to be a string", 0);
        if (makeStringDataFromBSONElement(userElement).empty())
            return _badValue("User document needs 'user' field to be non-empty", 0);

        // Validate the "userSource" element
        if (userSourceElement.type() != String ||
                makeStringDataFromBSONElement(userSourceElement).empty()) {
            return _badValue("User document needs 'userSource' field to be a non-empty string", 0);
        }
        StringData userSourceStr = makeStringDataFromBSONElement(userSourceElement);
        if (!NamespaceString::validDBName(userSourceStr) && userSourceStr != "$external") {
            return _badValue(mongoutils::str::stream() << "'" << userSourceStr <<
                                     "' is not a valid value for the userSource field.",
                             0);
        }

        // Validate the "credentials" element
        if (credentialsElement.eoo() && userSourceStr != "$external") {
            return _badValue("User document needs 'credentials' field unless userSource is "
                            "'$external'",
                    0);
        }
        if (!credentialsElement.eoo()) {
            if (credentialsElement.type() != Object) {
                return _badValue("User document needs 'credentials' field to be an object", 0);
            }

            BSONObj credentialsObj = credentialsElement.Obj();
            if (credentialsObj.isEmpty()) {
                return _badValue("User document needs 'credentials' field to be a non-empty object",
                                 0);
            }
            BSONElement MongoCRElement = credentialsObj[MONGODB_CR_CREDENTIAL_FIELD_NAME];
            if (!MongoCRElement.eoo() && (MongoCRElement.type() != String ||
                    makeStringDataFromBSONElement(MongoCRElement).empty())) {
                return _badValue("MONGODB-CR credential must to be a non-empty string, if present",
                                 0);
            }
        }

        // Validate the "roles" element.
        Status status = _checkV2RolesArray(rolesElement);
        if (!status.isOK())
            return status;

        return Status::OK();
    }

    std::string V2PrivilegeDocumentParser::extractUserNameFromPrivilegeDocument(
            const BSONObj& doc) const {
        return doc[AuthorizationManager::USER_NAME_FIELD_NAME].str();
    }

    Status V2PrivilegeDocumentParser::initializeUserCredentialsFromPrivilegeDocument(
            User* user, const BSONObj& privDoc) const {
        User::CredentialData credentials;
        std::string userSource = privDoc[AuthorizationManager::USER_SOURCE_FIELD_NAME].String();
        BSONElement credentialsElement = privDoc[CREDENTIALS_FIELD_NAME];
        if (!credentialsElement.eoo()) {
            if (credentialsElement.type() != Object) {
                return Status(ErrorCodes::UnsupportedFormat,
                              "'credentials' field in privilege documents must be an object");
            }
            BSONElement mongoCRCredentialElement =
                    credentialsElement.Obj()[MONGODB_CR_CREDENTIAL_FIELD_NAME];
            if (!mongoCRCredentialElement.eoo()) {
                if (mongoCRCredentialElement.type() != String ||
                        makeStringDataFromBSONElement(mongoCRCredentialElement).empty()) {
                    return Status(ErrorCodes::UnsupportedFormat,
                                  "MONGODB-CR credentials must be non-empty strings");
                } else {
                    credentials.isExternal = false;
                    credentials.password = mongoCRCredentialElement.String();
                }
            } else {
                return Status(ErrorCodes::UnsupportedFormat,
                              "Privilege documents must provide credentials for MONGODB-CR"
                              " authentication");
            }
        }
        else if (userSource == "$external") {
            credentials.isExternal = true;
        } else {
                return Status(ErrorCodes::UnsupportedFormat,
                              "Cannot extract credentials from user documents without a "
                              "'credentials' field and with userSource != \"$external\"");
        }

        user->setCredentials(credentials);
        return Status::OK();
    }

    Status V2PrivilegeDocumentParser::checkValidRoleObject(
            const BSONObj& roleObject) const {
        BSONElement roleNameElement = roleObject[ROLE_NAME_FIELD_NAME];
        BSONElement roleSourceElement = roleObject[ROLE_SOURCE_FIELD_NAME];
        BSONElement canDelegateElement = roleObject[ROLE_CAN_DELEGATE_FIELD_NAME];
        BSONElement hasRoleElement = roleObject[ROLE_HAS_ROLE_FIELD_NAME];

        if (roleNameElement.type() != String ||
                makeStringDataFromBSONElement(roleNameElement).empty()) {
            return Status(ErrorCodes::UnsupportedFormat,
                          "Role names must be non-empty strings");
        }
        if (roleSourceElement.type() != String ||
                makeStringDataFromBSONElement(roleSourceElement).empty()) {
            return Status(ErrorCodes::UnsupportedFormat,
                          "Role source must be non-empty strings");
        }
        if (canDelegateElement.type() != Bool) {
            return Status(ErrorCodes::UnsupportedFormat,
                          "Entries in 'roles' array need a 'canDelegate' boolean field");
        }
        if (hasRoleElement.type() != Bool) {
            return Status(ErrorCodes::UnsupportedFormat,
                          "Entries in 'roles' array need a 'hasRole' boolean field");
        }

        if (!hasRoleElement.Bool() && !canDelegateElement.Bool()) {
            return Status(ErrorCodes::UnsupportedFormat,
                          "At least one of 'canDelegate' and 'hasRole' must be true for "
                          "every role in the 'roles' array");
        }

        return Status::OK();
    }

    Status V2PrivilegeDocumentParser::initializeUserRolesFromPrivilegeDocument(
            User* user, const BSONObj& privDoc, const StringData&) const {

        BSONElement rolesElement = privDoc[ROLES_FIELD_NAME];

        if (rolesElement.type() != Array) {
            return Status(ErrorCodes::UnsupportedFormat,
                          "User document needs 'roles' field to be an array");
        }

        for (BSONObjIterator it(rolesElement.Obj()); it.more(); it.next()) {
            if ((*it).type() != Object) {
                return Status(ErrorCodes::UnsupportedFormat,
                              "User document needs values in 'roles' array to be a sub-documents");
            }
            BSONObj roleObject = (*it).Obj();

            Status status = checkValidRoleObject(roleObject);
            if (!status.isOK()) {
                return status;
            }

            BSONElement roleNameElement = roleObject[ROLE_NAME_FIELD_NAME];
            BSONElement roleSourceElement = roleObject[ROLE_SOURCE_FIELD_NAME];
            BSONElement canDelegateElement = roleObject[ROLE_CAN_DELEGATE_FIELD_NAME];
            BSONElement hasRoleElement = roleObject[ROLE_HAS_ROLE_FIELD_NAME];

            if (hasRoleElement.Bool()) {
                user->addRole(RoleName(roleNameElement.String(), roleSourceElement.String()));
            }
            if (canDelegateElement.Bool()) {
                user->addDelegatableRole(RoleName(roleNameElement.String(),
                                                  roleSourceElement.String()));
            }
        }
        return Status::OK();
    }

} // namespace mongo
