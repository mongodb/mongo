/**
 * Copyright (C) 2018 MongoDB Inc.
 *
 * This program is free software: you can redistribute it and/or  modify
 * it under the terms of the GNU Affero General Public License, version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, the copyright holders give permission to link the
 * code of portions of this program with the OpenSSL library under certain
 * conditions as described in each individual source file and distribute
 * linked combinations including the program with the OpenSSL library. You
 * must comply with the GNU Affero General Public License in all respects
 * for all of the code used other than as permitted herein. If you modify
 * file(s) with this exception, you may extend this exception to your
 * version of the file(s), but you are not obligated to do so. If you do not
 * wish to do so, delete this exception statement from your version. If you
 * delete this exception statement from all source files in the program,
 * then also delete it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/s/chunk_writes_tracker.h"

#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"


namespace mongo {

TEST(ChunkWritesTrackerTest, BytesWrittenStartsAtZero) {
    ChunkWritesTracker wt;
    ASSERT_EQ(wt.getBytesWritten(), 0ull);
}

TEST(ChunkWritesTrackerTest, AddBytesWrittenCorrectlyAddsBytes) {
    ChunkWritesTracker wt;
    uint64_t bytesToAdd{4};
    wt.addBytesWritten(bytesToAdd);
    ASSERT_EQ(wt.getBytesWritten(), bytesToAdd);
}

TEST(ChunkWritesTrackerTest, ClearBytesWrittenSetsBytesToZero) {
    ChunkWritesTracker wt;
    wt.addBytesWritten(4ull);
    wt.clearBytesWritten();
    ASSERT_EQ(wt.getBytesWritten(), 0ull);
}

TEST(ChunkWritesTrackerTest, ShouldSplitReturnsTrueWithBytesWrittenAndMaxChunkSizeZero) {
    ChunkWritesTracker wt;
    wt.addBytesWritten(4ull);
    uint64_t maxChunkSize{0};
    ASSERT_TRUE(wt.shouldSplit(maxChunkSize));
}

TEST(ChunkWritesTrackerTest, ShouldSplitReturnsFalseWithNoBytesWrittenAndMaxChunkSizeZero) {
    ChunkWritesTracker wt;
    uint64_t maxChunkSize{0};
    ASSERT_FALSE(wt.shouldSplit(maxChunkSize));
}

TEST(ChunkWritesTrackerTest, ShouldSplitReturnsTrueWithBytesWrittenGreaterThanMaxChunkSize) {
    ChunkWritesTracker wt;
    wt.addBytesWritten(4ull);
    uint64_t maxChunkSize{3};
    ASSERT_TRUE(wt.shouldSplit(maxChunkSize));
}

TEST(ChunkWritesTrackerTest, ShouldSplitReturnsFalseWithBytesWrittenLessThanThreshold) {
    ChunkWritesTracker wt;
    uint64_t maxChunkSize{10};
    auto expectedCutoff = maxChunkSize / ChunkWritesTracker::kSplitTestFactor;
    wt.addBytesWritten(expectedCutoff - 1);
    ASSERT_FALSE(wt.shouldSplit(maxChunkSize));
    wt.addBytesWritten(1ul);
    ASSERT_FALSE(wt.shouldSplit(maxChunkSize));
    wt.addBytesWritten(1ul);
    ASSERT_TRUE(wt.shouldSplit(maxChunkSize));
}

TEST(ChunkWritesTrackerTest, ShouldSplitReturnsFalseWithBytesWrittenEqualToThreshold) {
    ChunkWritesTracker wt;
    uint64_t maxChunkSize{10};
    auto expectedCutoff = maxChunkSize / ChunkWritesTracker::kSplitTestFactor;
    wt.addBytesWritten(expectedCutoff);
    ASSERT_FALSE(wt.shouldSplit(maxChunkSize));
}

TEST(ChunkWritesTrackerTest, ShouldSplitReturnsFalseWithBytesWrittenGreaterThanThreshold) {
    ChunkWritesTracker wt;
    uint64_t maxChunkSize{10};
    auto expectedCutoff = maxChunkSize / ChunkWritesTracker::kSplitTestFactor;
    wt.addBytesWritten(expectedCutoff + 1);
    ASSERT_TRUE(wt.shouldSplit(maxChunkSize));
}

TEST(ChunkWritesTrackerTest, ShouldSplitReturnsFalseWhenSplitLockAcquired) {
    ChunkWritesTracker wt;
    wt.addBytesWritten(4ull);
    wt.acquireSplitLock();
    uint64_t maxChunkSize{0};
    ASSERT_FALSE(wt.shouldSplit(maxChunkSize));
}

TEST(ChunkWritesTrackerTest, ShouldSplitReturnsTrueAfterSplitLockReleased) {
    ChunkWritesTracker wt;
    wt.addBytesWritten(4ull);
    wt.acquireSplitLock();
    wt.releaseSplitLock();
    uint64_t maxChunkSize{0};
    ASSERT_TRUE(wt.shouldSplit(maxChunkSize));
}

TEST(ChunkWritesTrackerTest, AcquireSplitLockReturnsFalseAfterReturningTrue) {
    ChunkWritesTracker wt;
    wt.addBytesWritten(4ull);
    ASSERT_TRUE(wt.acquireSplitLock());
    ASSERT_FALSE(wt.acquireSplitLock());
}

TEST(ChunkWritesTrackerTest, AcquireSplitLockThenReleaseThenReacquireReturnsTrue) {
    ChunkWritesTracker wt;
    wt.addBytesWritten(4ull);
    wt.acquireSplitLock();
    wt.releaseSplitLock();
    ASSERT_TRUE(wt.acquireSplitLock());
}

DEATH_TEST(ChunkWritesTrackerTest, ReleaseSplitLockWithoutAcquiringErrors, "Invariant failure") {
    ChunkWritesTracker wt;
    wt.releaseSplitLock();
}

}  // namespace mongo
