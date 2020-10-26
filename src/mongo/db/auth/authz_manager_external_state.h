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
#include <vector>

#include "mongo/base/status.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/authorization_manager_impl.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/auth/privilege_format.h"
#include "mongo/db/auth/role_name.h"
#include "mongo/db/auth/user.h"
#include "mongo/db/auth/user_name.h"
#include "mongo/db/jsobj.h"

namespace mongo {

class AuthzSessionExternalState;
class OperationContext;

/**
 * Public interface for a class that encapsulates all the information related to system
 * state not stored in AuthorizationManager.  This is primarily to make AuthorizationManager
 * easier to test as well as to allow different implementations for mongos and mongod.
 */
class AuthzManagerExternalState {
    AuthzManagerExternalState(const AuthzManagerExternalState&) = delete;
    AuthzManagerExternalState& operator=(const AuthzManagerExternalState&) = delete;

public:
    static std::unique_ptr<AuthzManagerExternalState> create();

    virtual ~AuthzManagerExternalState();

    /**
     * Initializes the external state object.  Must be called after construction and before
     * calling other methods.  Object may not be used after this method returns something other
     * than Status::OK().
     */
    virtual Status initialize(OperationContext* opCtx) {
        return Status::OK();
    }

    /**
     * Creates an external state manipulator for an AuthorizationSession whose
     * AuthorizationManager uses this object as its own external state manipulator.
     */
    virtual std::unique_ptr<AuthzSessionExternalState> makeAuthzSessionExternalState(
        AuthorizationManager* authzManager) = 0;

    /**
     * Retrieves the schema version of the persistent data describing users and roles.
     * Will leave *outVersion unmodified on non-OK status return values.
     */
    virtual Status getStoredAuthorizationVersion(OperationContext* opCtx, int* outVersion) = 0;

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
    virtual Status getUserDescription(OperationContext* opCtx,
                                      const UserRequest& user,
                                      BSONObj* result) = 0;

    /**
     * Fetches and/or synthesizes a User object similar to above eliding additional
     * marshalling of data to BSON and back.
     */
    virtual StatusWith<User> getUserObject(OperationContext* opCtx, const UserRequest& userReq) = 0;

    /**
     * Checks to see if the named roles exist.
     */
    virtual Status rolesExist(OperationContext* opCtx, const std::vector<RoleName>& roleNames) = 0;

    using ResolveRoleOption = AuthorizationManager::ResolveRoleOption;
    using ResolvedRoleData = AuthorizationManager::ResolvedRoleData;

    /**
     * Collects (in)direct roles, privileges, and restrictions for a set of start roles.
     */
    virtual StatusWith<ResolvedRoleData> resolveRoles(OperationContext* opCtx,
                                                      const std::vector<RoleName>& roleNames,
                                                      ResolveRoleOption option) = 0;

    /**
     * Fetches and returns objects representing named roles.
     *
     * Each BSONObj in the $result vector contains a full role description
     * as retrieved from admin.system.roles plus inherited role/privilege
     * information as appropriate.
     */
    virtual Status getRolesDescription(OperationContext* opCtx,
                                       const std::vector<RoleName>& roles,
                                       PrivilegeFormat showPrivileges,
                                       AuthenticationRestrictionsFormat,
                                       std::vector<BSONObj>* result) = 0;

    /**
     * Fetches named roles and synthesizes them into a fragment of a user document.
     *
     * The document synthesized into $result looks like a complete user document
     * representing the $roles specified and their subordinates, but without
     * an actual user name or credentials.
     */
    virtual Status getRolesAsUserFragment(OperationContext* opCtx,
                                          const std::vector<RoleName>& roles,
                                          AuthenticationRestrictionsFormat,
                                          BSONObj* result) = 0;

    /**
     * Writes into "result" documents describing the roles that are defined on the given
     * database. If showPrivileges is kOmit or kShowPrivileges, then a vector of BSON documents are
     * returned, where each document includes the other roles a particular role is a
     * member of, including those role memberships held implicitly through other roles
     * (indirect roles). If showPrivileges is kShowPrivileges, then the description documents
     * will also include a full list of the roles' privileges. If showBuiltinRoles is true, then
     * the result array will contain description documents for all the builtin roles for the given
     * database, if it is false the result will just include user defined roles. In the event that
     * some of the information in a given role description is inconsistent, the document will
     * contain a "warnings" array, with std::string messages describing inconsistencies.
     */
    virtual Status getRoleDescriptionsForDB(OperationContext* opCtx,
                                            StringData dbname,
                                            PrivilegeFormat showPrivileges,
                                            AuthenticationRestrictionsFormat,
                                            bool showBuiltinRoles,
                                            std::vector<BSONObj>* result) = 0;

    /**
     * Returns true if there exists at least one privilege document in the system.
     */
    virtual bool hasAnyPrivilegeDocuments(OperationContext* opCtx) = 0;

    virtual void logOp(OperationContext* opCtx,
                       AuthorizationManagerImpl* authManager,
                       StringData op,
                       const NamespaceString& ns,
                       const BSONObj& o,
                       const BSONObj* o2) {}

    virtual void setInUserManagementCommand(OperationContext* opCtx, bool val) {}

protected:
    AuthzManagerExternalState();  // This class should never be instantiated directly.

    /**
     * Construct a Status about one or more unknown roles.
     */
    static Status makeRoleNotFoundStatus(const stdx::unordered_set<RoleName>&);
};

}  // namespace mongo
