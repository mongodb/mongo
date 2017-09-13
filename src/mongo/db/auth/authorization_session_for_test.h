/**
 * Copyright (C) 2017 MongoDB Inc.
 *
 * This program is free software: you can redistribute it and/or  modify
 * it under the terms of the GNU Affero General Public License, version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, the copyright holders give permission to link the
 * code of portions of this program with the OpenSSL library under certain
 * conditions as described in each individual source file and distribute
 * linked combinations including the program with the OpenSSL library. You
 * must comply with the GNU Affero General Public License in all respects
 * for all of the code used other than as permitted herein. If you modify
 * file(s) with this exception, you may extend this exception to your
 * version of the file(s), but you are not obligated to do so. If you do not
 * wish to do so, delete this exception statement from your version. If you
 * delete this exception statement from all source files in the program,
 * then also delete it in the license file.
 */

#pragma once

#include <memory>
#include <vector>

#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/user.h"

namespace mongo {

class AuthorizationSessionForTest : public AuthorizationSession {
    MONGO_DISALLOW_COPYING(AuthorizationSessionForTest);

public:
    using AuthorizationSession::AuthorizationSession;

    // A database name used for testing purposes, deliberately named to minimize collisions with
    // other test users.
    static constexpr StringData kTestDBName = "authorizationSessionForTestDB"_sd;

    /**
     * Cleans up any privileges granted via assumePrivilegesForDB().
     */
    ~AuthorizationSessionForTest();

    /**
     * Grants this session all privileges in 'privileges' for the database named 'dbName'. Any prior
     * privileges granted on 'dbName' via a call to this method are erased.
     *
     * Do not use this method if also adding users via addAndAuthorizeUser() in the same database.
     */
    void assumePrivilegesForDB(PrivilegeVector privilege, StringData dbName = kTestDBName);
    void assumePrivilegesForDB(Privilege privilege, StringData dbName = kTestDBName);

    /**
     * Revoke all privileges granted via assumePrivilegesForDB() on the database named 'dbName'.
     *
     * Do not use this method if also adding users via addAndAuthorizeUser() in the same database.
     */
    void revokePrivilegesForDB(StringData dbName);

    /**
     * Revokes all privileges granted via assumePrivilegesForDB() on every database.
     *
     * Do not use this method if also adding users via addAndAuthorizeUser() in the same database.
     */
    void revokeAllPrivileges();

private:
    std::vector<std::unique_ptr<User>> _testUsers;
};
}  // namespace mongo
