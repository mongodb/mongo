/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#include "mongo/db/exec/sbe/values/sort_spec.h"
#include "mongo/db/exec/sbe/values/util.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/query/collation/collator_interface_mock.h"
#include "mongo/db/query/sbe_stage_builder_test_fixture.h"
#include "mongo/unittest/unittest.h"

namespace mongo::sbe {

class SbeValueTest : public SbeStageBuilderTestFixture {};

TEST_F(SbeValueTest, CompareTwoObjectsWithSubobjectsOfDifferentTypesWithDifferentFieldNames) {
    auto lhsObj = BSON("a" << kMinBSONKey);
    auto rhsObj = BSON("a" << BSON("c" << 1));
    auto [lhsTag, lhsVal] = value::copyValue(value::TypeTags::bsonObject,
                                             value::bitcastFrom<const char*>(lhsObj.objdata()));
    value::ValueGuard lhsGuard{lhsTag, lhsVal};

    auto [rhsTag, rhsVal] = value::copyValue(value::TypeTags::bsonObject,
                                             value::bitcastFrom<const char*>(rhsObj.objdata()));
    value::ValueGuard rhsGuard{rhsTag, rhsVal};

    // LHS should compare less than RHS.
    auto [cmpTag, cmpVal] = value::compareValue(lhsTag, lhsVal, rhsTag, rhsVal);
    ASSERT_EQ(cmpTag, value::TypeTags::NumberInt32);
    ASSERT_EQ(value::bitcastTo<int32_t>(cmpVal), -1);
}

TEST_F(SbeValueTest, CompareTwoArraySets) {
    using ValueFnType = void(value::ArraySet*);
    using AssertFnType = void(value::TypeTags, value::Value, value::TypeTags, value::Value);

    auto arraySetComparisonTestGenFn = [](std::function<ValueFnType> lhsValueGenFn,
                                          std::function<ValueFnType> rhsValueGenFn,
                                          std::function<AssertFnType> assertFn) {
        auto [lhsTag, lhsVal] = value::makeNewArraySet();
        value::ValueGuard lhsGuard{lhsTag, lhsVal};
        auto lhsView = value::getArraySetView(lhsVal);
        lhsValueGenFn(lhsView);

        auto [rhsTag, rhsVal] = value::makeNewArraySet();
        value::ValueGuard rhsGuard{rhsTag, rhsVal};
        auto rhsView = value::getArraySetView(rhsVal);
        rhsValueGenFn(rhsView);

        assertFn(lhsTag, lhsVal, rhsTag, rhsVal);
    };

    auto arraySetEqualityComparisonTestGenFn = [&](std::function<ValueFnType> lhsValueGenFn,
                                                   std::function<ValueFnType> rhsValueGenFn) {
        arraySetComparisonTestGenFn(lhsValueGenFn,
                                    rhsValueGenFn,
                                    [&](value::TypeTags lhsTag,
                                        value::Value lhsVal,
                                        value::TypeTags rhsTag,
                                        value::Value rhsVal) {
                                        ASSERT(valueEquals(lhsTag, lhsVal, rhsTag, rhsVal))
                                            << "lhs array set: " << std::make_pair(lhsTag, lhsVal)
                                            << "rhs array set: " << std::make_pair(rhsTag, rhsVal);
                                    });
    };

    auto arraySetInequalityComparisonTestGenFn = [&](std::function<ValueFnType> lhsValueGenFn,
                                                     std::function<ValueFnType> rhsValueGenFn) {
        arraySetComparisonTestGenFn(lhsValueGenFn,
                                    rhsValueGenFn,
                                    [&](value::TypeTags lhsTag,
                                        value::Value lhsVal,
                                        value::TypeTags rhsTag,
                                        value::Value rhsVal) {
                                        ASSERT(!valueEquals(lhsTag, lhsVal, rhsTag, rhsVal))
                                            << "lhs array set: " << std::make_pair(lhsTag, lhsVal)
                                            << "rhs array set: " << std::make_pair(rhsTag, rhsVal);
                                    });
    };

    auto addShortStringFn = [](value::ArraySet* set) {
        auto [rhsItemTag, rhsItemVal] = value::makeSmallString("abc"_sd);
        set->push_back(rhsItemTag, rhsItemVal);
    };
    auto addLongStringFn = [](value::ArraySet* set) {
        auto [rhsItemTag, rhsItemVal] = value::makeNewString("a long enough string"_sd);
        set->push_back(rhsItemTag, rhsItemVal);
    };
    auto addArrayFn = [](value::ArraySet* set) {
        auto bsonArr = BSON_ARRAY(1 << 2 << 3);
        auto [rhsItemTag, rhsItemVal] = value::copyValue(
            value::TypeTags::bsonArray, value::bitcastFrom<const char*>(bsonArr.objdata()));
        set->push_back(rhsItemTag, rhsItemVal);
    };
    auto addObjectFn = [](value::ArraySet* set) {
        auto bsonObj = BSON("c" << 1);
        auto [rhsItemTag, rhsItemVal] = value::copyValue(
            value::TypeTags::bsonObject, value::bitcastFrom<const char*>(bsonObj.objdata()));
        set->push_back(rhsItemTag, rhsItemVal);
    };
    auto addLongStringMultipleTimesFn = [&](value::ArraySet* set) {
        auto initSize = set->size();
        addLongStringFn(set);
        addLongStringFn(set);
        addLongStringFn(set);
        ASSERT(set->size() == initSize + 1)
            << "set: " << set << " should be of size " << initSize + 1;
    };
    auto addMultipleDecimalFn = [](value::ArraySet* set) {
        auto initSize = set->size();
        auto [rhsItemTag1, rhsItemVal1] = value::makeCopyDecimal(Decimal128{"3.14"});
        set->push_back(rhsItemTag1, rhsItemVal1);
        auto [rhsItemTag2, rhsItemVal2] = value::makeCopyDecimal(Decimal128{"2.71"});
        set->push_back(rhsItemTag2, rhsItemVal2);
        auto [rhsItemTag3, rhsItemVal3] = value::makeCopyDecimal(Decimal128{"3.14"});
        set->push_back(rhsItemTag3, rhsItemVal3);
        ASSERT(set->size() == initSize + 2)
            << "set: " << set << " should be of size " << initSize + 2;
    };

    // Compare ArraySets with single element of different (and mostly complex) types.
    arraySetEqualityComparisonTestGenFn(addShortStringFn, addShortStringFn);
    arraySetEqualityComparisonTestGenFn(addLongStringFn, addLongStringFn);
    arraySetEqualityComparisonTestGenFn(addArrayFn, addArrayFn);
    arraySetEqualityComparisonTestGenFn(addObjectFn, addObjectFn);
    arraySetEqualityComparisonTestGenFn(addMultipleDecimalFn, addMultipleDecimalFn);
    // Check whether adding a single complex type multiple times doesn't break the equality.
    arraySetEqualityComparisonTestGenFn(addLongStringMultipleTimesFn, addLongStringMultipleTimesFn);
    // Check whether the insertion into ArraySet is order agnostic.
    arraySetEqualityComparisonTestGenFn(
        [&](value::ArraySet* set) {
            addArrayFn(set);
            addMultipleDecimalFn(set);
            addObjectFn(set);
            addLongStringFn(set);
        },
        [&](value::ArraySet* set) {
            addObjectFn(set);
            addLongStringFn(set);
            addArrayFn(set);
            addMultipleDecimalFn(set);
        });

    // Check inequal ArraySets are actually not equal.
    arraySetInequalityComparisonTestGenFn(addShortStringFn, addLongStringFn);
    arraySetInequalityComparisonTestGenFn(addArrayFn, addObjectFn);
    arraySetInequalityComparisonTestGenFn(addMultipleDecimalFn, addObjectFn);
}

void insertIntoMapType(value::ValueMapType<size_t>* map,
                       value::TypeTags keyTag,
                       value::Value keyVal,
                       size_t value) {
    value::ValueGuard guard{keyTag, keyVal};
    auto [_, inserted] = map->insert({keyTag, keyVal}, value);
    if (inserted) {
        guard.reset();
    }
}

void releaseMapType(const value::ValueMapType<size_t>& map) {
    for (const auto& e : map) {
        auto [subTag, subVal] = e.first;
        value::releaseValue(subTag, subVal);
    }
}

TEST_F(SbeValueTest, CompareTwoValueMapTypes) {
    using MapType = value::ValueMapType<size_t>;
    using ValueFnType = void(MapType*);
    using AssertFnType = void(const MapType&, const MapType&);

    auto valueMapTypeComparisonTestGenFn = [](std::function<ValueFnType> lhsValueGenFn,
                                              std::function<ValueFnType> rhsValueGenFn,
                                              std::function<AssertFnType> assertFn) {
        CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kToLowerString);

        MapType lhsMap{0, value::ValueHash(&collator), value::ValueEq(&collator)};
        lhsValueGenFn(&lhsMap);

        MapType rhsMap{0, value::ValueHash(&collator), value::ValueEq(&collator)};
        rhsValueGenFn(&rhsMap);

        assertFn(lhsMap, rhsMap);

        releaseMapType(lhsMap);
        releaseMapType(rhsMap);
    };

    auto valueMapTypeEqualityComparisonTestGenFn = [&](std::function<ValueFnType> lhsValueGenFn,
                                                       std::function<ValueFnType> rhsValueGenFn) {
        valueMapTypeComparisonTestGenFn(
            lhsValueGenFn, rhsValueGenFn, [&](const MapType& lhs, const MapType& rhs) {
                ASSERT(lhs == rhs);
            });
    };

    auto valueMapTypeInequalityComparisonTestGenFn = [&](std::function<ValueFnType> lhsValueGenFn,
                                                         std::function<ValueFnType> rhsValueGenFn) {
        valueMapTypeComparisonTestGenFn(
            lhsValueGenFn, rhsValueGenFn, [&](const MapType& lhs, const MapType& rhs) {
                ASSERT(lhs != rhs);
            });
    };

    auto addShortStringKeyFn = [](MapType* map) {
        auto [rhsItemTag, rhsItemVal] = value::makeSmallString("abc"_sd);
        insertIntoMapType(map, rhsItemTag, rhsItemVal, 1);
    };
    auto addLongStringKeyFn1 = [](MapType* map) {
        auto [rhsItemTag, rhsItemVal] = value::makeNewString("a long enough string"_sd);
        insertIntoMapType(map, rhsItemTag, rhsItemVal, 2);
    };
    auto addLongStringKeyFn2 = [](MapType* map) {
        auto [rhsItemTag, rhsItemVal] = value::makeNewString("a long enough string"_sd);
        insertIntoMapType(map, rhsItemTag, rhsItemVal, 12);
    };
    auto addArrayKeyFn = [](MapType* map) {
        auto bsonArr = BSON_ARRAY(1 << 2 << 3);
        auto [rhsItemTag, rhsItemVal] = value::copyValue(
            value::TypeTags::bsonArray, value::bitcastFrom<const char*>(bsonArr.objdata()));
        insertIntoMapType(map, rhsItemTag, rhsItemVal, 3);
    };
    auto addObjectKeyFn = [](MapType* map) {
        auto bsonObj = BSON("c" << 1);
        auto [rhsItemTag, rhsItemVal] = value::copyValue(
            value::TypeTags::bsonObject, value::bitcastFrom<const char*>(bsonObj.objdata()));
        insertIntoMapType(map, rhsItemTag, rhsItemVal, 4);
    };
    auto addLongStringMultipleTimesKeyFn = [&](MapType* map) {
        auto initSize = map->size();
        addLongStringKeyFn1(map);
        addLongStringKeyFn1(map);
        addLongStringKeyFn1(map);
        ASSERT(map->size() == initSize + 1)
            << "map: " << map << " should be of size " << initSize + 1;
    };
    auto addMultipleDecimalKeyFn = [](MapType* map) {
        auto initSize = map->size();
        auto [rhsItemTag1, rhsItemVal1] = value::makeCopyDecimal(Decimal128{"3.14"});
        insertIntoMapType(map, rhsItemTag1, rhsItemVal1, 5);
        auto [rhsItemTag2, rhsItemVal2] = value::makeCopyDecimal(Decimal128{"2.71"});
        insertIntoMapType(map, rhsItemTag2, rhsItemVal2, 6);
        auto [rhsItemTag3, rhsItemVal3] = value::makeCopyDecimal(Decimal128{"3.14"});
        insertIntoMapType(map, rhsItemTag3, rhsItemVal3, 7);
        ASSERT(map->size() == initSize + 2)
            << "map: " << map << " should be of size " << initSize + 2;
    };

    // Compare MapTypes with single element of different (and mostly complex) types.
    valueMapTypeEqualityComparisonTestGenFn(addShortStringKeyFn, addShortStringKeyFn);
    valueMapTypeEqualityComparisonTestGenFn(addLongStringKeyFn1, addLongStringKeyFn1);
    valueMapTypeEqualityComparisonTestGenFn(addArrayKeyFn, addArrayKeyFn);
    valueMapTypeEqualityComparisonTestGenFn(addObjectKeyFn, addObjectKeyFn);
    valueMapTypeEqualityComparisonTestGenFn(addMultipleDecimalKeyFn, addMultipleDecimalKeyFn);
    // Check whether adding a single complex type multiple times doesn't break the equality.
    valueMapTypeEqualityComparisonTestGenFn(addLongStringMultipleTimesKeyFn,
                                            addLongStringMultipleTimesKeyFn);
    // Check whether the insertion into MapType is order agnostic.
    valueMapTypeEqualityComparisonTestGenFn(
        [&](MapType* map) {
            addArrayKeyFn(map);
            addMultipleDecimalKeyFn(map);
            addObjectKeyFn(map);
            addLongStringKeyFn1(map);
        },
        [&](MapType* map) {
            addObjectKeyFn(map);
            addLongStringKeyFn1(map);
            addArrayKeyFn(map);
            addMultipleDecimalKeyFn(map);
        });

    // Check inequal MapTypes are actually not equal.
    valueMapTypeInequalityComparisonTestGenFn(addShortStringKeyFn, addLongStringKeyFn1);
    valueMapTypeInequalityComparisonTestGenFn(addLongStringKeyFn1, addLongStringKeyFn2);
    valueMapTypeInequalityComparisonTestGenFn(addArrayKeyFn, addObjectKeyFn);
    valueMapTypeInequalityComparisonTestGenFn(addMultipleDecimalKeyFn, addObjectKeyFn);
}

TEST_F(SbeValueTest, ArrayMoveIsDestructive) {
    // Test that moving one SBE Array into another destroys the contents
    // of the first one.
    value::Array arr1;
    auto pushStr = [](value::Array* arr, StringData str) {
        auto [t, v] = value::makeBigString(str);
        arr->push_back(t, v);
    };

    pushStr(&arr1, "foo");
    pushStr(&arr1, "bar");

    value::Array arr2 = std::move(arr1);

    // arr1 should not hold dangling pointers to the values now owned by arr2.
    ASSERT_EQ(arr1.size(), 0);  // NOLINT(bugprone-use-after-move)
}

TEST_F(SbeValueTest, ArrayForEachMoveIsDestructive) {
    auto [tag, val] = value::makeNewArray();
    value::ValueGuard guard{tag, val};

    value::Array& arr1 = *value::getArrayView(val);

    auto pushStr = [](value::Array* arr, StringData str) {
        auto [t, v] = value::makeBigString(str);
        arr->push_back(t, v);
    };

    pushStr(&arr1, "foo");
    pushStr(&arr1, "bar");

    {
        auto [elTag, elVal] = arr1.getAt(0);
        ASSERT_TRUE(value::isString(elTag));
    }
    {
        auto [elTag, elVal] = arr1.getAt(1);
        ASSERT_TRUE(value::isString(elTag));
    }

    value::Array arr2;
    // Move elements from arr1 into arr2.
    value::arrayForEach<true>(
        tag, val, [&](value::TypeTags elTag, value::Value elVal) { arr2.push_back(elTag, elVal); });

    ASSERT_EQ(arr1.size(), 0);
    {
        auto [elTag, elVal] = arr2.getAt(0);
        ASSERT_TRUE(value::isString(elTag));
    }
    {
        auto [elTag, elVal] = arr2.getAt(1);
        ASSERT_TRUE(value::isString(elTag));
    }
}

TEST_F(SbeValueTest, ArraySetForEachMoveIsDestructive) {
    auto [tag, val] = value::makeNewArraySet();
    value::ValueGuard guard{tag, val};

    value::ArraySet& arr1 = *value::getArraySetView(val);

    auto pushStr = [](value::ArraySet* arr, StringData str) {
        auto [t, v] = value::makeBigString(str);
        arr->push_back(t, v);
    };

    pushStr(&arr1, "foo");
    pushStr(&arr1, "bar");

    ASSERT_EQ(arr1.size(), 2);

    value::ArraySet arr2;
    // Move elements from arr1 into arr2.
    value::arrayForEach<true>(
        tag, val, [&](value::TypeTags elTag, value::Value elVal) { arr2.push_back(elTag, elVal); });

    ASSERT_EQ(arr1.size(), 0);
    ASSERT_EQ(arr2.size(), 2);
}

template <typename... Args>
std::pair<value::TypeTags, value::Value> createArray(Args... args) {
    auto [arrayTag, arrayVal] = value::makeNewArray();
    auto array = value::getArrayView(arrayVal);
    for (const auto& [tag, val] : {args...}) {
        array->push_back(tag, val);
    }
    return {arrayTag, arrayVal};
}

TEST_F(SbeValueTest, SortSpecCompareSingleValueAsc) {
    auto sortSpecBson = BSON("x" << 1);
    value::SortSpec sortSpec(sortSpecBson);

    auto [tag1, val1] = std::make_pair(value::TypeTags::NumberInt32, 1);
    auto [tag2, val2] = std::make_pair(value::TypeTags::NumberInt32, 2);

    auto [cmpTag, cmpVal] = sortSpec.compare(tag1, val1, tag2, val2);
    ASSERT_EQ(cmpTag, value::TypeTags::NumberInt32);
    ASSERT_EQ(value::bitcastTo<int32_t>(cmpVal), -1);

    std::tie(cmpTag, cmpVal) = sortSpec.compare(tag2, val2, tag1, val1);
    ASSERT_EQ(cmpTag, value::TypeTags::NumberInt32);
    ASSERT_EQ(value::bitcastTo<int32_t>(cmpVal), 1);

    std::tie(cmpTag, cmpVal) = sortSpec.compare(tag1, val1, tag1, val1);
    ASSERT_EQ(cmpTag, value::TypeTags::NumberInt32);
    ASSERT_EQ(value::bitcastTo<int32_t>(cmpVal), 0);
}

TEST_F(SbeValueTest, SortSpecCompareSingleValueDsc) {
    auto sortSpecBson = BSON("x" << -1);
    value::SortSpec sortSpec(sortSpecBson);

    auto [tag1, val1] = std::make_pair(value::TypeTags::NumberInt32, 1);
    auto [tag2, val2] = std::make_pair(value::TypeTags::NumberInt32, 2);

    auto [cmpTag, cmpVal] = sortSpec.compare(tag1, val1, tag2, val2);
    ASSERT_EQ(cmpTag, value::TypeTags::NumberInt32);
    ASSERT_EQ(value::bitcastTo<int32_t>(cmpVal), 1);

    std::tie(cmpTag, cmpVal) = sortSpec.compare(tag2, val2, tag1, val1);
    ASSERT_EQ(cmpTag, value::TypeTags::NumberInt32);
    ASSERT_EQ(value::bitcastTo<int32_t>(cmpVal), -1);

    std::tie(cmpTag, cmpVal) = sortSpec.compare(tag1, val1, tag1, val1);
    ASSERT_EQ(cmpTag, value::TypeTags::NumberInt32);
    ASSERT_EQ(value::bitcastTo<int32_t>(cmpVal), 0);
}

TEST_F(SbeValueTest, SortSpecCompareCollation) {
    auto sortSpecBson = BSON("x" << 1);
    value::SortSpec sortSpec(sortSpecBson);

    auto [tag1, val1] = value::makeBigString("12345678");
    value::ValueGuard guard1{tag1, val1};
    auto [tag2, val2] = value::makeBigString("87654321");
    value::ValueGuard guard2{tag2, val2};

    auto collator =
        std::make_unique<CollatorInterfaceMock>(CollatorInterfaceMock::MockType::kReverseString);

    auto [cmpTag, cmpVal] = sortSpec.compare(tag1, val1, tag2, val2, collator.get());
    ASSERT_EQ(cmpTag, value::TypeTags::NumberInt32);
    ASSERT_EQ(value::bitcastTo<int32_t>(cmpVal), 1);

    std::tie(cmpTag, cmpVal) = sortSpec.compare(tag2, val2, tag1, val1, collator.get());
    ASSERT_EQ(cmpTag, value::TypeTags::NumberInt32);
    ASSERT_EQ(value::bitcastTo<int32_t>(cmpVal), -1);

    std::tie(cmpTag, cmpVal) = sortSpec.compare(tag1, val1, tag1, val1, collator.get());
    ASSERT_EQ(cmpTag, value::TypeTags::NumberInt32);
    ASSERT_EQ(value::bitcastTo<int32_t>(cmpVal), 0);
}

TEST_F(SbeValueTest, SortSpecCompareMultiValueMix) {
    auto sortSpecBson = BSON("x" << 1 << "y" << -1);
    value::SortSpec sortSpec(sortSpecBson);

    auto [tag11, val11] =
        createArray(value::makeBigString("11111111"), value::makeBigString("11111111"));
    value::ValueGuard guard11{tag11, val11};
    auto [tag12, val12] =
        createArray(value::makeBigString("11111111"), value::makeBigString("22222222"));
    value::ValueGuard guard12{tag12, val12};
    auto [tag21, val21] =
        createArray(value::makeBigString("22222222"), value::makeBigString("11111111"));
    value::ValueGuard guard21{tag21, val21};

    auto [cmpTag, cmpVal] = sortSpec.compare(tag11, val11, tag21, val21);
    ASSERT_EQ(cmpTag, value::TypeTags::NumberInt32);
    ASSERT_EQ(value::bitcastTo<int32_t>(cmpVal), -1);

    std::tie(cmpTag, cmpVal) = sortSpec.compare(tag11, val11, tag12, val12);
    ASSERT_EQ(cmpTag, value::TypeTags::NumberInt32);
    ASSERT_EQ(value::bitcastTo<int32_t>(cmpVal), 1);

    std::tie(cmpTag, cmpVal) = sortSpec.compare(tag21, val21, tag11, val11);
    ASSERT_EQ(cmpTag, value::TypeTags::NumberInt32);
    ASSERT_EQ(value::bitcastTo<int32_t>(cmpVal), 1);

    std::tie(cmpTag, cmpVal) = sortSpec.compare(tag12, val12, tag11, val11);
    ASSERT_EQ(cmpTag, value::TypeTags::NumberInt32);
    ASSERT_EQ(value::bitcastTo<int32_t>(cmpVal), -1);

    std::tie(cmpTag, cmpVal) = sortSpec.compare(tag11, val11, tag11, val11);
    ASSERT_EQ(cmpTag, value::TypeTags::NumberInt32);
    ASSERT_EQ(value::bitcastTo<int32_t>(cmpVal), 0);
}

TEST_F(SbeValueTest, SortSpecCompareInvalid) {
    auto sortSpecBson = BSON("x" << 1 << "y" << -1);
    value::SortSpec sortSpec(sortSpecBson);

    auto [tag1, val1] =
        createArray(value::makeBigString("11111111"), value::makeBigString("11111111"));
    value::ValueGuard guard1{tag1, val1};
    auto [tag2, val2] = createArray(value::makeBigString("11111111"),
                                    value::makeBigString("11111111"),
                                    value::makeBigString("11111111"));
    value::ValueGuard guard2{tag2, val2};

    auto [cmpTag, cmpVal] = sortSpec.compare(value::TypeTags::NumberInt32, 0, tag1, val1);
    ASSERT_EQ(cmpTag, value::TypeTags::Nothing);
    ASSERT_EQ(cmpVal, 0);

    std::tie(cmpTag, cmpVal) = sortSpec.compare(tag1, val1, value::TypeTags::NumberInt32, 0);
    ASSERT_EQ(cmpTag, value::TypeTags::Nothing);
    ASSERT_EQ(cmpVal, 0);

    std::tie(cmpTag, cmpVal) = sortSpec.compare(tag1, val1, tag2, val2);
    ASSERT_EQ(cmpTag, value::TypeTags::Nothing);
    ASSERT_EQ(cmpVal, 0);

    std::tie(cmpTag, cmpVal) = sortSpec.compare(tag2, val2, tag1, val1);
    ASSERT_EQ(cmpTag, value::TypeTags::Nothing);
    ASSERT_EQ(cmpVal, 0);
}
}  // namespace mongo::sbe
