// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/auth/authorization_session_for_test.h"

#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/builtin_roles.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/auth/user.h"
#include "mongo/db/auth/user_name.h"
#include "mongo/util/read_through_cache.h"

#include <set>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {
using namespace std::literals::string_view_literals;
void AuthorizationSessionForTest::assumePrivilegesForDB(Privilege privilege,
                                                        const DatabaseName& dbName) {
    assumePrivilegesForDB(std::vector<Privilege>{privilege}, dbName);
}

void AuthorizationSessionForTest::assumePrivilegesForDB(PrivilegeVector privileges,
                                                        const DatabaseName& dbName) {
    std::unique_ptr<UserRequest> request = std::make_unique<UserRequestGeneral>(
        UserName("authorizationSessionForTestUser"sv, dbName), boost::none);
    _authenticatedUser = UserHandle(User(std::move(request)));
    _authenticatedUser.value()->addPrivileges(privileges);
    _authenticationMode = AuthorizationSession::AuthenticationMode::kConnection;
    _updateInternalAuthorizationState();
}

void AuthorizationSessionForTest::assumePrivilegesForBuiltinRole(const RoleName& roleName) {
    PrivilegeVector privileges;
    auth::addPrivilegesForBuiltinRole(roleName, &privileges);
    auto db = roleName.getDatabaseName();
    if (db.isEmpty()) {
        db = DatabaseName::kAdmin;
    }

    assumePrivilegesForDB(privileges, db);
}

}  // namespace mongo
