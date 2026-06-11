/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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
#include "mongo/unittest/unittest.h"

using namespace mongo;

// ---------------------------------------------------------------------------
// Test helpers
// ---------------------------------------------------------------------------

inline RecordIdBound makeBound(int n) {
    return RecordIdBound(RecordId(n));
}

/**
 * Constructs a RecordIdRange with explicit finite bounds.
 */
inline RecordIdRange makeRange(int min, bool minInclusive, int max, bool maxInclusive) {
    RecordIdRange r;
    r.maybeNarrowMin(makeBound(min), minInclusive);
    r.maybeNarrowMax(makeBound(max), maxInclusive);
    return r;
}

/**
 * Constructs a RecordIdRange with only a lower bound (max = +∞).
 */
inline RecordIdRange makeRangeMinOnly(int min, bool minInclusive) {
    RecordIdRange r;
    r.maybeNarrowMin(makeBound(min), minInclusive);
    return r;
}

/**
 * Constructs a RecordIdRange with only an upper bound (min = -∞).
 */
inline RecordIdRange makeRangeMaxOnly(int max, bool maxInclusive) {
    RecordIdRange r;
    r.maybeNarrowMax(makeBound(max), maxInclusive);
    return r;
}

/**
 * Asserts that a range has the expected finite bounds and inclusivity.
 */
inline void assertRange(const RecordIdRange& r,
                        int expectedMin,
                        bool expectedMinInclusive,
                        int expectedMax,
                        bool expectedMaxInclusive) {
    ASSERT_TRUE(r.getMin()) << "Expected finite min";
    ASSERT_TRUE(r.getMax()) << "Expected finite max";
    ASSERT_EQ(r.getMin()->recordId(), RecordId(expectedMin));
    ASSERT_EQ(r.getMax()->recordId(), RecordId(expectedMax));
    ASSERT_EQ(r.isMinInclusive(), expectedMinInclusive);
    ASSERT_EQ(r.isMaxInclusive(), expectedMaxInclusive);
}

inline void assertRangeMinOnly(const RecordIdRange& r, int expectedMin, bool expectedMinInclusive) {
    ASSERT_TRUE(r.getMin()) << "Expected finite min";
    ASSERT_FALSE(r.getMax()) << "Expected absent max (+∞)";
    ASSERT_EQ(r.getMin()->recordId(), RecordId(expectedMin));
    ASSERT_EQ(r.isMinInclusive(), expectedMinInclusive);
}

inline void assertRangeMaxOnly(const RecordIdRange& r, int expectedMax, bool expectedMaxInclusive) {
    ASSERT_FALSE(r.getMin()) << "Expected absent min (-∞)";
    ASSERT_TRUE(r.getMax()) << "Expected finite max";
    ASSERT_EQ(r.getMax()->recordId(), RecordId(expectedMax));
    ASSERT_EQ(r.isMaxInclusive(), expectedMaxInclusive);
}

inline void assertRangeUnbounded(const RecordIdRange& r) {
    ASSERT_FALSE(r.getMin()) << "Expected absent min (-∞)";
    ASSERT_FALSE(r.getMax()) << "Expected absent max (+∞)";
}
