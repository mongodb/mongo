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


#include <absl/container/node_hash_map.h>
#include <algorithm>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>
#include <iterator>
#include <list>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "mongo/base/error_codes.h"
#include "mongo/base/init.h"  // IWYU pragma: keep
#include "mongo/base/initializer.h"
#include "mongo/base/status.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/config.h"  // IWYU pragma: keep
#include "mongo/db/auth/action_set.h"
#include "mongo/db/auth/address_restriction.h"
#include "mongo/db/auth/auth_name.h"
#include "mongo/db/auth/auth_types_gen.h"
#include "mongo/db/auth/authorization_manager_impl.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/authorization_session_impl.h"
#include "mongo/db/auth/authz_manager_external_state.h"
#include "mongo/db/auth/builtin_roles.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/auth/restriction_set.h"
#include "mongo/db/commands/authentication_commands.h"
#include "mongo/db/curop.h"
#include "mongo/db/global_settings.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_attr.h"
#include "mongo/logv2/log_component.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/duration.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/future.h"
#include "mongo/util/str.h"
#include "mongo/util/time_support.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kAccessControl


namespace mongo {
namespace {

std::shared_ptr<UserHandle> createSystemUserHandle() {
    UserRequest request(UserName("__system", "local"), boost::none);
    auto user = std::make_shared<UserHandle>(User(std::move(request)));

    ActionSet allActions;
    allActions.addAllActions();
    PrivilegeVector privileges;
    auth::generateUniversalPrivileges(&privileges, boost::none /* tenantId */);
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

bool isAuthzNamespace(const NamespaceString& nss) {
    return (nss == NamespaceString::kAdminRolesNamespace ||
            nss == NamespaceString::kAdminUsersNamespace ||
            nss == NamespaceString::kServerConfigurationNamespace);
}

bool isAuthzCollection(StringData coll) {
    return (coll == NamespaceString::kAdminRolesNamespace.coll() ||
            coll == NamespaceString::kAdminUsersNamespace.coll() ||
            coll == NamespaceString::kServerConfigurationNamespace.coll());
}

bool loggedCommandOperatesOnAuthzData(const NamespaceString& nss, const BSONObj& cmdObj) {
    if (nss != NamespaceString::kAdminCommandNamespace)
        return false;

    const StringData cmdName(cmdObj.firstElement().fieldNameStringData());

    if (cmdName == "drop") {
        return isAuthzCollection(cmdObj.firstElement().valueStringData());
    } else if (cmdName == "dropDatabase") {
        return true;
    } else if (cmdName == "renameCollection") {
        auto context = SerializationContext::stateStorageRequest();

        const NamespaceString fromNamespace = NamespaceStringUtil::deserialize(
            nss.tenantId(), cmdObj.firstElement().valueStringDataSafe(), context);
        const NamespaceString toNamespace =
            NamespaceStringUtil::deserialize(nss.tenantId(), cmdObj.getStringField("to"), context);

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
    auto pred = [&] {
        return !fp.isStillEnabled() || !user.isValid();
    };
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

AuthorizationManagerImpl::AuthorizationManagerImpl(
    Service* service, std::unique_ptr<AuthzManagerExternalState> externalState)
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
    invariant(_externalState != nullptr);
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
    return _externalState->getUserDescription(opCtx,
                                              UserRequest(userName, boost::none),
                                              result,
                                              CurOp::get(opCtx)->getUserAcquisitionStats());
}

Status AuthorizationManagerImpl::hasValidAuthSchemaVersionDocumentForInitialSync(
    OperationContext* opCtx) {
    BSONObj foundDoc;
    auto status = _externalState->hasValidStoredAuthorizationVersion(opCtx, &foundDoc);

    if (status == ErrorCodes::NoSuchKey || status == ErrorCodes::TypeMismatch) {
        std::string msg = str::stream()
            << "During initial sync, found malformed auth schema version document: "
            << status.toString() << "; document: " << foundDoc;
        return Status(ErrorCodes::AuthSchemaIncompatible, msg);
    }

    if (status.isOK()) {
        auto version = foundDoc.getIntField(AuthorizationManager::schemaVersionFieldName);
        if ((version != AuthorizationManager::schemaVersion26Final) &&
            (version != AuthorizationManager::schemaVersion28SCRAM)) {
            std::string msg = str::stream()
                << "During initial sync, found auth schema version " << version
                << ", but this version of MongoDB only supports schema versions "
                << AuthorizationManager::schemaVersion26Final << " and "
                << AuthorizationManager::schemaVersion28SCRAM;
            return {ErrorCodes::AuthSchemaIncompatible, msg};
        }
    }

    return status;
}

bool AuthorizationManagerImpl::hasUser(OperationContext* opCtx,
                                       const boost::optional<TenantId>& tenantId) {
    return _externalState->hasAnyUserDocuments(opCtx, tenantId).isOK();
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
                                                             const UserRequest& request) try {
    const auto& userName = request.name;

    auto systemUser = internalSecurity.getUser();
    if (userName == (*systemUser)->getName()) {
        uassert(ErrorCodes::OperationFailed,
                "Attempted to acquire system user with predefined roles",
                request.roles == boost::none);
        return *systemUser;
    }

    UserRequest userRequest(request);
#ifdef MONGO_CONFIG_SSL
    // X.509 will give us our roles for initial acquire, but we have to lose them during
    // reacquire (for now) so reparse those roles into the request if not already present.
    if ((request.roles == boost::none) && request.mechanismData.empty() &&
        (userName.getDatabaseName().isExternalDB())) {
        userRequest = getX509UserRequest(opCtx, std::move(userRequest));
    }
#endif

    auto userAcquisitionStats = CurOp::get(opCtx)->getUserAcquisitionStats();
    if (authUserCacheBypass.shouldFail()) {
        // Bypass cache and force a fresh load of the user.
        auto loadedUser =
            uassertStatusOK(_externalState->getUserObject(opCtx, request, userAcquisitionStats));
        // We have to inject into the cache in order to get a UserHandle.
        auto userHandle =
            _userCache.insertOrAssignAndGet(request, std::move(loadedUser), Date_t::now());
        invariant(userHandle);
        LOGV2_DEBUG(4859401, 1, "Bypassing user cache to load user", "user"_attr = userName);
        return userHandle;
    }

    // Track wait time and user cache access statistics for the current op for logging. An extra
    // second of delay is added via the failpoint for testing.
    UserAcquisitionStatsHandle userAcquisitionStatsHandle = UserAcquisitionStatsHandle(
        userAcquisitionStats.get(), opCtx->getServiceContext()->getTickSource(), kCache);
    if (authUserCacheSleep.shouldFail()) {
        sleepsecs(1);
    }

    auto cachedUser = _userCache.acquire(opCtx,
                                         userRequest,
                                         CacheCausalConsistency::kLatestCached,
                                         CurOp::get(opCtx)->getUserAcquisitionStats());

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
    // TODO SERVER-72678 avoid this edge case hack when rearchitecting user acquisition. This is
    // necessary now to preserve the mechanismData from the original UserRequest while eliminating
    // the roles. If the roles aren't reset to none, it will cause LDAP acquisition to be bypassed
    // in favor of reusing the ones from before.
    UserRequest requestWithoutRoles(user->getUserRequest());
    requestWithoutRoles.roles = boost::none;
    auto swUserHandle = acquireUser(opCtx, requestWithoutRoles);
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

void AuthorizationManagerImpl::_updateCacheGeneration() {
    stdx::lock_guard lg(_cacheGenerationMutex);
    _cacheGeneration = OID::gen();
}

void AuthorizationManagerImpl::invalidateUserByName(const UserName& userName) {
    LOGV2_DEBUG(20235, 2, "Invalidating user", "user"_attr = userName);
    _updateCacheGeneration();
    _authSchemaVersionCache.invalidateAll();
    // Invalidate the named User, assuming no externally provided roles. When roles are defined
    // externally, there exists no user document which may become invalid.
    _userCache.invalidateKey(UserRequest(userName, boost::none));
}

void AuthorizationManagerImpl::invalidateUsersFromDB(const DatabaseName& dbname) {
    LOGV2_DEBUG(20236, 2, "Invalidating all users from database", "database"_attr = dbname);
    _updateCacheGeneration();
    _authSchemaVersionCache.invalidateAll();
    _userCache.invalidateKeyIf([&](const UserRequest& userRequest) {
        return userRequest.name.getDatabaseName() == dbname;
    });
}

void AuthorizationManagerImpl::invalidateUsersByTenant(const boost::optional<TenantId>& tenant) {
    if (!tenant) {
        invalidateUserCache();
        return;
    }

    LOGV2_DEBUG(6323600, 2, "Invalidating tenant users", "tenant"_attr = tenant);
    _updateCacheGeneration();
    _authSchemaVersionCache.invalidateAll();
    _userCache.invalidateKeyIf(
        [&](const UserRequest& userRequest) { return userRequest.name.getTenant() == tenant; });
}

void AuthorizationManagerImpl::invalidateUserCache() {
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
            return userRequest.name.getDatabaseName().isExternalDB();
        });

    // Then, retrieve the corresponding Users from the backing store for users in the $external
    // database. Compare each of these user objects with the cached user object and call
    // insertOrAssign if they differ.
    bool isRefreshed{false};
    for (const auto& cachedUser : cachedUsers) {
        UserRequest request(cachedUser->getName(), boost::none);
        auto storedUserStatus = _externalState->getUserObject(
            opCtx, request, CurOp::get(opCtx)->getUserAcquisitionStats());
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
    if (auto status = _externalState->initialize(opCtx); !status.isOK()) {
        return status;
    }

    invalidateUserCache();
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
    Service* service, ThreadPoolInterface& threadPool, AuthzManagerExternalState* externalState)
    : ReadThroughCache(
          _mutex,
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
    Service* service,
    ThreadPoolInterface& threadPool,
    int cacheSize,
    AuthSchemaVersionCache* authSchemaVersionCache,
    AuthzManagerExternalState* externalState)
    : UserCache(
          _mutex,
          service,
          threadPool,
          [this](OperationContext* opCtx,
                 const UserRequest& userReq,
                 const UserHandle& cachedUser,
                 const SharedUserAcquisitionStats& userAcquisitionStats) {
              return _lookup(opCtx, userReq, cachedUser, userAcquisitionStats);
          },
          cacheSize),
      _authSchemaVersionCache(authSchemaVersionCache),
      _externalState(externalState) {}

AuthorizationManagerImpl::UserCacheImpl::LookupResult
AuthorizationManagerImpl::UserCacheImpl::_lookup(
    OperationContext* opCtx,
    const UserRequest& userReq,
    const UserHandle& unusedCachedUser,
    const SharedUserAcquisitionStats& userAcquisitionStats) {
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
                return LookupResult(uassertStatusOK(
                    _externalState->getUserObject(opCtx, userReq, userAcquisitionStats)));
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
