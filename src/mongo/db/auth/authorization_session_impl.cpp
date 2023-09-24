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
#include <absl/meta/type_traits.h>
#include <algorithm>
#include <boost/none.hpp>
#include <boost/optional.hpp>
#include <cstddef>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

#include "mongo/base/error_codes.h"
#include "mongo/base/init.h"  // IWYU pragma: keep
#include "mongo/base/initializer.h"
#include "mongo/base/shim.h"
#include "mongo/base/status.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/util/builder.h"
#include "mongo/bson/util/builder_fwd.h"
#include "mongo/db/api_parameters.h"
#include "mongo/db/audit.h"
#include "mongo/db/auth/access_checks_gen.h"
#include "mongo/db/auth/action_set.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/action_type_gen.h"
#include "mongo/db/auth/auth_name.h"
#include "mongo/db/auth/authorization_session_impl.h"
#include "mongo/db/auth/authz_session_external_state.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/auth/resource_pattern_search_list.h"
#include "mongo/db/auth/validated_tenancy_scope.h"
#include "mongo/db/bson/dotted_path_support.h"
#include "mongo/db/client.h"
#include "mongo/db/list_collections_gen.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/db/session/logical_session_id.h"
#include "mongo/db/tenant_id.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_attr.h"
#include "mongo/logv2/log_component.h"
#include "mongo/logv2/redaction.h"
#include "mongo/platform/compiler.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/clock_source.h"
#include "mongo/util/decorable.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/read_through_cache.h"
#include "mongo/util/str.h"
#include "mongo/util/testing_proctor.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kAccessControl


namespace mongo {

namespace dps = ::mongo::dotted_path_support;
using std::vector;
namespace {

std::unique_ptr<AuthorizationSession> authorizationSessionCreateImpl(
    AuthorizationManager* authzManager) {
    return std::make_unique<AuthorizationSessionImpl>(
        AuthzSessionExternalState::create(authzManager),
        AuthorizationSessionImpl::InstallMockForTestingOrAuthImpl{});
}

auto authorizationSessionCreateRegistration =
    MONGO_WEAK_FUNCTION_REGISTRATION(AuthorizationSession::create, authorizationSessionCreateImpl);

constexpr StringData SYSTEM_BUCKETS_PREFIX = "system.buckets."_sd;

bool checkContracts() {
    // Only check contracts in testing modes, invalid contracts should not break customers.
    if (!TestingProctor::instance().isEnabled()) {
        return false;
    }

    return true;
}

using ServerlessPermissionMap = stdx::unordered_map<MatchTypeEnum, ActionSet>;
ServerlessPermissionMap kServerlessPrivilegesPermitted;

/**
 * Load extra data from action_types.idl into runtime structure.
 * For any given resource match type, we allow only the ActionTypes named
 * to be granted to security token based users.
 */
MONGO_INITIALIZER(ServerlessPrivilegePermittedMap)(InitializerContext*) try {
    ServerlessPermissionMap ret;

    for (std::size_t i = 0; i < idlEnumCount<MatchTypeEnum>; ++i) {
        auto matchType = static_cast<MatchTypeEnum>(i);
        auto matchTypeName = MatchType_serializer(matchType);
        auto dataObj = MatchType_get_extra_data(matchType);
        auto data = MatchTypeExtraData::parse(IDLParserContext{matchTypeName}, dataObj);

        std::vector<std::string> unknownActions;
        auto actions =
            ActionSet::parseFromStringVector(data.getServerlessActionTypes(), &unknownActions);
        if (!unknownActions.empty()) {
            StringBuilder sb;
            sb << "Unknown actions listed for match type '" << matchTypeName << "':";
            for (const auto& unknownAction : unknownActions) {
                sb << " '" << unknownAction << "'";
            }
            uasserted(ErrorCodes::FailedToParse, sb.str());
        }

        ret[matchType] = std::move(actions);
    }

    kServerlessPrivilegesPermitted = std::move(ret);
} catch (const DBException& ex) {
    uassertStatusOK(ex.toStatus().withContext("Failed parsing extraData for MatchType enum"));
}

void validateSecurityTokenUserPrivileges(const User::ResourcePrivilegeMap& privs) {
    for (const auto& priv : privs) {
        auto matchType = priv.first.matchType();
        const auto& actions = priv.second.getActions();
        auto it = kServerlessPrivilegesPermitted.find(matchType);
        // This actually can't happen since the initializer above populated the map with all match
        // types.
        uassert(6161701,
                str::stream() << "Unknown matchType: " << MatchType_serializer(matchType),
                it != kServerlessPrivilegesPermitted.end());
        if (MONGO_unlikely(!it->second.isSupersetOf(actions))) {
            auto unauthorized = actions;
            unauthorized.removeAllActionsFromSet(it->second);
            uasserted(6161702,
                      str::stream()
                          << "Security Token user has one or more actions not approved for "
                             "resource matchType '"
                          << MatchType_serializer(matchType) << "': " << unauthorized.toString());
        }
    }
}

MONGO_FAIL_POINT_DEFINE(allowMultipleUsersWithApiStrict);

}  // namespace

AuthorizationSessionImpl::AuthorizationSessionImpl(
    std::unique_ptr<AuthzSessionExternalState> externalState, InstallMockForTestingOrAuthImpl)
    : _externalState(std::move(externalState)),
      _impersonationFlag(false),
      _contract(TestingProctor::instance().isEnabled()),
      _mayBypassWriteBlockingMode(false),
      _mayUseTenant(false) {}

AuthorizationSessionImpl::~AuthorizationSessionImpl() {
    invariant(_authenticatedUser == boost::none,
              "The authenticated user should have been logged out by the Client destruction hook");
}

AuthorizationManager& AuthorizationSessionImpl::getAuthorizationManager() {
    return _externalState->getAuthorizationManager();
}

void AuthorizationSessionImpl::startRequest(OperationContext* opCtx) {
    _externalState->startRequest(opCtx);
    if (_authenticationMode == AuthenticationMode::kSecurityToken) {
        // Previously authenticated using SecurityToken,
        // clear that user and reset to unauthenticated state.
        if (auto user = std::exchange(_authenticatedUser, boost::none); user) {
            LOGV2_DEBUG(6161507,
                        3,
                        "security token based user still authenticated at start of request, "
                        "clearing from authentication state",
                        "user"_attr = user.value()->getName().toBSON(true /* encode tenant */));
            _updateInternalAuthorizationState();
        }
        _authenticationMode = AuthenticationMode::kNone;
    } else {
        // For non-security token users, check if expiration has passed and move session into
        // expired state if so.
        if (_expirationTime &&
            _expirationTime.value() <= opCtx->getServiceContext()->getFastClockSource()->now()) {
            _expiredUserName = std::exchange(_authenticatedUser, boost::none).value()->getName();
            _expirationTime = boost::none;
            clearImpersonatedUserData();
            _updateInternalAuthorizationState();
        }
    }
    _refreshUserInfoAsNeeded(opCtx);
}

void AuthorizationSessionImpl::startContractTracking() {
    if (!checkContracts()) {
        return;
    }

    _contract.clear();
}

Status AuthorizationSessionImpl::addAndAuthorizeUser(OperationContext* opCtx,
                                                     const UserRequest& userRequest,
                                                     boost::optional<Date_t> expirationTime) try {
    const auto& userName = userRequest.name;

    // Check before we start to reveal as little as possible. Note that we do not need the lock
    // because only the Client thread can mutate _authenticatedUser.
    if (_authenticatedUser) {
        // Already logged in.
        auto previousUser = _authenticatedUser.value()->getName();
        if (previousUser == userName) {
            // Allow reauthenticating as the same user, but warn.
            LOGV2_WARNING(5626700,
                          "Client has attempted to reauthenticate as a single user",
                          "user"_attr = userName);

            // Strict API requires no reauth, even as same user, unless FP is enabled.
            const bool hasStrictAPI = APIParameters::get(opCtx).getAPIStrict().value_or(false);
            uassert(5626703,
                    "Each client connection may only be authenticated once",
                    !hasStrictAPI || allowMultipleUsersWithApiStrict.shouldFail());

            return Status::OK();
        } else {
            uassert(5626701,
                    str::stream() << "Each client connection may only be authenticated once. "
                                  << "Previously authenticated as: " << previousUser,
                    previousUser.getDB() == userName.getDB());
            uasserted(5626702,
                      str::stream() << "Client has attempted to authenticate on multiple databases."
                                    << "Already authenticated as: " << previousUser);
        }
        MONGO_UNREACHABLE;
    } else {
        // If session is expired, then treat this as reauth for an expired session and only permit
        // the same user.
        if (_expiredUserName) {
            uassert(7070100,
                    str::stream() << "Only same user is permitted to re-auth to an expired "
                                     "session. Expired user is "
                                  << _expiredUserName.value(),
                    _expiredUserName == userName);
        }
    }

    AuthorizationManager* authzManager = AuthorizationManager::get(opCtx->getServiceContext());
    auto user = uassertStatusOK(authzManager->acquireUser(opCtx, userRequest));

    auto restrictionStatus = user->validateRestrictions(opCtx);
    if (!restrictionStatus.isOK()) {
        LOGV2(20240,
              "Failed to acquire user because of unmet authentication restrictions",
              "user"_attr = userName,
              "reason"_attr = restrictionStatus.reason());
        return AuthorizationManager::authenticationFailedStatus;
    }

    stdx::lock_guard<Client> lk(*opCtx->getClient());

    auto validatedTenancyScope = auth::ValidatedTenancyScope::get(opCtx);
    if (validatedTenancyScope && validatedTenancyScope->hasAuthenticatedUser()) {
        uassert(
            6161501,
            "Attempt to authorize via security token on connection with established authentication",
            _authenticationMode != AuthenticationMode::kConnection);
        uassert(6161502,
                "Attempt to authorize a user other than that present in the security token",
                validatedTenancyScope->authenticatedUser() == userName);
        auto tokenExpires = validatedTenancyScope->getExpiration();
        if (!expirationTime) {
            expirationTime = tokenExpires;
        } else if (tokenExpires < expirationTime.get()) {
            expirationTime = tokenExpires;
        }
        validateSecurityTokenUserPrivileges(user->getPrivileges());
        _authenticationMode = AuthenticationMode::kSecurityToken;
    } else {
        uassert(7070102,
                "Invalid expiration time specified",
                !expirationTime ||
                    expirationTime.value() >
                        opCtx->getServiceContext()->getFastClockSource()->now());
        _authenticationMode = AuthenticationMode::kConnection;
    }
    _authenticatedUser = std::move(user);
    _expirationTime = std::move(expirationTime);
    _expiredUserName = boost::none;

    // If there are any users and roles in the impersonation data, clear it out.
    clearImpersonatedUserData();

    _updateInternalAuthorizationState();
    return Status::OK();

} catch (const DBException& ex) {
    return ex.toStatus();
}

User* AuthorizationSessionImpl::lookupUser(const UserName& name) {
    _contract.addAccessCheck(AccessCheckEnum::kLookupUser);

    if (!_authenticatedUser || (_authenticatedUser.value()->getName() != name)) {
        return nullptr;
    }
    return _authenticatedUser->get();
}

boost::optional<UserHandle> AuthorizationSessionImpl::getAuthenticatedUser() {
    _contract.addAccessCheck(AccessCheckEnum::kGetAuthenticatedUser);
    return _authenticatedUser;
}

void AuthorizationSessionImpl::logoutSecurityTokenUser(Client* client) {
    stdx::lock_guard<Client> lk(*client);

    uassert(6161503,
            "Attempted to deauth a security token user while using standard login",
            _authenticationMode != AuthenticationMode::kConnection);

    auto user = std::exchange(_authenticatedUser, boost::none);
    if (user) {
        LOGV2_DEBUG(6161506,
                    5,
                    "security token based user explicitly logged out",
                    "user"_attr = user.value()->getName().toBSON(true /* encode tenant */));
    }

    // Explicitly skip auditing the logout event,
    // security tokens don't represent a permanent login.
    clearImpersonatedUserData();
    _updateInternalAuthorizationState();
}

void AuthorizationSessionImpl::logoutAllDatabases(Client* client, StringData reason) {
    stdx::lock_guard<Client> lk(*client);

    uassert(6161504,
            "May not log out while using a security token based authentication",
            _authenticationMode != AuthenticationMode::kSecurityToken);

    auto authenticatedUser = std::exchange(_authenticatedUser, boost::none);
    auto expiredUserName = std::exchange(_expiredUserName, boost::none);

    if (authenticatedUser) {
        auto names = BSON_ARRAY(authenticatedUser.value()->getName().toBSON());
        audit::logLogout(client, reason, names, BSONArray());
    } else if (expiredUserName) {
        auto names = BSON_ARRAY(expiredUserName.value().toBSON());
        audit::logLogout(client, reason, names, BSONArray());
    } else {
        return;
    }

    _expirationTime = boost::none;

    clearImpersonatedUserData();
    _updateInternalAuthorizationState();
}


void AuthorizationSessionImpl::logoutDatabase(Client* client,
                                              const DatabaseName& dbname,
                                              StringData reason) {
    bool isLoggedInOnDB =
        (_authenticatedUser && _authenticatedUser.value()->getName().getDatabaseName() == dbname);
    bool isExpiredOnDB = (_expiredUserName && _expiredUserName.value().getDatabaseName() == dbname);

    if (isLoggedInOnDB || isExpiredOnDB) {
        // The session either has an authenticated or expired user belonging to the database being
        // logged out from. Calling logoutAllDatabases() will clear that user out.
        logoutAllDatabases(client, reason);
    }
}

boost::optional<UserName> AuthorizationSessionImpl::getAuthenticatedUserName() {
    _contract.addAccessCheck(AccessCheckEnum::kGetAuthenticatedUserName);

    if (_authenticatedUser) {
        return _authenticatedUser.value()->getName();
    } else {
        return boost::none;
    }
}

RoleNameIterator AuthorizationSessionImpl::getAuthenticatedRoleNames() {
    _contract.addAccessCheck(AccessCheckEnum::kGetAuthenticatedRoleNames);

    return makeRoleNameIterator(_authenticatedRoleNames.begin(), _authenticatedRoleNames.end());
}

void AuthorizationSessionImpl::grantInternalAuthorization(Client* client) {
    stdx::lock_guard<Client> lk(*client);
    if (MONGO_unlikely(_authenticatedUser != boost::none)) {
        auto previousUser = _authenticatedUser.value()->getName();
        uassert(ErrorCodes::Unauthorized,
                str::stream() << "Unable to grant internal authorization, previously authorized as "
                              << previousUser.getUnambiguousName(),
                previousUser == internalSecurity.getUser()->get()->getName());
        return;
    }

    uassert(ErrorCodes::ReauthenticationRequired,
            str::stream() << "Unable to grant internal authorization on an expired session, "
                          << "must reauthenticate as " << _expiredUserName->getUnambiguousName(),
            _expiredUserName == boost::none);

    _authenticatedUser = *internalSecurity.getUser();
    _authenticationMode = AuthenticationMode::kConnection;
    _expirationTime = boost::none;
    _updateInternalAuthorizationState();
}

/**
 * Overloaded function - takes in the opCtx of the current AuthSession
 * and calls the function above.
 */
void AuthorizationSessionImpl::grantInternalAuthorization(OperationContext* opCtx) {
    grantInternalAuthorization(opCtx->getClient());
}

PrivilegeVector AuthorizationSessionImpl::_getDefaultPrivileges() {
    PrivilegeVector defaultPrivileges;

    // If localhost exception is active (and no users exist),
    // return a vector of the minimum privileges required to bootstrap
    // a system and add the first user.
    if (_externalState->shouldAllowLocalhost()) {

        const ResourcePattern adminDBResource =
            ResourcePattern::forDatabaseName(DatabaseName::kAdmin);
        const ActionSet setupAdminUserActionSet{ActionType::createUser, ActionType::grantRole};
        Privilege setupAdminUserPrivilege(adminDBResource, setupAdminUserActionSet);

        const ResourcePattern externalDBResource =
            ResourcePattern::forDatabaseName(DatabaseName::kExternal);
        Privilege setupExternalUserPrivilege(externalDBResource, ActionType::createUser);

        ActionSet setupServerConfigActionSet;

        // If this server is an arbiter, add specific privileges meant to circumvent
        // the behavior of an arbiter in an authenticated replset. See SERVER-5479.
        if (_externalState->serverIsArbiter()) {
            setupServerConfigActionSet.addAction(ActionType::getCmdLineOpts);
            setupServerConfigActionSet.addAction(ActionType::getParameter);
            setupServerConfigActionSet.addAction(ActionType::serverStatus);
            setupServerConfigActionSet.addAction(ActionType::shutdown);
        }

        setupServerConfigActionSet.addAction(ActionType::addShard);
        setupServerConfigActionSet.addAction(ActionType::replSetConfigure);
        setupServerConfigActionSet.addAction(ActionType::replSetGetStatus);
        setupServerConfigActionSet.addAction(ActionType::issueDirectShardOperations);
        Privilege setupServerConfigPrivilege =
            Privilege(ResourcePattern::forClusterResource(boost::none), setupServerConfigActionSet);

        Privilege::addPrivilegeToPrivilegeVector(&defaultPrivileges, setupAdminUserPrivilege);
        Privilege::addPrivilegeToPrivilegeVector(&defaultPrivileges, setupExternalUserPrivilege);
        Privilege::addPrivilegeToPrivilegeVector(&defaultPrivileges, setupServerConfigPrivilege);
        return defaultPrivileges;
    }

    return defaultPrivileges;
}

boost::optional<TenantId> AuthorizationSessionImpl::getUserTenantId() const {
    return _authenticatedUser ? _authenticatedUser.value()->getName().getTenant() : boost::none;
}

bool AuthorizationSessionImpl::isAuthorizedToParseNamespaceElement(const BSONElement& element) {
    const bool isUUID = element.type() == BinData && element.binDataType() == BinDataType::newUUID;
    _contract.addAccessCheck(AccessCheckEnum::kIsAuthorizedToParseNamespaceElement);

    uassert(ErrorCodes::InvalidNamespace,
            "Failed to parse namespace element",
            element.type() == String || isUUID);

    if (isUUID) {
        return isAuthorizedForActionsOnResource(
            ResourcePattern::forClusterResource(getUserTenantId()), ActionType::useUUID);
    }

    return true;
}

bool AuthorizationSessionImpl::isAuthorizedToParseNamespaceElement(
    const NamespaceStringOrUUID& nss) {
    _contract.addAccessCheck(AccessCheckEnum::kIsAuthorizedToParseNamespaceElement);

    if (nss.isUUID()) {
        return isAuthorizedForActionsOnResource(
            ResourcePattern::forClusterResource(getUserTenantId()), ActionType::useUUID);
    }
    return true;
}

bool AuthorizationSessionImpl::isAuthorizedToCreateRole(const RoleName& roleName) {
    _contract.addAccessCheck(AccessCheckEnum::kIsAuthorizedToCreateRole);

    // A user is allowed to create a role under either of two conditions.

    // The user may create a role if the authorization system says they are allowed to.
    if (isAuthorizedForActionsOnResource(
            ResourcePattern::forDatabaseName(roleName.getDatabaseName()), ActionType::createRole)) {
        return true;
    }

    // The user may create a role if the localhost exception is enabled, and they already own the
    // role. This implies they have obtained the role through an external authorization mechanism.
    if (_externalState->shouldAllowLocalhost()) {
        if (_authenticatedUser && _authenticatedUser.value()->hasRole(roleName)) {
            return true;
        }
        LOGV2(20241,
              "Not authorized to create the first role in the system using the "
              "localhost exception. The user needs to acquire the role through "
              "external authentication first.",
              "role"_attr = roleName);
    }

    return false;
}

bool AuthorizationSessionImpl::isAuthorizedForPrivilege(const Privilege& privilege) {
    if (_externalState->shouldIgnoreAuthChecks())
        return true;

    return _isAuthorizedForPrivilege(privilege);
}

bool AuthorizationSessionImpl::isAuthorizedForPrivileges(const vector<Privilege>& privileges) {
    if (_externalState->shouldIgnoreAuthChecks())
        return true;

    for (size_t i = 0; i < privileges.size(); ++i) {
        if (!_isAuthorizedForPrivilege(privileges[i]))
            return false;
    }

    return true;
}

bool AuthorizationSessionImpl::isAuthorizedForActionsOnResource(const ResourcePattern& resource,
                                                                ActionType action) {
    return isAuthorizedForPrivilege(Privilege(resource, action));
}

bool AuthorizationSessionImpl::isAuthorizedForActionsOnResource(const ResourcePattern& resource,
                                                                const ActionSet& actions) {
    return isAuthorizedForPrivilege(Privilege(resource, actions));
}

bool AuthorizationSessionImpl::isAuthorizedForActionsOnNamespace(const NamespaceString& ns,
                                                                 ActionType action) {
    return isAuthorizedForPrivilege(Privilege(ResourcePattern::forExactNamespace(ns), action));
}

bool AuthorizationSessionImpl::isAuthorizedForActionsOnNamespace(const NamespaceString& ns,
                                                                 const ActionSet& actions) {
    return isAuthorizedForPrivilege(Privilege(ResourcePattern::forExactNamespace(ns), actions));
}

bool AuthorizationSessionImpl::isAuthorizedToChangeAsUser(const UserName& userName,
                                                          ActionType actionType) {
    _contract.addAccessCheck(AccessCheckEnum::kIsAuthorizedToChangeAsUser);

    User* user = lookupUser(userName);
    if (!user) {
        return false;
    }

    auth::ResourcePatternSearchList search(
        ResourcePattern::forDatabaseName(userName.getDatabaseName()));
    return std::any_of(search.cbegin(), search.cend(), [&user, &actionType](const auto& pattern) {
        return user->getActionsForResource(pattern).contains(actionType);
    });
}

StatusWith<PrivilegeVector> AuthorizationSessionImpl::checkAuthorizedToListCollections(
    const ListCollections& cmd) {
    const auto& dbname = cmd.getDbName();
    _contract.addAccessCheck(AccessCheckEnum::kCheckAuthorizedToListCollections);

    if (cmd.getAuthorizedCollections() && cmd.getNameOnly() &&
        AuthorizationSessionImpl::isAuthorizedForAnyActionOnAnyResourceInDB(dbname)) {
        return PrivilegeVector();
    }

    // Check for the listCollections ActionType on the database.
    PrivilegeVector privileges = {
        Privilege(ResourcePattern::forDatabaseName(dbname), ActionType::listCollections)};
    if (AuthorizationSessionImpl::isAuthorizedForPrivileges(privileges)) {
        return privileges;
    }

    return Status(ErrorCodes::Unauthorized,
                  str::stream() << "Not authorized to list collections on db: "
                                << dbname.toStringForErrorMsg());
}

bool AuthorizationSessionImpl::isAuthenticatedAsUserWithRole(const RoleName& roleName) {
    _contract.addAccessCheck(AccessCheckEnum::kIsAuthenticatedAsUserWithRole);
    return (_authenticatedUser && _authenticatedUser.value()->hasRole(roleName));
}

bool AuthorizationSessionImpl::shouldIgnoreAuthChecks() {
    _contract.addAccessCheck(AccessCheckEnum::kShouldIgnoreAuthChecks);

    return _externalState->shouldIgnoreAuthChecks();
}

bool AuthorizationSessionImpl::isAuthenticated() {
    _contract.addAccessCheck(AccessCheckEnum::kIsAuthenticated);

    return _authenticatedUser != boost::none;
}

void AuthorizationSessionImpl::_refreshUserInfoAsNeeded(OperationContext* opCtx) {
    if (_authenticatedUser == boost::none) {
        return;
    }

    auto currentUser = _authenticatedUser.value();
    const auto& name = currentUser->getName();

    const auto clearUser = [&] {
        stdx::lock_guard<Client> lk(*opCtx->getClient());
        _authenticatedUser = boost::none;
        _authenticationMode = AuthenticationMode::kNone;
        _expirationTime = boost::none;
        _updateInternalAuthorizationState();
    };

    const auto updateUser = [&](auto&& user) {
        stdx::lock_guard<Client> lk(*opCtx->getClient());
        _authenticatedUser = std::move(user);
        LOGV2_DEBUG(
            20244, 1, "Updated session cache of user information for user", "user"_attr = name);
        _updateInternalAuthorizationState();
    };

    auto swUser = getAuthorizationManager().reacquireUser(opCtx, currentUser);
    if (!swUser.isOK()) {
        auto& status = swUser.getStatus();
        switch (status.code()) {
            case ErrorCodes::UserNotFound: {
                // User does not exist anymore.
                clearUser();
                LOGV2(20245,
                      "Removed deleted user from session cache of user information",
                      "user"_attr = name);
                return;
            }
            case ErrorCodes::UnsupportedFormat: {
                // An auth subsystem has explicitly indicated a failure.
                clearUser();
                LOGV2(20246,
                      "Removed user from session cache of user information because of "
                      "refresh failure",
                      "user"_attr = name,
                      "error"_attr = status);
                return;
            }
            case ErrorCodes::ReauthenticationRequired: {
                // An auth subsystem has indicated that client reauthentication is required. The
                // session will exist in an expired state to signal this to drivers.
                _expiredUserName = _authenticatedUser.value()->getName();
                clearUser();
                LOGV2(
                    7119502,
                    "Removed user from session cache of user information because reauthentication "
                    "is required",
                    "user"_attr = name,
                    "error"_attr = status);
                return;
            }
            case ErrorCodes::LDAPRoleAcquisitionError: {
                LOGV2_WARNING(
                    7785501,
                    "Could not fetch updated user authorization rights via LDAP, continuing "
                    "to use old information",
                    "user"_attr = name,
                    "error"_attr = redact(status));
                return;
            }
            default:
                // Unrecognized error; assume that it's transient, and continue working with the
                // out-of-date privilege data.
                LOGV2_WARNING(20247,
                              "Could not fetch updated user privilege information for {user}; "
                              "continuing to use old information. Reason is {error}",
                              "Could not fetch updated user privilege information, continuing "
                              "to use old information",
                              "user"_attr = name,
                              "error"_attr = redact(status));
                return;
        }
    }

    // !ok check above should never fallthrough.
    invariant(swUser.isOK());

    if (currentUser.isValid() && !currentUser->isInvalidated()) {
        // Current user may carry on, no need to update.
        return;
    }

    // Our user handle has changed, update it.
    auto user = std::move(swUser.getValue());
    try {
        uassertStatusOK(user->validateRestrictions(opCtx));
    } catch (const DBException& ex) {
        clearUser();
        LOGV2(20242,
              "Removed user with unmet authentication restrictions from "
              "session cache of user information. Restriction failed",
              "user"_attr = name,
              "reason"_attr = ex.reason());
        return;
    } catch (...) {
        clearUser();
        LOGV2(20243,
              "Evaluating authentication restrictions for user resulted in an "
              "unknown exception. Removing user from the session cache",
              "user"_attr = name);
        return;
    }

    updateUser(std::move(user));
}

bool AuthorizationSessionImpl::isAuthorizedForAnyActionOnAnyResourceInDB(
    const DatabaseName& dbname) {
    _contract.addAccessCheck(AccessCheckEnum::kIsAuthorizedForAnyActionOnAnyResourceInDB);
    const auto& tenantId = dbname.tenantId();

    if (_externalState->shouldIgnoreAuthChecks()) {
        return true;
    }

    if (_authenticatedUser == boost::none) {
        return false;
    }

    const auto& user = _authenticatedUser.value();
    // First lookup any Privileges on this database specifying Database resources
    if (user->hasActionsForResource(ResourcePattern::forDatabaseName(dbname))) {
        return true;
    }

    // Any resource will match any collection in the database
    if (user->hasActionsForResource(ResourcePattern::forAnyResource(tenantId))) {
        return true;
    }

    // Any resource will match any system_buckets collection in the database
    if (user->hasActionsForResource(ResourcePattern::forAnySystemBuckets(tenantId)) ||
        user->hasActionsForResource(ResourcePattern::forAnySystemBucketsInDatabase(dbname))) {
        return true;
    }

    // If the user is authorized for anyNormalResource, then they implicitly have access
    // to most databases.
    if (!dbname.isLocalDB() && !dbname.isConfigDB() &&
        user->hasActionsForResource(ResourcePattern::forAnyNormalResource(tenantId))) {
        return true;
    }

    // We've checked all the resource types that can be directly expressed. Now we must
    // iterate all privileges, until we see something that could reside in the target database.
    auto map = user->getPrivileges();
    for (const auto& privilege : map) {
        const auto& privRsrc = privilege.first;

        // If the user has a Collection privilege, then they're authorized for this resource
        // on all databases.
        if (privRsrc.isCollectionPattern()) {
            return true;
        }

        // User can see system_buckets in any database so we consider them to have permission in
        // this database
        if (privRsrc.isAnySystemBucketsCollectionInAnyDB()) {
            return true;
        }

        // If the user has an exact namespace privilege on a collection in this database, they
        // have access to a resource in this database.
        if (privRsrc.isExactNamespacePattern() && (privRsrc.dbNameToMatch() == dbname)) {
            return true;
        }

        // If the user has an exact namespace privilege on a system.buckets collection in this
        // database, they have access to a resource in this database.
        if (privRsrc.isExactSystemBucketsCollection() && (privRsrc.dbNameToMatch() == dbname)) {
            return true;
        }
    }

    return false;
}

bool AuthorizationSessionImpl::isAuthorizedForAnyActionOnResource(const ResourcePattern& resource) {
    _contract.addAccessCheck(AccessCheckEnum::kIsAuthorizedForAnyActionOnResource);

    if (_externalState->shouldIgnoreAuthChecks()) {
        return true;
    }

    if (_authenticatedUser == boost::none) {
        return false;
    }

    const auto& user = _authenticatedUser.value();
    auth::ResourcePatternSearchList search(resource);
    return std::any_of(search.cbegin(), search.cend(), [&user](const auto& pattern) {
        return user->hasActionsForResource(pattern);
    });
}


bool AuthorizationSessionImpl::_isAuthorizedForPrivilege(const Privilege& privilege) {
    _contract.addPrivilege(privilege);

    const auto& rp = privilege.getResourcePattern();
    auth::ResourcePatternSearchList search(rp);
    ActionSet unmetRequirements = privilege.getActions();
    for (const auto& priv : _getDefaultPrivileges()) {
        for (auto patternIt = search.cbegin(); patternIt != search.cend(); ++patternIt) {
            if (priv.getResourcePattern() != *patternIt) {
                continue;
            }

            unmetRequirements.removeAllActionsFromSet(priv.getActions());
            if (unmetRequirements.empty()) {
                return true;
            }
        }
    }

    if (_authenticatedUser == boost::none) {
        return false;
    }

    const auto& user = _authenticatedUser.value();
    // Safeguard that cross-tenant privileges are only granted when users have cluster-useTenant.
    if (!MONGO_unlikely(_mayUseTenant) && user->getName().getTenant() != rp.tenantId()) {
        return unmetRequirements.empty();
    }
    return std::any_of(search.cbegin(), search.cend(), [&](const auto& pattern) {
        unmetRequirements.removeAllActionsFromSet(user->getActionsForResource(pattern));
        return unmetRequirements.empty();
    });
}

void AuthorizationSessionImpl::setImpersonatedUserData(const UserName& username,
                                                       const std::vector<RoleName>& roles) {
    _impersonatedUserName = username;
    _impersonatedRoleNames = roles;
    _impersonationFlag = true;
}

bool AuthorizationSessionImpl::isCoauthorizedWithClient(Client* opClient, WithLock opClientLock) {
    _contract.addAccessCheck(AccessCheckEnum::kIsCoauthorizedWithClient);
    auto getUserName = [](AuthorizationSession* authSession) {
        if (authSession->isImpersonating()) {
            return authSession->getImpersonatedUserName();
        } else {
            return authSession->getAuthenticatedUserName();
        }
    };

    if (auto myname = getUserName(this)) {
        return myname == getUserName(AuthorizationSession::get(opClient));
    } else {
        return false;
    }
}

bool AuthorizationSessionImpl::isCoauthorizedWith(const boost::optional<UserName>& userName) {
    _contract.addAccessCheck(AccessCheckEnum::kIsCoauthorizedWith);
    if (!getAuthorizationManager().isAuthEnabled()) {
        return true;
    }

    return getAuthenticatedUserName() == userName;
}

boost::optional<UserName> AuthorizationSessionImpl::getImpersonatedUserName() {
    _contract.addAccessCheck(AccessCheckEnum::kGetImpersonatedUserName);

    return _impersonatedUserName;
}

RoleNameIterator AuthorizationSessionImpl::getImpersonatedRoleNames() {
    _contract.addAccessCheck(AccessCheckEnum::kGetImpersonatedRoleNames);

    return makeRoleNameIterator(_impersonatedRoleNames.begin(), _impersonatedRoleNames.end());
}

bool AuthorizationSessionImpl::isUsingLocalhostBypass() {
    _contract.addAccessCheck(AccessCheckEnum::kIsUsingLocalhostBypass);

    return getAuthorizationManager().isAuthEnabled() && _externalState->shouldAllowLocalhost();
}

// Clear the vectors of impersonated usernames and roles.
void AuthorizationSessionImpl::clearImpersonatedUserData() {
    _impersonatedUserName = boost::none;
    _impersonatedRoleNames.clear();
    _impersonationFlag = false;
}


bool AuthorizationSessionImpl::isImpersonating() const {
    return _impersonationFlag;
}

auto AuthorizationSessionImpl::checkCursorSessionPrivilege(
    OperationContext* const opCtx, const boost::optional<LogicalSessionId> cursorSessionId)
    -> Status {
    _contract.addAccessCheck(AccessCheckEnum::kCheckCursorSessionPrivilege);

    auto nobodyIsLoggedIn = [authSession = this] {
        return !authSession->isAuthenticated();
    };

    auto authHasImpersonatePrivilege = [authSession = this] {
        return authSession->isAuthorizedForPrivilege(
            Privilege(ResourcePattern::forClusterResource(authSession->getUserTenantId()),
                      ActionType::impersonate));
    };

    auto authIsOn = [authSession = this] {
        return authSession->getAuthorizationManager().isAuthEnabled();
    };

    auto sessionIdToStringOrNone =
        [](const boost::optional<LogicalSessionId>& sessionId) -> std::string {
        if (sessionId) {
            return str::stream() << *sessionId;
        }
        return "none";
    };

    // If the cursor has a session then one of the following must be true:
    // 1: context session id must match cursor session id.
    // 2: user must be magic special (__system, or background task, etc).

    // We do not check the user's ID against the cursor's notion of a user ID, since higher level
    // auth checks will check that for us anyhow.
    if (authIsOn() &&  // If the authorization is not on, then we permit anybody to do anything.
        cursorSessionId != opCtx->getLogicalSessionId() &&  // If the cursor's session doesn't match
                                                            // the Operation Context's session, then
                                                            // we should forbid the operation even
                                                            // when the cursor has no session.
        !nobodyIsLoggedIn() &&          // Unless, for some reason a user isn't actually using this
                                        // Operation Context (which implies a background job
        !authHasImpersonatePrivilege()  // Or if the user has an impersonation privilege, in which
                                        // case, the user gets to sidestep certain checks.
    ) {
        return Status{ErrorCodes::Unauthorized,
                      str::stream()
                          << "Cursor session id (" << sessionIdToStringOrNone(cursorSessionId)
                          << ") is not the same as the operation context's session id ("
                          << sessionIdToStringOrNone(opCtx->getLogicalSessionId()) << ")"};
    }

    return Status::OK();
}

void AuthorizationSessionImpl::verifyContract(const AuthorizationContract* contract) const {
    if (contract == nullptr) {
        return;
    }

    if (!checkContracts()) {
        return;
    }

    // Make a mutable copy so that the common auth checks can be added.
    auto tempContract = *contract;

    // Certain access checks are done by code common to all commands.
    //
    // The first two checks are done by initializeOperationSessionInfo
    tempContract.addAccessCheck(AccessCheckEnum::kIsUsingLocalhostBypass);
    tempContract.addAccessCheck(AccessCheckEnum::kIsAuthenticated);

    // These checks are done by auditing
    tempContract.addAccessCheck(AccessCheckEnum::kGetAuthenticatedUserName);
    tempContract.addAccessCheck(AccessCheckEnum::kGetAuthenticatedRoleNames);
    tempContract.addAccessCheck(AccessCheckEnum::kGetImpersonatedUserName);
    tempContract.addAccessCheck(AccessCheckEnum::kGetImpersonatedRoleNames);

    // Since internal sessions are started by the server, the generated authorization contract is
    // missing the following user access checks, so we add them here to allow commands that spawn
    // internal sessions to pass this authorization check.
    tempContract.addAccessCheck(AccessCheckEnum::kGetAuthenticatedUser);
    tempContract.addAccessCheck(AccessCheckEnum::kLookupUser);

    // "internal" comes from readRequestMetadata and sharded clusters
    // "advanceClusterTime" is an implicit check in clusters in metadata handling
    tempContract.addPrivilege(Privilege(ResourcePattern::forClusterResource(boost::none),
                                        {ActionType::advanceClusterTime, ActionType::internal}));

    // Implicitly checked often to keep mayBypassWriteBlockingMode() fast
    tempContract.addPrivilege(Privilege(ResourcePattern::forClusterResource(boost::none),
                                        ActionType::bypassWriteBlockingMode));

    // Implicitly checked often to keep useTenant checks fast
    tempContract.addPrivilege(
        Privilege(ResourcePattern::forClusterResource(boost::none), ActionType::useTenant));

    // Needed for internal sessions started by the server.
    tempContract.addPrivilege(Privilege(ResourcePattern::forClusterResource(boost::none),
                                        ActionType::issueDirectShardOperations));

    uassert(5452401,
            "Authorization Session contains more authorization checks then permitted by contract.",
            tempContract.contains(_contract));
}

void AuthorizationSessionImpl::_updateInternalAuthorizationState() {
    // Update the authenticated role names vector to reflect current state.
    _authenticatedRoleNames.clear();
    if (_authenticatedUser == boost::none) {
        _authenticationMode = AuthenticationMode::kNone;
    } else {
        RoleNameIterator roles = _authenticatedUser.value()->getIndirectRoles();
        while (roles.more()) {
            RoleName roleName = roles.next();
            _authenticatedRoleNames.push_back(RoleName(roleName.getRole(), roleName.getDB()));
        }
    }

    // Update cached _mayBypassWriteBlockingMode to reflect current state.
    _mayBypassWriteBlockingMode = getAuthorizationManager().isAuthEnabled()
        ? _isAuthorizedForPrivilege(
              Privilege(ResourcePattern::forClusterResource(getUserTenantId()),
                        ActionType::bypassWriteBlockingMode))
        : true;

    // Update cached _mayUseTenant to reflect current state.
    _mayUseTenant = getAuthorizationManager().isAuthEnabled()
        ? _isAuthorizedForPrivilege(
              Privilege(ResourcePattern::forClusterResource(boost::none), ActionType::useTenant))
        : true;
}

bool AuthorizationSessionImpl::mayBypassWriteBlockingMode() const {
    return MONGO_unlikely(_mayBypassWriteBlockingMode);
}

bool AuthorizationSessionImpl::isExpired() const {
    return _expiredUserName.has_value();
}

}  // namespace mongo
