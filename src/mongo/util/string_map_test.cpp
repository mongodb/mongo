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


#include "mongo/util/string_map.h"

#include "mongo/stdx/type_traits.h"
#include "mongo/unittest/unittest.h"

#include <cstdio>
#include <utility>

#include <absl/container/node_hash_map.h>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest


namespace {
using namespace mongo;

TEST(StringMapTest, Hash1) {
    StringSet::hasher hash;
    ASSERT_EQUALS(hash(""), hash(""));
    ASSERT_EQUALS(hash("a"), hash("a"));
    ASSERT_EQUALS(hash("abc"), hash("abc"));

    ASSERT_NOT_EQUALS(hash(""), hash("a"));
    ASSERT_NOT_EQUALS(hash("a"), hash("ab"));

    ASSERT_NOT_EQUALS(hash("foo28"), hash("foo35"));
}

#define equalsBothWays(a, b)       \
    ASSERT_TRUE(equals((a), (b))); \
    ASSERT_TRUE(equals((b), (a)));

#define notEqualsBothWays(a, b)     \
    ASSERT_FALSE(equals((a), (b))); \
    ASSERT_FALSE(equals((b), (a)));

TEST(StringMapTest, Equals1) {
    StringSet::key_equal equals;

    equalsBothWays("", "");
    equalsBothWays("a", "a");
    equalsBothWays("bbbbb", "bbbbb");

    notEqualsBothWays("", "a");
    notEqualsBothWays("a", "b");
    notEqualsBothWays("abc", "def");
    notEqualsBothWays("abc", "defasdasd");
}

TEST(StringMapTest, Basic1) {
    StringMap<int> m;
    ASSERT_EQUALS(0U, m.size());
    ASSERT_EQUALS(true, m.empty());
    m["eliot"] = 5;
    ASSERT_EQUALS(5, m["eliot"]);
    ASSERT_EQUALS(1U, m.size());
    ASSERT_EQUALS(false, m.empty());
}

TEST(StringMapTest, Big1) {
    StringMap<int> m;
    char buf[64];

    for (int i = 0; i < 10000; i++) {
        sprintf(buf, "foo%d", i);
        m[buf] = i;
    }

    for (int i = 0; i < 10000; i++) {
        sprintf(buf, "foo%d", i);
        ASSERT_EQUALS(m[buf], i);
    }
}

TEST(StringMapTest, find1) {
    StringMap<int> m;

    ASSERT_EQ(m.count("foo"), 0u);
    ASSERT(m.find("foo") == m.end());

    m["foo"] = 5;
    ASSERT_EQ(m.count("foo"), 1u);
    StringMap<int>::const_iterator i = m.find("foo");
    ASSERT(i != m.end());
    ASSERT_EQ(i->second, 5);
    ASSERT_EQ(i->first, "foo");
    ++i;
    ASSERT(i == m.end());

    ASSERT_EQ(m.count("bar"), 0u);
    ASSERT(m.find("bar") == m.end());
}


TEST(StringMapTest, Erase1) {
    StringMap<int> m;
    char buf[64];

    m["eliot"] = 5;
    ASSERT_EQUALS(5, m["eliot"]);
    ASSERT_EQUALS(1U, m.size());
    ASSERT_EQUALS(false, m.empty());
    ASSERT_EQUALS(1U, m.erase("eliot"));
    ASSERT(m.end() == m.find("eliot"));
    ASSERT_EQUALS(0U, m.size());
    ASSERT_EQUALS(true, m.empty());
    ASSERT_EQUALS(0, m["eliot"]);
    ASSERT_EQUALS(1U, m.size());
    ASSERT_EQUALS(false, m.empty());
    ASSERT_EQUALS(1U, m.erase("eliot"));
    ASSERT(m.end() == m.find("eliot"));
    ASSERT_EQUALS(0U, m.erase("eliot"));

    size_t before = m.capacity();
    for (int i = 0; i < 10000; i++) {
        sprintf(buf, "foo%d", i);
        m[buf] = i;
        ASSERT_EQUALS(i, m[buf]);
        ASSERT_EQUALS(1U, m.erase(buf));
        ASSERT(m.end() == m.find(buf));
    }
    ASSERT_EQUALS(before, m.capacity());
}

TEST(StringMapTest, Erase2) {
    StringMap<int> m;
    m["eliot"] = 5;
    ASSERT_EQUALS(1U, m.size());
    ASSERT_EQUALS(false, m.empty());
    StringMap<int>::const_iterator i = m.find("eliot");
    ASSERT_EQUALS(5, i->second);
    m.erase(i);
    ASSERT_EQUALS(0U, m.size());
    ASSERT_EQUALS(true, m.empty());
}

TEST(StringMapTest, Iterator1) {
    StringMap<int> m;
    ASSERT(m.begin() == m.end());
}

TEST(StringMapTest, Iterator2) {
    StringMap<int> m;
    m["eliot"] = 5;
    StringMap<int>::const_iterator i = m.begin();
    ASSERT_EQUALS("eliot", i->first);
    ASSERT_EQUALS(5, i->second);
    ++i;
    ASSERT(i == m.end());
}

TEST(StringMapTest, Iterator3) {
    StringMap<int> m;
    m["eliot"] = 5;
    m["bob"] = 6;
    StringMap<int>::const_iterator i = m.begin();
    int sum = 0;
    for (; i != m.end(); ++i) {
        sum += i->second;
    }
    ASSERT_EQUALS(11, sum);
}


TEST(StringMapTest, Copy1) {
    StringMap<int> m;
    m["eliot"] = 5;
    StringMap<int> y = m;
    ASSERT_EQUALS(5, y["eliot"]);

    m["eliot"] = 6;
    ASSERT_EQUALS(6, m["eliot"]);
    ASSERT_EQUALS(5, y["eliot"]);
}

TEST(StringMapTest, Assign) {
    StringMap<int> m;
    m["eliot"] = 5;

    StringMap<int> y;
    y["eliot"] = 6;
    ASSERT_EQUALS(6, y["eliot"]);

    y = m;
    ASSERT_EQUALS(5, y["eliot"]);
}

TEST(StringMapTest, InitWithInitializerList) {
    StringMap<int> smap{
        {"q", 1},
        {"coollog", 2},
        {"mango", 3},
        {"mango", 4},
    };

    ASSERT_EQ(1, smap["q"]);
    ASSERT_EQ(2, smap["coollog"]);
    ASSERT_EQ(3, smap["mango"]);
}
}  // namespace
