/**
 *    Copyright (C) 2016 MongoDB Inc.
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
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/pipeline/lookup_set_cache.h"
#include "mongo/unittest/unittest.h"

namespace mongo {

BSONObj intToObj(int value) {
    return BSON("n" << value);
}

TEST(LookupSetCacheTest, InsertAndRetrieveWorksCorrectly) {
    LookupSetCache cache;
    cache.insert(Value(0), intToObj(1));
    cache.insert(Value(0), intToObj(2));
    cache.insert(Value(0), intToObj(3));
    cache.insert(Value(1), intToObj(4));
    cache.insert(Value(1), intToObj(5));

    ASSERT(cache[Value(0)]);
    ASSERT_TRUE(cache[Value(0)]->count(intToObj(1)));
    ASSERT_TRUE(cache[Value(0)]->count(intToObj(2)));
    ASSERT_TRUE(cache[Value(0)]->count(intToObj(3)));
    ASSERT_FALSE(cache[Value(0)]->count(intToObj(4)));
    ASSERT_FALSE(cache[Value(0)]->count(intToObj(5)));
}

TEST(LookupSetCacheTest, CacheDoesEvictInExpectedOrder) {
    LookupSetCache cache;

    cache.insert(Value(0), intToObj(0));
    cache.insert(Value(1), intToObj(0));
    cache.insert(Value(2), intToObj(0));
    cache.insert(Value(3), intToObj(0));

    // Cache ordering is {1: ..., 3: ..., 2: ..., 0: ...}.
    cache.evictOne();
    ASSERT_FALSE(cache[Value(0)]);
    cache.evictOne();
    ASSERT_FALSE(cache[Value(2)]);
    cache.evictOne();
    ASSERT_FALSE(cache[Value(3)]);
    cache.evictOne();
    ASSERT_FALSE(cache[Value(1)]);
}

TEST(LookupSetCacheTest, ReadDoesMoveKeyToFrontOfCache) {
    LookupSetCache cache;

    cache.insert(Value(0), intToObj(0));
    cache.insert(Value(1), intToObj(0));
    // Cache ordering is now {1: [1], 0: [0]}.

    ASSERT_TRUE(cache[Value(0)]);
    // Cache ordering is now {0: [0], 1: [1]}.

    cache.evictOne();
    ASSERT_TRUE(cache[Value(0)]);
    ASSERT_FALSE(cache[Value(1)]);
}

TEST(LookupSetCacheTest, InsertDoesPutKeyInMiddle) {
    LookupSetCache cache;

    cache.insert(Value(0), intToObj(0));
    cache.insert(Value(1), intToObj(0));
    cache.insert(Value(2), intToObj(0));
    cache.insert(Value(3), intToObj(0));

    cache.evictUntilSize(1);

    ASSERT_TRUE(cache[Value(1)]);
}

TEST(LookupSetCacheTest, EvictDoesRespectMemoryUsage) {
    LookupSetCache cache;

    cache.insert(Value(0), intToObj(0));
    cache.insert(Value(1), intToObj(0));

    // One size_t for the key, one for the value.
    cache.evictDownTo(Value(1).getApproximateSize() + static_cast<size_t>(intToObj(0).objsize()));

    ASSERT_TRUE(cache[Value(1)]);
    ASSERT_FALSE(cache[Value(0)]);
}

TEST(LookupSetCacheTest, ComplexAccessPatternDoesBehaveCorrectly) {
    LookupSetCache cache;

    for (int i = 0; i < 5; i++) {
        for (int j = 0; j < 5; j++) {
            cache.insert(Value(j), intToObj(i));
        }
    }

    // Cache ordering is now {0: ..., 3: ..., 4: ..., 2: ..., 1: ...}
    cache.evictOne();
    ASSERT_FALSE(cache[Value(0)]);

    ASSERT_TRUE(cache[Value(1)]);
    ASSERT_TRUE(cache[Value(1)]->count(intToObj(0)));
    ASSERT_TRUE(cache[Value(1)]->count(intToObj(1)));
    ASSERT_TRUE(cache[Value(1)]->count(intToObj(2)));
    ASSERT_TRUE(cache[Value(1)]->count(intToObj(3)));
    ASSERT_TRUE(cache[Value(1)]->count(intToObj(4)));

    cache.evictUntilSize(2);
    // Cache ordering is now {1: ..., 3: ...}

    ASSERT_TRUE(cache[Value(1)]);
    ASSERT_TRUE(cache[Value(3)]);
    // Cache ordering is now {3: ..., 1: ...}

    cache.evictOne();
    ASSERT_FALSE(cache[Value(1)]);

    cache.insert(Value(5), intToObj(0));
    cache.evictDownTo(Value(5).getApproximateSize() + static_cast<size_t>(intToObj(0).objsize()));

    ASSERT_EQ(cache.size(), 1U);
    ASSERT_TRUE(cache[Value(5)]);
}

}  // namespace mongo
