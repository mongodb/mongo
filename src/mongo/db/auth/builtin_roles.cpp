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

#include "mongo/db/auth/builtin_roles.h"

#include "mongo/base/init.h"
#include "mongo/base/status.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/auth/role_name.h"

namespace mongo {

namespace {
constexpr StringData ADMIN_DBNAME = "admin"_sd;
constexpr StringData BUILTIN_ROLE_READ = "read"_sd;
constexpr StringData BUILTIN_ROLE_READ_WRITE = "readWrite"_sd;
constexpr StringData BUILTIN_ROLE_USER_ADMIN = "userAdmin"_sd;
constexpr StringData BUILTIN_ROLE_DB_ADMIN = "dbAdmin"_sd;
constexpr StringData BUILTIN_ROLE_CLUSTER_ADMIN = "clusterAdmin"_sd;
constexpr StringData BUILTIN_ROLE_READ_ANY_DB = "readAnyDatabase"_sd;
constexpr StringData BUILTIN_ROLE_READ_WRITE_ANY_DB = "readWriteAnyDatabase"_sd;
constexpr StringData BUILTIN_ROLE_USER_ADMIN_ANY_DB = "userAdminAnyDatabase"_sd;
constexpr StringData BUILTIN_ROLE_DB_ADMIN_ANY_DB = "dbAdminAnyDatabase"_sd;
constexpr StringData BUILTIN_ROLE_ROOT = "root"_sd;
constexpr StringData BUILTIN_ROLE_INTERNAL = "__system"_sd;
constexpr StringData BUILTIN_ROLE_DB_OWNER = "dbOwner"_sd;
constexpr StringData BUILTIN_ROLE_CLUSTER_MONITOR = "clusterMonitor"_sd;
constexpr StringData BUILTIN_ROLE_HOST_MANAGEMENT = "hostManager"_sd;
constexpr StringData BUILTIN_ROLE_CLUSTER_MANAGEMENT = "clusterManager"_sd;
constexpr StringData BUILTIN_ROLE_BACKUP = "backup"_sd;
constexpr StringData BUILTIN_ROLE_RESTORE = "restore"_sd;
constexpr StringData BUILTIN_ROLE_ENABLE_SHARDING = "enableSharding"_sd;
constexpr StringData BUILTIN_ROLE_QUERYABLE_BACKUP = "__queryableBackup"_sd;
constexpr StringData BUILTIN_ROLE_DIRECT_SHARD_OPERATIONS = "directShardOperations"_sd;

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
        << ActionType::changeStream
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
        << ActionType::compactStructuredEncryptionData
        << ActionType::convertToCapped  // db admin gets this also
        << ActionType::createCollection  // db admin gets this also
        << ActionType::createIndex
        << ActionType::dropCollection
        << ActionType::dropIndex
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
        << ActionType::setAuthenticationRestriction
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
        << ActionType::storageDetails
        << ActionType::validate;

    // clusterMonitor role actions that target the cluster resource
    clusterMonitorRoleClusterActions
        << ActionType::checkFreeMonitoringStatus
        << ActionType::connPoolStats
        << ActionType::getCmdLineOpts
        << ActionType::getDefaultRWConcern // clusterManager gets this also
        << ActionType::getLog
        << ActionType::getParameter
        << ActionType::getShardMap
        << ActionType::hostInfo
        << ActionType::listDatabases
        << ActionType::listSessions // clusterManager gets this also
        << ActionType::listShards  // clusterManager gets this also
        << ActionType::netstat
        << ActionType::operationMetrics
        << ActionType::replSetGetConfig  // clusterManager gets this also
        << ActionType::replSetGetStatus  // clusterManager gets this also
        << ActionType::serverStatus
        << ActionType::top
        << ActionType::useUUID
        << ActionType::inprog
        << ActionType::shardingState;

    // clusterMonitor role actions that target a database (or collection) resource
    clusterMonitorRoleDatabaseActions
        << ActionType::collStats  // dbAdmin gets this also
        << ActionType::dbStats  // dbAdmin gets this also
        << ActionType::getDatabaseVersion
        << ActionType::getShardVersion
        << ActionType::indexStats;

    // hostManager role actions that target the cluster resource
    hostManagerRoleClusterActions
        << ActionType::applicationMessage  // clusterManager gets this also
        << ActionType::auditConfigure
        << ActionType::connPoolSync
        << ActionType::dropConnections
        << ActionType::logRotate
        << ActionType::oidReset
        << ActionType::setParameter
        << ActionType::shutdown
        << ActionType::touch
        << ActionType::unlock
        << ActionType::flushRouterConfig  // clusterManager gets this also
        << ActionType::fsync
        << ActionType::invalidateUserCache // userAdminAnyDatabase gets this also
        << ActionType::killAnyCursor
        << ActionType::killAnySession
        << ActionType::killop
        << ActionType::replSetResizeOplog
        << ActionType::resync  // clusterManager gets this also
        << ActionType::trafficRecord
        << ActionType::rotateCertificates;

    // hostManager role actions that target the database resource
    hostManagerRoleDatabaseActions
        << ActionType::killCursors;


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
        << ActionType::listSessions  // clusterMonitor gets this also
        << ActionType::listShards  // clusterMonitor gets this also
        << ActionType::flushRouterConfig  // hostManager gets this also
        << ActionType::cleanupOrphaned
        << ActionType::getDefaultRWConcern // clusterMonitor gets this also
        << ActionType::runTenantMigration
        << ActionType::setDefaultRWConcern
        << ActionType::setFeatureCompatibilityVersion
        << ActionType::setFreeMonitoring
        << ActionType::setClusterParameter
        << ActionType::getClusterParameter;

    clusterManagerRoleDatabaseActions
        << ActionType::clearJumboFlag
        << ActionType::splitChunk
        << ActionType::moveChunk
        << ActionType::enableSharding
        << ActionType::splitVector
        << ActionType::refineCollectionShardKey
        << ActionType::reshardCollection;
}
// clang-format on

void addReadOnlyDbPrivileges(PrivilegeVector* privileges, StringData dbName) {
    Privilege::addPrivilegeToPrivilegeVector(
        privileges, Privilege(ResourcePattern::forDatabaseName(dbName), readRoleActions));
    Privilege::addPrivilegeToPrivilegeVector(
        privileges,
        Privilege(ResourcePattern::forExactNamespace(NamespaceString(dbName, "system.js")),
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
    Privilege::addPrivilegeToPrivilegeVector(
        privileges, Privilege(ResourcePattern::forDatabaseName(dbName), userAdminRoleActions));
}

void addDbAdminDbPrivileges(PrivilegeVector* privileges, StringData dbName) {
    Privilege::addPrivilegeToPrivilegeVector(
        privileges, Privilege(ResourcePattern::forDatabaseName(dbName), dbAdminRoleActions));

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
    enableShardingActions.addAction(ActionType::refineCollectionShardKey);
    enableShardingActions.addAction(ActionType::reshardCollection);
    Privilege::addPrivilegeToPrivilegeVector(
        privileges, Privilege(ResourcePattern::forAnyNormalResource(), enableShardingActions));
}

void addReadOnlyAnyDbPrivileges(PrivilegeVector* privileges) {
    Privilege::addPrivilegeToPrivilegeVector(
        privileges, Privilege(ResourcePattern::forAnyNormalResource(), readRoleActions));
    Privilege::addPrivilegeToPrivilegeVector(
        privileges, Privilege(ResourcePattern::forClusterResource(), ActionType::listDatabases));
    Privilege::addPrivilegeToPrivilegeVector(
        privileges, Privilege(ResourcePattern::forCollectionName("system.js"), readRoleActions));
    Privilege::addPrivilegeToPrivilegeVector(
        privileges, Privilege(ResourcePattern::forAnySystemBuckets(), readRoleActions));
}

void addReadWriteAnyDbPrivileges(PrivilegeVector* privileges) {
    addReadOnlyAnyDbPrivileges(privileges);
    Privilege::addPrivilegeToPrivilegeVector(
        privileges, Privilege(ResourcePattern::forAnyNormalResource(), readWriteRoleActions));
    Privilege::addPrivilegeToPrivilegeVector(
        privileges,
        Privilege(ResourcePattern::forCollectionName("system.js"), readWriteRoleActions));
    Privilege::addPrivilegeToPrivilegeVector(
        privileges, Privilege(ResourcePattern::forAnySystemBuckets(), readWriteRoleActions));
}

void addUserAdminAnyDbPrivileges(PrivilegeVector* privileges) {
    Privilege::addPrivilegeToPrivilegeVector(
        privileges, Privilege(ResourcePattern::forAnyNormalResource(), userAdminRoleActions));
    Privilege::addPrivilegeToPrivilegeVector(
        privileges, Privilege(ResourcePattern::forDatabaseName("local"), userAdminRoleActions));
    Privilege::addPrivilegeToPrivilegeVector(
        privileges, Privilege(ResourcePattern::forDatabaseName("config"), userAdminRoleActions));
    Privilege::addPrivilegeToPrivilegeVector(
        privileges, Privilege(ResourcePattern::forClusterResource(), ActionType::listDatabases));
    Privilege::addPrivilegeToPrivilegeVector(
        privileges,
        Privilege(ResourcePattern::forClusterResource(), ActionType::authSchemaUpgrade));
    Privilege::addPrivilegeToPrivilegeVector(
        privileges,
        Privilege(ResourcePattern::forClusterResource(), ActionType::invalidateUserCache));
    Privilege::addPrivilegeToPrivilegeVector(
        privileges, Privilege(ResourcePattern::forClusterResource(), ActionType::viewUser));
    Privilege::addPrivilegeToPrivilegeVector(
        privileges,
        Privilege(ResourcePattern::forAnyNormalResource(), ActionType::listCachedAndActiveUsers));

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
        Privilege(ResourcePattern::forExactNamespace(
                      AuthorizationManager::usersBackupCollectionNamespace),
                  readRoleActions));
}

void addDbAdminAnyDbPrivileges(PrivilegeVector* privileges) {
    Privilege::addPrivilegeToPrivilegeVector(
        privileges, Privilege(ResourcePattern::forClusterResource(), ActionType::listDatabases));
    Privilege::addPrivilegeToPrivilegeVector(
        privileges, Privilege(ResourcePattern::forAnyNormalResource(), dbAdminRoleActions));
    ActionSet profileActions = readRoleActions;
    profileActions.addAction(ActionType::convertToCapped);
    profileActions.addAction(ActionType::createCollection);
    profileActions.addAction(ActionType::dropCollection);
    Privilege::addPrivilegeToPrivilegeVector(
        privileges,
        Privilege(ResourcePattern::forCollectionName("system.profile"), profileActions));
    Privilege::addPrivilegeToPrivilegeVector(
        privileges, Privilege(ResourcePattern::forClusterResource(), ActionType::applyOps));
    Privilege::addPrivilegeToPrivilegeVector(
        privileges, Privilege(ResourcePattern::forAnySystemBuckets(), dbAdminRoleActions));
}

void addClusterMonitorPrivileges(PrivilegeVector* privileges) {
    Privilege::addPrivilegeToPrivilegeVector(
        privileges,
        Privilege(ResourcePattern::forClusterResource(), clusterMonitorRoleClusterActions));
    Privilege::addPrivilegeToPrivilegeVector(
        privileges,
        Privilege(ResourcePattern::forAnyNormalResource(), clusterMonitorRoleDatabaseActions));
    Privilege::addPrivilegeToPrivilegeVector(
        privileges,
        Privilege(ResourcePattern::forDatabaseName("config"), clusterMonitorRoleDatabaseActions));
    Privilege::addPrivilegeToPrivilegeVector(
        privileges,
        Privilege(ResourcePattern::forDatabaseName("local"), clusterMonitorRoleDatabaseActions));
    addReadOnlyDbPrivileges(privileges, "config");
    addReadOnlyDbPrivileges(privileges, "local");
    Privilege::addPrivilegeToPrivilegeVector(
        privileges,
        Privilege(ResourcePattern::forExactNamespace(NamespaceString("local", "system.replset")),
                  ActionType::find));
    Privilege::addPrivilegeToPrivilegeVector(
        privileges,
        Privilege(ResourcePattern::forExactNamespace(NamespaceString("local", "replset.election")),
                  ActionType::find));
    Privilege::addPrivilegeToPrivilegeVector(
        privileges,
        Privilege(ResourcePattern::forExactNamespace(NamespaceString("local", "replset.minvalid")),
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
    Privilege::addPrivilegeToPrivilegeVector(
        privileges,
        Privilege(ResourcePattern::forAnySystemBuckets(), clusterManagerRoleDatabaseActions));
    Privilege::addPrivilegeToPrivilegeVector(
        privileges,
        Privilege(ResourcePattern::forDatabaseName("config"), clusterManagerRoleDatabaseActions));
    Privilege::addPrivilegeToPrivilegeVector(
        privileges,
        Privilege(ResourcePattern::forDatabaseName("local"), clusterManagerRoleDatabaseActions));
    Privilege::addPrivilegeToPrivilegeVector(
        privileges,
        Privilege(ResourcePattern::forExactNamespace(NamespaceString("local", "system.replset")),
                  readRoleActions));
    addReadOnlyDbPrivileges(privileges, "config");

    Privilege::addPrivilegeToPrivilegeVector(
        privileges, Privilege(ResourcePattern::forAnyResource(), ActionType::dbCheck));
    Privilege::addPrivilegeToPrivilegeVector(
        privileges,
        Privilege(ResourcePattern::forExactNamespace(NamespaceString("local", "system.healthlog")),
                  readRoleActions));

    ActionSet writeActions;
    writeActions << ActionType::insert << ActionType::update << ActionType::remove;
    Privilege::addPrivilegeToPrivilegeVector(
        privileges, Privilege(ResourcePattern::forDatabaseName("config"), writeActions));
    Privilege::addPrivilegeToPrivilegeVector(
        privileges, Privilege(ResourcePattern::forDatabaseName("local"), writeActions));
    Privilege::addPrivilegeToPrivilegeVector(
        privileges,
        Privilege(ResourcePattern::forExactNamespace(NamespaceString("local", "replset.election")),
                  writeActions));
    Privilege::addPrivilegeToPrivilegeVector(
        privileges,
        Privilege(ResourcePattern::forExactNamespace(NamespaceString("local", "replset.minvalid")),
                  writeActions));
}

void addClusterAdminPrivileges(PrivilegeVector* privileges) {
    addClusterMonitorPrivileges(privileges);
    addHostManagerPrivileges(privileges);
    addClusterManagerPrivileges(privileges);
    Privilege::addPrivilegeToPrivilegeVector(
        privileges, Privilege(ResourcePattern::forAnyNormalResource(), ActionType::dropDatabase));
    Privilege::addPrivilegeToPrivilegeVector(
        privileges, Privilege(ResourcePattern::forAnyResource(), ActionType::importCollection));
    Privilege::addPrivilegeToPrivilegeVector(
        privileges, Privilege(ResourcePattern::forAnyResource(), ActionType::exportCollection));
}


void addQueryableBackupPrivileges(PrivilegeVector* privileges) {
    Privilege::addPrivilegeToPrivilegeVector(
        privileges, Privilege(ResourcePattern::forAnyResource(), ActionType::collStats));
    Privilege::addPrivilegeToPrivilegeVector(
        privileges, Privilege(ResourcePattern::forAnyNormalResource(), ActionType::find));
    Privilege::addPrivilegeToPrivilegeVector(
        privileges, Privilege(ResourcePattern::forAnyResource(), ActionType::listCollections));
    Privilege::addPrivilegeToPrivilegeVector(
        privileges, Privilege(ResourcePattern::forAnyResource(), ActionType::listIndexes));
    Privilege::addPrivilegeToPrivilegeVector(
        privileges, Privilege(ResourcePattern::forAnySystemBuckets(), ActionType::find));

    ActionSet clusterActions;
    clusterActions << ActionType::getParameter  // To check authSchemaVersion
                   << ActionType::listDatabases << ActionType::useUUID;
    Privilege::addPrivilegeToPrivilegeVector(
        privileges, Privilege(ResourcePattern::forClusterResource(), clusterActions));

    Privilege::addPrivilegeToPrivilegeVector(
        privileges, Privilege(ResourcePattern::forDatabaseName("config"), ActionType::find));

    Privilege::addPrivilegeToPrivilegeVector(
        privileges, Privilege(ResourcePattern::forDatabaseName("local"), ActionType::find));

    Privilege::addPrivilegeToPrivilegeVector(
        privileges,
        Privilege(ResourcePattern::forExactNamespace(NamespaceString("local", "replset.election")),
                  ActionType::find));
    Privilege::addPrivilegeToPrivilegeVector(
        privileges,
        Privilege(ResourcePattern::forExactNamespace(NamespaceString("local", "replset.minvalid")),
                  ActionType::find));

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

    Privilege::addPrivilegeToPrivilegeVector(
        privileges,
        Privilege(ResourcePattern::forExactNamespace(NamespaceString("config", "settings")),
                  ActionType::find));
}

void addBackupPrivileges(PrivilegeVector* privileges) {
    ActionSet clusterActions;
    clusterActions << ActionType::appendOplogNote;        // For BRS
    clusterActions << ActionType::serverStatus;           // For push based initial sync
    clusterActions << ActionType::setUserWriteBlockMode;  // For C2C replication
    Privilege::addPrivilegeToPrivilegeVector(
        privileges, Privilege(ResourcePattern::forClusterResource(), clusterActions));

    ActionSet configSettingsActions;
    configSettingsActions << ActionType::insert << ActionType::update;
    Privilege::addPrivilegeToPrivilegeVector(
        privileges,
        Privilege(ResourcePattern::forExactNamespace(NamespaceString("config", "settings")),
                  configSettingsActions));

    addQueryableBackupPrivileges(privileges);
}

void addRestorePrivileges(PrivilegeVector* privileges) {
    ActionSet actions;
    actions << ActionType::bypassDocumentValidation << ActionType::collMod
            << ActionType::convertToCapped << ActionType::createCollection
            << ActionType::createIndex << ActionType::dropCollection << ActionType::insert;

    Privilege::addPrivilegeToPrivilegeVector(
        privileges, Privilege(ResourcePattern::forAnyNormalResource(), actions));

    Privilege::addPrivilegeToPrivilegeVector(
        privileges, Privilege(ResourcePattern::forCollectionName("system.js"), actions));

    Privilege::addPrivilegeToPrivilegeVector(
        privileges, Privilege(ResourcePattern::forAnyResource(), ActionType::listCollections));

    Privilege::addPrivilegeToPrivilegeVector(
        privileges, Privilege(ResourcePattern::forDatabaseName("config"), actions));

    Privilege::addPrivilegeToPrivilegeVector(
        privileges, Privilege(ResourcePattern::forAnySystemBuckets(), actions));

    Privilege::addPrivilegeToPrivilegeVector(
        privileges,
        Privilege(ResourcePattern::forExactNamespace(NamespaceString("local", "system.replset")),
                  actions));
    Privilege::addPrivilegeToPrivilegeVector(
        privileges,
        Privilege(ResourcePattern::forExactNamespace(NamespaceString("local", "replset.election")),
                  actions));
    Privilege::addPrivilegeToPrivilegeVector(
        privileges,
        Privilege(ResourcePattern::forExactNamespace(NamespaceString("local", "replset.minvalid")),
                  actions));

    Privilege::addPrivilegeToPrivilegeVector(
        privileges, Privilege(ResourcePattern::forDatabaseName("local"), actions));

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

    Privilege::addPrivilegeToPrivilegeVector(
        privileges,
        Privilege(ResourcePattern::forClusterResource(),
                  {
                      // Need to be able to force UUID consistency in sharded restores
                      ActionType::forceUUID,
                      ActionType::useUUID,
                      // Need to be able to set and bypass write blocking mode for C2C replication
                      ActionType::bypassWriteBlockingMode,
                      ActionType::setUserWriteBlockMode,
                      // Needed for `mongorestore --preserveUUID`
                      ActionType::applyOps,
                  }));
}

void addRootRolePrivileges(PrivilegeVector* privileges) {
    addClusterAdminPrivileges(privileges);
    addUserAdminAnyDbPrivileges(privileges);
    addDbAdminAnyDbPrivileges(privileges);
    addReadWriteAnyDbPrivileges(privileges);
    addBackupPrivileges(privileges);
    addRestorePrivileges(privileges);

    Privilege::addPrivilegeToPrivilegeVector(
        privileges, Privilege(ResourcePattern::forAnyResource(), ActionType::validate));

    // Root users may specify `$tenant` override to command parameters.
    Privilege::addPrivilegeToPrivilegeVector(
        privileges, Privilege(ResourcePattern::forClusterResource(), ActionType::useTenant));

    // Grant privilege to 'root' to perform 'find' and 'remove' on pre-images collection.
    Privilege::addPrivilegeToPrivilegeVector(
        privileges,
        Privilege(
            ResourcePattern::forExactNamespace(NamespaceString::kChangeStreamPreImagesNamespace),
            {ActionType::find, ActionType::remove}));
}

void addInternalRolePrivileges(PrivilegeVector* privileges) {
    auth::generateUniversalPrivileges(privileges);
}

// Placeholder role with no privileges for 6.0.0. From 7.0 onwards, this method will assign
// the requisite privileges to write directly to shards.
void addDirectShardOperationsPrivileges(PrivilegeVector* privileges) {}

class BuiltinRoleDefinition {
public:
    BuiltinRoleDefinition() = delete;

    using AddPrivilegesFn = void (*)(PrivilegeVector*);
    BuiltinRoleDefinition(bool adminOnly, AddPrivilegesFn fn)
        : _adminOnly(adminOnly), _addPrivileges(fn) {}

    using AddPrivilegesWithDBFn = void (*)(PrivilegeVector*, StringData);
    BuiltinRoleDefinition(bool adminOnly, AddPrivilegesWithDBFn fn)
        : _adminOnly(adminOnly), _addPrivilegesWithDB(fn) {}

    bool adminOnly() const {
        return _adminOnly;
    }

    void operator()(PrivilegeVector* result, StringData dbname) const {
        if (_addPrivileges) {
            dassert(!_addPrivilegesWithDB);
            _addPrivileges(result);
        } else {
            dassert(_addPrivilegesWithDB);
            _addPrivilegesWithDB(result, dbname);
        }
    }

private:
    bool _adminOnly;
    AddPrivilegesFn _addPrivileges = nullptr;
    AddPrivilegesWithDBFn _addPrivilegesWithDB = nullptr;
};

const std::map<StringData, BuiltinRoleDefinition> kBuiltinRoles({
    // All DBs.
    {BUILTIN_ROLE_READ, {false, addReadOnlyDbPrivileges}},
    {BUILTIN_ROLE_READ_WRITE, {false, addReadWriteDbPrivileges}},
    {BUILTIN_ROLE_USER_ADMIN, {false, addUserAdminDbPrivileges}},
    {BUILTIN_ROLE_DB_ADMIN, {false, addDbAdminDbPrivileges}},
    {BUILTIN_ROLE_DB_OWNER, {false, addDbOwnerPrivileges}},
    {BUILTIN_ROLE_ENABLE_SHARDING, {false, addEnableShardingPrivileges}},
    // Admin Only.
    {BUILTIN_ROLE_READ_ANY_DB, {true, addReadOnlyAnyDbPrivileges}},
    {BUILTIN_ROLE_READ_WRITE_ANY_DB, {true, addReadWriteAnyDbPrivileges}},
    {BUILTIN_ROLE_USER_ADMIN_ANY_DB, {true, addUserAdminAnyDbPrivileges}},
    {BUILTIN_ROLE_DB_ADMIN_ANY_DB, {true, addDbAdminAnyDbPrivileges}},
    {BUILTIN_ROLE_CLUSTER_MONITOR, {true, addClusterMonitorPrivileges}},
    {BUILTIN_ROLE_HOST_MANAGEMENT, {true, addHostManagerPrivileges}},
    {BUILTIN_ROLE_CLUSTER_MANAGEMENT, {true, addClusterManagerPrivileges}},
    {BUILTIN_ROLE_CLUSTER_ADMIN, {true, addClusterAdminPrivileges}},
    {BUILTIN_ROLE_QUERYABLE_BACKUP, {true, addQueryableBackupPrivileges}},
    {BUILTIN_ROLE_BACKUP, {true, addBackupPrivileges}},
    {BUILTIN_ROLE_RESTORE, {true, addRestorePrivileges}},
    {BUILTIN_ROLE_ROOT, {true, addRootRolePrivileges}},
    {BUILTIN_ROLE_INTERNAL, {true, addInternalRolePrivileges}},
    {BUILTIN_ROLE_DIRECT_SHARD_OPERATIONS, {true, addDirectShardOperationsPrivileges}},
});

// $external is a virtual database used for X509, LDAP,
// and other authentication mechanisms and not used for storage.
// Therefore, granting privileges on this database does not make sense.
bool isValidDB(const DatabaseName& dbname) {
    return NamespaceString::validDBName(dbname, NamespaceString::DollarInDbNameBehavior::Allow) &&
        (dbname.db() != NamespaceString::kExternalDb);
}

}  // namespace

stdx::unordered_set<RoleName> auth::getBuiltinRoleNamesForDB(const DatabaseName& dbname) {
    if (!isValidDB(dbname)) {
        return {};
    }

    const bool isAdmin = dbname.db() == ADMIN_DBNAME;

    stdx::unordered_set<RoleName> roleNames;
    for (const auto& [role, def] : kBuiltinRoles) {
        if (isAdmin || !def.adminOnly()) {
            roleNames.insert(RoleName(role, dbname));
        }
    }
    return roleNames;
}

bool auth::addPrivilegesForBuiltinRole(const RoleName& roleName, PrivilegeVector* result) {
    auto role = roleName.getRole();
    auto dbname = roleName.getDB();

    if (!isValidDB(roleName.getDatabaseName())) {
        return false;
    }

    auto it = kBuiltinRoles.find(role);
    if (it == kBuiltinRoles.end()) {
        return false;
    }
    const auto& def = it->second;

    if (def.adminOnly() && (dbname != ADMIN_DBNAME)) {
        return false;
    }

    def(result, dbname);
    return true;
}

void auth::generateUniversalPrivileges(PrivilegeVector* privileges) {
    ActionSet allActions;
    allActions.addAllActions();
    Privilege::addPrivilegeToPrivilegeVector(
        privileges, Privilege(ResourcePattern::forAnyResource(), allActions));
}

bool auth::isBuiltinRole(const RoleName& role) {
    if (!isValidDB(role.getDatabaseName())) {
        return false;
    }

    const auto it = kBuiltinRoles.find(role.getRole());
    if (it == kBuiltinRoles.end()) {
        return false;
    }

    return !it->second.adminOnly() || (role.getDB() == ADMIN_DBNAME);
}

}  // namespace mongo
