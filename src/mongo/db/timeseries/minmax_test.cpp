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

#include "mongo/platform/basic.h"

#include "mongo/base/simple_string_data_comparator.h"
#include "mongo/db/timeseries/minmax.h"
#include "mongo/unittest/unittest.h"

#include <numeric>

namespace mongo::timeseries {
namespace {

std::string concatFieldNames(const MinMaxStore::Obj& obj) {
    return std::accumulate(obj.begin(),
                           obj.end(),
                           std::string(),
                           [](std::string accum, const MinMaxStore::Element& elem) {
                               return std::move(accum) + elem.fieldName();
                           });
}

TEST(MinMax, Insert) {
    MinMaxStore minmax;

    // No subelements to start
    auto obj = minmax.root();
    ASSERT_EQ(std::distance(obj.begin(), obj.end()), 0);

    // We can insert at begin
    {
        auto [inserted, end] = obj.insert(obj.begin(), "b");
        ASSERT(obj.begin() == inserted);
        ASSERT(obj.end() == end);
        ASSERT_EQ(std::distance(inserted, end), 1);
        ASSERT_EQ(inserted->fieldName(), "b");

        // parent should be pointing to obj
        ASSERT(obj.object(inserted).parent().iterator() == obj.iterator());
    }

    // Insert when there is already something there
    {
        auto [inserted, end] = obj.insert(obj.begin(), "a");
        ASSERT(obj.begin() == inserted);
        ASSERT(obj.end() == end);
        ASSERT_EQ(std::distance(inserted, end), 2);
        ASSERT_EQ(inserted->fieldName(), "a");
        ASSERT(obj.object(inserted).parent().iterator() == obj.iterator());

        // Validate the existing element
        ++inserted;
        ASSERT_EQ(inserted->fieldName(), "b");
        ASSERT_EQ(concatFieldNames(obj), "ab");
        ASSERT(obj.object(inserted).parent().iterator() == obj.iterator());
    }

    // Insert at and when there is already something there
    {
        auto [inserted, end] = obj.insert(obj.end(), "d");
        ASSERT(obj.end() == end);
        ASSERT_EQ(std::distance(inserted, end), 1);
        ASSERT_EQ(std::distance(obj.begin(), obj.end()), 3);
        ASSERT_EQ(inserted->fieldName(), "d");
        ASSERT_EQ(concatFieldNames(obj), "abd");
        ASSERT(obj.object(inserted).parent().iterator() == obj.iterator());
    }

    // We can also insert at the middle
    {
        auto it = obj.begin();
        ++it;
        ++it;
        auto [inserted, end] = obj.insert(it, "c");
        ASSERT_EQ(concatFieldNames(obj), "abcd");
        ASSERT_EQ(std::distance(inserted, end), 2);
        ASSERT_EQ(std::distance(obj.begin(), obj.end()), 4);
        ASSERT(obj.object(inserted).parent().iterator() == obj.iterator());

        // Validate last element that it got parent updated
        ++inserted;
        ASSERT(obj.object(inserted).parent().iterator() == obj.iterator());
    }
}

TEST(MinMax, MinMaxNoUpdatesAfterFullMinMax) {
    MinMax minMaxObj;
    const auto* strCmp = &SimpleStringDataComparator::kInstance;
    minMaxObj.update(BSON("a" << 2 << "b" << 3 << "meta" << 4), "meta"_sd, strCmp);
    ASSERT_BSONOBJ_EQ(minMaxObj.min(), BSON("a" << 2 << "b" << 3));
    ASSERT_BSONOBJ_EQ(minMaxObj.minUpdates(), BSONObj());

    minMaxObj.update(BSON("a" << 1 << "b" << 3 << "meta" << 4), "meta"_sd, strCmp);
    ASSERT_BSONOBJ_EQ(minMaxObj.max(), BSON("a" << 2 << "b" << 3));
    ASSERT_BSONOBJ_EQ(minMaxObj.maxUpdates(), BSONObj());
    ASSERT_BSONOBJ_EQ(minMaxObj.minUpdates(), BSON("u" << BSON("a" << 1)));
}

TEST(MinMax, MinMaxNoUpdatesAfterFullMinMaxNested) {
    MinMax minMaxObj;
    const auto* strCmp = &SimpleStringDataComparator::kInstance;

    auto obj = BSON("a" << BSON("z" << 1) << "b" << BSON_ARRAY(BSON("z" << 1) << BSON("z" << 2)));
    minMaxObj.update(obj, "_meta"_sd, strCmp);
    ASSERT_BSONOBJ_EQ(minMaxObj.min(), obj);
    ASSERT_BSONOBJ_EQ(minMaxObj.max(), obj);
    ASSERT_BSONOBJ_EQ(minMaxObj.minUpdates(), BSONObj{});
    ASSERT_BSONOBJ_EQ(minMaxObj.maxUpdates(), BSONObj{});

    minMaxObj.update(
        BSON("a" << BSON_ARRAY(BSON("z" << 1) << BSON("z" << 2)) << "b" << BSON("z" << 1)),
        "_meta"_sd,
        strCmp);
    ASSERT_BSONOBJ_EQ(minMaxObj.minUpdates(), BSON("u" << BSON("b" << BSON("z" << 1))));
    ASSERT_BSONOBJ_EQ(minMaxObj.maxUpdates(),
                      BSON("u" << BSON("a" << BSON_ARRAY(BSON("z" << 1) << BSON("z" << 2)))));
    ASSERT_BSONOBJ_EQ(minMaxObj.minUpdates(), BSONObj{});
    ASSERT_BSONOBJ_EQ(minMaxObj.maxUpdates(), BSONObj{});
}

TEST(MinMax, MinMaxInitialUpdates) {
    MinMax minMaxObj;
    const auto* strCmp = &SimpleStringDataComparator::kInstance;
    minMaxObj.update(BSON("a" << 2 << "b" << 3 << "meta" << 4), "meta"_sd, strCmp);
    ASSERT_BSONOBJ_EQ(minMaxObj.minUpdates(), BSON("u" << BSON("a" << 2 << "b" << 3)));

    minMaxObj.update(BSON("a" << 1 << "b" << 3 << "meta" << 4), "meta"_sd, strCmp);
    ASSERT_BSONOBJ_EQ(minMaxObj.minUpdates(), BSON("u" << BSON("a" << 1)));
}

TEST(MinMax, MinMaxMixedUpdates) {
    MinMax minMaxObj;
    const auto* strCmp = &SimpleStringDataComparator::kInstance;
    minMaxObj.update(BSON("a" << 2 << "b" << 3 << "meta" << 4), "meta"_sd, strCmp);
    ASSERT_BSONOBJ_EQ(minMaxObj.min(), BSON("a" << 2 << "b" << 3));
    ASSERT_BSONOBJ_EQ(minMaxObj.minUpdates(), BSONObj());
    ASSERT_BSONOBJ_EQ(minMaxObj.maxUpdates(), BSON("u" << BSON("a" << 2 << "b" << 3)));
    ASSERT_BSONOBJ_EQ(minMaxObj.max(), BSON("a" << 2 << "b" << 3));

    minMaxObj.update(BSON("a" << 5 << "b" << 3 << "meta" << 4), "meta"_sd, strCmp);
    ASSERT_BSONOBJ_EQ(minMaxObj.minUpdates(), BSONObj());
    ASSERT_BSONOBJ_EQ(minMaxObj.maxUpdates(), BSON("u" << BSON("a" << 5)));
}

TEST(MinMax, SubObjInsert) {
    MinMaxStore minmax;
    auto obj = minmax.root();
    auto [inserted, _] = obj.insert(obj.end(), "b");

    auto subobjB = obj.object(inserted);
    ASSERT_EQ(std::distance(subobjB.begin(), subobjB.end()), 0);
    ASSERT(obj.begin() != subobjB.begin());
    ASSERT(obj.end() == subobjB.end());
    ASSERT(obj.begin() == subobjB.parent().begin());

    subobjB.insert(subobjB.begin(), "1");
    subobjB.insert(subobjB.end(), "3");
    obj = subobjB.parent();

    ASSERT_EQ(concatFieldNames(obj), "b");
    ASSERT_EQ(concatFieldNames(obj.object(obj.begin())), "13");

    obj.insert(obj.end(), "c");
    ASSERT_EQ(concatFieldNames(obj), "bc");
    ASSERT_EQ(concatFieldNames(obj.object(obj.begin())), "13");

    std::tie(inserted, _) = obj.insert(obj.begin(), "a");
    ASSERT_EQ(concatFieldNames(obj), "abc");

    subobjB = obj.object(std::next(inserted));
    ASSERT_EQ(concatFieldNames(subobjB), "13");

    // Insert in subobj and check that the last element in obj 'c' got its parent updated.
    std::tie(inserted, _) = subobjB.insert(std::next(subobjB.begin()), "2");
    obj = subobjB.parent();
    ASSERT_EQ(concatFieldNames(obj), "abc");
    ASSERT_EQ(concatFieldNames(subobjB), "123");
    auto itC = obj.begin();
    std::advance(itC, 2);
    ASSERT_EQ(itC->fieldName(), "c");
    ASSERT(obj.object(itC).parent().iterator() == obj.iterator());

    // Create a third level, validate that all iterators is updated
    auto subobjB2 = obj.object(std::next(obj.object(std::next(obj.begin())).begin()));
    subobjB2.insert(subobjB2.begin(), "x");
    subobjB = subobjB2.parent();
    obj = subobjB.parent();
    ASSERT_EQ(concatFieldNames(obj), "abc");
    ASSERT_EQ(concatFieldNames(subobjB), "123");
    ASSERT_EQ(concatFieldNames(subobjB2), "x");
    itC = obj.begin();
    std::advance(itC, 2);
    ASSERT_EQ(itC->fieldName(), "c");
    ASSERT(obj.object(itC).parent().iterator() == obj.iterator());
}

TEST(MinMax, Search) {
    MinMaxStore minmax;
    auto obj = minmax.root();
    obj.insert(obj.end(), "a");
    obj.insert(obj.end(), "b");
    obj.insert(obj.end(), "c");
    obj.insert(obj.end(), "d");
    ASSERT_EQ(concatFieldNames(obj), "abcd");

    ASSERT_EQ(obj.search(obj.begin(), "a")->fieldName(), "a");
    ASSERT_EQ(obj.search(obj.begin(), "c")->fieldName(), "c");
    ASSERT_EQ(obj.search(obj.begin(), "d")->fieldName(), "d");
    ASSERT(obj.search(obj.begin(), "e") == obj.end());
    ASSERT(obj.search(std::next(obj.begin()), "a") == obj.end());

    auto itC = std::next(std::next(obj.begin()));
    ASSERT(obj.search(obj.begin(), itC, "d") == itC);
}

TEST(MinMax, SearchLookupMap) {
    MinMaxStore minmax;
    auto obj = minmax.root();

    for (int i = 0; i < 100; ++i) {
        obj.insert(obj.end(), std::to_string(i));
    }

    // Trigger lookup map to be created by requiring a long search
    ASSERT_EQ(obj.search(obj.begin(), "99")->fieldName(), "99");

    // When lookup map exists we find things outside of the provided range
    ASSERT_EQ(obj.search(std::next(obj.begin()), "0")->fieldName(), "0");

    // Provided last is still respected when something is not found
    auto last = std::next(obj.begin());
    ASSERT(obj.search(obj.begin(), last, "100") == last);

    // Map based search is still accurate after inserts
    obj.insert(obj.begin(), "x");
    ASSERT_EQ(obj.search(obj.begin(), "50")->fieldName(), "50");
}


}  // namespace
}  // namespace mongo::timeseries
