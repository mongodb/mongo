// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#pragma once

#include "mongo/db/auth/authorization_backend_interface.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/shard_role/shard_catalog/db_raii.h"
#include "mongo/util/modules.h"

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

    Atomic<bool> _hasAnyPrivilegeDocuments{false};
};
}  // namespace mongo::auth
