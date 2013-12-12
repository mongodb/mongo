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
#include "mongo/bson/mutable/document.h"
#include "mongo/bson/mutable/element.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/db/auth/action_set.h"
#include "mongo/db/auth/authz_documents_update_guard.h"
#include "mongo/db/auth/authz_manager_external_state.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/auth/role_graph.h"
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
    const std::string AuthorizationManager::ROLE_SOURCE_FIELD_NAME = "db";
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

    const BSONObj AuthorizationManager::versionDocumentQuery = BSON("_id" << "authSchema");

    const std::string AuthorizationManager::schemaVersionServerParameter = "authSchemaVersion";
    const std::string AuthorizationManager::schemaVersionFieldName = "currentVersion";

#ifndef _MSC_EXTENSIONS
    const int AuthorizationManager::schemaVersion24;
    const int AuthorizationManager::schemaVersion26Upgrade;
    const int AuthorizationManager::schemaVersion26Final;
#endif

    bool AuthorizationManager::_doesSupportOldStylePrivileges = true;

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

        uint64_t _startGeneration;
        bool _isThisGuardInFetchPhase;
        AuthorizationManager* _authzManager;
        boost::unique_lock<boost::mutex> _lock;
    };

    AuthorizationManager::AuthorizationManager(AuthzManagerExternalState* externalState) :
        _authEnabled(false),
        _externalState(externalState),
        _version(schemaVersionInvalid),
        _cacheGeneration(0),
        _isFetchPhaseBusy(false) {
    }

    AuthorizationManager::~AuthorizationManager() {
        for (unordered_map<UserName, User*>::iterator it = _userCache.begin();
                it != _userCache.end(); ++it) {
            fassert(17265, it->second != internalSecurity.user);
            delete it->second ;
        }
    }

    int AuthorizationManager::getAuthorizationVersion() {
        CacheGuard guard(this, CacheGuard::fetchSynchronizationManual);
        int newVersion = _version;
        if (schemaVersionInvalid == newVersion) {
            while (guard.otherUpdateInFetchPhase())
                guard.wait();
            guard.beginFetchPhase();
            Status status = _externalState->getStoredAuthorizationVersion(&newVersion);
            guard.endFetchPhase();
            if (status.isOK()) {
                if (guard.isSameCacheGeneration()) {
                    _version = newVersion;
                }
            }
            else {
                warning() << "Could not determine schema version of authorization data. " << status;
            }
        }
        return newVersion;
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

    bool AuthorizationManager::isAuthEnabled() const {
        return _authEnabled;
    }

    bool AuthorizationManager::hasAnyPrivilegeDocuments() const {
        return _externalState->hasAnyPrivilegeDocuments();
    }

    Status AuthorizationManager::writeAuthSchemaVersionIfNeeded() {
        Status status =  _externalState->updateOne(
                AuthorizationManager::versionCollectionNamespace,
                AuthorizationManager::versionDocumentQuery,
                BSON("$set" << BSON(AuthorizationManager::schemaVersionFieldName <<
                                    AuthorizationManager::schemaVersion26Final)),
                true,  // upsert
                BSONObj());  // write concern
        if (status == ErrorCodes::NoMatchingDocument) {    // SERVER-11492
            status = Status::OK();
        }
        return status;
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

    Status AuthorizationManager::removeRoleDocuments(const BSONObj& query,
                                                     const BSONObj& writeConcern,
                                                     int* numRemoved) const {
        Status status = _externalState->remove(rolesCollectionNamespace,
                                               query,
                                               writeConcern,
                                               numRemoved);
        if (status.code() == ErrorCodes::UnknownError) {
            return Status(ErrorCodes::RoleModificationFailed, status.reason());
        }
        return status;
    }

    Status AuthorizationManager::insertRoleDocument(const BSONObj& roleObj,
                                                    const BSONObj& writeConcern) const {
        Status status = _externalState->insert(rolesCollectionNamespace,
                                               roleObj,
                                               writeConcern);
        if (status.isOK()) {
            return status;
        }
        if (status.code() == ErrorCodes::DuplicateKey) {
            std::string name = roleObj[AuthorizationManager::ROLE_NAME_FIELD_NAME].String();
            std::string source = roleObj[AuthorizationManager::ROLE_SOURCE_FIELD_NAME].String();
            return Status(ErrorCodes::DuplicateKey,
                          mongoutils::str::stream() << "Role \"" << name << "@" << source <<
                                  "\" already exists");
        }
        if (status.code() == ErrorCodes::UnknownError) {
            return Status(ErrorCodes::RoleModificationFailed, status.reason());
        }
        return status;
    }

    Status AuthorizationManager::updateRoleDocument(const RoleName& role,
                                                    const BSONObj& updateObj,
                                                    const BSONObj& writeConcern) const {
        Status status = _externalState->updateOne(
                rolesCollectionNamespace,
                BSON(AuthorizationManager::ROLE_NAME_FIELD_NAME << role.getRole() <<
                     AuthorizationManager::ROLE_SOURCE_FIELD_NAME << role.getDB()),
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
            const NamespaceString& collectionName,
            const BSONObj& query,
            const BSONObj& projection,
            const boost::function<void(const BSONObj&)>& resultProcessor) {
        return _externalState->query(collectionName, query, projection, resultProcessor);
    }

    Status AuthorizationManager::updateAuthzDocuments(const NamespaceString& collectionName,
                                                      const BSONObj& query,
                                                      const BSONObj& updatePattern,
                                                      bool upsert,
                                                      bool multi,
                                                      const BSONObj& writeConcern,
                                                      int* numUpdated) const {
        return _externalState->update(collectionName,
                                      query,
                                      updatePattern,
                                      upsert,
                                      multi,
                                      writeConcern,
                                      numUpdated);
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
        result.appendString(ROLE_SOURCE_FIELD_NAME, roleName.getDB());

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
            roleObj.appendString(ROLE_SOURCE_FIELD_NAME, subRole.getDB());
            rolesArrayElement.pushBack(roleObj);
        }

        return Status::OK();
    }

    static const RoleName userAdminAnyDatabase("userAdminAnyDatabase", "admin");
    static void _initializeUserPrivilegesFromRolesV1(User* user) {
        PrivilegeVector privileges;
        for (RoleNameIterator roles = user->getRoles(); roles.more(); roles.next()) {
            RoleGraph::addPrivilegesForBuiltinRole(roles.get(), &privileges);
            if (roles.get() == userAdminAnyDatabase) {
                // Giving schemaVersion24 users with userAdminAnyDatabase these privileges allows
                // them to conduct a manual upgrade from schemaVersion24 to schemaVersion26Final.
                ActionSet actions;
                actions.addAction(ActionType::find);
                actions.addAction(ActionType::insert);
                actions.addAction(ActionType::update);
                actions.addAction(ActionType::remove);
                actions.addAction(ActionType::createIndex);
                actions.addAction(ActionType::dropIndex);
                Privilege::addPrivilegeToPrivilegeVector(
                        &privileges,
                        Privilege(ResourcePattern::forExactNamespace(
                                          AuthorizationManager::versionCollectionNamespace),
                                  actions));
                Privilege::addPrivilegeToPrivilegeVector(
                        &privileges,
                        Privilege(ResourcePattern::forExactNamespace(
                                          AuthorizationManager::usersAltCollectionNamespace),
                                  actions));
                Privilege::addPrivilegeToPrivilegeVector(
                        &privileges,
                        Privilege(ResourcePattern::forExactNamespace(
                                          AuthorizationManager::usersBackupCollectionNamespace),
                                  actions));
            }
        }
        user->addPrivileges(privileges);
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
        status = parser.initializeUserPrivilegesFromUserDocument(privDoc, user);
        return Status::OK();
    }

    Status AuthorizationManager::getUserDescription(const UserName& userName, BSONObj* result) {
        return _externalState->getUserDescription(userName, result);
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

    Status AuthorizationManager::acquireUser(const UserName& userName, User** acquiredUser) {
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
                Status status = _externalState->getStoredAuthorizationVersion(&authzVersion);
                if (!status.isOK())
                    return status;
            }

            switch (authzVersion) {
            default:
                status = Status(ErrorCodes::BadValue, mongoutils::str::stream() <<
                                "Illegal value for authorization data schema version, " <<
                                authzVersion);
                break;
            case schemaVersion26Final:
            case schemaVersion26Upgrade:
                status = _fetchUserV2(userName, &user);
                break;
            case schemaVersion24:
                status = _fetchUserV1(userName, &user);
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

    Status AuthorizationManager::_fetchUserV2(const UserName& userName,
                                              std::auto_ptr<User>* acquiredUser) {
        BSONObj userObj;
        Status status = getUserDescription(userName, &userObj);
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

    Status AuthorizationManager::_fetchUserV1(const UserName& userName,
                                              std::auto_ptr<User>* acquiredUser) {

        BSONObj privDoc;
        V1UserDocumentParser parser;
        const bool isExternalUser = (userName.getDB() == "$external");
        const bool isAdminUser = (userName.getDB() == "admin");

        std::auto_ptr<User> user(new User(userName));
        user->setSchemaVersion1();
        user->markProbedV1("$external");
        if (isExternalUser) {
            User::CredentialData creds;
            creds.isExternal = true;
            user->setCredentials(creds);
        }
        else {
            // Users from databases other than "$external" must have an associated privilege
            // document in their database.
            Status status = _externalState->getPrivilegeDocumentV1(
                    userName.getDB(), userName, &privDoc);
            if (!status.isOK())
                return status;

            status = parser.initializeUserRolesFromUserDocument(
                    user.get(), privDoc, userName.getDB());
            if (!status.isOK())
                return status;

            status = parser.initializeUserCredentialsFromUserDocument(user.get(), privDoc);
            if (!status.isOK())
                return status;
            user->markProbedV1(userName.getDB());
        }
        if (!isAdminUser) {
            // Users from databases other than "admin" probe the "admin" database at login, to
            // ensure that the acquire any privileges derived from "otherDBRoles" fields in
            // admin.system.users.
            Status status = _externalState->getPrivilegeDocumentV1("admin", userName, &privDoc);
            if (status.isOK()) {
                status = parser.initializeUserRolesFromUserDocument(user.get(), privDoc, "admin");
                if (!status.isOK())
                    return status;
            }
            else if (status != ErrorCodes::UserNotFound) {
                return status;
            }
            user->markProbedV1("admin");
        }

        _initializeUserPrivilegesFromRolesV1(user.get());
        acquiredUser->reset(user.release());
        return Status::OK();
    }

    Status AuthorizationManager::acquireV1UserProbedForDb(
            const UserName& userName, const StringData& dbname, User** acquiredUser) {

        if (userName == internalSecurity.user->getName()) {
            *acquiredUser = internalSecurity.user;
            return Status::OK();
        }

        CacheGuard guard(this, CacheGuard::fetchSynchronizationManual);
        std::auto_ptr<User> user;
        {
            unordered_map<UserName, User*>::iterator it;
            while ((_userCache.end() == (it = _userCache.find(userName))) &&
                   guard.otherUpdateInFetchPhase()) {

                guard.wait();
            }

            if (_userCache.end() != it) {
                User* cachedUser = it->second;
                fassert(17225, cachedUser->isValid());
                if ((cachedUser->getSchemaVersion() != schemaVersion24) ||
                    cachedUser->hasProbedV1(dbname)) {

                    cachedUser->incrementRefCount();
                    *acquiredUser = cachedUser;
                    return Status::OK();
                }
                // We clone cachedUser for two reasons.  First, because it is not OK to mutate a
                // User object that may have been returned from acquireUser() or
                // acquireV1UserProbedForDb().  Second, because outside of this scope (or more
                // precisely, after calling guard.wait() or guard.beginFetchPhase(), after the
                // scope), references to data in the _userCache for which we do not know the
                // refcount is greater than zero are invalid.
                user.reset(cachedUser->clone());
            }
        }
        while (guard.otherUpdateInFetchPhase())
            guard.wait();
        guard.beginFetchPhase();

        if (!user.get()) {
            Status status = _fetchUserV1(userName, &user);
            if (status == ErrorCodes::AuthSchemaIncompatible) {
                // Must early-return from this if block, because we end the fetch phase.  Since the
                // auth schema is incompatible with schemaVersion24, make a best effort to do the
                // schemaVersion26(Upgrade|Final) user acquisition, and return.
                status = _fetchUserV2(userName, &user);
                guard.endFetchPhase();
                if (status.isOK()) {
                    // Not safe to throw from here until the function returns.
                    if (guard.isSameCacheGeneration()) {
                        _invalidateUserCache_inlock();
                        _userCache.insert(make_pair(userName, user.get()));
                    }
                    else {
                        user->invalidate();
                    }
                    user->incrementRefCount();
                    *acquiredUser = user.release();
                }
                return status;
            }
            if (!status.isOK())
                return status;
        }

        if (!user->hasProbedV1(dbname)) {
            BSONObj privDoc;
            Status status = _externalState->getPrivilegeDocumentV1(dbname, userName, &privDoc);
            if (status.isOK()) {
                V1UserDocumentParser parser;
                status = parser.initializeUserRolesFromUserDocument(user.get(), privDoc, dbname);
                if (!status.isOK())
                    return status;
                _initializeUserPrivilegesFromRolesV1(user.get());
                user->markProbedV1(dbname);
            }
            else if (status != ErrorCodes::UserNotFound) {
                return status;
            }
        }

        guard.endFetchPhase();
        user->incrementRefCount();
        // NOTE: It is not safe to throw an exception from here to the end of the method.
        *acquiredUser = user.release();
        if (guard.isSameCacheGeneration()) {
            unordered_map<UserName, User*>::iterator it = _userCache.find(userName);
            if (it != _userCache.end()) {
                it->second->invalidate();
                it->second = *acquiredUser;
            }
            else {
                _userCache.insert(make_pair(userName, *acquiredUser));
            }
        }
        else {
            // If the cache generation changed while this thread was in fetch mode, the data
            // associated with the user may now be invalid, so we must mark it as such.  The caller
            // may still opt to use the information for a short while, but not indefinitely.
            (*acquiredUser)->invalidate();
        }
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
        ++_cacheGeneration;
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
        ++_cacheGeneration;
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
        ++_cacheGeneration;
        for (unordered_map<UserName, User*>::iterator it = _userCache.begin();
                it != _userCache.end(); ++it) {
            fassert(17266, it->second != internalSecurity.user);
            it->second->invalidate();
        }
        _userCache.clear();

        // Reread the schema version before acquiring the next user.
        _version = schemaVersionInvalid;
    }

    Status AuthorizationManager::initialize() {
        invalidateUserCache();
        Status status = _externalState->initialize();
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
        log() << "Auth schema upgraded failed with " << status;
        return status;
    }

    /**
     * Upserts a schemaVersion26Upgrade user document in the usersAltCollectionNamespace
     * according to the schemaVersion24 user document "oldUserDoc" from database "sourceDB".
     *
     * Throws a DBException on errors.
     */
    void upgradeProcessUser(AuthzManagerExternalState* externalState,
                            const StringData& sourceDB,
                            const BSONObj& oldUserDoc,
                            const BSONObj& writeConcern) {

        std::string oldUserSource;
        uassertStatusOK(bsonExtractStringFieldWithDefault(
                                oldUserDoc,
                                "userSource",
                                sourceDB,
                                &oldUserSource));

        if (oldUserSource == "local")
            return;  // Skips users from "local" database, which cannot be upgraded.

        const std::string oldUserName = oldUserDoc["user"].String();
        BSONObj query = BSON("_id" << oldUserSource + "." + oldUserName);

        BSONObjBuilder updateBuilder;

        {
            BSONObjBuilder toSetBuilder(updateBuilder.subobjStart("$set"));
            toSetBuilder << "user" << oldUserName << "db" << oldUserSource;
            BSONElement pwdElement = oldUserDoc["pwd"];
            if (!pwdElement.eoo()) {
                toSetBuilder << "credentials" << BSON("MONGODB-CR" << pwdElement.String());
            }
            else if (oldUserSource == "$external") {
                toSetBuilder << "credentials" << BSON("external" << true);
            }
        }
        {
            BSONObjBuilder pushAllBuilder(updateBuilder.subobjStart("$pushAll"));
            BSONArrayBuilder rolesBuilder(pushAllBuilder.subarrayStart("roles"));

            const bool readOnly = oldUserDoc["readOnly"].trueValue();
            const BSONElement rolesElement = oldUserDoc["roles"];
            if (readOnly) {
                // Handles the cases where there is a truthy readOnly field, which is a 2.2-style
                // read-only user.
                if (sourceDB == "admin") {
                    rolesBuilder << BSON("role" << "readAnyDatabase" << "db" << "admin");
                }
                else {
                    rolesBuilder << BSON("role" << "read" << "db" << sourceDB);
                }
            }
            else if (rolesElement.eoo()) {
                // Handles the cases where the readOnly field is absent or falsey, but the
                // user is known to be 2.2-style because it lacks a roles array.
                if (sourceDB == "admin") {
                    rolesBuilder << BSON("role" << "root" << "db" << "admin");
                }
                else {
                    rolesBuilder << BSON("role" << "dbOwner" << "db" << sourceDB);
                }
            }
            else {
                // Handles 2.4-style user documents, with roles arrays and (optionally, in admin db)
                // otherDBRoles objects.
                uassert(17252,
                        "roles field in v2.4 user documents must be an array",
                        rolesElement.type() == Array);
                for (BSONObjIterator oldRoles(rolesElement.Obj());
                     oldRoles.more();
                     oldRoles.next()) {

                    BSONElement roleElement = *oldRoles;
                    rolesBuilder << BSON("role" << roleElement.String() << "db" << sourceDB);
                }

                BSONElement otherDBRolesElement = oldUserDoc["otherDBRoles"];
                if (sourceDB == "admin" && !otherDBRolesElement.eoo()) {
                    uassert(17253,
                            "otherDBRoles field in v2.4 user documents must be an object.",
                            otherDBRolesElement.type() == Object);

                    for (BSONObjIterator otherDBs(otherDBRolesElement.Obj());
                         otherDBs.more();
                         otherDBs.next()) {

                        BSONElement otherDBRoles = *otherDBs;
                        if (otherDBRoles.fieldNameStringData() == "local")
                            continue;
                        uassert(17254,
                                "Member fields of otherDBRoles objects must be arrays.",
                                otherDBRoles.type() == Array);
                        for (BSONObjIterator oldRoles(otherDBRoles.Obj());
                             oldRoles.more();
                             oldRoles.next()) {

                            BSONElement roleElement = *oldRoles;
                            rolesBuilder << BSON("role" << roleElement.String() <<
                                                 "db" << otherDBRoles.fieldNameStringData());
                        }
                    }
                }
            }
        }

        BSONObj update = updateBuilder.done();
        uassertStatusOK(externalState->updateOne(
                                AuthorizationManager::usersAltCollectionNamespace,
                                query,
                                update,
                                true,
                                writeConcern));
    }

    /**
     * For every schemaVersion24 user document in the system.users collection of "db",
     * upserts the appropriate schemaVersion26Upgrade user document in usersAltCollectionNamespace.
     */
    Status upgradeUsersFromDB(AuthzManagerExternalState* externalState,
                             const StringData& db,
                             const BSONObj& writeConcern) {
        log() << "Auth schema upgrade processing schema version " <<
            AuthorizationManager::schemaVersion24 << " users from database " << db;
        return externalState->query(
                NamespaceString(db, "system.users"),
                BSONObj(),
                BSONObj(),
                boost::bind(upgradeProcessUser, externalState, db, _1, writeConcern));
    }

    /**
     * Inserts "document" into "collection", throwing a DBException on failure.
     */
    void uassertInsertIntoCollection(
            AuthzManagerExternalState* externalState,
            const NamespaceString& collection,
            const BSONObj& document,
            const BSONObj& writeConcern) {
        uassertStatusOK(externalState->insert(collection, document, writeConcern));
    }

    /**
     * Copies the contents of "sourceCollection" into "targetCollection", which must be a distinct
     * collection.
     */
    Status copyCollectionContents(
            AuthzManagerExternalState* externalState,
            const NamespaceString& targetCollection,
            const NamespaceString& sourceCollection,
            const BSONObj& writeConcern) {
        return externalState->query(
                sourceCollection,
                BSONObj(),
                BSONObj(),
                boost::bind(uassertInsertIntoCollection,
                            externalState,
                            targetCollection,
                            _1,
                            writeConcern));
    }

    /**
     * Upgrades auth schema from schemaVersion24 to schemaVersion26Upgrade.
     *
     * Assumes that the current version is schemaVersion24.
     *
     * - Backs up usersCollectionNamespace into usersBackupCollectionNamespace.
     * - Empties usersAltCollectionNamespace.
     * - Builds usersAltCollectionNamespace from the contents of every database's system.users
     *   collection.
     * - Manipulates the schema version document appropriately.
     *
     * Upon successful completion, system is in schemaVersion26Upgrade.  On failure,
     * system is in schemaVersion24 or schemaVersion26Upgrade, but it is safe to re-run this
     * method.
     */
    Status buildNewUsersCollection(
            AuthzManagerExternalState* externalState,
            const BSONObj& writeConcern) {

        // Write an explicit schemaVersion24 into the schema version document, to facilitate
        // recovery.
        Status status = externalState->updateOne(
                AuthorizationManager::versionCollectionNamespace,
                AuthorizationManager::versionDocumentQuery,
                BSON("$set" << BSON(AuthorizationManager::schemaVersionFieldName <<
                                    AuthorizationManager::schemaVersion24)),
                true,
                writeConcern);
        if (!status.isOK())
            return logUpgradeFailed(status);

        log() << "Auth schema upgrade erasing contents of " <<
            AuthorizationManager::usersBackupCollectionNamespace;
        int numRemoved;
        status = externalState->remove(
                AuthorizationManager::usersBackupCollectionNamespace,
                BSONObj(),
                writeConcern,
                &numRemoved);
        if (!status.isOK())
            return logUpgradeFailed(status);

        log() << "Auth schema upgrade backing up " <<
            AuthorizationManager::usersCollectionNamespace << " into " <<
            AuthorizationManager::usersBackupCollectionNamespace;
        status = copyCollectionContents(
                externalState,
                AuthorizationManager::usersBackupCollectionNamespace,
                AuthorizationManager::usersCollectionNamespace,
                writeConcern);
        if (!status.isOK())
            return logUpgradeFailed(status);

        log() << "Auth schema upgrade dropping indexes from " <<
            AuthorizationManager::usersAltCollectionNamespace;
        status = externalState->dropIndexes(AuthorizationManager::usersAltCollectionNamespace,
                                            writeConcern);
        if (!status.isOK()) {
            warning() << "Auth schema upgrade failed to drop indexes on " <<
                AuthorizationManager::usersAltCollectionNamespace << " (" << status << ")";
        }

        log() << "Auth schema upgrade erasing contents of " <<
            AuthorizationManager::usersAltCollectionNamespace;
        status = externalState->remove(
                AuthorizationManager::usersAltCollectionNamespace,
                BSONObj(),
                writeConcern,
                &numRemoved);
        if (!status.isOK())
            return logUpgradeFailed(status);

        log() << "Auth schema upgrade creating needed indexes of " <<
            AuthorizationManager::usersAltCollectionNamespace;
        status = externalState->createIndex(
                AuthorizationManager::usersAltCollectionNamespace,
                BSON("user" << 1 << "db" << 1),
                true,
                writeConcern);
        if (!status.isOK())
            return logUpgradeFailed(status);

        // Update usersAltCollectionNamespace from the contents of each database's system.users
        // collection.
        std::vector<std::string> dbNames;
        status = externalState->getAllDatabaseNames(&dbNames);
        if (!status.isOK())
            return logUpgradeFailed(status);
        for (size_t i = 0; i < dbNames.size(); ++i) {
            const std::string& db = dbNames[i];
            status = upgradeUsersFromDB(externalState, db, writeConcern);
            if (!status.isOK())
                return logUpgradeFailed(status);
        }

        // Switch to schemaVersion26Upgrade.  Starting after this point, user information will be
        // read from usersAltCollectionNamespace.
        status = externalState->updateOne(
                AuthorizationManager::versionCollectionNamespace,
                AuthorizationManager::versionDocumentQuery,
                BSON("$set" << BSON(AuthorizationManager::schemaVersionFieldName <<
                                    AuthorizationManager::schemaVersion26Upgrade)),
                true,
                writeConcern);
        if (!status.isOK())
            return logUpgradeFailed(status);
        return Status::OK();
    }

    /**
     * Performs the upgrade to schemaVersion26Final from schemaVersion26Upgrade.
     *
     * Assumes that the current version is schemaVersion26Upgrade.
     *
     * - Erases contents and indexes of usersCollectionNamespace.
     * - Erases contents and indexes of rolesCollectionNamespace.
     * - Creates appropriate indexes on usersCollectionNamespace and rolesCollectionNamespace.
     * - Copies usersAltCollectionNamespace to usersCollectionNamespace.
     * - Manipulates the schema version document appropriately.
     *
     * Upon successful completion, system is in schemaVersion26Final.  On failure,
     * system is in schemaVersion26Upgrade or schemaVersion26Final, but it is safe to re-run this
     * method.
     */
    Status overwriteSystemUsersCollection(
            AuthzManagerExternalState* externalState,
            const BSONObj& writeConcern) {
        log() << "Auth schema upgrade erasing version " << AuthorizationManager::schemaVersion24 <<
            " users from " << AuthorizationManager::usersCollectionNamespace;
        Status status = externalState->dropIndexes(AuthorizationManager::usersCollectionNamespace,
                                                   writeConcern);
        if (!status.isOK())
            return logUpgradeFailed(status);
        int numRemoved;
        status = externalState->remove(
                AuthorizationManager::usersCollectionNamespace,
                BSONObj(),
                writeConcern,
                &numRemoved);
        if (!status.isOK())
            return logUpgradeFailed(status);

        log() << "Auth schema upgrade erasing " << AuthorizationManager::rolesCollectionNamespace;
        status = externalState->dropIndexes(AuthorizationManager::rolesCollectionNamespace,
                                            writeConcern);
        if (!status.isOK()) {
            warning() << "Auth schema upgrade failed to drop indexes on " <<
                AuthorizationManager::rolesCollectionNamespace << " (" << status << ")";
        }

        status = externalState->remove(
                AuthorizationManager::rolesCollectionNamespace,
                BSONObj(),
                writeConcern,
                &numRemoved);
        if (!status.isOK())
            return logUpgradeFailed(status);

        log() << "Auth schema upgrade creating needed indexes of " <<
            AuthorizationManager::rolesCollectionNamespace;
        status = externalState->createIndex(
                AuthorizationManager::rolesCollectionNamespace,
                BSON("role" << 1 << "db" << 1),
                true,
                writeConcern);
        if (!status.isOK())
            return logUpgradeFailed(status);

        log() << "Auth schema upgrade creating needed indexes of " <<
            AuthorizationManager::usersCollectionNamespace;
        status = externalState->createIndex(
                AuthorizationManager::usersCollectionNamespace,
                BSON("user" << 1 << "db" << 1),
                true,
                writeConcern);
        if (!status.isOK())
            return logUpgradeFailed(status);

        log() << "Auth schema upgrade copying version " <<
            AuthorizationManager::schemaVersion26Final << " users from " <<
            AuthorizationManager::usersAltCollectionNamespace << " to " <<
            AuthorizationManager::usersCollectionNamespace;

        status = copyCollectionContents(
                externalState,
                AuthorizationManager::usersCollectionNamespace,
                AuthorizationManager::usersAltCollectionNamespace,
                writeConcern);
        if (!status.isOK())
            return logUpgradeFailed(status);

        // Set the schema version in the schema version document, completing the process.
        status = externalState->updateOne(
                AuthorizationManager::versionCollectionNamespace,
                AuthorizationManager::versionDocumentQuery,
                BSON("$set" << BSON(AuthorizationManager::schemaVersionFieldName <<
                                    AuthorizationManager::schemaVersion26Final)),
                true,
                writeConcern);
        if (!status.isOK())
            return logUpgradeFailed(status);
        return Status::OK();
    }
}  // namespace

    Status AuthorizationManager::upgradeSchemaStep(const BSONObj& writeConcern, bool* isDone) {
        int authzVersion = getAuthorizationVersion();
        switch (authzVersion) {
        case schemaVersion24:
            *isDone = false;
            return buildNewUsersCollection(_externalState.get(), writeConcern);
        case schemaVersion26Upgrade: {
            Status status = overwriteSystemUsersCollection(_externalState.get(), writeConcern);
            if (status.isOK())
                *isDone = true;
            return status;
        }
        case schemaVersion26Final:
            *isDone = true;
            return Status::OK();
        default:
            return Status(ErrorCodes::AuthSchemaIncompatible, mongoutils::str::stream() <<
                          "Do not know how to upgrade auth schema from version " << authzVersion);
        }
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
            return isAuthzCollection(StringData(cmdObj.firstElement().valuestr(),
                                                cmdObj.firstElement().valuestrsize() - 1));
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
}  // namespace

    void AuthorizationManager::logOp(
            const char* op,
            const char* ns,
            const BSONObj& o,
            BSONObj* o2,
            bool* b) {

        _externalState->logOp(op, ns, o, o2, b);
        if (appliesToAuthzData(op, ns, o)) {
            CacheGuard guard(this, CacheGuard::fetchSynchronizationManual);
            _invalidateUserCache_inlock();
        }
    }

} // namespace mongo
