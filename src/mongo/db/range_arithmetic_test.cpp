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

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/range_arithmetic.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

TEST(BSONRange, SmallerLowerRangeNonSubset) {
    ASSERT_TRUE(
        rangeOverlaps(BSON("x" << 100), BSON("x" << 200), BSON("x" << 50), BSON("x" << 200)));
    ASSERT_TRUE(
        rangeOverlaps(BSON("x" << 100), BSON("x" << 200), BSON("x" << 60), BSON("x" << 199)));

    ASSERT_FALSE(
        rangeOverlaps(BSON("x" << 100), BSON("x" << 200), BSON("x" << 70), BSON("x" << 99)));
    ASSERT_FALSE(
        rangeOverlaps(BSON("x" << 100), BSON("x" << 200), BSON("x" << 80), BSON("x" << 100)));
}

TEST(BSONRange, BiggerUpperRangeNonSubset) {
    ASSERT_TRUE(
        rangeOverlaps(BSON("x" << 100), BSON("x" << 200), BSON("x" << 150), BSON("x" << 200)));
    ASSERT_TRUE(
        rangeOverlaps(BSON("x" << 100), BSON("x" << 200), BSON("x" << 160), BSON("x" << 201)));
    ASSERT_TRUE(
        rangeOverlaps(BSON("x" << 100), BSON("x" << 200), BSON("x" << 170), BSON("x" << 220)));

    ASSERT_FALSE(
        rangeOverlaps(BSON("x" << 100), BSON("x" << 200), BSON("x" << 200), BSON("x" << 240)));
}

TEST(BSONRange, RangeIsSubsetOfOther) {
    ASSERT_TRUE(
        rangeOverlaps(BSON("x" << 100), BSON("x" << 200), BSON("x" << 70), BSON("x" << 300)));
    ASSERT_TRUE(
        rangeOverlaps(BSON("x" << 100), BSON("x" << 200), BSON("x" << 140), BSON("x" << 180)));
}

TEST(BSONRange, EqualRange) {
    ASSERT_TRUE(
        rangeOverlaps(BSON("x" << 100), BSON("x" << 200), BSON("x" << 100), BSON("x" << 200)));
}

TEST(RangeMap, RangeMapOverlaps) {
    RangeMap rangeMap = SimpleBSONObjComparator::kInstance.makeBSONObjIndexedMap<BSONObj>();
    rangeMap.insert(std::make_pair(BSON("x" << 100), BSON("x" << 200)));

    ASSERT(rangeMapOverlaps(rangeMap, BSON("x" << 100), BSON("x" << 200)));
    ASSERT(rangeMapOverlaps(rangeMap, BSON("x" << 99), BSON("x" << 200)));
    ASSERT(rangeMapOverlaps(rangeMap, BSON("x" << 100), BSON("x" << 201)));
    ASSERT(rangeMapOverlaps(rangeMap, BSON("x" << 100), BSON("x" << 200)));
    ASSERT(!rangeMapOverlaps(rangeMap, BSON("x" << 99), BSON("x" << 100)));
    ASSERT(!rangeMapOverlaps(rangeMap, BSON("x" << 200), BSON("x" << 201)));
}

}  // namespace
}  // namespace mongo
