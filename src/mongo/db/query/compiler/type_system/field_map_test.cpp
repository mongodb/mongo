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
using StringMap = FieldMap<std::string>;

/// Counts the copies made of the value.
struct Copied {
    Copied() = default;
    explicit Copied(int value) : value(value) {}
    Copied(const Copied& other) : value(other.value), copies(other.copies + 1) {}
    Copied(Copied&&) = default;
    Copied& operator=(const Copied& other) {
        value = other.value;
        copies = other.copies + 1;
        return *this;
    }
    Copied& operator=(Copied&&) = default;

    bool operator==(const Copied& other) const {
        return value == other.value;
    }

    int value = 0;
    int copies = 0;
};

/// Joins the two values, so that a test can see exactly which values were combined.
std::string concat(const std::string& lhs, const std::string& rhs) {
    return lhs + "," + rhs;
}

template <typename T>
std::vector<std::string> keys(const FieldMap<T>& map) {
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

TEST(FieldMapTest, OwnedMapIsNotShared) {
    Map map;
    map.set("x"sv, 1);
    ASSERT_FALSE(map.isShared());
}

TEST(FieldMapTest, CopiedMapIsShared) {
    Map map;
    map.set("x"sv, 1);
    auto copied = map;
    ASSERT_TRUE(map.isShared());
}

TEST(FieldMapTest, UpdateTransformsValue) {
    StringMap map;
    map.set("x"sv, "number");
    map.update("x"sv, "implied", [](std::string held) { return concat(held, "narrowed"); });
    ASSERT_EQ(*map.find("x"sv), "number,narrowed");
}

TEST(FieldMapTest, UpdateUsesImpliedWhenThereIsNoValue) {
    StringMap map;
    map.set("x"sv, "number");
    map.update("y"sv, "implied", [](std::string held) { return concat(held, "narrowed"); });
    ASSERT_EQ(*map.find("y"sv), "implied,narrowed");
}

TEST(FieldMapTest, UpdateRemovesKeyWhenResultIsImplied) {
    StringMap map;
    map.set("x"sv, "number");
    map.set("y"sv, "date");
    map.update("x"sv, "implied", [](std::string) { return std::string{"implied"}; });
    ASSERT_FALSE(map.find("x"sv));
    ASSERT_EQ(*map.find("y"sv), "date");
}

TEST(FieldMapTest, UpdateDoesNotCloneWhenAbsentKeyStaysImplied) {
    StringMap map;
    map.set("x"sv, "number");
    auto shared = map;
    map.update("y"sv, "implied", [](std::string held) { return held; });
    ASSERT_FALSE(map.find("y"sv));
    ASSERT_TRUE(map.sameStorage(shared));
}

TEST(FieldMapTest, UpdateOnEmptyMapStaysEmptyWhenResultIsImplied) {
    StringMap map;
    map.update("x"sv, "implied", [](std::string held) { return held; });
    ASSERT_TRUE(map.empty());
    ASSERT_TRUE(map.sameStorage(StringMap{}));
}

TEST(FieldMapTest, UpdateOnSharedStorageClonesItAndLeavesOtherOwner) {
    StringMap map;
    map.set("x"sv, "number");
    auto shared = map;
    map.update("x"sv, "implied", [](std::string held) { return concat(held, "narrowed"); });
    ASSERT_FALSE(map.sameStorage(shared));
    ASSERT_EQ(*shared.find("x"sv), "number");
    ASSERT_EQ(*map.find("x"sv), "number,narrowed");
}

TEST(FieldMapTest, UpdateOnOwnedStorageDoesNotCopy) {
    StringMap map;
    map.set("x"sv, "number");
    const void* storage = map.getStorage_forTest();
    map.update("x"sv, "implied", [](std::string held) { return concat(held, "narrowed"); });
    ASSERT_EQ(map.getStorage_forTest(), storage);
}

TEST(FieldMapTest, UpdateRemovingLastValueReleasesStorage) {
    StringMap map;
    map.set("x"sv, "number");
    map.update("x"sv, "implied", [](std::string) { return std::string{"implied"}; });
    ASSERT_TRUE(map.empty());
    ASSERT_TRUE(map.sameStorage(StringMap{}));
}

TEST(FieldMapTest, MergeCombinesValuesHeldByBothMaps) {
    StringMap lhs;
    lhs.set("x"sv, "number");
    StringMap rhs;
    rhs.set("x"sv, "string");
    lhs.merge(rhs, "lhsImplied", "rhsImplied", "merged", concat);
    ASSERT_EQ(*lhs.find("x"sv), "number,string");
}

TEST(FieldMapTest, MergeUsesImpliedValueOfWhicheverSideLacksKey) {
    StringMap lhs;
    lhs.set("x"sv, "number");
    StringMap rhs;
    rhs.set("y"sv, "string");
    lhs.merge(rhs, "lhsImplied", "rhsImplied", "merged", concat);
    ASSERT_EQ(*lhs.find("x"sv), "number,rhsImplied");
    ASSERT_EQ(*lhs.find("y"sv), "lhsImplied,string");
}

TEST(FieldMapTest, MergeRemovesKeyWhoseResultIsMergedImplied) {
    StringMap lhs;
    lhs.set("x"sv, "number");
    lhs.set("y"sv, "date");
    StringMap rhs;
    rhs.set("x"sv, "string");
    lhs.merge(rhs, "lhsImplied", "rhsImplied", "number,string", concat);
    ASSERT_FALSE(lhs.find("x"sv));
    ASSERT_EQ(*lhs.find("y"sv), "date,rhsImplied");
}

TEST(FieldMapTest, MergeKeepsKeysInOrder) {
    StringMap lhs;
    lhs.set("b"sv, "number");
    lhs.set("d"sv, "number");
    StringMap rhs;
    rhs.set("a"sv, "number");
    rhs.set("c"sv, "number");
    rhs.set("e"sv, "number");
    lhs.merge(rhs, "lhsImplied", "rhsImplied", "merged", concat);
    ASSERT_EQ(keys(lhs), (std::vector<std::string>{"a", "b", "c", "d", "e"}));
}

TEST(FieldMapTest, MergeOnSharedStorageClonesItAndLeavesOtherOwner) {
    StringMap lhs;
    lhs.set("x"sv, "number");
    auto shared = lhs;
    StringMap rhs;
    rhs.set("x"sv, "string");
    lhs.merge(rhs, "lhsImplied", "rhsImplied", "merged", concat);
    ASSERT_FALSE(lhs.sameStorage(shared));
    ASSERT_EQ(*shared.find("x"sv), "number");
    ASSERT_EQ(*lhs.find("x"sv), "number,string");
}

TEST(FieldMapTest, MergeOnOwnedStorageDoesNotCopy) {
    StringMap lhs;
    lhs.set("x"sv, "number");
    const void* storage = lhs.getStorage_forTest();
    StringMap rhs;
    rhs.set("x"sv, "string");
    lhs.merge(rhs, "lhsImplied", "rhsImplied", "merged", concat);
    ASSERT_EQ(lhs.getStorage_forTest(), storage);
}

TEST(FieldMapTest, MergeEmptyingMapReleasesStorage) {
    StringMap lhs;
    lhs.set("x"sv, "number");
    StringMap rhs;
    rhs.set("x"sv, "string");
    lhs.merge(rhs, "lhsImplied", "rhsImplied", "number,string", concat);
    ASSERT_TRUE(lhs.empty());
    ASSERT_TRUE(lhs.sameStorage(StringMap{}));
}

TEST(FieldMapTest, MergeOfEmptyMapsHoldsNothing) {
    StringMap lhs;
    lhs.merge(StringMap{}, "lhsImplied", "rhsImplied", "merged", concat);
    ASSERT_TRUE(lhs.empty());
    ASSERT_TRUE(lhs.sameStorage(StringMap{}));
}

TEST(FieldMapTest, MergeLeavesNoEmptyStorageWhenCombineThrows) {
    StringMap lhs;
    lhs.set("x"sv, "");
    StringMap rhs;
    rhs.set("y"sv, "");
    // Combiner which results in the keys being erased (remove is the implied value).
    auto combine = [](std::string l, const std::string& r) {
        uassert(ErrorCodes::InternalError, "boom", l != "lhsImplied");
        return std::string{"remove"};
    };
    ASSERT_THROWS_CODE(lhs.merge(rhs, "lhsImplied", "rhsImplied", "remove", combine),
                       DBException,
                       ErrorCodes::InternalError);
    // The throw is at the second key (first one already erased).
    // So the map should be empty at this point.
    ASSERT_TRUE(lhs.empty());
}

TEST(FieldMapTest, MergeWithSelfCombinesEachValueWithItself) {
    Map value;
    value.set("y"sv, 1);
    MapOfMap map;
    map.set("x"sv, value);
    map.merge(map, Map{}, Map{}, Map{}, [&value](Map lhs, const Map& rhs) {
        ASSERT_EQ(lhs, rhs);
        ASSERT_EQ(lhs, value);
        ASSERT_EQ(rhs, value);
        return value;
    });
}

TEST(FieldMapTest, UpdateOnOwnedStorageHandsOverHeldValueWithoutCopying) {
    FieldMap<Copied> map;
    map.set("x"sv, Copied{1});
    int copies = -1;
    map.update("x"sv, Copied{0}, [&](Copied held) {
        copies = held.copies;
        return Copied{2};
    });
    ASSERT_EQ(copies, 0);
}

TEST(FieldMapTest, UpdateOnSharedStorageCopiesHeldValueExactlyOnce) {
    FieldMap<Copied> map;
    map.set("x"sv, Copied{1});
    auto shared = map;
    int copies = -1;
    map.update("x"sv, Copied{0}, [&](Copied held) {
        copies = held.copies;
        return Copied{2};
    });
    ASSERT_EQ(copies, 1);
}

TEST(FieldMapTest, MergeHandsOverHeldValueWithoutCopying) {
    FieldMap<Copied> lhs;
    lhs.set("x"sv, Copied{1});
    FieldMap<Copied> rhs;
    rhs.set("x"sv, Copied{2});
    int copies = -1;
    lhs.merge(rhs, Copied{0}, Copied{0}, Copied{-1}, [&](Copied l, Copied r) {
        copies = l.copies;
        return Copied{3};
    });
    ASSERT_EQ(copies, 0);
}


}  // namespace

}  // namespace mongo::pipeline::type_system
