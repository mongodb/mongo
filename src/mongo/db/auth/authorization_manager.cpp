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

#include "mongo/platform/basic.h"

#include "mongo/db/auth/authorization_manager.h"

#include <boost/thread/mutex.hpp>
#include <memory>
#include <string>
#include <vector>

#include "mongo/base/init.h"
#include "mongo/base/status.h"
#include "mongo/db/auth/action_set.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/auth/privilege_document_parser.h"
#include "mongo/db/auth/user.h"
#include "mongo/db/auth/user_name.h"
#include "mongo/db/auth/user_name_hash.h"
#include "mongo/db/jsobj.h"
#include "mongo/platform/compiler.h"
#include "mongo/platform/unordered_map.h"
#include "mongo/util/map_util.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

    AuthInfo internalSecurity;

    MONGO_INITIALIZER(SetupInternalSecurityUser)(InitializerContext* context) {
        User* user = new User(UserName("__system", "local"));

        user->incrementRefCount(); // Pin this user so the ref count never drops below 1.
        ActionSet allActions;
        allActions.addAllActions();
        user->addPrivilege(Privilege(AuthorizationManager::WILDCARD_RESOURCE_NAME, allActions));

        internalSecurity.user = user;

        return Status::OK();
    }

    const std::string AuthorizationManager::SERVER_RESOURCE_NAME = "$SERVER";
    const std::string AuthorizationManager::CLUSTER_RESOURCE_NAME = "$CLUSTER";
    const std::string AuthorizationManager::WILDCARD_RESOURCE_NAME = "*";
    const std::string AuthorizationManager::USER_NAME_FIELD_NAME = "name";
    const std::string AuthorizationManager::USER_SOURCE_FIELD_NAME = "source";
    const std::string AuthorizationManager::PASSWORD_FIELD_NAME = "pwd";
    const std::string AuthorizationManager::V1_USER_NAME_FIELD_NAME = "user";
    const std::string AuthorizationManager::V1_USER_SOURCE_FIELD_NAME = "userSource";

    const std::string AuthorizationManager::SYSTEM_ROLE_V0_READ = "oldRead";
    const std::string AuthorizationManager::SYSTEM_ROLE_V0_READ_WRITE= "oldReadWrite";
    const std::string AuthorizationManager::SYSTEM_ROLE_V0_ADMIN_READ = "oldAdminRead";
    const std::string AuthorizationManager::SYSTEM_ROLE_V0_ADMIN_READ_WRITE= "oldAdminReadWrite";

    bool AuthorizationManager::_doesSupportOldStylePrivileges = true;
    bool AuthorizationManager::_authEnabled = false;

namespace {
    const std::string ADMIN_DBNAME = "admin";

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


    AuthorizationManager::AuthorizationManager(AuthzManagerExternalState* externalState) :
            _externalState(externalState) {
        setAuthorizationVersion(2);
    }

    AuthorizationManager::~AuthorizationManager() {
        for (unordered_map<UserName, User*>::iterator it = _userCache.begin();
                it != _userCache.end(); ++it) {
            if (it->second != internalSecurity.user) {
                // The internal user should never be deleted.
                delete it->second ;
            }
        }
    }

    AuthzManagerExternalState* AuthorizationManager::getExternalState() const {
        return _externalState.get();
    }

    Status AuthorizationManager::setAuthorizationVersion(int version) {
        boost::lock_guard<boost::mutex> lk(_lock);

        if (version == 1) {
            _parser.reset(new V1PrivilegeDocumentParser());
        } else if (version == 2) {
            _parser.reset(new V2PrivilegeDocumentParser());
        } else {
            return Status(ErrorCodes::UnsupportedFormat,
                          mongoutils::str::stream() <<
                                  "Unrecognized authorization format version: " <<
                                  version);
        }

        _version = version;
        return Status::OK();
    }

    int AuthorizationManager::getAuthorizationVersion() {
        boost::lock_guard<boost::mutex> lk(_lock);
        return _getVersion_inlock();
    }

    void AuthorizationManager::setSupportOldStylePrivilegeDocuments(bool enabled) {
        _doesSupportOldStylePrivileges = enabled;
    }

    bool AuthorizationManager::getSupportOldStylePrivilegeDocuments() {
        return _doesSupportOldStylePrivileges;
    }

    void AuthorizationManager::setAuthEnabled(bool enabled) {
        _authEnabled = enabled;
    }

    bool AuthorizationManager::isAuthEnabled() {
        return _authEnabled;
    }

    bool AuthorizationManager::hasAnyPrivilegeDocuments() const {
        return _externalState->hasAnyPrivilegeDocuments();
    }

    Status AuthorizationManager::insertPrivilegeDocument(const std::string& dbname,
                                                         const BSONObj& userObj) const {
        return _externalState->insertPrivilegeDocument(dbname, userObj);
    }

    Status AuthorizationManager::updatePrivilegeDocument(const UserName& user,
                                                         const BSONObj& updateObj) const {
        return _externalState->updatePrivilegeDocument(user, updateObj);
    }

    Status AuthorizationManager::removePrivilegeDocuments(const BSONObj& query,
                                                          int* numRemoved) const {
        return _externalState->removePrivilegeDocuments(query, numRemoved);
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

    bool AuthorizationManager::roleExists(const RoleName& role) const {
        bool isAdminDB = role.getDB() == "admin";

        if (role.getRole() == SYSTEM_ROLE_READ) {
            return true;
        }
        else if (role.getRole() == SYSTEM_ROLE_READ_WRITE) {
            return true;
        }
        else if (role.getRole() == SYSTEM_ROLE_USER_ADMIN) {
            return true;
        }
        else if (role.getRole() == SYSTEM_ROLE_DB_ADMIN) {
            return true;
        }
        else if (role.getRole() == AuthorizationManager::SYSTEM_ROLE_V0_READ) {
            return true;
        }
        else if (role.getRole() == AuthorizationManager::SYSTEM_ROLE_V0_READ_WRITE) {
            return true;
        }
        else if (isAdminDB && role.getRole() == SYSTEM_ROLE_READ_ANY_DB) {
            return true;
        }
        else if (isAdminDB && role.getRole() == SYSTEM_ROLE_READ_WRITE_ANY_DB) {
            return true;
        }
        else if (isAdminDB && role.getRole() == SYSTEM_ROLE_USER_ADMIN_ANY_DB) {
            return true;
        }
        else if (isAdminDB && role.getRole() == SYSTEM_ROLE_DB_ADMIN_ANY_DB) {
            return true;
        }
        else if (isAdminDB && role.getRole() == SYSTEM_ROLE_CLUSTER_ADMIN) {
            return true;
        }
        else if (isAdminDB && role.getRole() == AuthorizationManager::SYSTEM_ROLE_V0_ADMIN_READ) {
            return true;
        }
        else if (isAdminDB &&
                role.getRole() == AuthorizationManager::SYSTEM_ROLE_V0_ADMIN_READ_WRITE) {
            return true;
        } else {
            // TODO(spencer): Check the role graph to see if this is a valid user-defined-role
            return false;
        }

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
        else if (role == AuthorizationManager::SYSTEM_ROLE_V0_READ) {
            outPrivileges->push_back(Privilege(dbname, compatibilityReadOnlyActions));
        }
        else if (role == AuthorizationManager::SYSTEM_ROLE_V0_READ_WRITE) {
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
        else if (isAdminDB && role == AuthorizationManager::SYSTEM_ROLE_V0_ADMIN_READ) {
            outPrivileges->push_back(
                    Privilege(AuthorizationManager::WILDCARD_RESOURCE_NAME,
                              compatibilityReadOnlyAdminActions));
        }
        else if (isAdminDB && role == AuthorizationManager::SYSTEM_ROLE_V0_ADMIN_READ_WRITE) {
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
    void _initializeUserPrivilegesFromRoles(User* user) {
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

    Status AuthorizationManager::_initializeUserFromPrivilegeDocument(
            User* user, const BSONObj& privDoc) const {
        std::string userName = _parser->extractUserNameFromPrivilegeDocument(privDoc);
        if (userName != user->getName().getUser()) {
            return Status(ErrorCodes::BadValue,
                          mongoutils::str::stream() << "User name from privilege document \""
                                  << userName
                                  << "\" doesn't match name of provided User \""
                                  << user->getName().getUser()
                                  << "\"",
                          0);
        }

        Status status = _parser->initializeUserCredentialsFromPrivilegeDocument(user, privDoc);
        if (!status.isOK()) {
            return status;
        }
        status = _parser->initializeUserRolesFromPrivilegeDocument(user,
                                                                 privDoc,
                                                                 user->getName().getDB());
        if (!status.isOK()) {
            return status;
        }
        _initializeUserPrivilegesFromRoles(user);
        return Status::OK();
    }

    Status AuthorizationManager::acquireUser(const UserName& userName, User** acquiredUser) {
        boost::lock_guard<boost::mutex> lk(_lock);
        unordered_map<UserName, User*>::iterator it = _userCache.find(userName);
        if (it != _userCache.end()) {
            fassert(16914, it->second);
            fassert(17003, it->second->isValid());
            fassert(17008, it->second->getRefCount() > 0);
            it->second->incrementRefCount();
            *acquiredUser = it->second;
            return Status::OK();
        }

        // Put the new user into an auto_ptr temporarily in case there's an error while
        // initializing the user.
        auto_ptr<User> userHolder(new User(userName));
        User* user = userHolder.get();

        BSONObj userObj;
        Status status = _externalState->getPrivilegeDocument(userName,
                                                             _getVersion_inlock(),
                                                             &userObj);
        if (!status.isOK()) {
            return status;
        }

        status = _initializeUserFromPrivilegeDocument(user, userObj);
        if (!status.isOK()) {
            return status;
        }

        user->incrementRefCount();
        _userCache.insert(make_pair(userName, userHolder.release()));
        *acquiredUser = user;
        return Status::OK();
    }

    void AuthorizationManager::releaseUser(User* user) {
        if (user == internalSecurity.user) {
            return;
        }

        boost::lock_guard<boost::mutex> lk(_lock);
        user->decrementRefCount();
        if (user->getRefCount() == 0) {
            // If it's been invalidated then it's not in the _userCache anymore.
            if (user->isValid()) {
                MONGO_COMPILER_VARIABLE_UNUSED bool erased = _userCache.erase(user->getName());
                dassert(erased);
            }
            delete user;
        }
    }

    void AuthorizationManager::invalidateUser(User* user) {
        boost::lock_guard<boost::mutex> lk(_lock);
        if (!user->isValid()) {
            return;
        }

        unordered_map<UserName, User*>::iterator it = _userCache.find(user->getName());
        massert(17052,
                mongoutils::str::stream() <<
                        "Invalidating cache for user " << user->getName().getFullName() <<
                        " failed as it is not present in the user cache",
                it != _userCache.end() && it->second == user);
        _userCache.erase(it);
        user->invalidate();
    }

    void AuthorizationManager::invalidateUserByName(const UserName& userName) {
        boost::lock_guard<boost::mutex> lk(_lock);

        unordered_map<UserName, User*>::iterator it = _userCache.find(userName);
        if (it == _userCache.end()) {
            return;
        }

        User* user = it->second;
        _userCache.erase(it);
        user->invalidate();
    }

    void AuthorizationManager::invalidateUsersFromDB(const std::string& dbname) {
        boost::lock_guard<boost::mutex> lk(_lock);

        unordered_map<UserName, User*>::iterator it = _userCache.begin();
        while (it != _userCache.end()) {
            User* user = it->second;
            if (user->getName().getDB() == dbname) {
                _userCache.erase(it++);
                user->invalidate();
            } else {
                ++it;
            }
        }
    }


    void AuthorizationManager::addInternalUser(User* user) {
        boost::lock_guard<boost::mutex> lk(_lock);
        _userCache.insert(make_pair(user->getName(), user));
    }

    void AuthorizationManager::invalidateUserCache() {
        boost::lock_guard<boost::mutex> lk(_lock);
        _invalidateUserCache_inlock();
    }

    void AuthorizationManager::_invalidateUserCache_inlock() {
        for (unordered_map<UserName, User*>::iterator it = _userCache.begin();
                it != _userCache.end(); ++it) {
            if (it->second->getName() == internalSecurity.user->getName()) {
                // Don't invalidate the internal user
                continue;
            }
            it->second->invalidate();
            // Need to decrement ref count and manually clean up User object to prevent memory leaks
            // since we're pinning all User objects by incrementing their ref count when we
            // initially populate the cache.
            // TODO(spencer): remove this once we're not pinning User objects.
            it->second->decrementRefCount();
            if (it->second->getRefCount() == 0)
                delete it->second;
        }
        _userCache.clear();
        // Make sure the internal user stays in the cache.
        _userCache.insert(make_pair(internalSecurity.user->getName(), internalSecurity.user));
    }

    Status AuthorizationManager::initialize() {
        if (getAuthorizationVersion() < 2) {
            // If we are not yet upgraded to the V2 authorization format, build up a read-only
            // view of the V1 style authorization data.
            return _initializeAllV1UserData();
        }
        return Status::OK();
    }

    Status AuthorizationManager::_initializeAllV1UserData() {
        boost::lock_guard<boost::mutex> lk(_lock);
        _invalidateUserCache_inlock();
        V1PrivilegeDocumentParser parser;

        try {
            std::vector<std::string> dbNames;
            Status status = _externalState->getAllDatabaseNames(&dbNames);
            if (!status.isOK()) {
                return status;
            }

            for (std::vector<std::string>::iterator dbIt = dbNames.begin();
                    dbIt != dbNames.end(); ++dbIt) {
                std::string dbname = *dbIt;
                std::vector<BSONObj> privDocs;
                Status status = _externalState->getAllV1PrivilegeDocsForDB(dbname, &privDocs);
                if (!status.isOK()) {
                    return status;
                }

                for (std::vector<BSONObj>::iterator docIt = privDocs.begin();
                        docIt != privDocs.end(); ++docIt) {
                    const BSONObj& privDoc = *docIt;

                    std::string source;
                    if (privDoc.hasField("userSource")) {
                        source = privDoc["userSource"].String();
                    } else {
                        source = dbname;
                    }
                    UserName userName(privDoc["user"].String(), source);
                    if (userName == internalSecurity.user->getName()) {
                        // Don't let clients override the internal user by creating a user with the
                        // same name.
                        continue;
                    }

                    User* user = mapFindWithDefault(_userCache, userName, static_cast<User*>(NULL));
                    if (!user) {
                        user = new User(userName);
                        // Make sure the user always has a refCount of at least 1 so it's
                        // effectively "pinned" and will never be removed from the _userCache
                        // unless the whole cache is invalidated.
                        user->incrementRefCount();
                        _userCache.insert(make_pair(userName, user));
                    }

                    if (source == dbname || source == "$external") {
                        status = parser.initializeUserCredentialsFromPrivilegeDocument(user,
                                                                                       privDoc);
                        if (!status.isOK()) {
                            return status;
                        }
                    }
                    status = parser.initializeUserRolesFromPrivilegeDocument(user, privDoc, dbname);
                    if (!status.isOK()) {
                        return status;
                    }
                    _initializeUserPrivilegesFromRoles(user);
                }
            }
        } catch (const DBException& e) {
            return e.toStatus();
        } catch (const std::exception& e) {
            return Status(ErrorCodes::InternalError, e.what());
        }

        return Status::OK();
    }

    namespace {
        class AuthzUpgradeLockGuard {
            MONGO_DISALLOW_COPYING(AuthzUpgradeLockGuard);
        public:
            explicit AuthzUpgradeLockGuard(AuthzManagerExternalState* externalState)
                : _externalState(externalState), _locked(false) {
            }

            ~AuthzUpgradeLockGuard() {
                if (_locked)
                    unlock();
            }

            bool tryLock() {
                fassert(17111, !_locked);
                _locked = _externalState->tryLockUpgradeProcess();
                return _locked;
            }

            void unlock() {
                fassert(17112, _locked);
                _externalState->unlockUpgradeProcess();
                _locked = false;
            }
        private:
            AuthzManagerExternalState* _externalState;
            bool _locked;
        };

        BSONObj userAsV2PrivilegeDocument(const User& user) {
            BSONObjBuilder builder;

            const UserName& name = user.getName();
            builder.append(AuthorizationManager::USER_NAME_FIELD_NAME, name.getUser());
            builder.append(AuthorizationManager::USER_SOURCE_FIELD_NAME, name.getDB());

            const User::CredentialData& credentials = user.getCredentials();
            if (!credentials.isExternal) {
                BSONObjBuilder credentialsBuilder(builder.subobjStart("credentials"));
                credentialsBuilder.append("MONGODB-CR", credentials.password);
                credentialsBuilder.doneFast();
            }

            BSONArrayBuilder rolesArray(builder.subarrayStart("roles"));
            for (RoleNameIterator roles = user.getRoles(); roles.more(); roles.next()) {
                const RoleName& roleName = roles.get();
                BSONObjBuilder roleBuilder(rolesArray.subobjStart());
                roleBuilder.append("name", roleName.getRole());
                roleBuilder.append("source", roleName.getDB());
                roleBuilder.appendBool("canDelegate", false);
                roleBuilder.appendBool("hasRole", true);
                roleBuilder.doneFast();
            }
            rolesArray.doneFast();
            return builder.obj();
        }

        const NamespaceString newusersCollectionName("admin._newusers");
        const NamespaceString usersCollectionName("admin.system.users");
        const NamespaceString backupUsersCollectionName("admin.backup.users");
        const NamespaceString versionCollectionName("admin.system.version");
        const BSONObj versionDocumentQuery = BSON("_id" << 1);

        /**
         * Fetches the admin.system.version document and extracts the currentVersion field's
         * value, supposing it is an integer, and writes it to outVersion.
         */
        Status readAuthzVersion(AuthzManagerExternalState* externalState, int* outVersion) {
            BSONObj versionDoc;
            Status status = externalState->findOne(
                    versionCollectionName, versionDocumentQuery, &versionDoc);
            if (!status.isOK() && ErrorCodes::NoMatchingDocument != status) {
                return status;
            }
            BSONElement currentVersionElement = versionDoc["currentVersion"];
            if (!versionDoc.isEmpty() && !currentVersionElement.isNumber()) {
                return Status(ErrorCodes::TypeMismatch,
                              "Field 'currentVersion' in admin.system.version must be a number.");
            }
            *outVersion = currentVersionElement.numberInt();
            return Status::OK();
        }
    }  // namespace

    Status AuthorizationManager::upgradeAuthCollections() {
        boost::lock_guard<boost::mutex> lkLocal(_lock);
        AuthzUpgradeLockGuard lkUpgrade(_externalState.get());
        if (!lkUpgrade.tryLock()) {
            return Status(ErrorCodes::LockBusy, "Could not lock auth data upgrade process lock.");
        }
        int durableVersion = 0;
        Status status = readAuthzVersion(_externalState.get(), &durableVersion);
        if (!status.isOK())
            return status;

        if (_version == 2) {
            switch (durableVersion) {
            case 0:
            case 1: {
                const char msg[] = "User data format version in memory and on disk inconsistent; "
                    "please restart this node.";
                error() << msg;
                return Status(ErrorCodes::UserDataInconsistent, msg);
            }
            case 2:
                return Status::OK();
            default:
                return Status(ErrorCodes::BadValue,
                              mongoutils::str::stream() <<
                              "Cannot upgrade admin.system.version to 2 from " <<
                              durableVersion);
            }
        }
        fassert(17113, _version == 1);
        switch (durableVersion) {
        case 0:
        case 1:
            break;
        case 2: {
            const char msg[] = "User data format version in memory and on disk inconsistent; "
                "please restart this node.";
            error() << msg;
            return Status(ErrorCodes::UserDataInconsistent, msg);
        }
        default:
                return Status(ErrorCodes::BadValue,
                              mongoutils::str::stream() <<
                              "Cannot upgrade admin.system.version from 2 to " <<
                              durableVersion);
        }

        // Upgrade from v1 to v2.
        status = _externalState->copyCollection(usersCollectionName, backupUsersCollectionName);
        if (!status.isOK())
            return status;
        status = _externalState->dropCollection(newusersCollectionName);
        if (!status.isOK())
            return status;
        status = _externalState->createIndex(
                newusersCollectionName,
                BSON(USER_NAME_FIELD_NAME << 1 << USER_SOURCE_FIELD_NAME << 1),
                true // unique
                );
        if (!status.isOK())
            return status;
        for (unordered_map<UserName, User*>::const_iterator iter = _userCache.begin();
             iter != _userCache.end(); ++iter) {

            // Do not create a user document for the internal user.
            if (iter->second == internalSecurity.user)
                continue;

            status = _externalState->insert(
                    newusersCollectionName, userAsV2PrivilegeDocument(*iter->second));
            if (!status.isOK())
                return status;
        }
        status = _externalState->renameCollection(newusersCollectionName, usersCollectionName);
        if (!status.isOK())
            return status;
        status = _externalState->updateOne(
                versionCollectionName,
                versionDocumentQuery,
                BSON("$set" << BSON("currentVersion" << 2)),
                true);
        if (!status.isOK())
            return status;
        _version = 2;
        return status;
    }

} // namespace mongo
