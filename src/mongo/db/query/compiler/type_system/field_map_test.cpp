// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/compiler/type_system/field_map.h"

#include "mongo/unittest/unittest.h"

#include <string>
#include <vector>

namespace mongo::pipeline::type_system {

using namespace std::literals::string_view_literals;

namespace {

using Map = FieldMap<int>;
using MapOfMap = FieldMap<FieldMap<int>>;

std::vector<std::string> keys(const Map& map) {
    std::vector<std::string> result;
    for (const auto& [key, value] : map) {
        result.push_back(key);
    }
    return result;
}

TEST(FieldMapTest, DefaultConstructedMapIsEmpty) {
    Map map;
    ASSERT_TRUE(map.empty());
    ASSERT_EQ(map.size(), 0u);
    ASSERT_TRUE(map.begin() == map.end());
    ASSERT_FALSE(map.find("x"sv));
}

TEST(FieldMapTest, SetStoresValue) {
    Map map;
    map.set("x"sv, 1);
    ASSERT_FALSE(map.empty());
    ASSERT_EQ(map.size(), 1u);
    ASSERT_EQ(*map.find("x"sv), 1);
    ASSERT_FALSE(map.find("y"sv));
}

TEST(FieldMapTest, SetOverwritesExistingValue) {
    Map map;
    map.set("x"sv, 1);
    map.set("x"sv, 2);
    ASSERT_EQ(map.size(), 1u);
    ASSERT_EQ(*map.find("x"sv), 2);
}

TEST(FieldMapTest, SetToSameValueIsNoop) {
    Map map;
    map.set("x"sv, 1);
    auto shared = map;
    map.set("x"sv, 1);
    ASSERT_TRUE(map.sameStorage(shared));
}

TEST(FieldMapTest, SetUsesCopyOnWrite) {
    Map map;
    map.set("x"sv, 1);
    auto shared = map;
    map.set("x"sv, 2);
    ASSERT_FALSE(map.sameStorage(shared));
    ASSERT_EQ(*shared.find("x"sv), 1);
    ASSERT_EQ(*map.find("x"sv), 2);
}

TEST(FieldMapTest, MoveMovesStorage) {
    Map map;
    map.set("x"sv, 1);
    auto storage = map.getStorage_forTest();
    Map moved = std::move(map);

    ASSERT_TRUE(map.empty());
    ASSERT_TRUE(map.sameStorage(Map{}));

    ASSERT_FALSE(moved.empty());
    ASSERT_EQ(moved.getStorage_forTest(), storage);
    ASSERT_EQ(*moved.find("x"), 1);
}

TEST(FieldMapTest, SuccessiveSetsDoNotCopy) {
    Map map;
    map.set("x"sv, 1);
    auto shared = map;
    map.set("a"sv, 2);
    const void* cloned = map.getStorage_forTest();
    ASSERT_NE(cloned, shared.getStorage_forTest());
    map.set("b"sv, 3);
    ASSERT_EQ(map.getStorage_forTest(), cloned);
    map.set("c"sv, 4);
    ASSERT_EQ(map.getStorage_forTest(), cloned);
}

TEST(FieldMapTest, SetsDoNotModifyShared) {
    Map map;
    map.set("a"sv, 1);
    auto shared = map;
    ASSERT_EQ(*map.find("a"sv), 1);
    ASSERT_EQ(*shared.find("a"sv), 1);
    map.set("b"sv, 2);
    // map has {a, b}, shared has only {a}.
    ASSERT_EQ(*map.find("a"sv), 1);
    ASSERT_EQ(*shared.find("a"sv), 1);
    ASSERT_EQ(*map.find("b"sv), 2);
    ASSERT_FALSE(shared.find("b"sv));
}

TEST(FieldMapTest, SetOnOwnedStorageDoesNotCopy) {
    Map map;
    map.set("x"sv, 1);
    const void* storage = map.getStorage_forTest();
    map.set("y"sv, 2);
    ASSERT_EQ(map.getStorage_forTest(), storage);
    ASSERT_EQ(map.size(), 2u);
}

TEST(FieldMapTest, EraseRemovesValue) {
    Map map;
    map.set("x"sv, 1);
    map.set("y"sv, 2);
    map.erase("x"sv);
    ASSERT_FALSE(map.find("x"sv));
    ASSERT_EQ(*map.find("y"sv), 2);
    ASSERT_EQ(map.size(), 1u);
}

TEST(FieldMapTest, EraseOfAbsentKeyIsNoop) {
    Map map;
    map.set("x"sv, 1);
    auto shared = map;
    map.erase("y"sv);
    ASSERT_TRUE(map.sameStorage(shared));
}

TEST(FieldMapTest, EraseUsesCopyOnWrite) {
    Map map;
    map.set("x"sv, 1);
    map.set("y"sv, 2);
    auto shared = map;
    map.erase("x"sv);
    ASSERT_FALSE(map.sameStorage(shared));
    ASSERT_EQ(*shared.find("x"sv), 1);
    ASSERT_FALSE(map.find("x"sv));
}

TEST(FieldMapTest, ErasingLastValueReleasesStorage) {
    Map map;
    map.set("x"sv, 1);
    map.erase("x"sv);
    ASSERT_TRUE(map.empty());
    ASSERT_TRUE(map.sameStorage(Map{}));
}

TEST(FieldMapTest, ErasingLastValueOnSharedDoesNotAllocateStorage) {
    Map map;
    map.set("x"sv, 1);
    Map shared = map;
    shared.erase("x"sv);
    ASSERT_FALSE(map.empty());
    ASSERT_TRUE(shared.empty());
    ASSERT_TRUE(shared.sameStorage(Map{}));
}

TEST(FieldMapTest, IteratesInKeyOrder) {
    Map map;
    map.set("b"sv, 2);
    map.set("a"sv, 1);
    map.set("c"sv, 3);
    ASSERT_EQ(keys(map), (std::vector<std::string>{"a", "b", "c"}));
}

TEST(FieldMapTest, MapsHoldingSameValuesCompareEqual) {
    Map one;
    one.set("a"sv, 1);
    one.set("b"sv, 2);
    Map other;
    other.set("b"sv, 2);
    other.set("a"sv, 1);
    ASSERT_FALSE(one.sameStorage(other));
    ASSERT_EQ(one, other);
}

TEST(FieldMapTest, MapsHoldingDifferentValuesCompareUnequal) {
    Map one;
    one.set("a"sv, 1);
    Map other;
    other.set("a"sv, 2);
    ASSERT_NE(one, other);
}

TEST(FieldMapTest, MapsHoldingDifferentKeysCompareUnequal) {
    Map one;
    one.set("a"sv, 1);
    Map other;
    other.set("b"sv, 1);
    ASSERT_NE(one, other);
}

TEST(FieldMapTest, CopySharesStorage) {
    Map map;
    map.set("x"sv, 1);
    auto copied = map;
    ASSERT_TRUE(copied.sameStorage(map));
    ASSERT_EQ(copied, map);
}

TEST(FieldMapTest, EmptyMapsCompareEqual) {
    ASSERT_EQ(Map{}, Map{});
}

TEST(FieldMapTest, NestedMapsWork) {
    MapOfMap map;
    map.set("x"sv, Map{});
    auto x = *map.find("x");
    x.set("y", 1);
    ASSERT_FALSE(map.find("x")->find("y"));
    // Set the new 'x' into the map.
    map.set("x", x);
    ASSERT_EQ(*map.find("x")->find("y"), 1);
}

}  // namespace

}  // namespace mongo::pipeline::type_system
