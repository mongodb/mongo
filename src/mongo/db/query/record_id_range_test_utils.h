// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

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
