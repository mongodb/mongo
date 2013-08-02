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


    class AuthorizationSessionTest : public ::mongo::unittest::Test {
    public:
        AuthzManagerExternalStateMock* managerState;
        AuthzSessionExternalStateMock* sessionState;
        scoped_ptr<AuthorizationManager> authzManager;
        scoped_ptr<AuthorizationSession> authzSession;

        void setUp() {
            managerState = new AuthzManagerExternalStateMock();
            authzManager.reset(new AuthorizationManager(managerState));
            sessionState = new AuthzSessionExternalStateMock(authzManager.get());
            authzSession.reset(new AuthorizationSession(sessionState));
        }

        void tearDown() {
            // All users have to have a ref count of zero when the AuthorizationManager is destroyed
            while (internalSecurity.user->getRefCount() > 0) {
                internalSecurity.user->decrementRefCount();
            }
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
        ASSERT_OK(managerState->insertPrivilegeDocument("test",
                BSON("user" << "spencer" <<
                     "pwd" << "a" <<
                     "roles" << BSON_ARRAY("readWrite" << "dbAdmin"))));
        ASSERT_OK(authzSession->addAndAuthorizeUser(UserName("spencer", "test")));

        ASSERT_TRUE(authzSession->checkAuthorization("test", ActionType::insert));
        ASSERT_TRUE(authzSession->checkAuthorization("test", ActionType::collMod));
        // Auth checks on a collection should be applied to the database name.
        ASSERT_TRUE(authzSession->checkAuthorization("test.foo", ActionType::insert));
        ASSERT_FALSE(authzSession->checkAuthorization("otherDb", ActionType::insert));

        // Add an admin user with readWriteAnyDatabase
        ASSERT_OK(managerState->insertPrivilegeDocument("admin",
                BSON("user" << "admin" <<
                     "pwd" << "a" <<
                     "roles" << BSON_ARRAY("readWriteAnyDatabase"))));
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


    TEST_F(AuthorizationSessionTest, ImplicitAcquireFromSomeDatabasesWithV1Users) {
        managerState->insertPrivilegeDocument("test",
                                    BSON("user" << "andy" <<
                                         "pwd" << "a" <<
                                         "roles" << BSON_ARRAY("readWrite")));
        managerState->insertPrivilegeDocument("test2",
                                    BSON("user" << "andy" <<
                                         "userSource" << "test" <<
                                         "roles" <<  BSON_ARRAY("read")));
        managerState->insertPrivilegeDocument("admin",
                                    BSON("user" << "andy" <<
                                         "userSource" << "test" <<
                                         "roles" << BSON_ARRAY("clusterAdmin") <<
                                         "otherDBRoles" << BSON("test3" << BSON_ARRAY("dbAdmin"))));

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
        ASSERT_OK(authzManager->initializeAllV1UserData());

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
    }

}  // namespace
}  // namespace mongo
