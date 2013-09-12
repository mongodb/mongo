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
#include "mongo/db/auth/role_graph.h"
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

    bool AuthorizationManager::_doesSupportOldStylePrivileges = true;
    bool AuthorizationManager::_authEnabled = false;


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
                                                         const BSONObj& userObj,
                                                         const BSONObj& writeConcern) const {
        return _externalState->insertPrivilegeDocument(dbname, userObj, writeConcern);
    }

    Status AuthorizationManager::updatePrivilegeDocument(const UserName& user,
                                                         const BSONObj& updateObj,
                                                         const BSONObj& writeConcern) const {
        return _externalState->updatePrivilegeDocument(user, updateObj, writeConcern);
    }

    Status AuthorizationManager::removePrivilegeDocuments(const BSONObj& query,
                                                          const BSONObj& writeConcern,
                                                          int* numRemoved) const {
        return _externalState->removePrivilegeDocuments(query, writeConcern, numRemoved);
    }

    ActionSet AuthorizationManager::getAllUserActions() {
        return RoleGraph::getAllUserActions();
    }

    bool AuthorizationManager::roleExists(const RoleName& role) {
        boost::lock_guard<boost::mutex> lk(_lock);
        return _roleGraph.roleExists(role);
    }

    void AuthorizationManager::_initializeUserPrivilegesFromRoles_inlock(User* user) {
        const User::RoleDataMap& roles = user->getRoles();
        for (User::RoleDataMap::const_iterator it = roles.begin(); it != roles.end(); ++it) {
            const User::RoleData& role= it->second;
            if (role.hasRole)
                user->addPrivileges(_roleGraph.getAllPrivileges(role.name));
        }
    }

    Status AuthorizationManager::_initializeUserFromPrivilegeDocument(
            User* user, const BSONObj& privDoc) {
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
        _initializeUserPrivilegesFromRoles_inlock(user);
        return Status::OK();
    }

    Status AuthorizationManager::acquireUser(const UserName& userName, User** acquiredUser) {
        boost::lock_guard<boost::mutex> lk(_lock);
        return _acquireUser_inlock(userName, acquiredUser);
    }

    Status AuthorizationManager::_acquireUser_inlock(const UserName& userName,
                                                     User** acquiredUser) {
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
                    _initializeUserPrivilegesFromRoles_inlock(user);
                }
            }
        } catch (const DBException& e) {
            return e.toStatus();
        } catch (const std::exception& e) {
            return Status(ErrorCodes::InternalError, e.what());
        }

        return Status::OK();
    }


    AuthorizationManager::Guard::Guard(AuthorizationManager* authzManager)
        : _authzManager(authzManager),
          _lockedForUpdate(false),
          _authzManagerLock(authzManager->_lock) {}

    AuthorizationManager::Guard::~Guard() {
        if (_lockedForUpdate) {
            if (!_authzManagerLock.owns_lock()) {
                _authzManagerLock.lock();
            }
            releaseAuthzUpdateLock();
        }
    }

    bool AuthorizationManager::Guard::tryAcquireAuthzUpdateLock(const StringData& why) {
        fassert(17111, !_lockedForUpdate);
        fassert(17126, _authzManagerLock.owns_lock());
        _lockedForUpdate = _authzManager->_externalState->tryAcquireAuthzUpdateLock(why);
        return _lockedForUpdate;
    }

    void AuthorizationManager::Guard::releaseAuthzUpdateLock() {
        fassert(17112, _lockedForUpdate);
        fassert(17127, _authzManagerLock.owns_lock());
        _authzManager->_externalState->releaseAuthzUpdateLock();
        _lockedForUpdate = false;
    }

    void AuthorizationManager::Guard::acquireAuthorizationManagerLock() {
        fassert(17129, !_authzManagerLock.owns_lock());
        _authzManagerLock.lock();
    }

    void AuthorizationManager::Guard::releaseAuthorizationManagerLock() {
        fassert(17128, _authzManagerLock.owns_lock());
        _authzManagerLock.unlock();
    }

    Status AuthorizationManager::Guard::acquireUser(const UserName& userName, User** acquiredUser) {
        fassert(17130, _authzManagerLock.owns_lock());
        return _authzManager->_acquireUser_inlock(userName, acquiredUser);
    }

    namespace {
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
            const User::RoleDataMap& roles = user.getRoles();
            for (User::RoleDataMap::const_iterator it = roles.begin(); it != roles.end(); ++it) {
                const User::RoleData& role = it->second;
                BSONObjBuilder roleBuilder(rolesArray.subobjStart());
                roleBuilder.append("name", role.name.getRole());
                roleBuilder.append("source", role.name.getDB());
                roleBuilder.appendBool("canDelegate", role.canDelegate);
                roleBuilder.appendBool("hasRole", role.hasRole);
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
        AuthorizationManager::Guard lkUpgrade(this);
        if (!lkUpgrade.tryAcquireAuthzUpdateLock("Upgrade authorization data")) {
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

        BSONObj writeConcern;
        // Upgrade from v1 to v2.
        status = _externalState->copyCollection(usersCollectionName,
                                                backupUsersCollectionName,
                                                writeConcern);
        if (!status.isOK())
            return status;
        status = _externalState->dropCollection(newusersCollectionName, writeConcern);
        if (!status.isOK())
            return status;
        status = _externalState->createIndex(
                newusersCollectionName,
                BSON(USER_NAME_FIELD_NAME << 1 << USER_SOURCE_FIELD_NAME << 1),
                true, // unique
                writeConcern
                );
        if (!status.isOK())
            return status;
        for (unordered_map<UserName, User*>::const_iterator iter = _userCache.begin();
             iter != _userCache.end(); ++iter) {

            // Do not create a user document for the internal user.
            if (iter->second == internalSecurity.user)
                continue;

            status = _externalState->insert(
                    newusersCollectionName, userAsV2PrivilegeDocument(*iter->second),
                    writeConcern);
            if (!status.isOK())
                return status;
        }
        status = _externalState->renameCollection(newusersCollectionName,
                                                  usersCollectionName,
                                                  writeConcern);
        if (!status.isOK())
            return status;
        status = _externalState->updateOne(
                versionCollectionName,
                versionDocumentQuery,
                BSON("$set" << BSON("currentVersion" << 2)),
                true,
                writeConcern);
        if (!status.isOK())
            return status;
        _version = 2;
        return status;
    }

} // namespace mongo
