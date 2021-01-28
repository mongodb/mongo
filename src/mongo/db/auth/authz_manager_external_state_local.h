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
#include <string>

#include "mongo/base/status.h"
#include "mongo/db/auth/authz_manager_external_state.h"
#include "mongo/db/auth/builtin_roles.h"
#include "mongo/db/auth/role_name.h"
#include "mongo/db/auth/user_name.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/db_raii.h"
#include "mongo/platform/mutex.h"

namespace mongo {

/**
 * Common implementation of AuthzManagerExternalState for systems where role
 * and user information are stored locally.
 */
class AuthzManagerExternalStateLocal : public AuthzManagerExternalState {
    AuthzManagerExternalStateLocal(const AuthzManagerExternalStateLocal&) = delete;
    AuthzManagerExternalStateLocal& operator=(const AuthzManagerExternalStateLocal&) = delete;

public:
    virtual ~AuthzManagerExternalStateLocal() = default;

    Status getStoredAuthorizationVersion(OperationContext* opCtx, int* outVersion) override;
    StatusWith<User> getUserObject(OperationContext* opCtx, const UserRequest& userReq) override;
    Status getUserDescription(OperationContext* opCtx,
                              const UserRequest& user,
                              BSONObj* result) override;
    Status rolesExist(OperationContext* opCtx, const std::vector<RoleName>& roleNames) override;
    StatusWith<ResolvedRoleData> resolveRoles(OperationContext* opCtx,
                                              const std::vector<RoleName>& roleNames,
                                              ResolveRoleOption option) override;
    Status getRolesDescription(OperationContext* opCtx,
                               const std::vector<RoleName>& roles,
                               PrivilegeFormat showPrivileges,
                               AuthenticationRestrictionsFormat,
                               std::vector<BSONObj>* result) override;
    Status getRolesAsUserFragment(OperationContext* opCtx,
                                  const std::vector<RoleName>& roles,
                                  AuthenticationRestrictionsFormat,
                                  BSONObj* result) override;
    Status getRoleDescriptionsForDB(OperationContext* opCtx,
                                    StringData dbname,
                                    PrivilegeFormat showPrivileges,
                                    AuthenticationRestrictionsFormat,
                                    bool showBuiltinRoles,
                                    std::vector<BSONObj>* result) override;

    bool hasAnyPrivilegeDocuments(OperationContext* opCtx) final;

    /**
     * Finds a document matching "query" in "collectionName", and store a shared-ownership
     * copy into "result".
     *
     * Returns Status::OK() on success.  If no match is found, returns
     * ErrorCodes::NoMatchingDocument.  Other errors returned as appropriate.
     */
    virtual Status findOne(OperationContext* opCtx,
                           const NamespaceString& collectionName,
                           const BSONObj& query,
                           BSONObj* result) = 0;

    /**
     * Checks for the existance of a document matching "query" in "collectionName".
     */
    virtual bool hasOne(OperationContext* opCtx,
                        const NamespaceString& collectionName,
                        const BSONObj& query) = 0;

    /**
     * Finds all documents matching "query" in "collectionName".  For each document returned,
     * calls the function resultProcessor on it.
     */
    virtual Status query(OperationContext* opCtx,
                         const NamespaceString& collectionName,
                         const BSONObj& query,
                         const BSONObj& projection,
                         const std::function<void(const BSONObj&)>& resultProcessor) = 0;

    void logOp(OperationContext* opCtx,
               AuthorizationManagerImpl* authManager,
               StringData op,
               const NamespaceString& ns,
               const BSONObj& o,
               const BSONObj* o2) final;

protected:
    AuthzManagerExternalStateLocal() = default;

    /**
     * Ensures a consistent logically valid view of the data across users and roles collections.
     *
     * If running with lock-free enabled, a storage snapshot is opened that subsequent reads will
     * all use for a consistent point-in-time data view.
     *
     * Otherwise, a MODE_S lock is taken on the roles collection to ensure no role changes are made
     * across reading first the users collection and then the roles collection. This ensures an
     * 'atomic' view of the roles and users collection data.
     *
     * The locks, or lock-free consistent data view, prevent the possibility of having permissions
     * available which are logically invalid. This is how reads/writes across two collections are
     * made 'atomic'.
     */
    class RolesLocks {
    public:
        RolesLocks() = default;
        RolesLocks(OperationContext*);
        ~RolesLocks();

    private:
        std::unique_ptr<AutoReadLockFree> _readLockFree;
        std::unique_ptr<Lock::DBLock> _adminLock;
        std::unique_ptr<Lock::CollectionLock> _rolesLock;
    };

    /**
     * Set an auto-releasing shared lock on the roles database.
     * This allows us to maintain a consistent state during user acquisiiton.
     *
     * virtual to allow Mock to not lock anything.
     */
    virtual RolesLocks _lockRoles(OperationContext* opCtx);

private:
    /**
     * Once *any* privilege document is observed we cache the state forever,
     * even if these collections are emptied/dropped.
     * This ensures that the only way to recover localHostAuthBypass is to
     * is to clear that in-memory cache by restarting the server.
     */
    AtomicWord<bool> _hasAnyPrivilegeDocuments{false};
};

}  // namespace mongo
