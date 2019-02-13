/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
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

    UserHandle p1 = std::make_shared<User>(UserName("Bob", "test"));
    UserHandle p2 = std::make_shared<User>(UserName("George", "test"));
    UserHandle p3 = std::make_shared<User>(UserName("Bob", "test2"));

    ASSERT_NULL(set.lookup(UserName("Bob", "test")));
    ASSERT_NULL(set.lookup(UserName("George", "test")));
    ASSERT_NULL(set.lookup(UserName("Bob", "test2")));
    ASSERT_NULL(set.lookupByDBName("test"));
    ASSERT_NULL(set.lookupByDBName("test2"));

    set.add(p1);

    ASSERT_EQUALS(p1, set.lookup(UserName("Bob", "test")));
    ASSERT_EQUALS(p1, set.lookupByDBName("test"));
    ASSERT_NULL(set.lookup(UserName("George", "test")));
    ASSERT_NULL(set.lookup(UserName("Bob", "test2")));
    ASSERT_NULL(set.lookupByDBName("test2"));

    // This should not replace the existing user "Bob" because they are different databases
    set.add(p3);

    ASSERT_EQUALS(p1, set.lookup(UserName("Bob", "test")));
    ASSERT_EQUALS(p1, set.lookupByDBName("test"));
    ASSERT_NULL(set.lookup(UserName("George", "test")));
    ASSERT_EQUALS(p3, set.lookup(UserName("Bob", "test2")));
    ASSERT_EQUALS(p3, set.lookupByDBName("test2"));

    set.add(p2);  // This should replace Bob since they're on the same database

    ASSERT_NULL(set.lookup(UserName("Bob", "test")));
    ASSERT_EQUALS(p2, set.lookup(UserName("George", "test")));
    ASSERT_EQUALS(p2, set.lookupByDBName("test"));
    ASSERT_EQUALS(p3, set.lookup(UserName("Bob", "test2")));
    ASSERT_EQUALS(p3, set.lookupByDBName("test2"));

    set.removeByDBName("test"_sd);

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

    UserHandle user = std::make_shared<User>(UserName("bob", "test"));
    pset.add(std::move(user));

    iter = pset.getNames();
    ASSERT(iter.more());
    ASSERT_EQUALS(*iter, UserName("bob", "test"));
    ASSERT_EQUALS(iter.next(), UserName("bob", "test"));
    ASSERT(!iter.more());
}

}  // namespace
}  // namespace mongo
