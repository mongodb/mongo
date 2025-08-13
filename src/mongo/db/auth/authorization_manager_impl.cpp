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


#include "mongo/db/auth/authorization_manager_impl.h"

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
#include "mongo/db/auth/authentication_session.h"
#include "mongo/db/auth/authorization_backend_interface.h"
#include "mongo/db/auth/authorization_manager_global_parameters_gen.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/authorization_session_impl.h"
#include "mongo/db/auth/builtin_roles.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/auth/restriction_set.h"
#include "mongo/db/auth/user.h"
#include "mongo/db/auth/user_request_x509.h"
#include "mongo/db/commands/authentication_commands.h"
#include "mongo/db/curop.h"
#include "mongo/db/global_settings.h"
#include "mongo/db/topology/cluster_role.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/logv2/log.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/duration.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/future.h"
#include "mongo/util/str.h"
#include "mongo/util/testing_proctor.h"
#include "mongo/util/time_support.h"

#include <algorithm>
#include <iterator>
#include <list>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <absl/container/node_hash_map.h>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kAccessControl


namespace mongo {
namespace {

std::shared_ptr<UserHandle> createSystemUserHandle() {
    auto user = std::make_shared<UserHandle>(
        User(std::make_unique<UserRequestGeneral>(UserName("__system", "local"), boost::none)));

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

}  // namespace

AuthorizationManagerImpl::AuthorizationManagerImpl(Service* service,
                                                   std::unique_ptr<AuthorizationRouter> authzRouter)
    : _authzRouter(std::move(authzRouter)) {}

AuthorizationManagerImpl::~AuthorizationManagerImpl() = default;

std::unique_ptr<AuthorizationSession> AuthorizationManagerImpl::makeAuthorizationSession(
    Client* client) {
    return std::make_unique<AuthorizationSessionImpl>(
        _authzRouter->makeAuthzSessionExternalState(client), client);
}

void AuthorizationManagerImpl::setShouldValidateAuthSchemaOnStartup(bool validate) {
    _startupAuthSchemaValidation = validate;
}

bool AuthorizationManagerImpl::shouldValidateAuthSchemaOnStartup() {
    return _startupAuthSchemaValidation;
}

OID AuthorizationManagerImpl::getCacheGeneration() {
    return _authzRouter->getCacheGeneration();
}

AuthorizationRouter* AuthorizationManagerImpl::getAuthorizationRouter_forTest() {
    return _authzRouter.get();
}

void AuthorizationManagerImpl::setAuthEnabled(bool enabled) {
    if (_authEnabled == enabled) {
        return;
    }

    tassert(ErrorCodes::OperationFailed,
            "Auth may not be disabled once enabled except for unit tests",
            enabled || TestingProctor::instance().isEnabled());

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

    bool privDocsExist = _authzRouter->hasAnyPrivilegeDocuments(opCtx);

    if (privDocsExist) {
        _privilegeDocsExist.store(true);
    }

    return _privilegeDocsExist.load();
}

void AuthorizationManagerImpl::notifyDDLOperation(OperationContext* opCtx,
                                                  StringData op,
                                                  const NamespaceString& nss,
                                                  const BSONObj& o,
                                                  const BSONObj* o2) {
    _authzRouter->notifyDDLOperation(opCtx, op, nss, o, o2);
}

StatusWith<UserHandle> AuthorizationManagerImpl::acquireUser(OperationContext* opCtx,
                                                             std::unique_ptr<UserRequest> request) {
    return _authzRouter->acquireUser(opCtx, std::move(request));
}

StatusWith<UserHandle> AuthorizationManagerImpl::reacquireUser(OperationContext* opCtx,
                                                               const UserHandle& user) {
    return _authzRouter->reacquireUser(opCtx, user);
}

void AuthorizationManagerImpl::invalidateUserByName(const UserName& userName) {
    _authzRouter->invalidateUserByName(userName);
}

void AuthorizationManagerImpl::invalidateUsersFromDB(const DatabaseName& dbname) {
    _authzRouter->invalidateUsersFromDB(dbname);
}

void AuthorizationManagerImpl::invalidateUsersByTenant(const boost::optional<TenantId>& tenant) {
    _authzRouter->invalidateUsersByTenant(tenant);
}

void AuthorizationManagerImpl::invalidateUserCache() {
    _authzRouter->invalidateUserCache();
}

Status AuthorizationManagerImpl::refreshExternalUsers(OperationContext* opCtx) {
    return _authzRouter->refreshExternalUsers(opCtx);
}

Status AuthorizationManagerImpl::initialize(OperationContext* opCtx) {
    if (auto* backend = auth::AuthorizationBackendInterface::get(opCtx->getService()); backend) {
        return backend->initialize(opCtx);
    }

    invalidateUserCache();
    return Status::OK();
}

std::vector<AuthorizationRouter::CachedUserInfo> AuthorizationManagerImpl::getUserCacheInfo()
    const {
    return _authzRouter->getUserCacheInfo();
}

}  // namespace mongo
