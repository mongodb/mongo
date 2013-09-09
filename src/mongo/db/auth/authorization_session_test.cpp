/*    Copyright 2012 10gen Inc.
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 */

/**
 * Unit tests of the AuthorizationSession type.
 */

#include "mongo/base/status.h"
#include "mongo/db/auth/authz_session_external_state_mock.h"
#include "mongo/db/auth/authz_manager_external_state_mock.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/namespace_string.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/map_util.h"

#define ASSERT_NULL(EXPR) ASSERT_FALSE(EXPR)
#define ASSERT_NON_NULL(EXPR) ASSERT_TRUE(EXPR)

namespace mongo {
namespace {

    class FailureCapableAuthzManagerExternalStateMock :
        public AuthzManagerExternalStateMock {
    public:
        FailureCapableAuthzManagerExternalStateMock() : _findsShouldFail(false) {}
        virtual ~FailureCapableAuthzManagerExternalStateMock() {}

        void setFindsShouldFail(bool enable) { _findsShouldFail = enable; }

        virtual Status _findUser(const std::string& usersNamespace,
                                 const BSONObj& query,
                                 BSONObj* result) {
            if (_findsShouldFail) {
                return Status(ErrorCodes::UnknownError, "_findUser set to fail in mock.");
            }
            return AuthzManagerExternalStateMock::_findUser(usersNamespace, query, result);
        }

    private:
        bool _findsShouldFail;
    };

    class AuthorizationSessionTest : public ::mongo::unittest::Test {
    public:
        FailureCapableAuthzManagerExternalStateMock* managerState;
        AuthzSessionExternalStateMock* sessionState;
        scoped_ptr<AuthorizationManager> authzManager;
        scoped_ptr<AuthorizationSession> authzSession;

        void setUp() {
            managerState = new FailureCapableAuthzManagerExternalStateMock();
            authzManager.reset(new AuthorizationManager(managerState));
            sessionState = new AuthzSessionExternalStateMock(authzManager.get());
            authzSession.reset(new AuthorizationSession(sessionState));
            // This duplicates the behavior from the server that adds the internal user at process
            // startup via a MONGO_INITIALIZER
            authzManager->addInternalUser(internalSecurity.user);
        }
    };

    TEST_F(AuthorizationSessionTest, AddUserAndCheckAuthorization) {
        // Check that disabling auth checks works
        ASSERT_FALSE(authzSession->checkAuthorization("test", ActionType::insert));
        sessionState->setReturnValueForShouldIgnoreAuthChecks(true);
        ASSERT_TRUE(authzSession->checkAuthorization("test", ActionType::insert));
        sessionState->setReturnValueForShouldIgnoreAuthChecks(false);
        ASSERT_FALSE(authzSession->checkAuthorization("test", ActionType::insert));

        // Check that you can't authorize a user that doesn't exist.
        ASSERT_EQUALS(ErrorCodes::UserNotFound,
                      authzSession->addAndAuthorizeUser(UserName("spencer", "test")));

        // Add a user with readWrite and dbAdmin on the test DB
        ASSERT_OK(managerState->insertPrivilegeDocument("admin",
                BSON("name" << "spencer" <<
                     "source" << "test" <<
                     "credentials" << BSON("MONGODB-CR" << "a") <<
                     "roles" << BSON_ARRAY(BSON("name" << "readWrite" <<
                                                "source" << "test" <<
                                                "hasRole" << true <<
                                                "canDelegate" << false) <<
                                           BSON("name" << "dbAdmin" <<
                                                "source" << "test" <<
                                                "hasRole" << true <<
                                                "canDelegate" << false))),
                BSONObj()));
        ASSERT_OK(authzSession->addAndAuthorizeUser(UserName("spencer", "test")));

        ASSERT_TRUE(authzSession->checkAuthorization("test", ActionType::insert));
        ASSERT_TRUE(authzSession->checkAuthorization("test", ActionType::collMod));
        // Auth checks on a collection should be applied to the database name.
        ASSERT_TRUE(authzSession->checkAuthorization("test.foo", ActionType::insert));
        ASSERT_FALSE(authzSession->checkAuthorization("otherDb", ActionType::insert));

        // Add an admin user with readWriteAnyDatabase
        ASSERT_OK(managerState->insertPrivilegeDocument("admin",
                BSON("name" << "admin" <<
                     "source" << "admin" <<
                     "credentials" << BSON("MONGODB-CR" << "a") <<
                     "roles" << BSON_ARRAY(BSON("name" << "readWriteAnyDatabase" <<
                                                "source" << "admin" <<
                                                "hasRole" << true <<
                                                "canDelegate" << false))),
                BSONObj()));
        ASSERT_OK(authzSession->addAndAuthorizeUser(UserName("admin", "admin")));

        ASSERT_TRUE(authzSession->checkAuthorization("*", ActionType::insert));
        ASSERT_TRUE(authzSession->checkAuthorization("otherDb", ActionType::insert));
        ASSERT_TRUE(authzSession->checkAuthorization("otherDb.foo", ActionType::insert));
        ASSERT_FALSE(authzSession->checkAuthorization("otherDb.foo", ActionType::collMod));
        ASSERT_TRUE(authzSession->checkAuthorization("test.foo", ActionType::insert));

        authzSession->logoutDatabase("test");
        ASSERT_TRUE(authzSession->checkAuthorization("otherDb", ActionType::insert));
        ASSERT_TRUE(authzSession->checkAuthorization("test", ActionType::insert));
        ASSERT_FALSE(authzSession->checkAuthorization("test.foo", ActionType::collMod));

        authzSession->logoutDatabase("admin");
        ASSERT_FALSE(authzSession->checkAuthorization("otherDb", ActionType::insert));
        ASSERT_FALSE(authzSession->checkAuthorization("test", ActionType::insert));
        ASSERT_FALSE(authzSession->checkAuthorization("test.foo", ActionType::collMod));
    }

    TEST_F(AuthorizationSessionTest, InvalidateUser) {
        // Add a readWrite user
        ASSERT_OK(managerState->insertPrivilegeDocument("admin",
                BSON("name" << "spencer" <<
                     "source" << "test" <<
                     "credentials" << BSON("MONGODB-CR" << "a") <<
                     "roles" << BSON_ARRAY(BSON("name" << "readWrite" <<
                                                "source" << "test" <<
                                                "hasRole" << true <<
                                                "canDelegate" << false))),
                BSONObj()));
        ASSERT_OK(authzSession->addAndAuthorizeUser(UserName("spencer", "test")));

        ASSERT_TRUE(authzSession->checkAuthorization("test", ActionType::find));
        ASSERT_TRUE(authzSession->checkAuthorization("test", ActionType::insert));

        User* user = authzSession->lookupUser(UserName("spencer", "test"));
        ASSERT(user->isValid());

        // Change the user to be read-only
        managerState->clearPrivilegeDocuments();
        ASSERT_OK(managerState->insertPrivilegeDocument("admin",
                BSON("name" << "spencer" <<
                     "source" << "test" <<
                     "credentials" << BSON("MONGODB-CR" << "a") <<
                     "roles" << BSON_ARRAY(BSON("name" << "read" <<
                                                "source" << "test" <<
                                                "hasRole" << true <<
                                                "canDelegate" << false))),
                BSONObj()));

        // Make sure that invalidating the user causes the session to reload its privileges.
        authzManager->invalidateUser(user);
        ASSERT_TRUE(authzSession->checkAuthorization("test", ActionType::find));
        ASSERT_FALSE(authzSession->checkAuthorization("test", ActionType::insert));

        user = authzSession->lookupUser(UserName("spencer", "test"));
        ASSERT(user->isValid());

        // Delete the user.
        managerState->clearPrivilegeDocuments();
        // Make sure that invalidating the user causes the session to reload its privileges.
        authzManager->invalidateUser(user);
        ASSERT_FALSE(authzSession->checkAuthorization("test", ActionType::find));
        ASSERT_FALSE(authzSession->checkAuthorization("test", ActionType::insert));
        ASSERT_FALSE(authzSession->lookupUser(UserName("spencer", "test")));
    }

    TEST_F(AuthorizationSessionTest, UseOldUserInfoInFaceOfConnectivityProblems) {
        // Add a readWrite user
        ASSERT_OK(managerState->insertPrivilegeDocument("admin",
                BSON("name" << "spencer" <<
                     "source" << "test" <<
                     "credentials" << BSON("MONGODB-CR" << "a") <<
                     "roles" << BSON_ARRAY(BSON("name" << "readWrite" <<
                                                "source" << "test" <<
                                                "hasRole" << true <<
                                                "canDelegate" << false))),
                BSONObj()));
        ASSERT_OK(authzSession->addAndAuthorizeUser(UserName("spencer", "test")));

        ASSERT_TRUE(authzSession->checkAuthorization("test", ActionType::find));
        ASSERT_TRUE(authzSession->checkAuthorization("test", ActionType::insert));

        User* user = authzSession->lookupUser(UserName("spencer", "test"));
        ASSERT(user->isValid());

        // Change the user to be read-only
        managerState->setFindsShouldFail(true);
        managerState->clearPrivilegeDocuments();
        ASSERT_OK(managerState->insertPrivilegeDocument("admin",
                BSON("name" << "spencer" <<
                     "source" << "test" <<
                     "credentials" << BSON("MONGODB-CR" << "a") <<
                     "roles" << BSON_ARRAY(BSON("name" << "read" <<
                                                "source" << "test" <<
                                                "hasRole" << true <<
                                                "canDelegate" << false))),
                BSONObj()));

        // Even though the user's privileges have been reduced, since we've configured user
        // document lookup to fail, the authz session should continue to use its known out-of-date
        // privilege data.
        authzManager->invalidateUser(user);
        ASSERT_TRUE(authzSession->checkAuthorization("test", ActionType::find));
        ASSERT_TRUE(authzSession->checkAuthorization("test", ActionType::insert));
    }


    TEST_F(AuthorizationSessionTest, ImplicitAcquireFromSomeDatabasesWithV1Users) {
        authzManager->setAuthorizationVersion(1);

        managerState->insert(NamespaceString("test.system.users"),
                                    BSON("user" << "andy" <<
                                         "pwd" << "a" <<
                                         "roles" << BSON_ARRAY("readWrite")),
                                    BSONObj());
        managerState->insert(NamespaceString("test2.system.users"),
                                    BSON("user" << "andy" <<
                                         "userSource" << "test" <<
                                         "roles" <<  BSON_ARRAY("read")),
                                    BSONObj());
        managerState->insert(NamespaceString("admin.system.users"),
                                    BSON("user" << "andy" <<
                                         "userSource" << "test" <<
                                         "roles" << BSON_ARRAY("clusterAdmin") <<
                                         "otherDBRoles" << BSON("test3" << BSON_ARRAY("dbAdmin"))),
                                    BSONObj());
        ASSERT_OK(authzManager->initialize());

        ASSERT(!authzSession->checkAuthorization("test.foo", ActionType::find));
        ASSERT(!authzSession->checkAuthorization("test.foo", ActionType::insert));
        ASSERT(!authzSession->checkAuthorization("test.foo", ActionType::collMod));
        ASSERT(!authzSession->checkAuthorization("test2.foo", ActionType::find));
        ASSERT(!authzSession->checkAuthorization("test2.foo", ActionType::insert));
        ASSERT(!authzSession->checkAuthorization("test2.foo", ActionType::collMod));
        ASSERT(!authzSession->checkAuthorization("test3.foo", ActionType::find));
        ASSERT(!authzSession->checkAuthorization("test3.foo", ActionType::insert));
        ASSERT(!authzSession->checkAuthorization("test3.foo", ActionType::collMod));
        ASSERT(!authzSession->checkAuthorization("admin.foo", ActionType::find));
        ASSERT(!authzSession->checkAuthorization("admin.foo", ActionType::insert));
        ASSERT(!authzSession->checkAuthorization("admin.foo", ActionType::collMod));
        ASSERT(!authzSession->checkAuthorization("$SERVER", ActionType::shutdown));

        ASSERT_OK(authzSession->addAndAuthorizeUser(UserName("andy", "test")));

        User* user = authzSession->lookupUser(UserName("andy", "test"));
        ASSERT(UserName("andy", "test") == user->getName());

        ASSERT(authzSession->checkAuthorization("test.foo", ActionType::find));
        ASSERT(authzSession->checkAuthorization("test.foo", ActionType::insert));
        ASSERT(!authzSession->checkAuthorization("test.foo", ActionType::collMod));
        ASSERT(authzSession->checkAuthorization("test2.foo", ActionType::find));
        ASSERT(!authzSession->checkAuthorization("test2.foo", ActionType::insert));
        ASSERT(!authzSession->checkAuthorization("test2.foo", ActionType::collMod));
        ASSERT(!authzSession->checkAuthorization("test3.foo", ActionType::find));
        ASSERT(!authzSession->checkAuthorization("test3.foo", ActionType::insert));
        ASSERT(authzSession->checkAuthorization("test3.foo", ActionType::collMod));
        ASSERT(!authzSession->checkAuthorization("admin.foo", ActionType::find));
        ASSERT(!authzSession->checkAuthorization("admin.foo", ActionType::insert));
        ASSERT(!authzSession->checkAuthorization("admin.foo", ActionType::collMod));
        ASSERT(authzSession->checkAuthorization("$SERVER", ActionType::shutdown));

        authzSession->logoutDatabase("test");

        ASSERT(!authzSession->checkAuthorization("test.foo", ActionType::find));
        ASSERT(!authzSession->checkAuthorization("test.foo", ActionType::insert));
        ASSERT(!authzSession->checkAuthorization("test.foo", ActionType::collMod));
        ASSERT(!authzSession->checkAuthorization("test2.foo", ActionType::find));
        ASSERT(!authzSession->checkAuthorization("test2.foo", ActionType::insert));
        ASSERT(!authzSession->checkAuthorization("test2.foo", ActionType::collMod));
        ASSERT(!authzSession->checkAuthorization("test3.foo", ActionType::find));
        ASSERT(!authzSession->checkAuthorization("test3.foo", ActionType::insert));
        ASSERT(!authzSession->checkAuthorization("test3.foo", ActionType::collMod));
        ASSERT(!authzSession->checkAuthorization("admin.foo", ActionType::find));
        ASSERT(!authzSession->checkAuthorization("admin.foo", ActionType::insert));
        ASSERT(!authzSession->checkAuthorization("admin.foo", ActionType::collMod));
        ASSERT(!authzSession->checkAuthorization("$SERVER", ActionType::shutdown));

        // initializeAllV1UserData() pins the users by adding 1 to their refCount, so need to
        // release the user an extra time to bring its refCount to 0.
        authzManager->releaseUser(user);
    }

}  // namespace
}  // namespace mongo
