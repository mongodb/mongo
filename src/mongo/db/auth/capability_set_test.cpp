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

namespace mongo {
namespace {

    TEST(CapabilitySetTest, CapabilitySet) {
        CapabilitySet capSet;
        ActionSet actions;
        Principal user1("user1");
        Principal user2("user2");

        ASSERT_OK(ActionSet::parseActionSetFromString("find,update", &actions));
        Capability fooUser("foo", &user2, actions);

        ASSERT_OK(ActionSet::parseActionSetFromString("find,update,userAdmin,remove", &actions));
        Capability fooUser2("foo", &user1, actions);

        ASSERT_OK(ActionSet::parseActionSetFromString("find,update", &actions));
        Capability barUser("bar", &user1, actions);

        ASSERT_OK(ActionSet::parseActionSetFromString("find", &actions));
        Capability barReadOnly("bar", &user2, actions);


        const Capability* capPtr;
        // No capabilities
        ASSERT(!capSet.getCapabilityForAction("foo", ActionType::find));

        capSet.grantCapability(fooUser);
        capPtr = capSet.getCapabilityForAction("foo", ActionType::find);
        ASSERT_TRUE(capPtr->includesAction(ActionType::find));
        ASSERT_FALSE(capPtr->includesAction(ActionType::remove));

        ASSERT(!capSet.getCapabilityForAction("foo", ActionType::remove));

        capSet.grantCapability(fooUser2);
        capPtr = capSet.getCapabilityForAction("foo", ActionType::userAdmin);
        ASSERT_TRUE(capPtr->includesAction(ActionType::find));
        ASSERT_TRUE(capPtr->includesAction(ActionType::remove));

        // No capabilities
        ASSERT(!capSet.getCapabilityForAction("bar", ActionType::find));

        capSet.grantCapability(barReadOnly);
        capPtr = capSet.getCapabilityForAction("bar", ActionType::find);
        ASSERT_TRUE(capPtr->includesAction(ActionType::find));
        ASSERT_FALSE(capPtr->includesAction(ActionType::update));
        ASSERT_FALSE(capPtr->includesAction(ActionType::remove));

        ASSERT(!capSet.getCapabilityForAction("bar", ActionType::update));

        capSet.grantCapability(barUser);
        capPtr = capSet.getCapabilityForAction("bar", ActionType::update);
        ASSERT_TRUE(capPtr->includesAction(ActionType::find));
        ASSERT_TRUE(capPtr->includesAction(ActionType::update));
        ASSERT_FALSE(capPtr->includesAction(ActionType::remove));

        // Now let's start revoking capabilities
        capSet.revokeCapabilitiesFromPrincipal(&user1);

        capPtr = capSet.getCapabilityForAction("foo", ActionType::find);
        ASSERT_TRUE(capPtr->includesAction(ActionType::find));
        ASSERT_FALSE(capPtr->includesAction(ActionType::remove));

        capPtr = capSet.getCapabilityForAction("bar", ActionType::find);
        ASSERT_TRUE(capPtr->includesAction(ActionType::find));
        ASSERT_FALSE(capPtr->includesAction(ActionType::update));
        ASSERT_FALSE(capPtr->includesAction(ActionType::remove));


        capSet.revokeCapabilitiesFromPrincipal(&user2);
        ASSERT(!capSet.getCapabilityForAction("foo", ActionType::find));
        ASSERT(!capSet.getCapabilityForAction("bar", ActionType::find));
    }

}  // namespace
}  // namespace mongo
