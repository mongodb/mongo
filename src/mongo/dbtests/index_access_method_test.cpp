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

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/simple_bsonobj_comparator.h"
#include "mongo/db/index/index_access_method.h"
#include "mongo/db/json.h"
#include "mongo/unittest/unittest.h"

namespace mongo {

namespace {
using std::vector;

KeyStringSet makeKeyStringSet(std::initializer_list<BSONObj> objs) {
    KeyStringSet keyStrings;
    for (auto& obj : objs) {
        KeyString::HeapBuilder keyString(
            KeyString::Version::kLatestVersion, obj, Ordering::make(BSONObj()));
        keyStrings.insert(keyString.release());
    }
    return keyStrings;
}

TEST(IndexAccessMethodSetDifference, EmptyInputsShouldHaveNoDifference) {
    KeyStringSet left;
    KeyStringSet right;
    auto diff = AbstractIndexAccessMethod::setDifference(left, right, Ordering::make(BSONObj()));
    ASSERT_EQ(0UL, diff.first.size());
    ASSERT_EQ(0UL, diff.second.size());
}

TEST(IndexAccessMethodSetDifference, EmptyLeftShouldHaveNoDifference) {
    KeyStringSet left;
    auto right = makeKeyStringSet({BSON("" << 0)});

    auto diff = AbstractIndexAccessMethod::setDifference(left, right, Ordering::make(BSONObj()));
    ASSERT_EQ(0UL, diff.first.size());
    ASSERT_EQ(1UL, diff.second.size());
}

TEST(IndexAccessMethodSetDifference, EmptyRightShouldReturnAllOfLeft) {
    auto left = makeKeyStringSet({BSON("" << 0), BSON("" << 1)});
    KeyStringSet right;

    auto diff = AbstractIndexAccessMethod::setDifference(left, right, Ordering::make(BSONObj()));
    ASSERT_EQ(2UL, diff.first.size());
    ASSERT_EQ(0UL, diff.second.size());
}

TEST(IndexAccessMethodSetDifference, IdenticalSetsShouldHaveNoDifference) {
    auto left = makeKeyStringSet({BSON("" << 0),
                                  BSON(""
                                       << "string"),
                                  BSON("" << BSONNULL)});
    auto right = makeKeyStringSet({BSON("" << 0),
                                   BSON(""
                                        << "string"),
                                   BSON("" << BSONNULL)});

    auto diff = AbstractIndexAccessMethod::setDifference(left, right, Ordering::make(BSONObj()));
    ASSERT_EQ(0UL, diff.first.size());
    ASSERT_EQ(0UL, diff.second.size());
}

//
// Number type comparisons.
//

void assertDistinct(BSONObj left, BSONObj right) {
    auto leftSet = makeKeyStringSet({left});
    auto rightSet = makeKeyStringSet({right});
    auto diff =
        AbstractIndexAccessMethod::setDifference(leftSet, rightSet, Ordering::make(BSONObj()));
    ASSERT_EQ(1UL, diff.first.size());
    ASSERT_EQ(1UL, diff.second.size());
}

TEST(IndexAccessMethodSetDifference, ZerosOfDifferentTypesAreNotEquivalent) {
    const BSONObj intObj = BSON("" << static_cast<int>(0));
    const BSONObj longObj = BSON("" << static_cast<long long>(0));
    const BSONObj doubleObj = BSON("" << static_cast<double>(0.0));

    // These should compare equal with woCompare(), but should not be treated equal by the index.
    ASSERT_EQ(0, intObj.woCompare(longObj));
    ASSERT_EQ(0, longObj.woCompare(doubleObj));

    assertDistinct(intObj, longObj);
    assertDistinct(intObj, doubleObj);

    assertDistinct(longObj, intObj);
    assertDistinct(longObj, doubleObj);

    assertDistinct(doubleObj, intObj);
    assertDistinct(doubleObj, longObj);

    const BSONObj decimalObj = fromjson("{'': NumberDecimal('0')}");

    ASSERT_EQ(0, doubleObj.woCompare(decimalObj));

    assertDistinct(intObj, decimalObj);
    assertDistinct(longObj, decimalObj);
    assertDistinct(doubleObj, decimalObj);

    assertDistinct(decimalObj, intObj);
    assertDistinct(decimalObj, longObj);
    assertDistinct(decimalObj, doubleObj);
}

TEST(IndexAccessMethodSetDifference, ShouldDetectOneDifferenceAmongManySimilarities) {
    auto left = makeKeyStringSet({BSON("" << 0),
                                  BSON(""
                                       << "string"),
                                  BSON("" << BSONNULL),
                                  BSON("" << static_cast<long long>(1)),  // This is different.
                                  BSON("" << BSON("sub"
                                                  << "document")),
                                  BSON("" << BSON_ARRAY(1 << "hi" << 42))});
    auto right = makeKeyStringSet({BSON("" << 0),
                                   BSON(""
                                        << "string"),
                                   BSON("" << BSONNULL),
                                   BSON("" << static_cast<double>(1.0)),  // This is different.
                                   BSON("" << BSON("sub"
                                                   << "document")),
                                   BSON("" << BSON_ARRAY(1 << "hi" << 42))});
    auto diff = AbstractIndexAccessMethod::setDifference(left, right, Ordering::make(BSONObj()));
    ASSERT_EQUALS(1UL, diff.first.size());
    ASSERT_EQUALS(1UL, diff.second.size());
}

TEST(IndexAccessMethodSetDifference, SingleObjInLeftShouldFindCorrespondingObjInRight) {
    auto left = makeKeyStringSet({BSON("" << 2)});
    auto right = makeKeyStringSet({BSON("" << 1), BSON("" << 2), BSON("" << 3)});
    auto diff = AbstractIndexAccessMethod::setDifference(left, right, Ordering::make(BSONObj()));
    ASSERT_EQUALS(0UL, diff.first.size());
    ASSERT_EQUALS(2UL, diff.second.size());
}

TEST(IndexAccessMethodSetDifference, SingleObjInRightShouldFindCorrespondingObjInLeft) {
    auto left = makeKeyStringSet({BSON("" << 1), BSON("" << 2), BSON("" << 3)});
    auto right = makeKeyStringSet({BSON("" << 2)});
    auto diff = AbstractIndexAccessMethod::setDifference(left, right, Ordering::make(BSONObj()));
    ASSERT_EQUALS(2UL, diff.first.size());
    ASSERT_EQUALS(0UL, diff.second.size());
}

TEST(IndexAccessMethodSetDifference, LeftSetAllSmallerThanRightShouldBeDisjoint) {
    auto left = makeKeyStringSet({BSON("" << 1), BSON("" << 2), BSON("" << 3)});
    auto right = makeKeyStringSet({BSON("" << 4), BSON("" << 5), BSON("" << 6)});
    auto diff = AbstractIndexAccessMethod::setDifference(left, right, Ordering::make(BSONObj()));
    ASSERT_EQUALS(3UL, diff.first.size());
    ASSERT_EQUALS(3UL, diff.second.size());
    for (auto&& obj : diff.first) {
        ASSERT(left.find(obj) != left.end());
    }
    for (auto&& obj : diff.second) {
        ASSERT(right.find(obj) != right.end());
    }
}

TEST(IndexAccessMethodSetDifference, LeftSetAllLargerThanRightShouldBeDisjoint) {
    auto left = makeKeyStringSet({BSON("" << 4), BSON("" << 5), BSON("" << 6)});
    auto right = makeKeyStringSet({BSON("" << 1), BSON("" << 2), BSON("" << 3)});
    auto diff = AbstractIndexAccessMethod::setDifference(left, right, Ordering::make(BSONObj()));
    ASSERT_EQUALS(3UL, diff.first.size());
    ASSERT_EQUALS(3UL, diff.second.size());
    for (auto&& obj : diff.first) {
        ASSERT(left.find(obj) != left.end());
    }
    for (auto&& obj : diff.second) {
        ASSERT(right.find(obj) != right.end());
    }
}

TEST(IndexAccessMethodSetDifference, ShouldNotReportOverlapsFromNonDisjointSets) {
    auto left = makeKeyStringSet({BSON("" << 0), BSON("" << 1), BSON("" << 4), BSON("" << 6)});
    auto right = makeKeyStringSet(
        {BSON("" << -1), BSON("" << 1), BSON("" << 3), BSON("" << 4), BSON("" << 7)});
    auto diff = AbstractIndexAccessMethod::setDifference(left, right, Ordering::make(BSONObj()));
    ASSERT_EQUALS(2UL, diff.first.size());   // 0, 6.
    ASSERT_EQUALS(3UL, diff.second.size());  // -1, 3, 7.
    for (auto&& keyString : diff.first) {
        ASSERT(left.find(keyString) != left.end());
        // Make sure it's not in the intersection.
        auto obj = KeyString::toBson(keyString, Ordering::make(BSONObj()));
        ASSERT_BSONOBJ_NE(obj, BSON("" << 1));
        ASSERT_BSONOBJ_NE(obj, BSON("" << 4));
    }
    for (auto&& keyString : diff.second) {
        ASSERT(right.find(keyString) != right.end());
        // Make sure it's not in the intersection.
        auto obj = KeyString::toBson(keyString, Ordering::make(BSONObj()));
        ASSERT_BSONOBJ_NE(obj, BSON("" << 1));
        ASSERT_BSONOBJ_NE(obj, BSON("" << 4));
    }
}

}  // namespace

}  // namespace mongo
