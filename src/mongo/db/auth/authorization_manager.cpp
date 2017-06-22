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

#include <memory>
#include <string>
#include <vector>

#include "mongo/base/init.h"
#include "mongo/base/status.h"
#include "mongo/bson/mutable/document.h"
#include "mongo/bson/mutable/element.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/crypto/mechanism_scram.h"
#include "mongo/db/auth/action_set.h"
#include "mongo/db/auth/address_restriction.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/authz_manager_external_state.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/auth/privilege_parser.h"
#include "mongo/db/auth/role_graph.h"
#include "mongo/db/auth/sasl_options.h"
#include "mongo/db/auth/user.h"
#include "mongo/db/auth/user_document_parser.h"
#include "mongo/db/auth/user_name.h"
#include "mongo/db/auth/user_name_hash.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/mongod_options.h"
#include "mongo/platform/compiler.h"
#include "mongo/platform/unordered_map.h"
#include "mongo/stdx/memory.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

using std::begin;
using std::end;
using std::endl;
using std::back_inserter;
using std::string;
using std::vector;

AuthInfo internalSecurity;

MONGO_INITIALIZER_WITH_PREREQUISITES(SetupInternalSecurityUser, ("EndStartupOptionStorage"))
(InitializerContext* const context) try {
    User* user = new User(UserName("__system", "local"));

    user->incrementRefCount();  // Pin this user so the ref count never drops below 1.
    ActionSet allActions;
    allActions.addAllActions();
    PrivilegeVector privileges;
    RoleGraph::generateUniversalPrivileges(&privileges);
    user->addPrivileges(privileges);

    if (mongodGlobalParams.whitelistedClusterNetwork) {
        const auto& whitelist = *mongodGlobalParams.whitelistedClusterNetwork;

        auto restriction = stdx::make_unique<ClientSourceRestriction>(whitelist);
        auto restrictionSet = stdx::make_unique<RestrictionSet<>>(std::move(restriction));
        auto restrictionDocument =
            stdx::make_unique<RestrictionDocument<>>(std::move(restrictionSet));

        RestrictionDocuments clusterWhiteList(std::move(restrictionDocument));

        user->setRestrictions(std::move(clusterWhiteList));
    }


    internalSecurity.user = user;

    return Status::OK();
} catch (...) {
    return exceptionToStatus();
}

const std::string AuthorizationManager::USER_NAME_FIELD_NAME = "user";
const std::string AuthorizationManager::USER_ID_FIELD_NAME = "userId";
const std::string AuthorizationManager::USER_DB_FIELD_NAME = "db";
const std::string AuthorizationManager::ROLE_NAME_FIELD_NAME = "role";
const std::string AuthorizationManager::ROLE_DB_FIELD_NAME = "db";
const std::string AuthorizationManager::PASSWORD_FIELD_NAME = "pwd";
const std::string AuthorizationManager::V1_USER_NAME_FIELD_NAME = "user";
const std::string AuthorizationManager::V1_USER_SOURCE_FIELD_NAME = "userSource";

const NamespaceString AuthorizationManager::adminCommandNamespace("admin.$cmd");
const NamespaceString AuthorizationManager::rolesCollectionNamespace("admin.system.roles");
const NamespaceString AuthorizationManager::usersAltCollectionNamespace("admin.system.new_users");
const NamespaceString AuthorizationManager::usersBackupCollectionNamespace(
    "admin.system.backup_users");
const NamespaceString AuthorizationManager::usersCollectionNamespace("admin.system.users");
const NamespaceString AuthorizationManager::versionCollectionNamespace("admin.system.version");
const NamespaceString AuthorizationManager::defaultTempUsersCollectionNamespace("admin.tempusers");
const NamespaceString AuthorizationManager::defaultTempRolesCollectionNamespace("admin.temproles");

const Status AuthorizationManager::authenticationFailedStatus(ErrorCodes::AuthenticationFailed,
                                                              "Authentication failed.");

const BSONObj AuthorizationManager::versionDocumentQuery = BSON("_id"
                                                                << "authSchema");

const std::string AuthorizationManager::schemaVersionFieldName = "currentVersion";

const int AuthorizationManager::schemaVersion24;
const int AuthorizationManager::schemaVersion26Upgrade;
const int AuthorizationManager::schemaVersion26Final;
const int AuthorizationManager::schemaVersion28SCRAM;

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
    enum FetchSynchronization { fetchSynchronizationAutomatic, fetchSynchronizationManual };

    /**
     * Constructs a cache guard, locking the mutex that synchronizes user cache accesses.
     */
    CacheGuard(AuthorizationManager* authzManager,
               const FetchSynchronization sync = fetchSynchronizationAutomatic)
        : _isThisGuardInFetchPhase(false),
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
    bool otherUpdateInFetchPhase() {
        return _authzManager->_isFetchPhaseBusy;
    }

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
    stdx::unique_lock<stdx::mutex> _lock;
};

AuthorizationManager::AuthorizationManager(std::unique_ptr<AuthzManagerExternalState> externalState)
    : _authEnabled(false),
      _privilegeDocsExist(false),
      _externalState(std::move(externalState)),
      _version(schemaVersionInvalid),
      _isFetchPhaseBusy(false) {
    _updateCacheGeneration_inlock();
}

AuthorizationManager::~AuthorizationManager() {
    for (unordered_map<UserName, User*>::iterator it = _userCache.begin(); it != _userCache.end();
         ++it) {
        fassert(17265, it->second != internalSecurity.user);
        delete it->second;
    }
}

std::unique_ptr<AuthorizationSession> AuthorizationManager::makeAuthorizationSession() {
    return stdx::make_unique<AuthorizationSession>(
        _externalState->makeAuthzSessionExternalState(this));
}

void AuthorizationManager::setShouldValidateAuthSchemaOnStartup(bool validate) {
    _startupAuthSchemaValidation = validate;
}

bool AuthorizationManager::shouldValidateAuthSchemaOnStartup() {
    return _startupAuthSchemaValidation;
}

Status AuthorizationManager::getAuthorizationVersion(OperationContext* opCtx, int* version) {
    CacheGuard guard(this, CacheGuard::fetchSynchronizationManual);
    int newVersion = _version;
    if (schemaVersionInvalid == newVersion) {
        while (guard.otherUpdateInFetchPhase())
            guard.wait();
        guard.beginFetchPhase();
        Status status = _externalState->getStoredAuthorizationVersion(opCtx, &newVersion);
        guard.endFetchPhase();
        if (!status.isOK()) {
            warning() << "Problem fetching the stored schema version of authorization data: "
                      << redact(status);
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

bool AuthorizationManager::hasAnyPrivilegeDocuments(OperationContext* opCtx) {
    stdx::unique_lock<stdx::mutex> lk(_privilegeDocsExistMutex);
    if (_privilegeDocsExist) {
        // If we know that a user exists, don't re-check.
        return true;
    }

    lk.unlock();
    bool privDocsExist = _externalState->hasAnyPrivilegeDocuments(opCtx);
    lk.lock();

    if (privDocsExist) {
        _privilegeDocsExist = true;
    }

    return _privilegeDocsExist;
}

Status AuthorizationManager::getBSONForPrivileges(const PrivilegeVector& privileges,
                                                  mutablebson::Element resultArray) {
    for (PrivilegeVector::const_iterator it = privileges.begin(); it != privileges.end(); ++it) {
        std::string errmsg;
        ParsedPrivilege privilege;
        if (!ParsedPrivilege::privilegeToParsedPrivilege(*it, &privilege, &errmsg)) {
            return Status(ErrorCodes::BadValue, errmsg);
        }
        resultArray.appendObject("privileges", privilege.toBSON()).transitional_ignore();
    }
    return Status::OK();
}

Status AuthorizationManager::getBSONForRole(RoleGraph* graph,
                                            const RoleName& roleName,
                                            mutablebson::Element result) {
    if (!graph->roleExists(roleName)) {
        return Status(ErrorCodes::RoleNotFound,
                      mongoutils::str::stream() << roleName.getFullName()
                                                << "does not name an existing role");
    }
    std::string id = mongoutils::str::stream() << roleName.getDB() << "." << roleName.getRole();
    result.appendString("_id", id).transitional_ignore();
    result.appendString(ROLE_NAME_FIELD_NAME, roleName.getRole()).transitional_ignore();
    result.appendString(ROLE_DB_FIELD_NAME, roleName.getDB()).transitional_ignore();

    // Build privileges array
    mutablebson::Element privilegesArrayElement =
        result.getDocument().makeElementArray("privileges");
    result.pushBack(privilegesArrayElement).transitional_ignore();
    const PrivilegeVector& privileges = graph->getDirectPrivileges(roleName);
    Status status = getBSONForPrivileges(privileges, privilegesArrayElement);
    if (!status.isOK()) {
        return status;
    }

    // Build roles array
    mutablebson::Element rolesArrayElement = result.getDocument().makeElementArray("roles");
    result.pushBack(rolesArrayElement).transitional_ignore();
    for (RoleNameIterator roles = graph->getDirectSubordinates(roleName); roles.more();
         roles.next()) {
        const RoleName& subRole = roles.get();
        mutablebson::Element roleObj = result.getDocument().makeElementObject("");
        roleObj.appendString(ROLE_NAME_FIELD_NAME, subRole.getRole()).transitional_ignore();
        roleObj.appendString(ROLE_DB_FIELD_NAME, subRole.getDB()).transitional_ignore();
        rolesArrayElement.pushBack(roleObj).transitional_ignore();
    }

    return Status::OK();
}

Status AuthorizationManager::_initializeUserFromPrivilegeDocument(User* user,
                                                                  const BSONObj& privDoc) {
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

    user->setID(parser.extractUserIDFromUserDocument(privDoc));

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
    if (!status.isOK()) {
        return status;
    }
    status = parser.initializeAuthenticationRestrictionsFromUserDocument(privDoc, user);
    if (!status.isOK()) {
        return status;
    }

    return Status::OK();
}

Status AuthorizationManager::getUserDescription(OperationContext* opCtx,
                                                const UserName& userName,
                                                BSONObj* result) {
    return _externalState->getUserDescription(opCtx, userName, result);
}

Status AuthorizationManager::getRoleDescription(OperationContext* opCtx,
                                                const RoleName& roleName,
                                                PrivilegeFormat privileges,
                                                AuthenticationRestrictionsFormat restrictions,
                                                BSONObj* result) {
    return _externalState->getRoleDescription(opCtx, roleName, privileges, restrictions, result);
}

Status AuthorizationManager::getRolesDescription(OperationContext* opCtx,
                                                 const std::vector<RoleName>& roleName,
                                                 PrivilegeFormat privileges,
                                                 AuthenticationRestrictionsFormat restrictions,
                                                 BSONObj* result) {
    return _externalState->getRolesDescription(opCtx, roleName, privileges, restrictions, result);
}


Status AuthorizationManager::getRoleDescriptionsForDB(OperationContext* opCtx,
                                                      const std::string dbname,
                                                      PrivilegeFormat privileges,
                                                      AuthenticationRestrictionsFormat restrictions,
                                                      bool showBuiltinRoles,
                                                      vector<BSONObj>* result) {
    return _externalState->getRoleDescriptionsForDB(
        opCtx, dbname, privileges, restrictions, showBuiltinRoles, result);
}

Status AuthorizationManager::acquireUserToRefreshSessionCache(OperationContext* opCtx,
                                                              const UserName& userName,
                                                              boost::optional<OID> id,
                                                              User** acquiredUser) {
    // Funnel down to acquireUserForInitialAuth, and then do the id checking.
    auto status = acquireUserForInitialAuth(opCtx, userName, acquiredUser);
    if (!status.isOK()) {
        // If we got a non-ok status, no acquiredUser should be set, so we can simply
        // return without doing an id check.
        return status;
    }

    // Verify that the returned user matches the id that we were passed.
    if (id != (*acquiredUser)->getID()) {
        // If the generations don't match, then fail.
        releaseUser(*acquiredUser);
        *acquiredUser = nullptr;
        return Status(ErrorCodes::UserNotFound,
                      mongoutils::str::stream() << "User id from privilege document \""
                                                << userName.toString()
                                                << "\" doesn't match provided user id",
                      0);
    }

    return status;
}

Status AuthorizationManager::acquireUserForInitialAuth(OperationContext* opCtx,
                                                       const UserName& userName,
                                                       User** acquiredUser) {
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

    std::unique_ptr<User> user;

    int authzVersion = _version;
    guard.beginFetchPhase();

    // Number of times to retry a user document that fetches due to transient
    // AuthSchemaIncompatible errors.  These errors should only ever occur during and shortly
    // after schema upgrades.
    static const int maxAcquireRetries = 2;
    Status status = Status::OK();
    for (int i = 0; i < maxAcquireRetries; ++i) {
        if (authzVersion == schemaVersionInvalid) {
            Status status = _externalState->getStoredAuthorizationVersion(opCtx, &authzVersion);
            if (!status.isOK())
                return status;
        }

        switch (authzVersion) {
            default:
                status = Status(ErrorCodes::BadValue,
                                mongoutils::str::stream()
                                    << "Illegal value for authorization data schema version, "
                                    << authzVersion);
                break;
            case schemaVersion28SCRAM:
            case schemaVersion26Final:
            case schemaVersion26Upgrade:
                status = _fetchUserV2(opCtx, userName, &user);
                break;
            case schemaVersion24:
                status = Status(ErrorCodes::AuthSchemaIncompatible,
                                mongoutils::str::stream()
                                    << "Authorization data schema version "
                                    << schemaVersion24
                                    << " not supported after MongoDB version 2.6.");
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
        _userCache.insert(std::make_pair(userName, user.get()));
        if (_version == schemaVersionInvalid)
            _version = authzVersion;
    } else {
        // If the cache generation changed while this thread was in fetch mode, the data
        // associated with the user may now be invalid, so we must mark it as such.  The caller
        // may still opt to use the information for a short while, but not indefinitely.
        user->invalidate();
    }
    *acquiredUser = user.release();

    return Status::OK();
}

Status AuthorizationManager::_fetchUserV2(OperationContext* opCtx,
                                          const UserName& userName,
                                          std::unique_ptr<User>* acquiredUser) {
    BSONObj userObj;
    Status status = getUserDescription(opCtx, userName, &userObj);
    if (!status.isOK()) {
        return status;
    }

    // Put the new user into an unique_ptr temporarily in case there's an error while
    // initializing the user.
    auto user = stdx::make_unique<User>(userName);

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
    for (unordered_map<UserName, User*>::iterator it = _userCache.begin(); it != _userCache.end();
         ++it) {
        fassert(17266, it->second != internalSecurity.user);
        it->second->invalidate();
    }
    _userCache.clear();

    // Reread the schema version before acquiring the next user.
    _version = schemaVersionInvalid;
}

Status AuthorizationManager::initialize(OperationContext* opCtx) {
    invalidateUserCache();
    Status status = _externalState->initialize(opCtx);
    if (!status.isOK())
        return status;

    return Status::OK();
}

namespace {
bool isAuthzNamespace(const NamespaceString& nss) {
    return (nss == AuthorizationManager::rolesCollectionNamespace ||
            nss == AuthorizationManager::usersCollectionNamespace ||
            nss == AuthorizationManager::versionCollectionNamespace);
}

bool isAuthzCollection(StringData coll) {
    return (coll == AuthorizationManager::rolesCollectionNamespace.coll() ||
            coll == AuthorizationManager::usersCollectionNamespace.coll() ||
            coll == AuthorizationManager::versionCollectionNamespace.coll());
}

bool loggedCommandOperatesOnAuthzData(const NamespaceString& nss, const BSONObj& cmdObj) {
    if (nss != AuthorizationManager::adminCommandNamespace)
        return false;
    const StringData cmdName(cmdObj.firstElement().fieldNameStringData());
    if (cmdName == "drop") {
        return isAuthzCollection(cmdObj.firstElement().valueStringData());
    } else if (cmdName == "dropDatabase") {
        return true;
    } else if (cmdName == "renameCollection") {
        return isAuthzCollection(cmdObj.firstElement().str()) ||
            isAuthzCollection(cmdObj["to"].str());
    } else if (cmdName == "dropIndexes" || cmdName == "deleteIndexes") {
        return false;
    } else if (cmdName == "create") {
        return false;
    } else {
        return true;
    }
}

bool appliesToAuthzData(const char* op, const NamespaceString& nss, const BSONObj& o) {
    switch (*op) {
        case 'i':
        case 'u':
        case 'd':
            if (op[1] != '\0')
                return false;  // "db" op type
            return isAuthzNamespace(nss);
        case 'c':
            return loggedCommandOperatesOnAuthzData(nss, o);
            break;
        case 'n':
            return false;
        default:
            return true;
    }
}

// Updates to users in the oplog are done by matching on the _id, which will always have the
// form "<dbname>.<username>".  This function extracts the UserName from that string.
StatusWith<UserName> extractUserNameFromIdString(StringData idstr) {
    size_t splitPoint = idstr.find('.');
    if (splitPoint == string::npos) {
        return StatusWith<UserName>(ErrorCodes::FailedToParse,
                                    mongoutils::str::stream()
                                        << "_id entries for user documents must be of "
                                           "the form <dbname>.<username>.  Found: "
                                        << idstr);
    }
    return StatusWith<UserName>(
        UserName(idstr.substr(splitPoint + 1), idstr.substr(0, splitPoint)));
}

}  // namespace

void AuthorizationManager::_updateCacheGeneration_inlock() {
    _cacheGeneration = OID::gen();
}

void AuthorizationManager::_invalidateRelevantCacheData(const char* op,
                                                        const NamespaceString& ns,
                                                        const BSONObj& o,
                                                        const BSONObj* o2) {
    if (ns == AuthorizationManager::rolesCollectionNamespace ||
        ns == AuthorizationManager::versionCollectionNamespace) {
        invalidateUserCache();
        return;
    }

    if (*op == 'i' || *op == 'd' || *op == 'u') {
        // If you got into this function isAuthzNamespace() must have returned true, and we've
        // already checked that it's not the roles or version collection.
        invariant(ns == AuthorizationManager::usersCollectionNamespace);

        StatusWith<UserName> userName = (*op == 'u')
            ? extractUserNameFromIdString((*o2)["_id"].str())
            : extractUserNameFromIdString(o["_id"].str());

        if (!userName.isOK()) {
            warning() << "Invalidating user cache based on user being updated failed, will "
                         "invalidate the entire cache instead: "
                      << userName.getStatus();
            invalidateUserCache();
            return;
        }
        invalidateUserByName(userName.getValue());
    } else {
        invalidateUserCache();
    }
}

void AuthorizationManager::logOp(OperationContext* opCtx,
                                 const char* op,
                                 const NamespaceString& nss,
                                 const BSONObj& o,
                                 const BSONObj* o2) {
    if (appliesToAuthzData(op, nss, o)) {
        _externalState->logOp(opCtx, op, nss, o, o2);
        _invalidateRelevantCacheData(op, nss, o, o2);
    }
}

}  // namespace mongo
