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
 */

#include "mongo/db/auth/role_graph.h"

#include "mongo/base/init.h"
#include "mongo/base/status.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/auth/role_name.h"

namespace mongo {

    const std::string RoleGraph::BUILTIN_ROLE_V0_READ = "oldRead";
    const std::string RoleGraph::BUILTIN_ROLE_V0_READ_WRITE= "oldReadWrite";
    const std::string RoleGraph::BUILTIN_ROLE_V0_ADMIN_READ = "oldAdminRead";
    const std::string RoleGraph::BUILTIN_ROLE_V0_ADMIN_READ_WRITE= "oldAdminReadWrite";

namespace {
    const std::string ADMIN_DBNAME = "admin";

    const std::string BUILTIN_ROLE_READ = "read";
    const std::string BUILTIN_ROLE_READ_WRITE = "readWrite";
    const std::string BUILTIN_ROLE_USER_ADMIN = "userAdmin";
    const std::string BUILTIN_ROLE_DB_ADMIN = "dbAdmin";
    const std::string BUILTIN_ROLE_CLUSTER_ADMIN = "clusterAdmin";
    const std::string BUILTIN_ROLE_READ_ANY_DB = "readAnyDatabase";
    const std::string BUILTIN_ROLE_READ_WRITE_ANY_DB = "readWriteAnyDatabase";
    const std::string BUILTIN_ROLE_USER_ADMIN_ANY_DB = "userAdminAnyDatabase";
    const std::string BUILTIN_ROLE_DB_ADMIN_ANY_DB = "dbAdminAnyDatabase";

    // ActionSets for the various built-in roles.  These ActionSets contain all the actions that
    // a user of each built-in role is granted.
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


    // This sets up the built-in role ActionSets.  This is what determines what actions each role
    // is authorized to perform
    MONGO_INITIALIZER(AuthorizationBuiltinRoles)(InitializerContext* context) {
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

    /**
     * Returns the privilege that corresponds with the given built-in role.
     */
    Privilege getPrivilegeForBuiltinRole(const RoleName& roleName) {
        const bool isAdminDB = (roleName.getDB() == ADMIN_DBNAME);

        if (roleName.getRole() == BUILTIN_ROLE_READ) {
            return Privilege(roleName.getDB().toString(), readRoleActions);
        }
        if (roleName.getRole() == BUILTIN_ROLE_READ_WRITE) {
            return Privilege(roleName.getDB().toString(), readWriteRoleActions);
        }
        if (roleName.getRole() == BUILTIN_ROLE_USER_ADMIN) {
            return Privilege(roleName.getDB().toString(), userAdminRoleActions);
        }
        if (roleName.getRole() == BUILTIN_ROLE_DB_ADMIN) {
            return Privilege(roleName.getDB().toString(), dbAdminRoleActions);
        }
        if (roleName.getRole() == RoleGraph::BUILTIN_ROLE_V0_READ) {
            return Privilege(roleName.getDB().toString(), compatibilityReadOnlyActions);
        }
        if (roleName.getRole() == RoleGraph::BUILTIN_ROLE_V0_READ_WRITE) {
            return Privilege(roleName.getDB().toString(), compatibilityReadWriteActions);
        }
        if (isAdminDB && roleName.getRole() == BUILTIN_ROLE_READ_ANY_DB) {
            return Privilege(AuthorizationManager::WILDCARD_RESOURCE_NAME, readRoleActions);
        }
        if (isAdminDB && roleName.getRole() == BUILTIN_ROLE_READ_WRITE_ANY_DB) {
            return Privilege(AuthorizationManager::WILDCARD_RESOURCE_NAME, readWriteRoleActions);
        }
        if (isAdminDB && roleName.getRole() == BUILTIN_ROLE_USER_ADMIN_ANY_DB) {
            return Privilege(AuthorizationManager::WILDCARD_RESOURCE_NAME, userAdminRoleActions);
        }
        if (isAdminDB && roleName.getRole() == BUILTIN_ROLE_DB_ADMIN_ANY_DB) {
            return Privilege(AuthorizationManager::WILDCARD_RESOURCE_NAME, dbAdminRoleActions);
        }
        if (isAdminDB && roleName.getRole() == BUILTIN_ROLE_CLUSTER_ADMIN) {
            return Privilege(AuthorizationManager::WILDCARD_RESOURCE_NAME, clusterAdminRoleActions);
        }
        if (isAdminDB && roleName.getRole() == RoleGraph::BUILTIN_ROLE_V0_ADMIN_READ) {
            return Privilege(AuthorizationManager::WILDCARD_RESOURCE_NAME,
                             compatibilityReadOnlyAdminActions);
        }
        if (isAdminDB && roleName.getRole() == RoleGraph::BUILTIN_ROLE_V0_ADMIN_READ_WRITE) {
            return Privilege(AuthorizationManager::WILDCARD_RESOURCE_NAME,
                             compatibilityReadWriteAdminActions);
        }

        fassertFailed(17116);
    }

}  // namespace

    ActionSet RoleGraph::getAllUserActions() {
        ActionSet allActions;
        allActions.addAllActionsFromSet(readRoleActions);
        allActions.addAllActionsFromSet(readWriteRoleActions);
        allActions.addAllActionsFromSet(userAdminRoleActions);
        allActions.addAllActionsFromSet(dbAdminRoleActions);
        allActions.addAllActionsFromSet(clusterAdminRoleActions);
        return allActions;
    }

    bool RoleGraph::_isBuiltinRole(const RoleName& role) {
        bool isAdminDB = role.getDB() == "admin";

        if (role.getRole() == BUILTIN_ROLE_READ) {
            return true;
        }
        else if (role.getRole() == BUILTIN_ROLE_READ_WRITE) {
            return true;
        }
        else if (role.getRole() == BUILTIN_ROLE_USER_ADMIN) {
            return true;
        }
        else if (role.getRole() == BUILTIN_ROLE_DB_ADMIN) {
            return true;
        }
        else if (role.getRole() == BUILTIN_ROLE_V0_READ) {
            return true;
        }
        else if (role.getRole() == BUILTIN_ROLE_V0_READ_WRITE) {
            return true;
        }
        else if (isAdminDB && role.getRole() == BUILTIN_ROLE_READ_ANY_DB) {
            return true;
        }
        else if (isAdminDB && role.getRole() == BUILTIN_ROLE_READ_WRITE_ANY_DB) {
            return true;
        }
        else if (isAdminDB && role.getRole() == BUILTIN_ROLE_USER_ADMIN_ANY_DB) {
            return true;
        }
        else if (isAdminDB && role.getRole() == BUILTIN_ROLE_DB_ADMIN_ANY_DB) {
            return true;
        }
        else if (isAdminDB && role.getRole() == BUILTIN_ROLE_CLUSTER_ADMIN) {
            return true;
        }
        else if (isAdminDB && role.getRole() == BUILTIN_ROLE_V0_ADMIN_READ) {
            return true;
        }
        else if (isAdminDB && role.getRole() == BUILTIN_ROLE_V0_ADMIN_READ_WRITE) {
            return true;
        }

        return false;
    }

    void RoleGraph::_createBuiltinRoleIfNeeded(const RoleName& role) {
        if (!_isBuiltinRole(role) || _roleExistsDontCreateBuiltin(role)) {
            return;
        }

        _createRoleDontCheckIfRoleExists(role);
        Privilege privilege = getPrivilegeForBuiltinRole(role);
        _addPrivilegeToRoleNoChecks(role, privilege);
        _allPrivilegesForRole[role].push_back(privilege);
    }

} // namespace mongo
