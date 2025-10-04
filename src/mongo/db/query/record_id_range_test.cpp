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

#include "mongo/db/query/record_id_range.h"

#include "mongo/base/error_extra_info.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/unittest/unittest.h"

namespace {

using namespace mongo;

void testRange(auto getBoundOptional,
               auto maybeNarrowBound,
               auto checkInclusivity,
               int initialValue,
               int narrowerValue,
               int widerValue) {
    ASSERT_FALSE(getBoundOptional());

    auto assertValueEq = [&](auto value) {
        auto bson = BSON("value" << value);
        auto recordId = record_id_helpers::keyForObj(bson);
        ASSERT_EQ(recordId, getBoundOptional()->recordId());
    };

    // narrow from unset
    maybeNarrowBound(BSON("value" << initialValue), true /* inclusive */);
    ASSERT_TRUE(getBoundOptional());
    assertValueEq(initialValue);
    ASSERT_TRUE(checkInclusivity());

    // narrow by removing inclusivity of the bound
    maybeNarrowBound(BSON("value" << initialValue), false /* not inclusive */);
    ASSERT_TRUE(getBoundOptional());
    assertValueEq(initialValue);
    ASSERT_FALSE(checkInclusivity());

    // cannot widen by re-adding inclusivity
    maybeNarrowBound(BSON("value" << initialValue), true /* inclusive */);
    ASSERT_TRUE(getBoundOptional());
    assertValueEq(initialValue);
    ASSERT_FALSE(checkInclusivity());

    // cannot widen by setting a wider bound
    maybeNarrowBound(BSON("value" << widerValue), true /* inclusive */);
    ASSERT_TRUE(getBoundOptional());
    assertValueEq(initialValue);
    ASSERT_FALSE(checkInclusivity());

    // cannot widen by setting a wider bound, regardless of inclusivity
    maybeNarrowBound(BSON("value" << widerValue), false /* not inclusive */);
    ASSERT_TRUE(getBoundOptional());
    assertValueEq(initialValue);
    ASSERT_FALSE(checkInclusivity());

    // narrow to a non-inclusive bound at a narrower value
    maybeNarrowBound(BSON("value" << narrowerValue), false /* not inclusive */);
    ASSERT_TRUE(getBoundOptional());
    assertValueEq(narrowerValue);
    ASSERT_FALSE(checkInclusivity());
}

TEST(RecordIdRangeTest, NarrowMin) {
    RecordIdRange range;

    testRange([&] { return range.getMin(); },
              [&](const BSONObj& newVal, bool inclusive) {
                  return range.maybeNarrowMin(newVal, inclusive);
              },
              [&] { return range.isMinInclusive(); },
              10,
              11,
              9);
}

TEST(RecordIdRangeTest, NarrowMax) {
    RecordIdRange range;

    testRange([&] { return range.getMax(); },
              [&](const BSONObj& newVal, bool inclusive) {
                  return range.maybeNarrowMax(newVal, inclusive);
              },
              [&] { return range.isMaxInclusive(); },
              10,
              9,
              11);
}

}  // namespace
