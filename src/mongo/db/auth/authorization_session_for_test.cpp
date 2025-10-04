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
void AuthorizationSessionForTest::assumePrivilegesForDB(Privilege privilege,
                                                        const DatabaseName& dbName) {
    assumePrivilegesForDB(std::vector<Privilege>{privilege}, dbName);
}

void AuthorizationSessionForTest::assumePrivilegesForDB(PrivilegeVector privileges,
                                                        const DatabaseName& dbName) {
    std::unique_ptr<UserRequest> request = std::make_unique<UserRequestGeneral>(
        UserName("authorizationSessionForTestUser"_sd, dbName), boost::none);
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
