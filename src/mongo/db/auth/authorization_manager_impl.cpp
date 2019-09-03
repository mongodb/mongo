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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kAccessControl

#include "mongo/platform/basic.h"

#include "mongo/db/auth/authorization_manager_impl.h"

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
#include "mongo/db/auth/authorization_manager_impl_parameters_gen.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/authorization_session_impl.h"
#include "mongo/db/auth/authz_manager_external_state.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/auth/privilege_parser.h"
#include "mongo/db/auth/role_graph.h"
#include "mongo/db/auth/sasl_options.h"
#include "mongo/db/auth/user.h"
#include "mongo/db/auth/user_document_parser.h"
#include "mongo/db/auth/user_management_commands_parser.h"
#include "mongo/db/auth/user_name.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/global_settings.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/mongod_options.h"
#include "mongo/platform/compiler.h"
#include "mongo/stdx/memory.h"
#include "mongo/stdx/mutex.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/log.h"
#include "mongo/util/str.h"

namespace mongo {
namespace {

using std::back_inserter;
using std::begin;
using std::end;
using std::endl;
using std::string;
using std::vector;

MONGO_INITIALIZER_GENERAL(SetupInternalSecurityUser,
                          ("EndStartupOptionStorage"),
                          ("CreateAuthorizationManager"))
(InitializerContext* const context) try {
    UserHandle user = std::make_shared<User>(UserName("__system", "local"));

    ActionSet allActions;
    allActions.addAllActions();
    PrivilegeVector privileges;
    RoleGraph::generateUniversalPrivileges(&privileges);
    user->addPrivileges(privileges);

    if (mongodGlobalParams.whitelistedClusterNetwork) {
        const auto& whitelist = *mongodGlobalParams.whitelistedClusterNetwork;

        auto restriction = std::make_unique<ClientSourceRestriction>(whitelist);
        auto restrictionSet = std::make_unique<RestrictionSet<>>(std::move(restriction));
        auto restrictionDocument =
            std::make_unique<RestrictionDocument<>>(std::move(restrictionSet));

        RestrictionDocuments clusterWhiteList(std::move(restrictionDocument));

        user->setRestrictions(std::move(clusterWhiteList));
    }


    internalSecurity.user = user;

    return Status::OK();
} catch (...) {
    return exceptionToStatus();
}

class PinnedUserSetParameter {
public:
    void append(OperationContext* opCtx, BSONObjBuilder& b, const std::string& name) const {
        BSONArrayBuilder sub(b.subarrayStart(name));
        stdx::lock_guard<stdx::mutex> lk(_mutex);
        for (const auto& username : _pinnedUsersList) {
            BSONObjBuilder nameObj(sub.subobjStart());
            nameObj << AuthorizationManager::USER_NAME_FIELD_NAME << username.getUser()
                    << AuthorizationManager::USER_DB_FIELD_NAME << username.getDB();
        }
    }

    Status set(const BSONElement& newValueElement) {
        if (newValueElement.type() == String) {
            return setFromString(newValueElement.valuestrsafe());
        } else if (newValueElement.type() == Array) {
            auto array = static_cast<BSONArray>(newValueElement.embeddedObject());
            std::vector<UserName> out;
            auto status = auth::parseUserNamesFromBSONArray(array, "", &out);
            if (!status.isOK())
                return status;

            status = _checkForSystemUser(out);
            if (!status.isOK()) {
                return status;
            }

            stdx::lock_guard<stdx::mutex> lk(_mutex);
            _pinnedUsersList = out;
            auto authzManager = _authzManager;
            if (!authzManager) {
                return Status::OK();
            }

            authzManager->updatePinnedUsersList(std::move(out));
            return Status::OK();
        } else {
            return {ErrorCodes::BadValue,
                    "authorizationManagerPinnedUsers must be either a string or a BSON array"};
        }
    }

    Status setFromString(const std::string& str) {
        std::vector<std::string> strList;
        str::splitStringDelim(str, &strList, ',');

        std::vector<UserName> out;
        for (const auto& nameStr : strList) {
            auto swUserName = UserName::parse(nameStr);
            if (!swUserName.isOK()) {
                return swUserName.getStatus();
            }
            out.push_back(std::move(swUserName.getValue()));
        }

        auto status = _checkForSystemUser(out);
        if (!status.isOK()) {
            return status;
        }

        stdx::lock_guard<stdx::mutex> lk(_mutex);
        _pinnedUsersList = out;
        auto authzManager = _authzManager;
        if (!authzManager) {
            return Status::OK();
        }

        authzManager->updatePinnedUsersList(std::move(out));
        return Status::OK();
    }

    void setAuthzManager(AuthorizationManager* authzManager) {
        stdx::lock_guard<stdx::mutex> lk(_mutex);
        _authzManager = authzManager;
        _authzManager->updatePinnedUsersList(std::move(_pinnedUsersList));
    }

private:
    Status _checkForSystemUser(const std::vector<UserName>& names) {
        if (std::any_of(names.begin(), names.end(), [&](const UserName& userName) {
                return (userName == internalSecurity.user->getName());
            })) {
            return {ErrorCodes::BadValue,
                    "Cannot set __system as a pinned user, it is always pinned"};
        }
        return Status::OK();
    }
    AuthorizationManager* _authzManager = nullptr;

    mutable stdx::mutex _mutex;
    std::vector<UserName> _pinnedUsersList;
} authorizationManagerPinnedUsers;

}  // namespace

int authorizationManagerCacheSize;

void AuthorizationManagerPinnedUsersServerParameter::append(OperationContext* opCtx,
                                                            BSONObjBuilder& out,
                                                            const std::string& name) {
    return authorizationManagerPinnedUsers.append(opCtx, out, name);
}

Status AuthorizationManagerPinnedUsersServerParameter::set(const BSONElement& newValue) {
    return authorizationManagerPinnedUsers.set(newValue);
}

Status AuthorizationManagerPinnedUsersServerParameter::setFromString(const std::string& str) {
    return authorizationManagerPinnedUsers.setFromString(str);
}

MONGO_REGISTER_SHIM(AuthorizationManager::create)()->std::unique_ptr<AuthorizationManager> {
    return std::make_unique<AuthorizationManagerImpl>();
}

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
 */
class AuthorizationManagerImpl::CacheGuard {
    CacheGuard(const CacheGuard&) = delete;
    CacheGuard& operator=(const CacheGuard&) = delete;

public:
    /**
     * Constructs a cache guard, locking the mutex that synchronizes user cache accesses.
     */
    explicit CacheGuard(OperationContext* opCtx, AuthorizationManagerImpl* authzManager)
        : _isThisGuardInFetchPhase(false),
          _authzManager(authzManager),
          _cacheLock(authzManager->_cacheWriteMutex) {}

    /**
     * Releases the mutex that synchronizes user cache access, if held, and notifies
     * any threads waiting for their own opportunity to update the user cache.
     */
    ~CacheGuard() {
        if (!_cacheLock.owns_lock()) {
            _cacheLock.lock();
        }
        if (_isThisGuardInFetchPhase) {
            fassert(17190, otherUpdateInFetchPhase());
            _authzManager->_isFetchPhaseBusy = false;
            _authzManager->_fetchPhaseIsReady.notify_all();
        }
    }

    /**
     * Returns true of the authzManager reports that it is in fetch phase.
     */
    bool otherUpdateInFetchPhase() const {
        return _authzManager->_isFetchPhaseBusy;
    }

    /**
     * Waits on the _authzManager->_fetchPhaseIsReady condition.
     */
    void wait() {
        fassert(17222, !_isThisGuardInFetchPhase);
        _authzManager->_fetchPhaseIsReady.wait(_cacheLock,
                                               [&] { return !otherUpdateInFetchPhase(); });
    }

    /**
     * Enters fetch phase, releasing the _authzManager->_cacheMutex after recording the current
     * cache generation.
     */
    void beginFetchPhase() {
        fassert(17191, !otherUpdateInFetchPhase());
        _isThisGuardInFetchPhase = true;
        _authzManager->_isFetchPhaseBusy = true;
        _startGeneration = _authzManager->_fetchGeneration;
        _cacheLock.unlock();
    }

    /**
     * Exits the fetch phase, reacquiring the _authzManager->_cacheMutex.
     */
    void endFetchPhase() {
        _cacheLock.lock();
        // We do not clear _authzManager->_isFetchPhaseBusy or notify waiters until
        // ~CacheGuard(), for two reasons.  First, there's no value to notifying the waiters
        // before you're ready to release the mutex, because they'll just go to sleep on the
        // mutex.  Second, in order to meaningfully check the preconditions of
        // isSameCacheGeneration(), we need a state that means "fetch phase was entered and now
        // has been exited."  That state is _isThisGuardInFetchPhase == true and
        // _lock.owns_lock() == true.
    }

    /**
     * Returns true if _authzManager->_fetchGeneration remained the same while this guard was
     * in fetch phase.  Behavior is undefined if this guard never entered fetch phase.
     *
     * If this returns true, do not update the cached data with this
     */
    bool isSameCacheGeneration() const {
        fassert(17223, _isThisGuardInFetchPhase);
        fassert(17231, _cacheLock.owns_lock());
        return _startGeneration == _authzManager->_fetchGeneration;
    }

private:
    OID _startGeneration;
    bool _isThisGuardInFetchPhase;
    AuthorizationManagerImpl* _authzManager;

    stdx::unique_lock<stdx::mutex> _cacheLock;
};

AuthorizationManagerImpl::AuthorizationManagerImpl()
    : AuthorizationManagerImpl(AuthzManagerExternalState::create(),
                               InstallMockForTestingOrAuthImpl{}) {}

AuthorizationManagerImpl::AuthorizationManagerImpl(
    std::unique_ptr<AuthzManagerExternalState> externalState, InstallMockForTestingOrAuthImpl)
    : _authEnabled(false),
      _privilegeDocsExist(false),
      _externalState(std::move(externalState)),
      _version(schemaVersionInvalid),
      _userCache(authorizationManagerCacheSize, UserCacheInvalidator()),
      _fetchGeneration(OID::gen()) {}

AuthorizationManagerImpl::~AuthorizationManagerImpl() {}

std::unique_ptr<AuthorizationSession> AuthorizationManagerImpl::makeAuthorizationSession() {
    return std::make_unique<AuthorizationSessionImpl>(
        _externalState->makeAuthzSessionExternalState(this),
        AuthorizationSessionImpl::InstallMockForTestingOrAuthImpl{});
}

void AuthorizationManagerImpl::setShouldValidateAuthSchemaOnStartup(bool validate) {
    _startupAuthSchemaValidation = validate;
}

bool AuthorizationManagerImpl::shouldValidateAuthSchemaOnStartup() {
    return _startupAuthSchemaValidation;
}

Status AuthorizationManagerImpl::getAuthorizationVersion(OperationContext* opCtx, int* version) {
    CacheGuard guard(opCtx, this);
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

OID AuthorizationManagerImpl::getCacheGeneration() {
    stdx::lock_guard<stdx::mutex> lk(_cacheWriteMutex);
    return _fetchGeneration;
}

void AuthorizationManagerImpl::setAuthEnabled(bool enabled) {
    _authEnabled = enabled;
}

bool AuthorizationManagerImpl::isAuthEnabled() const {
    return _authEnabled;
}

bool AuthorizationManagerImpl::hasAnyPrivilegeDocuments(OperationContext* opCtx) {
    if (_privilegeDocsExist.load()) {
        // If we know that a user exists, don't re-check.
        return true;
    }

    bool privDocsExist = _externalState->hasAnyPrivilegeDocuments(opCtx);

    if (privDocsExist) {
        _privilegeDocsExist.store(true);
    }

    return _privilegeDocsExist.load();
}

Status AuthorizationManagerImpl::_initializeUserFromPrivilegeDocument(User* user,
                                                                      const BSONObj& privDoc) {
    V2UserDocumentParser parser;
    std::string userName = parser.extractUserNameFromUserDocument(privDoc);
    if (userName != user->getName().getUser()) {
        return Status(ErrorCodes::BadValue,
                      str::stream() << "User name from privilege document \"" << userName
                                    << "\" doesn't match name of provided User \""
                                    << user->getName().getUser() << "\"");
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

Status AuthorizationManagerImpl::getUserDescription(OperationContext* opCtx,
                                                    const UserName& userName,
                                                    BSONObj* result) {
    return _externalState->getUserDescription(opCtx, userName, result);
}

Status AuthorizationManagerImpl::getRoleDescription(OperationContext* opCtx,
                                                    const RoleName& roleName,
                                                    PrivilegeFormat privileges,
                                                    AuthenticationRestrictionsFormat restrictions,
                                                    BSONObj* result) {
    return _externalState->getRoleDescription(opCtx, roleName, privileges, restrictions, result);
}

Status AuthorizationManagerImpl::getRolesDescription(OperationContext* opCtx,
                                                     const std::vector<RoleName>& roleName,
                                                     PrivilegeFormat privileges,
                                                     AuthenticationRestrictionsFormat restrictions,
                                                     BSONObj* result) {
    return _externalState->getRolesDescription(opCtx, roleName, privileges, restrictions, result);
}


Status AuthorizationManagerImpl::getRoleDescriptionsForDB(
    OperationContext* opCtx,
    StringData dbname,
    PrivilegeFormat privileges,
    AuthenticationRestrictionsFormat restrictions,
    bool showBuiltinRoles,
    vector<BSONObj>* result) {
    return _externalState->getRoleDescriptionsForDB(
        opCtx, dbname, privileges, restrictions, showBuiltinRoles, result);
}

void AuthorizationManagerImpl::UserCacheInvalidator::operator()(User* user) {
    LOG(1) << "Invalidating user " << user->getName().toString();
    user->_invalidate();
}

StatusWith<UserHandle> AuthorizationManagerImpl::acquireUser(OperationContext* opCtx,
                                                             const UserName& userName) {
    if (userName == internalSecurity.user->getName()) {
        return internalSecurity.user;
    }

    boost::optional<UserHandle> cachedUser = _userCache.get(userName);
    auto returnUser = [&](boost::optional<UserHandle> cachedUser) {
        auto ret = *cachedUser;
        fassert(16914, ret.get());

        LOG(1) << "Returning user " << userName << " from cache";
        return ret;
    };

    // The userCache is thread-safe, so if we can get a user out of the cache we don't need to
    // take any locks!
    if (cachedUser) {
        return returnUser(cachedUser);
    }

    // Otherwise make sure we have the locks we need and check whether and wait on another
    // thread is fetching into the cache
    CacheGuard guard(opCtx, this);

    while ((boost::none == (cachedUser = _userCache.get(userName))) &&
           guard.otherUpdateInFetchPhase()) {
        guard.wait();
    }

    if (cachedUser != boost::none) {
        return returnUser(cachedUser);
    }

    guard.beginFetchPhase();
    // If there's still no user in the cache, then we need to go to disk. Take the slow path.
    LOG(1) << "Getting user " << userName << " from disk";

    int authzVersion = _version;

    // Number of times to retry a user document that fetches due to transient
    // AuthSchemaIncompatible errors.  These errors should only ever occur during and shortly
    // after schema upgrades.
    static const int maxAcquireRetries = 2;
    Status status = Status::OK();
    std::unique_ptr<User> user;
    for (int i = 0; i < maxAcquireRetries; ++i) {
        if (authzVersion == schemaVersionInvalid) {
            Status status = _externalState->getStoredAuthorizationVersion(opCtx, &authzVersion);
            if (!status.isOK())
                return status;
        }

        switch (authzVersion) {
            default:
                status =
                    Status(ErrorCodes::BadValue,
                           str::stream() << "Illegal value for authorization data schema version, "
                                         << authzVersion);
                break;
            case schemaVersion28SCRAM:
            case schemaVersion26Final:
            case schemaVersion26Upgrade:
                status = _fetchUserV2(opCtx, userName, &user);
                break;
            case schemaVersion24:
                status =
                    Status(ErrorCodes::AuthSchemaIncompatible,
                           str::stream() << "Authorization data schema version " << schemaVersion24
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

    // All this does is re-acquire the _cacheWriteMutex if we don't hold it already - a caller
    // may also call endFetchPhase() after this returns.
    guard.endFetchPhase();

    UserHandle ret;
    if (guard.isSameCacheGeneration()) {
        if (_version == schemaVersionInvalid)
            _version = authzVersion;
        ret = _userCache.insertOrAssignAndGet(userName, std::move(user));
        _updateCacheGeneration_inlock(guard);
    } else {
        // If the cache generation changed while this thread was in fetch mode, the data
        // associated with the user may now be invalid, so we must mark it as such.  The caller
        // may still opt to use the information for a short while, but not indefinitely.
        user->_invalidate();
        ret = UserHandle(std::move(user));
    }

    return ret;
}

StatusWith<UserHandle> AuthorizationManagerImpl::acquireUserForSessionRefresh(
    OperationContext* opCtx, const UserName& userName, const User::UserId& uid) {
    auto swUserHandle = acquireUser(opCtx, userName);
    if (!swUserHandle.isOK()) {
        return swUserHandle.getStatus();
    }

    auto ret = std::move(swUserHandle.getValue());
    if (uid != ret->getID()) {
        return {ErrorCodes::UserNotFound,
                str::stream() << "User id from privilege document '" << userName.toString()
                              << "' does not match user id in session."};
    }

    return ret;
}

Status AuthorizationManagerImpl::_fetchUserV2(OperationContext* opCtx,
                                              const UserName& userName,
                                              std::unique_ptr<User>* out) {
    BSONObj userObj;
    Status status = getUserDescription(opCtx, userName, &userObj);
    if (!status.isOK()) {
        return status;
    }

    auto user = std::make_unique<User>(userName);

    status = _initializeUserFromPrivilegeDocument(user.get(), userObj);
    if (!status.isOK()) {
        return status;
    }

    std::swap(*out, user);
    return Status::OK();
}

void AuthorizationManagerImpl::updatePinnedUsersList(std::vector<UserName> names) {
    stdx::unique_lock<stdx::mutex> lk(_pinnedUsersMutex);
    _usersToPin = std::move(names);
    bool noUsersToPin = _usersToPin->empty();
    _pinnedUsersCond.notify_one();
    if (noUsersToPin) {
        LOG(1) << "There were no users to pin, not starting tracker thread";
        return;
    }

    std::call_once(_pinnedThreadTrackerStarted, [this] {
        stdx::thread thread(&AuthorizationManagerImpl::_pinnedUsersThreadRoutine, this);
        thread.detach();
    });
}

void AuthorizationManagerImpl::_pinnedUsersThreadRoutine() noexcept try {
    Client::initThread("PinnedUsersTracker");
    std::list<UserHandle> pinnedUsers;
    std::vector<UserName> usersToPin;
    LOG(1) << "Starting pinned users tracking thread";
    while (true) {
        auto opCtx = cc().makeOperationContext();

        stdx::unique_lock<stdx::mutex> lk(_pinnedUsersMutex);
        const Milliseconds timeout(authorizationManagerPinnedUsersRefreshIntervalMillis.load());
        auto waitRes = opCtx->waitForConditionOrInterruptFor(
            _pinnedUsersCond, lk, timeout, [&] { return _usersToPin.has_value(); });

        if (waitRes) {
            usersToPin = std::move(_usersToPin.get());
            _usersToPin = boost::none;
        }
        lk.unlock();
        if (usersToPin.empty()) {
            pinnedUsers.clear();
            continue;
        }

        // Remove any users that shouldn't be pinned anymore or that are invalid.
        for (auto it = pinnedUsers.begin(); it != pinnedUsers.end();) {
            const auto& user = *it;
            const auto shouldPin =
                std::any_of(usersToPin.begin(), usersToPin.end(), [&](const UserName& userName) {
                    return (user->getName() == userName);
                });

            if (!user->isValid() || !shouldPin) {
                if (!shouldPin) {
                    LOG(2) << "Unpinning user " << user->getName();
                } else {
                    LOG(2) << "Pinned user no longer valid, will re-pin " << user->getName();
                }
                it = pinnedUsers.erase(it);
            } else {
                LOG(3) << "Pinned user is still valid and pinned " << user->getName();
                ++it;
            }
        }

        for (const auto& userName : usersToPin) {
            if (std::any_of(pinnedUsers.begin(), pinnedUsers.end(), [&](const auto& user) {
                    return user->getName() == userName;
                })) {
                continue;
            }

            auto swUser = acquireUser(opCtx.get(), userName);

            if (swUser.isOK()) {
                LOG(2) << "Pinned user " << userName;
                pinnedUsers.emplace_back(std::move(swUser.getValue()));
            } else {
                const auto& status = swUser.getStatus();
                // If the user is not found, then it might just not exist yet. Skip this user for
                // now.
                if (status != ErrorCodes::UserNotFound) {
                    warning() << "Unable to fetch pinned user " << userName.toString() << ": "
                              << status;
                } else {
                    LOG(2) << "Pinned user not found: " << userName;
                }
            }
        }
    }
} catch (const ExceptionFor<ErrorCodes::InterruptedAtShutdown>&) {
    LOG(1) << "Ending pinned users tracking thread";
    return;
}

void AuthorizationManagerImpl::invalidateUserByName(OperationContext* opCtx,
                                                    const UserName& userName) {
    CacheGuard guard(opCtx, this);
    _updateCacheGeneration_inlock(guard);
    LOG(2) << "Invalidating user " << userName;
    _userCache.invalidate(userName);
}

void AuthorizationManagerImpl::invalidateUsersFromDB(OperationContext* opCtx, StringData dbname) {
    CacheGuard guard(opCtx, this);
    _updateCacheGeneration_inlock(guard);
    LOG(2) << "Invalidating all users from database " << dbname;
    _userCache.invalidateIf(
        [&](const UserName& user, const User*) { return user.getDB() == dbname; });
}

void AuthorizationManagerImpl::invalidateUserCache(OperationContext* opCtx) {
    CacheGuard guard(opCtx, this);
    LOG(2) << "Invalidating user cache";
    _invalidateUserCache_inlock(guard);
}

void AuthorizationManagerImpl::_invalidateUserCache_inlock(const CacheGuard& guard) {
    _updateCacheGeneration_inlock(guard);
    _userCache.invalidateIf([](const UserName& a, const User*) { return true; });

    // Reread the schema version before acquiring the next user.
    _version = schemaVersionInvalid;
}

Status AuthorizationManagerImpl::initialize(OperationContext* opCtx) {
    Status status = _externalState->initialize(opCtx);
    if (!status.isOK())
        return status;

    authorizationManagerPinnedUsers.setAuthzManager(this);
    invalidateUserCache(opCtx);
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
        const NamespaceString fromNamespace(cmdObj.firstElement().str());
        const NamespaceString toNamespace(cmdObj["to"].str());
        if (fromNamespace.db() == "admin" || toNamespace.db() == "admin") {
            return isAuthzCollection(fromNamespace.coll().toString()) ||
                isAuthzCollection(toNamespace.coll().toString());
        } else {
            return false;
        }

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

}  // namespace

void AuthorizationManagerImpl::_updateCacheGeneration_inlock(const CacheGuard&) {
    _fetchGeneration = OID::gen();
}

void AuthorizationManagerImpl::logOp(OperationContext* opCtx,
                                     const char* op,
                                     const NamespaceString& nss,
                                     const BSONObj& o,
                                     const BSONObj* o2) {
    if (appliesToAuthzData(op, nss, o)) {
        _externalState->logOp(opCtx, this, op, nss, o, o2);
    }
}

std::vector<AuthorizationManager::CachedUserInfo> AuthorizationManagerImpl::getUserCacheInfo()
    const {
    auto cacheData = _userCache.getCacheInfo();
    std::vector<AuthorizationManager::CachedUserInfo> ret;
    ret.reserve(cacheData.size());
    std::transform(
        cacheData.begin(), cacheData.end(), std::back_inserter(ret), [](const auto& info) {
            return AuthorizationManager::CachedUserInfo{info.key, info.active};
        });

    return ret;
}

}  // namespace mongo
