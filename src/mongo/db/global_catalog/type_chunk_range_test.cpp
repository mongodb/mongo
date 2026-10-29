// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/global_catalog/type_chunk_range.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/unittest/unittest.h"

#include <boost/optional/optional.hpp>

namespace mongo {
namespace {

TEST(ChunkRange, BasicBSONParsing) {
    const auto kMin = BSON("x" << 0);
    const auto kMax = BSON("x" << 10);

    auto chunkRange = ChunkRange::fromBSON(BSON("min" << kMin << "max" << kMax));
    ASSERT_BSONOBJ_EQ(chunkRange.getMin(), kMin);
    ASSERT_BSONOBJ_EQ(chunkRange.getMax(), kMax);
}

TEST(ChunkRange, Covers) {
    auto target = ChunkRange(BSON("x" << 5), BSON("x" << 10));
    ASSERT(!target.covers(ChunkRange(BSON("x" << 0), BSON("x" << 5))));
    ASSERT(!target.covers(ChunkRange(BSON("x" << 10), BSON("x" << 15))));
    ASSERT(!target.covers(ChunkRange(BSON("x" << 0), BSON("x" << 7))));
    ASSERT(!target.covers(ChunkRange(BSON("x" << 7), BSON("x" << 15))));
    ASSERT(!target.covers(ChunkRange(BSON("x" << 0), BSON("x" << 15))));
    ASSERT(!target.covers(ChunkRange(BSON("x" << 0), BSON("x" << 10))));
    ASSERT(!target.covers(ChunkRange(BSON("x" << 5), BSON("x" << 15))));
    ASSERT(target.covers(ChunkRange(BSON("x" << 5), BSON("x" << 10))));
    ASSERT(target.covers(ChunkRange(BSON("x" << 6), BSON("x" << 10))));
    ASSERT(target.covers(ChunkRange(BSON("x" << 5), BSON("x" << 9))));
    ASSERT(target.covers(ChunkRange(BSON("x" << 6), BSON("x" << 9))));
}

TEST(ChunkRange, Overlap) {
    auto target = ChunkRange(BSON("x" << 5), BSON("x" << 10));
    ASSERT(!target.overlapWith(ChunkRange(BSON("x" << 0), BSON("x" << 5))));
    ASSERT(!target.overlapWith(ChunkRange(BSON("x" << 0), BSON("x" << 4))));
    ASSERT(!target.overlapWith(ChunkRange(BSON("x" << 10), BSON("x" << 15))));
    ASSERT(!target.overlapWith(ChunkRange(BSON("x" << 11), BSON("x" << 15))));
    ASSERT(ChunkRange(BSON("x" << 7), BSON("x" << 10)) ==
           *target.overlapWith(ChunkRange(BSON("x" << 7), BSON("x" << 15))));
    ASSERT(ChunkRange(BSON("x" << 5), BSON("x" << 10)) ==
           *target.overlapWith(ChunkRange(BSON("x" << 0), BSON("x" << 10))));
    ASSERT(ChunkRange(BSON("x" << 5), BSON("x" << 10)) ==
           *target.overlapWith(ChunkRange(BSON("x" << 0), BSON("x" << 15))));
    ASSERT(ChunkRange(BSON("x" << 5), BSON("x" << 10)) ==
           *target.overlapWith(ChunkRange(BSON("x" << 5), BSON("x" << 15))));
    ASSERT(ChunkRange(BSON("x" << 5), BSON("x" << 9)) ==
           *target.overlapWith(ChunkRange(BSON("x" << 0), BSON("x" << 9))));
    ASSERT(ChunkRange(BSON("x" << 9), BSON("x" << 10)) ==
           *target.overlapWith(ChunkRange(BSON("x" << 9), BSON("x" << 15))));
}

TEST(ChunkRange, Union) {
    auto target = ChunkRange(BSON("x" << 5), BSON("x" << 10));
    ASSERT(ChunkRange(BSON("x" << 0), BSON("x" << 10)) ==
           target.unionWith(ChunkRange(BSON("x" << 0), BSON("x" << 5))));
    ASSERT(ChunkRange(BSON("x" << 0), BSON("x" << 10)) ==
           target.unionWith(ChunkRange(BSON("x" << 0), BSON("x" << 4))));
    ASSERT(ChunkRange(BSON("x" << 5), BSON("x" << 15)) ==
           target.unionWith(ChunkRange(BSON("x" << 10), BSON("x" << 15))));
    ASSERT(ChunkRange(BSON("x" << 5), BSON("x" << 15)) ==
           target.unionWith(ChunkRange(BSON("x" << 11), BSON("x" << 15))));
    ASSERT(ChunkRange(BSON("x" << 5), BSON("x" << 15)) ==
           target.unionWith(ChunkRange(BSON("x" << 7), BSON("x" << 15))));
    ASSERT(ChunkRange(BSON("x" << 0), BSON("x" << 10)) ==
           target.unionWith(ChunkRange(BSON("x" << 0), BSON("x" << 10))));
    ASSERT(ChunkRange(BSON("x" << 0), BSON("x" << 14)) ==
           target.unionWith(ChunkRange(BSON("x" << 0), BSON("x" << 14))));
    ASSERT(ChunkRange(BSON("x" << 5), BSON("x" << 15)) ==
           target.unionWith(ChunkRange(BSON("x" << 5), BSON("x" << 15))));
    ASSERT(ChunkRange(BSON("x" << 0), BSON("x" << 10)) ==
           target.unionWith(ChunkRange(BSON("x" << 0), BSON("x" << 9))));
    ASSERT(ChunkRange(BSON("x" << 5), BSON("x" << 15)) ==
           target.unionWith(ChunkRange(BSON("x" << 9), BSON("x" << 15))));
}

TEST(ChunkRange, ContainsKey) {
    auto target = ChunkRange(BSON("x" << 5), BSON("x" << 10));
    ASSERT_FALSE(target.containsKey(BSON("x" << MINKEY)));
    ASSERT_FALSE(target.containsKey(BSON("x" << 2)));
    ASSERT_TRUE(target.containsKey(BSON("x" << 5)));
    ASSERT_TRUE(target.containsKey(BSON("x" << 7)));
    ASSERT_FALSE(target.containsKey(BSON("x" << 10)));
    ASSERT_FALSE(target.containsKey(BSON("x" << 15)));
    ASSERT_FALSE(target.containsKey(BSON("x" << MAXKEY)));

    target = ChunkRange(BSON("x" << MINKEY), BSON("x" << 5));
    ASSERT_TRUE(target.containsKey(BSON("x" << MINKEY)));
    ASSERT_TRUE(target.containsKey(BSON("x" << 2)));
    ASSERT_FALSE(target.containsKey(BSON("x" << 5)));
    ASSERT_FALSE(target.containsKey(BSON("x" << 10)));
    ASSERT_FALSE(target.containsKey(BSON("x" << MAXKEY)));

    target = ChunkRange(BSON("x" << 5), BSON("x" << MAXKEY));
    ASSERT_FALSE(target.containsKey(BSON("x" << MINKEY)));
    ASSERT_FALSE(target.containsKey(BSON("x" << 2)));
    ASSERT_TRUE(target.containsKey(BSON("x" << 5)));
    ASSERT_TRUE(target.containsKey(BSON("x" << 10)));
    ASSERT_TRUE(target.containsKey(BSON("x" << MAXKEY)));
}

TEST(ChunkRange, ContainsKeyCompound) {
    auto target = ChunkRange(BSON("x" << 5 << "y" << 100), BSON("x" << 10 << "y" << 120));
    ASSERT_FALSE(target.containsKey(BSON("x" << MINKEY)));
    ASSERT_FALSE(target.containsKey(BSON("x" << MINKEY << "y" << 110)));
    ASSERT_FALSE(target.containsKey(BSON("x" << 2)));
    ASSERT_FALSE(target.containsKey(BSON("x" << 2 << "y" << 110)));
    ASSERT_FALSE(target.containsKey(BSON("x" << 5)));
    ASSERT_FALSE(target.containsKey(BSON("x" << 5 << "y" << 0)));
    ASSERT_TRUE(target.containsKey(BSON("x" << 5 << "y" << 120)));
    ASSERT_TRUE(target.containsKey(BSON("x" << 5 << "y" << MAXKEY)));
    ASSERT_TRUE(target.containsKey(BSON("x" << 5 << "y" << 100)));
    ASSERT_TRUE(target.containsKey(BSON("x" << 5 << "y" << 110)));
    ASSERT_TRUE(target.containsKey(BSON("x" << 7 << "y" << MINKEY)));
    ASSERT_TRUE(target.containsKey(BSON("x" << 7 << "y" << 100)));
    ASSERT_TRUE(target.containsKey(BSON("x" << 7 << "y" << 110)));
    ASSERT_TRUE(target.containsKey(BSON("x" << 7 << "y" << 120)));
    ASSERT_TRUE(target.containsKey(BSON("x" << 7 << "y" << MAXKEY)));
    ASSERT_TRUE(target.containsKey(BSON("x" << 10)));
    ASSERT_TRUE(target.containsKey(BSON("x" << 10 << "y" << 0)));
    ASSERT_TRUE(target.containsKey(BSON("x" << 10 << "y" << 100)));
    ASSERT_TRUE(target.containsKey(BSON("x" << 10 << "y" << 110)));
    ASSERT_FALSE(target.containsKey(BSON("x" << 10 << "y" << 120)));
    ASSERT_FALSE(target.containsKey(BSON("x" << 10 << "y" << MAXKEY)));
    ASSERT_FALSE(target.containsKey(BSON("x" << 15)));
    ASSERT_FALSE(target.containsKey(BSON("x" << MAXKEY)));
    ASSERT_FALSE(target.containsKey(BSON("x" << MAXKEY << "y" << 0)));
    ASSERT_FALSE(target.containsKey(BSON("x" << MAXKEY << "y" << 100)));
    ASSERT_FALSE(target.containsKey(BSON("x" << MAXKEY << "y" << 110)));
    ASSERT_FALSE(target.containsKey(BSON("x" << MAXKEY << "y" << 120)));
    ASSERT_FALSE(target.containsKey(BSON("x" << MAXKEY << "y" << MAXKEY)));

    target = ChunkRange(BSON("x" << 5 << "y" << 5), BSON("x" << MAXKEY << "y" << MAXKEY));
    ASSERT_FALSE(target.containsKey(BSON("x" << MINKEY << "y" << MAXKEY)));
    ASSERT_FALSE(target.containsKey(BSON("x" << 2 << "y" << MAXKEY)));
    ASSERT_TRUE(target.containsKey(BSON("x" << 5 << "y" << MAXKEY)));
    ASSERT_TRUE(target.containsKey(BSON("x" << MAXKEY << "y" << MAXKEY)));
}

TEST(ChunkRange, ParaseMinGreaterThanMaxShouldError) {
    const auto kMin = BSON("x" << 10);
    const auto kMax = BSON("x" << 0);

    const auto bsonRange = BSON("min" << kMin << "max" << kMax);

    ASSERT_THROWS_CODE(ChunkRange::fromBSON(bsonRange), DBException, ErrorCodes::BadValue);
}

TEST(ChunkRange, ParseMinEqualToMaxShouldError) {
    const auto kMin = BSON("x" << 10);
    const auto kMax = BSON("x" << 10);

    const auto bsonRange = BSON("min" << kMin << "max" << kMax);

    ASSERT_THROWS_CODE(ChunkRange::fromBSON(bsonRange), DBException, ErrorCodes::BadValue);
}

TEST(ChunkRange, ParseNotStrict) {
    const auto kMin = BSON("x" << 10);
    const auto kMax = BSON("x" << 20);

    const auto bsonRange = BSON("_id" << 4 << "unknownField"
                                      << "X"
                                      << "min" << kMin << "max" << kMax);

    ChunkRange::fromBSON(bsonRange);
}

}  // namespace
}  // namespace mongo
