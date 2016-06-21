/**
 *    Copyright (C) 2016 MongoDB Inc.
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
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/db/s/metadata_manager.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/s/collection_metadata.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/stdx/memory.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

namespace mongo {

using unittest::assertGet;

namespace {

TEST(MetadataManager, SetAndGetActiveMetadata) {
    MetadataManager manager;

    std::unique_ptr<CollectionMetadata> cm = stdx::make_unique<CollectionMetadata>();
    auto cmPtr = cm.get();

    manager.setActiveMetadata(std::move(cm));
    ScopedCollectionMetadata scopedMetadata = manager.getActiveMetadata();

    ASSERT_EQ(cmPtr, scopedMetadata.getMetadata());
};

TEST(MetadataManager, ResetActiveMetadata) {
    MetadataManager manager;
    manager.setActiveMetadata(stdx::make_unique<CollectionMetadata>());

    ScopedCollectionMetadata scopedMetadata1 = manager.getActiveMetadata();

    std::unique_ptr<CollectionMetadata> cm2 = stdx::make_unique<CollectionMetadata>();
    auto cm2Ptr = cm2.get();

    manager.setActiveMetadata(std::move(cm2));
    ScopedCollectionMetadata scopedMetadata2 = manager.getActiveMetadata();

    ASSERT_EQ(cm2Ptr, scopedMetadata2.getMetadata());
};

TEST(MetadataManager, AddAndRemoveRanges) {
    MetadataManager mm;
    ChunkRange cr1 = ChunkRange(BSON("key" << 0), BSON("key" << 10));
    ChunkRange cr2 = ChunkRange(BSON("key" << 10), BSON("key" << 20));

    mm.addRangeToClean(cr1);
    ASSERT_EQ(mm.getCopyOfRanges().size(), 1UL);
    mm.removeRangeToClean(cr1);
    ASSERT_EQ(mm.getCopyOfRanges().size(), 0UL);

    mm.addRangeToClean(cr1);
    mm.addRangeToClean(cr2);
    mm.removeRangeToClean(cr1);
    ASSERT_EQ(mm.getCopyOfRanges().size(), 1UL);
    auto ranges = mm.getCopyOfRanges();
    ChunkRange remainingChunk = ranges.find(cr2.getMin())->second;
    ASSERT_EQ(remainingChunk.toString(), cr2.toString());
    mm.removeRangeToClean(cr2);
}

// Tests that a removal in the middle of an existing ChunkRange results in
// two correct chunk ranges.
TEST(MetadataManager, RemoveRangeInMiddleOfRange) {
    MetadataManager mm;
    ChunkRange cr1 = ChunkRange(BSON("key" << 0), BSON("key" << 10));

    mm.addRangeToClean(cr1);
    mm.removeRangeToClean(ChunkRange(BSON("key" << 4), BSON("key" << 6)));
    ASSERT_EQ(mm.getCopyOfRanges().size(), 2UL);

    auto ranges = mm.getCopyOfRanges();
    auto it = ranges.find(BSON("key" << 0));
    ChunkRange expectedChunk = ChunkRange(BSON("key" << 0), BSON("key" << 4));
    ASSERT_EQ(it->second.toString(), expectedChunk.toString());

    it++;
    expectedChunk = ChunkRange(BSON("key" << 6), BSON("key" << 10));
    ASSERT_EQ(it->second.toString(), expectedChunk.toString());

    mm.removeRangeToClean(cr1);
    ASSERT_EQ(mm.getCopyOfRanges().size(), 0UL);
}

// Tests removals that overlap with just one ChunkRange.
TEST(MetadataManager, RemoveRangeWithSingleRangeOverlap) {
    MetadataManager mm;
    ChunkRange cr1 = ChunkRange(BSON("key" << 0), BSON("key" << 10));

    mm.addRangeToClean(cr1);
    mm.removeRangeToClean(ChunkRange(BSON("key" << 0), BSON("key" << 5)));
    ASSERT_EQ(mm.getCopyOfRanges().size(), 1UL);
    auto ranges = mm.getCopyOfRanges();
    ChunkRange remainingChunk = ranges.find(BSON("key" << 5))->second;
    ChunkRange expectedChunk = ChunkRange(BSON("key" << 5), BSON("key" << 10));
    ASSERT_EQ(remainingChunk.toString(), expectedChunk.toString());

    mm.removeRangeToClean(ChunkRange(BSON("key" << 4), BSON("key" << 6)));
    ASSERT_EQ(mm.getCopyOfRanges().size(), 1UL);
    ranges = mm.getCopyOfRanges();
    remainingChunk = ranges.find(BSON("key" << 6))->second;
    expectedChunk = ChunkRange(BSON("key" << 6), BSON("key" << 10));
    ASSERT_EQ(remainingChunk.toString(), expectedChunk.toString());

    mm.removeRangeToClean(ChunkRange(BSON("key" << 9), BSON("key" << 13)));
    ASSERT_EQ(mm.getCopyOfRanges().size(), 1UL);
    ranges = mm.getCopyOfRanges();
    remainingChunk = ranges.find(BSON("key" << 6))->second;
    expectedChunk = ChunkRange(BSON("key" << 6), BSON("key" << 9));
    ASSERT_EQ(remainingChunk.toString(), expectedChunk.toString());

    mm.removeRangeToClean(ChunkRange(BSON("key" << 0), BSON("key" << 10)));
    ASSERT_EQ(mm.getCopyOfRanges().size(), 0UL);
}

// Tests removals that overlap with more than one ChunkRange.
TEST(MetadataManager, RemoveRangeWithMultipleRangeOverlaps) {
    MetadataManager mm;
    ChunkRange cr1 = ChunkRange(BSON("key" << 0), BSON("key" << 10));
    ChunkRange cr2 = ChunkRange(BSON("key" << 10), BSON("key" << 20));
    ChunkRange cr3 = ChunkRange(BSON("key" << 20), BSON("key" << 30));

    mm.addRangeToClean(cr1);
    mm.addRangeToClean(cr2);
    mm.addRangeToClean(cr3);
    ASSERT_EQ(mm.getCopyOfRanges().size(), 3UL);

    mm.removeRangeToClean(ChunkRange(BSON("key" << 8), BSON("key" << 22)));
    ASSERT_EQ(mm.getCopyOfRanges().size(), 2UL);
    auto ranges = mm.getCopyOfRanges();
    auto it = ranges.find(BSON("key" << 0));
    ChunkRange expectedChunk = ChunkRange(BSON("key" << 0), BSON("key" << 8));
    ASSERT_EQ(it->second.toString(), expectedChunk.toString());
    it++;
    expectedChunk = ChunkRange(BSON("key" << 22), BSON("key" << 30));
    ASSERT_EQ(it->second.toString(), expectedChunk.toString());

    mm.removeRangeToClean(ChunkRange(BSON("key" << 0), BSON("key" << 30)));
    ASSERT_EQ(mm.getCopyOfRanges().size(), 0UL);
}

}  // namespace
}  // namespace mongo
