// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/auth/authorization_client_handle.h"
#include "mongo/db/auth/authorization_router.h"
#include "mongo/util/concurrency/thread_pool.h"
#include "mongo/util/modules.h"

#include <string_view>

namespace mongo {
class AuthorizationRouterImpl : public AuthorizationRouter {
public:
    AuthorizationRouterImpl(Service*, std::unique_ptr<AuthorizationClientHandle>);
    AuthorizationRouterImpl(const AuthorizationRouter&) = delete;
    AuthorizationRouterImpl& operator=(const AuthorizationRouterImpl&) = delete;

    ~AuthorizationRouterImpl() override = default;

    std::unique_ptr<AuthzSessionExternalState> makeAuthzSessionExternalState(Client* client) final;

    Status getUserDescription(OperationContext* opCtx,
                              const UserRequest& userRequest,
                              BSONObj* result,
                              const SharedUserAcquisitionStats& userAcquisitionStats) final;

    StatusWith<User> getUserObject(OperationContext* opCtx,
                                   const UserRequest& userRequest,
                                   const SharedUserAcquisitionStats& userAcquisitionStats) final;

    Status rolesExist(OperationContext* opCtx, const std::vector<RoleName>& roleNames) final;

    bool hasAnyPrivilegeDocuments(OperationContext* opCtx) final;

    void notifyDDLOperation(OperationContext* opCtx,
                            std::string_view op,
                            const NamespaceString& nss,
                            const BSONObj& o,
                            const BSONObj* o2) final;

    OID getCacheGeneration() final;

    StatusWith<UserHandle> acquireUser(OperationContext* opCtx,
                                       std::unique_ptr<UserRequest> userRequest) final;

    StatusWith<UserHandle> reacquireUser(OperationContext* opCtx, const UserHandle& user) final;

    /**
     * The cache invalidation methods are non-final so that AuthorizationRouterImplForTest can
     * override them.
     */
    void invalidateUserByName(const UserName& user) override;

    void invalidateUsersFromDB(const DatabaseName& dbname) override;

    void invalidateUsersByTenant(const boost::optional<TenantId>& tenant) override;

    void invalidateUserCache() override;

    Status refreshExternalUsers(OperationContext* opCtx) final;

    std::vector<CachedUserInfo> getUserCacheInfo() const final;

private:
    void _updateCacheGeneration();

    RolesInfoReply _runRolesInfoCmd(OperationContext* opCtx,
                                    const DatabaseName& dbName,
                                    const RolesInfoCommand& cmd);

    UsersInfoReply _runUsersInfoCmd(OperationContext* opCts,
                                    const DatabaseName& dbName,
                                    const UsersInfoCommand& cmd);

private:
    // Serves as a source for the return value of getCacheGeneration(). Refer to this method for
    // more details.
    std::mutex _cacheGenerationMutex;
    OID _cacheGeneration{OID::gen()};

    /**
     * Cache of the users known to the authorization subsystem.
     */
    class UserCacheImpl : public UserCache {
    public:
        UserCacheImpl(Service* service,
                      ThreadPoolInterface& threadPool,
                      int cacheSize,
                      AuthorizationRouter* authzRouter);

    private:
        // Even though the dist cache permits for lookup to return boost::none for non-existent
        // values, the contract of the authorization manager is that it should throw an exception if
        // the value can not be loaded, so if it returns, the value will always be set.
        LookupResult _lookup(OperationContext* opCtx,
                             const UserRequest::UserRequestCacheKey& userReqCacheKey,
                             const UserHandle& unusedCachedUser,
                             const UserRequest& userReq,
                             const SharedUserAcquisitionStats& userAcquisitionStats);

        std::mutex _mutex;

        AuthorizationRouter* _authzRouter;
    } _userCache;

    // Thread pool on which to perform blocking usersInfo or rolesInfo commands on the appropriate
    // node depending on whether it is a router or a shard role.
    ThreadPool _threadPool;

    std::unique_ptr<AuthorizationClientHandle> _clientHandle;
};

extern int authorizationRouterCacheSize;

}  // namespace mongo
