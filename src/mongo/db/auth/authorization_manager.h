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

#include <functional>
#include <memory>
#include <string>

#include <boost/optional.hpp>

#include "mongo/base/secure_allocator.h"
#include "mongo/base/shim.h"
#include "mongo/base/status.h"
#include "mongo/bson/mutable/element.h"
#include "mongo/bson/oid.h"
#include "mongo/db/auth/action_set.h"
#include "mongo/db/auth/privilege_format.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/auth/role_graph.h"
#include "mongo/db/auth/user.h"
#include "mongo/db/auth/user_name.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/server_options.h"
#include "mongo/platform/condition_variable.h"
#include "mongo/platform/mutex.h"
#include "mongo/stdx/unordered_map.h"

namespace mongo {

class AuthorizationSession;
class AuthzManagerExternalState;
class OperationContext;
class ServiceContext;
class UserDocumentParser;

/**
 * Internal secret key info.
 */
struct AuthInfo {
    UserHandle user;

    // Used during keyfile rollover to store the alternate key used to authenticate
    boost::optional<User::CredentialData> alternateCredentials;
};
extern AuthInfo internalSecurity;  // set at startup and not changed after initialization.

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
    static AuthorizationManager* get(ServiceContext* service);
    static AuthorizationManager* get(ServiceContext& service);
    static void set(ServiceContext* service, std::unique_ptr<AuthorizationManager> authzManager);

    virtual ~AuthorizationManager() = default;

    AuthorizationManager() = default;

    static MONGO_DECLARE_SHIM(()->std::unique_ptr<AuthorizationManager>) create;

    static constexpr StringData USERID_FIELD_NAME = "userId"_sd;
    static constexpr StringData USER_NAME_FIELD_NAME = "user"_sd;
    static constexpr StringData USER_DB_FIELD_NAME = "db"_sd;
    static constexpr StringData ROLE_NAME_FIELD_NAME = "role"_sd;
    static constexpr StringData ROLE_DB_FIELD_NAME = "db"_sd;
    static constexpr StringData PASSWORD_FIELD_NAME = "pwd"_sd;
    static constexpr StringData V1_USER_NAME_FIELD_NAME = "user"_sd;
    static constexpr StringData V1_USER_SOURCE_FIELD_NAME = "userSource"_sd;

    static const NamespaceString adminCommandNamespace;
    static const NamespaceString rolesCollectionNamespace;
    static const NamespaceString usersAltCollectionNamespace;
    static const NamespaceString usersBackupCollectionNamespace;
    static const NamespaceString usersCollectionNamespace;
    static const NamespaceString versionCollectionNamespace;
    static const NamespaceString defaultTempUsersCollectionNamespace;  // for mongorestore
    static const NamespaceString defaultTempRolesCollectionNamespace;  // for mongorestore


    /**
     * Status to be returned when authentication fails. Being consistent about our returned Status
     * prevents information leakage.
     */
    static const Status authenticationFailedStatus;

    /**
     * Query to match the auth schema version document in the versionCollectionNamespace.
     */
    static const BSONObj versionDocumentQuery;

    /**
     * Name of the field in the auth schema version document containing the current schema
     * version.
     */
    static constexpr StringData schemaVersionFieldName = "currentVersion"_sd;

    /**
     * Value used to represent that the schema version is not cached or invalid.
     */
    static const int schemaVersionInvalid = 0;

    /**
     * Auth schema version for MongoDB v2.4 and prior.
     */
    static const int schemaVersion24 = 1;

    /**
     * Auth schema version for MongoDB v2.6 during the upgrade process.  Same as
     * schemaVersion26Final, except that user documents are found in admin.new.users, and user
     * management commands are disabled.
     */
    static const int schemaVersion26Upgrade = 2;

    /**
     * Auth schema version for MongoDB 2.6 and 3.0 MONGODB-CR/SCRAM mixed auth mode.
     * Users are stored in admin.system.users, roles in admin.system.roles.
     */
    static const int schemaVersion26Final = 3;

    /**
     * Auth schema version for MongoDB 3.0 SCRAM only mode.
     * Users are stored in admin.system.users, roles in admin.system.roles.
     * MONGODB-CR credentials have been replaced with SCRAM credentials in the user documents.
     */
    static const int schemaVersion28SCRAM = 5;

    /**
     * Returns a new AuthorizationSession for use with this AuthorizationManager.
     */
    virtual std::unique_ptr<AuthorizationSession> makeAuthorizationSession() = 0;

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
     * Returns via the output parameter "version" the version number of the authorization
     * system.  Returns Status::OK() if it was able to successfully fetch the current
     * authorization version.  If it has problems fetching the most up to date version it
     * returns a non-OK status.  When returning a non-OK status, *version will be set to
     * schemaVersionInvalid (0).
     */
    virtual Status getAuthorizationVersion(OperationContext* opCtx, int* version) = 0;

    /**
     * Returns the user cache generation identifier.
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
     * Delegates method call to the underlying AuthzManagerExternalState.
     */
    virtual Status getUserDescription(OperationContext* opCtx,
                                      const UserName& userName,
                                      BSONObj* result) = 0;

    /**
     * Delegates method call to the underlying AuthzManagerExternalState.
     */
    virtual Status getRoleDescription(OperationContext* opCtx,
                                      const RoleName& roleName,
                                      PrivilegeFormat privilegeFormat,
                                      AuthenticationRestrictionsFormat,
                                      BSONObj* result) = 0;

    /**
     * Convenience wrapper for getRoleDescription() defaulting formats to kOmit.
     */
    Status getRoleDescription(OperationContext* ctx, const RoleName& roleName, BSONObj* result) {
        return getRoleDescription(
            ctx, roleName, PrivilegeFormat::kOmit, AuthenticationRestrictionsFormat::kOmit, result);
    }

    /**
     * Delegates method call to the underlying AuthzManagerExternalState.
     */
    virtual Status getRolesDescription(OperationContext* opCtx,
                                       const std::vector<RoleName>& roleName,
                                       PrivilegeFormat privilegeFormat,
                                       AuthenticationRestrictionsFormat,
                                       BSONObj* result) = 0;

    /**
     * Delegates method call to the underlying AuthzManagerExternalState.
     */
    virtual Status getRoleDescriptionsForDB(OperationContext* opCtx,
                                            StringData dbname,
                                            PrivilegeFormat privilegeFormat,
                                            AuthenticationRestrictionsFormat,
                                            bool showBuiltinRoles,
                                            std::vector<BSONObj>* result) = 0;

    /**
     * Returns a Status or UserHandle for the given userName. If the user cache already has a
     * user object for this user, it returns a handle from the cache, otherwise it reads the
     * user document from disk or LDAP - this may block for a long time.
     *
     * The returned user may be invalid by the time the caller gets access to it.
     */
    virtual StatusWith<UserHandle> acquireUser(OperationContext* opCtx,
                                               const UserName& userName) = 0;

    /**
     * Validate the ID associated with a known user while refreshing session cache.
     */
    virtual StatusWith<UserHandle> acquireUserForSessionRefresh(OperationContext* opCtx,
                                                                const UserName& userName,
                                                                const User::UserId& uid) = 0;

    /**
     * Marks the given user as invalid and removes it from the user cache.
     */
    virtual void invalidateUserByName(OperationContext* opCtx, const UserName& user) = 0;

    /**
     * Invalidates all users who's source is "dbname" and removes them from the user cache.
     */
    virtual void invalidateUsersFromDB(OperationContext* opCtx, StringData dbname) = 0;

    /**
     * Initializes the authorization manager.  Depending on what version the authorization
     * system is at, this may involve building up the user cache and/or the roles graph.
     * Call this function at startup and after resynchronizing a slave/secondary.
     */
    virtual Status initialize(OperationContext* opCtx) = 0;

    /**
     * Invalidates all of the contents of the user cache.
     */
    virtual void invalidateUserCache(OperationContext* opCtx) = 0;

    /**
     * Sets the list of users that should be pinned in memory.
     *
     * This will start the PinnedUserTracker thread if it hasn't been started already.
     */
    virtual void updatePinnedUsersList(std::vector<UserName> names) = 0;

    /**
     * Parses privDoc and fully initializes the user object (credentials, roles, and privileges)
     * with the information extracted from the privilege document.
     * This should never be called from outside the AuthorizationManager - the only reason it's
     * public instead of private is so it can be unit tested.
     */
    virtual Status _initializeUserFromPrivilegeDocument(User* user, const BSONObj& privDoc) = 0;

    /**
     * Hook called by replication code to let the AuthorizationManager observe changes
     * to relevant collections.
     */
    virtual void logOp(OperationContext* opCtx,
                       const char* opstr,
                       const NamespaceString& nss,
                       const BSONObj& obj,
                       const BSONObj* patt) = 0;

    /*
     * Represents a user in the user cache.
     */
    struct CachedUserInfo {
        UserName userName;  // The username of the user
        bool active;        // Whether the user is currently in use by a thread (a thread has
                            // called acquireUser and still owns the returned shared_ptr)
    };

    virtual std::vector<CachedUserInfo> getUserCacheInfo() const = 0;
};

}  // namespace mongo
