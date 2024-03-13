/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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


#include <compare>
#include <cstddef>
#include <fmt/format.h>
#include <initializer_list>
#include <memory>
#include <ostream>
#include <stdexcept>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>

#include <immer/detail/iterator_facade.hpp>
#include <immer/detail/rbts/rrbtree_iterator.hpp>
#include <immer/detail/util.hpp>

#include "mongo/base/string_data.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/framework.h"
#include "mongo/util/immutable/map.h"
#include "mongo/util/immutable/set.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo {
namespace {

class UserDefinedKey {
public:
    UserDefinedKey() = default;
    explicit UserDefinedKey(int val) : a(val) {}

    bool operator==(const UserDefinedKey& rhs) const {
        return a == rhs.a;
    }

    bool operator<(const UserDefinedKey& rhs) const {
        return a < rhs.a;
    }

    std::string toString() const {
        return std::to_string(a);
    }

private:
    int a = 0;
};


class Incomparable {
    friend struct CompareIncomparable;

public:
    Incomparable() = default;
    explicit Incomparable(int val) : a(val) {}

    bool operator==(const Incomparable&) = delete;
    bool operator<(const Incomparable&) = delete;

    std::string toString() const {
        return std::to_string(a);
    }

private:
    int a = 0;
};

struct CompareIncomparable {
    bool operator()(const Incomparable& a, const Incomparable& b) const {
        return a.a < b.a;
    }

    // Pair comparator needed for some testing macros to function properly for both maps and sets.
    bool operator()(const std::pair<Incomparable, int>& a,
                    const std::pair<Incomparable, int>& b) const {
        return a.first.a < b.first.a;
    }
};

struct StringCompare {
    bool operator()(const std::string& a, const std::string& b) const {
        return a < b;
    }
    bool operator()(const std::string& a, StringData b) const {
        return a < b;
    }
    bool operator()(StringData a, const std::string& b) const {
        return a < b;
    }
    bool operator()(const std::string& a, const char* b) const {
        return a < b;
    }
    bool operator()(const char* a, const std::string& b) const {
        return a < b;
    }
};

template <typename C, typename L>
void ensureContainerInvariants(const C& container, L&& less) {
    size_t visited = 0;
    for (auto it = container.begin(); it != container.end(); ++it) {
        if (++visited > 1) {
            ASSERT(less(*(it - 1), *it));
        }
    }
    ASSERT_EQ(visited, container.size());
}


template <typename C>
void ensureContainerInvariants(const C& container) {
    size_t visited = 0;
    for (auto it = container.begin(); it != container.end(); ++it) {
        if (++visited > 1) {
            ASSERT_LT(*(it - 1), *it);
        }
    }
    ASSERT_EQ(visited, container.size());
}

template <typename C>
void ensureContainerInvariants(const std::tuple<C, C, C, C>& containers) {
    ensureContainerInvariants(std::get<0>(containers));
    ensureContainerInvariants(std::get<1>(containers));
    ensureContainerInvariants(std::get<2>(containers));
    ensureContainerInvariants(std::get<3>(containers));
}

template <typename C>
void ensureContainerInvariants(std::initializer_list<const C> containers) {
    for (auto& c : containers) {
        ensureContainerInvariants(c);
    }
}

template <typename K, typename V, typename M = immutable::map<K, V>>
std::tuple<M, M, M, M> init_maps() {
    return std::make_tuple(M{}, M{}, M{}, M{});
}

template <typename K, typename S = immutable::set<K>>
std::tuple<S, S, S, S> init_sets() {
    return std::make_tuple(S{}, S{}, S{}, S{});
}

std::ostream& operator<<(std::ostream& s, const UserDefinedKey& k) {
    return s << k.toString();
}

std::ostream& operator<<(std::ostream& s, const Incomparable& k) {
    return s << k.toString();
}

template <typename T, typename U>
std::ostream& operator<<(std::ostream& s, const std::pair<T, U> pair) {
    return s << "(" << pair.first << "," << pair.second << ")";
}

template <typename C>
struct ContainerWrapper {
    ContainerWrapper(const C& c) : container{c} {}
    const C& container;
};
template <typename T>
std::ostream& operator<<(std::ostream& str, const ContainerWrapper<T>& wrapper) {
    str << "{";
    bool first = true;
    for (auto& el : wrapper.container) {
        if (first) {
            first = false;
        } else {
            str << ", ";
        }
        str << el;
    }
    str << "}";
    return str;
}

#define ASSERT_CONTAINS(containers, k)                    \
    {                                                     \
        ASSERT_TRUE(std::get<0>(containers).contains(k))  \
            << ContainerWrapper{std::get<0>(containers)}; \
        ASSERT_TRUE(std::get<1>(containers).contains(k))  \
            << ContainerWrapper{std::get<0>(containers)}; \
        ASSERT_TRUE(std::get<2>(containers).contains(k))  \
            << ContainerWrapper{std::get<0>(containers)}; \
        ASSERT_TRUE(std::get<3>(containers).contains(k))  \
            << ContainerWrapper{std::get<0>(containers)}; \
    }

#define ASSERT_NOT_CONTAINS(containers, k)                \
    {                                                     \
        ASSERT_FALSE(std::get<0>(containers).contains(k)) \
            << ContainerWrapper{std::get<0>(containers)}; \
        ASSERT_FALSE(std::get<1>(containers).contains(k)) \
            << ContainerWrapper{std::get<0>(containers)}; \
        ASSERT_FALSE(std::get<2>(containers).contains(k)) \
            << ContainerWrapper{std::get<0>(containers)}; \
        ASSERT_FALSE(std::get<3>(containers).contains(k)) \
            << ContainerWrapper{std::get<0>(containers)}; \
    }

#define ASSERT_VALUE_EQ(its, v)                 \
    {                                           \
        ASSERT_EQ(std::get<0>(its)->second, v); \
        ASSERT_EQ(std::get<1>(its)->second, v); \
        ASSERT_EQ(std::get<2>(its)->second, v); \
        ASSERT_EQ(std::get<3>(its)->second, v); \
    }

#define MUTATE_KV(containers, fn, k, v)                                                   \
    [&]() {                                                                               \
        auto _k1 = k;                                                                     \
        auto _v1 = v;                                                                     \
        auto _m1 = std::get<0>(containers).fn(_k1, _v1);                                  \
        ensureContainerInvariants(_m1);                                                   \
                                                                                          \
        auto _k2 = k;                                                                     \
        auto _v2 = v;                                                                     \
        auto _m2 = std::move(std::get<1>(containers)).fn(_k2, _v2);                       \
        ensureContainerInvariants(_m2);                                                   \
                                                                                          \
        auto _k3 = k;                                                                     \
        auto _v3 = v;                                                                     \
        auto _m3 = std::get<2>(containers).fn(std::move(_k3), std::move(_v3));            \
        ensureContainerInvariants(_m3);                                                   \
                                                                                          \
        auto _k4 = k;                                                                     \
        auto _v4 = v;                                                                     \
        auto _m4 = std::move(std::get<3>(containers)).fn(std::move(_k4), std::move(_v4)); \
        ensureContainerInvariants(_m4);                                                   \
                                                                                          \
        return std::make_tuple(_m1, _m2, _m3, _m4);                                       \
    }();

#define MUTATE_K(containers, fn, k)                                       \
    ([&]() {                                                              \
        auto _k1 = k;                                                     \
        auto _m1 = std::get<0>(containers).fn(_k1);                       \
        ensureContainerInvariants(_m1);                                   \
                                                                          \
        auto _k2 = k;                                                     \
        auto _m2 = std::move(std::get<1>(containers)).fn(_k2);            \
        ensureContainerInvariants(_m2);                                   \
                                                                          \
        auto _k3 = k;                                                     \
        auto _m3 = std::get<2>(containers).fn(std::move(_k3));            \
        ensureContainerInvariants(_m3);                                   \
                                                                          \
        auto _k4 = k;                                                     \
        auto _m4 = std::move(std::get<3>(containers)).fn(std::move(_k4)); \
        ensureContainerInvariants(_m4);                                   \
                                                                          \
        return std::make_tuple(_m1, _m2, _m3, _m4);                       \
    }());

#define MUTATE_IT_KV(containers, fn, its, k, v)                                                  \
    [&]() {                                                                                      \
        auto _k1 = k;                                                                            \
        auto _v1 = v;                                                                            \
        auto _m1 = std::get<0>(containers).fn(std::get<0>(its), _k1, _v1);                       \
        ensureContainerInvariants(_m1);                                                          \
                                                                                                 \
        auto _k2 = k;                                                                            \
        auto _v2 = v;                                                                            \
        auto _m2 = std::move(std::get<1>(containers)).fn(std::get<1>(its), _k2, _v2);            \
        ensureContainerInvariants(_m2);                                                          \
                                                                                                 \
        auto _k3 = k;                                                                            \
        auto _v3 = v;                                                                            \
        auto _m3 = std::get<2>(containers).fn(std::get<2>(its), std::move(_k3), std::move(_v3)); \
        ensureContainerInvariants(_m3);                                                          \
                                                                                                 \
        auto _k4 = k;                                                                            \
        auto _v4 = v;                                                                            \
        auto _m4 = std::move(std::get<3>(containers))                                            \
                       .fn(std::get<3>(its), std::move(_k4), std::move(_v4));                    \
        ensureContainerInvariants(_m4);                                                          \
                                                                                                 \
        return std::make_tuple(_m1, _m2, _m3, _m4);                                              \
    }();

#define MUTATE_IT_K(containers, fn, its, k)                                                 \
    [&]() {                                                                                 \
        auto _k1 = k;                                                                       \
        auto _m1 = std::get<0>(containers).fn(std::get<0>(its), _k1);                       \
        ensureContainerInvariants(_m1);                                                     \
                                                                                            \
        auto _k2 = k;                                                                       \
        auto _m2 = std::move(std::get<1>(containers)).fn(std::get<1>(its), _k2);            \
        ensureContainerInvariants(_m2);                                                     \
                                                                                            \
        auto _k3 = k;                                                                       \
        auto _m3 = std::get<2>(containers).fn(std::get<2>(its), std::move(_k3));            \
        ensureContainerInvariants(_m3);                                                     \
                                                                                            \
        auto _k4 = k;                                                                       \
        auto _m4 = std::move(std::get<3>(containers)).fn(std::get<3>(its), std::move(_k4)); \
        ensureContainerInvariants(_m4);                                                     \
                                                                                            \
        return std::make_tuple(_m1, _m2, _m3, _m4);                                         \
    }();

#define SEARCH(containers, fn, k)                   \
    [&]() {                                         \
        auto _i1 = std::get<0>(containers).fn(k);   \
        auto _i2 = std::get<1>(containers).fn(k);   \
        auto _i3 = std::get<2>(containers).fn(k);   \
        auto _i4 = std::get<3>(containers).fn(k);   \
        return std::make_tuple(_i1, _i2, _i3, _i4); \
    }();

#define END(containers)                             \
    [&]() {                                         \
        auto _i1 = std::get<0>(containers).end();   \
        auto _i2 = std::get<1>(containers).end();   \
        auto _i3 = std::get<2>(containers).end();   \
        auto _i4 = std::get<3>(containers).end();   \
        return std::make_tuple(_i1, _i2, _i3, _i4); \
    }();

TEST(ImmutableMap, Basic) {
    // Insert some values and verify that the data structure is behaving as expected
    immutable::map<int, int> v0;
    auto v1 = v0.set(1, 2);
    // Record the pointer to the value '1', verify that this doesn't change after performing more
    // inserts
    auto v1Val = v1.find(1);

    // Create distinct branches of the history from v1. v0 and v1 should be unaffected
    auto v2 = v1.update_if_exists(1, [](int v) { return v += 1; });
    auto v3 = v1.set(2, 3);

    // Verify that values are as expected
    ASSERT_EQ(v0.size(), 0);

    ASSERT_EQ(v1.size(), 1);
    ASSERT_TRUE(v1.contains(1));
    ASSERT_EQ(v1.find(1)->second, 2);

    ASSERT_EQ(v2.size(), 1);
    ASSERT_TRUE(v2.contains(1));
    ASSERT_EQ(v2.find(1)->second, 3);
    ASSERT_FALSE(v2.contains(2));

    ASSERT_EQ(v3.size(), 2);
    ASSERT_TRUE(v3.contains(1));
    ASSERT_EQ(v3.find(1)->second, 2);
    ASSERT_TRUE(v3.contains(2));
    ASSERT_EQ(v3.find(2)->second, 3);

    // Verify that v1's value did not change
    ASSERT_EQ(v1.find(1), v1Val);

    // Verify that erase works as expected, and preserves history.
    auto v4 = v3.erase(1).erase(2);
    ASSERT_FALSE(v4.contains(1));
    ASSERT_FALSE(v4.contains(2));
    ASSERT_TRUE(v3.contains(1));
    ASSERT_EQ(v3.find(1)->second, 2);
    ASSERT_TRUE(v3.contains(2));
    ASSERT_EQ(v3.find(2)->second, 3);
    ASSERT_TRUE(v2.contains(1));
    ASSERT_EQ(v2.find(1)->second, 3);
    ASSERT_FALSE(v2.contains(2));
    ASSERT_TRUE(v1.contains(1));
    ASSERT_EQ(v1.find(1)->second, 2);
    ASSERT_FALSE(v1.contains(2));

    ensureContainerInvariants({v0, v1, v2, v3, v4});
}

TEST(ImmutableMap, UserDefinedType) {
    immutable::map<UserDefinedKey, int> v0;
    auto v1 = v0.set(UserDefinedKey(1), 2);
    ASSERT_NE(v1.find(UserDefinedKey(1)), v1.end());
    ASSERT_EQ(v1.find(UserDefinedKey(1))->second, 2);

    ensureContainerInvariants({v0, v1});
}

TEST(ImmutableMap, IncomparableType) {
    immutable::map<Incomparable, int, CompareIncomparable> v0;
    auto v1 = v0.set(Incomparable(1), 2);
    ASSERT_NE(v1.find(Incomparable(1)), v1.end());
    ASSERT_EQ(v1.find(Incomparable(1))->second, 2);

    ensureContainerInvariants(v0, CompareIncomparable{});
    ensureContainerInvariants(v1, CompareIncomparable{});
}

TEST(ImmutableMap, HeterogeneousLookup) {
    immutable::map<std::string, int, StringCompare> v0;
    auto v1 = v0.set("str", 1);

    // Lookup using StringData without the need to convert to string.
    ASSERT_NE(v1.find("str"_sd), v1.end());

    ensureContainerInvariants({v0, v1});
}

TEST(ImmutableMap, Accessors) {
    immutable::map<int, int> v0;
    auto v1 = v0.insert(1, 1).insert(2, 2).insert(3, 3);

    ASSERT_EQ(v1[1], 1);
    ASSERT_EQ(v1[2], 2);
    ASSERT_EQ(v1[3], 3);
    ASSERT_EQ(v1.at(1), 1);
    ASSERT_EQ(v1.at(2), 2);
    ASSERT_EQ(v1.at(3), 3);

    // Handling of missing elements
    ASSERT_EQ(v1[4], 0);
    ASSERT_THROWS(v1.at(4), std::out_of_range);

    ensureContainerInvariants({v0, v1});
}

TEST(ImmutableMap, Bounds) {
    immutable::map<int, int> map;
    constexpr int numKeys = 100;
    for (int i = 0; i < numKeys; ++i) {
        map = map.set(2 * i, 2 * i);
        ensureContainerInvariants(map);
    }

    for (int i = 0; i < numKeys - 1; ++i) {
        auto lowerExact = map.lower_bound(2 * i);
        ASSERT(lowerExact != map.end() && lowerExact->first == 2 * i);
        auto upperExact = map.upper_bound(2 * i);
        ASSERT(upperExact != map.end() && upperExact->first == 2 * (i + 1));

        auto lowerNear = map.lower_bound(2 * i + 1);
        ASSERT(lowerNear != map.end() && lowerNear->first == 2 * (i + 1));
        auto upperNear = map.upper_bound(2 * i + 1);
        ASSERT(upperNear != map.end() && upperNear->first == 2 * (i + 1));
    }
}

TEST(ImmutableMap, Iteration) {
    immutable::map<int, int> map;
    constexpr int numKeys = 100;
    for (int i = 0; i < numKeys; ++i) {
        map = map.set(2 * i, 2 * i);
        ensureContainerInvariants(map);
    }

    auto map0 = map;
    auto it0 = map0.begin();

    for (int i = 0; i < numKeys; ++i) {
        map = map.set(2 * i + 1, 2 * i + 1);
        ensureContainerInvariants(map);
    }

    auto map1 = map;
    auto it1 = map1.begin();

    for (int i = 0; i < numKeys; ++i) {
        ASSERT_NE(it0, map0.end());
        ASSERT_EQ(it0->first, 2 * i);
        ++it0;

        ASSERT_NE(it1, map1.end());
        ASSERT_EQ(it1->first, 2 * i);
        ++it1;

        ASSERT_NE(it1, map1.end());
        ASSERT_EQ(it1->first, 2 * i + 1);
        ++it1;
    }
    ASSERT_EQ(it0, map0.end());
    ASSERT_EQ(it1, map1.end());
}

TEST(ImmutableMap, Insert) {
    auto v0 = init_maps<int, int>();

    // Populate an initial set of values.
    auto v1 = MUTATE_KV(v0, insert, 1, 1);
    auto v2 = MUTATE_KV(v1, insert, 2, 2);
    auto v3 = MUTATE_KV(v2, insert, 3, 3);
    auto v4 = MUTATE_KV(v3, insert, 5, 5);
    ASSERT_CONTAINS(v4, 1);
    ASSERT_CONTAINS(v4, 2);
    ASSERT_CONTAINS(v4, 3);
    ASSERT_CONTAINS(v4, 5);

    // Insert of existing key is a noop.
    auto v5 = MUTATE_KV(v4, insert, 1, 2);
    ASSERT_CONTAINS(v5, 1);
    auto i5 = SEARCH(v5, find, 1);
    ASSERT_VALUE_EQ(i5, 1);

    // Insert at beginning works.
    auto v6 = MUTATE_KV(v4, insert, 0, 0);
    ASSERT_CONTAINS(v6, 0);
    auto i6 = SEARCH(v6, find, 0);
    ASSERT_VALUE_EQ(i6, 0);

    // Insert at end works.
    auto v7 = MUTATE_KV(v4, insert, 6, 6);
    ASSERT_CONTAINS(v7, 6);
    auto i7 = SEARCH(v7, find, 6);
    ASSERT_VALUE_EQ(i7, 6);

    // Insert in middle works.
    auto v8 = MUTATE_KV(v4, insert, 4, 4);
    ASSERT_CONTAINS(v8, 4);
    auto i8 = SEARCH(v8, find, 4);
    ASSERT_VALUE_EQ(i8, 4);
}

TEST(ImmutableMap, InsertViaIterator) {
    auto v0 = init_maps<int, int>();

    // Populate an initial set of values.
    auto v1 = MUTATE_KV(v0, insert, 1, 1);
    auto v2 = MUTATE_KV(v1, insert, 2, 2);
    auto v3 = MUTATE_KV(v2, insert, 3, 3);
    auto v4 = MUTATE_KV(v3, insert, 6, 6);
    ASSERT_CONTAINS(v4, 1);
    ASSERT_CONTAINS(v4, 2);
    ASSERT_CONTAINS(v4, 3);
    ASSERT_CONTAINS(v4, 6);

    // Giving the iterator for an existing element does a noop.

    auto it4_2 = SEARCH(v4, find, 2);
    auto v5 = MUTATE_IT_KV(v4, insert, it4_2, 2, 5);
    auto it5_2 = SEARCH(v5, find, 2);
    ASSERT_VALUE_EQ(it5_2, 2);

    // Giving end() as hint works appropriately whether hint is accurate or not.

    auto end5 = END(v5);
    auto v6 = MUTATE_IT_KV(v5, insert, end5, 2, 5);
    auto it6_2 = SEARCH(v6, find, 2);
    ASSERT_VALUE_EQ(it6_2, 2);

    auto end6 = END(v6);
    auto v7 = MUTATE_IT_KV(v6, insert, end6, 4, 4);
    ASSERT_NOT_CONTAINS(v6, 4);
    ASSERT_CONTAINS(v7, 4);
    auto it7_4 = SEARCH(v7, find, 4);
    ASSERT_VALUE_EQ(it7_4, 4);

    auto end7 = END(v7);
    auto v8 = MUTATE_IT_KV(v7, insert, end7, 7, 7);
    ASSERT_NOT_CONTAINS(v7, 7);
    ASSERT_CONTAINS(v8, 7);
    auto it8_7 = SEARCH(v8, find, 7);
    ASSERT_VALUE_EQ(it8_7, 7);

    // Giving hint from lower_bound works for both existing and new entries.

    auto lb8_2 = SEARCH(v8, lower_bound, 2);
    auto v9 = MUTATE_IT_KV(v8, insert, lb8_2, 2, 5);
    auto it9_2 = SEARCH(v9, find, 2);
    ASSERT_VALUE_EQ(it9_2, 2);

    auto lb9_0 = SEARCH(v9, lower_bound, 0);
    auto v10 = MUTATE_IT_KV(v9, insert, lb9_0, 0, 0);
    ASSERT_NOT_CONTAINS(v9, 0);
    ASSERT_CONTAINS(v10, 0);
    auto it10_0 = SEARCH(v10, find, 0);
    ASSERT_VALUE_EQ(it10_0, 0);

    auto lb10_5 = SEARCH(v10, lower_bound, 5);
    auto v11 = MUTATE_IT_KV(v10, insert, lb10_5, 5, 5);
    ASSERT_NOT_CONTAINS(v10, 5);
    ASSERT_CONTAINS(v11, 5);
    auto it11_5 = SEARCH(v11, find, 5);
    ASSERT_VALUE_EQ(it11_5, 5);

    auto lb11_8 = SEARCH(v11, lower_bound, 8);
    auto v12 = MUTATE_IT_KV(v11, insert, lb11_8, 8, 8);
    ASSERT_NOT_CONTAINS(v11, 8);
    ASSERT_CONTAINS(v12, 8);
    auto it12_8 = SEARCH(v12, find, 8);
    ASSERT_VALUE_EQ(it12_8, 8);
}

TEST(ImmutableMap, Set) {
    auto v0 = init_maps<int, int>();

    // Populate an initial set of values.
    auto v1 = MUTATE_KV(v0, insert, 1, 1);
    auto v2 = MUTATE_KV(v1, insert, 2, 2);
    auto v3 = MUTATE_KV(v2, insert, 3, 3);
    auto v4 = MUTATE_KV(v3, insert, 5, 5);
    ASSERT_CONTAINS(v4, 1);
    ASSERT_CONTAINS(v4, 2);
    ASSERT_CONTAINS(v4, 3);
    ASSERT_CONTAINS(v4, 5);

    // Set on existing key updates value.
    auto v5 = MUTATE_KV(v4, set, 1, 2);
    ASSERT_CONTAINS(v5, 1);
    auto i5 = SEARCH(v5, find, 1);
    ASSERT_VALUE_EQ(i5, 2);

    // Set to insert at beginning works.
    auto v6 = MUTATE_KV(v4, set, 0, 0);
    ASSERT_CONTAINS(v6, 0);
    auto i6 = SEARCH(v6, find, 0);
    ASSERT_VALUE_EQ(i6, 0);

    // Set to insert at end works.
    auto v7 = MUTATE_KV(v4, set, 6, 6);
    ASSERT_CONTAINS(v7, 6);
    auto i7 = SEARCH(v7, find, 6);
    ASSERT_VALUE_EQ(i7, 6);

    // Set to insert in middle works.
    auto v8 = MUTATE_KV(v4, set, 4, 4);
    ASSERT_CONTAINS(v8, 4);
    auto i8 = SEARCH(v8, find, 4);
    ASSERT_VALUE_EQ(i8, 4);
}

TEST(ImmutableMap, SetViaIterator) {
    auto v0 = init_maps<int, int>();

    // Populate an initial set of values.
    auto v1 = MUTATE_KV(v0, insert, 1, 1);
    auto v2 = MUTATE_KV(v1, insert, 2, 2);
    auto v3 = MUTATE_KV(v2, insert, 3, 3);
    auto v4 = MUTATE_KV(v3, insert, 6, 6);
    ASSERT_CONTAINS(v4, 1);
    ASSERT_CONTAINS(v4, 2);
    ASSERT_CONTAINS(v4, 3);
    ASSERT_CONTAINS(v4, 6);

    // Giving the iterator for an existing element updates the value.

    auto it4_2 = SEARCH(v4, find, 2);
    auto v5 = MUTATE_IT_KV(v4, set, it4_2, 2, 5);
    auto it5_2 = SEARCH(v5, find, 2);
    ASSERT_VALUE_EQ(it5_2, 5);

    // Giving end() as hint works appropriately whether hint is accurate or not.

    auto end5 = END(v5);
    auto v6 = MUTATE_IT_KV(v5, set, end5, 2, 7);
    auto it6_2 = SEARCH(v6, find, 2);
    ASSERT_VALUE_EQ(it6_2, 7);

    auto end6 = END(v6);
    auto v7 = MUTATE_IT_KV(v6, set, end6, 4, 4);
    ASSERT_NOT_CONTAINS(v6, 4);
    ASSERT_CONTAINS(v7, 4);
    auto it7_4 = SEARCH(v7, find, 4);
    ASSERT_VALUE_EQ(it7_4, 4);

    auto end7 = END(v7);
    auto v8 = MUTATE_IT_KV(v7, set, end7, 7, 7);
    ASSERT_NOT_CONTAINS(v7, 7);
    ASSERT_CONTAINS(v8, 7);
    auto it8_7 = SEARCH(v8, find, 7);
    ASSERT_VALUE_EQ(it8_7, 7);

    // Giving hint from lower_bound works for both existing and new entries.

    auto lb8_2 = SEARCH(v8, lower_bound, 2);
    auto v9 = MUTATE_IT_KV(v8, set, lb8_2, 2, 9);
    auto it9_2 = SEARCH(v9, find, 2);
    ASSERT_VALUE_EQ(it9_2, 9);

    auto lb9_0 = SEARCH(v9, lower_bound, 0);
    auto v10 = MUTATE_IT_KV(v9, set, lb9_0, 0, 0);
    ASSERT_NOT_CONTAINS(v9, 0);
    ASSERT_CONTAINS(v10, 0);
    auto it10_0 = SEARCH(v10, find, 0);
    ASSERT_VALUE_EQ(it10_0, 0);

    auto lb10_5 = SEARCH(v10, lower_bound, 5);
    auto v11 = MUTATE_IT_KV(v10, set, lb10_5, 5, 5);
    ASSERT_NOT_CONTAINS(v10, 5);
    ASSERT_CONTAINS(v11, 5);
    auto it11_5 = SEARCH(v11, find, 5);
    ASSERT_VALUE_EQ(it11_5, 5);

    auto lb11_8 = SEARCH(v11, lower_bound, 8);
    auto v12 = MUTATE_IT_KV(v11, set, lb11_8, 8, 8);
    ASSERT_NOT_CONTAINS(v11, 8);
    ASSERT_CONTAINS(v12, 8);
    auto it12_8 = SEARCH(v12, find, 8);
    ASSERT_VALUE_EQ(it12_8, 8);
}

TEST(ImmutableMap, Update) {
    auto v0 = init_maps<int, int>();

    // Populate an initial set of values.
    auto v1 = MUTATE_KV(v0, insert, 1, 1);
    auto v2 = MUTATE_KV(v1, insert, 2, 2);
    auto v3 = MUTATE_KV(v2, insert, 3, 3);
    auto v4 = MUTATE_KV(v3, insert, 5, 5);
    ASSERT_CONTAINS(v4, 1);
    ASSERT_CONTAINS(v4, 2);
    ASSERT_CONTAINS(v4, 3);
    ASSERT_CONTAINS(v4, 5);

    // Update on existing key updates value.
    auto v5 = MUTATE_KV(v4, update, 1, [](int x) { return x + 1; });
    ASSERT_CONTAINS(v5, 1);
    auto i5 = SEARCH(v5, find, 1);
    ASSERT_VALUE_EQ(i5, 2);

    // Update to insert at beginning works.
    auto v6 = MUTATE_KV(v4, update, 0, [](int x) { return x + 1; });
    ASSERT_CONTAINS(v6, 0);
    auto i6 = SEARCH(v6, find, 0);
    ASSERT_VALUE_EQ(i6, 1);

    // Update to insert at end works.
    auto v7 = MUTATE_KV(v4, update, 6, [](int x) { return x + 1; });
    ASSERT_CONTAINS(v7, 6);
    auto i7 = SEARCH(v7, find, 6);
    ASSERT_VALUE_EQ(i7, 1);

    // Update to insert in middle works.
    auto v8 = MUTATE_KV(v4, update, 4, [](int x) { return x + 1; });
    ASSERT_CONTAINS(v8, 4);
    auto i8 = SEARCH(v8, find, 4);
    ASSERT_VALUE_EQ(i8, 1);
}

TEST(ImmutableMap, UpdateViaIterator) {
    auto v0 = init_maps<int, int>();

    // Populate an initial set of values.
    auto v1 = MUTATE_KV(v0, insert, 1, 1);
    auto v2 = MUTATE_KV(v1, insert, 2, 2);
    auto v3 = MUTATE_KV(v2, insert, 3, 3);
    auto v4 = MUTATE_KV(v3, insert, 6, 6);
    ASSERT_CONTAINS(v4, 1);
    ASSERT_CONTAINS(v4, 2);
    ASSERT_CONTAINS(v4, 3);
    ASSERT_CONTAINS(v4, 6);

    // Giving the iterator for an existing element updates the value.

    auto it4_2 = SEARCH(v4, find, 2);
    auto v5 = MUTATE_IT_KV(v4, update, it4_2, 2, [](int x) { return x + 1; });
    auto it5_2 = SEARCH(v5, find, 2);
    ASSERT_VALUE_EQ(it5_2, 3);

    // Giving end() as hint works appropriately whether hint is accurate or not.

    auto end5 = END(v5);
    auto v6 = MUTATE_IT_KV(v5, update, end5, 2, [](int x) { return x + 1; });
    auto it6_2 = SEARCH(v6, find, 2);
    ASSERT_VALUE_EQ(it6_2, 4);

    auto end6 = END(v6);
    auto v7 = MUTATE_IT_KV(v6, update, end6, 4, [](int x) { return x + 1; });
    ASSERT_NOT_CONTAINS(v6, 4);
    ASSERT_CONTAINS(v7, 4);
    auto it7_4 = SEARCH(v7, find, 4);
    ASSERT_VALUE_EQ(it7_4, 1);

    auto end7 = END(v7);
    auto v8 = MUTATE_IT_KV(v7, update, end7, 7, [](int x) { return x + 1; });
    ASSERT_NOT_CONTAINS(v7, 7);
    ASSERT_CONTAINS(v8, 7);
    auto it8_7 = SEARCH(v8, find, 7);
    ASSERT_VALUE_EQ(it8_7, 1);

    // Giving hint from lower_bound works for both existing and new entries.

    auto lb8_2 = SEARCH(v8, lower_bound, 2);
    auto v9 = MUTATE_IT_KV(v8, update, lb8_2, 2, [](int x) { return x + 1; });
    auto it9_2 = SEARCH(v9, find, 2);
    ASSERT_VALUE_EQ(it9_2, 5);

    auto lb9_0 = SEARCH(v9, lower_bound, 0);
    auto v10 = MUTATE_IT_KV(v9, update, lb9_0, 0, [](int x) { return x + 1; });
    ASSERT_NOT_CONTAINS(v9, 0);
    ASSERT_CONTAINS(v10, 0);
    auto it10_0 = SEARCH(v10, find, 0);
    ASSERT_VALUE_EQ(it10_0, 1);

    auto lb10_5 = SEARCH(v10, lower_bound, 5);
    auto v11 = MUTATE_IT_KV(v10, update, lb10_5, 5, [](int x) { return x + 1; });
    ASSERT_NOT_CONTAINS(v10, 5);
    ASSERT_CONTAINS(v11, 5);
    auto it11_5 = SEARCH(v11, find, 5);
    ASSERT_VALUE_EQ(it11_5, 1);

    auto lb11_8 = SEARCH(v11, lower_bound, 8);
    auto v12 = MUTATE_IT_KV(v11, update, lb11_8, 8, [](int x) { return x + 1; });
    ASSERT_NOT_CONTAINS(v11, 8);
    ASSERT_CONTAINS(v12, 8);
    auto it12_8 = SEARCH(v12, find, 8);
    ASSERT_VALUE_EQ(it12_8, 1);
}

TEST(ImmutableMap, UpdateIfExists) {
    auto v0 = init_maps<int, int>();

    // Populate an initial set of values.
    auto v1 = MUTATE_KV(v0, insert, 1, 1);
    auto v2 = MUTATE_KV(v1, insert, 2, 2);
    auto v3 = MUTATE_KV(v2, insert, 3, 3);
    auto v4 = MUTATE_KV(v3, insert, 5, 5);
    ASSERT_CONTAINS(v4, 1);
    ASSERT_CONTAINS(v4, 2);
    ASSERT_CONTAINS(v4, 3);
    ASSERT_CONTAINS(v4, 5);

    // Update on existing key updates value.
    auto v5 = MUTATE_KV(v4, update_if_exists, 1, [](int x) { return x + 1; });
    ASSERT_CONTAINS(v5, 1);
    auto i5 = SEARCH(v5, find, 1);
    ASSERT_VALUE_EQ(i5, 2);

    // Update to insert at beginning does nothing.
    auto v6 = MUTATE_KV(v4, update_if_exists, 0, [](int x) { return x + 1; });
    ASSERT_NOT_CONTAINS(v6, 0);

    // Update to insert at end does nothing.
    auto v7 = MUTATE_KV(v4, update_if_exists, 6, [](int x) { return x + 1; });
    ASSERT_NOT_CONTAINS(v7, 6);

    // Update to insert in middle does nothing.
    auto v8 = MUTATE_KV(v4, update_if_exists, 4, [](int x) { return x + 1; });
    ASSERT_NOT_CONTAINS(v8, 4);
}

TEST(ImmutableMap, UpdateIfExistsViaIterator) {
    auto v0 = init_maps<int, int>();

    // Populate an initial set of values.
    auto v1 = MUTATE_KV(v0, insert, 1, 1);
    auto v2 = MUTATE_KV(v1, insert, 2, 2);
    auto v3 = MUTATE_KV(v2, insert, 3, 3);
    auto v4 = MUTATE_KV(v3, insert, 6, 6);
    ASSERT_CONTAINS(v4, 1);
    ASSERT_CONTAINS(v4, 2);
    ASSERT_CONTAINS(v4, 3);
    ASSERT_CONTAINS(v4, 6);

    // Giving the iterator for an existing element updates the value.

    auto it4_2 = SEARCH(v4, find, 2);
    auto v5 = MUTATE_IT_KV(v4, update_if_exists, it4_2, 2, [](int x) { return x + 1; });
    auto it5_2 = SEARCH(v5, find, 2);
    ASSERT_VALUE_EQ(it5_2, 3);

    // Giving end() as hint works appropriately whether hint is accurate or not.

    auto end5 = END(v5);
    auto v6 = MUTATE_IT_KV(v5, update_if_exists, end5, 2, [](int x) { return x + 1; });
    auto it6_2 = SEARCH(v6, find, 2);
    ASSERT_VALUE_EQ(it6_2, 4);

    auto end6 = END(v6);
    auto v7 = MUTATE_IT_KV(v6, update_if_exists, end6, 4, [](int x) { return x + 1; });
    ASSERT_NOT_CONTAINS(v7, 4);

    auto end7 = END(v7);
    auto v8 = MUTATE_IT_KV(v7, update_if_exists, end7, 7, [](int x) { return x + 1; });
    ASSERT_NOT_CONTAINS(v8, 7);

    // Giving hint from lower_bound works for both existing and non-existing entries.

    auto lb8_2 = SEARCH(v8, lower_bound, 2);
    auto v9 = MUTATE_IT_KV(v8, update_if_exists, lb8_2, 2, [](int x) { return x + 1; });
    auto it9_2 = SEARCH(v9, find, 2);
    ASSERT_VALUE_EQ(it9_2, 5);

    auto lb9_0 = SEARCH(v9, lower_bound, 0);
    auto v10 = MUTATE_IT_KV(v9, update_if_exists, lb9_0, 0, [](int x) { return x + 1; });
    ASSERT_NOT_CONTAINS(v10, 0);

    auto lb10_5 = SEARCH(v10, lower_bound, 5);
    auto v11 = MUTATE_IT_KV(v10, update_if_exists, lb10_5, 5, [](int x) { return x + 1; });
    ASSERT_NOT_CONTAINS(v11, 5);

    auto lb11_8 = SEARCH(v11, lower_bound, 8);
    auto v12 = MUTATE_IT_KV(v11, update_if_exists, lb11_8, 8, [](int x) { return x + 1; });
    ASSERT_NOT_CONTAINS(v12, 8);
}

TEST(ImmutableMap, Erase) {
    auto v0 = init_maps<int, int>();

    // Populate an initial set of values.
    auto v1 = MUTATE_KV(v0, insert, 1, 1);
    auto v2 = MUTATE_KV(v1, insert, 2, 2);
    auto v3 = MUTATE_KV(v2, insert, 3, 3);
    auto v4 = MUTATE_KV(v3, insert, 5, 5);
    ASSERT_CONTAINS(v4, 1);
    ASSERT_CONTAINS(v4, 2);
    ASSERT_CONTAINS(v4, 3);
    ASSERT_CONTAINS(v4, 5);

    // Erase on existing key removes value.
    auto v5 = MUTATE_K(v4, erase, 1);
    ASSERT_NOT_CONTAINS(v5, 1);

    // Erase on non-existent key does nothing.
    auto v6 = MUTATE_K(v5, erase, 0);
    ASSERT_NOT_CONTAINS(v6, 0);
    ASSERT_CONTAINS(v6, 2);
    ASSERT_CONTAINS(v6, 3);
    ASSERT_CONTAINS(v6, 5);
}

TEST(ImmutableMap, EraseViaIterator) {
    auto v0 = init_maps<int, int>();

    // Populate an initial set of values.
    auto v1 = MUTATE_KV(v0, insert, 1, 1);
    auto v2 = MUTATE_KV(v1, insert, 2, 2);
    auto v3 = MUTATE_KV(v2, insert, 3, 3);
    auto v4 = MUTATE_KV(v3, insert, 5, 5);
    auto v5 = MUTATE_KV(v4, insert, 6, 6);
    ASSERT_CONTAINS(v5, 1);
    ASSERT_CONTAINS(v5, 2);
    ASSERT_CONTAINS(v5, 3);
    ASSERT_CONTAINS(v5, 5);
    ASSERT_CONTAINS(v5, 6);

    // Giving the iterator for an existing element erases the value.

    auto it5_2 = SEARCH(v5, find, 2);
    auto v6 = MUTATE_IT_K(v5, erase, it5_2, 2);
    ASSERT_NOT_CONTAINS(v6, 2);

    // Giving end() as hint works appropriately whether hint is accurate or not.

    auto end6 = END(v6);
    auto v7 = MUTATE_IT_K(v6, erase, end6, 3);
    ASSERT_NOT_CONTAINS(v7, 3);

    auto end7 = END(v7);
    auto v8 = MUTATE_IT_K(v7, erase, end7, 4);
    ASSERT_NOT_CONTAINS(v8, 4);

    auto end8 = END(v8);
    auto v9 = MUTATE_IT_K(v8, erase, end8, 7);
    ASSERT_NOT_CONTAINS(v9, 7);

    // Giving hint from lower_bound works for both existing and non-existing entries.

    auto lb9_5 = SEARCH(v9, lower_bound, 5);
    auto v10 = MUTATE_IT_K(v9, erase, lb9_5, 5);
    ASSERT_NOT_CONTAINS(v10, 5);

    auto lb10_0 = SEARCH(v10, lower_bound, 0);
    auto v11 = MUTATE_IT_K(v10, erase, lb10_0, 0);

    auto lb11_4 = SEARCH(v10, lower_bound, 4);
    auto v12 = MUTATE_IT_K(v11, erase, lb11_4, 4);

    auto lb12_7 = SEARCH(v12, lower_bound, 7);
    auto v13 = MUTATE_IT_K(v12, erase, lb12_7, 7);

    ASSERT_CONTAINS(v13, 1);
    ASSERT_CONTAINS(v13, 6);
    ASSERT_NOT_CONTAINS(v13, 0);
    ASSERT_NOT_CONTAINS(v13, 2);
    ASSERT_NOT_CONTAINS(v13, 3);
    ASSERT_NOT_CONTAINS(v13, 4);
    ASSERT_NOT_CONTAINS(v13, 5);
    ASSERT_NOT_CONTAINS(v13, 7);
}

TEST(ImmutableMap, ExclusiveOwnership) {
    immutable::map<int, int> v0;

    auto v1 = v0.set(1, 1);
    auto v2 = v1.set(2, 2);
    auto v3 = v2.set(3, 3);

    ASSERT_TRUE(v1.contains(1));
    ASSERT_TRUE(v2.contains(1));
    ASSERT_TRUE(v3.contains(1));

    // Claiming exclusive ownership over v3 means v3 will no longer be valid after mutation, but
    // older versions should be unperturbed.
    auto v4 = std::move(v3).erase(1);
    ASSERT_TRUE(v1.contains(1));
    ASSERT_TRUE(v2.contains(1));
    ASSERT_FALSE(v4.contains(1));
}

TEST(ImmutableSet, Basic) {
    // Insert some values and verify that the data structure is behaving as expected
    immutable::set<int> v0;
    auto v1 = v0.insert(1);
    // Record the iterator for the key '1', verify that this doesn't change after performing
    // more inserts
    auto v1it = v1.find(1);

    // Create distinct branches of the history from v1. v0 and v1 should be unaffected
    auto v2 = v1.insert(2);
    auto v3 = v1.insert(3);

    // Verify that values are as expected
    ASSERT_EQ(v0.size(), 0);

    ASSERT_EQ(v1.size(), 1);
    ASSERT_TRUE(v1.contains(1));
    ASSERT_EQ(*v1.find(1), 1);
    ASSERT_FALSE(v1.contains(2));
    ASSERT_FALSE(v1.contains(3));

    ASSERT_EQ(v2.size(), 2);
    ASSERT_TRUE(v2.contains(1));
    ASSERT_EQ(*v2.find(1), 1);
    ASSERT_TRUE(v2.contains(2));
    ASSERT_EQ(*v2.find(2), 2);
    ASSERT_FALSE(v2.contains(3));

    ASSERT_EQ(v3.size(), 2);
    ASSERT_TRUE(v3.contains(1));
    ASSERT_EQ(*v3.find(1), 1);
    ASSERT_TRUE(v3.contains(3));
    ASSERT_EQ(*v3.find(3), 3);
    ASSERT_FALSE(v3.contains(2));

    // Verify that v1's iterator did not change
    ASSERT_EQ(v1.find(1), v1it);

    // Verify that erase works as expected, and preserves history.
    auto v4 = v3.erase(1).erase(3);
    ASSERT_EQ(v4.size(), 0);
    ASSERT_FALSE(v4.contains(1));
    ASSERT_FALSE(v4.contains(2));
    ASSERT_FALSE(v4.contains(3));
    ASSERT_EQ(v3.size(), 2);
    ASSERT_TRUE(v3.contains(1));
    ASSERT_TRUE(v3.contains(3));
    ASSERT_EQ(v2.size(), 2);
    ASSERT_TRUE(v2.contains(1));
    ASSERT_TRUE(v2.contains(2));
    ASSERT_EQ(v1.size(), 1);
    ASSERT_TRUE(v1.contains(1));

    ensureContainerInvariants({v0, v1, v2, v3, v4});
}

TEST(ImmutableSet, UserDefinedType) {
    immutable::set<UserDefinedKey> v0;
    auto v1 = v0.insert(UserDefinedKey(1));
    ASSERT_NE(v1.find(UserDefinedKey(1)), v1.end());
    ASSERT_EQ(*v1.find(UserDefinedKey(1)), UserDefinedKey(1));

    ensureContainerInvariants({v0, v1});
}

TEST(ImmutableSet, IncomparableType) {
    immutable::set<Incomparable, CompareIncomparable> v0;
    auto v1 = v0.insert(Incomparable(1));
    ASSERT_TRUE(v1.contains(Incomparable(1)));

    ensureContainerInvariants(v0, CompareIncomparable{});
    ensureContainerInvariants(v1, CompareIncomparable{});
}

TEST(ImmutableSet, HeterogeneousLookup) {
    immutable::set<std::string, StringCompare> v0;
    auto v1 = v0.insert("str");

    // Lookup using StringData without the need to convert to string.
    ASSERT_NE(v1.find("str"_sd), v1.end());

    ensureContainerInvariants({v0, v1});
}

TEST(ImmutableSet, Bounds) {
    immutable::set<int> set;
    constexpr int numKeys = 100;
    for (int i = 0; i < numKeys; ++i) {
        set = set.insert(2 * i);
        ensureContainerInvariants(set);
    }

    for (int i = 0; i < numKeys - 1; ++i) {
        auto lowerExact = set.lower_bound(2 * i);
        ASSERT(lowerExact != set.end() && *lowerExact == 2 * i);
        auto upperExact = set.upper_bound(2 * i);
        ASSERT(upperExact != set.end() && *upperExact == 2 * (i + 1));

        auto lowerNear = set.lower_bound(2 * i + 1);
        ASSERT(lowerNear != set.end() && *lowerNear == 2 * (i + 1));
        auto upperNear = set.upper_bound(2 * i + 1);
        ASSERT(upperNear != set.end() && *upperNear == 2 * (i + 1));
    }
}

TEST(ImmutableSet, Iteration) {
    immutable::set<int> set;
    constexpr int numKeys = 100;
    for (int i = 0; i < numKeys; ++i) {
        set = set.insert(2 * i);
        ensureContainerInvariants(set);
    }

    auto set0 = set;
    auto it0 = set0.begin();

    for (int i = 0; i < numKeys; ++i) {
        set = set.insert(2 * i + 1);
        ensureContainerInvariants(set);
    }

    auto set1 = set;
    auto it1 = set1.begin();

    for (int i = 0; i < numKeys; ++i) {
        ASSERT_NE(it0, set0.end());
        ASSERT_EQ(*it0, 2 * i);
        ++it0;

        ASSERT_NE(it1, set1.end());
        ASSERT_EQ(*it1, 2 * i);
        ++it1;

        ASSERT_NE(it1, set1.end());
        ASSERT_EQ(*it1, 2 * i + 1);
        ++it1;
    }
    ASSERT_EQ(it0, set0.end());
    ASSERT_EQ(it1, set1.end());
}

TEST(ImmutableSet, Insert) {
    auto v0 = init_sets<int>();

    // Populate an initial set of values.
    auto v1 = MUTATE_K(v0, insert, 1);
    auto v2 = MUTATE_K(v1, insert, 2);
    auto v3 = MUTATE_K(v2, insert, 3);
    auto v4 = MUTATE_K(v3, insert, 5);
    ASSERT_CONTAINS(v4, 1);
    ASSERT_CONTAINS(v4, 2);
    ASSERT_CONTAINS(v4, 3);
    ASSERT_CONTAINS(v4, 5);

    // Insert of existing key is a noop.
    auto v5 = MUTATE_K(v4, insert, 1);  // enforces no duplicates
    ASSERT_CONTAINS(v5, 1);

    // Insert at beginning works.
    auto v6 = MUTATE_K(v4, insert, 0);
    ASSERT_CONTAINS(v6, 0);

    // Insert at end works.
    auto v7 = MUTATE_K(v4, insert, 6);
    ASSERT_CONTAINS(v7, 6);

    // Insert in middle works.
    auto v8 = MUTATE_K(v4, insert, 4);
    ASSERT_CONTAINS(v8, 4);
}

TEST(ImmutableSet, InsertViaIterator) {
    auto v0 = init_sets<int>();

    // Populate an initial set of values.
    auto v1 = MUTATE_K(v0, insert, 1);
    auto v2 = MUTATE_K(v1, insert, 2);
    auto v3 = MUTATE_K(v2, insert, 3);
    auto v4 = MUTATE_K(v3, insert, 6);
    ASSERT_CONTAINS(v4, 1);
    ASSERT_CONTAINS(v4, 2);
    ASSERT_CONTAINS(v4, 3);
    ASSERT_CONTAINS(v4, 6);

    // Giving the iterator for an existing element does a noop.

    auto it4_2 = SEARCH(v4, find, 2);
    auto v5 = MUTATE_IT_K(v4, insert, it4_2, 2);  // enforces no duplicates
    ASSERT_CONTAINS(v5, 2);

    // Giving end() as hint works appropriately whether hint is accurate or not.

    auto end5 = END(v5);
    auto v6 = MUTATE_IT_K(v5, insert, end5, 2);  // enforces no duplicates
    ASSERT_CONTAINS(v6, 2);

    auto end6 = END(v6);
    auto v7 = MUTATE_IT_K(v6, insert, end6, 4);
    ASSERT_CONTAINS(v7, 4);

    auto end7 = END(v7);
    auto v8 = MUTATE_IT_K(v7, insert, end7, 7);
    ASSERT_CONTAINS(v8, 7);

    // Giving hint from lower_bound works for both existing and new entries.

    auto lb8_2 = SEARCH(v8, lower_bound, 2);
    auto v9 = MUTATE_IT_K(v8, insert, lb8_2, 2);  // enforces no duplicates
    ASSERT_CONTAINS(v9, 2);

    auto lb9_0 = SEARCH(v9, lower_bound, 0);
    auto v10 = MUTATE_IT_K(v9, insert, lb9_0, 0);
    ASSERT_CONTAINS(v10, 0);

    auto lb10_5 = SEARCH(v10, lower_bound, 5);
    auto v11 = MUTATE_IT_K(v10, insert, lb10_5, 5);
    ASSERT_CONTAINS(v11, 5);

    auto lb11_8 = SEARCH(v11, lower_bound, 8);
    auto v12 = MUTATE_IT_K(v11, insert, lb11_8, 8);
    ASSERT_CONTAINS(v12, 8);
}

TEST(ImmutableSet, Erase) {
    auto v0 = init_sets<int>();

    // Populate an initial set of values.
    auto v1 = MUTATE_K(v0, insert, 1);
    auto v2 = MUTATE_K(v1, insert, 2);
    auto v3 = MUTATE_K(v2, insert, 3);
    auto v4 = MUTATE_K(v3, insert, 5);
    ASSERT_CONTAINS(v4, 1);
    ASSERT_CONTAINS(v4, 2);
    ASSERT_CONTAINS(v4, 3);
    ASSERT_CONTAINS(v4, 5);

    // Erase on existing key removes value.
    auto v5 = MUTATE_K(v4, erase, 1);
    ASSERT_NOT_CONTAINS(v5, 1);

    // Erase on non-existent key does nothing.
    auto v6 = MUTATE_K(v5, erase, 0);
    ASSERT_NOT_CONTAINS(v6, 0);
    ASSERT_CONTAINS(v6, 2);
    ASSERT_CONTAINS(v6, 3);
    ASSERT_CONTAINS(v6, 5);
}

TEST(ImmutableSet, EraseViaIterator) {
    auto v0 = init_sets<int>();

    // Populate an initial set of values.
    auto v1 = MUTATE_K(v0, insert, 1);
    auto v2 = MUTATE_K(v1, insert, 2);
    auto v3 = MUTATE_K(v2, insert, 3);
    auto v4 = MUTATE_K(v3, insert, 5);
    auto v5 = MUTATE_K(v4, insert, 6);
    ASSERT_CONTAINS(v5, 1);
    ASSERT_CONTAINS(v5, 2);
    ASSERT_CONTAINS(v5, 3);
    ASSERT_CONTAINS(v5, 5);
    ASSERT_CONTAINS(v5, 6);

    // Giving the iterator for an existing element erases the value.

    auto it5_2 = SEARCH(v5, find, 2);
    auto v6 = MUTATE_IT_K(v5, erase, it5_2, 2);
    ASSERT_NOT_CONTAINS(v6, 2);

    // Giving end() as hint works appropriately whether hint is accurate or not.

    auto end6 = END(v6);
    auto v7 = MUTATE_IT_K(v6, erase, end6, 3);
    ASSERT_NOT_CONTAINS(v7, 3);

    auto end7 = END(v7);
    auto v8 = MUTATE_IT_K(v7, erase, end7, 4);
    ASSERT_NOT_CONTAINS(v8, 4);

    auto end8 = END(v8);
    auto v9 = MUTATE_IT_K(v8, erase, end8, 7);
    ASSERT_NOT_CONTAINS(v9, 7);

    // Giving hint from lower_bound works for both existing and non-existing entries.

    auto lb9_5 = SEARCH(v9, lower_bound, 5);
    auto v10 = MUTATE_IT_K(v9, erase, lb9_5, 5);
    ASSERT_NOT_CONTAINS(v10, 5);

    auto lb10_0 = SEARCH(v10, lower_bound, 0);
    auto v11 = MUTATE_IT_K(v10, erase, lb10_0, 0);

    auto lb11_4 = SEARCH(v10, lower_bound, 4);
    auto v12 = MUTATE_IT_K(v11, erase, lb11_4, 4);

    auto lb12_7 = SEARCH(v12, lower_bound, 7);
    auto v13 = MUTATE_IT_K(v12, erase, lb12_7, 7);

    ASSERT_CONTAINS(v13, 1);
    ASSERT_CONTAINS(v13, 6);
    ASSERT_NOT_CONTAINS(v13, 0);
    ASSERT_NOT_CONTAINS(v13, 2);
    ASSERT_NOT_CONTAINS(v13, 3);
    ASSERT_NOT_CONTAINS(v13, 4);
    ASSERT_NOT_CONTAINS(v13, 5);
    ASSERT_NOT_CONTAINS(v13, 7);
}

TEST(ImmutableSet, ExclusiveOwnership) {
    immutable::set<int> v0;

    auto v1 = v0.insert(1);
    auto v2 = v1.insert(2);
    auto v3 = v2.insert(3);

    ASSERT_TRUE(v1.contains(1));
    ASSERT_TRUE(v2.contains(1));
    ASSERT_TRUE(v3.contains(1));

    // Claiming exclusive ownership over v3 means v3 will no longer be valid after mutation, but
    // older versions should be unperturbed.
    auto v4 = std::move(v3).erase(1);
    ASSERT_TRUE(v1.contains(1));
    ASSERT_TRUE(v2.contains(1));
    ASSERT_FALSE(v4.contains(1));
}

}  // namespace
}  // namespace mongo
