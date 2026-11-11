// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/plan_stats.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/unittest/unittest.h"

#include <memory>

namespace mongo {
namespace {

/**
 * Returns an unowned BSONObj that is a view into 'buffer'. Destroying or reassigning 'buffer'
 * invalidates the returned object, so callers must not use it afterwards.
 */
BSONObj makeUnownedView(BSONObj& buffer) {
    BSONObj unowned = BSONObj(buffer.objdata());
    ASSERT_FALSE(unowned.isOwned());
    return unowned;
}

// 'SpecificStats' objects are cloned into plan cache entries, which outlive the request that
// produced them. clone() must therefore return owned copies of any BSONObj members rather than
// views, since the buffer a view points into may already be gone by the time the cached entry is
// serialized by $planCacheStats.
TEST(MergeSortStats, CloneOwnsSortPattern) {
    auto owner = std::make_unique<BSONObj>(BSON("c" << 1));

    MergeSortStats original;
    original.sortPattern = makeUnownedView(*owner);
    ASSERT_FALSE(original.sortPattern.isOwned());

    auto cloned = original.clone();
    auto* clonedMergeSort = static_cast<MergeSortStats*>(cloned.get());
    ASSERT_TRUE(clonedMergeSort->sortPattern.isOwned());

    // Release the buffer the original view pointed into. The clone must be unaffected.
    owner.reset();
    ASSERT_BSONOBJ_EQ(clonedMergeSort->sortPattern, BSON("c" << 1));
}

TEST(MergeSortStats, CloneCopiesCounters) {
    MergeSortStats original;
    original.sortPattern = BSON("a" << 1 << "b" << -1);
    original.dupsTested = 7u;
    original.dupsDropped = 3u;

    auto cloned = original.clone();
    auto* clonedMergeSort = static_cast<MergeSortStats*>(cloned.get());
    ASSERT_BSONOBJ_EQ(clonedMergeSort->sortPattern, original.sortPattern);
    ASSERT_EQ(clonedMergeSort->dupsTested, 7u);
    ASSERT_EQ(clonedMergeSort->dupsDropped, 3u);
}

}  // namespace
}  // namespace mongo
