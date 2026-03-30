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

#include "mongo/db/keypattern.h"

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/stdx/type_traits.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

namespace {

using namespace mongo;

TEST(KeyPattern, ExtendRangeBound) {
    BSONObj bound = BSON("a" << 55);
    BSONObj longBound = BSON("a" << 55 << "b" << 66);

    // test keyPattern shorter than bound, should fail
    {
        KeyPattern keyPat(BSON("a" << 1));
        ASSERT_THROWS(keyPat.extendRangeBound(longBound, false), AssertionException);
    }

    // test keyPattern doesn't match bound, should fail
    {
        KeyPattern keyPat(BSON("b" << 1));
        ASSERT_THROWS(keyPat.extendRangeBound(bound, false), AssertionException);
    }
    {
        KeyPattern keyPat(BSON("a" << 1 << "c" << 1));
        ASSERT_THROWS(keyPat.extendRangeBound(longBound, false), AssertionException);
    }

    // test keyPattern same as bound
    {
        KeyPattern keyPat(BSON("a" << 1));
        BSONObj newB = keyPat.extendRangeBound(bound, false);
        ASSERT_BSONOBJ_EQ(newB, BSON("a" << 55));
    }
    {
        KeyPattern keyPat(BSON("a" << 1));
        BSONObj newB = keyPat.extendRangeBound(bound, false);
        ASSERT_BSONOBJ_EQ(newB, BSON("a" << 55));
    }

    // test keyPattern longer than bound, simple
    {
        KeyPattern keyPat(BSON("a" << 1 << "b" << 1));
        BSONObj newB = keyPat.extendRangeBound(bound, false);
        ASSERT_BSONOBJ_EQ(newB, BSON("a" << 55 << "b" << MINKEY));
    }
    {
        KeyPattern keyPat(BSON("a" << 1 << "b" << 1));
        BSONObj newB = keyPat.extendRangeBound(bound, true);
        ASSERT_BSONOBJ_EQ(newB, BSON("a" << 55 << "b" << MAXKEY));
    }

    // test keyPattern longer than bound, more complex pattern directions
    {
        KeyPattern keyPat(BSON("a" << 1 << "b" << -1));
        BSONObj newB = keyPat.extendRangeBound(bound, false);
        ASSERT_BSONOBJ_EQ(newB, BSON("a" << 55 << "b" << MAXKEY));
    }
    {
        KeyPattern keyPat(BSON("a" << 1 << "b" << -1));
        BSONObj newB = keyPat.extendRangeBound(bound, true);
        ASSERT_BSONOBJ_EQ(newB, BSON("a" << 55 << "b" << MINKEY));
    }
    {
        KeyPattern keyPat(BSON("a" << 1 << "b" << -1 << "c" << 1));
        BSONObj newB = keyPat.extendRangeBound(bound, false);
        ASSERT_BSONOBJ_EQ(newB, BSON("a" << 55 << "b" << MAXKEY << "c" << MINKEY));
    }
    {
        KeyPattern keyPat(BSON("a" << 1 << "b" << -1 << "c" << 1));
        BSONObj newB = keyPat.extendRangeBound(bound, true);
        ASSERT_BSONOBJ_EQ(newB, BSON("a" << 55 << "b" << MINKEY << "c" << MAXKEY));
    }
}

TEST(KeyPattern, GlobalMinMax) {
    //
    // Simple KeyPatterns
    //

    ASSERT_BSONOBJ_EQ(KeyPattern(BSON("a" << 1)).globalMin(), BSON("a" << MINKEY));
    ASSERT_BSONOBJ_EQ(KeyPattern(BSON("a" << 1)).globalMax(), BSON("a" << MAXKEY));

    ASSERT_BSONOBJ_EQ(KeyPattern(BSON("a" << -1)).globalMin(), BSON("a" << MAXKEY));
    ASSERT_BSONOBJ_EQ(KeyPattern(BSON("a" << -1)).globalMax(), BSON("a" << MINKEY));

    ASSERT_BSONOBJ_EQ(KeyPattern(BSON("a" << 1 << "b" << 1.0)).globalMin(),
                      BSON("a" << MINKEY << "b" << MINKEY));
    ASSERT_BSONOBJ_EQ(KeyPattern(BSON("a" << 1 << "b" << 1.0)).globalMax(),
                      BSON("a" << MAXKEY << "b" << MAXKEY));

    ASSERT_BSONOBJ_EQ(KeyPattern(BSON("a" << 1 << "b" << -1.0f)).globalMin(),
                      BSON("a" << MINKEY << "b" << MAXKEY));
    ASSERT_BSONOBJ_EQ(KeyPattern(BSON("a" << 1 << "b" << -1.0f)).globalMax(),
                      BSON("a" << MAXKEY << "b" << MINKEY));

    ASSERT_BSONOBJ_EQ(KeyPattern(BSON("a" << "hashed")).globalMin(), BSON("a" << MINKEY));
    ASSERT_BSONOBJ_EQ(KeyPattern(BSON("a" << "hashed")).globalMax(), BSON("a" << MAXKEY));

    //
    // Nested KeyPatterns
    //

    ASSERT_BSONOBJ_EQ(KeyPattern(BSON("a.b" << 1)).globalMin(), BSON("a.b" << MINKEY));
    ASSERT_BSONOBJ_EQ(KeyPattern(BSON("a.b" << 1)).globalMax(), BSON("a.b" << MAXKEY));

    ASSERT_BSONOBJ_EQ(KeyPattern(BSON("a.b.c" << -1)).globalMin(), BSON("a.b.c" << MAXKEY));
    ASSERT_BSONOBJ_EQ(KeyPattern(BSON("a.b.c" << -1)).globalMax(), BSON("a.b.c" << MINKEY));
}

TEST(KeyPattern, IsGlobalMax) {
    KeyPattern singleKey(BSON("a" << 1));
    KeyPattern compoundKey(BSON("a" << 1 << "b" << 1));

    ASSERT_TRUE(singleKey.isGlobalMax(BSON("a" << MAXKEY)));
    ASSERT_TRUE(compoundKey.isGlobalMax(BSON("a" << MAXKEY << "b" << MAXKEY)));

    ASSERT_FALSE(singleKey.isGlobalMax(BSON("a" << MINKEY)));
    ASSERT_FALSE(singleKey.isGlobalMax(BSON("a" << 5)));
    ASSERT_FALSE(compoundKey.isGlobalMax(BSON("a" << MAXKEY << "b" << MINKEY)));
    ASSERT_FALSE(compoundKey.isGlobalMax(BSON("a" << MAXKEY << "b" << 10)));
    ASSERT_FALSE(singleKey.isGlobalMax(BSONObj()));

    // Prefix bound (fewer fields than pattern) with all-MaxKey is still global max.
    ASSERT_TRUE(compoundKey.isGlobalMax(BSON("a" << MAXKEY)));

    // Bound with mismatched field names should fail with massert, same as extendRangeBound.
    ASSERT_THROWS_CODE(singleKey.isGlobalMax(BSON("z" << MAXKEY)), DBException, 12153300);
    ASSERT_THROWS_CODE(
        compoundKey.isGlobalMax(BSON("a" << MAXKEY << "z" << MAXKEY)), DBException, 12153300);
    ASSERT_THROWS_CODE(
        compoundKey.isGlobalMax(BSON("z" << MAXKEY << "b" << MAXKEY)), DBException, 12153300);

    // Bound with more fields than the pattern should fail with massert.
    ASSERT_THROWS_CODE(singleKey.isGlobalMax(BSON("a" << MAXKEY << "b" << MAXKEY)),
                       DBException,
                       ErrorCodes::KeyPatternShorterThanBound);
}

TEST(KeyPattern, ExtendRangeBoundMaxKeyWithMakeUpperInclusive) {
    // When the bound is the global max and makeUpperInclusive=true,
    // trailing fields should be padded with MaxKey instead of MinKey.
    {
        KeyPattern kp(BSON("a" << 1 << "b" << 1));
        BSONObj maxBound = BSON("a" << MAXKEY);
        BSONObj extended = kp.extendRangeBound(maxBound, true);
        ASSERT_BSONOBJ_EQ(extended, BSON("a" << MAXKEY << "b" << MAXKEY));
    }
    {
        KeyPattern kp(BSON("a" << 1 << "b" << 1 << "c" << 1));
        BSONObj maxBound = BSON("a" << MAXKEY);
        BSONObj extended = kp.extendRangeBound(maxBound, true);
        ASSERT_BSONOBJ_EQ(extended, BSON("a" << MAXKEY << "b" << MAXKEY << "c" << MAXKEY));
    }
    // Non-MaxKey bound with makeUpperInclusive=true pads with MaxKey.
    {
        KeyPattern kp(BSON("a" << 1 << "b" << 1));
        BSONObj normalBound = BSON("a" << 5);
        BSONObj extended = kp.extendRangeBound(normalBound, true);
        ASSERT_BSONOBJ_EQ(extended, BSON("a" << 5 << "b" << MAXKEY));
    }
}
}  // namespace
