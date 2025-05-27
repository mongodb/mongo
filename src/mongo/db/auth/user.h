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

#pragma once

#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/crypto/sha1_block.h"
#include "mongo/crypto/sha256_block.h"
#include "mongo/db/auth/action_set.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/auth/restriction_set.h"
#include "mongo/db/auth/role_name.h"
#include "mongo/db/auth/user_acquisition_stats.h"
#include "mongo/db/auth/user_name.h"
#include "mongo/db/operation_context.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/stdx/unordered_set.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/base64.h"
#include "mongo/util/read_through_cache.h"

#include <algorithm>
#include <cstdint>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>


namespace mongo {

/**
 * Represents the properties required to request a UserHandle.
 * It contains a UserRequestCacheKey which is hashable and can be
 * used as a cache key.
 */
class UserRequest {
public:
    virtual ~UserRequest() = default;

    class UserRequestCacheKey {
    public:
        UserRequestCacheKey(const UserName& userName, const std::vector<std::string>& hashElements)
            : _hashElements(std::move(hashElements)), _userName(userName) {}

        const UserName& getUserName() const {
            return _userName;
        }

        template <typename H>
        friend H AbslHashValue(H h, const UserRequestCacheKey& key) {
            for (const auto& elem : key.getHashElements()) {
                h = H::combine(std::move(h), elem);
            }
            return h;
        }

        bool operator==(const UserRequestCacheKey& key) const {
            return getHashElements() == key.getHashElements();
        }

        bool operator<(const UserRequestCacheKey& key) const {
            return getHashElements() < key.getHashElements();
        }

        bool operator>(const UserRequestCacheKey& k) const {
            return k < *this;
        }

        bool operator>=(const UserRequestCacheKey& k) const {
            return !(*this < k);
        }

        bool operator<=(const UserRequestCacheKey& k) const {
            return !(k < *this);
        }

    private:
        const std::vector<std::string>& getHashElements() const {
            return _hashElements;
        }

        const std::vector<std::string> _hashElements;

        // We store the userName here for userCacheInfo.
        const UserName _userName;
    };

    enum class UserRequestType { General, X509, OIDC };

    virtual const UserName& getUserName() const = 0;
    virtual const boost::optional<std::set<RoleName>>& getRoles() const = 0;
    virtual UserRequestType getType() const = 0;
    virtual void setRoles(boost::optional<std::set<RoleName>> roles) = 0;
    virtual std::unique_ptr<UserRequest> clone() const = 0;

    /**
     * Version of clone that clones the UserRequest by erasing the roles from
     * the document and re-fetching the roles for OIDC / X509.
     */
    virtual StatusWith<std::unique_ptr<UserRequest>> cloneForReacquire() const = 0;
    virtual UserRequestCacheKey generateUserRequestCacheKey() const = 0;

    static std::vector<std::string> getUserNameAndRolesVector(
        const UserName& userName, const boost::optional<std::set<RoleName>>& roles);
};

/**
 * This is a version of the UserRequest that is used for all authentication types
 * that are not X509 and OIDC.
 */
class UserRequestGeneral : public UserRequest {
public:
    UserRequestGeneral(UserName name, boost::optional<std::set<RoleName>> roles)
        : name(std::move(name)), roles(std::move(roles)) {}

    const UserName& getUserName() const final {
        return name;
    }

    const boost::optional<std::set<RoleName>>& getRoles() const final {
        return roles;
    }

    UserRequestType getType() const override {
        return UserRequestType::General;
    }

    void setRoles(boost::optional<std::set<RoleName>> roles) final {
        this->roles = std::move(roles);
    }

    std::unique_ptr<UserRequest> clone() const override {
        return std::unique_ptr<UserRequest>(
            std::make_unique<UserRequestGeneral>(getUserName(), getRoles()));
    }

    StatusWith<std::unique_ptr<UserRequest>> cloneForReacquire() const override {
        return std::unique_ptr<UserRequest>(
            std::make_unique<UserRequestGeneral>(getUserName(), boost::none));
    }

    UserRequestCacheKey generateUserRequestCacheKey() const override;

protected:
    // The name of the requested user
    UserName name;
    // Any authorization grants which should override and be used in favor of roles acquisition.
    boost::optional<std::set<RoleName>> roles;
};

/**
 * Represents a MongoDB user.  Stores information about the user necessary for access control
 * checks and authentications, such as what privileges this user has, as well as what roles
 * the user belongs to.
 *
 * Every User object is owned by an AuthorizationManager.  The AuthorizationManager is the only
 * one that should construct, modify, or delete a User object.  All other consumers of User must
 * use only the const methods.  The AuthorizationManager is responsible for maintaining the
 * reference count on all User objects it gives out and must not mutate any User objects with
 * a non-zero reference count (except to call invalidate()).  Any consumer of a User object
 * should check isInvalidated() before using it, and if it has been invalidated, it should
 * return the object to the AuthorizationManager and fetch a new User object instance for this
 * user from the AuthorizationManager.
 */
class User {
    User(const User&) = delete;
    User& operator=(const User&) = delete;

public:
    using UserId = std::vector<std::uint8_t>;
    constexpr static auto kSHA1FieldName = "SCRAM-SHA-1"_sd;
    constexpr static auto kSHA256FieldName = "SCRAM-SHA-256"_sd;
    constexpr static auto kExternalFieldName = "external"_sd;
    constexpr static auto kIterationCountFieldName = "iterationCount"_sd;
    constexpr static auto kSaltFieldName = "salt"_sd;
    constexpr static auto kServerKeyFieldName = "serverKey"_sd;
    constexpr static auto kStoredKeyFieldName = "storedKey"_sd;

    template <typename HashBlock>
    struct SCRAMCredentials {
        SCRAMCredentials() : iterationCount(0), salt(""), serverKey(""), storedKey("") {}

        int iterationCount;
        std::string salt;
        std::string serverKey;
        std::string storedKey;

        bool isValid() const {
            constexpr auto kEncodedHashLength = base64::encodedLength(HashBlock::kHashLength);
            constexpr auto kEncodedSaltLength = base64::encodedLength(HashBlock::kHashLength - 4);

            return (iterationCount > 0) && (salt.size() == kEncodedSaltLength) &&
                base64::validate(salt) && (serverKey.size() == kEncodedHashLength) &&
                base64::validate(serverKey) && (storedKey.size() == kEncodedHashLength) &&
                base64::validate(storedKey);
        }

        bool empty() const {
            return !iterationCount && salt.empty() && serverKey.empty() && storedKey.empty();
        }

        void toBSON(BSONObjBuilder* builder) const {
            builder->append(kIterationCountFieldName, iterationCount);
            builder->append(kSaltFieldName, salt);
            builder->append(kStoredKeyFieldName, storedKey);
            builder->append(kServerKeyFieldName, serverKey);
        }
    };

    struct CredentialData {
        CredentialData() : scram_sha1(), scram_sha256(), isExternal(false) {}

        SCRAMCredentials<SHA1Block> scram_sha1;
        SCRAMCredentials<SHA256Block> scram_sha256;
        bool isExternal;

        // Select the template determined version of SCRAMCredentials.
        // For example: creds.scram<SHA1Block>().isValid()
        // is equivalent to creds.scram_sha1.isValid()
        template <typename HashBlock>
        SCRAMCredentials<HashBlock>& scram();

        template <typename HashBlock>
        const SCRAMCredentials<HashBlock>& scram() const;

        void toBSON(BSONObjBuilder* builder) const {
            if (scram_sha1.isValid()) {
                BSONObjBuilder sha1ObjBuilder(builder->subobjStart(kSHA1FieldName));
                scram_sha1.toBSON(&sha1ObjBuilder);
                sha1ObjBuilder.doneFast();
            }
            if (scram_sha256.isValid()) {
                BSONObjBuilder sha256ObjBuilder(builder->subobjStart(kSHA256FieldName));
                scram_sha256.toBSON(&sha256ObjBuilder);
                sha256ObjBuilder.doneFast();
            }
            if (isExternal) {
                builder->append(kExternalFieldName, true);
            }
        }

        std::vector<StringData> toMechanismsVector() const {
            std::vector<StringData> mechanismsVec;
            if (scram_sha1.isValid()) {
                mechanismsVec.push_back(kSHA1FieldName);
            }
            if (scram_sha256.isValid()) {
                mechanismsVec.push_back(kSHA256FieldName);
            }
            if (isExternal) {
                mechanismsVec.push_back(kExternalFieldName);
            }

            // Valid CredentialData objects must have at least one mechanism.
            invariant(mechanismsVec.size() > 0);

            return mechanismsVec;
        }
    };

    using ResourcePrivilegeMap = stdx::unordered_map<ResourcePattern, Privilege>;

    explicit User(std::unique_ptr<UserRequest> request);
    User(User&&) = default;
    User& operator=(User&&) = default;

    const UserId& getID() const {
        return _id;
    }

    void setID(UserId id) {
        _id = std::move(id);
    }

    const UserRequest* getUserRequest() const {
        return _request.get();
    }

    /**
     * Returns the user name for this user.
     */
    const UserName& getName() const {
        return _request->getUserName();
    }

    /**
     * Checks if the user has been invalidated.
     */
    bool isInvalidated() const {
        return _isInvalidated;
    }

    /**
     * Invalidates the user.
     */
    void invalidate() {
        _isInvalidated = true;
    }

    /**
     * Returns a digest of the user's identity
     */
    const SHA256Block& getDigest() const {
        return _digest;
    }

    /**
     * Returns an iterator over the names of the user's direct roles
     */
    RoleNameIterator getRoles() const;

    /**
     * Returns an iterator over the names of the user's indirect roles
     */
    RoleNameIterator getIndirectRoles() const;

    /**
     * Returns true if this user is a member of the given role.
     */
    bool hasRole(const RoleName& roleName) const;

    /**
     * Returns a reference to the information about the user's privileges.
     */
    const ResourcePrivilegeMap& getPrivileges() const {
        return _privileges;
    }

    /**
     * Returns the CredentialData for this user.
     */
    const CredentialData& getCredentials() const;

    /**
     * Gets the set of actions this user is allowed to perform on the given resource.
     */
    ActionSet getActionsForResource(const ResourcePattern& resource) const;

    /**
     * Returns true if the user has is allowed to perform an action on the given resource.
     */
    bool hasActionsForResource(const ResourcePattern& resource) const;

    // Mutators below.  Mutation functions should *only* be called by the AuthorizationManager

    /**
     * Sets this user's authentication credentials.
     */
    void setCredentials(const CredentialData& credentials);

    /**
     * Replaces any existing user role membership information with the roles from "roles".
     */
    void setRoles(RoleNameIterator roles);

    /**
     * Replaces any existing indirect user role membership information with the roles from
     * "indirectRoles".
     */
    void setIndirectRoles(RoleNameIterator indirectRoles);

    /**
     * Replaces any existing user privilege information with "privileges".
     */
    void setPrivileges(const PrivilegeVector& privileges);

    /**
     * Adds the given role name to the list of roles of which this user is a member.
     */
    void addRole(const RoleName& role);

    /**
     * Adds the given role names to the list of roles that this user belongs to.
     */
    void addRoles(const std::vector<RoleName>& roles);

    /**
     * Adds the given privilege to the list of privileges this user is authorized for.
     */
    void addPrivilege(const Privilege& privilege);

    /**
     * Adds the given privileges to the list of privileges this user is authorized for.
     */
    void addPrivileges(const PrivilegeVector& privileges);

    /**
     * Replaces any existing authentication restrictions with "restrictions".
     */
    void setRestrictions(RestrictionDocuments restrictions) &;

    /**
     * Gets any set authentication restrictions.
     */
    const RestrictionDocuments& getRestrictions() const& {
        return _restrictions;
    }

    /**
     * Replaces any existing authentication restrictions with "restrictions".
     */
    void setIndirectRestrictions(RestrictionDocuments restrictions) &;

    /**
     * Gets any set authentication restrictions.
     */
    const RestrictionDocuments& getIndirectRestrictions() const& {
        return _indirectRestrictions;
    }

    /**
     * Process both direct and indirect authentication restrictions.
     */
    Status validateRestrictions(OperationContext* opCtx) const;

    /**
     * Generates a BSON representation of the User object with all the information needed for
     * usersInfo.
     */
    void reportForUsersInfo(BSONObjBuilder* builder,
                            bool showCredentials,
                            bool showPrivileges,
                            bool showAuthenticationRestrictions) const;

    /**
     * Returns true if the User object has at least one different direct or indirect role from the
     * otherUser.
     */
    bool hasDifferentRoles(const User& otherUser) const;

private:
    // Unique ID (often UUID) for this user. May be empty for legacy users.
    UserId _id;

    // The original UserRequest which resolved into this user
    std::unique_ptr<UserRequest> _request;

    // User was explicitly invalidated
    bool _isInvalidated;

    // Digest of the full username
    SHA256Block _digest;

    // Maps resource name to privilege on that resource
    ResourcePrivilegeMap _privileges;

    // Roles the user has privileges from
    stdx::unordered_set<RoleName> _roles;

    // Roles that the user indirectly has privileges from, due to role inheritance.
    std::vector<RoleName> _indirectRoles;

    // Credential information.
    CredentialData _credentials;

    // Restrictions which must be met by a Client in order to authenticate as this user.
    RestrictionDocuments _restrictions;

    // Indirect restrictions inherited via roles.
    RestrictionDocuments _indirectRestrictions;
};

using UserCache = ReadThroughCache<UserRequest::UserRequestCacheKey,
                                   User,
                                   CacheNotCausallyConsistent,
                                   std::shared_ptr<UserRequest>,
                                   SharedUserAcquisitionStats>;
using UserHandle = UserCache::ValueHandle;

}  // namespace mongo
