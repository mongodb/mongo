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
    const std::string LOCAL_DBNAME = "local";

    const std::string ROLES_FIELD_NAME = "roles";
    const std::string OTHER_DB_ROLES_FIELD_NAME = "otherDBRoles";
    const std::string READONLY_FIELD_NAME = "readOnly";
    const std::string CREDENTIALS_FIELD_NAME = "credentials";
    const std::string DELEGATABLE_ROLES_FIELD_NAME = "delegatableRoles";
    const std::string ROLE_NAME_FIELD_NAME = "name";
    const std::string ROLE_SOURCE_FIELD_NAME = "source";
    const std::string MONGODB_CR_CREDENTIAL_FIELD_NAME = "MONGODB-CR";

    const std::string SYSTEM_ROLE_READ = "read";
    const std::string SYSTEM_ROLE_READ_WRITE = "readWrite";
    const std::string SYSTEM_ROLE_USER_ADMIN = "userAdmin";
    const std::string SYSTEM_ROLE_DB_ADMIN = "dbAdmin";
    const std::string SYSTEM_ROLE_CLUSTER_ADMIN = "clusterAdmin";
    const std::string SYSTEM_ROLE_READ_ANY_DB = "readAnyDatabase";
    const std::string SYSTEM_ROLE_READ_WRITE_ANY_DB = "readWriteAnyDatabase";
    const std::string SYSTEM_ROLE_USER_ADMIN_ANY_DB = "userAdminAnyDatabase";
    const std::string SYSTEM_ROLE_DB_ADMIN_ANY_DB = "dbAdminAnyDatabase";

    // System roles for backwards compatibility with 2.2 and prior
    const std::string SYSTEM_ROLE_V0_READ = "oldRead";
    const std::string SYSTEM_ROLE_V0_READ_WRITE= "oldReadWrite";
    const std::string SYSTEM_ROLE_V0_ADMIN_READ = "oldAdminRead";
    const std::string SYSTEM_ROLE_V0_ADMIN_READ_WRITE= "oldAdminReadWrite";

    // ActionSets for the various system roles.  These ActionSets contain all the actions that
    // a user of each system role is granted.
    ActionSet readRoleActions;
    ActionSet readWriteRoleActions;
    ActionSet userAdminRoleActions;
    ActionSet dbAdminRoleActions;
    ActionSet clusterAdminRoleActions;
    // Can only be performed by internal connections.  Nothing ever explicitly grants these actions,
    // but they're included when calling addAllActions on an ActionSet, which is how internal
    // connections are granted their privileges.
    ActionSet internalActions;
    // Old-style user roles
    ActionSet compatibilityReadOnlyActions;
    ActionSet compatibilityReadWriteActions;
    ActionSet compatibilityReadOnlyAdminActions;
    ActionSet compatibilityReadWriteAdminActions;

}  // namespace

    // This sets up the system role ActionSets.  This is what determines what actions each role
    // is authorized to perform
    MONGO_INITIALIZER(AuthorizationSystemRoles)(InitializerContext* context) {
        // Read role
        readRoleActions.addAction(ActionType::cloneCollectionLocalSource);
        readRoleActions.addAction(ActionType::collStats);
        readRoleActions.addAction(ActionType::dbHash);
        readRoleActions.addAction(ActionType::dbStats);
        readRoleActions.addAction(ActionType::find);
        readRoleActions.addAction(ActionType::indexRead);
        readRoleActions.addAction(ActionType::killCursors);

        // Read-write role
        readWriteRoleActions.addAllActionsFromSet(readRoleActions);
        readWriteRoleActions.addAction(ActionType::cloneCollectionTarget);
        readWriteRoleActions.addAction(ActionType::convertToCapped);
        readWriteRoleActions.addAction(ActionType::createCollection); // db admin gets this also
        readWriteRoleActions.addAction(ActionType::dropCollection);
        readWriteRoleActions.addAction(ActionType::dropIndexes);
        readWriteRoleActions.addAction(ActionType::emptycapped);
        readWriteRoleActions.addAction(ActionType::ensureIndex);
        readWriteRoleActions.addAction(ActionType::insert);
        readWriteRoleActions.addAction(ActionType::remove);
        readWriteRoleActions.addAction(ActionType::renameCollectionSameDB); // db admin gets this also
        readWriteRoleActions.addAction(ActionType::update);

        // User admin role
        userAdminRoleActions.addAction(ActionType::userAdmin);

        // DB admin role
        dbAdminRoleActions.addAction(ActionType::clean);
        dbAdminRoleActions.addAction(ActionType::cloneCollectionLocalSource);
        dbAdminRoleActions.addAction(ActionType::collMod);
        dbAdminRoleActions.addAction(ActionType::collStats);
        dbAdminRoleActions.addAction(ActionType::compact);
        dbAdminRoleActions.addAction(ActionType::convertToCapped);
        dbAdminRoleActions.addAction(ActionType::createCollection); // read_write gets this also
        dbAdminRoleActions.addAction(ActionType::dbStats);
        dbAdminRoleActions.addAction(ActionType::dropCollection);
        dbAdminRoleActions.addAction(ActionType::dropIndexes);
        dbAdminRoleActions.addAction(ActionType::ensureIndex);
        dbAdminRoleActions.addAction(ActionType::indexRead);
        dbAdminRoleActions.addAction(ActionType::indexStats);
        dbAdminRoleActions.addAction(ActionType::profileEnable);
        dbAdminRoleActions.addAction(ActionType::profileRead);
        dbAdminRoleActions.addAction(ActionType::reIndex);
        dbAdminRoleActions.addAction(ActionType::renameCollectionSameDB); // read_write gets this also
        dbAdminRoleActions.addAction(ActionType::storageDetails);
        dbAdminRoleActions.addAction(ActionType::validate);

        // We separate clusterAdmin read-only and read-write actions for backwards
        // compatibility with old-style read-only admin users.  This separation is not exposed to
        // the user, and could go away once we stop supporting old-style privilege documents.
        ActionSet clusterAdminRoleReadActions;
        ActionSet clusterAdminRoleWriteActions;

        // Cluster admin role
        clusterAdminRoleReadActions.addAction(ActionType::connPoolStats);
        clusterAdminRoleReadActions.addAction(ActionType::connPoolSync);
        clusterAdminRoleReadActions.addAction(ActionType::getCmdLineOpts);
        clusterAdminRoleReadActions.addAction(ActionType::getLog);
        clusterAdminRoleReadActions.addAction(ActionType::getParameter);
        clusterAdminRoleReadActions.addAction(ActionType::getShardMap);
        clusterAdminRoleReadActions.addAction(ActionType::getShardVersion);
        clusterAdminRoleReadActions.addAction(ActionType::hostInfo);
        clusterAdminRoleReadActions.addAction(ActionType::listDatabases);
        clusterAdminRoleReadActions.addAction(ActionType::listShards);
        clusterAdminRoleReadActions.addAction(ActionType::logRotate);
        clusterAdminRoleReadActions.addAction(ActionType::netstat);
        clusterAdminRoleReadActions.addAction(ActionType::replSetFreeze);
        clusterAdminRoleReadActions.addAction(ActionType::replSetGetStatus);
        clusterAdminRoleReadActions.addAction(ActionType::replSetMaintenance);
        clusterAdminRoleReadActions.addAction(ActionType::replSetStepDown);
        clusterAdminRoleReadActions.addAction(ActionType::replSetSyncFrom);
        clusterAdminRoleReadActions.addAction(ActionType::setParameter);
        clusterAdminRoleReadActions.addAction(ActionType::setShardVersion); // TODO: should this be internal?
        clusterAdminRoleReadActions.addAction(ActionType::serverStatus);
        clusterAdminRoleReadActions.addAction(ActionType::splitVector);
        // Shutdown is in read actions b/c that's how it was in 2.2
        clusterAdminRoleReadActions.addAction(ActionType::shutdown);
        clusterAdminRoleReadActions.addAction(ActionType::top);
        clusterAdminRoleReadActions.addAction(ActionType::touch);
        clusterAdminRoleReadActions.addAction(ActionType::unlock);
        clusterAdminRoleReadActions.addAction(ActionType::unsetSharding);
        clusterAdminRoleReadActions.addAction(ActionType::writeBacksQueued);

        clusterAdminRoleWriteActions.addAction(ActionType::addShard);
        clusterAdminRoleWriteActions.addAction(ActionType::cleanupOrphaned);
        clusterAdminRoleWriteActions.addAction(ActionType::closeAllDatabases);
        clusterAdminRoleWriteActions.addAction(ActionType::cpuProfiler);
        clusterAdminRoleWriteActions.addAction(ActionType::cursorInfo);
        clusterAdminRoleWriteActions.addAction(ActionType::diagLogging);
        clusterAdminRoleWriteActions.addAction(ActionType::dropDatabase); // TODO: Should there be a CREATE_DATABASE also?
        clusterAdminRoleWriteActions.addAction(ActionType::enableSharding);
        clusterAdminRoleWriteActions.addAction(ActionType::flushRouterConfig);
        clusterAdminRoleWriteActions.addAction(ActionType::fsync);
        clusterAdminRoleWriteActions.addAction(ActionType::inprog);
        clusterAdminRoleWriteActions.addAction(ActionType::killop);
        clusterAdminRoleWriteActions.addAction(ActionType::mergeChunks);
        clusterAdminRoleWriteActions.addAction(ActionType::moveChunk);
        clusterAdminRoleWriteActions.addAction(ActionType::movePrimary);
        clusterAdminRoleWriteActions.addAction(ActionType::removeShard);
        clusterAdminRoleWriteActions.addAction(ActionType::repairDatabase);
        clusterAdminRoleWriteActions.addAction(ActionType::replSetInitiate);
        clusterAdminRoleWriteActions.addAction(ActionType::replSetReconfig);
        clusterAdminRoleWriteActions.addAction(ActionType::resync);
        clusterAdminRoleWriteActions.addAction(ActionType::shardCollection);
        clusterAdminRoleWriteActions.addAction(ActionType::shardingState);
        clusterAdminRoleWriteActions.addAction(ActionType::split);
        clusterAdminRoleWriteActions.addAction(ActionType::splitChunk);

        clusterAdminRoleActions.addAllActionsFromSet(clusterAdminRoleReadActions);
        clusterAdminRoleActions.addAllActionsFromSet(clusterAdminRoleWriteActions);
        clusterAdminRoleActions.addAction(ActionType::killCursors);

        // Old-style user actions, for backwards compatibility
        compatibilityReadOnlyActions.addAllActionsFromSet(readRoleActions);

        compatibilityReadWriteActions.addAllActionsFromSet(readWriteRoleActions);
        compatibilityReadWriteActions.addAllActionsFromSet(dbAdminRoleActions);
        compatibilityReadWriteActions.addAllActionsFromSet(userAdminRoleActions);
        compatibilityReadWriteActions.addAction(ActionType::clone);
        compatibilityReadWriteActions.addAction(ActionType::copyDBTarget);
        compatibilityReadWriteActions.addAction(ActionType::dropDatabase);
        compatibilityReadWriteActions.addAction(ActionType::repairDatabase);

        compatibilityReadOnlyAdminActions.addAllActionsFromSet(compatibilityReadOnlyActions);
        compatibilityReadOnlyAdminActions.addAllActionsFromSet(clusterAdminRoleReadActions);

        compatibilityReadWriteAdminActions.addAllActionsFromSet(compatibilityReadWriteActions);
        compatibilityReadWriteAdminActions.addAllActionsFromSet(compatibilityReadOnlyAdminActions);
        compatibilityReadWriteAdminActions.addAllActionsFromSet(clusterAdminRoleWriteActions);

        // Internal commands
        internalActions.addAction(ActionType::clone);
        internalActions.addAction(ActionType::handshake);
        internalActions.addAction(ActionType::mapReduceShardedFinish);
        internalActions.addAction(ActionType::replSetElect);
        internalActions.addAction(ActionType::replSetFresh);
        internalActions.addAction(ActionType::replSetGetRBID);
        internalActions.addAction(ActionType::replSetHeartbeat);
        internalActions.addAction(ActionType::writebacklisten);
        internalActions.addAction(ActionType::userAdminV1);
        internalActions.addAction(ActionType::_migrateClone);
        internalActions.addAction(ActionType::_recvChunkAbort);
        internalActions.addAction(ActionType::_recvChunkCommit);
        internalActions.addAction(ActionType::_recvChunkStart);
        internalActions.addAction(ActionType::_recvChunkStatus);
        internalActions.addAction(ActionType::_transferMods);

        return Status::OK();
    }

    static inline Status _oldPrivilegeFormatNotSupported() {
        return Status(ErrorCodes::UnsupportedFormat,
                      "Support for compatibility-form privilege documents disabled; "
                      "All system.users entries must contain a 'roles' field");
    }

    static inline Status _badValue(const char* reason, int location) {
        return Status(ErrorCodes::BadValue, reason, location);
    }

    static inline Status _badValue(const std::string& reason, int location) {
        return Status(ErrorCodes::BadValue, reason, location);
    }

    static inline StringData makeStringDataFromBSONElement(const BSONElement& element) {
        return StringData(element.valuestr(), element.valuestrsize() - 1);
    }

    ActionSet PrivilegeDocumentParser::getAllUserActions() const {
        ActionSet allActions;
        allActions.addAllActionsFromSet(readRoleActions);
        allActions.addAllActionsFromSet(readWriteRoleActions);
        allActions.addAllActionsFromSet(userAdminRoleActions);
        allActions.addAllActionsFromSet(dbAdminRoleActions);
        allActions.addAllActionsFromSet(clusterAdminRoleActions);
        return allActions;
    }

    Status PrivilegeDocumentParser::initializeUserFromPrivilegeDocument(
            User* user, const BSONObj& privDoc) const {
        std::string userName = privDoc[AuthorizationManager::USER_NAME_FIELD_NAME].str();
        if (userName != user->getName().getUser()) {
            return Status(ErrorCodes::BadValue,
                          mongoutils::str::stream() << "User name from privilege document \""
                                  << userName
                                  << "\" doesn't match name of provided User \""
                                  << user->getName().getUser()
                                  << "\"",
                          0);
        }

        Status status = initializeUserCredentialsFromPrivilegeDocument(user, privDoc);
        if (!status.isOK()) {
            return status;
        }
        status = initializeUserRolesFromPrivilegeDocument(user, privDoc, user->getName().getDB());
        if (!status.isOK()) {
            return status;
        }
        initializeUserPrivilegesFromRoles(user);
        return Status::OK();
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

    Status V1PrivilegeDocumentParser::checkValidPrivilegeDocument(const StringData& dbname,
                                                                  const BSONObj& doc) const {
        BSONElement userElement = doc[AuthorizationManager::USER_NAME_FIELD_NAME];
        BSONElement userSourceElement = doc[AuthorizationManager::USER_SOURCE_FIELD_NAME];
        BSONElement passwordElement = doc[AuthorizationManager::PASSWORD_FIELD_NAME];
        BSONElement rolesElement = doc[ROLES_FIELD_NAME];
        BSONElement otherDBRolesElement = doc[OTHER_DB_ROLES_FIELD_NAME];
        BSONElement readOnlyElement = doc[READONLY_FIELD_NAME];

        // Validate the "user" element.
        if (userElement.type() != String)
            return _badValue("system.users entry needs 'user' field to be a string", 14051);
        if (makeStringDataFromBSONElement(userElement).empty())
            return _badValue("system.users entry needs 'user' field to be non-empty", 14053);

        // Must set exactly one of "userSource" and "pwd" fields.
        if (userSourceElement.eoo() == passwordElement.eoo()) {
            return _badValue("system.users entry must have either a 'pwd' field or a 'userSource' "
                             "field, but not both", 0);
        }

        if (!AuthorizationManager::getSupportOldStylePrivilegeDocuments() && rolesElement.eoo()) {
            return _oldPrivilegeFormatNotSupported();
        }

        // Cannot have both "roles" and "readOnly" elements.
        if (!rolesElement.eoo() && !readOnlyElement.eoo()) {
            return _badValue("system.users entry must not have both 'roles' and 'readOnly' fields",
                             0);
        }

        // Validate the "pwd" element, if present.
        if (!passwordElement.eoo()) {
            if (passwordElement.type() != String)
                return _badValue("system.users entry needs 'pwd' field to be a string", 14052);
            if (makeStringDataFromBSONElement(passwordElement).empty())
                return _badValue("system.users entry needs 'pwd' field to be non-empty", 14054);
        }

        // Validate the "userSource" element, if present.
        if (!userSourceElement.eoo()) {
            if (userSourceElement.type() != String ||
                makeStringDataFromBSONElement(userSourceElement).empty()) {

                return _badValue("system.users entry needs 'userSource' field to be a non-empty "
                                 "string, if present", 0);
            }
            if (userSourceElement.str() == dbname) {
                return _badValue(mongoutils::str::stream() << "'" << dbname <<
                                 "' is not a valid value for the userSource field in " <<
                                 dbname << ".system.users entries",
                                 0);
            }
            if (rolesElement.eoo()) {
                return _badValue("system.users entry needs 'roles' field if 'userSource' field "
                                 "is present.", 0);
            }
        }

        // Validate the "roles" element.
        if (!rolesElement.eoo()) {
            Status status = _checkV1RolesArray(rolesElement);
            if (!status.isOK())
                return status;
        }

        if (!otherDBRolesElement.eoo()) {
            if (dbname != ADMIN_DBNAME) {
                return _badValue("Only admin.system.users entries may contain 'otherDBRoles' "
                                 "fields", 0);
            }
            if (rolesElement.eoo()) {
                return _badValue("system.users entries with 'otherDBRoles' fields must contain "
                                 "'roles' fields", 0);
            }
            if (otherDBRolesElement.type() != Object) {
                return _badValue("'otherDBRoles' field must be an object when present in "
                                 "system.users entries", 0);
            }
            for (BSONObjIterator iter(otherDBRolesElement.embeddedObject());
                 iter.more(); iter.next()) {

                Status status = _checkV1RolesArray(*iter);
                if (!status.isOK())
                    return status;
            }
        }

        return Status::OK();
    }

    Status V1PrivilegeDocumentParser::initializeUserCredentialsFromPrivilegeDocument(
            User* user, const BSONObj& privDoc) const {
        User::CredentialData credentials;
        if (privDoc.hasField(AuthorizationManager::PASSWORD_FIELD_NAME)) {
            credentials.password = privDoc[AuthorizationManager::PASSWORD_FIELD_NAME].String();
            credentials.isExternal = false;
        }
        else if (privDoc.hasField(AuthorizationManager::USER_SOURCE_FIELD_NAME)) {
            std::string userSource = privDoc[AuthorizationManager::USER_SOURCE_FIELD_NAME].String();
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
                user->addRole(RoleName(SYSTEM_ROLE_V0_ADMIN_READ, "admin"));
            } else {
                user->addRole(RoleName(SYSTEM_ROLE_V0_ADMIN_READ_WRITE, "admin"));
            }
        } else {
            if (readOnly) {
                user->addRole(RoleName(SYSTEM_ROLE_V0_READ, dbname));
            } else {
                user->addRole(RoleName(SYSTEM_ROLE_V0_READ_WRITE, dbname));
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

    /**
     * Adds to "outPrivileges" the privileges associated with having the named "role" on "dbname".
     *
     * Returns non-OK status if "role" is not a defined role in "dbname".
     */
    void _addPrivilegesForSystemRole(const std::string& dbname,
                                     const std::string& role,
                                     std::vector<Privilege>* outPrivileges) {
        const bool isAdminDB = (dbname == ADMIN_DBNAME);

        if (role == SYSTEM_ROLE_READ) {
            outPrivileges->push_back(Privilege(dbname, readRoleActions));
        }
        else if (role == SYSTEM_ROLE_READ_WRITE) {
            outPrivileges->push_back(Privilege(dbname, readWriteRoleActions));
        }
        else if (role == SYSTEM_ROLE_USER_ADMIN) {
            outPrivileges->push_back(Privilege(dbname, userAdminRoleActions));
        }
        else if (role == SYSTEM_ROLE_DB_ADMIN) {
            outPrivileges->push_back(Privilege(dbname, dbAdminRoleActions));
        }
        else if (role == SYSTEM_ROLE_V0_READ) {
            outPrivileges->push_back(Privilege(dbname, compatibilityReadOnlyActions));
        }
        else if (role == SYSTEM_ROLE_V0_READ_WRITE) {
            outPrivileges->push_back(Privilege(dbname, compatibilityReadWriteActions));
        }
        else if (isAdminDB && role == SYSTEM_ROLE_READ_ANY_DB) {
            outPrivileges->push_back(Privilege(AuthorizationManager::WILDCARD_RESOURCE_NAME,
                                               readRoleActions));
        }
        else if (isAdminDB && role == SYSTEM_ROLE_READ_WRITE_ANY_DB) {
            outPrivileges->push_back(
                    Privilege(AuthorizationManager::WILDCARD_RESOURCE_NAME, readWriteRoleActions));
        }
        else if (isAdminDB && role == SYSTEM_ROLE_USER_ADMIN_ANY_DB) {
            outPrivileges->push_back(
                    Privilege(AuthorizationManager::WILDCARD_RESOURCE_NAME, userAdminRoleActions));
        }
        else if (isAdminDB && role == SYSTEM_ROLE_DB_ADMIN_ANY_DB) {
            outPrivileges->push_back(
                    Privilege(AuthorizationManager::WILDCARD_RESOURCE_NAME, dbAdminRoleActions));
        }
        else if (isAdminDB && role == SYSTEM_ROLE_CLUSTER_ADMIN) {
            outPrivileges->push_back(
                    Privilege(AuthorizationManager::WILDCARD_RESOURCE_NAME,
                              clusterAdminRoleActions));
        }
        else if (isAdminDB && role == SYSTEM_ROLE_V0_ADMIN_READ) {
            outPrivileges->push_back(
                    Privilege(AuthorizationManager::WILDCARD_RESOURCE_NAME,
                              compatibilityReadOnlyAdminActions));
        }
        else if (isAdminDB && role == SYSTEM_ROLE_V0_ADMIN_READ_WRITE) {
            outPrivileges->push_back(
                    Privilege(AuthorizationManager::WILDCARD_RESOURCE_NAME,
                              compatibilityReadWriteAdminActions));
        }
        else {
            warning() << "No such role, \"" << role << "\", in database " << dbname <<
                    ". No privileges will be acquired from this role" << endl;
        }
    }

    /**
     * Modifies the given User object by inspecting its roles and giving it the relevant
     * privileges from those roles.
     */
    void V1PrivilegeDocumentParser::initializeUserPrivilegesFromRoles(User* user) const {
        std::vector<Privilege> privileges;

        RoleNameIterator it = user->getRoles();
        while (it.more()) {
            const RoleName& roleName = it.next();
            _addPrivilegesForSystemRole(roleName.getDB().toString(),
                                        roleName.getRole().toString(),
                                        &privileges);
        }
        user->addPrivileges(privileges);
    }

    Status _checkV2RolesArray(const BSONElement& rolesElement) {
        StringData fieldName = rolesElement.fieldNameStringData();

        if (rolesElement.eoo()) {
            return _badValue(mongoutils::str::stream() << "User document needs '" << fieldName <<
                                     "' field to be provided",
                             0);
        }
        if (rolesElement.type() != Array) {
            return _badValue(mongoutils::str::stream() << fieldName << " field must be an array",
                             0);
        }
        for (BSONObjIterator iter(rolesElement.embeddedObject()); iter.more(); iter.next()) {
            if ((*iter).type() != Object) {
                return _badValue(mongoutils::str::stream() << "Elements in '" << fieldName <<
                                         "' array must objects.",
                                 0);
            }
            BSONObj roleObj = (*iter).Obj();
            BSONElement nameElement = roleObj[ROLE_NAME_FIELD_NAME];
            BSONElement sourceElement = roleObj[ROLE_SOURCE_FIELD_NAME];
            if (nameElement.type() != String ||
                    makeStringDataFromBSONElement(nameElement).empty()) {
                return _badValue(mongoutils::str::stream() << "Entries in '" << fieldName <<
                                         "' array need 'name' field to be a non-empty string",
                                 0);
            }
            if (sourceElement.type() != String ||
                    makeStringDataFromBSONElement(sourceElement).empty()) {
                return _badValue(mongoutils::str::stream() << "Entries in '" << fieldName <<
                                         "' array need 'source' field to be a non-empty string",
                                 0);
            }
        }
        return Status::OK();
    }

    Status V2PrivilegeDocumentParser::checkValidPrivilegeDocument(const StringData& dbname,
                                                                  const BSONObj& doc) const {
        BSONElement userElement = doc[AuthorizationManager::USER_NAME_FIELD_NAME];
        BSONElement userSourceElement = doc[AuthorizationManager::USER_SOURCE_FIELD_NAME];
        BSONElement credentialsElement = doc[CREDENTIALS_FIELD_NAME];
        BSONElement rolesElement = doc[ROLES_FIELD_NAME];
        BSONElement delegatableRolesElement = doc[DELEGATABLE_ROLES_FIELD_NAME];

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
        if (userSourceStr != dbname) {
            return _badValue(mongoutils::str::stream() << "userSource '" << userSourceStr <<
                                     "' does not match database '" << dbname << "'", 0);
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

        // Validate the "delegatableRoles" element.
        status = _checkV2RolesArray(delegatableRolesElement);
        if (!status.isOK())
            return status;

        return Status::OK();
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

            BSONElement roleNameElement = roleObject[ROLE_NAME_FIELD_NAME];
            BSONElement roleSourceElement = roleObject[ROLE_SOURCE_FIELD_NAME];

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

            user->addRole(RoleName(roleNameElement.String(), roleSourceElement.String()));
        }
        return Status::OK();
    }

    void V2PrivilegeDocumentParser::initializeUserPrivilegesFromRoles(User* user) const {
        // NOT YET IMPELEMENTED
    }

} // namespace mongo
