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
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#include "mongo/db/auth/role_graph.h"

#include "mongo/base/init.h"
#include "mongo/base/status.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/auth/role_name.h"

namespace mongo {

const std::string RoleGraph::BUILTIN_ROLE_V0_READ = "read";
const std::string RoleGraph::BUILTIN_ROLE_V0_READ_WRITE = "dbOwner";
const std::string RoleGraph::BUILTIN_ROLE_V0_ADMIN_READ = "readAnyDatabase";
const std::string RoleGraph::BUILTIN_ROLE_V0_ADMIN_READ_WRITE = "root";

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
const std::string BUILTIN_ROLE_ROOT = "root";
const std::string BUILTIN_ROLE_INTERNAL = "__system";
const std::string BUILTIN_ROLE_DB_OWNER = "dbOwner";
const std::string BUILTIN_ROLE_CLUSTER_MONITOR = "clusterMonitor";
const std::string BUILTIN_ROLE_HOST_MANAGEMENT = "hostManager";
const std::string BUILTIN_ROLE_CLUSTER_MANAGEMENT = "clusterManager";
const std::string BUILTIN_ROLE_BACKUP = "backup";
const std::string BUILTIN_ROLE_RESTORE = "restore";
const std::string BUILTIN_ROLE_ENABLE_SHARDING = "enableSharding";

/// Actions that the "read" role may perform on a normal resources of a specific database, and
/// that the "readAnyDatabase" role may perform on normal resources of any database.
ActionSet readRoleActions;

/// Actions that the "readWrite" role may perform on a normal resources of a specific database,
/// and that the "readWriteAnyDatabase" role may perform on normal resources of any database.
ActionSet readWriteRoleActions;

/// Actions that the "userAdmin" role may perform on normal resources of a specific database,
/// and that the "userAdminAnyDatabase" role may perform on normal resources of any database.
ActionSet userAdminRoleActions;

/// Actions that the "dbAdmin" role may perform on normal resources of a specific database,
// and that the "dbAdminAnyDatabase" role may perform on normal resources of any database.
ActionSet dbAdminRoleActions;

/// Actions that the "clusterMonitor" role may perform on the cluster resource.
ActionSet clusterMonitorRoleClusterActions;

/// Actions that the "clusterMonitor" role may perform on any database.
ActionSet clusterMonitorRoleDatabaseActions;

/// Actions that the "hostManager" role may perform on the cluster resource.
ActionSet hostManagerRoleClusterActions;

/// Actions that the "hostManager" role may perform on any database.
ActionSet hostManagerRoleDatabaseActions;

/// Actions that the "clusterManager" role may perform on the cluster resource.
ActionSet clusterManagerRoleClusterActions;

/// Actions that the "clusterManager" role may perform on any database
ActionSet clusterManagerRoleDatabaseActions;

ActionSet& operator<<(ActionSet& target, ActionType source) {
    target.addAction(source);
    return target;
}

void operator+=(ActionSet& target, const ActionSet& source) {
    target.addAllActionsFromSet(source);
}

// This sets up the built-in role ActionSets.  This is what determines what actions each role
// is authorized to perform
// Note: we suppress clang-format for this function because we want each enum value on a separate
// line
// clang-format off
MONGO_INITIALIZER(AuthorizationBuiltinRoles)(InitializerContext* context) {
    // Read role
    readRoleActions
        << ActionType::collStats
        << ActionType::dbHash
        << ActionType::dbStats
        << ActionType::find
        << ActionType::killCursors
        << ActionType::listCollections
        << ActionType::listIndexes
        << ActionType::planCacheRead;

    // Read-write role
    readWriteRoleActions += readRoleActions;
    readWriteRoleActions
        << ActionType::convertToCapped  // db admin gets this also
        << ActionType::createCollection  // db admin gets this also
        << ActionType::dropCollection
        << ActionType::dropIndex
        << ActionType::emptycapped
        << ActionType::createIndex
        << ActionType::insert
        << ActionType::remove
        << ActionType::renameCollectionSameDB  // db admin gets this also
        << ActionType::update;

    // User admin role
    userAdminRoleActions
        << ActionType::changeCustomData
        << ActionType::changePassword
        << ActionType::createUser
        << ActionType::createRole
        << ActionType::dropUser
        << ActionType::dropRole
        << ActionType::grantRole
        << ActionType::revokeRole
        << ActionType::viewUser
        << ActionType::viewRole;


    // DB admin role
    dbAdminRoleActions
        << ActionType::bypassDocumentValidation
        << ActionType::collMod
        << ActionType::collStats  // clusterMonitor gets this also
        << ActionType::compact
        << ActionType::convertToCapped  // read_write gets this also
        << ActionType::createCollection // read_write gets this also
        << ActionType::dbStats  // clusterMonitor gets this also
        << ActionType::dropCollection
        << ActionType::dropDatabase  // clusterAdmin gets this also TODO(spencer): should
                                     // readWriteAnyDatabase?
        << ActionType::dropIndex
        << ActionType::createIndex
        << ActionType::enableProfiler
        << ActionType::listCollections
        << ActionType::listIndexes
        << ActionType::planCacheIndexFilter
        << ActionType::planCacheRead
        << ActionType::planCacheWrite
        << ActionType::reIndex
        << ActionType::renameCollectionSameDB  // read_write gets this also
        << ActionType::repairDatabase
        << ActionType::storageDetails
        << ActionType::validate;

    // clusterMonitor role actions that target the cluster resource
    clusterMonitorRoleClusterActions
        << ActionType::connPoolStats
        << ActionType::getCmdLineOpts
        << ActionType::getLog
        << ActionType::getParameter
        << ActionType::getShardMap
        << ActionType::hostInfo
        << ActionType::listDatabases
        << ActionType::listShards  // clusterManager gets this also
        << ActionType::netstat
        << ActionType::replSetGetConfig  // clusterManager gets this also
        << ActionType::replSetGetStatus  // clusterManager gets this also
        << ActionType::serverStatus 
        << ActionType::top
        << ActionType::inprog
        << ActionType::shardingState;

    // clusterMonitor role actions that target a database (or collection) resource
    clusterMonitorRoleDatabaseActions 
        << ActionType::collStats  // dbAdmin gets this also
        << ActionType::dbStats  // dbAdmin gets this also
        << ActionType::getShardVersion
        << ActionType::indexStats;

    // hostManager role actions that target the cluster resource
    hostManagerRoleClusterActions
        << ActionType::applicationMessage  // clusterManager gets this also
        << ActionType::connPoolSync
        << ActionType::cpuProfiler
        << ActionType::logRotate
        << ActionType::setParameter
        << ActionType::shutdown
        << ActionType::touch
        << ActionType::unlock
        << ActionType::diagLogging
        << ActionType::flushRouterConfig  // clusterManager gets this also
        << ActionType::fsync
        << ActionType::invalidateUserCache // userAdminAnyDatabase gets this also
        << ActionType::killop
        << ActionType::resync;  // clusterManager gets this also

    // hostManager role actions that target the database resource
    hostManagerRoleDatabaseActions
        << ActionType::killCursors
        << ActionType::repairDatabase;


    // clusterManager role actions that target the cluster resource
    clusterManagerRoleClusterActions
        << ActionType::appendOplogNote  // backup gets this also
        << ActionType::applicationMessage  // hostManager gets this also
        << ActionType::replSetConfigure
        << ActionType::replSetGetConfig  // clusterMonitor gets this also
        << ActionType::replSetGetStatus  // clusterMonitor gets this also
        << ActionType::replSetStateChange
        << ActionType::resync  // hostManager gets this also
        << ActionType::addShard 
        << ActionType::removeShard
        << ActionType::listShards  // clusterMonitor gets this also
        << ActionType::flushRouterConfig  // hostManager gets this also
        << ActionType::cleanupOrphaned;

    clusterManagerRoleDatabaseActions
        << ActionType::splitChunk
        << ActionType::moveChunk
        << ActionType::enableSharding
        << ActionType::splitVector;

    return Status::OK();
}
// clang-format on

void addReadOnlyDbPrivileges(PrivilegeVector* privileges, StringData dbName) {
    Privilege::addPrivilegeToPrivilegeVector(
        privileges, Privilege(ResourcePattern::forDatabaseName(dbName), readRoleActions));
    Privilege::addPrivilegeToPrivilegeVector(
        privileges,
        Privilege(ResourcePattern::forExactNamespace(NamespaceString(dbName, "system.indexes")),
                  readRoleActions));
    Privilege::addPrivilegeToPrivilegeVector(
        privileges,
        Privilege(ResourcePattern::forExactNamespace(NamespaceString(dbName, "system.js")),
                  readRoleActions));
    Privilege::addPrivilegeToPrivilegeVector(
        privileges,
        Privilege(ResourcePattern::forExactNamespace(NamespaceString(dbName, "system.namespaces")),
                  readRoleActions));
}

void addReadWriteDbPrivileges(PrivilegeVector* privileges, StringData dbName) {
    addReadOnlyDbPrivileges(privileges, dbName);
    Privilege::addPrivilegeToPrivilegeVector(
        privileges, Privilege(ResourcePattern::forDatabaseName(dbName), readWriteRoleActions));
    Privilege::addPrivilegeToPrivilegeVector(
        privileges,
        Privilege(ResourcePattern::forExactNamespace(NamespaceString(dbName, "system.js")),
                  readWriteRoleActions));
}

void addUserAdminDbPrivileges(PrivilegeVector* privileges, StringData dbName) {
    privileges->push_back(
        Privilege(ResourcePattern::forDatabaseName(dbName), userAdminRoleActions));
}

void addDbAdminDbPrivileges(PrivilegeVector* privileges, StringData dbName) {
    Privilege::addPrivilegeToPrivilegeVector(
        privileges, Privilege(ResourcePattern::forDatabaseName(dbName), dbAdminRoleActions));
    Privilege::addPrivilegeToPrivilegeVector(
        privileges,
        Privilege(ResourcePattern::forExactNamespace(NamespaceString(dbName, "system.indexes")),
                  readRoleActions));
    Privilege::addPrivilegeToPrivilegeVector(
        privileges,
        Privilege(ResourcePattern::forExactNamespace(NamespaceString(dbName, "system.namespaces")),
                  readRoleActions));

    ActionSet profileActions = readRoleActions;
    profileActions.addAction(ActionType::convertToCapped);
    profileActions.addAction(ActionType::createCollection);
    profileActions.addAction(ActionType::dropCollection);
    Privilege::addPrivilegeToPrivilegeVector(
        privileges,
        Privilege(ResourcePattern::forExactNamespace(NamespaceString(dbName, "system.profile")),
                  profileActions));
}

void addDbOwnerPrivileges(PrivilegeVector* privileges, StringData dbName) {
    addReadWriteDbPrivileges(privileges, dbName);
    addDbAdminDbPrivileges(privileges, dbName);
    addUserAdminDbPrivileges(privileges, dbName);
}

void addEnableShardingPrivileges(PrivilegeVector* privileges) {
    ActionSet enableShardingActions;
    enableShardingActions.addAction(ActionType::enableSharding);
    Privilege::addPrivilegeToPrivilegeVector(
        privileges, Privilege(ResourcePattern::forAnyNormalResource(), enableShardingActions));
}

void addReadOnlyAnyDbPrivileges(PrivilegeVector* privileges) {
    Privilege::addPrivilegeToPrivilegeVector(
        privileges, Privilege(ResourcePattern::forAnyNormalResource(), readRoleActions));
    Privilege::addPrivilegeToPrivilegeVector(
        privileges, Privilege(ResourcePattern::forClusterResource(), ActionType::listDatabases));
    Privilege::addPrivilegeToPrivilegeVector(
        privileges,
        Privilege(ResourcePattern::forCollectionName("system.indexes"), readRoleActions));
    Privilege::addPrivilegeToPrivilegeVector(
        privileges, Privilege(ResourcePattern::forCollectionName("system.js"), readRoleActions));
    Privilege::addPrivilegeToPrivilegeVector(
        privileges,
        Privilege(ResourcePattern::forCollectionName("system.namespaces"), readRoleActions));
}

void addReadWriteAnyDbPrivileges(PrivilegeVector* privileges) {
    addReadOnlyAnyDbPrivileges(privileges);
    Privilege::addPrivilegeToPrivilegeVector(
        privileges, Privilege(ResourcePattern::forAnyNormalResource(), readWriteRoleActions));
    Privilege::addPrivilegeToPrivilegeVector(
        privileges,
        Privilege(ResourcePattern::forCollectionName("system.js"), readWriteRoleActions));
}

void addUserAdminAnyDbPrivileges(PrivilegeVector* privileges) {
    Privilege::addPrivilegeToPrivilegeVector(
        privileges, Privilege(ResourcePattern::forAnyNormalResource(), userAdminRoleActions));
    Privilege::addPrivilegeToPrivilegeVector(
        privileges, Privilege(ResourcePattern::forClusterResource(), ActionType::listDatabases));
    Privilege::addPrivilegeToPrivilegeVector(
        privileges,
        Privilege(ResourcePattern::forClusterResource(), ActionType::authSchemaUpgrade));
    Privilege::addPrivilegeToPrivilegeVector(
        privileges,
        Privilege(ResourcePattern::forClusterResource(), ActionType::invalidateUserCache));


    ActionSet readRoleAndIndexActions;
    readRoleAndIndexActions += readRoleActions;
    readRoleAndIndexActions << ActionType::createIndex << ActionType::dropIndex;

    Privilege::addPrivilegeToPrivilegeVector(
        privileges, Privilege(ResourcePattern::forCollectionName("system.users"), readRoleActions));
    Privilege::addPrivilegeToPrivilegeVector(
        privileges,
        Privilege(
            ResourcePattern::forExactNamespace(AuthorizationManager::usersCollectionNamespace),
            readRoleAndIndexActions));
    Privilege::addPrivilegeToPrivilegeVector(
        privileges,
        Privilege(
            ResourcePattern::forExactNamespace(AuthorizationManager::rolesCollectionNamespace),
            readRoleAndIndexActions));
    Privilege::addPrivilegeToPrivilegeVector(
        privileges,
        Privilege(
            ResourcePattern::forExactNamespace(AuthorizationManager::versionCollectionNamespace),
            readRoleActions));
    Privilege::addPrivilegeToPrivilegeVector(
        privileges,
        Privilege(
            ResourcePattern::forExactNamespace(AuthorizationManager::usersAltCollectionNamespace),
            readRoleActions));
    Privilege::addPrivilegeToPrivilegeVector(
        privileges,
        Privilege(ResourcePattern::forExactNamespace(
                      AuthorizationManager::usersBackupCollectionNamespace),
                  readRoleActions));
}

void addDbAdminAnyDbPrivileges(PrivilegeVector* privileges) {
    Privilege::addPrivilegeToPrivilegeVector(
        privileges, Privilege(ResourcePattern::forClusterResource(), ActionType::listDatabases));
    Privilege::addPrivilegeToPrivilegeVector(
        privileges, Privilege(ResourcePattern::forAnyNormalResource(), dbAdminRoleActions));
    Privilege::addPrivilegeToPrivilegeVector(
        privileges,
        Privilege(ResourcePattern::forCollectionName("system.indexes"), readRoleActions));
    Privilege::addPrivilegeToPrivilegeVector(
        privileges,
        Privilege(ResourcePattern::forCollectionName("system.namespaces"), readRoleActions));
    ActionSet profileActions = readRoleActions;
    profileActions.addAction(ActionType::convertToCapped);
    profileActions.addAction(ActionType::createCollection);
    profileActions.addAction(ActionType::dropCollection);
    Privilege::addPrivilegeToPrivilegeVector(
        privileges,
        Privilege(ResourcePattern::forCollectionName("system.profile"), profileActions));
}

void addClusterMonitorPrivileges(PrivilegeVector* privileges) {
    Privilege::addPrivilegeToPrivilegeVector(
        privileges,
        Privilege(ResourcePattern::forClusterResource(), clusterMonitorRoleClusterActions));
    Privilege::addPrivilegeToPrivilegeVector(
        privileges,
        Privilege(ResourcePattern::forAnyNormalResource(), clusterMonitorRoleDatabaseActions));
    addReadOnlyDbPrivileges(privileges, "config");
    Privilege::addPrivilegeToPrivilegeVector(
        privileges,
        Privilege(ResourcePattern::forExactNamespace(NamespaceString("local.system.replset")),
                  ActionType::find));
    Privilege::addPrivilegeToPrivilegeVector(
        privileges,
        Privilege(ResourcePattern::forExactNamespace(NamespaceString("local.sources")),
                  ActionType::find));
    Privilege::addPrivilegeToPrivilegeVector(
        privileges,
        Privilege(ResourcePattern::forCollectionName("system.profile"), ActionType::find));
}

void addHostManagerPrivileges(PrivilegeVector* privileges) {
    Privilege::addPrivilegeToPrivilegeVector(
        privileges,
        Privilege(ResourcePattern::forClusterResource(), hostManagerRoleClusterActions));
    Privilege::addPrivilegeToPrivilegeVector(
        privileges,
        Privilege(ResourcePattern::forAnyNormalResource(), hostManagerRoleDatabaseActions));
}

void addClusterManagerPrivileges(PrivilegeVector* privileges) {
    Privilege::addPrivilegeToPrivilegeVector(
        privileges,
        Privilege(ResourcePattern::forClusterResource(), clusterManagerRoleClusterActions));
    Privilege::addPrivilegeToPrivilegeVector(
        privileges,
        Privilege(ResourcePattern::forAnyNormalResource(), clusterManagerRoleDatabaseActions));
    addReadOnlyDbPrivileges(privileges, "config");

    ActionSet writeActions;
    writeActions << ActionType::insert << ActionType::update << ActionType::remove;
    Privilege::addPrivilegeToPrivilegeVector(
        privileges,
        Privilege(ResourcePattern::forExactNamespace(NamespaceString("config", "settings")),
                  writeActions));
    Privilege::addPrivilegeToPrivilegeVector(
        privileges,
        Privilege(ResourcePattern::forExactNamespace(NamespaceString("local", "system.replset")),
                  readRoleActions));
    Privilege::addPrivilegeToPrivilegeVector(
        privileges,
        Privilege(ResourcePattern::forExactNamespace(NamespaceString("config", "tags")),
                  writeActions));
    // Primarily for zone commands
    Privilege::addPrivilegeToPrivilegeVector(
        privileges,
        Privilege(ResourcePattern::forExactNamespace(NamespaceString("config", "shards")),
                  writeActions));
}

void addClusterAdminPrivileges(PrivilegeVector* privileges) {
    addClusterMonitorPrivileges(privileges);
    addHostManagerPrivileges(privileges);
    addClusterManagerPrivileges(privileges);
    Privilege::addPrivilegeToPrivilegeVector(
        privileges, Privilege(ResourcePattern::forAnyNormalResource(), ActionType::dropDatabase));
}

void addBackupPrivileges(PrivilegeVector* privileges) {
    Privilege::addPrivilegeToPrivilegeVector(
        privileges, Privilege(ResourcePattern::forAnyResource(), ActionType::collStats));
    Privilege::addPrivilegeToPrivilegeVector(
        privileges, Privilege(ResourcePattern::forAnyNormalResource(), ActionType::find));
    Privilege::addPrivilegeToPrivilegeVector(
        privileges, Privilege(ResourcePattern::forAnyResource(), ActionType::listCollections));
    Privilege::addPrivilegeToPrivilegeVector(
        privileges, Privilege(ResourcePattern::forAnyResource(), ActionType::listIndexes));

    ActionSet clusterActions;
    clusterActions << ActionType::getParameter  // To check authSchemaVersion
                   << ActionType::listDatabases << ActionType::appendOplogNote;  // For BRS
    Privilege::addPrivilegeToPrivilegeVector(
        privileges, Privilege(ResourcePattern::forClusterResource(), clusterActions));

    Privilege::addPrivilegeToPrivilegeVector(
        privileges,
        Privilege(ResourcePattern::forCollectionName("system.indexes"), ActionType::find));

    Privilege::addPrivilegeToPrivilegeVector(
        privileges,
        Privilege(ResourcePattern::forCollectionName("system.namespaces"), ActionType::find));

    Privilege::addPrivilegeToPrivilegeVector(
        privileges, Privilege(ResourcePattern::forCollectionName("system.js"), ActionType::find));

    Privilege::addPrivilegeToPrivilegeVector(
        privileges,
        Privilege(ResourcePattern::forCollectionName("system.users"), ActionType::find));

    Privilege::addPrivilegeToPrivilegeVector(
        privileges,
        Privilege(ResourcePattern::forCollectionName("system.profile"), ActionType::find));

    Privilege::addPrivilegeToPrivilegeVector(
        privileges,
        Privilege(
            ResourcePattern::forExactNamespace(AuthorizationManager::usersAltCollectionNamespace),
            ActionType::find));

    Privilege::addPrivilegeToPrivilegeVector(
        privileges,
        Privilege(ResourcePattern::forExactNamespace(
                      AuthorizationManager::usersBackupCollectionNamespace),
                  ActionType::find));

    Privilege::addPrivilegeToPrivilegeVector(
        privileges,
        Privilege(
            ResourcePattern::forExactNamespace(AuthorizationManager::rolesCollectionNamespace),
            ActionType::find));

    Privilege::addPrivilegeToPrivilegeVector(
        privileges,
        Privilege(
            ResourcePattern::forExactNamespace(AuthorizationManager::versionCollectionNamespace),
            ActionType::find));

    ActionSet configSettingsActions;
    configSettingsActions << ActionType::insert << ActionType::update << ActionType::find;
    Privilege::addPrivilegeToPrivilegeVector(
        privileges,
        Privilege(ResourcePattern::forExactNamespace(NamespaceString("config", "settings")),
                  configSettingsActions));
}

void addRestorePrivileges(PrivilegeVector* privileges) {
    ActionSet actions;
    actions << ActionType::bypassDocumentValidation << ActionType::collMod
            << ActionType::createCollection << ActionType::createIndex << ActionType::dropCollection
            << ActionType::insert;

    Privilege::addPrivilegeToPrivilegeVector(
        privileges, Privilege(ResourcePattern::forAnyNormalResource(), actions));

    Privilege::addPrivilegeToPrivilegeVector(
        privileges, Privilege(ResourcePattern::forCollectionName("system.js"), actions));

    // Need to be able to query system.namespaces to check existing collection options.
    Privilege::addPrivilegeToPrivilegeVector(
        privileges,
        Privilege(ResourcePattern::forCollectionName("system.namespaces"), ActionType::find));
    Privilege::addPrivilegeToPrivilegeVector(
        privileges, Privilege(ResourcePattern::forAnyResource(), ActionType::listCollections));

    // Privileges for user/role management
    Privilege::addPrivilegeToPrivilegeVector(
        privileges, Privilege(ResourcePattern::forAnyNormalResource(), userAdminRoleActions));

    Privilege::addPrivilegeToPrivilegeVector(
        privileges,
        Privilege(ResourcePattern::forExactNamespace(
                      AuthorizationManager::defaultTempUsersCollectionNamespace),
                  ActionType::find));

    Privilege::addPrivilegeToPrivilegeVector(
        privileges,
        Privilege(ResourcePattern::forExactNamespace(
                      AuthorizationManager::defaultTempRolesCollectionNamespace),
                  ActionType::find));

    Privilege::addPrivilegeToPrivilegeVector(
        privileges,
        Privilege(
            ResourcePattern::forExactNamespace(AuthorizationManager::usersAltCollectionNamespace),
            actions));

    Privilege::addPrivilegeToPrivilegeVector(
        privileges,
        Privilege(ResourcePattern::forExactNamespace(
                      AuthorizationManager::usersBackupCollectionNamespace),
                  actions));

    actions << ActionType::find;
    Privilege::addPrivilegeToPrivilegeVector(
        privileges,
        Privilege(
            ResourcePattern::forExactNamespace(AuthorizationManager::versionCollectionNamespace),
            actions));

    // Need additional actions on system.users.
    actions << ActionType::update << ActionType::remove;
    Privilege::addPrivilegeToPrivilegeVector(
        privileges, Privilege(ResourcePattern::forCollectionName("system.users"), actions));

    // Need to be able to run getParameter to check authSchemaVersion
    Privilege::addPrivilegeToPrivilegeVector(
        privileges, Privilege(ResourcePattern::forClusterResource(), ActionType::getParameter));

    // Need to be able to create an index on the system.roles collection.
    Privilege::addPrivilegeToPrivilegeVector(
        privileges,
        Privilege(
            ResourcePattern::forExactNamespace(AuthorizationManager::rolesCollectionNamespace),
            ActionType::createIndex));
}

void addRootRolePrivileges(PrivilegeVector* privileges) {
    addClusterAdminPrivileges(privileges);
    addUserAdminAnyDbPrivileges(privileges);
    addDbAdminAnyDbPrivileges(privileges);
    addReadWriteAnyDbPrivileges(privileges);
    addRestorePrivileges(privileges);
    Privilege::addPrivilegeToPrivilegeVector(
        privileges, Privilege(ResourcePattern::forAnyResource(), ActionType::validate));
}

void addInternalRolePrivileges(PrivilegeVector* privileges) {
    RoleGraph::generateUniversalPrivileges(privileges);
}

}  // namespace

bool RoleGraph::addPrivilegesForBuiltinRole(const RoleName& roleName, PrivilegeVector* result) {
    const bool isAdminDB = (roleName.getDB() == ADMIN_DBNAME);

    if (roleName.getRole() == BUILTIN_ROLE_READ) {
        addReadOnlyDbPrivileges(result, roleName.getDB());
    } else if (roleName.getRole() == BUILTIN_ROLE_READ_WRITE) {
        addReadWriteDbPrivileges(result, roleName.getDB());
    } else if (roleName.getRole() == BUILTIN_ROLE_USER_ADMIN) {
        addUserAdminDbPrivileges(result, roleName.getDB());
    } else if (roleName.getRole() == BUILTIN_ROLE_DB_ADMIN) {
        addDbAdminDbPrivileges(result, roleName.getDB());
    } else if (roleName.getRole() == BUILTIN_ROLE_DB_OWNER) {
        addDbOwnerPrivileges(result, roleName.getDB());
    } else if (roleName.getRole() == BUILTIN_ROLE_ENABLE_SHARDING) {
        addEnableShardingPrivileges(result);
    } else if (isAdminDB && roleName.getRole() == BUILTIN_ROLE_READ_ANY_DB) {
        addReadOnlyAnyDbPrivileges(result);
    } else if (isAdminDB && roleName.getRole() == BUILTIN_ROLE_READ_WRITE_ANY_DB) {
        addReadWriteAnyDbPrivileges(result);
    } else if (isAdminDB && roleName.getRole() == BUILTIN_ROLE_USER_ADMIN_ANY_DB) {
        addUserAdminAnyDbPrivileges(result);
    } else if (isAdminDB && roleName.getRole() == BUILTIN_ROLE_DB_ADMIN_ANY_DB) {
        addDbAdminAnyDbPrivileges(result);
    } else if (isAdminDB && roleName.getRole() == BUILTIN_ROLE_CLUSTER_MONITOR) {
        addClusterMonitorPrivileges(result);
    } else if (isAdminDB && roleName.getRole() == BUILTIN_ROLE_HOST_MANAGEMENT) {
        addHostManagerPrivileges(result);
    } else if (isAdminDB && roleName.getRole() == BUILTIN_ROLE_CLUSTER_MANAGEMENT) {
        addClusterManagerPrivileges(result);
    } else if (isAdminDB && roleName.getRole() == BUILTIN_ROLE_CLUSTER_ADMIN) {
        addClusterAdminPrivileges(result);
    } else if (isAdminDB && roleName.getRole() == BUILTIN_ROLE_BACKUP) {
        addBackupPrivileges(result);
    } else if (isAdminDB && roleName.getRole() == BUILTIN_ROLE_RESTORE) {
        addRestorePrivileges(result);
    } else if (isAdminDB && roleName.getRole() == BUILTIN_ROLE_ROOT) {
        addRootRolePrivileges(result);
    } else if (isAdminDB && roleName.getRole() == BUILTIN_ROLE_INTERNAL) {
        addInternalRolePrivileges(result);
    } else {
        return false;
    }
    return true;
}

void RoleGraph::generateUniversalPrivileges(PrivilegeVector* privileges) {
    ActionSet allActions;
    allActions.addAllActions();
    privileges->push_back(Privilege(ResourcePattern::forAnyResource(), allActions));
}

bool RoleGraph::isBuiltinRole(const RoleName& role) {
    if (!NamespaceString::validDBName(role.getDB(),
                                      NamespaceString::DollarInDbNameBehavior::Allow) ||
        role.getDB() == "$external") {
        return false;
    }

    bool isAdminDB = role.getDB() == ADMIN_DBNAME;

    if (role.getRole() == BUILTIN_ROLE_READ) {
        return true;
    } else if (role.getRole() == BUILTIN_ROLE_READ_WRITE) {
        return true;
    } else if (role.getRole() == BUILTIN_ROLE_USER_ADMIN) {
        return true;
    } else if (role.getRole() == BUILTIN_ROLE_DB_ADMIN) {
        return true;
    } else if (role.getRole() == BUILTIN_ROLE_DB_OWNER) {
        return true;
    } else if (role.getRole() == BUILTIN_ROLE_ENABLE_SHARDING) {
        return true;
    } else if (isAdminDB && role.getRole() == BUILTIN_ROLE_READ_ANY_DB) {
        return true;
    } else if (isAdminDB && role.getRole() == BUILTIN_ROLE_READ_WRITE_ANY_DB) {
        return true;
    } else if (isAdminDB && role.getRole() == BUILTIN_ROLE_USER_ADMIN_ANY_DB) {
        return true;
    } else if (isAdminDB && role.getRole() == BUILTIN_ROLE_DB_ADMIN_ANY_DB) {
        return true;
    } else if (isAdminDB && role.getRole() == BUILTIN_ROLE_CLUSTER_MONITOR) {
        return true;
    } else if (isAdminDB && role.getRole() == BUILTIN_ROLE_HOST_MANAGEMENT) {
        return true;
    } else if (isAdminDB && role.getRole() == BUILTIN_ROLE_CLUSTER_MANAGEMENT) {
        return true;
    } else if (isAdminDB && role.getRole() == BUILTIN_ROLE_CLUSTER_ADMIN) {
        return true;
    } else if (isAdminDB && role.getRole() == BUILTIN_ROLE_BACKUP) {
        return true;
    } else if (isAdminDB && role.getRole() == BUILTIN_ROLE_RESTORE) {
        return true;
    } else if (isAdminDB && role.getRole() == BUILTIN_ROLE_ROOT) {
        return true;
    } else if (isAdminDB && role.getRole() == BUILTIN_ROLE_INTERNAL) {
        return true;
    }
    return false;
}

void RoleGraph::_createBuiltinRolesForDBIfNeeded(const std::string& dbname) {
    _createBuiltinRoleIfNeeded(RoleName(BUILTIN_ROLE_READ, dbname));
    _createBuiltinRoleIfNeeded(RoleName(BUILTIN_ROLE_READ_WRITE, dbname));
    _createBuiltinRoleIfNeeded(RoleName(BUILTIN_ROLE_USER_ADMIN, dbname));
    _createBuiltinRoleIfNeeded(RoleName(BUILTIN_ROLE_DB_ADMIN, dbname));
    _createBuiltinRoleIfNeeded(RoleName(BUILTIN_ROLE_DB_OWNER, dbname));
    _createBuiltinRoleIfNeeded(RoleName(BUILTIN_ROLE_ENABLE_SHARDING, dbname));

    if (dbname == "admin") {
        _createBuiltinRoleIfNeeded(RoleName(BUILTIN_ROLE_READ_ANY_DB, dbname));
        _createBuiltinRoleIfNeeded(RoleName(BUILTIN_ROLE_READ_WRITE_ANY_DB, dbname));
        _createBuiltinRoleIfNeeded(RoleName(BUILTIN_ROLE_USER_ADMIN_ANY_DB, dbname));
        _createBuiltinRoleIfNeeded(RoleName(BUILTIN_ROLE_DB_ADMIN_ANY_DB, dbname));
        _createBuiltinRoleIfNeeded(RoleName(BUILTIN_ROLE_CLUSTER_MONITOR, dbname));
        _createBuiltinRoleIfNeeded(RoleName(BUILTIN_ROLE_HOST_MANAGEMENT, dbname));
        _createBuiltinRoleIfNeeded(RoleName(BUILTIN_ROLE_CLUSTER_MANAGEMENT, dbname));
        _createBuiltinRoleIfNeeded(RoleName(BUILTIN_ROLE_CLUSTER_ADMIN, dbname));
        _createBuiltinRoleIfNeeded(RoleName(BUILTIN_ROLE_BACKUP, dbname));
        _createBuiltinRoleIfNeeded(RoleName(BUILTIN_ROLE_RESTORE, dbname));
        _createBuiltinRoleIfNeeded(RoleName(BUILTIN_ROLE_ROOT, dbname));
        _createBuiltinRoleIfNeeded(RoleName(BUILTIN_ROLE_INTERNAL, dbname));
    }
}

void RoleGraph::_createBuiltinRoleIfNeeded(const RoleName& role) {
    if (!isBuiltinRole(role) || _roleExistsDontCreateBuiltin(role)) {
        return;
    }

    _createRoleDontCheckIfRoleExists(role);
    PrivilegeVector privileges;
    fassert(17145, addPrivilegesForBuiltinRole(role, &privileges));
    for (size_t i = 0; i < privileges.size(); ++i) {
        _addPrivilegeToRoleNoChecks(role, privileges[i]);
        _allPrivilegesForRole[role].push_back(privileges[i]);
    }
}

}  // namespace mongo
