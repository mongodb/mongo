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
#include "mongo/unittest/unittest.h"

#define ASSERT_OK(EXPR) ASSERT_EQUALS(Status::OK(), (EXPR))
#define ASSERT_NULL(EXPR) ASSERT_FALSE((EXPR))

namespace mongo {
namespace {

    TEST(PrincipalSetTest, BasicTest) {
        PrincipalSet set;

        Principal* p1 = new Principal("Bob");
        Principal* p2 = new Principal("George");

        ASSERT_NULL(set.lookup("Bob"));
        ASSERT_NULL(set.lookup("George"));

        set.add(p1);

        ASSERT_EQUALS("Bob", set.lookup("Bob")->getName());
        ASSERT_NULL(set.lookup("George"));

        set.add(p2);

        ASSERT_EQUALS("Bob", set.lookup("Bob")->getName());
        ASSERT_EQUALS("George", set.lookup("George")->getName());

        set.removeByName("Bob");

        ASSERT_NULL(set.lookup("Bob"));
        ASSERT_EQUALS("George", set.lookup("George")->getName());

        set.removeByName("George");

        ASSERT_NULL(set.lookup("Bob"));
        ASSERT_NULL(set.lookup("George"));
    }

}  // namespace
}  // namespace mongo
