// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/authorization_session_impl.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/auth/role_name.h"
#include "mongo/db/auth/user.h"
#include "mongo/db/database_name.h"
#include "mongo/util/modules.h"

#include <memory>
#include <vector>

namespace mongo {

class AuthorizationSessionForTest : public AuthorizationSessionImpl {
    AuthorizationSessionForTest(const AuthorizationSessionForTest&) = delete;
    AuthorizationSessionForTest& operator=(const AuthorizationSessionForTest&) = delete;

public:
    using AuthorizationSessionImpl::AuthorizationSessionImpl;

    /**
     * Grants this session all privileges in 'privileges' for the database named 'dbName'. Any prior
     * privileges granted on 'dbName' via a call to this method are erased.
     *
     * Do not use this method if also adding users via addAndAuthorizeUser() in the same database.
     */
    void assumePrivilegesForDB(PrivilegeVector privileges, const DatabaseName& dbName);
    void assumePrivilegesForDB(Privilege privilege, const DatabaseName& dbName);

    /**
     * Grants this session all privileges for the given builtin role. Do not mix with other methods.
     */
    void assumePrivilegesForBuiltinRole(const RoleName& roleName);
};
}  // namespace mongo
