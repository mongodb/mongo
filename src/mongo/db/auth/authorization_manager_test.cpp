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

#define ASSERT_OK(EXPR) ASSERT_EQUALS(Status::OK(), (EXPR))
#define ASSERT_NULL(EXPR) ASSERT_FALSE(EXPR)
#define ASSERT_NON_NULL(EXPR) ASSERT_TRUE(EXPR)

namespace mongo {
namespace {

    TEST(AuthorizationManagerTest, AcquireCapabilityAndCheckAuthorization) {
        Principal* principal = new Principal("Spencer");
        ActionSet actions;
        actions.addAction(ActionType::READ_WRITE);
        Capability writeCapability("test", principal, actions);
        Capability allDBsWriteCapability("*", principal, actions);
        ExternalStateMock* externalState = new ExternalStateMock();
        AuthorizationManager authManager(externalState);

        ASSERT_NULL(authManager.checkAuthorization("test", ActionType::READ_WRITE));
        externalState->setReturnValueForShouldIgnoreAuthChecks(true);
        ASSERT_EQUALS("special",
                      authManager.checkAuthorization("test", ActionType::READ_WRITE)->getName());
        externalState->setReturnValueForShouldIgnoreAuthChecks(false);
        ASSERT_NULL(authManager.checkAuthorization("test", ActionType::READ_WRITE));

        ASSERT_EQUALS(ErrorCodes::UserNotFound,
                      authManager.acquireCapability(writeCapability).code());
        authManager.addAuthorizedPrincipal(principal);
        ASSERT_OK(authManager.acquireCapability(writeCapability));
        ASSERT_EQUALS(principal, authManager.checkAuthorization("test", ActionType::READ_WRITE));

        ASSERT_NULL(authManager.checkAuthorization("otherDb", ActionType::READ_WRITE));
        ASSERT_OK(authManager.acquireCapability(allDBsWriteCapability));
        ASSERT_EQUALS(principal, authManager.checkAuthorization("otherDb", ActionType::READ_WRITE));
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
        ASSERT_NULL(capabilitySet.getCapabilityForAction("test", ActionType::READ_WRITE));
        ASSERT_NON_NULL(capabilitySet.getCapabilityForAction("test", ActionType::READ));

        ASSERT_OK(AuthorizationManager::buildCapabilitySet("test",
                                                           principal,
                                                           readWrite,
                                                           &capabilitySet));
        ASSERT_NON_NULL(capabilitySet.getCapabilityForAction("test", ActionType::READ));
        ASSERT_NON_NULL(capabilitySet.getCapabilityForAction("test", ActionType::READ_WRITE));
        ASSERT_NON_NULL(capabilitySet.getCapabilityForAction("test", ActionType::USER_ADMIN));
        ASSERT_NON_NULL(capabilitySet.getCapabilityForAction("test", ActionType::DB_ADMIN));
        ASSERT_NULL(capabilitySet.getCapabilityForAction("test", ActionType::SERVER_ADMIN));
        ASSERT_NULL(capabilitySet.getCapabilityForAction("test", ActionType::CLUSTER_ADMIN));

        ASSERT_NULL(capabilitySet.getCapabilityForAction("admin", ActionType::READ));
        ASSERT_NULL(capabilitySet.getCapabilityForAction("*", ActionType::READ));
        ASSERT_OK(AuthorizationManager::buildCapabilitySet("admin",
                                                           principal,
                                                           readOnly,
                                                           &capabilitySet));
        ASSERT_NON_NULL(capabilitySet.getCapabilityForAction("admin", ActionType::READ));
        ASSERT_NON_NULL(capabilitySet.getCapabilityForAction("*", ActionType::READ));

        ASSERT_NULL(capabilitySet.getCapabilityForAction("admin", ActionType::READ_WRITE));
        ASSERT_NULL(capabilitySet.getCapabilityForAction("*", ActionType::READ_WRITE));
        ASSERT_OK(AuthorizationManager::buildCapabilitySet("admin",
                                                           principal,
                                                           readWrite,
                                                           &capabilitySet));
        ASSERT_NON_NULL(capabilitySet.getCapabilityForAction("admin", ActionType::READ_WRITE));
        ASSERT_NON_NULL(capabilitySet.getCapabilityForAction("*", ActionType::READ_WRITE));
    }

}  // namespace
}  // namespace mongo
