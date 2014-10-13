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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kAccessControl

#include "mongo/platform/basic.h"

#include "mongo/db/auth/authorization_manager.h"

#include <boost/bind.hpp>
#include <boost/thread/mutex.hpp>
#include <memory>
#include <string>
#include <vector>

#include "mongo/base/init.h"
#include "mongo/base/status.h"
#include "mongo/bson/mutable/document.h"
#include "mongo/bson/mutable/element.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/client/auth_helpers.h"
#include "mongo/crypto/mechanism_scram.h"
#include "mongo/db/auth/action_set.h"
#include "mongo/db/auth/authz_documents_update_guard.h"
#include "mongo/db/auth/authz_manager_external_state.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/auth/role_graph.h"
#include "mongo/db/auth/sasl_options.h"
#include "mongo/db/auth/user.h"
#include "mongo/db/auth/user_document_parser.h"
#include "mongo/db/auth/user_name.h"
#include "mongo/db/auth/user_name_hash.h"
#include "mongo/db/jsobj.h"
#include "mongo/platform/compiler.h"
#include "mongo/platform/unordered_map.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/log.h"
#include "mongo/util/map_util.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

    AuthInfo internalSecurity;

    MONGO_INITIALIZER_WITH_PREREQUISITES(SetupInternalSecurityUser, MONGO_NO_PREREQUISITES)(
            InitializerContext* context) {

        User* user = new User(UserName("__system", "local"));

        user->incrementRefCount(); // Pin this user so the ref count never drops below 1.
        ActionSet allActions;
        allActions.addAllActions();
        PrivilegeVector privileges;
        RoleGraph::generateUniversalPrivileges(&privileges);
        user->addPrivileges(privileges);
        internalSecurity.user = user;

        return Status::OK();
    }

    const std::string AuthorizationManager::USER_NAME_FIELD_NAME = "user";
    const std::string AuthorizationManager::USER_DB_FIELD_NAME = "db";
    const std::string AuthorizationManager::ROLE_NAME_FIELD_NAME = "role";
    const std::string AuthorizationManager::ROLE_DB_FIELD_NAME = "db";
    const std::string AuthorizationManager::PASSWORD_FIELD_NAME = "pwd";
    const std::string AuthorizationManager::V1_USER_NAME_FIELD_NAME = "user";
    const std::string AuthorizationManager::V1_USER_SOURCE_FIELD_NAME = "userSource";

    const NamespaceString AuthorizationManager::adminCommandNamespace("admin.$cmd");
    const NamespaceString AuthorizationManager::rolesCollectionNamespace("admin.system.roles");
    const NamespaceString AuthorizationManager::usersAltCollectionNamespace(
            "admin.system.new_users");
    const NamespaceString AuthorizationManager::usersBackupCollectionNamespace(
            "admin.system.backup_users");
    const NamespaceString AuthorizationManager::usersCollectionNamespace("admin.system.users");
    const NamespaceString AuthorizationManager::versionCollectionNamespace("admin.system.version");
    const NamespaceString AuthorizationManager::defaultTempUsersCollectionNamespace(
            "admin.tempusers");
    const NamespaceString AuthorizationManager::defaultTempRolesCollectionNamespace(
            "admin.temproles");

    const BSONObj AuthorizationManager::versionDocumentQuery = BSON("_id" << "authSchema");

    const std::string AuthorizationManager::schemaVersionFieldName = "currentVersion";

#ifndef _MSC_EXTENSIONS
    const int AuthorizationManager::schemaVersion24;
    const int AuthorizationManager::schemaVersion26Upgrade;
    const int AuthorizationManager::schemaVersion26Final;
    const int AuthorizationManager::schemaVersion28SCRAM;
#endif

    /**
     * Guard object for synchronizing accesses to data cached in AuthorizationManager instances.
     * This guard allows one thread to access the cache at a time, and provides an exception-safe
     * mechanism for a thread to release the cache mutex while performing network or disk operations
     * while allowing other readers to proceed.
     *
     * There are two ways to use this guard.  One may simply instantiate the guard like a
     * std::lock_guard, and perform reads or writes of the cache.
     *
     * Alternatively, one may instantiate the guard, examine the cache, and then enter into an
     * update mode by first wait()ing until otherUpdateInFetchPhase() is false, and then
     * calling beginFetchPhase().  At this point, other threads may acquire the guard in the simple
     * manner and do reads, but other threads may not enter into a fetch phase.  During the fetch
     * phase, the thread should perform required network or disk activity to determine what update
     * it will make to the cache.  Then, it should call endFetchPhase(), to reacquire the user cache
     * mutex.  At that point, the thread can make its modifications to the cache and let the guard
     * go out of scope.
     *
     * All updates by guards using a fetch-phase are totally ordered with respect to one another,
     * and all guards using no fetch phase are totally ordered with respect to one another, but
     * there is not a total ordering among all guard objects.
     *
     * The cached data has an associated counter, called the cache generation.  If the cache
     * generation changes while a guard is in fetch phase, the fetched data should not be stored
     * into the cache, because some invalidation event occurred during the fetch phase.
     *
     * NOTE: It is not safe to enter fetch phase while holding a database lock.  Fetch phase
     * operations are allowed to acquire database locks themselves, so entering fetch while holding
     * a database lock may lead to deadlock.
     */
    class AuthorizationManager::CacheGuard {
        MONGO_DISALLOW_COPYING(CacheGuard);
    public:
        enum FetchSynchronization {
            fetchSynchronizationAutomatic,
            fetchSynchronizationManual
        };

        /**
         * Constructs a cache guard, locking the mutex that synchronizes user cache accesses.
         */
        CacheGuard(AuthorizationManager* authzManager,
                   const FetchSynchronization sync = fetchSynchronizationAutomatic) :
            _isThisGuardInFetchPhase(false),
            _authzManager(authzManager),
            _lock(authzManager->_cacheMutex) {

            if (fetchSynchronizationAutomatic == sync) {
                synchronizeWithFetchPhase();
            }
        }

        /**
         * Releases the mutex that synchronizes user cache access, if held, and notifies
         * any threads waiting for their own opportunity to update the user cache.
         */
        ~CacheGuard() {
            if (!_lock.owns_lock()) {
                _lock.lock();
            }
            if (_isThisGuardInFetchPhase) {
                fassert(17190, _authzManager->_isFetchPhaseBusy);
                _authzManager->_isFetchPhaseBusy = false;
                _authzManager->_fetchPhaseIsReady.notify_all();
            }
        }

        /**
         * Returns true of the authzManager reports that it is in fetch phase.
         */
        bool otherUpdateInFetchPhase() { return _authzManager->_isFetchPhaseBusy; }

        /**
         * Waits on the _authzManager->_fetchPhaseIsReady condition.
         */
        void wait() {
            fassert(17222, !_isThisGuardInFetchPhase);
            _authzManager->_fetchPhaseIsReady.wait(_lock);
        }

        /**
         * Enters fetch phase, releasing the _authzManager->_cacheMutex after recording the current
         * cache generation.
         */
        void beginFetchPhase() {
            fassert(17191, !_authzManager->_isFetchPhaseBusy);
            _isThisGuardInFetchPhase = true;
            _authzManager->_isFetchPhaseBusy = true;
            _startGeneration = _authzManager->_cacheGeneration;
            _lock.unlock();
        }

        /**
         * Exits the fetch phase, reacquiring the _authzManager->_cacheMutex.
         */
        void endFetchPhase() {
            _lock.lock();
            // We do not clear _authzManager->_isFetchPhaseBusy or notify waiters until
            // ~CacheGuard(), for two reasons.  First, there's no value to notifying the waiters
            // before you're ready to release the mutex, because they'll just go to sleep on the
            // mutex.  Second, in order to meaningfully check the preconditions of
            // isSameCacheGeneration(), we need a state that means "fetch phase was entered and now
            // has been exited."  That state is _isThisGuardInFetchPhase == true and
            // _lock.owns_lock() == true.
        }

        /**
         * Returns true if _authzManager->_cacheGeneration remained the same while this guard was
         * in fetch phase.  Behavior is undefined if this guard never entered fetch phase.
         *
         * If this returns true, do not update the cached data with this
         */
        bool isSameCacheGeneration() const {
            fassert(17223, _isThisGuardInFetchPhase);
            fassert(17231, _lock.owns_lock());
            return _startGeneration == _authzManager->_cacheGeneration;
        }

    private:
        void synchronizeWithFetchPhase() {
            while (otherUpdateInFetchPhase())
                wait();
            fassert(17192, !_authzManager->_isFetchPhaseBusy);
            _isThisGuardInFetchPhase = true;
            _authzManager->_isFetchPhaseBusy = true;
        }

        OID _startGeneration;
        bool _isThisGuardInFetchPhase;
        AuthorizationManager* _authzManager;
        boost::unique_lock<boost::mutex> _lock;
    };

    AuthorizationManager::AuthorizationManager(AuthzManagerExternalState* externalState) :
            _authEnabled(false),
            _externalState(externalState),
            _version(schemaVersionInvalid),
            _isFetchPhaseBusy(false) {
        _updateCacheGeneration_inlock();
    }

    AuthorizationManager::~AuthorizationManager() {
        for (unordered_map<UserName, User*>::iterator it = _userCache.begin();
                it != _userCache.end(); ++it) {
            fassert(17265, it->second != internalSecurity.user);
            delete it->second ;
        }
    }

    Status AuthorizationManager::getAuthorizationVersion(OperationContext* txn, int* version) {
        CacheGuard guard(this, CacheGuard::fetchSynchronizationManual);
        int newVersion = _version;
        if (schemaVersionInvalid == newVersion) {
            while (guard.otherUpdateInFetchPhase())
                guard.wait();
            guard.beginFetchPhase();
            Status status = _externalState->getStoredAuthorizationVersion(txn, &newVersion);
            guard.endFetchPhase();
            if (!status.isOK()) {
                warning() << "Problem fetching the stored schema version of authorization data: "
                          << status;
                *version = schemaVersionInvalid;
                return status;
            }

            if (guard.isSameCacheGeneration()) {
                _version = newVersion;
            }
        }
        *version = newVersion;
        return Status::OK();
    }

    OID AuthorizationManager::getCacheGeneration() {
        CacheGuard guard(this, CacheGuard::fetchSynchronizationManual);
        return _cacheGeneration;
    }

    void AuthorizationManager::setAuthEnabled(bool enabled) {
        _authEnabled = enabled;
    }

    bool AuthorizationManager::isAuthEnabled() const {
        return _authEnabled;
    }

    bool AuthorizationManager::hasAnyPrivilegeDocuments(OperationContext* txn) const {
        return _externalState->hasAnyPrivilegeDocuments(txn);
    }

    Status AuthorizationManager::writeAuthSchemaVersionIfNeeded(OperationContext* txn,
                                                                int foundSchemaVersion) {
        Status status =  _externalState->updateOne(
                txn,
                AuthorizationManager::versionCollectionNamespace,
                AuthorizationManager::versionDocumentQuery,
                BSON("$set" << BSON(AuthorizationManager::schemaVersionFieldName <<
                                    foundSchemaVersion)),
                true,  // upsert
                BSONObj());  // write concern
        if (status == ErrorCodes::NoMatchingDocument) {    // SERVER-11492
            status = Status::OK();
        }
        return status;
    }

    Status AuthorizationManager::insertPrivilegeDocument(OperationContext* txn,
                                                         const std::string& dbname,
                                                         const BSONObj& userObj,
                                                         const BSONObj& writeConcern) const {
        return _externalState->insertPrivilegeDocument(txn, dbname, userObj, writeConcern);
    }

    Status AuthorizationManager::updatePrivilegeDocument(OperationContext* txn,
                                                         const UserName& user,
                                                         const BSONObj& updateObj,
                                                         const BSONObj& writeConcern) const {
        return _externalState->updatePrivilegeDocument(txn, user, updateObj, writeConcern);
    }

    Status AuthorizationManager::removePrivilegeDocuments(OperationContext* txn,
                                                          const BSONObj& query,
                                                          const BSONObj& writeConcern,
                                                          int* numRemoved) const {
        return _externalState->removePrivilegeDocuments(txn, query, writeConcern, numRemoved);
    }

    Status AuthorizationManager::removeRoleDocuments(OperationContext* txn,
                                                     const BSONObj& query,
                                                     const BSONObj& writeConcern,
                                                     int* numRemoved) const {
        Status status = _externalState->remove(txn,
                                               rolesCollectionNamespace,
                                               query,
                                               writeConcern,
                                               numRemoved);
        if (status.code() == ErrorCodes::UnknownError) {
            return Status(ErrorCodes::RoleModificationFailed, status.reason());
        }
        return status;
    }

    Status AuthorizationManager::insertRoleDocument(OperationContext* txn,
                                                    const BSONObj& roleObj,
                                                    const BSONObj& writeConcern) const {
        Status status = _externalState->insert(txn,
                                               rolesCollectionNamespace,
                                               roleObj,
                                               writeConcern);
        if (status.isOK()) {
            return status;
        }
        if (status.code() == ErrorCodes::DuplicateKey) {
            std::string name = roleObj[AuthorizationManager::ROLE_NAME_FIELD_NAME].String();
            std::string source = roleObj[AuthorizationManager::ROLE_DB_FIELD_NAME].String();
            return Status(ErrorCodes::DuplicateKey,
                          mongoutils::str::stream() << "Role \"" << name << "@" << source <<
                                  "\" already exists");
        }
        if (status.code() == ErrorCodes::UnknownError) {
            return Status(ErrorCodes::RoleModificationFailed, status.reason());
        }
        return status;
    }

    Status AuthorizationManager::updateRoleDocument(OperationContext* txn,
                                                    const RoleName& role,
                                                    const BSONObj& updateObj,
                                                    const BSONObj& writeConcern) const {
        Status status = _externalState->updateOne(
                txn,
                rolesCollectionNamespace,
                BSON(AuthorizationManager::ROLE_NAME_FIELD_NAME << role.getRole() <<
                     AuthorizationManager::ROLE_DB_FIELD_NAME << role.getDB()),
                updateObj,
                false,
                writeConcern);
        if (status.isOK()) {
            return status;
        }
        if (status.code() == ErrorCodes::NoMatchingDocument) {
            return Status(ErrorCodes::RoleNotFound,
                          mongoutils::str::stream() << "Role " << role.getFullName() <<
                                  " not found");
        }
        if (status.code() == ErrorCodes::UnknownError) {
            return Status(ErrorCodes::RoleModificationFailed, status.reason());
        }
        return status;
    }

    Status AuthorizationManager::queryAuthzDocument(
            OperationContext* txn,
            const NamespaceString& collectionName,
            const BSONObj& query,
            const BSONObj& projection,
            const stdx::function<void(const BSONObj&)>& resultProcessor) {
        return _externalState->query(txn, collectionName, query, projection, resultProcessor);
    }

    Status AuthorizationManager::updateAuthzDocuments(OperationContext* txn,
                                                      const NamespaceString& collectionName,
                                                      const BSONObj& query,
                                                      const BSONObj& updatePattern,
                                                      bool upsert,
                                                      bool multi,
                                                      const BSONObj& writeConcern,
                                                      int* nMatched) const {
        return _externalState->update(txn,
                                      collectionName,
                                      query,
                                      updatePattern,
                                      upsert,
                                      multi,
                                      writeConcern,
                                      nMatched);
    }

    Status AuthorizationManager::getBSONForPrivileges(const PrivilegeVector& privileges,
                                                      mutablebson::Element resultArray) {
        for (PrivilegeVector::const_iterator it = privileges.begin();
                it != privileges.end(); ++it) {
            std::string errmsg;
            ParsedPrivilege privilege;
            if (!ParsedPrivilege::privilegeToParsedPrivilege(*it, &privilege, &errmsg)) {
                return Status(ErrorCodes::BadValue, errmsg);
            }
            resultArray.appendObject("privileges", privilege.toBSON());
        }
        return Status::OK();
    }

    Status AuthorizationManager::getBSONForRole(RoleGraph* graph,
                                                const RoleName& roleName,
                                                mutablebson::Element result) {
        if (!graph->roleExists(roleName)) {
            return Status(ErrorCodes::RoleNotFound,
                          mongoutils::str::stream() << roleName.getFullName() <<
                                  "does not name an existing role");
        }
        std::string id = mongoutils::str::stream() << roleName.getDB() << "." << roleName.getRole();
        result.appendString("_id", id);
        result.appendString(ROLE_NAME_FIELD_NAME, roleName.getRole());
        result.appendString(ROLE_DB_FIELD_NAME, roleName.getDB());

        // Build privileges array
        mutablebson::Element privilegesArrayElement =
                result.getDocument().makeElementArray("privileges");
        result.pushBack(privilegesArrayElement);
        const PrivilegeVector& privileges = graph->getDirectPrivileges(roleName);
        Status status = getBSONForPrivileges(privileges, privilegesArrayElement);
        if (!status.isOK()) {
            return status;
        }

        // Build roles array
        mutablebson::Element rolesArrayElement = result.getDocument().makeElementArray("roles");
        result.pushBack(rolesArrayElement);
        for (RoleNameIterator roles = graph->getDirectSubordinates(roleName);
             roles.more();
             roles.next()) {

            const RoleName& subRole = roles.get();
            mutablebson::Element roleObj = result.getDocument().makeElementObject("");
            roleObj.appendString(ROLE_NAME_FIELD_NAME, subRole.getRole());
            roleObj.appendString(ROLE_DB_FIELD_NAME, subRole.getDB());
            rolesArrayElement.pushBack(roleObj);
        }

        return Status::OK();
    }

    Status AuthorizationManager::_initializeUserFromPrivilegeDocument(
            User* user, const BSONObj& privDoc) {
        V2UserDocumentParser parser;
        std::string userName = parser.extractUserNameFromUserDocument(privDoc);
        if (userName != user->getName().getUser()) {
            return Status(ErrorCodes::BadValue,
                          mongoutils::str::stream() << "User name from privilege document \""
                                  << userName
                                  << "\" doesn't match name of provided User \""
                                  << user->getName().getUser()
                                  << "\"",
                          0);
        }

        Status status = parser.initializeUserCredentialsFromUserDocument(user, privDoc);
        if (!status.isOK()) {
            return status;
        }
        status = parser.initializeUserRolesFromUserDocument(privDoc, user);
        if (!status.isOK()) {
            return status;
        }
        status = parser.initializeUserIndirectRolesFromUserDocument(privDoc, user);
        if (!status.isOK()) {
            return status;
        }
        status = parser.initializeUserPrivilegesFromUserDocument(privDoc, user);
        return Status::OK();
    }

    Status AuthorizationManager::getUserDescription(OperationContext* txn,
                                                    const UserName& userName,
                                                    BSONObj* result) {
        return _externalState->getUserDescription(txn, userName, result);
    }

    Status AuthorizationManager::getRoleDescription(const RoleName& roleName,
                                                    bool showPrivileges,
                                                    BSONObj* result) {
        return _externalState->getRoleDescription(roleName, showPrivileges, result);
    }

    Status AuthorizationManager::getRoleDescriptionsForDB(const std::string dbname,
                                                          bool showPrivileges,
                                                          bool showBuiltinRoles,
                                                          vector<BSONObj>* result) {
        return _externalState->getRoleDescriptionsForDB(dbname,
                                                        showPrivileges,
                                                        showBuiltinRoles,
                                                        result);
    }

    Status AuthorizationManager::acquireUser(
                OperationContext* txn, const UserName& userName, User** acquiredUser) {
        if (userName == internalSecurity.user->getName()) {
            *acquiredUser = internalSecurity.user;
            return Status::OK();
        }

        unordered_map<UserName, User*>::iterator it;

        CacheGuard guard(this, CacheGuard::fetchSynchronizationManual);
        while ((_userCache.end() == (it = _userCache.find(userName))) &&
               guard.otherUpdateInFetchPhase()) {

            guard.wait();
        }

        if (it != _userCache.end()) {
            fassert(16914, it->second);
            fassert(17003, it->second->isValid());
            fassert(17008, it->second->getRefCount() > 0);
            it->second->incrementRefCount();
            *acquiredUser = it->second;
            return Status::OK();
        }

        std::auto_ptr<User> user;

        int authzVersion = _version;
        guard.beginFetchPhase();

        // Number of times to retry a user document that fetches due to transient
        // AuthSchemaIncompatible errors.  These errors should only ever occur during and shortly
        // after schema upgrades.
        static const int maxAcquireRetries = 2;
        Status status = Status::OK();
        for (int i = 0; i < maxAcquireRetries; ++i) {
            if (authzVersion == schemaVersionInvalid) {
                Status status = _externalState->getStoredAuthorizationVersion(txn, &authzVersion);
                if (!status.isOK())
                    return status;
            }

            switch (authzVersion) {
            default:
                status = Status(ErrorCodes::BadValue, mongoutils::str::stream() <<
                                "Illegal value for authorization data schema version, " <<
                                authzVersion);
                break;
            case schemaVersion28SCRAM:
            case schemaVersion26Final:
            case schemaVersion26Upgrade:
                status = _fetchUserV2(txn, userName, &user);
                break;
            case schemaVersion24:
                status = Status(ErrorCodes::AuthSchemaIncompatible, mongoutils::str::stream() <<
                                "Authorization data schema version " << schemaVersion24 <<
                                " not supported after MongoDB version 2.6.");
                break;
            }
            if (status.isOK())
                break;
            if (status != ErrorCodes::AuthSchemaIncompatible)
                return status;

            authzVersion = schemaVersionInvalid;
        }
        if (!status.isOK())
            return status;

        guard.endFetchPhase();

        user->incrementRefCount();
        // NOTE: It is not safe to throw an exception from here to the end of the method.
        if (guard.isSameCacheGeneration()) {
            _userCache.insert(make_pair(userName, user.get()));
            if (_version == schemaVersionInvalid)
                _version = authzVersion;
        }
        else {
            // If the cache generation changed while this thread was in fetch mode, the data
            // associated with the user may now be invalid, so we must mark it as such.  The caller
            // may still opt to use the information for a short while, but not indefinitely.
            user->invalidate();
        }
        *acquiredUser = user.release();

        return Status::OK();
    }

    Status AuthorizationManager::_fetchUserV2(OperationContext* txn,
                                              const UserName& userName,
                                              std::auto_ptr<User>* acquiredUser) {
        BSONObj userObj;
        Status status = getUserDescription(txn, userName, &userObj);
        if (!status.isOK()) {
            return status;
        }

        // Put the new user into an auto_ptr temporarily in case there's an error while
        // initializing the user.
        std::auto_ptr<User> user(new User(userName));

        status = _initializeUserFromPrivilegeDocument(user.get(), userObj);
        if (!status.isOK()) {
            return status;
        }
        acquiredUser->reset(user.release());
        return Status::OK();
    }

    void AuthorizationManager::releaseUser(User* user) {
        if (user == internalSecurity.user) {
            return;
        }

        CacheGuard guard(this, CacheGuard::fetchSynchronizationManual);
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

    void AuthorizationManager::invalidateUserByName(const UserName& userName) {
        CacheGuard guard(this, CacheGuard::fetchSynchronizationManual);
        _updateCacheGeneration_inlock();
        unordered_map<UserName, User*>::iterator it = _userCache.find(userName);
        if (it == _userCache.end()) {
            return;
        }

        User* user = it->second;
        _userCache.erase(it);
        user->invalidate();
    }

    void AuthorizationManager::invalidateUsersFromDB(const std::string& dbname) {
        CacheGuard guard(this, CacheGuard::fetchSynchronizationManual);
        _updateCacheGeneration_inlock();
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

    void AuthorizationManager::invalidateUserCache() {
        CacheGuard guard(this, CacheGuard::fetchSynchronizationManual);
        _invalidateUserCache_inlock();
    }

    void AuthorizationManager::_invalidateUserCache_inlock() {
        _updateCacheGeneration_inlock();
        for (unordered_map<UserName, User*>::iterator it = _userCache.begin();
                it != _userCache.end(); ++it) {
            fassert(17266, it->second != internalSecurity.user);
            it->second->invalidate();
        }
        _userCache.clear();

        // Reread the schema version before acquiring the next user.
        _version = schemaVersionInvalid;
    }

    Status AuthorizationManager::initialize(OperationContext* txn) {
        invalidateUserCache();
        Status status = _externalState->initialize(txn);
        if (!status.isOK())
            return status;

        return Status::OK();
    }

    bool AuthorizationManager::tryAcquireAuthzUpdateLock(const StringData& why) {
        return _externalState->tryAcquireAuthzUpdateLock(why);
    }

    void AuthorizationManager::releaseAuthzUpdateLock() {
        return _externalState->releaseAuthzUpdateLock();
    }

namespace {

    /**
     * Logs that the auth schema upgrade failed because of "status" and returns "status".
     */
    Status logUpgradeFailed(const Status& status) {
        log() << "Auth schema upgrade failed with " << status;
        return status;
    }

    /** 
     * Updates a single user document from MONGODB-CR to SCRAM credentials.
     *
     * Throws a DBException on errors.
     */
    void updateUserCredentials(OperationContext* txn,
                               AuthzManagerExternalState* externalState,
                               const StringData& sourceDB,
                               const BSONObj& userDoc,
                               const BSONObj& writeConcern) {
        BSONElement credentialsElement = userDoc["credentials"];
        uassert(18806,
                mongoutils::str::stream() << "While preparing to upgrade user doc from "
                        "2.6/2.8 user data schema to the 2.8 SCRAM only schema, found a user doc "
                        "with missing or incorrectly formatted credentials: "
                        << userDoc.toString(),
                        credentialsElement.type() == Object);

        BSONObj credentialsObj = credentialsElement.Obj();
        BSONElement mongoCRElement = credentialsObj["MONGODB-CR"];
        BSONElement scramElement = credentialsObj["SCRAM-SHA-1"];

        // Ignore any user documents that already have SCRAM credentials. This should only
        // occur if a previous authSchemaUpgrade was interrupted halfway.
        if (!scramElement.eoo()) {
            return;
        }

        uassert(18744,
                mongoutils::str::stream() << "While preparing to upgrade user doc from "
                        "2.6/2.8 user data schema to the 2.8 SCRAM only schema, found a user doc "
                        "missing MONGODB-CR credentials :"
                        << userDoc.toString(),
                !mongoCRElement.eoo());

        std::string hashedPassword = mongoCRElement.String();

        BSONObj query = BSON("_id" <<  userDoc["_id"].String());
        BSONObjBuilder updateBuilder;
        {
            BSONObjBuilder toSetBuilder(updateBuilder.subobjStart("$set"));
            toSetBuilder << "credentials" <<
                            BSON("SCRAM-SHA-1" << scram::generateCredentials(hashedPassword,
                                        saslGlobalParams.scramIterationCount));
        }

        uassertStatusOK(externalState->updateOne(txn,
                                                 NamespaceString("admin", "system.users"),
                                                 query,
                                                 updateBuilder.obj(),
                                                 true,
                                                 writeConcern));
    }

    /** Loop through all the user documents in the admin.system.users collection.
     *  For each user document:
     *   1. Compute SCRAM credentials based on the MONGODB-CR hash
     *   2. Remove the MONGODB-CR hash
     *   3. Add SCRAM credentials to the user document credentials section
     */
    Status updateCredentials(
            OperationContext* txn,
            AuthzManagerExternalState* externalState,
            const BSONObj& writeConcern) {

        // Loop through and update the user documents in admin.system.users.
        Status status = externalState->query(
                txn,
                NamespaceString("admin", "system.users"),
                BSONObj(),
                BSONObj(),
                boost::bind(updateUserCredentials, txn, externalState, "admin", _1, writeConcern));
        if (!status.isOK())
            return logUpgradeFailed(status);

        // Update the schema version document.
        status = externalState->updateOne(
                txn,
                AuthorizationManager::versionCollectionNamespace,
                AuthorizationManager::versionDocumentQuery,
                BSON("$set" << BSON(AuthorizationManager::schemaVersionFieldName <<
                                    AuthorizationManager::schemaVersion28SCRAM)),
                true,
                writeConcern);
        if (!status.isOK())
            return logUpgradeFailed(status);

        return Status::OK();
    }
} //namespace

    Status AuthorizationManager::upgradeSchemaStep(
                            OperationContext* txn, const BSONObj& writeConcern, bool* isDone) {
        int authzVersion;
        Status status = getAuthorizationVersion(txn, &authzVersion);
        if (!status.isOK()) {
            return status;
        }

        switch (authzVersion) {
        case schemaVersion26Final: {
            Status status = updateCredentials(txn, _externalState.get(), writeConcern);
            if (status.isOK())
                *isDone = true;
            return status;
        }
        case schemaVersion28SCRAM:
            *isDone = true;
            return Status::OK();
        default:
            return Status(ErrorCodes::AuthSchemaIncompatible, mongoutils::str::stream() <<
                          "Do not know how to upgrade auth schema from version " << authzVersion);
        }
    }

    Status AuthorizationManager::upgradeSchema(
                OperationContext* txn, int maxSteps, const BSONObj& writeConcern) {

        if (maxSteps < 1) {
            return Status(ErrorCodes::BadValue,
                          "Minimum value for maxSteps parameter to upgradeSchema is 1");
        }
        invalidateUserCache();
        for (int i = 0; i < maxSteps; ++i) {
            bool isDone;
            Status status = upgradeSchemaStep(txn, writeConcern, &isDone);
            invalidateUserCache();
            if (!status.isOK() || isDone) {
                return status;
            }
        }
        return Status(ErrorCodes::OperationIncomplete, mongoutils::str::stream() <<
                      "Auth schema upgrade incomplete after " << maxSteps << " successful steps.");
    }

namespace {
    bool isAuthzNamespace(const StringData& ns) {
        return (ns == AuthorizationManager::rolesCollectionNamespace.ns() ||
                ns == AuthorizationManager::usersCollectionNamespace.ns() ||
                ns == AuthorizationManager::versionCollectionNamespace.ns());
    }

    bool isAuthzCollection(const StringData& coll) {
        return (coll == AuthorizationManager::rolesCollectionNamespace.coll() ||
                coll == AuthorizationManager::usersCollectionNamespace.coll() ||
                coll == AuthorizationManager::versionCollectionNamespace.coll());
    }

    bool loggedCommandOperatesOnAuthzData(const char* ns, const BSONObj& cmdObj) {
        if (ns != AuthorizationManager::adminCommandNamespace.ns())
            return false;
        const StringData cmdName(cmdObj.firstElement().fieldNameStringData());
        if (cmdName == "drop") {
            return isAuthzCollection(cmdObj.firstElement().valueStringData());
        }
        else if (cmdName == "dropDatabase") {
            return true;
        }
        else if (cmdName == "renameCollection") {
            return isAuthzCollection(cmdObj.firstElement().str()) ||
                isAuthzCollection(cmdObj["to"].str());
        }
        else if (cmdName == "dropIndexes" || cmdName == "deleteIndexes") {
            return false;
        }
        else if (cmdName == "create") {
            return false;
        }
        else {
            return true;
        }
    }

    bool appliesToAuthzData(
            const char* op,
            const char* ns,
            const BSONObj& o) {

        switch (*op) {
        case 'i':
        case 'u':
        case 'd':
            if (op[1] != '\0') return false; // "db" op type
            return isAuthzNamespace(ns);
        case 'c':
            return loggedCommandOperatesOnAuthzData(ns, o);
            break;
        case 'n':
            return false;
        default:
            return true;
        }
    }

    // Updates to users in the oplog are done by matching on the _id, which will always have the
    // form "<dbname>.<username>".  This function extracts the UserName from that string.
    StatusWith<UserName> extractUserNameFromIdString(const StringData& idstr) {
        size_t splitPoint = idstr.find('.');
        if (splitPoint == string::npos) {
            return StatusWith<UserName>(
                    ErrorCodes::FailedToParse,
                    mongoutils::str::stream() << "_id entries for user documents must be of "
                            "the form <dbname>.<username>.  Found: " << idstr);
        }
        return StatusWith<UserName>(UserName(idstr.substr(splitPoint),
                                             idstr.substr(0, splitPoint)));
    }

}  // namespace

    void AuthorizationManager::_updateCacheGeneration_inlock() {
        _cacheGeneration = OID::gen();
    }

    void AuthorizationManager::_invalidateRelevantCacheData(const char* op,
                                                            const char* ns,
                                                            const BSONObj& o,
                                                            const BSONObj* o2) {
        if (ns == AuthorizationManager::rolesCollectionNamespace.ns() ||
                ns == AuthorizationManager::versionCollectionNamespace.ns()) {
            invalidateUserCache();
            return;
        }

        if (*op == 'i' || *op == 'd' || *op == 'u') {
            // If you got into this function isAuthzNamespace() must have returned true, and we've
            // already checked that it's not the roles or version collection.
            invariant(ns == AuthorizationManager::usersCollectionNamespace.ns());

            StatusWith<UserName> userName(Status::OK());
            if (*op == 'u') {
                userName = extractUserNameFromIdString((*o2)["_id"].str());
            } else {
                userName = extractUserNameFromIdString(o["_id"].str());
            }
            if (!userName.isOK()) {
                warning() << "Invalidating user cache based on user being updated failed, will "
                        "invalidate the entire cache instead: " << userName.getStatus() << endl;
                invalidateUserCache();
                return;
            }
            invalidateUserByName(userName.getValue());
        } else {
            invalidateUserCache();
        }
    }

    void AuthorizationManager::logOp(
            const char* op,
            const char* ns,
            const BSONObj& o,
            BSONObj* o2,
            bool* b) {

        _externalState->logOp(op, ns, o, o2, b);
        if (appliesToAuthzData(op, ns, o)) {
            _invalidateRelevantCacheData(op, ns, o, o2);
        }
    }

} // namespace mongo
