/**
 *    Copyright (C) 2017 MongoDB Inc.
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
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/db/jsobj.h"
#include "mongo/db/s/chunk_size_tracker.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

using unittest::assertGet;

TEST(ChunkSizeTrackerTest, NotSplitThenSplitTest) {

    ChunkSizeTracker chunkSizeTracker;

    // Random keys
    BSONObj key1 = BSON("foo" << 0);
    BSONObj key2 = BSON("foo" << 33);

    uint64_t maxChunkSize = 1048576;  // 1 MB
    uint64_t deltaBytes = 104858;     // Slightly more than 10% of 1 MB

    // The first doesn't split, as we do not exceed 20% of the max chunk size.
    // The second one exceeds 20%, and we should split.
    ASSERT_FALSE(chunkSizeTracker.noteBytes(key1, key2, deltaBytes, maxChunkSize));
    ASSERT_TRUE(chunkSizeTracker.noteBytes(key1, key2, deltaBytes, maxChunkSize));
}

TEST(ChunkSizeTrackerTest, NotSplitTest) {

    ChunkSizeTracker chunkSizeTracker;

    // Random keys
    BSONObj minKey = BSON("foo" << 33);
    BSONObj maxKey = BSON("foo" << 180);

    uint64_t maxChunkSize = 1048576;  // 1 MB
    uint64_t deltaBytes = 209715;     // Slightly less than 20% of 1 MB

    // If the chunk size == split threshold, we do not split
    ASSERT_FALSE(chunkSizeTracker.noteBytes(minKey, maxKey, deltaBytes, maxChunkSize));
}

TEST(ChunkSizeTrackerTest, SplitTest) {

    ChunkSizeTracker chunkSizeTracker;

    // Random keys
    BSONObj minKey = BSON("foo" << 180);
    BSONObj maxKey = BSON("foo" << 2309);

    uint64_t maxChunkSize = 1048576;  // 1 MB
    uint64_t deltaBytes = 209716;     // Slightly more than 20% of 1 MB

    // If the chunk size > split threshold, we split
    ASSERT_TRUE(chunkSizeTracker.noteBytes(minKey, maxKey, deltaBytes, maxChunkSize));
}

TEST(ChunkSizeTrackerTest, ResetTest) {

    ChunkSizeTracker chunkSizeTracker;

    // Random keys
    BSONObj minKey = BSON("foo" << 2309);
    BSONObj maxKey = BSON("foo" << 3331);

    uint64_t maxChunkSize = 1048576;  // 1 MB
    uint64_t deltaBytes = 209715;     // Slightly less than 20% of 1 MB

    // Every time the function returns true, it should reset to 0, and hence a future
    // insert should return false.
    ASSERT_FALSE(chunkSizeTracker.noteBytes(minKey, maxKey, deltaBytes, maxChunkSize));
    ASSERT_TRUE(chunkSizeTracker.noteBytes(minKey, maxKey, deltaBytes, maxChunkSize));
    ASSERT_FALSE(chunkSizeTracker.noteBytes(minKey, maxKey, deltaBytes, maxChunkSize));
    ASSERT_TRUE(chunkSizeTracker.noteBytes(minKey, maxKey, deltaBytes, maxChunkSize));
    ASSERT_FALSE(chunkSizeTracker.noteBytes(minKey, maxKey, deltaBytes, maxChunkSize));
    ASSERT_TRUE(chunkSizeTracker.noteBytes(minKey, maxKey, deltaBytes, maxChunkSize));
}

TEST(ChunkSizeTrackerTest, ForgetRangeTest) {

    ChunkSizeTracker chunkSizeTracker;

    // Random keys
    BSONObj key1 = BSON("foo" << 33);
    BSONObj key2 = BSON("foo" << 180);
    BSONObj key3 = BSON("foo" << 2309);
    BSONObj key4 = BSON("foo" << 3331);

    uint64_t maxChunkSize = 1048576;  // 1 MB
    uint64_t deltaBytes = 209715;     // Slightly less than 20% of 1 MB

    // Because the deltaBytes is less than 20% of the maxChunkSize, we should not split.
    ASSERT_FALSE(chunkSizeTracker.noteBytes(key1, key2, deltaBytes, maxChunkSize));
    ASSERT_FALSE(chunkSizeTracker.noteBytes(key2, key3, deltaBytes, maxChunkSize));
    ASSERT_FALSE(chunkSizeTracker.noteBytes(key3, key4, deltaBytes, maxChunkSize));

    // Now forget the ranges [key1, key2], [key2, key3] from the chunk size tracker.
    chunkSizeTracker.forgetRange(key1, key2);
    chunkSizeTracker.forgetRange(key2, key3);

    // Because the entries are deleted from the map, incrementing the bytes at the chunks
    // [key1, key2] and [key2, key3] should again return false. However, because the
    // chunk with the range [key3, key4] was not reset, it will return true on another
    // increment.
    ASSERT_FALSE(chunkSizeTracker.noteBytes(key1, key2, deltaBytes, maxChunkSize));
    ASSERT_FALSE(chunkSizeTracker.noteBytes(key2, key3, deltaBytes, maxChunkSize));
    ASSERT_TRUE(chunkSizeTracker.noteBytes(key3, key4, deltaBytes, maxChunkSize));

    // On another increment, we should have the opposite. [key1, key2] and [key2, key3]
    // should now exceed the threshold, and [key3, key4] will not, due to it being
    // reset on the previous increment.
    ASSERT_TRUE(chunkSizeTracker.noteBytes(key1, key2, deltaBytes, maxChunkSize));
    ASSERT_TRUE(chunkSizeTracker.noteBytes(key2, key3, deltaBytes, maxChunkSize));
    ASSERT_FALSE(chunkSizeTracker.noteBytes(key3, key4, deltaBytes, maxChunkSize));
}

TEST(ChunkSizeTrackerTest, ThrowsExceptionTest) {

    ChunkSizeTracker chunkSizeTracker;

    // Random keys
    BSONObj key1 = BSON("foo" << 0);
    BSONObj key2 = BSON("foo" << 33);
    BSONObj key3 = BSON("foo" << 34);

    uint64_t maxChunkSize = 1048576;  // 1 MB
    uint64_t deltaBytes = 104858;     // Slightly more than 10% of 1 MB

    // The first doesn't split, as we do not exceed 20% of the max chunk size.
    // The second causes an error, as we try to update on [key1, key3] when [key1, key2] already
    // exists.
    ASSERT_FALSE(chunkSizeTracker.noteBytes(key1, key2, deltaBytes, maxChunkSize));
    ASSERT_THROWS(chunkSizeTracker.noteBytes(key1, key3, deltaBytes, maxChunkSize), UserException);
}

}  // namespace
}  // namespace mongo
