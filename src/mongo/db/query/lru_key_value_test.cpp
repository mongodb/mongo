/**
 *    Copyright (C) 2013 10gen Inc.
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

#include "mongo/db/query/lru_key_value.h"

#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

using namespace mongo;

namespace {

//
// Convenience functions
//

void assertInKVStore(LRUKeyValue<int, int>& cache, int key, int value) {
    int* cachedValue = NULL;
    ASSERT_TRUE(cache.hasKey(key));
    Status s = cache.get(key, &cachedValue);
    ASSERT_OK(s);
    ASSERT_EQUALS(*cachedValue, value);
}

void assertNotInKVStore(LRUKeyValue<int, int>& cache, int key) {
    int* cachedValue = NULL;
    ASSERT_FALSE(cache.hasKey(key));
    Status s = cache.get(key, &cachedValue);
    ASSERT_NOT_OK(s);
}

/**
 * Test that we can add an entry and get it back out.
 */
TEST(LRUKeyValueTest, BasicAddGet) {
    LRUKeyValue<int, int> cache(100);
    cache.add(1, new int(2));
    assertInKVStore(cache, 1, 2);
}

/**
 * A kv-store with a max size of 0 isn't too useful, but test
 * that at the very least we don't blow up.
 */
TEST(LRUKeyValueTest, SizeZeroCache) {
    LRUKeyValue<int, int> cache(0);
    cache.add(1, new int(2));
    assertNotInKVStore(cache, 1);
}

/**
 * Make sure eviction and promotion work properly with
 * a kv-store of size 1.
 */
TEST(LRUKeyValueTest, SizeOneCache) {
    LRUKeyValue<int, int> cache(1);
    cache.add(0, new int(0));
    assertInKVStore(cache, 0, 0);

    // Second entry should immediately evict the first.
    cache.add(1, new int(1));
    assertNotInKVStore(cache, 0);
    assertInKVStore(cache, 1, 1);
}

/**
 * Fill up a size 10 kv-store with 10 entries. Call get()
 * on every entry except for one. Then call add() and
 * make sure that the proper entry got evicted.
 */
TEST(LRUKeyValueTest, EvictionTest) {
    int maxSize = 10;
    LRUKeyValue<int, int> cache(maxSize);
    for (int i = 0; i < maxSize; ++i) {
        std::unique_ptr<int> evicted = cache.add(i, new int(i));
        ASSERT(NULL == evicted.get());
    }
    ASSERT_EQUALS(cache.size(), (size_t)maxSize);

    // Call get() on all but one key.
    int evictKey = 5;
    for (int i = 0; i < maxSize; ++i) {
        if (i == evictKey) {
            continue;
        }
        assertInKVStore(cache, i, i);
    }

    // Adding another entry causes an eviction.
    std::unique_ptr<int> evicted = cache.add(maxSize + 1, new int(maxSize + 1));
    ASSERT_EQUALS(cache.size(), (size_t)maxSize);
    ASSERT(NULL != evicted.get());
    ASSERT_EQUALS(*evicted, evictKey);

    // Check that the least recently accessed has been evicted.
    for (int i = 0; i < maxSize; ++i) {
        if (i == evictKey) {
            assertNotInKVStore(cache, evictKey);
        } else {
            assertInKVStore(cache, i, i);
        }
    }
}

/**
 * Fill up a size 10 kv-store with 10 entries. Call get()
 * on a single entry to promote it to most recently
 * accessed. Then cause 9 evictions and make sure only
 * the entry on which we called get() remains.
 */
TEST(LRUKeyValueTest, PromotionTest) {
    int maxSize = 10;
    LRUKeyValue<int, int> cache(maxSize);
    for (int i = 0; i < maxSize; ++i) {
        std::unique_ptr<int> evicted = cache.add(i, new int(i));
        ASSERT(NULL == evicted.get());
    }
    ASSERT_EQUALS(cache.size(), (size_t)maxSize);

    // Call get() on a particular key.
    int promoteKey = 5;
    assertInKVStore(cache, promoteKey, promoteKey);

    // Evict all but one of the original entries.
    for (int i = maxSize; i < (maxSize + maxSize - 1); ++i) {
        std::unique_ptr<int> evicted = cache.add(i, new int(i));
        ASSERT(NULL != evicted.get());
    }
    ASSERT_EQUALS(cache.size(), (size_t)maxSize);

    // Check that the promoteKey has not been evicted.
    for (int i = 0; i < maxSize; ++i) {
        if (i == promoteKey) {
            assertInKVStore(cache, promoteKey, promoteKey);
        } else {
            assertNotInKVStore(cache, i);
        }
    }
}

/**
 * Test that calling add() with a key that already exists
 * in the kv-store deletes the existing entry.
 */
TEST(LRUKeyValueTest, ReplaceKeyTest) {
    LRUKeyValue<int, int> cache(10);
    cache.add(4, new int(4));
    assertInKVStore(cache, 4, 4);
    cache.add(4, new int(5));
    assertInKVStore(cache, 4, 5);
}

/**
 * Test iteration over the kv-store.
 */
TEST(LRUKeyValueTest, IterationTest) {
    LRUKeyValue<int, int> cache(2);
    cache.add(1, new int(1));
    cache.add(2, new int(2));

    typedef std::list<std::pair<int, int*>>::const_iterator CacheIterator;
    CacheIterator i = cache.begin();
    ASSERT_EQUALS(i->first, 2);
    ASSERT_EQUALS(*i->second, 2);
    ++i;
    ASSERT_EQUALS(i->first, 1);
    ASSERT_EQUALS(*i->second, 1);
    ++i;
    ASSERT(i == cache.end());
}

}  // namespace
