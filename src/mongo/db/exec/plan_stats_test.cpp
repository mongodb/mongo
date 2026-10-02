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
#include "mongo/db/exec/plan_stats.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/unittest/unittest.h"

#include <memory>

namespace mongo {
namespace {

// 'SpecificStats' objects are cloned into plan cache entries, which outlive the request that
// produced them. clone() must therefore return owned copies of any BSONObj members rather than
// views, since the buffer a view points into may already be gone by the time the cached entry is
// serialized by $planCacheStats.
TEST(MergeSortStats, CloneOwnsSortPattern) {
    auto owner = std::make_unique<BSONObj>(BSON("c" << 1));

    MergeSortStats original;
    // An unowned view into 'owner''s buffer. Releasing 'owner' below invalidates it.
    original.sortPattern = BSONObj(owner->objdata());
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
