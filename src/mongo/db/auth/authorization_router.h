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

#pragma once

#include "mongo/db/auth/user.h"

namespace mongo {

class AuthorizationManager;
class AuthzSessionExternalState;
class AuthorizationRouter {
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
                                    StringData op,
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
