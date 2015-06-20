/*    Copyright 2012 10gen Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
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

    const std::unique_ptr<User> delp1(p1);
    const std::unique_ptr<User> delp2(p2);
    const std::unique_ptr<User> delp3(p3);

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

    User* replaced = set.add(p2);  // This should replace Bob since they're on the same database

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

    UserNameIterator iter = set.getNames();
    ASSERT_TRUE(iter.more());
    ASSERT_EQUALS(iter.next(), UserName("Bob", "test2"));
    ASSERT_FALSE(iter.more());
}

TEST(UserSetTest, IterateNames) {
    UserSet pset;
    UserNameIterator iter = pset.getNames();
    ASSERT(!iter.more());

    std::unique_ptr<User> user(new User(UserName("bob", "test")));
    ASSERT_NULL(pset.add(user.get()));

    iter = pset.getNames();
    ASSERT(iter.more());
    ASSERT_EQUALS(*iter, UserName("bob", "test"));
    ASSERT_EQUALS(iter.next(), UserName("bob", "test"));
    ASSERT(!iter.more());
}

}  // namespace
}  // namespace mongo
