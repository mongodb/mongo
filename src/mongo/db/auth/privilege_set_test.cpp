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

    // Convenience methods for outputing PrincipalName and construction ActionSets that make tests
    // concise, but that we're reluctant to put into the types themselves.

    std::ostream& operator<<(std::ostream& os, const PrincipalName& pname) {
        return os << pname.toString();
    }

    std::ostream& operator<<(std::ostream&os, const std::vector<PrincipalName>& ps) {
        os << "[ ";
        for (size_t i = 0; i < ps.size(); ++i)
            os << ps[i] << ' ';
        os << ']';
        return os;
    }

    ActionSet operator|(const ActionSet& lhs, const ActionSet& rhs) {
        ActionSet result = lhs;
        result.addAllActionsFromSet(rhs);
        return result;
    }

    ActionSet operator|(const ActionSet& lhs, const ActionType& rhs) {
        ActionSet result = lhs;
        result.addAction(rhs);
        return result;
    }

    ActionSet operator|(const ActionType& lhs, const ActionType& rhs) {
        ActionSet result;
        result.addAction(lhs);
        result.addAction(rhs);
        return result;
    }

    // Tests

    TEST(PrivilegeSetTest, PrivilegeSet) {
        PrivilegeSet capSet;
        PrincipalName user1("user1", "test");
        PrincipalName user2("user2", "test2");

        // Initially, the capability set contains no privileges at all.
        ASSERT_FALSE(capSet.hasPrivilege(Privilege("foo", ActionType::find)));
        ASSERT_FALSE(capSet.hasPrivilege(Privilege("bar", ActionType::find)));

        // Grant find and update to "foo", only.
        capSet.grantPrivilege(Privilege("foo", ActionType::find|ActionType::update), user1);

        ASSERT_TRUE(capSet.hasPrivilege(Privilege("foo", ActionType::find)));
        ASSERT_TRUE(capSet.hasPrivilege(Privilege("foo", ActionType::find|ActionType::update)));
        ASSERT_FALSE(capSet.hasPrivilege(Privilege("foo", ActionType::find|ActionType::remove)));
        ASSERT_FALSE(capSet.hasPrivilege(Privilege("bar", ActionType::find)));
        ASSERT_FALSE(capSet.hasPrivilege(Privilege("foo", ActionType::remove)));

        // Grant "userAdmin", "update" and "remove" on "foo" to user2, which changes the set of
        // actions this privilege set will approve.
        capSet.grantPrivilege(
                Privilege("foo", ActionType::userAdmin|ActionType::update|ActionType::remove),
                user2);

        ASSERT_TRUE(capSet.hasPrivilege(Privilege("foo", ActionType::userAdmin)));
        ASSERT_TRUE(capSet.hasPrivilege(Privilege("foo", ActionType::update)));
        ASSERT_TRUE(capSet.hasPrivilege(Privilege("foo", ActionType::userAdmin)));
        ASSERT_TRUE(capSet.hasPrivilege(Privilege("foo", ActionType::find|ActionType::remove)));

        // Revoke user2's privileges.
        capSet.revokePrivilegesFromPrincipal(user2);

        ASSERT_FALSE(capSet.hasPrivilege(Privilege("foo", ActionType::userAdmin)));
        ASSERT_FALSE(capSet.hasPrivilege(Privilege("foo", ActionType::find|ActionType::remove)));
        ASSERT_TRUE(capSet.hasPrivilege(Privilege("foo", ActionType::update)));

        // Revoke user2's privileges again; should be a no-op.
        capSet.revokePrivilegesFromPrincipal(user2);

        ASSERT_FALSE(capSet.hasPrivilege(Privilege("foo", ActionType::userAdmin)));
        ASSERT_FALSE(capSet.hasPrivilege(Privilege("foo", ActionType::find|ActionType::remove)));
        ASSERT_TRUE(capSet.hasPrivilege(Privilege("foo", ActionType::update)));

        // Re-grant "userAdmin", "update" and "remove" on "foo" to user2.
        capSet.grantPrivilege(
                Privilege("foo", ActionType::userAdmin|ActionType::update|ActionType::remove),
                user2);

        // The set still contains no capabilities on "bar".
        ASSERT_FALSE(capSet.hasPrivilege(Privilege("bar", ActionType::find)));

        // Let user2 "find" on "bar".
        capSet.grantPrivilege(Privilege("bar", ActionType::find), user2);

        ASSERT_TRUE(capSet.hasPrivilege(Privilege("bar", ActionType::find)));
        ASSERT_FALSE(capSet.hasPrivilege(Privilege("bar", ActionType::update)));
        ASSERT_FALSE(capSet.hasPrivilege(Privilege("bar", ActionType::remove)));

        // Let user1 "find" and "update" on "bar".
        capSet.grantPrivilege(Privilege("bar", ActionType::update|ActionType::find), user1);

        ASSERT_TRUE(capSet.hasPrivilege(Privilege("bar", ActionType::find|ActionType::update)));
        ASSERT_FALSE(capSet.hasPrivilege(
                             Privilege("bar",
                                       ActionType::find|ActionType::update|ActionType::remove)));

        // Revoke user1's privileges.
        capSet.revokePrivilegesFromPrincipal(user1);

        ASSERT_TRUE(capSet.hasPrivilege(Privilege("foo", ActionType::update)));
        ASSERT_FALSE(capSet.hasPrivilege(Privilege("foo", ActionType::find)));
        ASSERT_TRUE(capSet.hasPrivilege(Privilege("bar", ActionType::find)));
        ASSERT_FALSE(capSet.hasPrivilege(Privilege("bar", ActionType::update)));

        // Revoke user2's privileges.
        capSet.revokePrivilegesFromPrincipal(user2);

        ASSERT_FALSE(capSet.hasPrivilege(Privilege("foo", ActionType::update)));
        ASSERT_FALSE(capSet.hasPrivilege(Privilege("bar", ActionType::find)));
    }

    TEST(PrivilegeSetTest, WildcardPrivileges) {
        // Tests acquisition and revocation of privileges on WILDCARD_RESOURCE.

        PrivilegeSet privSet;

        PrincipalName user("user", "db");
        Privilege wildcardFind("*", ActionType::find);
        Privilege wildcardUpdate("*", ActionType::update);
        Privilege wildcardFindAndUpdate("*", ActionType::find|ActionType::update);
        Privilege fooFind("foo", ActionType::find);
        Privilege fooUpdate("foo", ActionType::update);
        Privilege fooFindAndUpdate("foo", ActionType::find|ActionType::update);
        Privilege barFind("bar", ActionType::find);
        Privilege barUpdate("bar", ActionType::update);
        Privilege barFindAndUpdate("bar", ActionType::find|ActionType::update);

        // With no granted privileges, assert that hasPrivilege returns false.
        ASSERT_FALSE(privSet.hasPrivilege(wildcardFind));
        ASSERT_FALSE(privSet.hasPrivilege(wildcardUpdate));
        ASSERT_FALSE(privSet.hasPrivilege(wildcardFindAndUpdate));

        ASSERT_FALSE(privSet.hasPrivilege(fooFind));
        ASSERT_FALSE(privSet.hasPrivilege(fooUpdate));
        ASSERT_FALSE(privSet.hasPrivilege(fooFindAndUpdate));

        ASSERT_FALSE(privSet.hasPrivilege(barFind));
        ASSERT_FALSE(privSet.hasPrivilege(barUpdate));
        ASSERT_FALSE(privSet.hasPrivilege(barFindAndUpdate));

        // Grant some privileges, and ensure that exactly those privileges are granted.
        std::vector<Privilege> grantedPrivileges;
        grantedPrivileges.push_back(wildcardFind);
        grantedPrivileges.push_back(fooUpdate);

        privSet.grantPrivileges(grantedPrivileges, user);

        ASSERT_TRUE(privSet.hasPrivilege(wildcardFind));
        ASSERT_FALSE(privSet.hasPrivilege(wildcardUpdate));
        ASSERT_FALSE(privSet.hasPrivilege(wildcardFindAndUpdate));

        ASSERT_TRUE(privSet.hasPrivilege(fooFind));
        ASSERT_TRUE(privSet.hasPrivilege(fooUpdate));
        ASSERT_TRUE(privSet.hasPrivilege(fooFindAndUpdate));

        ASSERT_TRUE(privSet.hasPrivilege(barFind));
        ASSERT_FALSE(privSet.hasPrivilege(barUpdate));
        ASSERT_FALSE(privSet.hasPrivilege(barFindAndUpdate));

        // Revoke the granted privileges, and assert that hasPrivilege returns false.
        privSet.revokePrivilegesFromPrincipal(user);

        ASSERT_FALSE(privSet.hasPrivilege(wildcardFind));
        ASSERT_FALSE(privSet.hasPrivilege(wildcardUpdate));
        ASSERT_FALSE(privSet.hasPrivilege(wildcardFindAndUpdate));

        ASSERT_FALSE(privSet.hasPrivilege(fooFind));
        ASSERT_FALSE(privSet.hasPrivilege(fooUpdate));
        ASSERT_FALSE(privSet.hasPrivilege(fooFindAndUpdate));

        ASSERT_FALSE(privSet.hasPrivilege(barFind));
        ASSERT_FALSE(privSet.hasPrivilege(barUpdate));
        ASSERT_FALSE(privSet.hasPrivilege(barFindAndUpdate));
    }

}  // namespace
}  // namespace mongo
