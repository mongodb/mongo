/**
*    Copyright (C) 2012 10gen Inc.
*
*    This program is free software: you can redistribute it and/or  modify
*    it under the terms of the GNU Affero General Public License, version 3,
*    as published by the Free Software Foundation.
*
*    This program is distributed in the hope that it will be useful,
*    but WITHOUT ANY WARRANTY; without even the implied warranty of
*    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*    GNU Affero General Public License for more details.
*
*    You should have received a copy of the GNU Affero General Public License
*    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*
*    As a special exception, the copyright holders give permission to link the
*    code of portions of this program with the OpenSSL library under certain
*    conditions as described in each individual source file and distribute
*    linked combinations including the program with the OpenSSL library. You
*    must comply with the GNU Affero General Public License in all respects for
*    all of the code used other than as permitted herein. If you modify file(s)
*    with this exception, you may extend this exception to your version of the
*    file(s), but you are not obligated to do so. If you do not wish to do so,
*    delete this exception statement from your version. If you delete this
*    exception statement from all source files in the program, then also delete
*    it in the license file.
*/

#pragma once

#include <memory>
#include <string>

#include "mongo/base/disallow_copying.h"
#include "mongo/base/status.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/authz_manager_external_state.h"
#include "mongo/db/auth/privilege_format.h"
#include "mongo/db/auth/user_name.h"
#include "mongo/stdx/functional.h"


namespace mongo {

/**
 * The implementation of AuthzManagerExternalState functionality for mongos.
 */
class AuthzManagerExternalStateMongos : public AuthzManagerExternalState {
    MONGO_DISALLOW_COPYING(AuthzManagerExternalStateMongos);

public:
    AuthzManagerExternalStateMongos();
    ~AuthzManagerExternalStateMongos() override;

    Status initialize(OperationContext* opCtx) override;
    std::unique_ptr<AuthzSessionExternalState> makeAuthzSessionExternalState(
        AuthorizationManager* authzManager) override;
    Status getStoredAuthorizationVersion(OperationContext* opCtx, int* outVersion) override;
    Status getUserDescription(OperationContext* opCtx,
                              const UserName& userName,
                              BSONObj* result) override;
    Status getRoleDescription(OperationContext* opCtx,
                              const RoleName& roleName,
                              PrivilegeFormat showPrivileges,
                              AuthenticationRestrictionsFormat,
                              BSONObj* result) override;
    Status getRolesDescription(OperationContext* opCtx,
                               const std::vector<RoleName>& roles,
                               PrivilegeFormat showPrivileges,
                               AuthenticationRestrictionsFormat,
                               BSONObj* result) override;
    Status getRoleDescriptionsForDB(OperationContext* opCtx,
                                    const std::string& dbname,
                                    PrivilegeFormat showPrivileges,
                                    AuthenticationRestrictionsFormat,
                                    bool showBuiltinRoles,
                                    std::vector<BSONObj>* result) override;

    bool hasAnyPrivilegeDocuments(OperationContext* opCtx) override;
};

}  // namespace mongo
