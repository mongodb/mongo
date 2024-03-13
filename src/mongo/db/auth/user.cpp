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

#include "mongo/db/auth/user.h"

#include <absl/container/node_hash_map.h>
#include <absl/container/node_hash_set.h>
#include <absl/meta/type_traits.h>
#include <cstddef>
#include <vector>


#include "mongo/base/data_range.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/crypto/sha1_block.h"
#include "mongo/crypto/sha256_block.h"
#include "mongo/db/auth/auth_name.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/auth/restriction_environment.h"
#include "mongo/db/auth/role_name.h"
#include "mongo/db/auth/user_name.h"
#include "mongo/db/multitenancy_gen.h"
#include "mongo/util/str.h"
#include "mongo/util/uuid.h"

namespace mongo {

namespace {
// Field names used when serializing User objects to BSON for usersInfo.
constexpr auto kIdFieldName = "_id"_sd;
constexpr auto kUserIdFieldName = "userId"_sd;
constexpr auto kUserFieldName = "user"_sd;
constexpr auto kDbFieldName = "db"_sd;
constexpr auto kMechanismsFieldName = "mechanisms"_sd;
constexpr auto kCredentialsFieldName = "credentials"_sd;
constexpr auto kRolesFieldName = "roles"_sd;
constexpr auto kInheritedRolesFieldName = "inheritedRoles"_sd;
constexpr auto kInheritedPrivilegesFieldName = "inheritedPrivileges"_sd;
constexpr auto kInheritedAuthenticationRestrictionsFieldName =
    "inheritedAuthenticationRestrictions"_sd;
constexpr auto kAuthenticationRestrictionsFieldName = "authenticationRestrictions"_sd;

SHA256Block computeDigest(const UserName& name) {
    auto fn = name.getDisplayName();
    return SHA256Block::computeHash({ConstDataRange(fn.c_str(), fn.size())});
};

}  // namespace

User::User(UserRequest request)
    : _request(std::move(request)), _isInvalidated(false), _digest(computeDigest(_request.name)) {}

template <>
User::SCRAMCredentials<SHA1Block>& User::CredentialData::scram<SHA1Block>() {
    return scram_sha1;
}
template <>
const User::SCRAMCredentials<SHA1Block>& User::CredentialData::scram<SHA1Block>() const {
    return scram_sha1;
}

template <>
User::SCRAMCredentials<SHA256Block>& User::CredentialData::scram<SHA256Block>() {
    return scram_sha256;
}
template <>
const User::SCRAMCredentials<SHA256Block>& User::CredentialData::scram<SHA256Block>() const {
    return scram_sha256;
}

RoleNameIterator User::getRoles() const {
    return makeRoleNameIteratorForContainer(_roles);
}

RoleNameIterator User::getIndirectRoles() const {
    return makeRoleNameIteratorForContainer(_indirectRoles);
}

bool User::hasRole(const RoleName& roleName) const {
    return _roles.count(roleName);
}

const User::CredentialData& User::getCredentials() const {
    return _credentials;
}

ActionSet User::getActionsForResource(const ResourcePattern& resource) const {
    if (auto it = _privileges.find(resource); it != _privileges.end()) {
        return it->second.getActions();
    }

    return ActionSet();
}

bool User::hasActionsForResource(const ResourcePattern& resource) const {
    return !getActionsForResource(resource).empty();
}

void User::setCredentials(const CredentialData& credentials) {
    _credentials = credentials;
}

void User::setRoles(RoleNameIterator roles) {
    _roles.clear();
    while (roles.more()) {
        _roles.insert(roles.next());
    }
}

void User::setIndirectRoles(RoleNameIterator indirectRoles) {
    _indirectRoles.clear();
    while (indirectRoles.more()) {
        _indirectRoles.push_back(indirectRoles.next());
    }
    // Keep indirectRoles sorted for more efficient comparison against other users.
    std::sort(_indirectRoles.begin(), _indirectRoles.end());
}

void User::setPrivileges(const PrivilegeVector& privileges) {
    _privileges.clear();
    for (size_t i = 0; i < privileges.size(); ++i) {
        const Privilege& privilege = privileges[i];
        _privileges[privilege.getResourcePattern()] = privilege;
    }
}

void User::addRole(const RoleName& roleName) {
    _roles.insert(roleName);
}

void User::addRoles(const std::vector<RoleName>& roles) {
    for (std::vector<RoleName>::const_iterator it = roles.begin(); it != roles.end(); ++it) {
        addRole(*it);
    }
}

void User::addPrivilege(const Privilege& privilegeToAdd) {
    ResourcePrivilegeMap::iterator it = _privileges.find(privilegeToAdd.getResourcePattern());
    if (it == _privileges.end()) {
        // No privilege exists yet for this resource
        _privileges.insert(std::make_pair(privilegeToAdd.getResourcePattern(), privilegeToAdd));
    } else {
        dassert(it->first == privilegeToAdd.getResourcePattern());
        it->second.addActions(privilegeToAdd.getActions());
    }
}

void User::addPrivileges(const PrivilegeVector& privileges) {
    for (PrivilegeVector::const_iterator it = privileges.begin(); it != privileges.end(); ++it) {
        addPrivilege(*it);
    }
}

void User::setRestrictions(RestrictionDocuments restrictions) & {
    _restrictions = std::move(restrictions);
}

void User::setIndirectRestrictions(RestrictionDocuments restrictions) & {
    _indirectRestrictions = std::move(restrictions);
}

Status User::validateRestrictions(OperationContext* opCtx) const {
    auto& transportSession = opCtx->getClient()->session();
    if (!transportSession) {
        // If Client has no transport session, it must be internal system connection
        invariant(opCtx->getClient()->isFromSystemConnection());
        return Status::OK();
    }

    auto& env = transportSession->getAuthEnvironment();
    auto status = _restrictions.validate(env);
    if (!status.isOK()) {
        return {status.code(),
                str::stream() << "Evaluation of direct authentication restrictions failed: "
                              << status.reason()};
    }

    status = _indirectRestrictions.validate(env);
    if (!status.isOK()) {
        return {status.code(),
                str::stream() << "Evaluation of indirect authentication restrictions failed: "
                              << status.reason()};
    }

    return Status::OK();
}

void User::reportForUsersInfo(BSONObjBuilder* builder,
                              bool showCredentials,
                              bool showPrivileges,
                              bool showAuthenticationRestrictions) const {
    builder->append(kIdFieldName, getName().getUnambiguousName());
    UUID::fromCDR(ConstDataRange(_id)).appendToBuilder(builder, kUserIdFieldName);
    builder->append(kUserFieldName, getName().getUser());
    builder->append(kDbFieldName, getName().getDB());

    BSONArrayBuilder mechanismNamesBuilder(builder->subarrayStart(kMechanismsFieldName));
    for (StringData mechanism : _credentials.toMechanismsVector()) {
        mechanismNamesBuilder.append(mechanism);
    }
    mechanismNamesBuilder.doneFast();

    BSONArrayBuilder rolesBuilder(builder->subarrayStart(kRolesFieldName));
    for (const auto& role : _roles) {
        role.serializeToBSON(&rolesBuilder);
    }
    rolesBuilder.doneFast();

    if (showCredentials) {
        BSONObjBuilder credentialsBuilder(builder->subobjStart(kCredentialsFieldName));
        _credentials.toBSON(&credentialsBuilder);
        credentialsBuilder.doneFast();
    }

    if (showPrivileges || showAuthenticationRestrictions) {
        BSONArrayBuilder inheritedRolesBuilder(builder->subarrayStart(kInheritedRolesFieldName));
        for (const auto& indirectRole : _indirectRoles) {
            indirectRole.serializeToBSON(&inheritedRolesBuilder);
        }
        inheritedRolesBuilder.doneFast();

        BSONArrayBuilder privsBuilder(builder->subarrayStart(kInheritedPrivilegesFieldName));
        for (const auto& resourceToPrivilege : _privileges) {
            privsBuilder.append(resourceToPrivilege.second.toBSON());
        }
        privsBuilder.doneFast();

        BSONArray indirectRestrictionsArr = _indirectRestrictions.toBSON();
        builder->append(kInheritedAuthenticationRestrictionsFieldName, indirectRestrictionsArr);
    }

    if (showAuthenticationRestrictions) {
        // The user document parser expects an array of documents, where each document represents
        // a restriction. Since _restrictions is of type RestrictionDocuments, its serialization
        // logic supports multiple arrays of documents rather than just one. Therefore, we only
        // should append the first array here.
        BSONArray authenticationRestrictionsArr = _restrictions.toBSON();
        if (authenticationRestrictionsArr.nFields() == 0) {
            builder->append(kAuthenticationRestrictionsFieldName, BSONArray());
        } else {
            builder->append(kAuthenticationRestrictionsFieldName,
                            BSONArray(authenticationRestrictionsArr.begin()->Obj()));
        }
    }
}

bool User::hasDifferentRoles(const User& otherUser) const {
    // If the number of direct or indirect roles in the users' are not the same, they have
    // different roles.
    if (_roles.size() != otherUser._roles.size() ||
        _indirectRoles.size() != otherUser._indirectRoles.size()) {
        return true;
    }

    // At this point, it is known that the users have the same number of direct roles. The
    // direct roles sets are equivalent if all of the roles in the first user's directRoles are
    // also in the other user's directRoles.
    for (const auto& role : _roles) {
        if (otherUser._roles.find(role) == otherUser._roles.end()) {
            return true;
        }
    }

    // Indirect roles should always be sorted.
    dassert(std::is_sorted(_indirectRoles.begin(), _indirectRoles.end()));
    dassert(std::is_sorted(otherUser._indirectRoles.begin(), otherUser._indirectRoles.end()));

    return !std::equal(
        _indirectRoles.begin(), _indirectRoles.end(), otherUser._indirectRoles.begin());
}

}  // namespace mongo
