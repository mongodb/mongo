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
 * Unit tests of the CapabilitySet type.
 */

#include "mongo/db/auth/action_set.h"
#include "mongo/db/auth/capability.h"
#include "mongo/db/auth/capability_set.h"
#include "mongo/unittest/unittest.h"

#define ASSERT_OK(EXPR) ASSERT_EQUALS(Status::OK(), (EXPR))

namespace mongo {
namespace {

    TEST(CapabilitySetTest, CapabilitySet) {
        CapabilitySet capSet;
        ActionSet actions;
        Principal user1("user1");
        Principal user2("user2");

        ASSERT_OK(ActionSet::parseActionSetFromString("r,w,u,d", &actions));
        Capability fooAdmin("foo", &user1, actions);

        ASSERT_OK(ActionSet::parseActionSetFromString("r,w", &actions));
        Capability barUser("bar", &user1, actions);

        ASSERT_OK(ActionSet::parseActionSetFromString("r,w", &actions));
        Capability fooUser("foo", &user2, actions);

        ASSERT_OK(ActionSet::parseActionSetFromString("r", &actions));
        Capability barReadOnly("bar", &user2, actions);


        const Capability* capPtr;
        // No capabilities
        ASSERT(!capSet.getCapabilityForAction("foo", ActionType::READ));

        capSet.grantCapability(fooUser);
        capPtr = capSet.getCapabilityForAction("foo", ActionType::READ);
        ASSERT_TRUE(capPtr->includesAction(ActionType::READ));
        ASSERT_FALSE(capPtr->includesAction(ActionType::DB_ADMIN));

        ASSERT(!capSet.getCapabilityForAction("foo", ActionType::DB_ADMIN));

        capSet.grantCapability(fooAdmin);
        capPtr = capSet.getCapabilityForAction("foo", ActionType::USER_ADMIN);
        ASSERT_TRUE(capPtr->includesAction(ActionType::READ));
        ASSERT_TRUE(capPtr->includesAction(ActionType::DB_ADMIN));

        // No capabilities
        ASSERT(!capSet.getCapabilityForAction("bar", ActionType::READ));

        capSet.grantCapability(barReadOnly);
        capPtr = capSet.getCapabilityForAction("bar", ActionType::READ);
        ASSERT_TRUE(capPtr->includesAction(ActionType::READ));
        ASSERT_FALSE(capPtr->includesAction(ActionType::READ_WRITE));
        ASSERT_FALSE(capPtr->includesAction(ActionType::DB_ADMIN));

        ASSERT(!capSet.getCapabilityForAction("bar", ActionType::READ_WRITE));

        capSet.grantCapability(barUser);
        capPtr = capSet.getCapabilityForAction("bar", ActionType::READ_WRITE);
        ASSERT_TRUE(capPtr->includesAction(ActionType::READ));
        ASSERT_TRUE(capPtr->includesAction(ActionType::READ_WRITE));
        ASSERT_FALSE(capPtr->includesAction(ActionType::DB_ADMIN));

        // Now let's start revoking capabilities
        capSet.revokeCapabilitiesFromPrincipal(&user1);

        capPtr = capSet.getCapabilityForAction("foo", ActionType::READ);
        ASSERT_TRUE(capPtr->includesAction(ActionType::READ));
        ASSERT_FALSE(capPtr->includesAction(ActionType::DB_ADMIN));

        capPtr = capSet.getCapabilityForAction("bar", ActionType::READ);
        ASSERT_TRUE(capPtr->includesAction(ActionType::READ));
        ASSERT_FALSE(capPtr->includesAction(ActionType::READ_WRITE));
        ASSERT_FALSE(capPtr->includesAction(ActionType::DB_ADMIN));


        capSet.revokeCapabilitiesFromPrincipal(&user2);
        ASSERT(!capSet.getCapabilityForAction("foo", ActionType::READ));
        ASSERT(!capSet.getCapabilityForAction("bar", ActionType::READ));
    }

}  // namespace
}  // namespace mongo
