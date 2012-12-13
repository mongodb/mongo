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
 * Unit tests of the PrivilegeSet type.
 */

#include "mongo/db/auth/acquired_privilege.h"
#include "mongo/db/auth/action_set.h"
#include "mongo/db/auth/privilege_set.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

    TEST(PrivilegeSetTest, PrivilegeSet) {
        PrivilegeSet capSet;
        ActionSet actions;
        Principal user1(PrincipalName("user1", "test"));
        Principal user2(PrincipalName("user2", "test2"));

        ASSERT_OK(ActionSet::parseActionSetFromString("find,update", &actions));
        AcquiredPrivilege fooUser(Privilege("foo", actions), &user2);

        ASSERT_OK(ActionSet::parseActionSetFromString("find,update,userAdmin,remove", &actions));
        AcquiredPrivilege fooUser2(Privilege("foo", actions), &user1);

        ASSERT_OK(ActionSet::parseActionSetFromString("find,update", &actions));
        AcquiredPrivilege barUser(Privilege("bar", actions), &user1);

        ASSERT_OK(ActionSet::parseActionSetFromString("find", &actions));
        AcquiredPrivilege barReadOnly(Privilege("bar", actions), &user2);


        const AcquiredPrivilege* capPtr;
        // No capabilities
        ASSERT(!capSet.getPrivilegeForAction("foo", ActionType::find));

        capSet.grantPrivilege(fooUser);
        capPtr = capSet.getPrivilegeForAction("foo", ActionType::find);
        ASSERT_TRUE(capPtr->getPrivilege().includesAction(ActionType::find));
        ASSERT_FALSE(capPtr->getPrivilege().includesAction(ActionType::remove));

        ASSERT(!capSet.getPrivilegeForAction("foo", ActionType::remove));

        capSet.grantPrivilege(fooUser2);
        capPtr = capSet.getPrivilegeForAction("foo", ActionType::userAdmin);
        ASSERT_TRUE(capPtr->getPrivilege().includesAction(ActionType::find));
        ASSERT_TRUE(capPtr->getPrivilege().includesAction(ActionType::remove));

        // No capabilities
        ASSERT(!capSet.getPrivilegeForAction("bar", ActionType::find));

        capSet.grantPrivilege(barReadOnly);
        capPtr = capSet.getPrivilegeForAction("bar", ActionType::find);
        ASSERT_TRUE(capPtr->getPrivilege().includesAction(ActionType::find));
        ASSERT_FALSE(capPtr->getPrivilege().includesAction(ActionType::update));
        ASSERT_FALSE(capPtr->getPrivilege().includesAction(ActionType::remove));

        ASSERT(!capSet.getPrivilegeForAction("bar", ActionType::update));

        capSet.grantPrivilege(barUser);
        capPtr = capSet.getPrivilegeForAction("bar", ActionType::update);
        ASSERT_TRUE(capPtr->getPrivilege().includesAction(ActionType::find));
        ASSERT_TRUE(capPtr->getPrivilege().includesAction(ActionType::update));
        ASSERT_FALSE(capPtr->getPrivilege().includesAction(ActionType::remove));

        // Now let's start revoking capabilities
        capSet.revokePrivilegesFromPrincipal(&user1);

        capPtr = capSet.getPrivilegeForAction("foo", ActionType::find);
        ASSERT_TRUE(capPtr->getPrivilege().includesAction(ActionType::find));
        ASSERT_FALSE(capPtr->getPrivilege().includesAction(ActionType::remove));

        capPtr = capSet.getPrivilegeForAction("bar", ActionType::find);
        ASSERT_TRUE(capPtr->getPrivilege().includesAction(ActionType::find));
        ASSERT_FALSE(capPtr->getPrivilege().includesAction(ActionType::update));
        ASSERT_FALSE(capPtr->getPrivilege().includesAction(ActionType::remove));


        capSet.revokePrivilegesFromPrincipal(&user2);
        ASSERT(!capSet.getPrivilegeForAction("foo", ActionType::find));
        ASSERT(!capSet.getPrivilegeForAction("bar", ActionType::find));
    }

}  // namespace
}  // namespace mongo
