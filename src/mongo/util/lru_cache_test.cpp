
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

#include "mongo/platform/basic.h"

#include <iostream>
#include <type_traits>
#include <utility>

#include "mongo/stdx/type_traits.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/lru_cache.h"

using namespace mongo;

namespace {

/**
 * This class provides an ostream operator to allow us to use the ASSERT_*
 * unittest macros.
 */
template <typename I>
class iterator_wrapper {
public:
    explicit iterator_wrapper(const I& i) : _iter(i) {}

    friend bool operator==(const iterator_wrapper& a, const iterator_wrapper& b) {
        return a._iter == b._iter;
    }

    friend bool operator!=(const iterator_wrapper& a, const iterator_wrapper& b) {
        return !(a == b);
    }

    friend std::ostream& operator<<(std::ostream& os, iterator_wrapper it) {
        return os;
    }

private:
    I _iter;
};

// All of the template magic below this point is to allow us to use the assert
// unittest macros, which require ostream operators, with the LRUCache's iterators

// SFINAE on the property of whether a type has an ostream operator available.
template <typename T, typename = void>
struct hasOstreamOperator : std::false_type {};

template <typename T>
struct hasOstreamOperator<
    T,
    stdx::void_t<decltype(std::declval<std::ostream&>() << std::declval<T>())>> : std::true_type {};

// sanity check
static_assert(hasOstreamOperator<int>::value, "ERROR: int should have an ostream operator");
static_assert(!hasOstreamOperator<std::list<int>::iterator>::value,
              "ERROR: this type does not have an ostream operator");

// This utility allows us to wrap things that don't have ostream operators so we
// can provide them with a dummy ostream operator.
template <typename T>
iterator_wrapper<T> makeWrapper(const T& t) {
    return iterator_wrapper<T>(t);
}

// To forward to the assert macros properly, we can define specializations of this type
// that either call ASSERT_* directly or wrap, and then call ASSERT_*
template <typename T, bool needToWrap = !hasOstreamOperator<T>::value>
struct assertWithOstream;

// Types that have an ostream operator will flow through this struct.
template <typename T>
struct assertWithOstream<T, false> {
    void operator()(const T& a, const T& b, bool eq) {
        if (eq) {
            ASSERT_EQUALS(a, b);
        } else {
            ASSERT_NOT_EQUALS(a, b);
        }
    }
};

// Types that lack an ostream operator will flow through this struct.
template <typename T>
struct assertWithOstream<T, true> {
    void operator()(const T& a, const T& b, const bool eq) {
        assertWithOstream<iterator_wrapper<T>>{}(makeWrapper<T>(a), makeWrapper<T>(b), eq);
    }
};

template <typename T>
void assertEquals(const T& a, const T& b) {
    assertWithOstream<T>()(a, b, true);
}

template <typename T>
void assertNotEquals(const T& a, const T& b) {
    assertWithOstream<T>()(a, b, false);
}

template <typename K, typename V>
void assertInCache(const LRUCache<K, V>& cache, const K& key, const V& value) {
    ASSERT_TRUE(cache.hasKey(key));
    auto i = cache.cfind(key);
    assertNotEquals(i, cache.cend());
    assertEquals(i->second, value);
}

template <typename K, typename V>
void assertNotInCache(const LRUCache<K, V>& cache, const K& key) {
    ASSERT_FALSE(cache.hasKey(key));
    assertEquals(cache.cfind(key), cache.cend());
}

const std::array<int, 7> kTestSizes{1, 2, 3, 4, 5, 10, 1000};
using SizedTest = stdx::function<void(int)>;
void runWithDifferentSizes(SizedTest test) {
    for (auto size : kTestSizes) {
        mongo::unittest::log() << "\t\tTesting cache size of " << size;
        test(size);
    }
}

// Test that using cfind() returns the element without promoting it.
TEST(LRUCacheTest, CFindTest) {
    runWithDifferentSizes([](int maxSize) {
        LRUCache<int, int> cache(maxSize);

        // Fill up the cache
        for (int i = 0; i < maxSize; i++) {
            auto evicted = cache.add(i, i);
            ASSERT_FALSE(evicted);
        }

        // Call cfind on each key, ensure that list order does not change.
        auto firstElem = cache.begin();
        for (int i = 0; i < maxSize; i++) {
            auto found = cache.cfind(i);
            assertNotEquals(found, cache.cend());
            assertEquals(firstElem, cache.begin());
        }
    });
}

// Test that we can add an entry and get it back out.
TEST(LRUCacheTest, BasicAddGet) {
    LRUCache<int, int> cache(100);
    assertEquals<size_t>(cache.size(), 0);

    cache.add(1, 2);
    assertEquals(cache.size(), size_t(1));
    assertInCache(cache, 1, 2);

    assertNotInCache(cache, 2);
    assertNotInCache(cache, 4);

    assertEquals(cache.size(), size_t(1));
}

// A cache with a max size of 0 is permanently empty.
TEST(LRUCacheTest, SizeZeroCache) {
    LRUCache<int, int> cache(0);
    assertEquals(cache.size(), size_t(0));

    // When elements are added to a zero-size cache, instant eviction.
    auto evicted = cache.add(1, 2);
    assertEquals(*evicted, 2);
    assertEquals(cache.size(), size_t(0));
    assertNotInCache(cache, 1);

    // Promote should be a no-op
    cache.promote(3);
    assertEquals(cache.size(), size_t(0));
    assertNotInCache(cache, 3);

    // Erase should be a no-op
    assertEquals(cache.erase(4), size_t(0));
    assertEquals(cache.size(), size_t(0));
    assertNotInCache(cache, 4);

    // Find should be a no-op
    assertEquals(cache.find(1), cache.end());
    assertEquals(cache.size(), size_t(0));
    assertNotInCache(cache, 1);
}

// Test a very large cache size
TEST(LRUCacheTest, StressTest) {
    const int maxSize = 1000000;
    LRUCache<int, int> cache(maxSize);

    // Fill up the cache
    for (int i = 0; i < maxSize; i++) {
        auto evicted = cache.add(i, i);
        ASSERT_FALSE(evicted);
    }

    assertEquals(cache.size(), size_t(maxSize));

    // Perform some basic functions on the cache
    std::array<int, 5> sample{1, 34, 400, 12345, 999999};
    for (auto s : sample) {
        auto found = cache.find(s);
        assertEquals(found->second, s);
        assertEquals(found, cache.begin());

        const auto nextAfterFound = std::next(found);
        assertEquals(cache.erase(found), nextAfterFound);
        assertEquals(cache.size(), size_t(maxSize - 1));
        cache.add(s, s);
        assertEquals(cache.size(), size_t(maxSize));
        assertEquals(cache.erase(s), size_t(1));
        assertEquals(cache.size(), size_t(maxSize - 1));
        cache.add(s, s);
    }

    // Try causing an eviction
    auto evicted = cache.add(maxSize + 1, maxSize + 1);
    assertEquals(cache.size(), size_t(maxSize));
    assertEquals(*evicted, 0);
    assertInCache(cache, maxSize + 1, maxSize + 1);
    assertNotInCache(cache, 0);
}

// Make sure eviction and promotion work properly with a cache of size 1.
TEST(LRUCacheTest, SizeOneCache) {
    LRUCache<int, int> cache(1);
    assertEquals(cache.size(), size_t(0));

    cache.add(0, 0);
    assertEquals(cache.size(), size_t(1));
    assertInCache(cache, 0, 0);

    // Second entry should immediately evict the first.
    cache.add(1, 1);
    assertEquals(cache.size(), size_t(1));
    assertNotInCache(cache, 0);
    assertInCache(cache, 1, 1);
}

// Test cache eviction when the cache is full and new elements are added.
TEST(LRUCacheTest, EvictionTest) {
    runWithDifferentSizes([](int maxSize) {

        // Test eviction for any permutation of the original cache
        for (int i = 0; i < maxSize; i++) {
            LRUCache<int, int> cache(maxSize);

            // Fill up the cache
            for (int j = 0; j < maxSize; j++) {
                auto evicted = cache.add(j, j);
                ASSERT_FALSE(evicted);
            }

            // Find all but one key, moving that to the back
            for (int j = 0; j < maxSize; j++) {
                if (i != j) {
                    assertNotEquals(cache.find(j), cache.end());
                }
            }

            // Adding another entry will evict the least-recently used one
            auto evicted = cache.add(maxSize, maxSize);
            assertEquals(cache.size(), size_t(maxSize));
            assertEquals(*evicted, i);
            assertInCache(cache, maxSize, maxSize);
            assertNotInCache(cache, *evicted);
        }
    });
}

// Test that using promote() makes the promoted element the most-recently used,
// from any original position in the cache.
TEST(LRUCacheTest, PromoteTest) {
    runWithDifferentSizes([](int maxSize) {

        // Test promotion for any position in the original cache
        // i <= maxSize here, so we test promotion of cache.end(),
        // and of a non-existent key.
        for (int i = 0; i <= maxSize; i++) {
            LRUCache<int, int> cache(maxSize);

            // Fill up the cache
            for (int j = 0; j < maxSize; j++) {
                auto evicted = cache.add(j, j);
                ASSERT_FALSE(evicted);
            }

            // Promote a key, check that it is now at the front
            cache.promote(i);
            if (i < maxSize) {
                assertEquals((cache.begin())->first, i);
            } else {
                // if key did not exist, no change to the list
                assertEquals((cache.begin())->first, maxSize - 1);
            }

            // Promote the iterator at position i, check that it happens properly
            auto it = cache.begin();
            for (int j = 0; j < i; j++) {
                it++;
            }

            cache.promote(it);

            if (it == cache.end()) {
                // If we are at this case, no elements should have been promoted this round.
                assertEquals((cache.begin())->first, maxSize - 1);
            } else {
                // Otherwise, check that promotion happened as expected.
                assertEquals(cache.begin(), it);
            }
        }
    });
}

// Test that calling add() with a key that already exists in the cache deletes
// the existing entry and gets promoted properly
TEST(LRUCacheTest, ReplaceKeyTest) {
    runWithDifferentSizes([](int maxSize) {

        // Test replacement for any position in the original cache
        for (int i = 0; i < maxSize; i++) {
            LRUCache<int, int> cache(maxSize);

            // Fill up the cache
            for (int j = 0; j < maxSize; j++) {
                auto evicted = cache.add(j, j);
                ASSERT_FALSE(evicted);
            }

            // Replace a key, check for promotion with new value
            auto evicted = cache.add(i, maxSize);
            ASSERT_FALSE(evicted);
            assertEquals((cache.begin())->first, i);
            assertEquals((cache.begin())->second, maxSize);
        }
    });
}

// Test that calling add() with a key that already exists in the cache deletes
// the existing entry and gets promoted properly
TEST(LRUCacheTest, EraseByKey) {
    runWithDifferentSizes([](int maxSize) {

        // Test replacement for any position in the original cache
        // i <= maxSize so we erase a non-existent element
        for (int i = 0; i <= maxSize; i++) {
            LRUCache<int, int> cache(maxSize);

            // Fill up the cache
            for (int j = 0; j < maxSize; j++) {
                auto evicted = cache.add(j, j);
                ASSERT_FALSE(evicted);
            }

            assertEquals(cache.size(), size_t(maxSize));

            // Erase an element
            if (i != maxSize) {
                assertEquals(cache.erase(i), size_t(1));
                assertEquals(cache.size(), size_t(maxSize - 1));
            } else {
                assertEquals(cache.erase(i), size_t(0));
                assertEquals(cache.size(), size_t(maxSize));
            }

            // Check that all expected elements are still in the list
            for (int j = 0; j < maxSize; j++) {
                if (i != j || i == maxSize) {
                    assertInCache(cache, j, j);
                } else {
                    assertNotInCache(cache, j);
                }
            }
        }
    });
}

// Test removal of elements by iterator from the cache
TEST(LRUCacheTest, EraseByIterator) {
    runWithDifferentSizes([](int maxSize) {

        // Test replacement for any position in the original cache
        for (int i = 0; i < maxSize; i++) {
            LRUCache<int, int> cache(maxSize);

            // Fill up the cache
            for (int j = 0; j < maxSize; j++) {
                auto evicted = cache.add(j, j);
                ASSERT_FALSE(evicted);
            }

            assertEquals(cache.size(), size_t(maxSize));

            auto it = cache.begin();
            auto elem = maxSize - 1;
            for (int j = 0; j < i; j++) {
                it++;
                elem--;
            }

            auto nextElement = std::next(it);
            assertEquals(cache.erase(it), nextElement);

            if (i == maxSize) {
                assertEquals(cache.size(), size_t(maxSize));
            } else {
                assertEquals(cache.size(), size_t(maxSize - 1));
            }

            // Check that all expected elements are still in the cache
            for (int j = 0; j < maxSize; j++) {
                if (elem != j || i == maxSize) {
                    assertInCache(cache, j, j);
                } else {
                    assertNotInCache(cache, j);
                }
            }
        }
    });
}

// Test iteration over the cache.
TEST(LRUCacheTest, IterationTest) {
    const int maxSize = 4;
    LRUCache<int, int> cache(maxSize);

    // iterate over empty cache
    assertEquals(cache.size(), size_t(0));
    auto it = cache.begin();
    assertEquals(it, cache.end());

    // iterate over partially full cache
    cache.add(1, 1);
    cache.add(2, 2);
    assertEquals(cache.size(), size_t(2));
    it = cache.begin();
    assertEquals((it++)->first, 2);
    assertEquals((it++)->first, 1);
    assertEquals(it, cache.end());

    // iterate over full cache (and iteration doesn't change LRU order)
    cache.add(3, 3);
    cache.add(4, 4);
    assertEquals(cache.size(), size_t(4));

    it = cache.begin();
    assertEquals((it++)->first, 4);
    assertEquals((it++)->first, 3);
    assertEquals((it++)->first, 2);
    assertEquals((it++)->first, 1);
    ASSERT(it == cache.end());

    // iterate while promoting
    for (int i = 0; i < maxSize; i++) {
        for (int j = 0; j < maxSize; j++) {

            // Promote element j while we are paused iterating over i
            auto iter = cache.begin();
            for (int pos = 0; pos < i; pos++) {
                iter++;
            }

            cache.promote(j);

            // Test if we should now be pointing to beginning of cache
            if (iter->first == j) {
                assertEquals(iter, cache.begin());
            } else if (i != 0) {
                assertNotEquals(iter, cache.begin());
            }
        }
    }

    // two iterators compare equal when on the same element
    it = cache.begin();
    auto it2 = cache.begin();
    assertEquals(it++, it2++);
    assertEquals(it++, it2++);
    assertEquals(it++, it2++);
    assertEquals(it, it2);

    // Check for bidirectionality of iterators
    assertEquals(--it, --it2);
    assertEquals(--it, --it2);

    // Check for equal transformations
    it = cache.begin();
    it2 = cache.begin();
    it++;
    it++;
    it++;
    it--;
    it--;
    it2++;
    it2--;
    it2++;
    assertEquals(it, it2);

    // eviction while iterating doesn't affect iterators
    it = cache.begin();
    auto prevFirst = it->first;
    cache.add(5, 5);
    assertEquals(it->first, prevFirst);
}

// A helper class that has a custom hasher and equality operator
struct FunkyKeyType {
    FunkyKeyType(int a, int b) : _a(a), _b(b) {}

    struct Hash {
        size_t operator()(const FunkyKeyType& key) const {
            return key._a;
        }
    };

    struct Equal {
        bool operator()(const FunkyKeyType& lhs, const FunkyKeyType& rhs) const {
            return lhs._a == rhs._a;
        }
    };

    int _a;
    int _b;
};

// Test that we can properly use this cache with types with custom hash functors
// and equality operators.
TEST(LRUCacheTest, CustomHashAndEqualityTypeTest) {
    LRUCache<FunkyKeyType, int, FunkyKeyType::Hash, FunkyKeyType::Equal> cache(10);

    // Round trip an element into and out of the cache
    FunkyKeyType key(10, 20);
    auto b = key._b;
    cache.add(key, key._b);

    auto found = cache.find(key);
    assertNotEquals(found, cache.end());
    assertEquals(found->second, b);

    // Attempt to insert a key that is equal by our comparison operator,
    // this should replace the original value of 20 with 0.
    FunkyKeyType sortaEqual(10, 0);
    assertEquals(cache.size(), size_t(1));
    cache.add(sortaEqual, sortaEqual._b);
    assertEquals(cache.size(), size_t(1));
    found = cache.find(key);
    assertNotEquals(found, cache.end());
    assertNotEquals(found->second, b);
    assertEquals(found->second, sortaEqual._b);
}

TEST(LRUCacheTest, EmptyTest) {
    const int maxSize = 4;
    LRUCache<int, int> cache(maxSize);
    assertEquals(cache.empty(), true);
    cache.add(1, 2);
    assertEquals(cache.empty(), false);
    cache.erase(1);
    assertEquals(cache.empty(), true);
}

TEST(LRUCacheTest, CountTest) {
    const int maxSize = 4;
    LRUCache<int, int> cache(maxSize);
    assertEquals(cache.count(1), size_t(0));
    cache.add(1, 2);
    assertEquals(cache.count(1), size_t(1));
    cache.erase(1);
    assertEquals(cache.count(1), size_t(0));
}

}  // namespace
