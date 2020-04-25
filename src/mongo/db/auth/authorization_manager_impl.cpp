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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kAccessControl

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
#include "mongo/db/auth/authorization_manager_global_parameters_gen.h"
#include "mongo/db/auth/authorization_manager_impl_parameters_gen.h"
#include "mongo/db/auth/authorization_session_impl.h"
#include "mongo/db/auth/authz_manager_external_state.h"
#include "mongo/db/auth/sasl_options.h"
#include "mongo/db/auth/user_document_parser.h"
#include "mongo/db/auth/user_management_commands_parser.h"
#include "mongo/db/global_settings.h"
#include "mongo/db/mongod_options.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/net/ssl_types.h"
#include "mongo/util/str.h"

namespace mongo {
namespace {

MONGO_INITIALIZER_GENERAL(SetupInternalSecurityUser,
                          ("EndStartupOptionStorage"),
                          ("CreateAuthorizationManager"))
(InitializerContext* const context) try {
    UserHandle user(User(UserName("__system", "local")));

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
                return (userName == internalSecurity.user->getName());
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
        const NamespaceString toNamespace(cmdObj["to"].valueStringDataSafe());

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

/**
 * Parses privDoc and fully initializes the user object (credentials, roles, and privileges) with
 * the information extracted from the privilege document.
 */
Status initializeUserFromPrivilegeDocument(User* user, const BSONObj& privDoc) {
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

}  // namespace

int authorizationManagerCacheSize;

void AuthorizationManagerPinnedUsersServerParameter::append(OperationContext* opCtx,
                                                            BSONObjBuilder& out,
                                                            const std::string& name) {
    return authorizationManagerPinnedUsers.append(out, name);
}

Status AuthorizationManagerPinnedUsersServerParameter::set(const BSONElement& newValue) {
    return authorizationManagerPinnedUsers.set(newValue);
}

Status AuthorizationManagerPinnedUsersServerParameter::setFromString(const std::string& str) {
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
    std::vector<BSONObj>* result) {
    return _externalState->getRoleDescriptionsForDB(
        opCtx, dbname, privileges, restrictions, showBuiltinRoles, result);
}

StatusWith<UserHandle> AuthorizationManagerImpl::acquireUser(OperationContext* opCtx,
                                                             const UserName& userName) try {
    if (userName == internalSecurity.user->getName()) {
        return internalSecurity.user;
    }

    UserRequest request(userName, boost::none);

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

    auto cachedUser = _userCache.acquire(opCtx, request);
    invariant(cachedUser);

    LOGV2_DEBUG(20226, 1, "Returning user from cache", "user"_attr = userName);
    return cachedUser;
} catch (const DBException& ex) {
    return ex.toStatus();
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
                                  "user"_attr = userName.toString(),
                                  "status"_attr = status);
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
    _userCache.invalidate(UserRequest(userName, boost::none));
}

void AuthorizationManagerImpl::invalidateUsersFromDB(OperationContext* opCtx, StringData dbname) {
    LOGV2_DEBUG(20236, 2, "Invalidating all users from database", "database"_attr = dbname);
    _updateCacheGeneration();
    _authSchemaVersionCache.invalidateAll();
    _userCache.invalidateIf(
        [&](const UserRequest& userRequest) { return userRequest.name.getDB() == dbname; });
}

void AuthorizationManagerImpl::invalidateUserCache(OperationContext* opCtx) {
    LOGV2_DEBUG(20237, 2, "Invalidating user cache");
    _updateCacheGeneration();
    _authSchemaVersionCache.invalidateAll();
    _userCache.invalidateAll();
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
            return AuthorizationManager::CachedUserInfo{info.key.name, info.useCount > 0};
        });

    return ret;
}

AuthorizationManagerImpl::AuthSchemaVersionCache::AuthSchemaVersionCache(
    ServiceContext* service,
    ThreadPoolInterface& threadPool,
    AuthzManagerExternalState* externalState)
    : ReadThroughCache(_mutex, service, threadPool, 1 /* cacheSize */),
      _externalState(externalState) {}

boost::optional<int> AuthorizationManagerImpl::AuthSchemaVersionCache::lookup(
    OperationContext* opCtx, const int& unusedKey) {
    invariant(unusedKey == 0);

    int authzVersion;
    uassertStatusOK(_externalState->getStoredAuthorizationVersion(opCtx, &authzVersion));

    return authzVersion;
}

AuthorizationManagerImpl::UserCacheImpl::UserCacheImpl(
    ServiceContext* service,
    ThreadPoolInterface& threadPool,
    int cacheSize,
    AuthSchemaVersionCache* authSchemaVersionCache,
    AuthzManagerExternalState* externalState)
    : UserCache(_mutex, service, threadPool, cacheSize),
      _authSchemaVersionCache(authSchemaVersionCache),
      _externalState(externalState) {}

boost::optional<User> AuthorizationManagerImpl::UserCacheImpl::lookup(OperationContext* opCtx,
                                                                      const UserRequest& userReq) {
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
            case schemaVersion26Upgrade: {
                BSONObj userObj;
                uassertStatusOK(_externalState->getUserDescription(opCtx, userReq, &userObj));

                User user(userReq.name);
                uassertStatusOK(initializeUserFromPrivilegeDocument(&user, userObj));
                return user;
            }
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
