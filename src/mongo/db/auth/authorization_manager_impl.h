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

#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/platform/mutex.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/util/concurrency/thread_pool.h"

namespace mongo {

/**
 * Contains server/cluster-wide information about Authorization.
 */
class AuthorizationManagerImpl final : public AuthorizationManager {
public:
    struct InstallMockForTestingOrAuthImpl {
        explicit InstallMockForTestingOrAuthImpl() = default;
    };

    AuthorizationManagerImpl(ServiceContext* service,
                             std::unique_ptr<AuthzManagerExternalState> externalState);
    ~AuthorizationManagerImpl();


    std::unique_ptr<AuthorizationSession> makeAuthorizationSession() override;

    void setShouldValidateAuthSchemaOnStartup(bool validate) override;

    bool shouldValidateAuthSchemaOnStartup() override;

    void setAuthEnabled(bool enabled) override;

    bool isAuthEnabled() const override;

    Status getAuthorizationVersion(OperationContext* opCtx, int* version) override;

    OID getCacheGeneration() override;

    bool hasAnyPrivilegeDocuments(OperationContext* opCtx) override;

    Status getUserDescription(OperationContext* opCtx,
                              const UserName& userName,
                              BSONObj* result) override;

    Status rolesExist(OperationContext* opCtx, const std::vector<RoleName>& roleNames) override;

    StatusWith<ResolvedRoleData> resolveRoles(OperationContext* opCtx,
                                              const std::vector<RoleName>& roleNames,
                                              ResolveRoleOption option) override;

    Status getRolesDescription(OperationContext* opCtx,
                               const std::vector<RoleName>& roleName,
                               PrivilegeFormat privilegeFormat,
                               AuthenticationRestrictionsFormat,
                               std::vector<BSONObj>* result) override;

    Status getRolesAsUserFragment(OperationContext* opCtx,
                                  const std::vector<RoleName>& roleName,
                                  AuthenticationRestrictionsFormat,
                                  BSONObj* result) override;

    Status getRoleDescriptionsForDB(OperationContext* opCtx,
                                    const DatabaseName& dbname,
                                    PrivilegeFormat privilegeFormat,
                                    AuthenticationRestrictionsFormat,
                                    bool showBuiltinRoles,
                                    std::vector<BSONObj>* result) override;

    StatusWith<UserHandle> acquireUser(OperationContext* opCtx, const UserName& userName) override;
    StatusWith<UserHandle> reacquireUser(OperationContext* opCtx, const UserHandle& user) override;

    /**
     * Invalidate a user, and repin it if necessary.
     */
    void invalidateUserByName(OperationContext* opCtx, const UserName& user) override;

    void invalidateUsersFromDB(OperationContext* opCtx, const DatabaseName& dbname) override;

    void invalidateUsersByTenant(OperationContext* opCtx,
                                 const boost::optional<TenantId>& tenant) override;

    /**
     * Verify role information for users in the $external database and insert updated information
     * into the cache if necessary. Currently, this is only used to refresh LDAP users.
     */
    Status refreshExternalUsers(OperationContext* opCtx) override;

    Status initialize(OperationContext* opCtx) override;

    /**
     * Invalidate the user cache, and repin all pinned users.
     */
    void invalidateUserCache(OperationContext* opCtx) override;

    void updatePinnedUsersList(std::vector<UserName> names) override;

    void logOp(OperationContext* opCtx,
               StringData opstr,
               const NamespaceString& nss,
               const BSONObj& obj,
               const BSONObj* patt) override;

    std::vector<CachedUserInfo> getUserCacheInfo() const override;

private:
    void _updateCacheGeneration();

    void _pinnedUsersThreadRoutine() noexcept;

    std::unique_ptr<AuthzManagerExternalState> _externalState;

    // True if AuthSchema startup checks should be applied in this AuthorizationManager. Changes to
    // its value are not synchronized, so it should only be set once, at initalization time.
    bool _startupAuthSchemaValidation{true};

    // True if access control enforcement is enabled in this AuthorizationManager. Changes to its
    // value are not synchronized, so it should only be set once, at initalization time.
    bool _authEnabled{false};

    // A cache of whether there are any users set up for the cluster.
    AtomicWord<bool> _privilegeDocsExist{false};

    // Serves as a source for the return value of getCacheGeneration(). Refer to this method for
    // more details.
    Mutex _cacheGenerationMutex =
        MONGO_MAKE_LATCH("AuthorizationManagerImpl::_cacheGenerationMutex");
    OID _cacheGeneration{OID::gen()};

    /**
     * Cache which contains at most a single entry (which has key 0), whose value is the version of
     * the auth schema.
     */
    class AuthSchemaVersionCache : public ReadThroughCache<int, int> {
    public:
        AuthSchemaVersionCache(ServiceContext* service,
                               ThreadPoolInterface& threadPool,
                               AuthzManagerExternalState* externalState);

    private:
        // Even though the dist cache permits for lookup to return boost::none for non-existent
        // values, the contract of the authorization manager is that it should throw an exception if
        // the value can not be loaded, so if it returns, the value will always be set.
        LookupResult _lookup(OperationContext* opCtx,
                             int unusedKey,
                             const ValueHandle& unusedCachedValue);

        Mutex _mutex =
            MONGO_MAKE_LATCH("AuthorizationManagerImpl::AuthSchemaVersionDistCache::_mutex");

        AuthzManagerExternalState* const _externalState;
    } _authSchemaVersionCache;

    /**
     * Cache of the users known to the authentication subsystem.
     */
    class UserCacheImpl : public UserCache {
    public:
        UserCacheImpl(ServiceContext* service,
                      ThreadPoolInterface& threadPool,
                      int cacheSize,
                      AuthSchemaVersionCache* authSchemaVersionCache,
                      AuthzManagerExternalState* externalState);

    private:
        // Even though the dist cache permits for lookup to return boost::none for non-existent
        // values, the contract of the authorization manager is that it should throw an exception if
        // the value can not be loaded, so if it returns, the value will always be set.
        LookupResult _lookup(OperationContext* opCtx,
                             const UserRequest& user,
                             const UserHandle& unusedCachedUser);

        Mutex _mutex = MONGO_MAKE_LATCH("AuthorizationManagerImpl::UserDistCacheImpl::_mutex");

        AuthSchemaVersionCache* const _authSchemaVersionCache;

        AuthzManagerExternalState* const _externalState;
    } _userCache;

    // Thread pool on which to perform the blocking activities that load the user credentials from
    // storage
    ThreadPool _threadPool;

    Mutex _pinnedUsersMutex = MONGO_MAKE_LATCH("AuthorizationManagerImpl::_pinnedUsersMutex");
    stdx::condition_variable _pinnedUsersCond;
    std::once_flag _pinnedThreadTrackerStarted;
    boost::optional<std::vector<UserName>> _usersToPin;
};

extern int authorizationManagerCacheSize;

}  // namespace mongo
