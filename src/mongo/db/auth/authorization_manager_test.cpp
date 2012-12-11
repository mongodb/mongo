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
 * Unit tests of the AuthorizationManager type.
 */

#include "mongo/db/auth/authorization_manager.h"

#include "mongo/base/status.h"
#include "mongo/db/auth/auth_external_state_mock.h"
#include "mongo/db/jsobj.h"
#include "mongo/unittest/unittest.h"

#define ASSERT_NULL(EXPR) ASSERT_FALSE(EXPR)
#define ASSERT_NON_NULL(EXPR) ASSERT_TRUE(EXPR)

namespace mongo {
namespace {

    TEST(AuthorizationManagerTest, AcquirePrivilegeAndCheckAuthorization) {
        Principal* principal = new Principal("Spencer", "test");
        ActionSet actions;
        actions.addAction(ActionType::insert);
        AcquiredPrivilege writePrivilege(Privilege("test", actions), principal);
        AcquiredPrivilege allDBsWritePrivilege(Privilege("*", actions), principal);
        AuthExternalStateMock* externalState = new AuthExternalStateMock();
        AuthorizationManager authManager(externalState);

        ASSERT_FALSE(authManager.checkAuthorization("test", ActionType::insert));
        externalState->setReturnValueForShouldIgnoreAuthChecks(true);
        ASSERT_TRUE(authManager.checkAuthorization("test", ActionType::insert));
        externalState->setReturnValueForShouldIgnoreAuthChecks(false);
        ASSERT_FALSE(authManager.checkAuthorization("test", ActionType::insert));

        ASSERT_EQUALS(ErrorCodes::UserNotFound,
                      authManager.acquirePrivilege(writePrivilege).code());
        authManager.addAuthorizedPrincipal(principal);
        ASSERT_OK(authManager.acquirePrivilege(writePrivilege));
        ASSERT_TRUE(authManager.checkAuthorization("test", ActionType::insert));

        ASSERT_FALSE(authManager.checkAuthorization("otherDb", ActionType::insert));
        ASSERT_OK(authManager.acquirePrivilege(allDBsWritePrivilege));
        ASSERT_TRUE(authManager.checkAuthorization("otherDb", ActionType::insert));
        // Auth checks on a collection should be applied to the database name.
        ASSERT_TRUE(authManager.checkAuthorization("otherDb.collectionName", ActionType::insert));

        authManager.logoutDatabase("test");
        ASSERT_FALSE(authManager.checkAuthorization("test", ActionType::insert));
    }

    TEST(AuthorizationManagerTest, GetPrivilegesFromPrivilegeDocument) {
        Principal* principal = new Principal("Spencer", "test");
        BSONObj invalid;
        BSONObj readWrite = BSON("user" << "Spencer" << "pwd" << "passwordHash");
        BSONObj readOnly = BSON("user" << "Spencer" << "pwd" << "passwordHash" <<
                                "readOnly" << true);

        PrivilegeSet privilegeSet;
        ASSERT_EQUALS(ErrorCodes::UnsupportedFormat,
                      AuthorizationManager::buildPrivilegeSet("test",
                                                               principal,
                                                               invalid,
                                                               &privilegeSet).code());

        ASSERT_OK(AuthorizationManager::buildPrivilegeSet("test",
                                                           principal,
                                                           readOnly,
                                                           &privilegeSet));
        ASSERT_NULL(privilegeSet.getPrivilegeForAction("test", ActionType::insert));
        ASSERT_NON_NULL(privilegeSet.getPrivilegeForAction("test", ActionType::find));

        ASSERT_OK(AuthorizationManager::buildPrivilegeSet("test",
                                                           principal,
                                                           readWrite,
                                                           &privilegeSet));
        ASSERT_NON_NULL(privilegeSet.getPrivilegeForAction("test", ActionType::find));
        ASSERT_NON_NULL(privilegeSet.getPrivilegeForAction("test", ActionType::insert));
        ASSERT_NON_NULL(privilegeSet.getPrivilegeForAction("test", ActionType::userAdmin));
        ASSERT_NON_NULL(privilegeSet.getPrivilegeForAction("test", ActionType::compact));
        ASSERT_NULL(privilegeSet.getPrivilegeForAction("test", ActionType::shutdown));
        ASSERT_NULL(privilegeSet.getPrivilegeForAction("test", ActionType::addShard));

        ASSERT_NULL(privilegeSet.getPrivilegeForAction("admin", ActionType::find));
        ASSERT_NULL(privilegeSet.getPrivilegeForAction("*", ActionType::find));
        ASSERT_OK(AuthorizationManager::buildPrivilegeSet("admin",
                                                           principal,
                                                           readOnly,
                                                           &privilegeSet));
        // Should grant privileges on *, not on admin DB directly
        ASSERT_NULL(privilegeSet.getPrivilegeForAction("admin", ActionType::find));
        ASSERT_NON_NULL(privilegeSet.getPrivilegeForAction("*", ActionType::find));

        ASSERT_NULL(privilegeSet.getPrivilegeForAction("admin", ActionType::insert));
        ASSERT_NULL(privilegeSet.getPrivilegeForAction("*", ActionType::insert));
        ASSERT_OK(AuthorizationManager::buildPrivilegeSet("admin",
                                                           principal,
                                                           readWrite,
                                                           &privilegeSet));
        ASSERT_NULL(privilegeSet.getPrivilegeForAction("admin", ActionType::insert));
        ASSERT_NON_NULL(privilegeSet.getPrivilegeForAction("*", ActionType::insert));
    }

}  // namespace
}  // namespace mongo
