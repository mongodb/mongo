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


#include "mongo/platform/basic.h"

#include "mongo/db/auth/authorization_manager_impl.h"

#include <memory>
#include <string>
#include <vector>

#include "mongo/base/init.h"
#include "mongo/base/shim.h"
#include "mongo/base/status.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/config.h"
#include "mongo/crypto/mechanism_scram.h"
#include "mongo/db/auth/address_restriction.h"
#include "mongo/db/auth/auth_types_gen.h"
#include "mongo/db/auth/authorization_manager_global_parameters_gen.h"
#include "mongo/db/auth/authorization_manager_impl_parameters_gen.h"
#include "mongo/db/auth/authorization_session_impl.h"
#include "mongo/db/auth/authz_manager_external_state.h"
#include "mongo/db/auth/sasl_options.h"
#include "mongo/db/auth/user_document_parser.h"
#include "mongo/db/auth/user_management_commands_parser.h"
#include "mongo/db/curop.h"
#include "mongo/db/global_settings.h"
#include "mongo/db/mongod_options.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/net/ssl_peer_info.h"
#include "mongo/util/net/ssl_types.h"
#include "mongo/util/str.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kAccessControl


namespace mongo {
namespace {

std::shared_ptr<UserHandle> createSystemUserHandle() {
    auto user = std::make_shared<UserHandle>(User(UserName("__system", "local")));

    ActionSet allActions;
    allActions.addAllActions();
    PrivilegeVector privileges;
    auth::generateUniversalPrivileges(&privileges);
    (*user)->addPrivileges(privileges);

    if (internalSecurity.credentials) {
        (*user)->setCredentials(internalSecurity.credentials.value());
    }

    return user;
}

class ClusterNetworkRestrictionManagerImpl : public ClusterNetworkRestrictionManager {
public:
    static void configureRestrictions(std::shared_ptr<UserHandle> user) {
        const auto allowlistedClusterNetwork =
            std::atomic_load(&mongodGlobalParams.allowlistedClusterNetwork);  // NOLINT
        if (allowlistedClusterNetwork) {
            auto restriction =
                std::make_unique<ClientSourceRestriction>(*allowlistedClusterNetwork);
            auto restrictionSet = std::make_unique<RestrictionSet<>>(std::move(restriction));
            auto restrictionDocument =
                std::make_unique<RestrictionDocument<>>(std::move(restrictionSet));

            RestrictionDocuments clusterAllowList(std::move(restrictionDocument));
            (*user)->setRestrictions(clusterAllowList);
        }
    }

    void updateClusterNetworkRestrictions() override {
        auto user = createSystemUserHandle();
        configureRestrictions(user);
        auto originalUser = internalSecurity.setUser(user);
        (*originalUser)->invalidate();
    }
};

MONGO_INITIALIZER_GENERAL(SetupInternalSecurityUser,
                          ("EndStartupOptionStorage"),
                          ("CreateAuthorizationManager"))
(InitializerContext* const context) try {
    auto user = createSystemUserHandle();
    ClusterNetworkRestrictionManagerImpl::configureRestrictions(user);
    internalSecurity.setUser(user);
} catch (...) {
    uassertStatusOK(exceptionToStatus());
}

ServiceContext::ConstructorActionRegisterer setClusterNetworkRestrictionManager{
    "SetClusterNetworkRestrictionManager", [](ServiceContext* service) {
        std::unique_ptr<ClusterNetworkRestrictionManager> manager =
            std::make_unique<ClusterNetworkRestrictionManagerImpl>();
        ClusterNetworkRestrictionManager::set(service, std::move(manager));
    }};

class PinnedUserSetParameter {
public:
    void append(BSONObjBuilder& b, const std::string& name) const {
        BSONArrayBuilder sub(b.subarrayStart(name));
        stdx::lock_guard<Latch> lk(_mutex);
        for (const auto& username : _pinnedUsersList) {
            BSONObjBuilder nameObj(sub.subobjStart());
            nameObj << AuthorizationManager::USER_NAME_FIELD_NAME << username.getUser()
                    << AuthorizationManager::USER_DB_FIELD_NAME << username.getDB();
        }
    }

    Status set(const BSONElement& newValueElement) {
        if (newValueElement.type() == String) {
            return setFromString(newValueElement.str());
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

            stdx::lock_guard<Latch> lk(_mutex);
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

    Status setFromString(StringData str) {
        std::vector<std::string> strList;
        str::splitStringDelim(str.toString(), &strList, ',');

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

        stdx::lock_guard<Latch> lk(_mutex);
        _pinnedUsersList = out;
        auto authzManager = _authzManager;
        if (!authzManager) {
            return Status::OK();
        }

        authzManager->updatePinnedUsersList(std::move(out));
        return Status::OK();
    }

    void setAuthzManager(AuthorizationManager* authzManager) {
        stdx::lock_guard<Latch> lk(_mutex);
        _authzManager = authzManager;
        _authzManager->updatePinnedUsersList(std::move(_pinnedUsersList));
    }

private:
    Status _checkForSystemUser(const std::vector<UserName>& names) {
        if (std::any_of(names.begin(), names.end(), [&](const UserName& userName) {
                return (userName == (*internalSecurity.getUser())->getName());
            })) {
            return {ErrorCodes::BadValue,
                    "Cannot set __system as a pinned user, it is always pinned"};
        }
        return Status::OK();
    }

    mutable Mutex _mutex = MONGO_MAKE_LATCH("PinnedUserSetParameter::_mutex");

    AuthorizationManager* _authzManager = nullptr;

    std::vector<UserName> _pinnedUsersList;

} authorizationManagerPinnedUsers;

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
        const NamespaceString fromNamespace(cmdObj.firstElement().valueStringDataSafe());
        const NamespaceString toNamespace(cmdObj.getStringField("to"));

        if (fromNamespace.isAdminDB() || toNamespace.isAdminDB()) {
            return isAuthzCollection(fromNamespace.coll()) || isAuthzCollection(toNamespace.coll());
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

bool appliesToAuthzData(StringData op, const NamespaceString& nss, const BSONObj& o) {
    if (op.empty()) {
        return true;
    }

    switch (op[0]) {
        case 'i':
        case 'u':
        case 'd':
            if (op.size() != 1) {
                return false;  // "db" op type
            }
            return isAuthzNamespace(nss);
        case 'c':
            return loggedCommandOperatesOnAuthzData(nss, o);
        case 'n':
            return false;
        default:
            return true;
    }
}

/**
 * Returns true if roles for this user were provided by the client, and can be obtained from
 * the connection.
 */
bool shouldUseRolesFromConnection(OperationContext* opCtx, const UserName& userName) {
#ifdef MONGO_CONFIG_SSL
    if (!opCtx || !opCtx->getClient() || !opCtx->getClient()->session()) {
        return false;
    }

    if (!allowRolesFromX509Certificates) {
        return false;
    }

    auto& sslPeerInfo = SSLPeerInfo::forSession(opCtx->getClient()->session());
    return sslPeerInfo.subjectName.toString() == userName.getUser() &&
        userName.getDB() == "$external"_sd && !sslPeerInfo.roles.empty();
#else
    return false;
#endif
}


std::unique_ptr<AuthorizationManager> authorizationManagerCreateImpl(
    ServiceContext* serviceContext) {
    return std::make_unique<AuthorizationManagerImpl>(serviceContext,
                                                      AuthzManagerExternalState::create());
}

auto authorizationManagerCreateRegistration =
    MONGO_WEAK_FUNCTION_REGISTRATION(AuthorizationManager::create, authorizationManagerCreateImpl);

MONGO_FAIL_POINT_DEFINE(waitForUserCacheInvalidation);
void handleWaitForUserCacheInvalidation(OperationContext* opCtx, const UserHandle& user) {
    auto fp = waitForUserCacheInvalidation.scopedIf([&](const auto& bsonData) {
        IDLParserContext ctx("waitForUserCacheInvalidation");
        auto data = WaitForUserCacheInvalidationFailPoint::parse(ctx, bsonData);

        const auto& blockedUserName = data.getUserName();
        return blockedUserName == user->getName();
    });

    if (!fp.isActive()) {
        return;
    }

    // Since we do not have notifications from both the user cache and the fail point itself,
    // loop until our condition is satisfied. To avoid this loop, we would need a way to union
    // notifications from both. This may be possible with a CancellationToken and a condition
    // variable or with some novel Notifiable/Waitable-like type that synthesizes multiple
    // notifications.
    constexpr auto kCheckPeriod = Milliseconds{1};
    auto m = MONGO_MAKE_LATCH();
    auto cv = stdx::condition_variable{};
    auto pred = [&] { return !fp.isStillEnabled() || !user.isValid(); };
    auto waitOneCycle = [&] {
        auto lk = stdx::unique_lock(m);
        return !opCtx->waitForConditionOrInterruptFor(cv, lk, kCheckPeriod, pred);
    };

    while (waitOneCycle()) {
        // Not yet finished.
    }
}


}  // namespace

int authorizationManagerCacheSize;

void AuthorizationManagerPinnedUsersServerParameter::append(OperationContext* opCtx,
                                                            BSONObjBuilder* out,
                                                            StringData name,
                                                            const boost::optional<TenantId>&) {
    return authorizationManagerPinnedUsers.append(*out, name.toString());
}

Status AuthorizationManagerPinnedUsersServerParameter::set(const BSONElement& newValue,
                                                           const boost::optional<TenantId>&) {
    return authorizationManagerPinnedUsers.set(newValue);
}

Status AuthorizationManagerPinnedUsersServerParameter::setFromString(
    StringData str, const boost::optional<TenantId>&) {
    return authorizationManagerPinnedUsers.setFromString(str);
}

AuthorizationManagerImpl::AuthorizationManagerImpl(
    ServiceContext* service, std::unique_ptr<AuthzManagerExternalState> externalState)
    : _externalState(std::move(externalState)),
      _authSchemaVersionCache(service, _threadPool, _externalState.get()),
      _userCache(service,
                 _threadPool,
                 authorizationManagerCacheSize,
                 &_authSchemaVersionCache,
                 _externalState.get()),
      _threadPool([] {
          ThreadPool::Options options;
          options.poolName = "AuthorizationManager";
          options.minThreads = 0;
          options.maxThreads = ThreadPool::Options::kUnlimited;

          return options;
      }()) {
    _threadPool.startup();
}

AuthorizationManagerImpl::~AuthorizationManagerImpl() = default;

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

Status AuthorizationManagerImpl::getAuthorizationVersion(OperationContext* opCtx,
                                                         int* version) try {
    *version = *_authSchemaVersionCache.acquire(opCtx, 0);
    return Status::OK();
} catch (const DBException& ex) {
    return ex.toStatus();
}

OID AuthorizationManagerImpl::getCacheGeneration() {
    stdx::lock_guard lg(_cacheGenerationMutex);
    return _cacheGeneration;
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

Status AuthorizationManagerImpl::getUserDescription(OperationContext* opCtx,
                                                    const UserName& userName,
                                                    BSONObj* result) {
    return _externalState->getUserDescription(opCtx, UserRequest(userName, boost::none), result);
}

Status AuthorizationManagerImpl::rolesExist(OperationContext* opCtx,
                                            const std::vector<RoleName>& roleNames) {
    return _externalState->rolesExist(opCtx, roleNames);
}

StatusWith<AuthorizationManager::ResolvedRoleData> AuthorizationManagerImpl::resolveRoles(
    OperationContext* opCtx, const std::vector<RoleName>& roleNames, ResolveRoleOption option) {
    return _externalState->resolveRoles(opCtx, roleNames, option);
}

Status AuthorizationManagerImpl::getRolesDescription(OperationContext* opCtx,
                                                     const std::vector<RoleName>& roleName,
                                                     PrivilegeFormat privileges,
                                                     AuthenticationRestrictionsFormat restrictions,
                                                     std::vector<BSONObj>* result) {
    return _externalState->getRolesDescription(opCtx, roleName, privileges, restrictions, result);
}


Status AuthorizationManagerImpl::getRolesAsUserFragment(
    OperationContext* opCtx,
    const std::vector<RoleName>& roleName,
    AuthenticationRestrictionsFormat restrictions,
    BSONObj* result) {
    return _externalState->getRolesAsUserFragment(opCtx, roleName, restrictions, result);
}


Status AuthorizationManagerImpl::getRoleDescriptionsForDB(
    OperationContext* opCtx,
    const DatabaseName& dbname,
    PrivilegeFormat privileges,
    AuthenticationRestrictionsFormat restrictions,
    bool showBuiltinRoles,
    std::vector<BSONObj>* result) {
    return _externalState->getRoleDescriptionsForDB(
        opCtx, dbname, privileges, restrictions, showBuiltinRoles, result);
}

namespace {
MONGO_FAIL_POINT_DEFINE(authUserCacheBypass);
MONGO_FAIL_POINT_DEFINE(authUserCacheSleep);
}  // namespace

StatusWith<UserHandle> AuthorizationManagerImpl::acquireUser(OperationContext* opCtx,
                                                             const UserName& userName) try {
    auto systemUser = internalSecurity.getUser();
    if (userName == (*systemUser)->getName()) {
        return *systemUser;
    }

    UserRequest request(userName, boost::none);

#ifdef MONGO_CONFIG_SSL
    // Clients connected via TLS may present an X.509 certificate which contains an authorization
    // grant. If this is the case, the roles must be provided to the external state, for expansion
    // into privileges.
    if (shouldUseRolesFromConnection(opCtx, userName)) {
        auto& sslPeerInfo = SSLPeerInfo::forSession(opCtx->getClient()->session());
        request.roles = std::set<RoleName>();

        // In order to be hashable, the role names must be converted from unordered_set to a set.
        std::copy(sslPeerInfo.roles.begin(),
                  sslPeerInfo.roles.end(),
                  std::inserter(*request.roles, request.roles->begin()));
    }
#endif

    if (authUserCacheBypass.shouldFail()) {
        // Bypass cache and force a fresh load of the user.
        auto loadedUser = uassertStatusOK(_externalState->getUserObject(opCtx, request));
        // We have to inject into the cache in order to get a UserHandle.
        auto userHandle =
            _userCache.insertOrAssignAndGet(request, std::move(loadedUser), Date_t::now());
        invariant(userHandle);
        LOGV2_DEBUG(4859401, 1, "Bypassing user cache to load user", "user"_attr = userName);
        return userHandle;
    }

    // Track wait time and user cache access statistics for the current op for logging. An extra
    // second of delay is added via the failpoint for testing.
    UserAcquisitionStatsHandle userAcquisitionStatsHandle =
        UserAcquisitionStatsHandle(CurOp::get(opCtx)->getMutableUserAcquisitionStats(),
                                   opCtx->getServiceContext()->getTickSource(),
                                   kCache);
    if (authUserCacheSleep.shouldFail()) {
        sleepsecs(1);
    }

    auto cachedUser = _userCache.acquire(opCtx, request);

    userAcquisitionStatsHandle.recordTimerEnd();
    invariant(cachedUser);

    LOGV2_DEBUG(20226, 1, "Returning user from cache", "user"_attr = userName);
    return cachedUser;
} catch (const DBException& ex) {
    return ex.toStatus();
}

StatusWith<UserHandle> AuthorizationManagerImpl::reacquireUser(OperationContext* opCtx,
                                                               const UserHandle& user) {
    const UserName& userName = user->getName();
    handleWaitForUserCacheInvalidation(opCtx, user);
    if (user.isValid() && !user->isInvalidated()) {
        return user;
    }

    // Make a good faith effort to acquire an up-to-date user object, since the one
    // we've cached is marked "out-of-date."
    auto swUserHandle = acquireUser(opCtx, userName);
    if (!swUserHandle.isOK()) {
        return swUserHandle.getStatus();
    }

    auto ret = std::move(swUserHandle.getValue());
    if (user->getID() != ret->getID()) {
        return {ErrorCodes::UserNotFound,
                str::stream() << "User id from privilege document '" << userName
                              << "' does not match user id in session."};
    }

    return ret;
}

void AuthorizationManagerImpl::updatePinnedUsersList(std::vector<UserName> names) {
    stdx::unique_lock<Latch> lk(_pinnedUsersMutex);
    _usersToPin = std::move(names);
    bool noUsersToPin = _usersToPin->empty();
    _pinnedUsersCond.notify_one();
    if (noUsersToPin) {
        LOGV2_DEBUG(20227, 1, "There were no users to pin, not starting tracker thread");
        return;
    }

    std::call_once(_pinnedThreadTrackerStarted, [this] {
        stdx::thread thread(&AuthorizationManagerImpl::_pinnedUsersThreadRoutine, this);
        thread.detach();
    });
}

void AuthorizationManagerImpl::_updateCacheGeneration() {
    stdx::lock_guard lg(_cacheGenerationMutex);
    _cacheGeneration = OID::gen();
}

void AuthorizationManagerImpl::_pinnedUsersThreadRoutine() noexcept try {
    Client::initThread("PinnedUsersTracker");
    std::list<UserHandle> pinnedUsers;
    std::vector<UserName> usersToPin;
    LOGV2_DEBUG(20228, 1, "Starting pinned users tracking thread");
    while (true) {
        auto opCtx = cc().makeOperationContext();

        stdx::unique_lock<Latch> lk(_pinnedUsersMutex);
        const Milliseconds timeout(authorizationManagerPinnedUsersRefreshIntervalMillis.load());
        auto waitRes = opCtx->waitForConditionOrInterruptFor(
            _pinnedUsersCond, lk, timeout, [&] { return _usersToPin.has_value(); });

        if (waitRes) {
            usersToPin = std::move(_usersToPin.value());
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

            if (!user.isValid() || !shouldPin) {
                if (!shouldPin) {
                    LOGV2_DEBUG(20229, 2, "Unpinning user", "user"_attr = user->getName());
                } else {
                    LOGV2_DEBUG(20230,
                                2,
                                "Pinned user no longer valid, will re-pin",
                                "user"_attr = user->getName());
                }
                it = pinnedUsers.erase(it);
            } else {
                LOGV2_DEBUG(20231,
                            3,
                            "Pinned user is still valid and pinned",
                            "user"_attr = user->getName());
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
                LOGV2_DEBUG(20232, 2, "Pinned user", "user"_attr = userName);
                pinnedUsers.emplace_back(std::move(swUser.getValue()));
            } else {
                const auto& status = swUser.getStatus();
                // If the user is not found, then it might just not exist yet. Skip this user for
                // now.
                if (status != ErrorCodes::UserNotFound) {
                    LOGV2_WARNING(20239,
                                  "Unable to fetch pinned user",
                                  "user"_attr = userName,
                                  "error"_attr = status);
                } else {
                    LOGV2_DEBUG(20233, 2, "Pinned user not found", "user"_attr = userName);
                }
            }
        }
    }
} catch (const ExceptionFor<ErrorCodes::InterruptedAtShutdown>&) {
    LOGV2_DEBUG(20234, 1, "Ending pinned users tracking thread");
    return;
}

void AuthorizationManagerImpl::invalidateUserByName(OperationContext* opCtx,
                                                    const UserName& userName) {
    LOGV2_DEBUG(20235, 2, "Invalidating user", "user"_attr = userName);
    _updateCacheGeneration();
    _authSchemaVersionCache.invalidateAll();
    // Invalidate the named User, assuming no externally provided roles. When roles are defined
    // externally, there exists no user document which may become invalid.
    _userCache.invalidateKey(UserRequest(userName, boost::none));
}

void AuthorizationManagerImpl::invalidateUsersFromDB(OperationContext* opCtx,
                                                     const DatabaseName& dbname) {
    LOGV2_DEBUG(20236, 2, "Invalidating all users from database", "database"_attr = dbname);
    _updateCacheGeneration();
    _authSchemaVersionCache.invalidateAll();
    _userCache.invalidateKeyIf([&](const UserRequest& userRequest) {
        return userRequest.name.getDatabaseName() == dbname;
    });
}

void AuthorizationManagerImpl::invalidateUsersByTenant(OperationContext* opCtx,
                                                       const boost::optional<TenantId>& tenant) {
    if (!tenant) {
        invalidateUserCache(opCtx);
        return;
    }

    LOGV2_DEBUG(6323600, 2, "Invalidating tenant users", "tenant"_attr = tenant);
    _updateCacheGeneration();
    _authSchemaVersionCache.invalidateAll();
    _userCache.invalidateKeyIf(
        [&](const UserRequest& userRequest) { return userRequest.name.getTenant() == tenant; });
}

void AuthorizationManagerImpl::invalidateUserCache(OperationContext* opCtx) {
    LOGV2_DEBUG(20237, 2, "Invalidating user cache");
    _updateCacheGeneration();
    _authSchemaVersionCache.invalidateAll();
    _userCache.invalidateAll();
}

Status AuthorizationManagerImpl::refreshExternalUsers(OperationContext* opCtx) {
    LOGV2_DEBUG(5914801, 2, "Refreshing all users from the $external database");
    // First, get a snapshot of the UserHandles in the cache.
    auto cachedUsers =
        _userCache.peekLatestCachedIf([&](const UserRequest& userRequest, const User&) {
            return userRequest.name.getDB() == "$external"_sd;
        });

    // Then, retrieve the corresponding Users from the backing store for users in the $external
    // database. Compare each of these user objects with the cached user object and call
    // insertOrAssign if they differ.
    bool isRefreshed{false};
    for (const auto& cachedUser : cachedUsers) {
        UserRequest request(cachedUser->getName(), boost::none);
        auto storedUserStatus = _externalState->getUserObject(opCtx, request);
        if (!storedUserStatus.isOK()) {
            // If the user simply is not found, then just invalidate the cached user and continue.
            if (storedUserStatus.getStatus().code() == ErrorCodes::UserNotFound) {
                _userCache.invalidateKey(request);
                continue;
            } else {
                return storedUserStatus.getStatus();
            }
        }

        if (cachedUser->hasDifferentRoles(storedUserStatus.getValue())) {
            _userCache.insertOrAssign(
                request, std::move(storedUserStatus.getValue()), Date_t::now());
            isRefreshed = true;
        }
    }

    // If any entries were refreshed, then the cache generation must be bumped for mongos to refresh
    // its cache.
    if (isRefreshed) {
        _updateCacheGeneration();
    }

    return Status::OK();
}

Status AuthorizationManagerImpl::initialize(OperationContext* opCtx) {
    Status status = _externalState->initialize(opCtx);
    if (!status.isOK())
        return status;

    authorizationManagerPinnedUsers.setAuthzManager(this);
    invalidateUserCache(opCtx);
    return Status::OK();
}

void AuthorizationManagerImpl::logOp(OperationContext* opCtx,
                                     StringData op,
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
            return AuthorizationManager::CachedUserInfo{info.key.name, info.useCount > 0};
        });

    return ret;
}

AuthorizationManagerImpl::AuthSchemaVersionCache::AuthSchemaVersionCache(
    ServiceContext* service,
    ThreadPoolInterface& threadPool,
    AuthzManagerExternalState* externalState)
    : ReadThroughCache(_mutex,
                       service,
                       threadPool,
                       [this](OperationContext* opCtx, int key, const ValueHandle& cachedValue) {
                           return _lookup(opCtx, key, cachedValue);
                       },
                       1 /* cacheSize */),
      _externalState(externalState) {}

AuthorizationManagerImpl::AuthSchemaVersionCache::LookupResult
AuthorizationManagerImpl::AuthSchemaVersionCache::_lookup(OperationContext* opCtx,
                                                          int unusedKey,
                                                          const ValueHandle& unusedCachedValue) {
    invariant(unusedKey == 0);

    int authzVersion;
    uassertStatusOK(_externalState->getStoredAuthorizationVersion(opCtx, &authzVersion));

    return LookupResult(authzVersion);
}

AuthorizationManagerImpl::UserCacheImpl::UserCacheImpl(
    ServiceContext* service,
    ThreadPoolInterface& threadPool,
    int cacheSize,
    AuthSchemaVersionCache* authSchemaVersionCache,
    AuthzManagerExternalState* externalState)
    : UserCache(_mutex,
                service,
                threadPool,
                [this](OperationContext* opCtx, const UserRequest& userReq, UserHandle cachedUser) {
                    return _lookup(opCtx, userReq, cachedUser);
                },
                cacheSize),
      _authSchemaVersionCache(authSchemaVersionCache),
      _externalState(externalState) {}

AuthorizationManagerImpl::UserCacheImpl::LookupResult
AuthorizationManagerImpl::UserCacheImpl::_lookup(OperationContext* opCtx,
                                                 const UserRequest& userReq,
                                                 const UserHandle& unusedCachedUser) {
    LOGV2_DEBUG(20238, 1, "Getting user record", "user"_attr = userReq.name);

    // Number of times to retry a user document that fetches due to transient AuthSchemaIncompatible
    // errors. These errors should only ever occur during and shortly after schema upgrades.
    int acquireAttemptsLeft = 2;

    while (true) {
        const int authzVersion = [&] {
            auto authSchemaVersionHandle = _authSchemaVersionCache->acquire(opCtx, 0);
            invariant(authSchemaVersionHandle);
            return *authSchemaVersionHandle;
        }();

        switch (authzVersion) {
            case schemaVersion28SCRAM:
            case schemaVersion26Final:
            case schemaVersion26Upgrade:
                return LookupResult(uassertStatusOK(_externalState->getUserObject(opCtx, userReq)));
            case schemaVersion24:
                _authSchemaVersionCache->invalidateAll();

                uassert(ErrorCodes::AuthSchemaIncompatible,
                        str::stream() << "Authorization data schema version " << schemaVersion24
                                      << " not supported after MongoDB version 2.6.",
                        --acquireAttemptsLeft);
                break;
            default:
                uasserted(ErrorCodes::BadValue,
                          str::stream() << "Illegal value for authorization data schema version, "
                                        << authzVersion);
                break;
        }
    }

    MONGO_UNREACHABLE;
}

}  // namespace mongo
