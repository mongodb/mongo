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
 * Unit tests of the PrincipalSet type.
 */

#include "mongo/db/auth/principal_set.h"
#include "mongo/db/auth/principal.h"
#include "mongo/db/auth/principal_name.h"
#include "mongo/unittest/unittest.h"

#define ASSERT_NULL(EXPR) ASSERT_FALSE((EXPR))

namespace mongo {
namespace {

    TEST(PrincipalSetTest, BasicTest) {
        PrincipalSet set;

        Principal* p1 = new Principal(PrincipalName("Bob", "test"));
        Principal* p2 = new Principal(PrincipalName("George", "test"));
        Principal* p3 = new Principal(PrincipalName("Bob", "test2"));

        ASSERT_NULL(set.lookup(PrincipalName("Bob", "test")));
        ASSERT_NULL(set.lookup(PrincipalName("George", "test")));
        ASSERT_NULL(set.lookup(PrincipalName("Bob", "test2")));
        ASSERT_NULL(set.lookupByDBName("test"));
        ASSERT_NULL(set.lookupByDBName("test2"));

        set.add(p1);

        ASSERT_EQUALS(p1, set.lookup(PrincipalName("Bob", "test")));
        ASSERT_EQUALS(p1, set.lookupByDBName("test"));
        ASSERT_NULL(set.lookup(PrincipalName("George", "test")));
        ASSERT_NULL(set.lookup(PrincipalName("Bob", "test2")));
        ASSERT_NULL(set.lookupByDBName("test2"));

        // This should not replace the existing user "Bob" because they are different databases
        set.add(p3);

        ASSERT_EQUALS(p1, set.lookup(PrincipalName("Bob", "test")));
        ASSERT_EQUALS(p1, set.lookupByDBName("test"));
        ASSERT_NULL(set.lookup(PrincipalName("George", "test")));
        ASSERT_EQUALS(p3, set.lookup(PrincipalName("Bob", "test2")));
        ASSERT_EQUALS(p3, set.lookupByDBName("test2"));

        set.add(p2); // This should replace Bob since they're on the same database

        ASSERT_NULL(set.lookup(PrincipalName("Bob", "test")));
        ASSERT_EQUALS(p2, set.lookup(PrincipalName("George", "test")));
        ASSERT_EQUALS(p2, set.lookupByDBName("test"));
        ASSERT_EQUALS(p3, set.lookup(PrincipalName("Bob", "test2")));
        ASSERT_EQUALS(p3, set.lookupByDBName("test2"));

        set.removeByDBName("test");

        ASSERT_NULL(set.lookup(PrincipalName("Bob", "test")));
        ASSERT_NULL(set.lookup(PrincipalName("George", "test")));
        ASSERT_NULL(set.lookupByDBName("test"));
        ASSERT_EQUALS(p3, set.lookup(PrincipalName("Bob", "test2")));
        ASSERT_EQUALS(p3, set.lookupByDBName("test2"));
    }

}  // namespace
}  // namespace mongo
