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
 * Unit tests of the UserSet type.
 */

#include "mongo/db/auth/user.h"
#include "mongo/db/auth/user_name.h"
#include "mongo/db/auth/user_set.h"
#include "mongo/unittest/unittest.h"

#define ASSERT_NULL(EXPR) ASSERT_FALSE((EXPR))

namespace mongo {
namespace {

    TEST(UserSetTest, BasicTest) {
        UserSet set;

        User* p1 = new User(UserName("Bob", "test"));
        User* p2 = new User(UserName("George", "test"));
        User* p3 = new User(UserName("Bob", "test2"));

        ASSERT_NULL(set.lookup(UserName("Bob", "test")));
        ASSERT_NULL(set.lookup(UserName("George", "test")));
        ASSERT_NULL(set.lookup(UserName("Bob", "test2")));
        ASSERT_NULL(set.lookupByDBName("test"));
        ASSERT_NULL(set.lookupByDBName("test2"));

        ASSERT_NULL(set.add(p1));

        ASSERT_EQUALS(p1, set.lookup(UserName("Bob", "test")));
        ASSERT_EQUALS(p1, set.lookupByDBName("test"));
        ASSERT_NULL(set.lookup(UserName("George", "test")));
        ASSERT_NULL(set.lookup(UserName("Bob", "test2")));
        ASSERT_NULL(set.lookupByDBName("test2"));

        // This should not replace the existing user "Bob" because they are different databases
        ASSERT_NULL(set.add(p3));

        ASSERT_EQUALS(p1, set.lookup(UserName("Bob", "test")));
        ASSERT_EQUALS(p1, set.lookupByDBName("test"));
        ASSERT_NULL(set.lookup(UserName("George", "test")));
        ASSERT_EQUALS(p3, set.lookup(UserName("Bob", "test2")));
        ASSERT_EQUALS(p3, set.lookupByDBName("test2"));

        User* replaced = set.add(p2); // This should replace Bob since they're on the same database

        ASSERT_EQUALS(replaced, p1);
        ASSERT_NULL(set.lookup(UserName("Bob", "test")));
        ASSERT_EQUALS(p2, set.lookup(UserName("George", "test")));
        ASSERT_EQUALS(p2, set.lookupByDBName("test"));
        ASSERT_EQUALS(p3, set.lookup(UserName("Bob", "test2")));
        ASSERT_EQUALS(p3, set.lookupByDBName("test2"));

        User* removed = set.removeByDBName("test");

        ASSERT_EQUALS(removed, p2);
        ASSERT_NULL(set.lookup(UserName("Bob", "test")));
        ASSERT_NULL(set.lookup(UserName("George", "test")));
        ASSERT_NULL(set.lookupByDBName("test"));
        ASSERT_EQUALS(p3, set.lookup(UserName("Bob", "test2")));
        ASSERT_EQUALS(p3, set.lookupByDBName("test2"));

        UserSet::NameIterator iter = set.getNames();
        ASSERT_TRUE(iter.more());
        ASSERT_EQUALS(iter.next(), UserName("Bob", "test2"));
        ASSERT_FALSE(iter.more());
    }

    TEST(UserSetTest, IterateNames) {
        UserSet pset;
        UserSet::NameIterator iter = pset.getNames();
        ASSERT(!iter.more());

        ASSERT_NULL(pset.add(new User(UserName("bob", "test"))));

        iter = pset.getNames();
        ASSERT(iter.more());
        ASSERT_EQUALS(*iter, UserName("bob", "test"));
        ASSERT_EQUALS(iter.next(), UserName("bob", "test"));
        ASSERT(!iter.more());
    }

}  // namespace
}  // namespace mongo
