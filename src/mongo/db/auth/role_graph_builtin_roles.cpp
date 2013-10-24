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

    const std::string RoleGraph::BUILTIN_ROLE_V0_READ = "read";
    const std::string RoleGraph::BUILTIN_ROLE_V0_READ_WRITE= "dbOwner";
    const std::string RoleGraph::BUILTIN_ROLE_V0_ADMIN_READ = "readAnyDatabase";
    const std::string RoleGraph::BUILTIN_ROLE_V0_ADMIN_READ_WRITE= "root";

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

    /// Actions that the clusterAdmin role may perform on the cluster resource.
    ActionSet clusterAdminRoleClusterActions;

    /// Actions that the clusterAdmin role may perform on any database.
    ActionSet clusterAdminRoleDatabaseActions;

    /// Actions that the "dbOwner" role may perform on normal resources of a specific database.
    ActionSet dbOwnerRoleActions;

    ActionSet& operator<<(ActionSet& target, ActionType source) {
        target.addAction(source);
        return target;
    }

    void operator+=(ActionSet& target, const ActionSet& source) {
        target.addAllActionsFromSet(source);
    }

    // This sets up the built-in role ActionSets.  This is what determines what actions each role
    // is authorized to perform
    MONGO_INITIALIZER(AuthorizationBuiltinRoles)(InitializerContext* context) {
        // Read role
        readRoleActions
            << ActionType::cloneCollectionLocalSource
            << ActionType::collStats
            << ActionType::dbHash
            << ActionType::dbStats
            << ActionType::find
            << ActionType::killCursors;

        // Read-write role
        readWriteRoleActions += readRoleActions;
        readWriteRoleActions
            << ActionType::cloneCollectionTarget
            << ActionType::convertToCapped // db admin gets this also
            << ActionType::createCollection // db admin gets this also
            << ActionType::dropCollection
            << ActionType::dropIndex
            << ActionType::emptycapped
            << ActionType::createIndex
            << ActionType::insert
            << ActionType::remove
            << ActionType::renameCollectionSameDB // db admin gets this also
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
            << ActionType::userAdmin
            << ActionType::viewUser
            << ActionType::viewRole;


        // DB admin role
        dbAdminRoleActions
            << ActionType::clean
            << ActionType::cloneCollectionLocalSource
            << ActionType::collMod
            << ActionType::collStats
            << ActionType::compact
            << ActionType::convertToCapped // read_write gets this also
            << ActionType::createCollection // read_write gets this also
            << ActionType::dbStats
            << ActionType::dropCollection
            << ActionType::dropIndex
            << ActionType::createIndex
            << ActionType::indexStats
            << ActionType::profileEnable
            << ActionType::reIndex
            << ActionType::renameCollectionSameDB // read_write gets this also
            << ActionType::storageDetails
            << ActionType::validate;

        // Cluster admin role actions that target the cluster resource.
        clusterAdminRoleClusterActions
            << ActionType::applicationMessage
            << ActionType::connPoolStats
            << ActionType::connPoolSync
            << ActionType::getCmdLineOpts
            << ActionType::getLog
            << ActionType::getParameter
            << ActionType::getShardMap
            << ActionType::getShardVersion
            << ActionType::hostInfo
            << ActionType::listDatabases
            << ActionType::listShards
            << ActionType::logRotate
            << ActionType::netstat
            << ActionType::replSetFreeze
            << ActionType::replSetGetStatus
            << ActionType::replSetMaintenance
            << ActionType::replSetStepDown
            << ActionType::replSetSyncFrom
            << ActionType::setParameter
            << ActionType::setShardVersion // TODO: should this be internal?
            << ActionType::serverStatus
            << ActionType::splitVector
            << ActionType::shutdown
            << ActionType::top
            << ActionType::touch
            << ActionType::unlock
            << ActionType::unsetSharding
            << ActionType::writeBacksQueued
            << ActionType::addShard
            << ActionType::cleanupOrphaned
            << ActionType::closeAllDatabases
            << ActionType::cpuProfiler
            << ActionType::cursorInfo
            << ActionType::diagLogging
            << ActionType::enableSharding
            << ActionType::flushRouterConfig
            << ActionType::fsync
            << ActionType::inprog
            << ActionType::invalidateUserCache
            << ActionType::killop
            << ActionType::mergeChunks
            << ActionType::moveChunk
            << ActionType::movePrimary
            << ActionType::removeShard
            << ActionType::replSetInitiate
            << ActionType::replSetReconfig
            << ActionType::resync
            << ActionType::shardCollection
            << ActionType::shardingState
            << ActionType::split
            << ActionType::splitChunk;

        clusterAdminRoleDatabaseActions
            << ActionType::dropDatabase
            << ActionType::killCursors
            << ActionType::repairDatabase;

        // Database-owner role database actions.
        dbOwnerRoleActions += readWriteRoleActions;
        dbOwnerRoleActions += dbAdminRoleActions;
        dbOwnerRoleActions += userAdminRoleActions;
        dbOwnerRoleActions
            << ActionType::clone
            << ActionType::copyDBTarget
            << ActionType::dropDatabase
            << ActionType::repairDatabase;

        return Status::OK();
    }

    void addReadOnlyDbPrivileges(PrivilegeVector* privileges, const StringData& dbName) {
        Privilege::addPrivilegeToPrivilegeVector(
                privileges, Privilege(ResourcePattern::forDatabaseName(dbName), readRoleActions));
        Privilege::addPrivilegeToPrivilegeVector(
                privileges,
                Privilege(ResourcePattern::forExactNamespace(
                                  NamespaceString(dbName, "system.indexes")),
                          readRoleActions));
        Privilege::addPrivilegeToPrivilegeVector(
                privileges,
                Privilege(ResourcePattern::forExactNamespace(NamespaceString(dbName, "system.js")),
                          readRoleActions));
        Privilege::addPrivilegeToPrivilegeVector(
                privileges,
                Privilege(ResourcePattern::forExactNamespace(
                                  NamespaceString(dbName, "system.namespaces")),
                          readRoleActions));
    }

    void addReadWriteDbPrivileges(PrivilegeVector* privileges, const StringData& dbName) {
        addReadOnlyDbPrivileges(privileges, dbName);
        Privilege::addPrivilegeToPrivilegeVector(
                privileges,
                Privilege(ResourcePattern::forDatabaseName(dbName), readWriteRoleActions));
        Privilege::addPrivilegeToPrivilegeVector(
                privileges,
                Privilege(ResourcePattern::forExactNamespace(NamespaceString(dbName, "system.js")),
                          readWriteRoleActions));
    }

    void addUserAdminDbPrivileges(PrivilegeVector* privileges, const StringData& dbName) {
        privileges->push_back(
                Privilege(ResourcePattern::forDatabaseName(dbName), userAdminRoleActions));
    }

    void addDbAdminDbPrivileges(PrivilegeVector* privileges, const StringData& dbName) {
        Privilege::addPrivilegeToPrivilegeVector(
                privileges,
                Privilege(ResourcePattern::forDatabaseName(dbName), dbAdminRoleActions));
        Privilege::addPrivilegeToPrivilegeVector(
                privileges,
                Privilege(ResourcePattern::forExactNamespace(
                                  NamespaceString(dbName, "system.indexes")),
                          readRoleActions));
        Privilege::addPrivilegeToPrivilegeVector(
                privileges,
                Privilege(ResourcePattern::forExactNamespace(
                                  NamespaceString(dbName, "system.namespaces")),
                          readRoleActions));
        ActionSet profileActions = readRoleActions;
        profileActions.addAction(ActionType::dropCollection);
        Privilege::addPrivilegeToPrivilegeVector(
                privileges,
                Privilege(ResourcePattern::forExactNamespace(
                                  NamespaceString(dbName, "system.profile")),
                          profileActions));
    }

    void addDbOwnerPrivileges(PrivilegeVector* privileges, const StringData& dbName) {

        addReadWriteDbPrivileges(privileges, dbName);
        addDbAdminDbPrivileges(privileges, dbName);
        addUserAdminDbPrivileges(privileges, dbName);
        Privilege::addPrivilegeToPrivilegeVector(
                privileges,
                Privilege(ResourcePattern::forDatabaseName(dbName), dbOwnerRoleActions));
    }


    void addReadOnlyAnyDbPrivileges(PrivilegeVector* privileges) {
        Privilege::addPrivilegeToPrivilegeVector(
                privileges,
                Privilege(ResourcePattern::forAnyNormalResource(), readRoleActions));
        Privilege::addPrivilegeToPrivilegeVector(
                privileges,
                Privilege(ResourcePattern::forClusterResource(), ActionType::listDatabases));
        Privilege::addPrivilegeToPrivilegeVector(
                privileges,
                Privilege(ResourcePattern::forCollectionName("system.indexes"),
                          readRoleActions));
        Privilege::addPrivilegeToPrivilegeVector(
                privileges,
                Privilege(ResourcePattern::forCollectionName("system.js"),
                          readRoleActions));
        Privilege::addPrivilegeToPrivilegeVector(
                privileges,
                Privilege(ResourcePattern::forCollectionName("system.namespaces"),
                          readRoleActions));
    }

    void addReadWriteAnyDbPrivileges(PrivilegeVector* privileges) {
        addReadOnlyAnyDbPrivileges(privileges);
        Privilege::addPrivilegeToPrivilegeVector(
                privileges,
                Privilege(ResourcePattern::forAnyNormalResource(), readWriteRoleActions));
        Privilege::addPrivilegeToPrivilegeVector(
                privileges,
                Privilege(ResourcePattern::forCollectionName("system.js"), readWriteRoleActions));
    }

    void addUserAdminAnyDbPrivileges(PrivilegeVector* privileges) {
        Privilege::addPrivilegeToPrivilegeVector(
                privileges,
                Privilege(ResourcePattern::forAnyNormalResource(), userAdminRoleActions));
        Privilege::addPrivilegeToPrivilegeVector(
                privileges,
                Privilege(ResourcePattern::forCollectionName("system.roles"),
                          readRoleActions));
        Privilege::addPrivilegeToPrivilegeVector(
                privileges,
                Privilege(ResourcePattern::forCollectionName("system.users"),
                          readRoleActions));
        Privilege::addPrivilegeToPrivilegeVector(
                privileges,
                Privilege(ResourcePattern::forExactNamespace(
                                  NamespaceString("admin.system.version")),
                          readRoleActions));
    }

    void addDbAdminAnyDbPrivileges(PrivilegeVector* privileges) {
        Privilege::addPrivilegeToPrivilegeVector(
                privileges,
                Privilege(ResourcePattern::forAnyNormalResource(), dbAdminRoleActions));
        Privilege::addPrivilegeToPrivilegeVector(
                privileges,
                Privilege(ResourcePattern::forCollectionName("system.indexes"),
                          readRoleActions));
        Privilege::addPrivilegeToPrivilegeVector(
                privileges,
                Privilege(ResourcePattern::forCollectionName("system.namespaces"),
                          readRoleActions));
        ActionSet profileActions = readRoleActions;
        profileActions.addAction(ActionType::dropCollection);
        Privilege::addPrivilegeToPrivilegeVector(
                privileges,
                Privilege(ResourcePattern::forCollectionName("system.profile"),
                          profileActions));
    }

    void addClusterAdminPrivileges(PrivilegeVector* privileges) {
        Privilege::addPrivilegeToPrivilegeVector(
                privileges,
                Privilege(ResourcePattern::forClusterResource(), clusterAdminRoleClusterActions));
        Privilege::addPrivilegeToPrivilegeVector(
                privileges,
                Privilege(ResourcePattern::forAnyNormalResource(),
                          clusterAdminRoleDatabaseActions));
        Privilege::addPrivilegeToPrivilegeVector(
                privileges,
                Privilege(ResourcePattern::forExactNamespace(NamespaceString("local",
                                                                             "system.replset")),
                          readRoleActions));
    }

    void addRootRolePrivileges(PrivilegeVector* privileges) {
        addClusterAdminPrivileges(privileges);
        addUserAdminAnyDbPrivileges(privileges);
        addDbAdminAnyDbPrivileges(privileges);
        addReadWriteAnyDbPrivileges(privileges);
        Privilege::addPrivilegeToPrivilegeVector(
                privileges,
                Privilege(ResourcePattern::forAnyNormalResource(), dbOwnerRoleActions));
    }

    void addInternalRolePrivileges(PrivilegeVector* privileges) {
        RoleGraph::generateUniversalPrivileges(privileges);
    }

}  // namespace

    bool RoleGraph::addPrivilegesForBuiltinRole(const RoleName& roleName,
                                                PrivilegeVector* result) {
        const bool isAdminDB = (roleName.getDB() == ADMIN_DBNAME);

        if (roleName.getRole() == BUILTIN_ROLE_READ) {
            addReadOnlyDbPrivileges(result, roleName.getDB());
        }
        else if (roleName.getRole() == BUILTIN_ROLE_READ_WRITE) {
            addReadWriteDbPrivileges(result, roleName.getDB());
        }
        else if (roleName.getRole() == BUILTIN_ROLE_USER_ADMIN) {
            addUserAdminDbPrivileges(result, roleName.getDB());
        }
        else if (roleName.getRole() == BUILTIN_ROLE_DB_ADMIN) {
            addDbAdminDbPrivileges(result, roleName.getDB());
        }
        else if (roleName.getRole() == BUILTIN_ROLE_DB_OWNER) {
            addDbOwnerPrivileges(result, roleName.getDB());
        }
        else if (isAdminDB && roleName.getRole() == BUILTIN_ROLE_READ_ANY_DB) {
            addReadOnlyAnyDbPrivileges(result);
        }
        else if (isAdminDB && roleName.getRole() == BUILTIN_ROLE_READ_WRITE_ANY_DB) {
            addReadWriteAnyDbPrivileges(result);
        }
        else if (isAdminDB && roleName.getRole() == BUILTIN_ROLE_USER_ADMIN_ANY_DB) {
            addUserAdminAnyDbPrivileges(result);
        }
        else if (isAdminDB && roleName.getRole() == BUILTIN_ROLE_DB_ADMIN_ANY_DB) {
            addDbAdminAnyDbPrivileges(result);
        }
        else if (isAdminDB && roleName.getRole() == BUILTIN_ROLE_CLUSTER_ADMIN) {
            addClusterAdminPrivileges(result);
        }
        else if (isAdminDB && roleName.getRole() == BUILTIN_ROLE_ROOT) {
            addRootRolePrivileges(result);
        }
        else if (isAdminDB && roleName.getRole() == BUILTIN_ROLE_INTERNAL) {
            addInternalRolePrivileges(result);
        }
        else {
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
        bool isAdminDB = role.getDB() == ADMIN_DBNAME;

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
        else if (role.getRole() == BUILTIN_ROLE_DB_OWNER) {
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
        else if (isAdminDB && role.getRole() == BUILTIN_ROLE_ROOT) {
            return true;
        }
        else if (isAdminDB && role.getRole() == BUILTIN_ROLE_INTERNAL) {
            return true;
        }
        return false;
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

} // namespace mongo
