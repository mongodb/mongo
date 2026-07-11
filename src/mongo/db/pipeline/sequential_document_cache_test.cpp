// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/pipeline/sequential_document_cache.h"

#include "mongo/db/exec/document_value/document_value_test_util.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"

#include <string>


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

}  // namespace
}  // namespace mongo
