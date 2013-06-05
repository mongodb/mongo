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

#include "mongo/base/init.h"
#include "mongo/base/status.h"
#include "mongo/db/auth/action_set.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/auth/privilege_set.h"
#include "mongo/db/auth/user_name.h"
#include "mongo/db/jsobj.h"
#include "mongo/util/mongoutils/str.h"

#include <string>
#include <vector>

namespace mongo {

    AuthInfo::AuthInfo() {
        user = "__system";
    }
    AuthInfo internalSecurity;

    const std::string AuthorizationManager::SERVER_RESOURCE_NAME = "$SERVER";
    const std::string AuthorizationManager::CLUSTER_RESOURCE_NAME = "$CLUSTER";
    const std::string AuthorizationManager::USER_NAME_FIELD_NAME = "user";
    const std::string AuthorizationManager::USER_SOURCE_FIELD_NAME = "userSource";
    const std::string AuthorizationManager::PASSWORD_FIELD_NAME = "pwd";

namespace {
    const std::string ADMIN_DBNAME = "admin";
    const std::string LOCAL_DBNAME = "local";

    const std::string ROLES_FIELD_NAME = "roles";
    const std::string OTHER_DB_ROLES_FIELD_NAME = "otherDBRoles";
    const std::string READONLY_FIELD_NAME = "readOnly";

    const std::string SYSTEM_ROLE_READ = "read";
    const std::string SYSTEM_ROLE_READ_WRITE = "readWrite";
    const std::string SYSTEM_ROLE_USER_ADMIN = "userAdmin";
    const std::string SYSTEM_ROLE_DB_ADMIN = "dbAdmin";
    const std::string SYSTEM_ROLE_CLUSTER_ADMIN = "clusterAdmin";
    const std::string SYSTEM_ROLE_READ_ANY_DB = "readAnyDatabase";
    const std::string SYSTEM_ROLE_READ_WRITE_ANY_DB = "readWriteAnyDatabase";
    const std::string SYSTEM_ROLE_USER_ADMIN_ANY_DB = "userAdminAnyDatabase";
    const std::string SYSTEM_ROLE_DB_ADMIN_ANY_DB = "dbAdminAnyDatabase";

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
        clusterAdminRoleReadActions.addAction(ActionType::shutdown);
        clusterAdminRoleReadActions.addAction(ActionType::top);
        clusterAdminRoleReadActions.addAction(ActionType::touch);
        clusterAdminRoleReadActions.addAction(ActionType::unlock);
        clusterAdminRoleReadActions.addAction(ActionType::unsetSharding);
        clusterAdminRoleReadActions.addAction(ActionType::writeBacksQueued);

        clusterAdminRoleWriteActions.addAction(ActionType::addShard);
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


    AuthorizationManager::AuthorizationManager(AuthzManagerExternalState* externalState) :
            _externalState(externalState),
            _authEnabled(false),
            _doesSupportOldStylePrivileges(true) {}

    AuthzManagerExternalState* AuthorizationManager::getExternalState() const {
        return _externalState.get();
    }

    void AuthorizationManager::setSupportOldStylePrivilegeDocuments(bool enabled) {
        _doesSupportOldStylePrivileges = enabled;
    }

    bool AuthorizationManager::getSupportOldStylePrivilegeDocuments() const {
        return _doesSupportOldStylePrivileges;
    }

    void AuthorizationManager::setAuthEnabled(bool enabled) {
        _authEnabled = enabled;
    }

    bool AuthorizationManager::isAuthEnabled() const {
        return _authEnabled;
    }

    Status AuthorizationManager::getPrivilegeDocument(const std::string& dbname,
                                                      const UserName& userName,
                                                      BSONObj* result) const {
        return _externalState->getPrivilegeDocument(dbname, userName, result);
    }

    bool AuthorizationManager::hasPrivilegeDocument(const std::string& dbname) const {
        return _externalState->hasPrivilegeDocument(dbname);
    }

    ActionSet AuthorizationManager::getAllUserActions() const {
        ActionSet allActions;
        allActions.addAllActionsFromSet(readRoleActions);
        allActions.addAllActionsFromSet(readWriteRoleActions);
        allActions.addAllActionsFromSet(userAdminRoleActions);
        allActions.addAllActionsFromSet(dbAdminRoleActions);
        allActions.addAllActionsFromSet(clusterAdminRoleActions);
        return allActions;
    }

    ActionSet AuthorizationManager::getActionsForOldStyleUser(const std::string& dbname,
                                                              bool readOnly) const {
        if (dbname == ADMIN_DBNAME || dbname == LOCAL_DBNAME) {
            if (readOnly) {
                return compatibilityReadOnlyAdminActions;
            } else {
                return compatibilityReadWriteAdminActions;
            }
        } else {
            if (readOnly) {
                return compatibilityReadOnlyActions;
            } else {
                return compatibilityReadWriteActions;
            }
        }
    }


    Status _checkRolesArray(const BSONElement& rolesElement) {
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

    Status AuthorizationManager::checkValidPrivilegeDocument(const StringData& dbname,
                                                             const BSONObj& doc) {
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
            Status status = _checkRolesArray(rolesElement);
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

                Status status = _checkRolesArray(*iter);
                if (!status.isOK())
                    return status;
            }
        }

        return Status::OK();
    }

    Status AuthorizationManager::buildPrivilegeSet(const std::string& dbname,
                                                   const UserName& user,
                                                   const BSONObj& privilegeDocument,
                                                   PrivilegeSet* result) const {
        if (!privilegeDocument.hasField(ROLES_FIELD_NAME)) {
            // Old-style (v2.2 and prior) privilege document
            if (AuthorizationManager::getSupportOldStylePrivilegeDocuments()) {
                return _buildPrivilegeSetFromOldStylePrivilegeDocument(dbname,
                                                                       user,
                                                                       privilegeDocument,
                                                                       result);
            }
            else {
                return _oldPrivilegeFormatNotSupported();
            }
        }
        else {
            return _buildPrivilegeSetFromExtendedPrivilegeDocument(
                    dbname, user, privilegeDocument, result);
        }
    }

    Status AuthorizationManager::_buildPrivilegeSetFromOldStylePrivilegeDocument(
            const std::string& dbname,
            const UserName& user,
            const BSONObj& privilegeDocument,
            PrivilegeSet* result) const {
        if (!(privilegeDocument.hasField(AuthorizationManager::USER_NAME_FIELD_NAME) &&
              privilegeDocument.hasField(AuthorizationManager::PASSWORD_FIELD_NAME))) {

            return Status(ErrorCodes::UnsupportedFormat,
                          mongoutils::str::stream() << "Invalid old-style privilege document "
                                  "received when trying to extract privileges: "
                                   << privilegeDocument,
                          0);
        }
        std::string userName = privilegeDocument[AuthorizationManager::USER_NAME_FIELD_NAME].str();
        if (userName != user.getUser()) {
            return Status(ErrorCodes::BadValue,
                          mongoutils::str::stream() << "Principal name from privilege document \""
                                  << userName
                                  << "\" doesn't match name of provided Principal \""
                                  << user.getUser()
                                  << "\"",
                          0);
        }

        bool readOnly = privilegeDocument[READONLY_FIELD_NAME].trueValue();
        ActionSet actions = getActionsForOldStyleUser(dbname, readOnly);
        std::string resourceName = (dbname == ADMIN_DBNAME || dbname == LOCAL_DBNAME) ?
            PrivilegeSet::WILDCARD_RESOURCE : dbname;
        result->grantPrivilege(Privilege(resourceName, actions), user);

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
        else if (isAdminDB && role == SYSTEM_ROLE_READ_ANY_DB) {
            outPrivileges->push_back(Privilege(PrivilegeSet::WILDCARD_RESOURCE, readRoleActions));
        }
        else if (isAdminDB && role == SYSTEM_ROLE_READ_WRITE_ANY_DB) {
            outPrivileges->push_back(
                    Privilege(PrivilegeSet::WILDCARD_RESOURCE, readWriteRoleActions));
        }
        else if (isAdminDB && role == SYSTEM_ROLE_USER_ADMIN_ANY_DB) {
            outPrivileges->push_back(
                    Privilege(PrivilegeSet::WILDCARD_RESOURCE, userAdminRoleActions));
        }
        else if (isAdminDB && role == SYSTEM_ROLE_DB_ADMIN_ANY_DB) {
            outPrivileges->push_back(
                    Privilege(PrivilegeSet::WILDCARD_RESOURCE, dbAdminRoleActions));
        }
        else if (isAdminDB && role == SYSTEM_ROLE_CLUSTER_ADMIN) {
            outPrivileges->push_back(
                    Privilege(PrivilegeSet::WILDCARD_RESOURCE, clusterAdminRoleActions));
        }
        else {
            warning() << "No such role, \"" << role << "\", in database " << dbname <<
                    ". No privileges will be acquired from this role" << endl;
        }
    }

    /**
     * Given a database name and a BSONElement representing an array of roles, populates
     * "outPrivileges" with the privileges associated with the given roles on the named database.
     *
     * Returns Status::OK() on success.
     */
    Status _getPrivilegesFromRoles(const std::string& dbname,
                                   const BSONElement& rolesElement,
                                   std::vector<Privilege>* outPrivileges) {

        static const char privilegesTypeMismatchMessage[] =
            "Roles must be enumerated in an array of strings.";

        if (dbname == PrivilegeSet::WILDCARD_RESOURCE) {
            return Status(ErrorCodes::BadValue,
                          PrivilegeSet::WILDCARD_RESOURCE + " is an invalid database name.");
        }

        if (rolesElement.type() != Array)
            return Status(ErrorCodes::TypeMismatch, privilegesTypeMismatchMessage);

        for (BSONObjIterator iter(rolesElement.embeddedObject()); iter.more(); iter.next()) {
            BSONElement roleElement = *iter;
            if (roleElement.type() != String)
                return Status(ErrorCodes::TypeMismatch, privilegesTypeMismatchMessage);
            _addPrivilegesForSystemRole(dbname, roleElement.str(), outPrivileges);
        }
        return Status::OK();
    }

    Status AuthorizationManager::_buildPrivilegeSetFromExtendedPrivilegeDocument(
            const std::string& dbname,
            const UserName& user,
            const BSONObj& privilegeDocument,
            PrivilegeSet* result) const {

        if (!privilegeDocument[READONLY_FIELD_NAME].eoo()) {
            return Status(ErrorCodes::UnsupportedFormat,
                          "Privilege documents may not contain both \"readonly\" and "
                          "\"roles\" fields");
        }

        std::vector<Privilege> acquiredPrivileges;

        // Acquire privileges on "dbname".
        Status status = _getPrivilegesFromRoles(
                dbname, privilegeDocument[ROLES_FIELD_NAME], &acquiredPrivileges);
        if (!status.isOK())
            return status;

        // If "dbname" is the admin database, handle the otherDBPrivileges field, which
        // grants privileges on databases other than "dbname".
        BSONElement otherDbPrivileges = privilegeDocument[OTHER_DB_ROLES_FIELD_NAME];
        if (dbname == ADMIN_DBNAME) {
            switch (otherDbPrivileges.type()) {
            case EOO:
                break;
            case Object: {
                for (BSONObjIterator iter(otherDbPrivileges.embeddedObject());
                     iter.more(); iter.next()) {

                    BSONElement rolesElement = *iter;
                    status = _getPrivilegesFromRoles(
                            rolesElement.fieldName(), rolesElement, &acquiredPrivileges);
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
            return Status(ErrorCodes::BadValue, "Only the admin database may contain a field "
                          "called \"otherDBRoles\"");
        }

        result->grantPrivileges(acquiredPrivileges, user);
        return Status::OK();
    }
} // namespace mongo
