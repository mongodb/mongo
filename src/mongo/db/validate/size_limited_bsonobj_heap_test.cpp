// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/db/validate/size_limited_bsonobj_heap.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/timestamp.h"
#include "mongo/unittest/unittest.h"

#include <vector>

namespace mongo {

namespace {
BSONObj makeSizedDoc(size_t bytes) {
    // Build a document whose total size is approximately `bytes`.
    // We just need relative sizes to differ, not exact equality.
    BSONObjBuilder bob;
    // '_id' string length will dominate the objsize.
    bob.append("_id", std::string(bytes, 'a'));
    return bob.obj();
}
}  // namespace

TEST(SizeLimitedBSONObjHeapTest, AddWithinLimitNoEviction) {
    const size_t maxBytes = 500 * 1024;
    SizeLimitedBSONObjHeap heap(maxBytes);

    BSONObj a = makeSizedDoc(100 * 1024);
    BSONObj b = makeSizedDoc(150 * 1024);

    EXPECT_FALSE(heap.add(a));
    EXPECT_FALSE(heap.add(b));

    EXPECT_EQ(heap.size(), 2u);
    EXPECT_LE(heap.usedBytes(), maxBytes);
}

TEST(SizeLimitedBSONObjHeapTest, AddExceedingLimitEvictsLargest) {
    const size_t maxBytes = 400 * 1024;
    SizeLimitedBSONObjHeap heap(maxBytes);

    BSONObj small = makeSizedDoc(100 * 1024);
    BSONObj medium = makeSizedDoc(150 * 1024);
    BSONObj large = makeSizedDoc(300 * 1024);

    EXPECT_FALSE(heap.add(small));   // ~100 KB
    EXPECT_FALSE(heap.add(medium));  // ~250 KB total

    const bool evicted = heap.add(large);  // ~550 KB total > 400 KB
    EXPECT_TRUE(evicted);
    EXPECT_EQ(heap.size(), 2u);
    EXPECT_LE(heap.usedBytes(), maxBytes);

    // All remaining entries should be <= large in size; in practice we expect
    // the largest object (large) to have been evicted.
    for (const auto& obj : heap.entries()) {
        EXPECT_LE(static_cast<size_t>(obj.objsize()), static_cast<size_t>(large.objsize()));
    }
}

TEST(SizeLimitedBSONObjHeapTest, AlwaysKeepsAtLeastOneElement) {
    // maxBytes is tiny compared to any doc we insert.
    const size_t maxBytes = 1;
    SizeLimitedBSONObjHeap heap(maxBytes);

    BSONObj big1 = makeSizedDoc(128 * 1024);
    BSONObj big2 = makeSizedDoc(256 * 1024);

    // First add: size() goes to 1, allowed even though usedBytes > maxBytes.
    EXPECT_FALSE(heap.add(big1));
    EXPECT_EQ(heap.size(), 1u);

    // Second add: must evict, but size() should still be 1.
    const bool evicted = heap.add(big2);
    EXPECT_TRUE(evicted);
    EXPECT_EQ(heap.size(), 1u);
}

TEST(SizeLimitedBSONObjHeapTest, PrefersSmallerEntryWhenOverLimit) {
    // Force a situation where only one entry fits; smaller one should win.
    const size_t maxBytes = 200 * 1024;
    SizeLimitedBSONObjHeap heap(maxBytes);

    BSONObj large = makeSizedDoc(190 * 1024);
    BSONObj small = makeSizedDoc(50 * 1024);

    EXPECT_FALSE(heap.add(large));
    const bool evicted = heap.add(small);
    EXPECT_TRUE(evicted);
    EXPECT_EQ(heap.size(), 1u);

    const BSONObj& remaining = heap.entries().front();
    EXPECT_LE(static_cast<size_t>(remaining.objsize()), static_cast<size_t>(small.objsize()));
}

TEST(SizeLimitedBSONObjHeapTest, ClearResetsState) {
    const size_t maxBytes = 300 * 1024;
    SizeLimitedBSONObjHeap heap(maxBytes);

    BSONObj a = makeSizedDoc(100 * 1024);
    BSONObj b = makeSizedDoc(150 * 1024);

    EXPECT_FALSE(heap.add(a));
    EXPECT_FALSE(heap.add(b));
    EXPECT_EQ(heap.size(), 2u);
    EXPECT_GT(heap.usedBytes(), 0u);

    heap.clear();
    EXPECT_EQ(heap.size(), 0u);
    EXPECT_EQ(heap.usedBytes(), 0u);

    // After clear, adding again should behave like a fresh heap.
    EXPECT_FALSE(heap.add(a));
    EXPECT_EQ(heap.size(), 1u);
    EXPECT_GT(heap.usedBytes(), 0u);
}

TEST(SizeLimitedBSONObjHeapTest, EvictsNewlyAddedObjectWhenLargest) {
    const size_t maxBytes = 250 * 1024;
    SizeLimitedBSONObjHeap heap(maxBytes);

    BSONObj a = makeSizedDoc(100 * 1024);
    BSONObj b = makeSizedDoc(100 * 1024);
    BSONObj big = makeSizedDoc(300 * 1024);

    EXPECT_FALSE(heap.add(a));
    EXPECT_FALSE(heap.add(b));
    const size_t usedBefore = heap.usedBytes();

    // `big` is the largest object, so it should be the one evicted, leaving the
    // heap in exactly its prior state.
    EXPECT_TRUE(heap.add(big));
    EXPECT_EQ(heap.size(), 2u);
    EXPECT_EQ(heap.usedBytes(), usedBefore);
    EXPECT_LE(heap.usedBytes(), maxBytes);

    // The just-added `big` must not be present.
    for (const auto& obj : heap.entries()) {
        EXPECT_LT(static_cast<size_t>(obj.objsize()), static_cast<size_t>(big.objsize()));
    }
}

TEST(SizeLimitedBSONObjHeapTest, ZeroMaxBytesKeepsSingleElement) {
    SizeLimitedBSONObjHeap heap(0);

    BSONObj a = makeSizedDoc(100 * 1024);
    BSONObj b = makeSizedDoc(150 * 1024);

    // First add is always kept because size() <= 1, even though it exceeds the
    // limit and usedBytes > maxBytes.
    EXPECT_FALSE(heap.add(a));
    EXPECT_EQ(heap.size(), 1u);
    EXPECT_GT(heap.usedBytes(), heap.maxBytes());

    // Second add must evict to stay at a single element.
    EXPECT_TRUE(heap.add(b));
    EXPECT_EQ(heap.size(), 1u);
}

TEST(SizeLimitedBSONObjHeapTest, EqualSizedObjectsEvictAndStayUnderLimit) {
    // Three equally-sized docs with a limit that holds two of them. The
    // comparator only inspects objsize(); equal sizes must not crash and must
    // still enforce the limit.
    BSONObj a = makeSizedDoc(100 * 1024);
    BSONObj b = makeSizedDoc(100 * 1024);
    BSONObj c = makeSizedDoc(100 * 1024);
    const size_t docSize = static_cast<size_t>(a.objsize());
    SizeLimitedBSONObjHeap heap(docSize * 2 + docSize / 2);

    EXPECT_FALSE(heap.add(a));
    EXPECT_FALSE(heap.add(b));
    EXPECT_TRUE(heap.add(c));

    EXPECT_EQ(heap.size(), 2u);
    EXPECT_LE(heap.usedBytes(), heap.maxBytes());
}

TEST(SizeLimitedBSONObjHeapTest, EmptyReflectsState) {
    SizeLimitedBSONObjHeap heap(300 * 1024);
    EXPECT_TRUE(heap.empty());

    EXPECT_FALSE(heap.add(makeSizedDoc(100 * 1024)));
    EXPECT_FALSE(heap.empty());

    heap.clear();
    EXPECT_TRUE(heap.empty());
}

}  // namespace mongo
