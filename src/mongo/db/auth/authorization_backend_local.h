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

#include "mongo/db/auth/authorization_backend_interface.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/local_catalog/db_raii.h"

namespace mongo::auth {

class AuthorizationBackendLocal : public AuthorizationBackendInterface {
protected:
    Status rolesExist(OperationContext* opCtx, const std::vector<RoleName>& roleNames) final;

    StatusWith<ResolvedRoleData> resolveRoles(OperationContext* opCtx,
                                              const std::vector<RoleName>& roleNames,
                                              ResolveRoleOption option) final;

    UsersInfoReply lookupUsers(OperationContext* opCtx, const UsersInfoCommand& cmd) final;

    RolesInfoReply lookupRoles(OperationContext* opCtx, const RolesInfoCommand& cmd) final;

    StatusWith<User> getUserObject(OperationContext* opCtx,
                                   const UserRequest& userReq,
                                   const SharedUserAcquisitionStats& userAcquisitionStats) override;

    /**
     * Ensures a consistent logically valid view of the data across users and roles collections.
     *
     * A lock-free read operation opens storage snapshot that subsequent reads will
     * all use for a consistent point-in-time data view.
     *
     * The lock-free consistent data view, prevent the possibility of having permissions
     * available which are logically invalid. This is how reads/writes across two collections are
     * made 'atomic'.
     */
    class RolesSnapshot {
    public:
        RolesSnapshot() = default;
        RolesSnapshot(OperationContext*);
        ~RolesSnapshot();

    private:
        std::unique_ptr<AutoReadLockFree> _readLockFree;
    };

    /**
     * Writes into "result" a document describing the requested user and returns Status::OK().
     * The caller is required to provide all information necessary to unique identify the request
     * for a user, including the user's name and any roles which the user must possess via
     * out-of-band attestation. The returned description includes the user credentials and
     * customData, if present, the user's role membership and delegation information, a full
     * list of the user's privileges, and a full list of the user's roles, including those
     * roles held implicitly through other roles (indirect roles). In the event that some of this
     * information is inconsistent, the document will contain a "warnings" array,
     * with std::string messages describing inconsistencies.
     *
     * If the user does not exist, returns ErrorCodes::UserNotFound.
     */
    Status getUserDescription(OperationContext* opCtx,
                              const UserRequest& user,
                              BSONObj* result,
                              const SharedUserAcquisitionStats& userAcquisitionStats) override;

    static Status makeRoleNotFoundStatus(const stdx::unordered_set<RoleName>& unknownRoles);

    virtual Status findOne(OperationContext* opCtx,
                           const NamespaceString& nss,
                           const BSONObj& query,
                           BSONObj* result);

    Status getRolesAsUserFragment(OperationContext* opCtx,
                                  const std::vector<RoleName>& roleNames,
                                  AuthenticationRestrictionsFormat showRestrictions,
                                  BSONObj* result);

    Status getRolesDescription(OperationContext* opCtx,
                               const std::vector<RoleName>& roleNames,
                               PrivilegeFormat showPrivileges,
                               AuthenticationRestrictionsFormat showRestrictions,
                               std::vector<BSONObj>* result);

    Status getRoleDescriptionsForDB(OperationContext* opCtx,
                                    const DatabaseName& dbname,
                                    PrivilegeFormat showPrivileges,
                                    AuthenticationRestrictionsFormat showRestrictions,
                                    bool showBuiltinRoles,
                                    std::vector<BSONObj>* result);

    std::vector<BSONObj> performNoPrivilegeNoRestrictionsLookup(OperationContext* opCtx,
                                                                const UsersInfoCommand& cmd);

    std::vector<BSONObj> performLookupWithPrivilegesAndRestrictions(
        OperationContext* opCtx,
        const UsersInfoCommand& cmd,
        const std::vector<UserName>& usernames);

    /**
     * Set an auto-releasing shared lock on the roles database.
     * This allows us to maintain a consistent state during user acquisiiton.
     *
     * virtual to allow Mock to not lock anything.
     */
    virtual RolesSnapshot _snapshotRoles(OperationContext* opCtx);

    AtomicWord<bool> _hasAnyPrivilegeDocuments{false};
};
}  // namespace mongo::auth
