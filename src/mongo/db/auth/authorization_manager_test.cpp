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
#include "mongo/db/auth/external_state_mock.h"
#include "mongo/db/jsobj.h"
#include "mongo/unittest/unittest.h"

#define ASSERT_NULL(EXPR) ASSERT_FALSE(EXPR)
#define ASSERT_NON_NULL(EXPR) ASSERT_TRUE(EXPR)

namespace mongo {
namespace {

    TEST(AuthorizationManagerTest, AcquireCapabilityAndCheckAuthorization) {
        Principal* principal = new Principal("Spencer");
        ActionSet actions;
        actions.addAction(ActionType::insert);
        Capability writeCapability("test", principal, actions);
        Capability allDBsWriteCapability("*", principal, actions);
        ExternalStateMock* externalState = new ExternalStateMock();
        AuthorizationManager authManager(externalState);

        ASSERT_NULL(authManager.checkAuthorization("test", ActionType::insert));
        externalState->setReturnValueForShouldIgnoreAuthChecks(true);
        ASSERT_EQUALS("special",
                      authManager.checkAuthorization("test", ActionType::insert)->getName());
        externalState->setReturnValueForShouldIgnoreAuthChecks(false);
        ASSERT_NULL(authManager.checkAuthorization("test", ActionType::insert));

        ASSERT_EQUALS(ErrorCodes::UserNotFound,
                      authManager.acquireCapability(writeCapability).code());
        authManager.addAuthorizedPrincipal(principal);
        ASSERT_OK(authManager.acquireCapability(writeCapability));
        ASSERT_EQUALS(principal, authManager.checkAuthorization("test", ActionType::insert));

        ASSERT_NULL(authManager.checkAuthorization("otherDb", ActionType::insert));
        ASSERT_OK(authManager.acquireCapability(allDBsWriteCapability));
        ASSERT_EQUALS(principal, authManager.checkAuthorization("otherDb", ActionType::insert));
    }

    TEST(AuthorizationManagerTest, GrantInternalAuthorization) {
        ExternalStateMock* externalState = new ExternalStateMock();
        AuthorizationManager authManager(externalState);

        ASSERT_NULL(authManager.checkAuthorization("test", ActionType::insert));
        ASSERT_NULL(authManager.checkAuthorization("test", ActionType::replSetHeartbeat));

        authManager.grantInternalAuthorization();

        ASSERT_NON_NULL(authManager.checkAuthorization("test", ActionType::insert));
        ASSERT_NON_NULL(authManager.checkAuthorization("test", ActionType::replSetHeartbeat));
    }

    TEST(AuthorizationManagerTest, GetCapabilitiesFromPrivilegeDocument) {
        Principal* principal = new Principal("Spencer");
        BSONObj invalid;
        BSONObj readWrite = BSON("user" << "Spencer" << "pwd" << "passwordHash");
        BSONObj readOnly = BSON("user" << "Spencer" << "pwd" << "passwordHash" <<
                                "readOnly" << true);

        CapabilitySet capabilitySet;
        ASSERT_EQUALS(ErrorCodes::UnsupportedFormat,
                      AuthorizationManager::buildCapabilitySet("test",
                                                               principal,
                                                               invalid,
                                                               &capabilitySet).code());

        ASSERT_OK(AuthorizationManager::buildCapabilitySet("test",
                                                           principal,
                                                           readOnly,
                                                           &capabilitySet));
        ASSERT_NULL(capabilitySet.getCapabilityForAction("test", ActionType::insert));
        ASSERT_NON_NULL(capabilitySet.getCapabilityForAction("test", ActionType::find));

        ASSERT_OK(AuthorizationManager::buildCapabilitySet("test",
                                                           principal,
                                                           readWrite,
                                                           &capabilitySet));
        ASSERT_NON_NULL(capabilitySet.getCapabilityForAction("test", ActionType::find));
        ASSERT_NON_NULL(capabilitySet.getCapabilityForAction("test", ActionType::insert));
        ASSERT_NON_NULL(capabilitySet.getCapabilityForAction("test", ActionType::userAdmin));
        ASSERT_NON_NULL(capabilitySet.getCapabilityForAction("test", ActionType::compact));
        ASSERT_NULL(capabilitySet.getCapabilityForAction("test", ActionType::shutdown));
        ASSERT_NULL(capabilitySet.getCapabilityForAction("test", ActionType::addShard));

        ASSERT_NULL(capabilitySet.getCapabilityForAction("admin", ActionType::find));
        ASSERT_NULL(capabilitySet.getCapabilityForAction("*", ActionType::find));
        ASSERT_OK(AuthorizationManager::buildCapabilitySet("admin",
                                                           principal,
                                                           readOnly,
                                                           &capabilitySet));
        ASSERT_NON_NULL(capabilitySet.getCapabilityForAction("admin", ActionType::find));
        ASSERT_NON_NULL(capabilitySet.getCapabilityForAction("*", ActionType::find));

        ASSERT_NULL(capabilitySet.getCapabilityForAction("admin", ActionType::insert));
        ASSERT_NULL(capabilitySet.getCapabilityForAction("*", ActionType::insert));
        ASSERT_OK(AuthorizationManager::buildCapabilitySet("admin",
                                                           principal,
                                                           readWrite,
                                                           &capabilitySet));
        ASSERT_NON_NULL(capabilitySet.getCapabilityForAction("admin", ActionType::insert));
        ASSERT_NON_NULL(capabilitySet.getCapabilityForAction("*", ActionType::insert));
    }

}  // namespace
}  // namespace mongo
