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

#include "mongo/db/auth/authorization_backend_local.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/auth/address_restriction.h"
#include "mongo/db/auth/auth_name.h"
#include "mongo/db/auth/auth_types_gen.h"
#include "mongo/db/auth/builtin_roles.h"
#include "mongo/db/auth/parsed_privilege_gen.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/auth/restriction_set.h"
#include "mongo/db/auth/user_document_parser.h"
#include "mongo/db/auth/user_name.h"
#include "mongo/db/commands/user_management_commands_gen.h"
#include "mongo/db/curop.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/local_catalog/lock_manager/lock_manager_defs.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/aggregate_command_gen.h"
#include "mongo/db/pipeline/aggregation_request_helper.h"
#include "mongo/db/query/client_cursor/cursor_response.h"
#include "mongo/db/storage/storage_options.h"
#include "mongo/db/tenant_id.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/logv2/log.h"
#include "mongo/rpc/op_msg_rpc_impls.h"
#include "mongo/stdx/unordered_set.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/duration.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/str.h"
#include "mongo/util/time_support.h"

#include <set>
#include <string>
#include <type_traits>
#include <utility>

#include <absl/container/node_hash_set.h>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>
#include <fmt/format.h>


#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kAccessControl


namespace mongo::auth {

using ResolvedRoleData = AuthorizationBackendInterface::ResolvedRoleData;
using ResolveRoleOption = AuthorizationBackendInterface::ResolveRoleOption;

Status AuthorizationBackendLocal::findOne(OperationContext* opCtx,
                                          const NamespaceString& nss,
                                          const BSONObj& query,
                                          BSONObj* result) {
    auto collection = acquireCollectionMaybeLockFree(
        opCtx,
        CollectionAcquisitionRequest::fromOpCtx(
            opCtx, nss, AcquisitionPrerequisites::OperationType::kRead));

    BSONObj found;
    if (Helpers::findOne(opCtx, collection, query, found)) {
        *result = found.getOwned();
        return Status::OK();
    }
    return {ErrorCodes::NoMatchingDocument,
            str::stream() << "No document in " << nss.toStringForErrorMsg() << " matches "
                          << query};
}

namespace {

Status query(OperationContext* opCtx,
             const NamespaceString& collectionName,
             const BSONObj& filter,
             const BSONObj& projection,
             const std::function<void(const BSONObj&)>& resultProcessor) {
    try {
        DBDirectClient client(opCtx);
        FindCommandRequest findRequest{collectionName};
        findRequest.setFilter(filter);
        findRequest.setProjection(projection);
        client.find(std::move(findRequest), resultProcessor);
        return Status::OK();
    } catch (const DBException& e) {
        return e.toStatus();
    }
}

bool hasOne(OperationContext* opCtx, const NamespaceString& nss, const BSONObj& query) {
    auto collection = acquireCollectionMaybeLockFree(
        opCtx,
        CollectionAcquisitionRequest::fromOpCtx(
            opCtx, nss, AcquisitionPrerequisites::OperationType::kRead));
    return !Helpers::findOne(opCtx, collection, query).isNull();
}

}  // namespace
namespace {
NamespaceString usersNSS(const boost::optional<TenantId>& tenant) {
    if (tenant) {
        return NamespaceString::makeTenantUsersCollection(tenant);
    } else {
        return NamespaceString::kAdminUsersNamespace;
    }
}

NamespaceString rolesNSS(const boost::optional<TenantId>& tenant) {
    if (tenant) {
        return NamespaceString::makeTenantRolesCollection(tenant);
    } else {
        return NamespaceString::kAdminRolesNamespace;
    }
}

NamespaceString getRolesCollection(const boost::optional<TenantId>& tenant) {
    return NamespaceString::makeTenantRolesCollection(tenant);
}

void serializeResolvedRoles(BSONObjBuilder* user,
                            const ResolvedRoleData& data,
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
            if ((ar.type() == BSONType::array) && (ar.Obj().nFields() > 0)) {
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
            uassert(ErrorCodes::BadValue,
                    "Invalid roles field, expected array",
                    elem.type() == BSONType::array);
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

Status AuthorizationBackendLocal::makeRoleNotFoundStatus(
    const stdx::unordered_set<RoleName>& unknownRoles) {
    dassert(unknownRoles.size());

    char delim = ':';
    StringBuilder sb;
    sb << "Could not find role";
    if (unknownRoles.size() > 1) {
        sb << 's';
    }
    for (const auto& unknownRole : unknownRoles) {
        sb << delim << ' ' << unknownRole;
        delim = ',';
    }
    return {ErrorCodes::RoleNotFound, sb.str()};
}

AuthorizationBackendLocal::RolesSnapshot::RolesSnapshot(OperationContext* opCtx) {
    _readLockFree = std::make_unique<AutoReadLockFree>(opCtx);
}

AuthorizationBackendLocal::RolesSnapshot::~RolesSnapshot() {
    _readLockFree.reset(nullptr);
}

AuthorizationBackendLocal::RolesSnapshot AuthorizationBackendLocal::_snapshotRoles(
    OperationContext* opCtx) {
    return AuthorizationBackendLocal::RolesSnapshot(opCtx);
}

Status AuthorizationBackendLocal::rolesExist(OperationContext* opCtx,
                                             const std::vector<RoleName>& roleNames) {
    // Perform DB queries for user-defined roles (skipping builtin roles).
    stdx::unordered_set<RoleName> unknownRoles;
    for (const auto& roleName : roleNames) {
        if (!auth::isBuiltinRole(roleName) &&
            !hasOne(opCtx, rolesNSS(roleName.tenantId()), roleName.toBSON())) {
            unknownRoles.insert(roleName);
        }
    }

    // If anything remains, raise it as an unknown role error.
    if (!unknownRoles.empty()) {
        return makeRoleNotFoundStatus(unknownRoles);
    }

    return Status::OK();
}

StatusWith<ResolvedRoleData> AuthorizationBackendLocal::resolveRoles(
    OperationContext* opCtx, const std::vector<RoleName>& roleNames, ResolveRoleOption option) try {
    using RoleNameSet = typename decltype(ResolvedRoleData::roles)::value_type;
    const bool processRoles = option.shouldMineRoles();
    const bool processPrivs = option.shouldMinePrivileges();
    const bool processRests = option.shouldMineRestrictions();
    const bool walkIndirect = !option.shouldMineDirectOnly();
    const bool skipUnknownRolesLog = option.shouldIgnoreUnknown();
    IDLParserContext idlctx("resolveRoles");

    RoleNameSet inheritedRoles;
    PrivilegeVector inheritedPrivileges;
    RestrictionDocuments::sequence_type inheritedRestrictions;

    RoleNameSet frontier(roleNames.cbegin(), roleNames.cend());
    RoleNameSet visited;
    while (!frontier.empty()) {
        RoleNameSet nextFrontier;
        for (const auto& role : frontier) {
            visited.insert(role);

            if (auth::isBuiltinRole(role)) {
                if (processPrivs) {
                    invariant(auth::addPrivilegesForBuiltinRole(role, &inheritedPrivileges));
                }
                continue;
            }

            BSONObj roleDoc;
            auto status = findOne(opCtx, rolesNSS(role.tenantId()), role.toBSON(), &roleDoc);
            if (!status.isOK()) {
                if (status.code() == ErrorCodes::NoMatchingDocument) {
                    if (!skipUnknownRolesLog) {
                        LOGV2(5029200, "Role does not exist", "role"_attr = role);
                    }

                    continue;
                }
                return status;
            }

            BSONElement elem;
            if ((processRoles || walkIndirect) && (elem = roleDoc["roles"])) {
                if (elem.type() != BSONType::array) {
                    return {ErrorCodes::BadValue,
                            str::stream()
                                << "Invalid 'roles' field in role document '" << role
                                << "', expected an array but found " << typeName(elem.type())};
                }
                for (const auto& subroleElem : elem.Obj()) {
                    auto subrole = RoleName::parseFromBSON(subroleElem, role.tenantId());
                    if (visited.count(subrole) || nextFrontier.count(subrole)) {
                        continue;
                    }
                    if (walkIndirect) {
                        nextFrontier.insert(subrole);
                    }
                    if (processRoles) {
                        inheritedRoles.insert(std::move(subrole));
                    }
                }
            }

            if (processPrivs && (elem = roleDoc["privileges"])) {
                if (elem.type() != BSONType::array) {
                    return {ErrorCodes::UnsupportedFormat,
                            str::stream()
                                << "Invalid 'privileges' field in role document '" << role << "'"};
                }
                for (const auto& privElem : elem.Obj()) {
                    if (privElem.type() != BSONType::object) {
                        return {ErrorCodes::UnsupportedFormat,
                                fmt::format("Expected privilege document as object, got {}",
                                            typeName(privElem.type()))};
                    }
                    auto pp = auth::ParsedPrivilege::parse(privElem.Obj(), idlctx);
                    Privilege::addPrivilegeToPrivilegeVector(
                        &inheritedPrivileges,
                        Privilege::resolvePrivilegeWithTenant(role.tenantId(), pp));
                }
            }

            if (processRests && (elem = roleDoc["authenticationRestrictions"])) {
                if (elem.type() != BSONType::array) {
                    return {ErrorCodes::UnsupportedFormat,
                            str::stream()
                                << "Invalid 'authenticationRestrictions' field in role document '"
                                << role << "'"};
                }
                inheritedRestrictions.push_back(
                    uassertStatusOK(parseAuthenticationRestriction(BSONArray(elem.Obj()))));
            }
        }
        frontier = std::move(nextFrontier);
    }

    ResolvedRoleData ret;
    if (processRoles) {
        ret.roles = std::move(inheritedRoles);
    }
    if (processPrivs) {
        ret.privileges = std::move(inheritedPrivileges);
    }
    if (processRests) {
        ret.restrictions = RestrictionDocuments(std::move(inheritedRestrictions));
    }

    return ret;
} catch (const AssertionException& ex) {
    return ex.toStatus();
}
namespace {
MONGO_FAIL_POINT_DEFINE(authLocalGetSubRoles);

void handleAuthLocalGetSubRolesFailPoint(const std::vector<RoleName>& directRoles) {
    auto sfp = authLocalGetSubRoles.scoped();
    if (!sfp.isActive()) {
        return;
    }

    IDLParserContext ctx("authLocalGetSubRoles");
    auto delay = AuthLocalGetSubRolesFailPoint::parse(sfp.getData(), ctx).getResolveRolesDelayMS();

    if (delay <= 0) {
        return;
    }

    LOGV2_DEBUG(4859400,
                3,
                "Sleeping prior to merging direct roles, after user acquisition",
                "duration"_attr = Milliseconds(delay),
                "directRoles"_attr = directRoles);
    sleepmillis(delay);
}

}  // namespace

StatusWith<User> AuthorizationBackendLocal::getUserObject(
    OperationContext* opCtx,
    const UserRequest& userReq,
    const SharedUserAcquisitionStats& userAcquisitionStats) try {
    std::vector<RoleName> directRoles;

    User user(userReq.clone());

    const UserRequest* request = user.getUserRequest();
    const UserName& userName = request->getUserName();

    auto RolesSnapshot = _snapshotRoles(opCtx);

    // Set ResolveRoleOption to mine all information from role tree.
    auto options = ResolveRoleOption::kAllInfo();

    bool hasExternalRoles = request->getRoles().has_value();
    if (!hasExternalRoles) {
        // Normal path: Acquire a user from the local store by UserName.
        BSONObj userDoc;
        auto status = findOne(opCtx, usersNSS(userName.tenantId()), userName.toBSON(), &userDoc);
        if (!status.isOK()) {
            if (status == ErrorCodes::NoMatchingDocument) {
                return {ErrorCodes::UserNotFound,
                        str::stream() << "Could not find user \"" << userName.getUser()
                                      << "\" for db \"" << userName.getDB() << "\""};
            }
            return status;
        }

        V2UserDocumentParser userDocParser;
        userDocParser.setTenantId(request->getUserName().tenantId());
        uassertStatusOK(userDocParser.initializeUserFromUserDocument(userDoc, &user));
        for (auto iter = user.getRoles(); iter.more();) {
            directRoles.push_back(iter.next());
        }
    } else {
        // Proxy path.  Some other external mechanism (e.g. X509 or LDAP) has acquired
        // a base user definition with a set of immediate roles.
        // We're being asked to use the local roles collection to derive privileges,
        // subordinate roles, and authentication restrictions.
        const auto& requestRoles = *request->getRoles();
        directRoles.assign(requestRoles.begin(), requestRoles.end());

        User::CredentialData credentials;
        credentials.isExternal = true;
        user.setCredentials(credentials);
        user.setRoles(makeRoleNameIteratorForContainer(directRoles));

        // Update ResolveRoleOption to skip emitting warning logs for unknown roles, since they came
        // from an external source.
        options.setIgnoreUnknown(true /* shouldEnable */);
    }

    if (auto tenant = userName.tenantId()) {
        // Apply TenantID for user to all roles (which are assumed to be part of the same tenant).
        for (auto& role : directRoles) {
            role = RoleName(role.getRole(), role.getDB(), tenant);
        }
    }

    handleAuthLocalGetSubRolesFailPoint(directRoles);

    auto data = uassertStatusOK(resolveRoles(opCtx, directRoles, options));
    data.roles->insert(directRoles.cbegin(), directRoles.cend());
    user.setIndirectRoles(makeRoleNameIteratorForContainer(data.roles.value()));
    user.addPrivileges(data.privileges.value());
    user.setIndirectRestrictions(data.restrictions.value());

    return std::move(user);
} catch (const AssertionException& ex) {
    return ex.toStatus();
}

Status AuthorizationBackendLocal::getUserDescription(
    OperationContext* opCtx,
    const UserRequest& userReq,
    BSONObj* result,
    const SharedUserAcquisitionStats& userAcquisitionStats) try {
    const UserName& userName = userReq.getUserName();
    std::vector<RoleName> directRoles;
    BSONObjBuilder resultBuilder;

    auto RolesSnapshot = _snapshotRoles(opCtx);

    auto options = ResolveRoleOption::kAllInfo();
    bool hasExternalRoles = userReq.getRoles().has_value();
    if (!hasExternalRoles) {
        BSONObj userDoc;
        auto status = findOne(opCtx, usersNSS(userName.tenantId()), userName.toBSON(), &userDoc);
        if (!status.isOK()) {
            if (status == ErrorCodes::NoMatchingDocument) {
                return {ErrorCodes::UserNotFound,
                        str::stream() << "Could not find user \"" << userName.getUser()
                                      << "\" for db \"" << userName.getDB() << "\""};
            }
            return status;
        }

        directRoles =
            filterAndMapRole(&resultBuilder, userDoc, options, false, userName.tenantId());
    } else {
        // Set ResolveRoleOption to include the ignoreUnknownFlag so that external roles that don't
        // exist do not generate warning logs.
        options.setIgnoreUnknown(true /* shouldEnable */);

        uassert(ErrorCodes::BadValue,
                "Illegal combination of pre-defined roles with tenant identifier",
                userName.tenantId() == boost::none);

        // We are able to artifically construct the external user from the request
        resultBuilder.append("_id", str::stream() << userName.getDB() << '.' << userName.getUser());
        resultBuilder.append("user", userName.getUser());
        resultBuilder.append("db", userName.getDB());
        resultBuilder.append("credentials", BSON("external" << true));

        directRoles =
            std::vector<RoleName>(userReq.getRoles()->cbegin(), userReq.getRoles()->cend());
        BSONArrayBuilder rolesBuilder(resultBuilder.subarrayStart("roles"));
        for (const RoleName& role : directRoles) {
            rolesBuilder.append(role.toBSON());
        }
        rolesBuilder.doneFast();
    }

    if (auto tenant = userName.tenantId()) {
        // Apply TenantID for user to all roles (which are assumed to be part of the same tenant).
        for (auto& role : directRoles) {
            role = RoleName(role.getRole(), role.getDB(), tenant);
        }
    }

    handleAuthLocalGetSubRolesFailPoint(directRoles);

    auto data = uassertStatusOK(resolveRoles(opCtx, directRoles, options));
    data.roles->insert(directRoles.cbegin(), directRoles.cend());
    serializeResolvedRoles(&resultBuilder, data);
    *result = resultBuilder.obj();

    return Status::OK();
} catch (const AssertionException& ex) {
    return ex.toStatus();
}

Status AuthorizationBackendLocal::getRolesAsUserFragment(
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

Status AuthorizationBackendLocal::getRolesDescription(
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
                auto status = findOne(opCtx, rolesNSS(role.tenantId()), role.toBSON(), &roleDoc);
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

Status AuthorizationBackendLocal::getRoleDescriptionsForDB(
    OperationContext* opCtx,
    const DatabaseName& dbname,
    PrivilegeFormat showPrivileges,
    AuthenticationRestrictionsFormat showRestrictions,
    bool showBuiltinRoles,
    std::vector<BSONObj>* result) {
    auto option = makeResolveRoleOption(showPrivileges, showRestrictions);

    if (showPrivileges == PrivilegeFormat::kShowAsUserFragment) {
        return {ErrorCodes::IllegalOperation,
                "Cannot get user fragment for all roles in a database"};
    }

    if (showBuiltinRoles) {
        for (const auto& roleName : auth::getBuiltinRoleNamesForDB(dbname)) {
            BSONObjBuilder roleBuilder;

            roleBuilder.append(AuthorizationManager::ROLE_NAME_FIELD_NAME, roleName.getRole());
            roleBuilder.append(AuthorizationManager::ROLE_DB_FIELD_NAME, roleName.getDB());
            roleBuilder.append("isBuiltin", true);

            roleBuilder.append("roles", BSONArray());
            roleBuilder.append("inheritedRoles", BSONArray());

            if (showPrivileges == PrivilegeFormat::kShowSeparate) {
                BSONArrayBuilder privsBuilder(roleBuilder.subarrayStart("privileges"));
                PrivilegeVector privs;
                invariant(auth::addPrivilegesForBuiltinRole(roleName, &privs));
                for (const auto& privilege : privs) {
                    privsBuilder.append(privilege.toBSON());
                }
                privsBuilder.doneFast();

                // Builtin roles have identival privs/inheritedPrivs
                BSONArrayBuilder ipBuilder(roleBuilder.subarrayStart("inheritedPrivileges"));
                for (const auto& privilege : privs) {
                    ipBuilder.append(privilege.toBSON());
                }
                ipBuilder.doneFast();
            }

            if (showRestrictions == AuthenticationRestrictionsFormat::kShow) {
                roleBuilder.append("authenticationRestrictions", BSONArray());
                roleBuilder.append("inheritedAuthenticationRestrictions", BSONArray());
            }

            result->push_back(roleBuilder.obj());
        }
    }

    return query(opCtx,
                 rolesNSS(dbname.tenantId()),
                 BSON(AuthorizationManager::ROLE_DB_FIELD_NAME
                      << dbname.serializeWithoutTenantPrefix_UNSAFE()),
                 BSONObj(),
                 [&](const BSONObj& roleDoc) {
                     BSONObjBuilder roleBuilder;

                     auto subRoles =
                         filterAndMapRole(&roleBuilder, roleDoc, option, true, dbname.tenantId());
                     roleBuilder.append("isBuiltin", false);
                     auto data = uassertStatusOK(resolveRoles(opCtx, subRoles, option));
                     data.roles->insert(subRoles.cbegin(), subRoles.cend());
                     serializeResolvedRoles(&roleBuilder, data, roleDoc);
                     result->push_back(roleBuilder.obj());
                 });
}

std::vector<BSONObj> AuthorizationBackendLocal::performNoPrivilegeNoRestrictionsLookup(
    OperationContext* opCtx, const UsersInfoCommand& cmd) {
    invariant(cmd.getShowPrivileges() == false);
    invariant(cmd.getShowAuthenticationRestrictions() == false);

    std::vector<BSONObj> users;
    const auto& arg = cmd.getCommandParameter();
    const auto& dbname = cmd.getDbName();
    // If you don't need privileges, or authenticationRestrictions, you can just do a
    // regular query on system.users
    std::vector<BSONObj> pipeline;

    if (arg.isAllForAllDBs()) {
        // Leave the pipeline unconstrained, we want to return every user.
    } else if (arg.isAllOnCurrentDB()) {
        pipeline.push_back(BSON("$match" << BSON(AuthorizationManager::USER_DB_FIELD_NAME
                                                 << dbname.serializeWithoutTenantPrefix_UNSAFE())));
    } else {
        invariant(arg.isExact());
        BSONArrayBuilder usersMatchArray;
        for (const auto& userName : arg.getElements(dbname)) {
            usersMatchArray.append(userName.toBSON());
        }
        pipeline.push_back(BSON("$match" << BSON("$or" << usersMatchArray.arr())));
    }

    // Order results by user field then db field, matching how UserNames are ordered
    pipeline.push_back(BSON("$sort" << BSON("user" << 1 << "db" << 1)));

    // Rewrite the credentials object into an array of its fieldnames.
    pipeline.push_back(
        BSON("$addFields" << BSON(
                 "mechanisms" << BSON(
                     "$map" << BSON("input" << BSON("$objectToArray" << "$credentials") << "as"
                                            << "cred"
                                            << "in"
                                            << "$$cred.k")))));

    // Authentication restrictions are only rendered in the single user case.
    BSONArrayBuilder fieldsToRemoveBuilder;
    fieldsToRemoveBuilder.append("authenticationRestrictions");
    if (!cmd.getShowCredentials()) {
        // Remove credentials as well, they're not required in the output.
        fieldsToRemoveBuilder.append("credentials");
    }
    if (!cmd.getShowCustomData()) {
        // Remove customData as well, it's not required in the output.
        fieldsToRemoveBuilder.append("customData");
    }
    pipeline.push_back(BSON("$unset" << fieldsToRemoveBuilder.arr()));

    // Handle a user specified filter.
    if (auto filter = cmd.getFilter()) {
        pipeline.push_back(BSON("$match" << *filter));
    }

    DBDirectClient client(opCtx);
    AggregateCommandRequest aggRequest(usersNSS(dbname.tenantId()), std::move(pipeline));
    // Impose no cursor privilege requirements, as cursor is drained internally

    auto swCursor = DBClientCursor::fromAggregationRequest(&client, aggRequest, false, false);
    uassertStatusOK(swCursor.getStatus());

    auto cursor = std::move(swCursor.getValue());
    while (cursor->more()) {
        users.push_back(cursor->next().getOwned());
    }

    return users;
}

std::vector<BSONObj> AuthorizationBackendLocal::performLookupWithPrivilegesAndRestrictions(
    OperationContext* opCtx, const UsersInfoCommand& cmd, const std::vector<UserName>& usernames) {
    std::vector<BSONObj> users;

    for (const auto& username : usernames) {
        BSONObj userDetails;
        auto status = getUserDescription(opCtx,
                                         UserRequestGeneral(username, boost::none),
                                         &userDetails,
                                         CurOp::get(opCtx)->getUserAcquisitionStats());
        if (status.code() == ErrorCodes::UserNotFound) {
            continue;
        }
        uassertStatusOK(status);

        // getUserDescription always includes credentials and restrictions, which may need
        // to be stripped out
        BSONObjBuilder strippedUser;
        for (const BSONElement& e : userDetails) {
            if (e.fieldNameStringData() == "credentials") {
                BSONArrayBuilder mechanismNamesBuilder;
                BSONObj mechanismsObj = e.Obj();
                for (const BSONElement& mechanismElement : mechanismsObj) {
                    mechanismNamesBuilder.append(mechanismElement.fieldNameStringData());
                }
                strippedUser.append("mechanisms", mechanismNamesBuilder.arr());

                if (!cmd.getShowCredentials()) {
                    continue;
                }
            }

            if ((e.fieldNameStringData() == "authenticationRestrictions") &&
                !cmd.getShowAuthenticationRestrictions()) {
                continue;
            }

            if ((e.fieldNameStringData() == "customData") && !cmd.getShowCustomData()) {
                continue;
            }

            strippedUser.append(e);
        }
        users.push_back(strippedUser.obj());
    }

    return users;
}

UsersInfoReply AuthorizationBackendLocal::lookupUsers(OperationContext* opCtx,
                                                      const UsersInfoCommand& cmd) {
    const auto& arg = cmd.getCommandParameter();
    const auto& dbname = cmd.getDbName();

    std::vector<BSONObj> users;

    if (cmd.getShowPrivileges() || cmd.getShowAuthenticationRestrictions()) {
        uassert(ErrorCodes::IllegalOperation,
                "Privilege or restriction details require exact-match usersInfo queries",
                !cmd.getFilter() && arg.isExact());
        users = performLookupWithPrivilegesAndRestrictions(opCtx, cmd, arg.getElements(dbname));
    } else {
        users = performNoPrivilegeNoRestrictionsLookup(opCtx, cmd);
    }

    UsersInfoReply reply;
    reply.setUsers(std::move(users));
    return reply;
}

RolesInfoReply AuthorizationBackendLocal::lookupRoles(OperationContext* opCtx,
                                                      const RolesInfoCommand& cmd) {
    const auto& arg = cmd.getCommandParameter();
    const auto& dbname = cmd.getDbName();

    // Only usersInfo actually supports {forAllDBs: 1} mode.
    invariant(!arg.isAllForAllDBs());

    auto privFmt = *(cmd.getShowPrivileges());
    auto restrictionFormat = cmd.getShowAuthenticationRestrictions()
        ? AuthenticationRestrictionsFormat::kShow
        : AuthenticationRestrictionsFormat::kOmit;

    RolesInfoReply reply;
    if (arg.isAllOnCurrentDB()) {

        uassert(ErrorCodes::IllegalOperation,
                "Cannot get user fragment for all roles in a database",
                privFmt != PrivilegeFormat::kShowAsUserFragment);

        std::vector<BSONObj> roles;
        uassertStatusOK(getRoleDescriptionsForDB(
            opCtx, dbname, privFmt, restrictionFormat, cmd.getShowBuiltinRoles(), &roles));
        reply.setRoles(std::move(roles));
    } else {
        invariant(arg.isExact());
        auto roleNames = arg.getElements(dbname);

        if (privFmt == PrivilegeFormat::kShowAsUserFragment) {
            BSONObj fragment;
            uassertStatusOK(getRolesAsUserFragment(opCtx, roleNames, restrictionFormat, &fragment));
            reply.setUserFragment(fragment);
        } else {
            std::vector<BSONObj> roles;
            uassertStatusOK(
                getRolesDescription(opCtx, roleNames, privFmt, restrictionFormat, &roles));
            reply.setRoles(std::move(roles));
        }
    }

    return reply;
}

namespace {
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
}  // namespace
}  // namespace mongo::auth
