// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/auth/user.h"
#include "mongo/util/modules.h"

#include <string_view>

namespace mongo {

class AuthorizationManager;
class AuthzSessionExternalState;
class [[MONGO_MOD_PUBLIC]] AuthorizationRouter {
public:
    AuthorizationRouter(const AuthorizationRouter&) = delete;
    AuthorizationRouter& operator=(const AuthorizationRouter&) = delete;

    virtual ~AuthorizationRouter() = default;

    virtual std::unique_ptr<AuthzSessionExternalState> makeAuthzSessionExternalState(
        Client* client) = 0;

    virtual Status getUserDescription(OperationContext* opCtx,
                                      const UserRequest& userRequest,
                                      BSONObj* result,
                                      const SharedUserAcquisitionStats& userAcquisitionStats) = 0;

    virtual StatusWith<User> getUserObject(
        OperationContext* opCtx,
        const UserRequest& userRequest,
        const SharedUserAcquisitionStats& userAcquisitionStats) = 0;

    virtual Status rolesExist(OperationContext* opCtx, const std::vector<RoleName>& roleNames) = 0;

    virtual bool hasAnyPrivilegeDocuments(OperationContext* opCtx) = 0;

    virtual void notifyDDLOperation(OperationContext* opCtx,
                                    std::string_view op,
                                    const NamespaceString& nss,
                                    const BSONObj& o,
                                    const BSONObj* o2) = 0;

    virtual OID getCacheGeneration() = 0;

    virtual StatusWith<UserHandle> acquireUser(OperationContext* opCtx,
                                               std::unique_ptr<UserRequest> userRequest) = 0;

    virtual StatusWith<UserHandle> reacquireUser(OperationContext* opCtx,
                                                 const UserHandle& user) = 0;

    virtual void invalidateUserByName(const UserName& user) = 0;

    virtual void invalidateUsersFromDB(const DatabaseName& dbname) = 0;

    virtual void invalidateUsersByTenant(const boost::optional<TenantId>& tenant) = 0;

    virtual void invalidateUserCache() = 0;

    virtual Status refreshExternalUsers(OperationContext* opCtx) = 0;

    /*
     * Represents a user in the user cache.
     */
    struct CachedUserInfo {
        UserName userName;  // The username of the user
        bool active;        // Whether the user is currently in use by a thread (a thread has
                            // called acquireUser and still owns the returned shared_ptr)
    };

    virtual std::vector<CachedUserInfo> getUserCacheInfo() const = 0;

protected:
    AuthorizationRouter() = default;

    /**
     * Construct an error message for unknown roles.
     */
    static std::string buildUnknownRolesErrorMsg(const stdx::unordered_set<RoleName>&);
};

}  // namespace mongo
