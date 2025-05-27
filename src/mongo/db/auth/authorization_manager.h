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
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/oid.h"
#include "mongo/db/auth/action_set.h"
#include "mongo/db/auth/authorization_router.h"
#include "mongo/db/auth/builtin_roles.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/auth/privilege_format.h"
#include "mongo/db/auth/resolve_role_option.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/auth/restriction_set.h"
#include "mongo/db/auth/role_name.h"
#include "mongo/db/auth/user.h"
#include "mongo/db/auth/user_name.h"
#include "mongo/db/client.h"
#include "mongo/db/database_name.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/db/tenant_id.h"
#include "mongo/stdx/unordered_set.h"

#include <cstdint>
#include <memory>
#include <vector>

#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

class AuthorizationSession;
class Client;
class OperationContext;
class ServiceContext;

/**
 * Internal secret key info.
 */
struct SystemAuthInfo {
    std::shared_ptr<UserHandle> getUser() {
        return std::atomic_load(&_user);  // NOLINT
    }

    std::shared_ptr<UserHandle> setUser(std::shared_ptr<UserHandle> user) {
        return std::atomic_exchange(&_user, user);  // NOLINT
    }

    // Used during keyfile rollover to store the alternate key used to authenticate
    boost::optional<User::CredentialData> credentials;
    boost::optional<User::CredentialData> alternateCredentials;

private:
    std::shared_ptr<UserHandle> _user;
};
extern SystemAuthInfo internalSecurity;

/**
 * How user management functions should structure the BSON representation of privileges and roles.
 */
enum class AuthenticationRestrictionsFormat {
    kOmit,  // AuthenticationRestrictions should not be included in the BSON representation.
    kShow,  // AuthenticationRestrictions should be included in the BSON representation.
};

/**
 * Contains server/cluster-wide information about Authorization.
 */
class AuthorizationManager {
    AuthorizationManager(const AuthorizationManager&) = delete;
    AuthorizationManager& operator=(const AuthorizationManager&) = delete;

public:
    static AuthorizationManager* get(Service* service);
    static AuthorizationManager* get(Service& service);
    static void set(Service* service, std::unique_ptr<AuthorizationManager> authzManager);

    static std::unique_ptr<AuthorizationManager> create(Service* service);

    AuthorizationManager() = default;

    virtual ~AuthorizationManager() = default;

    static constexpr StringData USERID_FIELD_NAME = "userId"_sd;
    static constexpr StringData USER_NAME_FIELD_NAME = "user"_sd;
    static constexpr StringData USER_DB_FIELD_NAME = "db"_sd;
    static constexpr StringData ROLE_NAME_FIELD_NAME = "role"_sd;
    static constexpr StringData ROLE_DB_FIELD_NAME = "db"_sd;
    static constexpr StringData PASSWORD_FIELD_NAME = "pwd"_sd;
    static constexpr StringData V1_USER_NAME_FIELD_NAME = "user"_sd;
    static constexpr StringData V1_USER_SOURCE_FIELD_NAME = "userSource"_sd;

    /**
     * Status to be returned when authentication fails. Being consistent about our returned Status
     * prevents information leakage.
     */
    static const Status authenticationFailedStatus;

    /**
     * Query to match the auth schema version document in the versionCollectionNamespace while
     * upserting it on FCV downgrade.
     */
    static const BSONObj versionDocumentQuery;

    /**
     * Name of the field in the auth schema version document containing the current schema version.
     */
    static constexpr StringData schemaVersionFieldName = "currentVersion"_sd;

    /**
     * Auth schema version for MongoDB 3.0 SCRAM only mode.
     * Users are stored in admin.system.users, roles in admin.system.roles.
     * MONGODB-CR credentials have been replaced with SCRAM credentials in the user documents.
     * Note - this is the only supported auth schema version now. It is left simply so that
     * it can be supplied in the output of {getParameter: {authSchemaVersion: 1}}.
     */
    static constexpr int schemaVersion28SCRAM = 5;

    /**
     * Returns a new AuthorizationSession for use with this AuthorizationManager.
     */
    virtual std::unique_ptr<AuthorizationSession> makeAuthorizationSession(Client* client) = 0;

    /**
     * Sets whether or not startup AuthSchema validation checks should be applied in this manager.
     */
    virtual void setShouldValidateAuthSchemaOnStartup(bool validate) = 0;

    /**
     * Returns true if startup AuthSchema validation checks should be applied in this manager.
     */
    virtual bool shouldValidateAuthSchemaOnStartup() = 0;

    /**
     * Sets whether or not access control enforcement is enabled for this manager.
     */
    virtual void setAuthEnabled(bool enabled) = 0;

    /**
     * Returns true if access control is enabled for this manager .
     */
    virtual bool isAuthEnabled() const = 0;

    /**
     * The value reported by this method must change every time some persisted authorization rule
     * gets modified. It serves as a means for consumers of authorization data to discover that
     * something changed and that they need to re-cache.
     *
     * The most prominent consumer of this value is MongoS, which uses it to determine whether it
     * needs to re-fetch the authentication info from the config server.
     */
    virtual OID getCacheGeneration() = 0;

    /**
     * Returns true if there exists at least one privilege document in the system.
     * Used by the AuthorizationSession to determine whether localhost connections should be
     * granted special access to bootstrap the system.
     * NOTE: If this method ever returns true, the result is cached in _privilegeDocsExist,
     * meaning that once this method returns true it will continue to return true for the
     * lifetime of this process, even if all users are subsequently dropped from the system.
     */
    virtual bool hasAnyPrivilegeDocuments(OperationContext* opCtx) = 0;

    /**
     * This method is used to indicate that an operation has occurred that may affect
     * the user cache. Delegates method call to the underlying AuthorizationRouter.
     * Note that this method is not expected to be called on the router Service.
     * Doing so will result in a no-op.
     */
    virtual void notifyDDLOperation(OperationContext* opCtx,
                                    StringData op,
                                    const NamespaceString& nss,
                                    const BSONObj& o,
                                    const BSONObj* o2) = 0;

    /**
     * Returns a Status or UserHandle for the given userRequest. If the user cache already has a
     * user object for this user, it returns a handle from the cache, otherwise it retrieves the
     * user via a usersInfo command routed to the appropriate node - this may block for a long time.
     *
     * The returned user may be invalid by the time the caller gets access to it.
     */
    virtual StatusWith<UserHandle> acquireUser(OperationContext* opCtx,
                                               std::unique_ptr<UserRequest> userRequest) = 0;

    /**
     * Validate the ID associated with a known user while refreshing session cache.
     */
    virtual StatusWith<UserHandle> reacquireUser(OperationContext* opCtx,
                                                 const UserHandle& user) = 0;

    /**
     * Marks the given user as invalid and removes it from the user cache.
     */
    virtual void invalidateUserByName(const UserName& user) = 0;

    /**
     * Invalidates all users whose source is "dbname" and removes them from the user cache.
     */
    virtual void invalidateUsersFromDB(const DatabaseName& dbname) = 0;

    /**
     * Invalidate all users associated with a given tenant,
     * or entire cache if tenant == boost::none.
     */
    virtual void invalidateUsersByTenant(const boost::optional<TenantId>& tenant) = 0;

    /**
     * Invalidates all of the contents of the user cache.
     */
    virtual void invalidateUserCache() = 0;

    /**
     * Retrieves all users whose source is "$external" and checks if the corresponding user in the
     * backing store has a different set of roles now. If so, it updates the cache entry with the
     * new UserHandle.
     */
    virtual Status refreshExternalUsers(OperationContext* opCtx) = 0;

    /**
     * Initializes the authorization manager.  Depending on what version the authorization
     * system is at, this may involve building up the user cache and/or the roles graph.
     * Call this function at startup and after resynchronizing a secondary.
     */
    virtual Status initialize(OperationContext* opCtx) = 0;

    virtual std::vector<AuthorizationRouter::CachedUserInfo> getUserCacheInfo() const = 0;

    /**
     * Return type for resolveRoles().
     * Each member will be populated ONLY IF their corresponding Option flag was specifed.
     * Otherwise, they will be equal to boost::none.
     */
};
}  // namespace mongo
