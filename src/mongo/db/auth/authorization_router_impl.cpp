/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include "mongo/db/auth/authorization_router_impl.h"

#include "mongo/db/auth/authorization_manager_global_parameters_gen.h"
#include "mongo/db/auth/user_document_parser.h"
#include "mongo/db/auth/user_request_x509.h"
#include "mongo/db/curop.h"
#include "mongo/db/multitenancy.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kAccessControl

namespace mongo {

namespace {
MONGO_FAIL_POINT_DEFINE(waitForUserCacheInvalidation);
void handleWaitForUserCacheInvalidation(OperationContext* opCtx, const UserHandle& user) {
    auto fp = waitForUserCacheInvalidation.scopedIf([&](const auto& bsonData) {
        IDLParserContext ctx("waitForUserCacheInvalidation");
        auto data = WaitForUserCacheInvalidationFailPoint::parse(bsonData, ctx);

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
    stdx::mutex m;
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

int authorizationRouterCacheSize;

AuthorizationRouterImpl::AuthorizationRouterImpl(
    Service* service, std::unique_ptr<AuthorizationClientHandle> clientHandle)
    : _userCache(service, _threadPool, authorizationRouterCacheSize, this),
      _threadPool([] {
          ThreadPool::Options options;
          options.poolName = "AuthorizationRouter";
          options.minThreads = 0;
          options.maxThreads = ThreadPool::Options::kUnlimited;

          return options;
      }()),
      _clientHandle(std::move(clientHandle)) {
    _threadPool.startup();
}

std::unique_ptr<AuthzSessionExternalState> AuthorizationRouterImpl::makeAuthzSessionExternalState(
    Client* client) {
    return _clientHandle->makeAuthzSessionExternalState(client);
}

Status AuthorizationRouterImpl::getUserDescription(
    OperationContext* opCtx,
    const UserRequest& userRequest,
    BSONObj* result,
    const SharedUserAcquisitionStats& userAcquisitionStats) try {
    bool hasExternalRoles = userRequest.getRoles().has_value();
    if (!hasExternalRoles) {
        // If the userRequest does not have roles, then we need to run usersInfo.
        UsersInfoCommand usersInfoCmd(auth::UsersInfoCommandArg(userRequest.getUserName()));
        usersInfoCmd.setShowPrivileges(true);
        usersInfoCmd.setShowCredentials(true);
        usersInfoCmd.setShowAuthenticationRestrictions(true);
        usersInfoCmd.setShowCustomData(false);

        const auto& usersNSS =
            NamespaceString::makeTenantUsersCollection(userRequest.getUserName().tenantId());
        const auto& usersInfoReply = _runUsersInfoCmd(opCtx, usersNSS.dbName(), usersInfoCmd);
        const auto& foundUsers = usersInfoReply.getUsers();
        uassert(ErrorCodes::UserNotFound,
                str::stream() << "Could not find user \"" << userRequest.getUserName().getUser()
                              << "\" for db \"" << userRequest.getUserName().getDB() << "\"",
                foundUsers.size() > 0);
        uassert(ErrorCodes::UserDataInconsistent,
                str::stream() << "Found multiple users on the \""
                              << userRequest.getUserName().getDB() << "\" database with name \""
                              << userRequest.getUserName() << "\"",
                foundUsers.size() == 1);

        *result = foundUsers[0].getOwned();
    } else {
        // Otherwise, we run rolesInfo for role resolution.
        const auto& rolesArr = *userRequest.getRoles();
        auth::RolesInfoCommandArg::Multiple roleNames(rolesArr.begin(), rolesArr.end());

        RolesInfoCommand rolesInfoCmd{auth::RolesInfoCommandArg{roleNames}};
        rolesInfoCmd.setShowPrivileges(
            auth::ParsedPrivilegeFormat(PrivilegeFormat::kShowAsUserFragment));

        const auto& rolesNSS =
            NamespaceString::makeTenantRolesCollection(userRequest.getUserName().tenantId());
        const auto& rolesInfoReply = _runRolesInfoCmd(opCtx, rolesNSS.dbName(), rolesInfoCmd);
        uassert(ErrorCodes::FailedToParse,
                "Unable to get user via rolesInfo",
                rolesInfoReply.getUserFragment());

        const auto& cmdResult = rolesInfoReply.getUserFragment().value();

        BSONElement userRoles = cmdResult["roles"];
        BSONElement userInheritedRoles = cmdResult["inheritedRoles"];
        BSONElement userInheritedPrivileges = cmdResult["inheritedPrivileges"];

        bool isResponseMalformed = userRoles.eoo() || userInheritedRoles.eoo() ||
            userInheritedPrivileges.eoo() || !userRoles.isABSONObj() ||
            !userInheritedRoles.isABSONObj() || !userInheritedPrivileges.isABSONObj();
        uassert(ErrorCodes::UserDataInconsistent,
                "Received malformed response to rolesInfo request",
                !isResponseMalformed);

        const UserName& userName = userRequest.getUserName();
        *result =
            BSON("_id" << userName.getUser() << "user" << userName.getUser() << "db"
                       << userName.getDB() << "credentials" << BSON("external" << true) << "roles"
                       << BSONArray(cmdResult["roles"].Obj()) << "inheritedRoles"
                       << BSONArray(cmdResult["inheritedRoles"].Obj()) << "inheritedPrivileges"
                       << BSONArray(cmdResult["inheritedPrivileges"].Obj()));
    }

    return Status::OK();
} catch (const DBException& ex) {
    return ex.toStatus();
}

StatusWith<User> AuthorizationRouterImpl::getUserObject(
    OperationContext* opCtx,
    const UserRequest& userRequest,
    const SharedUserAcquisitionStats& userAcquisitionStats) {

    BSONObj userDoc;
    auto status = getUserDescription(opCtx, userRequest, &userDoc, userAcquisitionStats);
    if (!status.isOK()) {
        return status;
    }

    User user(userRequest.clone());
    V2UserDocumentParser dp;
    dp.setTenantId(getActiveTenant(opCtx));
    status = dp.initializeUserFromUserDocument(userDoc, &user);
    if (!status.isOK()) {
        return status;
    }

    std::vector<RoleName> directRoles;
    for (auto iter = user.getRoles(); iter.more();) {
        directRoles.push_back(iter.next());
    }

    LOGV2_DEBUG(5517200,
                3,
                "Acquired new user object",
                "userName"_attr = user.getName(),
                "directRoles"_attr = directRoles);

    return std::move(user);
}

Status AuthorizationRouterImpl::rolesExist(OperationContext* opCtx,
                                           const std::vector<RoleName>& roleNames) try {
    // Marshal role names into a set before querying so that we don't get a false-negative
    // from repeated roles only providing one result at the end.
    auth::RolesInfoCommandArg::Multiple roleNamesCopy(roleNames.begin(), roleNames.end());
    RolesInfoCommand rolesInfoCmd{auth::RolesInfoCommandArg{roleNamesCopy}};

    // This method only considers roles that exist on the admin database for the system
    // (boost::none) tenant.
    const auto& rolesInfoReply = _runRolesInfoCmd(opCtx, DatabaseName::kAdmin, rolesInfoCmd);
    uassert(ErrorCodes::OperationFailed,
            "Received invalid response from rolesInfo command",
            rolesInfoReply.getRoles());

    const auto& roles = rolesInfoReply.getRoles().value();
    stdx::unordered_set<RoleName> roleNamesSet(roleNames.begin(), roleNames.end());

    if (roles.size() != roleNamesSet.size()) {
        // One or more missing roles, cross out the ones that do exist, and return error.
        for (const auto& roleObj : roles) {
            auto roleName = RoleName::parseFromBSONObj(roleObj);
            roleNamesSet.erase(roleName);
        }

        uasserted(ErrorCodes::RoleNotFound, buildUnknownRolesErrorMsg(roleNamesSet));
    }

    return Status::OK();
} catch (const AssertionException& ex) {
    return ex.toStatus();
}

bool AuthorizationRouterImpl::hasAnyPrivilegeDocuments(OperationContext* opCtx) {
    UsersInfoCommand usersInfoCmd{auth::UsersInfoCommandArg{}};
    try {
        // This method only considers users on the system (boost::none) tenant.
        const auto& usersInfoReply = _runUsersInfoCmd(opCtx, DatabaseName::kAdmin, usersInfoCmd);
        if (!usersInfoReply.getUsers().empty()) {
            return true;
        }
    } catch (const DBException&) {
        // If an exception was thrown while running usersInfo, then we do not know whether or not
        // there are any user documents. It is safer to assume that there are in the event that the
        // exception was thrown due to a transiently unavailable node that still has user docs.
        return true;
    }

    RolesInfoCommand rolesInfoCmd{auth::RolesInfoCommandArg{}};
    try {
        // This method only considers roles on the system (boost::none) tenant.
        const auto& rolesInfoReply = _runRolesInfoCmd(opCtx, DatabaseName::kAdmin, rolesInfoCmd);
        const auto& foundRoles = rolesInfoReply.getRoles();

        if (foundRoles && !foundRoles->empty()) {
            return true;
        }
    } catch (const DBException&) {
        // Same rationale as above - if an exception is thrown while running rolesInfo, it is safer
        // to assume that there are privilege documents since we do not know for sure.
        return true;
    }

    return false;
}

void AuthorizationRouterImpl::notifyDDLOperation(OperationContext* opCtx,
                                                 StringData op,
                                                 const NamespaceString& nss,
                                                 const BSONObj& o,
                                                 const BSONObj* o2) {
    _clientHandle->notifyDDLOperation(opCtx, this, op, nss, o, o2);
}

OID AuthorizationRouterImpl::getCacheGeneration() {
    stdx::lock_guard lg(_cacheGenerationMutex);
    return _cacheGeneration;
}

namespace {
MONGO_FAIL_POINT_DEFINE(authUserCacheBypass);
MONGO_FAIL_POINT_DEFINE(authUserCacheSleep);

struct CurOpPauseGuard {
    explicit CurOpPauseGuard(CurOp* curOp) : curOp{curOp} {
        if (curOp && curOp->isStarted()) {
            curOp->pauseTimer();
        }
    }
    ~CurOpPauseGuard() {
        if (curOp && curOp->isPaused()) {
            curOp->resumeTimer();
        }
    }
    CurOp* curOp;
};
}  // namespace

StatusWith<UserHandle> AuthorizationRouterImpl::acquireUser(
    OperationContext* opCtx, std::unique_ptr<UserRequest> userRequest) try {
    const UserName userName = userRequest->getUserName();
    auto systemUser = internalSecurity.getUser();
    if (userName == (*systemUser)->getName()) {
        uassert(ErrorCodes::OperationFailed,
                "Attempted to acquire system user with predefined roles",
                userRequest->getRoles() == boost::none);
        return *systemUser;
    }

    auto userAcquisitionStats = CurOp::get(opCtx)->getUserAcquisitionStats();
    if (authUserCacheBypass.shouldFail()) {
        // Bypass cache and force a fresh load of the user.
        auto loadedUser =
            uassertStatusOK(getUserObject(opCtx, *userRequest.get(), userAcquisitionStats));
        // We have to inject into the cache in order to get a UserHandle.

        auto userHandle = _userCache.insertOrAssignAndGet(
            loadedUser.getUserRequest()->generateUserRequestCacheKey(),
            std::move(loadedUser),
            Date_t::now());
        invariant(userHandle);
        LOGV2_DEBUG(4859401, 1, "Bypassing user cache to load user", "user"_attr = userName);
        return userHandle;
    }

    // Track wait time and user cache access statistics for the current op for logging. An extra
    // second of delay is added via the failpoint for testing.
    UserAcquisitionStatsHandle userAcquisitionStatsHandle = UserAcquisitionStatsHandle(
        userAcquisitionStats.get(), opCtx->getServiceContext()->getTickSource(), kCache);
    CurOpPauseGuard curOpPauseGuard{CurOp::get(opCtx)};
    if (authUserCacheSleep.shouldFail()) {
        sleepsecs(1);
    }

    std::shared_ptr<UserRequest> sharedReq = std::move(userRequest);
    auto cachedUser = _userCache.acquire(opCtx,
                                         sharedReq->generateUserRequestCacheKey(),
                                         CacheCausalConsistency::kLatestCached,
                                         std::move(sharedReq),
                                         CurOp::get(opCtx)->getUserAcquisitionStats());

    userAcquisitionStatsHandle.recordTimerEnd();
    invariant(cachedUser);

    LOGV2_DEBUG(20226, 1, "Returning user from cache", "user"_attr = userName);
    return cachedUser;
} catch (const DBException& ex) {
    return ex.toStatus();
}

StatusWith<UserHandle> AuthorizationRouterImpl::reacquireUser(OperationContext* opCtx,
                                                              const UserHandle& userHandle) {
    const UserName& userName = userHandle->getName();
    handleWaitForUserCacheInvalidation(opCtx, userHandle);
    if (userHandle.isValid() && !userHandle->isInvalidated()) {
        return userHandle;
    }

    // Since we throw in the constructor if we have an error in acquiring roles for OIDC and X509,
    // we want to catch these exceptions and return them as proper statuses to be handled later.
    auto swRequestWithoutRoles = userHandle->getUserRequest()->cloneForReacquire();
    if (!swRequestWithoutRoles.isOK()) {
        return swRequestWithoutRoles.getStatus();
    }

    auto swUserHandle = acquireUser(opCtx, std::move(swRequestWithoutRoles.getValue()));
    if (!swUserHandle.isOK()) {
        return swUserHandle.getStatus();
    }

    auto ret = std::move(swUserHandle.getValue());
    if (userHandle->getID() != ret->getID()) {
        return {ErrorCodes::UserNotFound,
                str::stream() << "User id from privilege document '" << userName
                              << "' does not match user id in session."};
    }

    return ret;
}

void AuthorizationRouterImpl::_updateCacheGeneration() {
    stdx::lock_guard lg(_cacheGenerationMutex);
    _cacheGeneration = OID::gen();
}

void AuthorizationRouterImpl::invalidateUserByName(const UserName& userName) {
    LOGV2_DEBUG(20235, 2, "Invalidating user", "user"_attr = userName);

    // There may be multiple entries in the cache with arbitrary other UserRequest data.
    // Scan the full cache looking for a weak match on UserName only.
    _userCache.invalidateKeyIf([&userName](const UserRequest::UserRequestCacheKey& key) {
        return key.getUserName() == userName;
    });
    _updateCacheGeneration();
}

void AuthorizationRouterImpl::invalidateUsersFromDB(const DatabaseName& dbName) {
    LOGV2_DEBUG(20236, 2, "Invalidating all users from database", "database"_attr = dbName);
    _userCache.invalidateKeyIf([&](const UserRequest::UserRequestCacheKey& key) {
        return key.getUserName().getDatabaseName() == dbName;
    });
    _updateCacheGeneration();
}

void AuthorizationRouterImpl::invalidateUsersByTenant(const boost::optional<TenantId>& tenant) {
    if (!tenant) {
        invalidateUserCache();
        return;
    }

    LOGV2_DEBUG(6323600, 2, "Invalidating tenant users", "tenant"_attr = tenant);
    _userCache.invalidateKeyIf([&](const UserRequest::UserRequestCacheKey& key) {
        return key.getUserName().tenantId() == tenant;
    });
    _updateCacheGeneration();
}

void AuthorizationRouterImpl::invalidateUserCache() {
    LOGV2_DEBUG(20237, 2, "Invalidating user cache");
    _userCache.invalidateAll();
    _updateCacheGeneration();
}

Status AuthorizationRouterImpl::refreshExternalUsers(OperationContext* opCtx) try {
    LOGV2_DEBUG(5914801, 2, "Refreshing all users from the $external database");
    // First, get a snapshot of the UserHandles in the cache.
    auto cachedUsers = _userCache.peekLatestCachedIf(
        [&](const UserRequest::UserRequestCacheKey& cacheKey, const User&) {
            return cacheKey.getUserName().getDatabaseName().isExternalDB();
        });

    // Then, retrieve the corresponding Users from the backing store for users in the $external
    // database. Compare each of these user objects with the cached user object and call
    // insertOrAssign if they differ.
    bool isRefreshed{false};
    for (const auto& cachedUser : cachedUsers) {
        auto handleError = [this](const Status& status, const UserHandle& cachedUser) -> Status {
            if (status.code() == ErrorCodes::UserNotFound) {
                _userCache.invalidateKey(
                    cachedUser->getUserRequest()->generateUserRequestCacheKey());
                return Status::OK();
            } else {
                return status;
            }
        };

        auto swUserReq = cachedUser->getUserRequest()->cloneForReacquire();
        if (!swUserReq.isOK()) {
            auto status = handleError(swUserReq.getStatus(), cachedUser);
            if (!status.isOK()) {
                return status;
            }
            continue;
        }

        auto& userReq = swUserReq.getValue();

        auto storedUserStatus =
            getUserObject(opCtx, *userReq.get(), CurOp::get(opCtx)->getUserAcquisitionStats());

        if (!storedUserStatus.isOK()) {
            auto status = handleError(storedUserStatus.getStatus(), cachedUser);
            if (!status.isOK()) {
                return status;
            }
            continue;
        }

        if (cachedUser->hasDifferentRoles(storedUserStatus.getValue())) {
            _userCache.insertOrAssign(userReq->generateUserRequestCacheKey(),
                                      std::move(storedUserStatus.getValue()),
                                      Date_t::now());
            isRefreshed = true;
        }
    }

    // If any entries were refreshed, then the cache generation must be bumped for mongos to refresh
    // its cache.
    if (isRefreshed) {
        _updateCacheGeneration();
    }

    return Status::OK();
} catch (const DBException& e) {
    return e.toStatus();
}

std::vector<AuthorizationRouter::CachedUserInfo> AuthorizationRouterImpl::getUserCacheInfo() const {
    auto cacheData = _userCache.getCacheInfo();
    std::vector<AuthorizationRouter::CachedUserInfo> ret;
    ret.reserve(cacheData.size());
    std::transform(
        cacheData.begin(), cacheData.end(), std::back_inserter(ret), [](const auto& info) {
            return AuthorizationRouter::CachedUserInfo{info.key.getUserName(), info.useCount > 0};
        });

    return ret;
}

RolesInfoReply AuthorizationRouterImpl::_runRolesInfoCmd(OperationContext* opCtx,
                                                         const DatabaseName& dbName,
                                                         const RolesInfoCommand& cmd) {
    return uassertStatusOK(_clientHandle->sendRolesInfoRequest(opCtx, dbName, std::move(cmd)));
}

UsersInfoReply AuthorizationRouterImpl::_runUsersInfoCmd(OperationContext* opCtx,
                                                         const DatabaseName& dbName,
                                                         const UsersInfoCommand& cmd) {
    return uassertStatusOK(_clientHandle->sendUsersInfoRequest(opCtx, dbName, std::move(cmd)));
}

AuthorizationRouterImpl::UserCacheImpl::UserCacheImpl(Service* service,
                                                      ThreadPoolInterface& threadPool,
                                                      int cacheSize,
                                                      AuthorizationRouter* authzRouter)
    : UserCache(
          _mutex,
          service,
          threadPool,
          [this](OperationContext* opCtx,
                 const UserRequest::UserRequestCacheKey& userReqCacheKey,
                 const UserHandle& cachedUser,
                 std::shared_ptr<UserRequest> userReq,
                 const SharedUserAcquisitionStats& userAcquisitionStats) {
              return _lookup(
                  opCtx, userReqCacheKey, cachedUser, *userReq.get(), userAcquisitionStats);
          },
          cacheSize),
      _authzRouter(authzRouter) {}

AuthorizationRouterImpl::UserCacheImpl::LookupResult
AuthorizationRouterImpl::UserCacheImpl::_lookup(
    OperationContext* opCtx,
    const UserRequest::UserRequestCacheKey& userReqCacheKey,
    const UserHandle& unusedCachedUser,
    const UserRequest& userReq,
    const SharedUserAcquisitionStats& userAcquisitionStats) {

    LOGV2_DEBUG(20238, 1, "Getting user record", "user"_attr = userReq.getUserName());

    return LookupResult(
        uassertStatusOK(_authzRouter->getUserObject(opCtx, userReq, userAcquisitionStats)));
}

}  // namespace mongo
