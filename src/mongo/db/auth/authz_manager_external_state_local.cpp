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


#include "mongo/db/auth/authorization_backend_interface.h"
#include <absl/container/node_hash_set.h>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>
#include <fmt/format.h>
#include <set>
#include <string>
#include <type_traits>
#include <utility>

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/auth/address_restriction.h"
#include "mongo/db/auth/auth_name.h"
#include "mongo/db/auth/auth_types_gen.h"
#include "mongo/db/auth/authorization_backend_local.h"
#include "mongo/db/auth/authz_manager_external_state_local.h"
#include "mongo/db/auth/builtin_roles.h"
#include "mongo/db/auth/parsed_privilege_gen.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/auth/restriction_set.h"
#include "mongo/db/auth/user_document_parser.h"
#include "mongo/db/auth/user_name.h"
#include "mongo/db/concurrency/lock_manager_defs.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/storage/storage_options.h"
#include "mongo/db/tenant_id.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_attr.h"
#include "mongo/logv2/log_component.h"
#include "mongo/stdx/unordered_set.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/duration.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/str.h"
#include "mongo/util/time_support.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kAccessControl


namespace mongo {
using namespace fmt::literals;

using std::vector;
using ResolveRoleOption = AuthzManagerExternalStateLocal::ResolveRoleOption;

namespace {

NamespaceString getUsersCollection(const boost::optional<TenantId>& tenant) {
    return NamespaceString::makeTenantUsersCollection(tenant);
}

NamespaceString getRolesCollection(const boost::optional<TenantId>& tenant) {
    return NamespaceString::makeTenantRolesCollection(tenant);
}

void serializeResolvedRoles(BSONObjBuilder* user,
                            const AuthzManagerExternalState::ResolvedRoleData& data,
                            boost::optional<const BSONObj&> roleDoc = boost::none) {
    BSONArrayBuilder rolesBuilder(user->subarrayStart("inheritedRoles"));
    for (const auto& roleName : data.roles.value()) {
        roleName.serializeToBSON(&rolesBuilder);
    }
    rolesBuilder.doneFast();

    if (data.privileges) {
        BSONArrayBuilder privsBuilder(user->subarrayStart("inheritedPrivileges"));
        if (roleDoc) {
            auto privs = roleDoc.value()["privileges"];
            if (privs) {
                for (const auto& privilege : privs.Obj()) {
                    privsBuilder.append(privilege);
                }
            }
        }
        for (const auto& privilege : data.privileges.value()) {
            privsBuilder.append(privilege.toBSON());
        }
        privsBuilder.doneFast();
    }

    if (data.restrictions) {
        BSONArrayBuilder arBuilder(user->subarrayStart("inheritedAuthenticationRestrictions"));
        if (roleDoc) {
            auto ar = roleDoc.value()["authenticationRestrictions"];
            if ((ar.type() == Array) && (ar.Obj().nFields() > 0)) {
                arBuilder.append(ar);
            }
        }
        if (auto ar = data.restrictions->toBSON(); ar.nFields() > 0) {
            // TODO: SERVER-50283 Refactor UnnamedRestriction BSON serialization APIs.
            for (const auto& elem : ar) {
                arBuilder.append(elem);
            }
        }
        arBuilder.doneFast();
    }
}

/**
 * Make sure the roleDoc as retreived from storage matches expectations for options.
 */
constexpr auto kRolesFieldName = "roles"_sd;
constexpr auto kPrivilegesFieldName = "privileges"_sd;
constexpr auto kAuthenticationRestrictionFieldName = "authenticationRestrictions"_sd;

std::vector<RoleName> filterAndMapRole(BSONObjBuilder* builder,
                                       BSONObj role,
                                       ResolveRoleOption option,
                                       bool liftAuthenticationRestrictions,
                                       const boost::optional<TenantId>& tenant) {
    std::vector<RoleName> subRoles;
    bool sawRestrictions = false;

    for (const auto& elem : role) {
        if (elem.fieldNameStringData() == kRolesFieldName) {
            uassert(
                ErrorCodes::BadValue, "Invalid roles field, expected array", elem.type() == Array);
            for (const auto& roleName : elem.Obj()) {
                subRoles.push_back(RoleName::parseFromBSON(roleName, tenant));
            }
            if (!option.shouldMineRoles()) {
                continue;
            }
        }

        if ((elem.fieldNameStringData() == kPrivilegesFieldName) &&
            (!option.shouldMinePrivileges())) {
            continue;
        }

        if (elem.fieldNameStringData() == kAuthenticationRestrictionFieldName) {
            sawRestrictions = true;
            if (option.shouldMineRestrictions()) {
                if (liftAuthenticationRestrictions) {
                    // For a rolesInfo invocation, we need to lift ARs up into a container.
                    BSONArrayBuilder arBuilder(
                        builder->subarrayStart(kAuthenticationRestrictionFieldName));
                    arBuilder.append(elem);
                    arBuilder.doneFast();
                } else {
                    // For a usersInfo invocation, we leave it as is.
                    builder->append(elem);
                }
            }
            continue;
        }

        builder->append(elem);
    }

    if (!sawRestrictions && (option.shouldMineRestrictions())) {
        builder->append(kAuthenticationRestrictionFieldName, BSONArray());
    }

    return subRoles;
}

ResolveRoleOption makeResolveRoleOption(PrivilegeFormat showPrivileges,
                                        AuthenticationRestrictionsFormat showRestrictions) {
    auto option = ResolveRoleOption::kRoles();
    if (showPrivileges != PrivilegeFormat::kOmit) {
        option.setPrivileges(true /* shouldEnable */);
    }
    if (showRestrictions != AuthenticationRestrictionsFormat::kOmit) {
        option.setRestrictions(true /* shouldEnable */);
    }

    return option;
}

}  // namespace

// If tenantId is none, we're checking whether to enable localhost auth bypass which by definition
// will be a local user.
bool AuthzManagerExternalStateLocal::hasAnyPrivilegeDocuments(OperationContext* opCtx) {
    return AuthorizationManager::get(opCtx->getService())->hasAnyPrivilegeDocuments(opCtx);
}

AuthzManagerExternalStateLocal::RolesLocks::RolesLocks(OperationContext* opCtx,
                                                       const boost::optional<TenantId>& tenant) {
    if (!storageGlobalParams.disableLockFreeReads) {
        _readLockFree = std::make_unique<AutoReadLockFree>(opCtx);
    } else {
        _adminLock = std::make_unique<Lock::DBLock>(opCtx, DatabaseName::kAdmin, LockMode::MODE_IS);
        _rolesLock = std::make_unique<Lock::CollectionLock>(
            opCtx, getRolesCollection(tenant), LockMode::MODE_S);
    }
}

AuthzManagerExternalStateLocal::RolesLocks::~RolesLocks() {
    _readLockFree.reset(nullptr);
    _rolesLock.reset(nullptr);
    _adminLock.reset(nullptr);
}

AuthzManagerExternalStateLocal::RolesLocks AuthzManagerExternalStateLocal::_lockRoles(
    OperationContext* opCtx, const boost::optional<TenantId>& tenant) {
    return AuthzManagerExternalStateLocal::RolesLocks(opCtx, tenant);
}

StatusWith<User> AuthzManagerExternalStateLocal::getUserObject(
    OperationContext* opCtx,
    const UserRequest& userReq,
    const SharedUserAcquisitionStats& userAcquisitionStats) {
    const auto& backend = auth::AuthorizationBackendInterface::get(opCtx->getService());
    invariant(backend);
    return backend->getUserObject(opCtx, userReq, userAcquisitionStats);
}

Status AuthzManagerExternalStateLocal::getUserDescription(
    OperationContext* opCtx,
    const UserRequest& userReq,
    BSONObj* result,
    const SharedUserAcquisitionStats& userAcquisitionStats) {
    const auto& backend = auth::AuthorizationBackendInterface::get(opCtx->getService());
    invariant(backend);
    return backend->getUserDescription(opCtx, userReq, result, userAcquisitionStats);
}

Status AuthzManagerExternalStateLocal::rolesExist(OperationContext* opCtx,
                                                  const std::vector<RoleName>& roleNames) {
    const auto& backend = auth::AuthorizationBackendInterface::get(opCtx->getService());
    invariant(backend);
    return backend->rolesExist(opCtx, roleNames);
}

using ResolvedRoleData = AuthzManagerExternalState::ResolvedRoleData;
StatusWith<ResolvedRoleData> AuthzManagerExternalStateLocal::resolveRoles(
    OperationContext* opCtx, const std::vector<RoleName>& roleNames, ResolveRoleOption option) {
    const auto& backend = auth::AuthorizationBackendInterface::get(opCtx->getService());
    invariant(backend);
    return backend->resolveRoles(opCtx, roleNames, option);
}

Status AuthzManagerExternalStateLocal::getRolesAsUserFragment(
    OperationContext* opCtx,
    const std::vector<RoleName>& roleNames,
    AuthenticationRestrictionsFormat showRestrictions,
    BSONObj* result) {
    auto option = makeResolveRoleOption(PrivilegeFormat::kShowAsUserFragment, showRestrictions);

    BSONObjBuilder fragment;

    BSONArrayBuilder rolesBuilder(fragment.subarrayStart("roles"));
    for (const auto& roleName : roleNames) {
        roleName.serializeToBSON(&rolesBuilder);
    }
    rolesBuilder.doneFast();

    auto swData = resolveRoles(opCtx, roleNames, option);
    if (!swData.isOK()) {
        return swData.getStatus();
    }
    auto data = std::move(swData.getValue());
    data.roles->insert(roleNames.cbegin(), roleNames.cend());
    serializeResolvedRoles(&fragment, data);

    *result = fragment.obj();
    return Status::OK();
}

Status AuthzManagerExternalStateLocal::getRolesDescription(
    OperationContext* opCtx,
    const std::vector<RoleName>& roleNames,
    PrivilegeFormat showPrivileges,
    AuthenticationRestrictionsFormat showRestrictions,
    std::vector<BSONObj>* result) {
    if (showPrivileges == PrivilegeFormat::kShowAsUserFragment) {
        // Shouldn't be called this way, but cope if we are.
        BSONObj fragment;
        auto status = getRolesAsUserFragment(opCtx, roleNames, showRestrictions, &fragment);
        if (status.isOK()) {
            result->push_back(fragment);
        }
        return status;
    }

    auto option = makeResolveRoleOption(showPrivileges, showRestrictions);

    for (const auto& role : roleNames) {
        try {
            BSONObj roleDoc;

            if (auth::isBuiltinRole(role)) {
                // Synthesize builtin role from definition.
                PrivilegeVector privs;
                uassert(ErrorCodes::OperationFailed,
                        "Failed generating builtin role privileges",
                        auth::addPrivilegesForBuiltinRole(role, &privs));

                BSONObjBuilder builtinBuilder;
                builtinBuilder.append("db", role.getDB());
                builtinBuilder.append("role", role.getRole());
                builtinBuilder.append("roles", BSONArray());
                if (showPrivileges == PrivilegeFormat::kShowSeparate) {
                    BSONArrayBuilder builtinPrivs(builtinBuilder.subarrayStart("privileges"));
                    for (const auto& priv : privs) {
                        builtinPrivs.append(priv.toBSON());
                    }
                    builtinPrivs.doneFast();
                }

                roleDoc = builtinBuilder.obj();
            } else {
                auto status =
                    findOne(opCtx, getRolesCollection(role.tenantId()), role.toBSON(), &roleDoc);
                if (status.code() == ErrorCodes::NoMatchingDocument) {
                    continue;
                }
                uassertStatusOK(status);  // throws
            }

            BSONObjBuilder roleBuilder;
            auto subRoles = filterAndMapRole(&roleBuilder, roleDoc, option, true, role.tenantId());
            auto data = uassertStatusOK(resolveRoles(opCtx, subRoles, option));
            data.roles->insert(subRoles.cbegin(), subRoles.cend());
            serializeResolvedRoles(&roleBuilder, data, roleDoc);
            roleBuilder.append("isBuiltin", auth::isBuiltinRole(role));

            result->push_back(roleBuilder.obj());
        } catch (const AssertionException& ex) {
            return {ex.code(),
                    str::stream() << "Failed fetching role '" << role << "': " << ex.reason()};
        }
    }

    return Status::OK();
}

/**
 * Below this point is the implementation of our OpObserver handler.
 *
 * Ops which mutate user documents will invalidate those specific users
 * from the UserCache.
 *
 * Any other privilege related op (mutation to roles or version collection,
 * or command issued on the admin namespace) will invalidate the entire
 * user cache.
 */

namespace {
class AuthzCollection {
public:
    enum class AuthzCollectionType {
        kNone,
        kUsers,
        kRoles,
        kVersion,
        kAdmin,
    };

    AuthzCollection() = default;
    explicit AuthzCollection(const NamespaceString& nss) : _tenant(nss.tenantId()) {
        // Capture events regardless of what Tenant they occured in,
        // invalidators will purge cache on a per-tenant basis as needed.
        auto db = nss.dbName();
        auto coll = nss.coll();
        if (!db.isAdminDB()) {
            return;
        }

        // System-only collections.
        if (coll == NamespaceString::kServerConfigurationNamespace.coll()) {
            _type = AuthzCollectionType::kVersion;
            return;
        }

        if (coll == NamespaceString::kAdminCommandNamespace.coll()) {
            _type = AuthzCollectionType::kAdmin;
            return;
        }

        if (coll == NamespaceString::kSystemUsers) {
            // admin.system.users or {tenantID}_admin.system.users
            _type = AuthzCollectionType::kUsers;
            return;
        }

        if (coll == NamespaceString::kSystemRoles) {
            // admin.system.roles or {tenantID}_admin.system.roles
            _type = AuthzCollectionType::kRoles;
            return;
        }
    }

    operator bool() const {
        return _type != AuthzCollectionType::kNone;
    }

    bool isPrivilegeCollection() const {
        return (_type == AuthzCollectionType::kUsers) || (_type == AuthzCollectionType::kRoles);
    }

    AuthzCollectionType getType() const {
        return _type;
    }

    const boost::optional<TenantId>& tenantId() const {
        return _tenant;
    }

private:
    AuthzCollectionType _type = AuthzCollectionType::kNone;
    boost::optional<TenantId> _tenant;
};

constexpr auto kOpInsert = "i"_sd;
constexpr auto kOpUpdate = "u"_sd;
constexpr auto kOpDelete = "d"_sd;

using InvalidateFn = std::function<void(AuthorizationManager*)>;

/**
 * When we are currently in a WriteUnitOfWork, invalidation of the user cache must wait until
 * after the operation causing the invalidation commits. This is because if in a different thread,
 * the cache is read after invalidation but before the related commit occurs, the cache will be
 * populated with stale data until the next invalidation.
 */
void invalidateUserCacheOnCommit(OperationContext* opCtx, InvalidateFn invalidate) {
    auto unit = shard_role_details::getRecoveryUnit(opCtx);
    if (unit && unit->inUnitOfWork()) {
        unit->onCommit([invalidate = std::move(invalidate)](OperationContext* opCtx,
                                                            boost::optional<Timestamp>) {
            invalidate(AuthorizationManager::get(opCtx->getService()));
        });
    } else {
        invalidate(AuthorizationManager::get(opCtx->getService()));
    }
}

void _invalidateUserCache(OperationContext* opCtx,
                          AuthorizationManagerImpl* authzManager,
                          StringData op,
                          AuthzCollection coll,
                          const BSONObj& o,
                          const BSONObj* o2) {
    if ((coll.getType() == AuthzCollection::AuthzCollectionType::kUsers) &&
        ((op == kOpInsert) || (op == kOpUpdate) || (op == kOpDelete))) {
        const BSONObj* src = (op == kOpUpdate) ? o2 : &o;
        auto id = (*src)["_id"].str();
        auto splitPoint = id.find('.');
        if (splitPoint == std::string::npos) {
            invalidateUserCacheOnCommit(opCtx, [](auto* am) { am->invalidateUserCache(); });
            return;
        }
        UserName userName(id.substr(splitPoint + 1), id.substr(0, splitPoint), coll.tenantId());
        invalidateUserCacheOnCommit(opCtx, [userName = std::move(userName)](auto* am) {
            am->invalidateUserByName(userName);
        });
    } else if (const auto& tenant = coll.tenantId()) {
        invalidateUserCacheOnCommit(opCtx, [tenantId = tenant.value()](auto* am) {
            am->invalidateUsersByTenant(tenantId);
        });
    } else {
        invalidateUserCacheOnCommit(opCtx, [](auto* am) { am->invalidateUserCache(); });
    }
}
}  // namespace

void AuthzManagerExternalStateLocal::logOp(OperationContext* opCtx,
                                           AuthorizationManagerImpl* authzManager,
                                           StringData op,
                                           const NamespaceString& nss,
                                           const BSONObj& o,
                                           const BSONObj* o2) {
    AuthzCollection coll(nss);
    if (!coll) {
        return;
    }

    _invalidateUserCache(opCtx, authzManager, op, coll, o, o2);
}

}  // namespace mongo
