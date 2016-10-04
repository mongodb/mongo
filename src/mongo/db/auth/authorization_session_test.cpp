/*    Copyright 2012 10gen Inc.
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
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#include "mongo/platform/basic.h"

/**
 * Unit tests of the AuthorizationSession type.
 */
#include "mongo/base/status.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/authz_manager_external_state_mock.h"
#include "mongo/db/auth/authz_session_external_state_mock.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context_noop.h"
#include "mongo/stdx/memory.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/map_util.h"

#define ASSERT_NULL(EXPR) ASSERT_FALSE(EXPR)
#define ASSERT_NON_NULL(EXPR) ASSERT_TRUE(EXPR)

namespace mongo {
namespace {

class FailureCapableAuthzManagerExternalStateMock : public AuthzManagerExternalStateMock {
public:
    FailureCapableAuthzManagerExternalStateMock() : _findsShouldFail(false) {}
    virtual ~FailureCapableAuthzManagerExternalStateMock() {}

    void setFindsShouldFail(bool enable) {
        _findsShouldFail = enable;
    }

    virtual Status findOne(OperationContext* txn,
                           const NamespaceString& collectionName,
                           const BSONObj& query,
                           BSONObj* result) {
        if (_findsShouldFail && collectionName == AuthorizationManager::usersCollectionNamespace) {
            return Status(ErrorCodes::UnknownError,
                          "findOne on admin.system.users set to fail in mock.");
        }
        return AuthzManagerExternalStateMock::findOne(txn, collectionName, query, result);
    }

private:
    bool _findsShouldFail;
};

class AuthorizationSessionTest : public ::mongo::unittest::Test {
public:
    FailureCapableAuthzManagerExternalStateMock* managerState;
    OperationContextNoop _txn;
    AuthzSessionExternalStateMock* sessionState;
    std::unique_ptr<AuthorizationManager> authzManager;
    std::unique_ptr<AuthorizationSession> authzSession;

    void setUp() {
        auto localManagerState = stdx::make_unique<FailureCapableAuthzManagerExternalStateMock>();
        managerState = localManagerState.get();
        managerState->setAuthzVersion(AuthorizationManager::schemaVersion26Final);
        authzManager = stdx::make_unique<AuthorizationManager>(std::move(localManagerState));
        auto localSessionState =
            stdx::make_unique<AuthzSessionExternalStateMock>(authzManager.get());
        sessionState = localSessionState.get();
        authzSession = stdx::make_unique<AuthorizationSession>(std::move(localSessionState));
        authzManager->setAuthEnabled(true);
    }
};

const ResourcePattern testDBResource(ResourcePattern::forDatabaseName("test"));
const ResourcePattern otherDBResource(ResourcePattern::forDatabaseName("other"));
const ResourcePattern adminDBResource(ResourcePattern::forDatabaseName("admin"));
const ResourcePattern testFooCollResource(
    ResourcePattern::forExactNamespace(NamespaceString("test.foo")));
const ResourcePattern otherFooCollResource(
    ResourcePattern::forExactNamespace(NamespaceString("other.foo")));
const ResourcePattern thirdFooCollResource(
    ResourcePattern::forExactNamespace(NamespaceString("third.foo")));
const ResourcePattern adminFooCollResource(
    ResourcePattern::forExactNamespace(NamespaceString("admin.foo")));
const ResourcePattern testUsersCollResource(
    ResourcePattern::forExactNamespace(NamespaceString("test.system.users")));
const ResourcePattern otherUsersCollResource(
    ResourcePattern::forExactNamespace(NamespaceString("other.system.users")));
const ResourcePattern thirdUsersCollResource(
    ResourcePattern::forExactNamespace(NamespaceString("third.system.users")));
const ResourcePattern testIndexesCollResource(
    ResourcePattern::forExactNamespace(NamespaceString("test.system.indexes")));
const ResourcePattern otherIndexesCollResource(
    ResourcePattern::forExactNamespace(NamespaceString("other.system.indexes")));
const ResourcePattern thirdIndexesCollResource(
    ResourcePattern::forExactNamespace(NamespaceString("third.system.indexes")));
const ResourcePattern testProfileCollResource(
    ResourcePattern::forExactNamespace(NamespaceString("test.system.profile")));
const ResourcePattern otherProfileCollResource(
    ResourcePattern::forExactNamespace(NamespaceString("other.system.profile")));
const ResourcePattern thirdProfileCollResource(
    ResourcePattern::forExactNamespace(NamespaceString("third.system.profile")));

TEST_F(AuthorizationSessionTest, AddUserAndCheckAuthorization) {
    // Check that disabling auth checks works
    ASSERT_FALSE(
        authzSession->isAuthorizedForActionsOnResource(testFooCollResource, ActionType::insert));
    sessionState->setReturnValueForShouldIgnoreAuthChecks(true);
    ASSERT_TRUE(
        authzSession->isAuthorizedForActionsOnResource(testFooCollResource, ActionType::insert));
    sessionState->setReturnValueForShouldIgnoreAuthChecks(false);
    ASSERT_FALSE(
        authzSession->isAuthorizedForActionsOnResource(testFooCollResource, ActionType::insert));

    // Check that you can't authorize a user that doesn't exist.
    ASSERT_EQUALS(ErrorCodes::UserNotFound,
                  authzSession->addAndAuthorizeUser(&_txn, UserName("spencer", "test")));

    // Add a user with readWrite and dbAdmin on the test DB
    ASSERT_OK(managerState->insertPrivilegeDocument(&_txn,
                                                    BSON("user"
                                                         << "spencer"
                                                         << "db"
                                                         << "test"
                                                         << "credentials"
                                                         << BSON("MONGODB-CR"
                                                                 << "a")
                                                         << "roles"
                                                         << BSON_ARRAY(BSON("role"
                                                                            << "readWrite"
                                                                            << "db"
                                                                            << "test")
                                                                       << BSON("role"
                                                                               << "dbAdmin"
                                                                               << "db"
                                                                               << "test"))),
                                                    BSONObj()));
    ASSERT_OK(authzSession->addAndAuthorizeUser(&_txn, UserName("spencer", "test")));

    ASSERT_TRUE(
        authzSession->isAuthorizedForActionsOnResource(testFooCollResource, ActionType::insert));
    ASSERT_TRUE(
        authzSession->isAuthorizedForActionsOnResource(testDBResource, ActionType::dbStats));
    ASSERT_FALSE(
        authzSession->isAuthorizedForActionsOnResource(otherFooCollResource, ActionType::insert));

    // Add an admin user with readWriteAnyDatabase
    ASSERT_OK(
        managerState->insertPrivilegeDocument(&_txn,
                                              BSON("user"
                                                   << "admin"
                                                   << "db"
                                                   << "admin"
                                                   << "credentials"
                                                   << BSON("MONGODB-CR"
                                                           << "a")
                                                   << "roles"
                                                   << BSON_ARRAY(BSON("role"
                                                                      << "readWriteAnyDatabase"
                                                                      << "db"
                                                                      << "admin"))),
                                              BSONObj()));
    ASSERT_OK(authzSession->addAndAuthorizeUser(&_txn, UserName("admin", "admin")));

    ASSERT_TRUE(authzSession->isAuthorizedForActionsOnResource(
        ResourcePattern::forExactNamespace(NamespaceString("anydb.somecollection")),
        ActionType::insert));
    ASSERT_TRUE(
        authzSession->isAuthorizedForActionsOnResource(otherDBResource, ActionType::insert));
    ASSERT_TRUE(
        authzSession->isAuthorizedForActionsOnResource(otherFooCollResource, ActionType::insert));
    ASSERT_FALSE(
        authzSession->isAuthorizedForActionsOnResource(otherFooCollResource, ActionType::collMod));
    ASSERT_TRUE(
        authzSession->isAuthorizedForActionsOnResource(testFooCollResource, ActionType::insert));

    authzSession->logoutDatabase("test");
    ASSERT_TRUE(
        authzSession->isAuthorizedForActionsOnResource(otherFooCollResource, ActionType::insert));
    ASSERT_TRUE(
        authzSession->isAuthorizedForActionsOnResource(testFooCollResource, ActionType::insert));
    ASSERT_FALSE(
        authzSession->isAuthorizedForActionsOnResource(testFooCollResource, ActionType::collMod));

    authzSession->logoutDatabase("admin");
    ASSERT_FALSE(
        authzSession->isAuthorizedForActionsOnResource(otherFooCollResource, ActionType::insert));
    ASSERT_FALSE(
        authzSession->isAuthorizedForActionsOnResource(testFooCollResource, ActionType::insert));
    ASSERT_FALSE(
        authzSession->isAuthorizedForActionsOnResource(testFooCollResource, ActionType::collMod));
}

TEST_F(AuthorizationSessionTest, DuplicateRolesOK) {
    // Add a user with doubled-up readWrite and single dbAdmin on the test DB
    ASSERT_OK(managerState->insertPrivilegeDocument(&_txn,
                                                    BSON("user"
                                                         << "spencer"
                                                         << "db"
                                                         << "test"
                                                         << "credentials"
                                                         << BSON("MONGODB-CR"
                                                                 << "a")
                                                         << "roles"
                                                         << BSON_ARRAY(BSON("role"
                                                                            << "readWrite"
                                                                            << "db"
                                                                            << "test")
                                                                       << BSON("role"
                                                                               << "dbAdmin"
                                                                               << "db"
                                                                               << "test")
                                                                       << BSON("role"
                                                                               << "readWrite"
                                                                               << "db"
                                                                               << "test"))),
                                                    BSONObj()));
    ASSERT_OK(authzSession->addAndAuthorizeUser(&_txn, UserName("spencer", "test")));

    ASSERT_TRUE(
        authzSession->isAuthorizedForActionsOnResource(testFooCollResource, ActionType::insert));
    ASSERT_TRUE(
        authzSession->isAuthorizedForActionsOnResource(testDBResource, ActionType::dbStats));
    ASSERT_FALSE(
        authzSession->isAuthorizedForActionsOnResource(otherFooCollResource, ActionType::insert));
}

TEST_F(AuthorizationSessionTest, SystemCollectionsAccessControl) {
    ASSERT_OK(managerState->insertPrivilegeDocument(&_txn,
                                                    BSON("user"
                                                         << "rw"
                                                         << "db"
                                                         << "test"
                                                         << "credentials"
                                                         << BSON("MONGODB-CR"
                                                                 << "a")
                                                         << "roles"
                                                         << BSON_ARRAY(BSON("role"
                                                                            << "readWrite"
                                                                            << "db"
                                                                            << "test")
                                                                       << BSON("role"
                                                                               << "dbAdmin"
                                                                               << "db"
                                                                               << "test"))),
                                                    BSONObj()));
    ASSERT_OK(managerState->insertPrivilegeDocument(&_txn,
                                                    BSON("user"
                                                         << "useradmin"
                                                         << "db"
                                                         << "test"
                                                         << "credentials"
                                                         << BSON("MONGODB-CR"
                                                                 << "a")
                                                         << "roles"
                                                         << BSON_ARRAY(BSON("role"
                                                                            << "userAdmin"
                                                                            << "db"
                                                                            << "test"))),
                                                    BSONObj()));
    ASSERT_OK(
        managerState->insertPrivilegeDocument(&_txn,
                                              BSON("user"
                                                   << "rwany"
                                                   << "db"
                                                   << "test"
                                                   << "credentials"
                                                   << BSON("MONGODB-CR"
                                                           << "a")
                                                   << "roles"
                                                   << BSON_ARRAY(BSON("role"
                                                                      << "readWriteAnyDatabase"
                                                                      << "db"
                                                                      << "admin")
                                                                 << BSON("role"
                                                                         << "dbAdminAnyDatabase"
                                                                         << "db"
                                                                         << "admin"))),
                                              BSONObj()));
    ASSERT_OK(
        managerState->insertPrivilegeDocument(&_txn,
                                              BSON("user"
                                                   << "useradminany"
                                                   << "db"
                                                   << "test"
                                                   << "credentials"
                                                   << BSON("MONGODB-CR"
                                                           << "a")
                                                   << "roles"
                                                   << BSON_ARRAY(BSON("role"
                                                                      << "userAdminAnyDatabase"
                                                                      << "db"
                                                                      << "admin"))),
                                              BSONObj()));

    ASSERT_OK(authzSession->addAndAuthorizeUser(&_txn, UserName("rwany", "test")));

    ASSERT_FALSE(
        authzSession->isAuthorizedForActionsOnResource(testUsersCollResource, ActionType::insert));
    ASSERT_FALSE(
        authzSession->isAuthorizedForActionsOnResource(testUsersCollResource, ActionType::find));
    ASSERT_FALSE(
        authzSession->isAuthorizedForActionsOnResource(otherUsersCollResource, ActionType::insert));
    ASSERT_FALSE(
        authzSession->isAuthorizedForActionsOnResource(otherUsersCollResource, ActionType::find));
    ASSERT_TRUE(
        authzSession->isAuthorizedForActionsOnResource(testIndexesCollResource, ActionType::find));
    ASSERT_TRUE(
        authzSession->isAuthorizedForActionsOnResource(testProfileCollResource, ActionType::find));
    ASSERT_TRUE(
        authzSession->isAuthorizedForActionsOnResource(otherIndexesCollResource, ActionType::find));
    ASSERT_TRUE(
        authzSession->isAuthorizedForActionsOnResource(otherProfileCollResource, ActionType::find));

    // Logging in as useradminany@test implicitly logs out rwany@test.
    ASSERT_OK(authzSession->addAndAuthorizeUser(&_txn, UserName("useradminany", "test")));
    ASSERT_FALSE(
        authzSession->isAuthorizedForActionsOnResource(testUsersCollResource, ActionType::insert));
    ASSERT_TRUE(
        authzSession->isAuthorizedForActionsOnResource(testUsersCollResource, ActionType::find));
    ASSERT_FALSE(
        authzSession->isAuthorizedForActionsOnResource(otherUsersCollResource, ActionType::insert));
    ASSERT_TRUE(
        authzSession->isAuthorizedForActionsOnResource(otherUsersCollResource, ActionType::find));
    ASSERT_FALSE(
        authzSession->isAuthorizedForActionsOnResource(testIndexesCollResource, ActionType::find));
    ASSERT_FALSE(
        authzSession->isAuthorizedForActionsOnResource(testProfileCollResource, ActionType::find));
    ASSERT_FALSE(
        authzSession->isAuthorizedForActionsOnResource(otherIndexesCollResource, ActionType::find));
    ASSERT_FALSE(
        authzSession->isAuthorizedForActionsOnResource(otherProfileCollResource, ActionType::find));

    // Logging in as rw@test implicitly logs out useradminany@test.
    ASSERT_OK(authzSession->addAndAuthorizeUser(&_txn, UserName("rw", "test")));

    ASSERT_FALSE(
        authzSession->isAuthorizedForActionsOnResource(testUsersCollResource, ActionType::insert));
    ASSERT_FALSE(
        authzSession->isAuthorizedForActionsOnResource(testUsersCollResource, ActionType::find));
    ASSERT_FALSE(
        authzSession->isAuthorizedForActionsOnResource(otherUsersCollResource, ActionType::insert));
    ASSERT_FALSE(
        authzSession->isAuthorizedForActionsOnResource(otherUsersCollResource, ActionType::find));
    ASSERT_TRUE(
        authzSession->isAuthorizedForActionsOnResource(testIndexesCollResource, ActionType::find));
    ASSERT_TRUE(
        authzSession->isAuthorizedForActionsOnResource(testProfileCollResource, ActionType::find));
    ASSERT_FALSE(
        authzSession->isAuthorizedForActionsOnResource(otherIndexesCollResource, ActionType::find));
    ASSERT_FALSE(
        authzSession->isAuthorizedForActionsOnResource(otherProfileCollResource, ActionType::find));


    // Logging in as useradmin@test implicitly logs out rw@test.
    ASSERT_OK(authzSession->addAndAuthorizeUser(&_txn, UserName("useradmin", "test")));
    ASSERT_FALSE(
        authzSession->isAuthorizedForActionsOnResource(testUsersCollResource, ActionType::insert));
    ASSERT_FALSE(
        authzSession->isAuthorizedForActionsOnResource(testUsersCollResource, ActionType::find));
    ASSERT_FALSE(
        authzSession->isAuthorizedForActionsOnResource(otherUsersCollResource, ActionType::insert));
    ASSERT_FALSE(
        authzSession->isAuthorizedForActionsOnResource(otherUsersCollResource, ActionType::find));
    ASSERT_FALSE(
        authzSession->isAuthorizedForActionsOnResource(testIndexesCollResource, ActionType::find));
    ASSERT_FALSE(
        authzSession->isAuthorizedForActionsOnResource(testProfileCollResource, ActionType::find));
    ASSERT_FALSE(
        authzSession->isAuthorizedForActionsOnResource(otherIndexesCollResource, ActionType::find));
    ASSERT_FALSE(
        authzSession->isAuthorizedForActionsOnResource(otherProfileCollResource, ActionType::find));
}

TEST_F(AuthorizationSessionTest, InvalidateUser) {
    // Add a readWrite user
    ASSERT_OK(managerState->insertPrivilegeDocument(&_txn,
                                                    BSON("user"
                                                         << "spencer"
                                                         << "db"
                                                         << "test"
                                                         << "credentials"
                                                         << BSON("MONGODB-CR"
                                                                 << "a")
                                                         << "roles"
                                                         << BSON_ARRAY(BSON("role"
                                                                            << "readWrite"
                                                                            << "db"
                                                                            << "test"))),
                                                    BSONObj()));
    ASSERT_OK(authzSession->addAndAuthorizeUser(&_txn, UserName("spencer", "test")));

    ASSERT_TRUE(
        authzSession->isAuthorizedForActionsOnResource(testFooCollResource, ActionType::find));
    ASSERT_TRUE(
        authzSession->isAuthorizedForActionsOnResource(testFooCollResource, ActionType::insert));

    User* user = authzSession->lookupUser(UserName("spencer", "test"));
    ASSERT(user->isValid());

    // Change the user to be read-only
    int ignored;
    managerState->remove(
        &_txn, AuthorizationManager::usersCollectionNamespace, BSONObj(), BSONObj(), &ignored);
    ASSERT_OK(managerState->insertPrivilegeDocument(&_txn,
                                                    BSON("user"
                                                         << "spencer"
                                                         << "db"
                                                         << "test"
                                                         << "credentials"
                                                         << BSON("MONGODB-CR"
                                                                 << "a")
                                                         << "roles"
                                                         << BSON_ARRAY(BSON("role"
                                                                            << "read"
                                                                            << "db"
                                                                            << "test"))),
                                                    BSONObj()));

    // Make sure that invalidating the user causes the session to reload its privileges.
    authzManager->invalidateUserByName(user->getName());
    authzSession->startRequest(&_txn);  // Refreshes cached data for invalid users
    ASSERT_TRUE(
        authzSession->isAuthorizedForActionsOnResource(testFooCollResource, ActionType::find));
    ASSERT_FALSE(
        authzSession->isAuthorizedForActionsOnResource(testFooCollResource, ActionType::insert));

    user = authzSession->lookupUser(UserName("spencer", "test"));
    ASSERT(user->isValid());

    // Delete the user.
    managerState->remove(
        &_txn, AuthorizationManager::usersCollectionNamespace, BSONObj(), BSONObj(), &ignored);
    // Make sure that invalidating the user causes the session to reload its privileges.
    authzManager->invalidateUserByName(user->getName());
    authzSession->startRequest(&_txn);  // Refreshes cached data for invalid users
    ASSERT_FALSE(
        authzSession->isAuthorizedForActionsOnResource(testFooCollResource, ActionType::find));
    ASSERT_FALSE(
        authzSession->isAuthorizedForActionsOnResource(testFooCollResource, ActionType::insert));
    ASSERT_FALSE(authzSession->lookupUser(UserName("spencer", "test")));
}

TEST_F(AuthorizationSessionTest, UseOldUserInfoInFaceOfConnectivityProblems) {
    // Add a readWrite user
    ASSERT_OK(managerState->insertPrivilegeDocument(&_txn,
                                                    BSON("user"
                                                         << "spencer"
                                                         << "db"
                                                         << "test"
                                                         << "credentials"
                                                         << BSON("MONGODB-CR"
                                                                 << "a")
                                                         << "roles"
                                                         << BSON_ARRAY(BSON("role"
                                                                            << "readWrite"
                                                                            << "db"
                                                                            << "test"))),
                                                    BSONObj()));
    ASSERT_OK(authzSession->addAndAuthorizeUser(&_txn, UserName("spencer", "test")));

    ASSERT_TRUE(
        authzSession->isAuthorizedForActionsOnResource(testFooCollResource, ActionType::find));
    ASSERT_TRUE(
        authzSession->isAuthorizedForActionsOnResource(testFooCollResource, ActionType::insert));

    User* user = authzSession->lookupUser(UserName("spencer", "test"));
    ASSERT(user->isValid());

    // Change the user to be read-only
    int ignored;
    managerState->setFindsShouldFail(true);
    managerState->remove(
        &_txn, AuthorizationManager::usersCollectionNamespace, BSONObj(), BSONObj(), &ignored);
    ASSERT_OK(managerState->insertPrivilegeDocument(&_txn,
                                                    BSON("user"
                                                         << "spencer"
                                                         << "db"
                                                         << "test"
                                                         << "credentials"
                                                         << BSON("MONGODB-CR"
                                                                 << "a")
                                                         << "roles"
                                                         << BSON_ARRAY(BSON("role"
                                                                            << "read"
                                                                            << "db"
                                                                            << "test"))),
                                                    BSONObj()));

    // Even though the user's privileges have been reduced, since we've configured user
    // document lookup to fail, the authz session should continue to use its known out-of-date
    // privilege data.
    authzManager->invalidateUserByName(user->getName());
    authzSession->startRequest(&_txn);  // Refreshes cached data for invalid users
    ASSERT_TRUE(
        authzSession->isAuthorizedForActionsOnResource(testFooCollResource, ActionType::find));
    ASSERT_TRUE(
        authzSession->isAuthorizedForActionsOnResource(testFooCollResource, ActionType::insert));

    // Once we configure document lookup to succeed again, authorization checks should
    // observe the new values.
    managerState->setFindsShouldFail(false);
    authzSession->startRequest(&_txn);  // Refreshes cached data for invalid users
    ASSERT_TRUE(
        authzSession->isAuthorizedForActionsOnResource(testFooCollResource, ActionType::find));
    ASSERT_FALSE(
        authzSession->isAuthorizedForActionsOnResource(testFooCollResource, ActionType::insert));
}

}  // namespace
}  // namespace mongo
