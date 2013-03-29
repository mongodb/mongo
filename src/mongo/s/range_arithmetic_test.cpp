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
 */

#include "mongo/s/range_arithmetic.h"
#include "mongo/unittest/unittest.h"

namespace {
    using mongo::rangeOverlaps;

    TEST(BSONRange, SmallerLowerRangeNonSubset) {
        ASSERT_TRUE(rangeOverlaps(BSON("x" << 100), BSON("x" << 200),
                                  BSON("x" << 50), BSON("x" << 200)));
        ASSERT_TRUE(rangeOverlaps(BSON("x" << 100), BSON("x" << 200),
                                  BSON("x" << 60), BSON("x" << 199)));

        ASSERT_FALSE(rangeOverlaps(BSON("x" << 100), BSON("x" << 200),
                                   BSON("x" << 70), BSON("x" << 99)));
        ASSERT_FALSE(rangeOverlaps(BSON("x" << 100), BSON("x" << 200),
                                   BSON("x" << 80), BSON("x" << 100)));
    }

    TEST(BSONRange, BiggerUpperRangeNonSubset) {
        ASSERT_TRUE(rangeOverlaps(BSON("x" << 100), BSON("x" << 200),
                                  BSON("x" << 150), BSON("x" << 200)));
        ASSERT_TRUE(rangeOverlaps(BSON("x" << 100), BSON("x" << 200),
                                  BSON("x" << 160), BSON("x" << 201)));
        ASSERT_TRUE(rangeOverlaps(BSON("x" << 100), BSON("x" << 200),
                                  BSON("x" << 170), BSON("x" << 220)));

        ASSERT_FALSE(rangeOverlaps(BSON("x" << 100), BSON("x" << 200),
                                   BSON("x" << 200), BSON("x" << 240)));
    }

    TEST(BSONRange, RangeIsSubsetOfOther) {
        ASSERT_TRUE(rangeOverlaps(BSON("x" << 100), BSON("x" << 200),
                                  BSON("x" << 70), BSON("x" << 300)));
        ASSERT_TRUE(rangeOverlaps(BSON("x" << 100), BSON("x" << 200),
                                  BSON("x" << 140), BSON("x" << 180)));
    }

    TEST(BSONRange, EqualRange) {
        ASSERT_TRUE(rangeOverlaps(BSON("x" << 100), BSON("x" << 200),
                                  BSON("x" << 100), BSON("x" << 200)));
    }
}
