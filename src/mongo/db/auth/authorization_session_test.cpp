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

    TEST(AuthorizationSessionTest, AcquirePrivilegeAndCheckAuthorization) {
        Principal* principal = new Principal(UserName("Spencer", "test"));
        ActionSet actions;
        actions.addAction(ActionType::insert);
        Privilege writePrivilege("test", actions);
        Privilege allDBsWritePrivilege("*", actions);
        AuthzManagerExternalStateMock* managerExternalState = new AuthzManagerExternalStateMock();
        AuthorizationManager authManager(managerExternalState);
        AuthzSessionExternalStateMock* sessionExternalState = new AuthzSessionExternalStateMock(
                &authManager);
        AuthorizationSession authzSession(sessionExternalState);

        ASSERT_FALSE(authzSession.checkAuthorization("test", ActionType::insert));
        sessionExternalState->setReturnValueForShouldIgnoreAuthChecks(true);
        ASSERT_TRUE(authzSession.checkAuthorization("test", ActionType::insert));
        sessionExternalState->setReturnValueForShouldIgnoreAuthChecks(false);
        ASSERT_FALSE(authzSession.checkAuthorization("test", ActionType::insert));

        ASSERT_EQUALS(ErrorCodes::UserNotFound,
                      authzSession.acquirePrivilege(writePrivilege, principal->getName()));
        authzSession.addAndAuthorizePrincipal(principal);
        ASSERT_OK(authzSession.acquirePrivilege(writePrivilege, principal->getName()));
        ASSERT_TRUE(authzSession.checkAuthorization("test", ActionType::insert));

        ASSERT_FALSE(authzSession.checkAuthorization("otherDb", ActionType::insert));
        ASSERT_OK(authzSession.acquirePrivilege(allDBsWritePrivilege, principal->getName()));
        ASSERT_TRUE(authzSession.checkAuthorization("otherDb", ActionType::insert));
        // Auth checks on a collection should be applied to the database name.
        ASSERT_TRUE(authzSession.checkAuthorization("otherDb.collectionName", ActionType::insert));

        authzSession.logoutDatabase("test");
        ASSERT_FALSE(authzSession.checkAuthorization("test", ActionType::insert));
    }

    class AuthManagerExternalStateImplictPriv : public AuthzManagerExternalStateMock {
    public:
        AuthManagerExternalStateImplictPriv() : AuthzManagerExternalStateMock() {}

        virtual Status _findUser(const string& usersNamespace,
                                 const BSONObj& query,
                                 BSONObj* result) const {

            NamespaceString nsstring(usersNamespace);
            std::string user = query[AuthorizationManager::USER_NAME_FIELD_NAME].String();
            std::string userSource;
            if (!query[AuthorizationManager::USER_SOURCE_FIELD_NAME].trueValue()) {
                userSource = nsstring.db().toString();
            }
            else {
                userSource = query[AuthorizationManager::USER_SOURCE_FIELD_NAME].String();
            }
            *result = mapFindWithDefault(_privilegeDocs,
                                         std::make_pair(nsstring.db().toString(),
                                                        UserName(user, userSource)),
                                         BSON("invalid" << 1));
            return (*result)["invalid"].trueValue() ?
                    Status(ErrorCodes::UserNotFound, "User not found") :
                    Status::OK();
        }

        void addPrivilegeDocument(const string& dbname,
                                  const UserName& user,
                                  const BSONObj& doc) {

            ASSERT(_privilegeDocs.insert(std::make_pair(std::make_pair(dbname, user),
                                                        doc.getOwned())).second);
        }

    private:
        std::map<std::pair<std::string, UserName>, BSONObj > _privilegeDocs;
    };

    class AuthSessionExternalStateImplictPriv : public AuthzSessionExternalStateMock {
    public:
        AuthSessionExternalStateImplictPriv(AuthorizationManager* authzManager,
                                            AuthManagerExternalStateImplictPriv* managerExternal) :
            AuthzSessionExternalStateMock(authzManager),
            _authManagerExternalState(managerExternal) {}

        void addPrivilegeDocument(const string& dbname,
                                  const UserName& user,
                                  const BSONObj& doc) {
            _authManagerExternalState->addPrivilegeDocument(dbname, user, doc);
        }

    private:
        AuthManagerExternalStateImplictPriv* _authManagerExternalState;
    };

    class ImplicitPriviligesTest : public ::mongo::unittest::Test {
    public:
        AuthManagerExternalStateImplictPriv* managerState;
        AuthSessionExternalStateImplictPriv* sessionState;
        scoped_ptr<AuthorizationSession> authzSession;
        scoped_ptr<AuthorizationManager> authzManager;

        void setUp() {
            managerState = new AuthManagerExternalStateImplictPriv();
            authzManager.reset(new AuthorizationManager(managerState));
            sessionState = new AuthSessionExternalStateImplictPriv(authzManager.get(),
                                                                   managerState);
            authzSession.reset(new AuthorizationSession(sessionState));
        }
    };

    TEST_F(ImplicitPriviligesTest, ImplicitAcquireFromSomeDatabases) {
        sessionState->addPrivilegeDocument("test", UserName("andy", "test"),
                                    BSON("user" << "andy" <<
                                         "pwd" << "a" <<
                                         "roles" << BSON_ARRAY("readWrite")));
        sessionState->addPrivilegeDocument("test2", UserName("andy", "test"),
                                    BSON("user" << "andy" <<
                                         "userSource" << "test" <<
                                         "roles" <<  BSON_ARRAY("read")));
        sessionState->addPrivilegeDocument("admin", UserName("andy", "test"),
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

        Principal* principal = new Principal(UserName("andy", "test"));
        authzSession->addAndAuthorizePrincipal(principal);

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
