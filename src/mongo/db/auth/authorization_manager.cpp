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

#include "mongo/db/auth/authorization_manager.h"

#include <string>
#include <vector>

#include "mongo/base/init.h"
#include "mongo/base/status.h"
#include "mongo/db/auth/action_set.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/auth_external_state.h"
#include "mongo/db/auth/principal.h"
#include "mongo/db/auth/principal_set.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/auth/privilege_set.h"
#include "mongo/db/client.h"
#include "mongo/db/namespacestring.h"
#include "mongo/db/security_common.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

    // this is a config setting, set at startup and not changing after initialization.
    bool noauth = true;

    AuthInfo::AuthInfo() {
        user = "__system";
    }
    AuthInfo internalSecurity;

    const std::string AuthorizationManager::SERVER_RESOURCE_NAME = "$SERVER";
    const std::string AuthorizationManager::CLUSTER_RESOURCE_NAME = "$CLUSTER";

namespace {
    const std::string ADMIN_DBNAME = "admin";
    const std::string LOCAL_DBNAME = "local";

    const std::string ROLES_FIELD_NAME = "roles";
    const std::string OTHER_DB_ROLES_FIELD_NAME = "otherDBRoles";
    const std::string READONLY_FIELD_NAME = "readOnly";
    const std::string USERNAME_FIELD_NAME = "user";
    const std::string USERSOURCE_FIELD_NAME = "userSource";
    const std::string PASSWORD_FIELD_NAME = "pwd";

    const std::string SYSTEM_ROLE_READ = "read";
    const std::string SYSTEM_ROLE_READ_WRITE = "readWrite";
    const std::string SYSTEM_ROLE_USER_ADMIN = "userAdmin";
    const std::string SYSTEM_ROLE_DB_ADMIN = "dbAdmin";
    const std::string SYSTEM_ROLE_SERVER_ADMIN = "serverAdmin";
    const std::string SYSTEM_ROLE_CLUSTER_ADMIN = "clusterAdmin";
    const std::string SYSTEM_ROLE_READ_ANY_DB = "readAnyDB";
    const std::string SYSTEM_ROLE_READ_WRITE_ANY_DB = "readWriteAnyDatabase";
    const std::string SYSTEM_ROLE_USER_ADMIN_ANY_DB = "userAdminAnyDatabase";
    const std::string SYSTEM_ROLE_DB_ADMIN_ANY_DB = "dbAdminAnyDatabase";

}  // namespace

    // ActionSets for the various system roles.  These ActionSets contain all the actions that
    // a user of each system role is granted.
    ActionSet readRoleActions;
    ActionSet readWriteRoleActions;
    ActionSet userAdminRoleActions;
    ActionSet dbAdminRoleActions;
    // Separate serverAdmin and clusterAdmin read-only and read-write action for backwards
    // compatibility with old-style read-only admin users.
    ActionSet serverAdminRoleReadActions;
    ActionSet serverAdminRoleWriteActions;
    ActionSet serverAdminRoleActions;
    ActionSet clusterAdminRoleReadActions;
    ActionSet clusterAdminRoleWriteActions;
    ActionSet clusterAdminRoleActions;
    // Can only be performed by internal connections.  Nothing ever explicitly grants these actions,
    // but they're included when calling addAllActions on an ActionSet, which is how internal
    // connections are granted their privileges.
    ActionSet internalActions;

    // This sets up the system role ActionSets.  This is what determines what actions each role
    // is authorized to perform
    MONGO_INITIALIZER(AuthorizationSystemRoles)(InitializerContext* context) {
        // Read role
        // TODO: Remove OLD_READ once commands require the proper actions
        readRoleActions.addAction(ActionType::oldRead);
        readRoleActions.addAction(ActionType::collStats);
        readRoleActions.addAction(ActionType::dbHash);
        readRoleActions.addAction(ActionType::dbStats);
        readRoleActions.addAction(ActionType::find);

        // Read-write role
        readWriteRoleActions.addAllActionsFromSet(readRoleActions);
        // TODO: Remove OLD_WRITE once commands require the proper actions
        readWriteRoleActions.addAction(ActionType::oldWrite);
        readWriteRoleActions.addAction(ActionType::convertToCapped);
        readWriteRoleActions.addAction(ActionType::createCollection); // TODO: should db admin get this also?
        readWriteRoleActions.addAction(ActionType::dropCollection);
        readWriteRoleActions.addAction(ActionType::dropIndexes);
        readWriteRoleActions.addAction(ActionType::emptycapped);
        readWriteRoleActions.addAction(ActionType::ensureIndex);
        readWriteRoleActions.addAction(ActionType::insert);
        readWriteRoleActions.addAction(ActionType::remove);
        readWriteRoleActions.addAction(ActionType::update);

        // User admin role
        userAdminRoleActions.addAction(ActionType::userAdmin);

        // DB admin role
        dbAdminRoleActions.addAction(ActionType::clean);
        dbAdminRoleActions.addAction(ActionType::collMod);
        dbAdminRoleActions.addAction(ActionType::collStats);
        dbAdminRoleActions.addAction(ActionType::compact);
        dbAdminRoleActions.addAction(ActionType::convertToCapped);
        dbAdminRoleActions.addAction(ActionType::dbStats);
        dbAdminRoleActions.addAction(ActionType::dropCollection);
        dbAdminRoleActions.addAction(ActionType::profileEnable);
        dbAdminRoleActions.addAction(ActionType::profileRead);
        dbAdminRoleActions.addAction(ActionType::reIndex); // TODO: Should readWrite have this also? This isn't consistent with ENSURE_INDEX and DROP_INDEXES
        dbAdminRoleActions.addAction(ActionType::renameCollection);
        dbAdminRoleActions.addAction(ActionType::validate);

        // Server admin role
        serverAdminRoleReadActions.addAction(ActionType::connPoolStats);
        serverAdminRoleReadActions.addAction(ActionType::connPoolSync);
        serverAdminRoleReadActions.addAction(ActionType::getCmdLineOpts);
        serverAdminRoleReadActions.addAction(ActionType::getLog);
        serverAdminRoleReadActions.addAction(ActionType::getParameter);
        serverAdminRoleReadActions.addAction(ActionType::getShardMap);
        serverAdminRoleReadActions.addAction(ActionType::hostInfo);
        serverAdminRoleReadActions.addAction(ActionType::listDatabases);
        serverAdminRoleReadActions.addAction(ActionType::logRotate);
        serverAdminRoleReadActions.addAction(ActionType::replSetFreeze);
        serverAdminRoleReadActions.addAction(ActionType::replSetGetStatus);
        serverAdminRoleReadActions.addAction(ActionType::replSetMaintenance);
        serverAdminRoleReadActions.addAction(ActionType::replSetStepDown);
        serverAdminRoleReadActions.addAction(ActionType::replSetSyncFrom);
        serverAdminRoleReadActions.addAction(ActionType::setParameter);
        serverAdminRoleReadActions.addAction(ActionType::serverStatus);
        serverAdminRoleReadActions.addAction(ActionType::shutdown);
        serverAdminRoleReadActions.addAction(ActionType::top);
        serverAdminRoleReadActions.addAction(ActionType::touch);
        serverAdminRoleReadActions.addAction(ActionType::unlock);

        serverAdminRoleWriteActions.addAction(ActionType::applyOps);
        serverAdminRoleWriteActions.addAction(ActionType::closeAllDatabases);
        serverAdminRoleWriteActions.addAction(ActionType::cpuProfiler);
        serverAdminRoleWriteActions.addAction(ActionType::cursorInfo);
        serverAdminRoleWriteActions.addAction(ActionType::diagLogging);
        serverAdminRoleWriteActions.addAction(ActionType::fsync);
        serverAdminRoleWriteActions.addAction(ActionType::inprog);
        serverAdminRoleWriteActions.addAction(ActionType::killop);
        serverAdminRoleWriteActions.addAction(ActionType::repairDatabase);
        serverAdminRoleWriteActions.addAction(ActionType::replSetInitiate);
        serverAdminRoleWriteActions.addAction(ActionType::replSetReconfig);
        serverAdminRoleWriteActions.addAction(ActionType::resync);

        serverAdminRoleActions.addAllActionsFromSet(serverAdminRoleReadActions);
        serverAdminRoleActions.addAllActionsFromSet(serverAdminRoleWriteActions);

        // Cluster admin role
        clusterAdminRoleReadActions.addAction(ActionType::getShardVersion);
        clusterAdminRoleReadActions.addAction(ActionType::listShards);
        clusterAdminRoleReadActions.addAction(ActionType::netstat);
        clusterAdminRoleReadActions.addAction(ActionType::setShardVersion); // TODO: should this be internal?
        clusterAdminRoleReadActions.addAction(ActionType::splitVector);
        clusterAdminRoleReadActions.addAction(ActionType::unsetSharding);

        clusterAdminRoleWriteActions.addAction(ActionType::addShard);
        clusterAdminRoleWriteActions.addAction(ActionType::dropDatabase); // TODO: Should there be a CREATE_DATABASE also?
        clusterAdminRoleWriteActions.addAction(ActionType::enableSharding);
        clusterAdminRoleWriteActions.addAction(ActionType::flushRouterConfig);
        clusterAdminRoleWriteActions.addAction(ActionType::moveChunk);
        clusterAdminRoleWriteActions.addAction(ActionType::movePrimary);
        clusterAdminRoleWriteActions.addAction(ActionType::removeShard);
        clusterAdminRoleWriteActions.addAction(ActionType::shardCollection);
        clusterAdminRoleWriteActions.addAction(ActionType::shardingState);
        clusterAdminRoleWriteActions.addAction(ActionType::split);
        clusterAdminRoleWriteActions.addAction(ActionType::splitChunk);

        clusterAdminRoleActions.addAllActionsFromSet(clusterAdminRoleReadActions);
        clusterAdminRoleActions.addAllActionsFromSet(clusterAdminRoleWriteActions);

        // Internal commands
        internalActions.addAction(ActionType::clone);
        internalActions.addAction(ActionType::handshake);
        internalActions.addAction(ActionType::mapReduceShardedFinish);
        internalActions.addAction(ActionType::replSetElect);
        internalActions.addAction(ActionType::replSetFresh);
        internalActions.addAction(ActionType::replSetGetRBID);
        internalActions.addAction(ActionType::replSetHeartbeat);
        internalActions.addAction(ActionType::writebacklisten);
        internalActions.addAction(ActionType::writeBacksQueued);
        internalActions.addAction(ActionType::_migrateClone);
        internalActions.addAction(ActionType::_recvChunkAbort);
        internalActions.addAction(ActionType::_recvChunkCommit);
        internalActions.addAction(ActionType::_recvChunkStart);
        internalActions.addAction(ActionType::_recvChunkStatus);
        internalActions.addAction(ActionType::_transferMods);

        return Status::OK();
    }

    AuthorizationManager::AuthorizationManager(AuthExternalState* externalState) {
        _externalState.reset(externalState);
    }

    AuthorizationManager::~AuthorizationManager(){}

    ActionSet AuthorizationManager::getAllUserActions() {
        ActionSet allActions;
        allActions.addAllActionsFromSet(readRoleActions);
        allActions.addAllActionsFromSet(readWriteRoleActions);
        allActions.addAllActionsFromSet(userAdminRoleActions);
        allActions.addAllActionsFromSet(dbAdminRoleActions);
        allActions.addAllActionsFromSet(serverAdminRoleActions);
        allActions.addAllActionsFromSet(clusterAdminRoleActions);
        return allActions;
    }

    void AuthorizationManager::startRequest() {
        _externalState->startRequest();
    }

    void AuthorizationManager::addAuthorizedPrincipal(Principal* principal) {
        _authenticatedPrincipals.add(principal);
    }

    Principal* AuthorizationManager::lookupPrincipal(const PrincipalName& name) {
        return _authenticatedPrincipals.lookup(name);
    }

    void AuthorizationManager::logoutDatabase(const std::string& dbname) {
        Principal* principal = _authenticatedPrincipals.lookupByDBName(dbname);
        if (!principal)
            return;
        _acquiredPrivileges.revokePrivilegesFromPrincipal(principal->getName());
        _authenticatedPrincipals.removeByDBName(dbname);
    }

    PrincipalSet::NameIterator AuthorizationManager::getAuthenticatedPrincipalNames() {
        return _authenticatedPrincipals.getNames();
    }

    Status AuthorizationManager::acquirePrivilege(const Privilege& privilege,
                                                  const PrincipalName& authorizingPrincipal) {
        if (!_authenticatedPrincipals.lookup(authorizingPrincipal)) {
            return Status(ErrorCodes::UserNotFound,
                          mongoutils::str::stream()
                                  << "No authenticated principle found with name: "
                                  << authorizingPrincipal.getUser()
                                  << " from database "
                                  << authorizingPrincipal.getDB(),
                          0);
        }
        _acquiredPrivileges.grantPrivilege(privilege, authorizingPrincipal);
        return Status::OK();
    }

    void AuthorizationManager::grantInternalAuthorization(const std::string& principalName) {
        Principal* principal = new Principal(PrincipalName(principalName, "local"));
        ActionSet actions;
        actions.addAllActions();

        addAuthorizedPrincipal(principal);
        fassert(16581, acquirePrivilege(Privilege(PrivilegeSet::WILDCARD_RESOURCE, actions),
                                    principal->getName()).isOK());
    }

    bool AuthorizationManager::hasInternalAuthorization() {
        ActionSet allActions;
        allActions.addAllActions();
        return _acquiredPrivileges.hasPrivilege(Privilege(PrivilegeSet::WILDCARD_RESOURCE,
                                                          allActions));
    }

    ActionSet AuthorizationManager::getActionsForOldStyleUser(const std::string& dbname,
                                                              bool readOnly) {
        ActionSet actions;
        // Basic actions
        if (readOnly) {
            actions.addAllActionsFromSet(readRoleActions);
        }
        else {
            actions.addAllActionsFromSet(readWriteRoleActions);
            actions.addAllActionsFromSet(dbAdminRoleActions);
            actions.addAllActionsFromSet(userAdminRoleActions);
            actions.addAction(ActionType::dropDatabase);
            actions.addAction(ActionType::repairDatabase);
        }
        // Admin actions
        if (dbname == ADMIN_DBNAME || dbname == LOCAL_DBNAME) {
            actions.addAllActionsFromSet(serverAdminRoleReadActions);
            actions.addAllActionsFromSet(clusterAdminRoleReadActions);
            if (!readOnly) {
                actions.addAllActionsFromSet(serverAdminRoleWriteActions);
                actions.addAllActionsFromSet(clusterAdminRoleWriteActions);
            }
        }
        return actions;
    }

    Status AuthorizationManager::acquirePrivilegesFromPrivilegeDocument(
            const std::string& dbname, const PrincipalName& principal, const BSONObj& privilegeDocument) {
        if (!_authenticatedPrincipals.lookup(principal)) {
            return Status(ErrorCodes::UserNotFound,
                          mongoutils::str::stream()
                                  << "No authenticated principle found with name: "
                                  << principal.getUser()
                                  << " from database "
                                  << principal.getDB(),
                          0);
        }
        if (principal.getUser() == internalSecurity.user) {
            // Grant full access to internal user
            ActionSet allActions;
            allActions.addAllActions();
            return acquirePrivilege(Privilege(PrivilegeSet::WILDCARD_RESOURCE, allActions),
                                    principal);
        }
        return buildPrivilegeSet(dbname, principal, privilegeDocument, &_acquiredPrivileges);
    }

    Status AuthorizationManager::buildPrivilegeSet(const std::string& dbname,
                                                   const PrincipalName& principal,
                                                   const BSONObj& privilegeDocument,
                                                   PrivilegeSet* result) {
        if (!privilegeDocument.hasField(ROLES_FIELD_NAME)) {
            // Old-style (v2.2 and prior) privilege document
            return _buildPrivilegeSetFromOldStylePrivilegeDocument(dbname,
                                                                   principal,
                                                                   privilegeDocument,
                                                                   result);
        }
        else {
            return _buildPrivilegeSetFromExtendedPrivilegeDocument(
                    dbname, principal, privilegeDocument, result);
        }
    }

    Status AuthorizationManager::_buildPrivilegeSetFromOldStylePrivilegeDocument(
            const std::string& dbname,
            const PrincipalName& principal,
            const BSONObj& privilegeDocument,
            PrivilegeSet* result) {
        if (!(privilegeDocument.hasField(USERNAME_FIELD_NAME) &&
              privilegeDocument.hasField(PASSWORD_FIELD_NAME))) {

            return Status(ErrorCodes::UnsupportedFormat,
                          mongoutils::str::stream() << "Invalid old-style privilege document "
                                  "received when trying to extract privileges: "
                                   << privilegeDocument,
                          0);
        }
        if (privilegeDocument[USERNAME_FIELD_NAME].str() != principal.getUser()) {
            return Status(ErrorCodes::BadValue,
                          mongoutils::str::stream() << "Principal name from privilege document \""
                                  << privilegeDocument[USERNAME_FIELD_NAME].str()
                                  << "\" doesn't match name of provided Principal \""
                                  << principal.getUser()
                                  << "\"",
                          0);
        }

        bool readOnly = privilegeDocument[READONLY_FIELD_NAME].trueValue();
        ActionSet actions = getActionsForOldStyleUser(dbname, readOnly);
        std::string resourceName = (dbname == ADMIN_DBNAME || dbname == LOCAL_DBNAME) ?
            PrivilegeSet::WILDCARD_RESOURCE : dbname;
        result->grantPrivilege(Privilege(resourceName, actions), principal);

        return Status::OK();
    }

    /**
     * Adds to "outPrivileges" the privileges associated with having the named "role" on "dbname".
     *
     * Returns non-OK status if "role" is not a defined role in "dbname".
     */
    static Status _addPrivilegesForSystemRole(const std::string& dbname,
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
        else if (isAdminDB && role == SYSTEM_ROLE_SERVER_ADMIN) {
            outPrivileges->push_back(
                    Privilege(PrivilegeSet::WILDCARD_RESOURCE, serverAdminRoleActions));
        }
        else if (isAdminDB && role == SYSTEM_ROLE_CLUSTER_ADMIN) {
            outPrivileges->push_back(
                    Privilege(PrivilegeSet::WILDCARD_RESOURCE, clusterAdminRoleActions));
        }
        else {
            return Status(ErrorCodes::BadValue,
                          mongoutils::str::stream() <<"No such role, " << role <<
                          ", in database " << dbname);
        }
        return Status::OK();
    }

    /**
     * Given a database name and a BSONElement representing an array of roles, populates
     * "outPrivileges" with the privileges associated with the given roles on the named database.
     *
     * Returns Status::OK() on success.
     */
    static Status _getPrivilegesFromRoles(const std::string& dbname,
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
            Status status = _addPrivilegesForSystemRole(dbname, roleElement.str(), outPrivileges);
            if (!status.isOK())
                return status;
        }
        return Status::OK();
    }

    Status AuthorizationManager::_buildPrivilegeSetFromExtendedPrivilegeDocument(
            const std::string& dbname,
            const PrincipalName& principal,
            const BSONObj& privilegeDocument,
            PrivilegeSet* result) {

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

        result->grantPrivileges(acquiredPrivileges, principal);
        return Status::OK();
    }

    bool AuthorizationManager::checkAuthorization(const std::string& resource,
                                                  ActionType action) {
        if (_externalState->shouldIgnoreAuthChecks())
            return true;

        return _acquiredPrivileges.hasPrivilege(Privilege(nsToDatabase(resource), action));
    }

    bool AuthorizationManager::checkAuthorization(const std::string& resource,
                                                  ActionSet actions) {
        if (_externalState->shouldIgnoreAuthChecks())
            return true;

        return _acquiredPrivileges.hasPrivilege(Privilege(nsToDatabase(resource), actions));
    }

    Status AuthorizationManager::checkAuthForQuery(const std::string& ns) {
        NamespaceString namespaceString(ns);
        verify(!namespaceString.isCommand());
        if (namespaceString.coll == "system.users") {
            if (!checkAuthorization(ns, ActionType::userAdmin)) {
                return Status(ErrorCodes::Unauthorized,
                              mongoutils::str::stream() <<
                                      "unauthorized to read user information for database " <<
                                      namespaceString.db,
                              0);
            }
        }
        else if (namespaceString.coll == "system.profile") {
            if (!checkAuthorization(ns, ActionType::profileRead)) {
                return Status(ErrorCodes::Unauthorized,
                              mongoutils::str::stream() << "unauthorized to read " <<
                                      namespaceString.db << ".system.profile",
                              0);
            }
        }
        else {
            if (!checkAuthorization(ns, ActionType::find)) {
                return Status(ErrorCodes::Unauthorized,
                              mongoutils::str::stream() << "unauthorized for query on " << ns,
                              0);
            }
        }
        return Status::OK();
    }

    Status AuthorizationManager::checkAuthForInsert(const std::string& ns) {
        NamespaceString namespaceString(ns);
        if (namespaceString.coll == "system.users") {
            if (!checkAuthorization(ns, ActionType::userAdmin)) {
                return Status(ErrorCodes::Unauthorized,
                              mongoutils::str::stream() <<
                                      "unauthorized to create user for database " <<
                                      namespaceString.db,
                              0);
            }
        }
        else {
            if (!checkAuthorization(ns, ActionType::insert)) {
                return Status(ErrorCodes::Unauthorized,
                              mongoutils::str::stream() << "unauthorized for insert on " << ns,
                              0);
            }
        }
        return Status::OK();
    }

    Status AuthorizationManager::checkAuthForUpdate(const std::string& ns, bool upsert) {
        NamespaceString namespaceString(ns);
        if (namespaceString.coll == "system.users") {
            if (!checkAuthorization(ns, ActionType::userAdmin)) {
                return Status(ErrorCodes::Unauthorized,
                              mongoutils::str::stream() <<
                                      "not authorized to update user information for database " <<
                                      namespaceString.db,
                              0);
            }
        }
        else {
            if (!checkAuthorization(ns, ActionType::update)) {
                return Status(ErrorCodes::Unauthorized,
                              mongoutils::str::stream() << "not authorized for update on " << ns,
                              0);
            }
            if (upsert && !checkAuthorization(ns, ActionType::insert)) {
                return Status(ErrorCodes::Unauthorized,
                              mongoutils::str::stream() << "not authorized for upsert on " << ns,
                              0);
            }
        }
        return Status::OK();
    }

    Status AuthorizationManager::checkAuthForDelete(const std::string& ns) {
        NamespaceString namespaceString(ns);
        if (namespaceString.coll == "system.users") {
            if (!checkAuthorization(ns, ActionType::userAdmin)) {
                return Status(ErrorCodes::Unauthorized,
                              mongoutils::str::stream() <<
                                      "not authorized to remove user from database " <<
                                      namespaceString.db,
                              0);
            }
        }
        else {
            if (!checkAuthorization(ns, ActionType::remove)) {
                return Status(ErrorCodes::Unauthorized,
                              mongoutils::str::stream() << "not authorized to remove from " << ns,
                              0);
            }
        }
        return Status::OK();
    }

    Status AuthorizationManager::checkAuthForGetMore(const std::string& ns) {
        return checkAuthForQuery(ns);
    }

    Status AuthorizationManager::checkAuthForPrivileges(const vector<Privilege>& privileges) {
        for (std::vector<Privilege>::const_iterator it = privileges.begin();
                it != privileges.end(); ++it) {
            const Privilege& privilege = *it;
            if (!checkAuthorization(privilege.getResource(), privilege.getActions())) {
                return Status(ErrorCodes::Unauthorized, "unauthorized", 0);
            }
        }
        return Status::OK();
    }

} // namespace mongo
