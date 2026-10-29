// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/pipeline/sequential_document_cache.h"

#include "mongo/bson/json.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/document_value_test_util.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"

#include <string>
#include <vector>


namespace mongo {
namespace {

const size_t kCacheSizeBytes = 1024;

TEST(SequentialDocumentCacheTest, CacheIsInBuildingModeUponInstantiation) {
    SequentialDocumentCache cache(kCacheSizeBytes);
    ASSERT(cache.isBuilding());
}

TEST(SequentialDocumentCacheTest, CanAddDocumentsToCacheWhileBuilding) {
    SequentialDocumentCache cache(kCacheSizeBytes);
    ASSERT(cache.isBuilding());

    cache.add(DOC("_id" << 0));
    cache.add(DOC("_id" << 1));

    ASSERT_EQ(cache.count(), 2ul);
}

DEATH_TEST(SequentialDocumentCacheTestDeathTest,
           CannotIterateCacheWhileBuilding,
           "Tripwire assertion") {
    SequentialDocumentCache cache(kCacheSizeBytes);
    ASSERT(cache.isBuilding());

    cache.getNext();
}

DEATH_TEST(SequentialDocumentCacheTestDeathTest,
           CannotRestartCacheIterationWhileBuilding,
           "Tripwire assertion") {
    SequentialDocumentCache cache(kCacheSizeBytes);
    ASSERT(cache.isBuilding());

    cache.restartIteration();
}

TEST(SequentialDocumentCacheTest, CanIterateCacheAfterFreezing) {
    SequentialDocumentCache cache(kCacheSizeBytes);
    ASSERT(cache.isBuilding());

    cache.add(DOC("_id" << 0));
    cache.add(DOC("_id" << 1));

    ASSERT_EQ(cache.count(), 2ul);

    cache.freeze();

    ASSERT_DOCUMENT_EQ(*cache.getNext(), DOC("_id" << 0));
    ASSERT_DOCUMENT_EQ(*cache.getNext(), DOC("_id" << 1));
    ASSERT_FALSE(cache.getNext().has_value());
}

TEST(SequentialDocumentCacheTest, CanRestartCacheIterationAfterFreezing) {
    SequentialDocumentCache cache(kCacheSizeBytes);
    ASSERT(cache.isBuilding());

    cache.add(DOC("_id" << 0));
    cache.add(DOC("_id" << 1));

    ASSERT_EQ(cache.count(), 2ul);

    cache.freeze();

    ASSERT_DOCUMENT_EQ(*cache.getNext(), DOC("_id" << 0));
    ASSERT_DOCUMENT_EQ(*cache.getNext(), DOC("_id" << 1));
    ASSERT_FALSE(cache.getNext().has_value());

    cache.restartIteration();

    ASSERT_DOCUMENT_EQ(*cache.getNext(), DOC("_id" << 0));
    ASSERT_DOCUMENT_EQ(*cache.getNext(), DOC("_id" << 1));
    ASSERT_FALSE(cache.getNext().has_value());
}

DEATH_TEST(SequentialDocumentCacheTestDeathTest,
           CannotAddDocumentsToCacheAfterFreezing,
           "Tripwire assertion") {
    SequentialDocumentCache cache(kCacheSizeBytes);
    cache.freeze();

    ASSERT(cache.isServing());

    cache.add(DOC("_id" << 0));
}

TEST(SequentialDocumentCacheTest, ShouldAbandonCacheIfMaxSizeBytesExceeded) {
    SequentialDocumentCache cache(0);
    ASSERT(cache.isBuilding());

    cache.add(DOC("_id" << 0));

    ASSERT(cache.isAbandoned());
}

DEATH_TEST(SequentialDocumentCacheTestDeathTest,
           CannotAddDocumentsToAbandonedCache,
           "Tripwire assertion") {
    SequentialDocumentCache cache(kCacheSizeBytes);
    cache.abandon();

    cache.add(DOC("_id" << 0));
}

DEATH_TEST(SequentialDocumentCacheTestDeathTest,
           CannotFreezeCacheWhenAbandoned,
           "Tripwire assertion") {
    SequentialDocumentCache cache(kCacheSizeBytes);
    cache.abandon();

    cache.freeze();
}

DEATH_TEST(SequentialDocumentCacheTestDeathTest,
           CannotRestartCacheIterationWhenAbandoned,
           "Tripwire assertion") {
    SequentialDocumentCache cache(kCacheSizeBytes);
    ASSERT(cache.isBuilding());

    cache.abandon();

    cache.restartIteration();
}

DEATH_TEST(SequentialDocumentCacheTestDeathTest,
           CannotCallGetNextWhenAbandoned,
           "Tripwire assertion") {
    SequentialDocumentCache cache(kCacheSizeBytes);
    ASSERT(cache.isBuilding());

    cache.add(DOC("_id" << 0));
    cache.add(DOC("_id" << 1));

    cache.abandon();

    cache.getNext();
}

// --- BSON materialization on freeze() ---

// freeze() materializes modified (non-trivially-convertible) documents into owned BSON so that
// downstream consumers can match against the whole document instead of re-serializing a projection.
// The served document must preserve its content and be trivially convertible afterward.
TEST(SequentialDocumentCacheTest, MaterializesModifiedDocumentsOnFreeze) {
    SequentialDocumentCache cache(kCacheSizeBytes);

    MutableDocument md(DOC("_id" << 0));
    md.setField("computed", Value(42));
    Document modified = md.freeze();
    // Precondition: a mutated document is not trivially convertible.
    ASSERT_FALSE(modified.toBsonIfTriviallyConvertible().has_value());
    cache.add(modified);

    cache.freeze();

    auto served = cache.getNext();
    ASSERT_TRUE(served.has_value());
    // Content is preserved ...
    ASSERT_DOCUMENT_EQ(*served, DOC("_id" << 0 << "computed" << 42));
    // ... and the served document has been materialized into owned BSON.
    ASSERT_TRUE(served->toBsonIfTriviallyConvertible().has_value());
}

// Values, types, nesting and array contents survive materialization unchanged.
TEST(SequentialDocumentCacheTest, PreservesModifiedDocumentValuesAndTypesOnFreeze) {
    SequentialDocumentCache cache(kCacheSizeBytes);

    MutableDocument md(Document(fromjson("{_id: 0}")));
    md.setField("s", Value("str"sv));
    md.setField("arr", Value(std::vector<Value>{Value(1), Value(2)}));
    md.setField("nested", Value(Document(fromjson("{a: 1}"))));
    md.setField("d", Value(3.5));
    cache.add(md.freeze());

    cache.freeze();

    auto served = cache.getNext();
    ASSERT_TRUE(served.has_value());
    ASSERT_DOCUMENT_EQ(
        *served, Document(fromjson("{_id: 0, s: 'str', arr: [1, 2], nested: {a: 1}, d: 3.5}")));
    ASSERT_TRUE(served->toBsonIfTriviallyConvertible().has_value());
}

// Documents carrying metadata are left as-is by materialization (which would otherwise strip
// metadata via toBson()), so their metadata survives freeze().
TEST(SequentialDocumentCacheTest, PreservesDocumentMetadataOnFreeze) {
    SequentialDocumentCache cache(kCacheSizeBytes);

    MutableDocument md(DOC("_id" << 0));
    md.metadata().setSearchScore(1.5);
    Document withMeta = md.freeze();
    ASSERT_TRUE(withMeta.metadata().hasSearchScore());
    cache.add(withMeta);

    cache.freeze();

    auto served = cache.getNext();
    ASSERT_TRUE(served.has_value());
    ASSERT_DOCUMENT_EQ(*served, DOC("_id" << 0));
    ASSERT_TRUE(served->metadata().hasSearchScore());
    ASSERT_EQ(served->metadata().getSearchScore(), 1.5);
}

}  // namespace
}  // namespace mongo
