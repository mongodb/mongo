/**
 * Copyright (C) 2017 MongoDB Inc.
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
 * must comply with the GNU Affero General Public License in all respects for
 * all of the code used other than as permitted herein. If you modify file(s)
 * with this exception, you may extend this exception to your version of the
 * file(s), but you are not obligated to do so. If you do not wish to do so,
 * delete this exception statement from your version. If you delete this
 * exception statement from all source files in the program, then also delete
 * it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/db/pipeline/sequential_document_cache.h"

#include "mongo/db/pipeline/document_value_test_util.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"

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

DEATH_TEST(SequentialDocumentCacheTest, CannotIterateCacheWhileBuilding, "invariant") {
    SequentialDocumentCache cache(kCacheSizeBytes);
    ASSERT(cache.isBuilding());

    cache.getNext();
}

DEATH_TEST(SequentialDocumentCacheTest, CannotRestartCacheIterationWhileBuilding, "invariant") {
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
    ASSERT_FALSE(cache.getNext().is_initialized());
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
    ASSERT_FALSE(cache.getNext().is_initialized());

    cache.restartIteration();

    ASSERT_DOCUMENT_EQ(*cache.getNext(), DOC("_id" << 0));
    ASSERT_DOCUMENT_EQ(*cache.getNext(), DOC("_id" << 1));
    ASSERT_FALSE(cache.getNext().is_initialized());
}

DEATH_TEST(SequentialDocumentCacheTest, CannotAddDocumentsToCacheAfterFreezing, "invariant") {
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

DEATH_TEST(SequentialDocumentCacheTest, CannotAddDocumentsToAbandonedCache, "invariant") {
    SequentialDocumentCache cache(kCacheSizeBytes);
    cache.abandon();

    cache.add(DOC("_id" << 0));
}

DEATH_TEST(SequentialDocumentCacheTest, CannotFreezeCacheWhenAbandoned, "invariant") {
    SequentialDocumentCache cache(kCacheSizeBytes);
    cache.abandon();

    cache.freeze();
}

DEATH_TEST(SequentialDocumentCacheTest, CannotRestartCacheIterationWhenAbandoned, "invariant") {
    SequentialDocumentCache cache(kCacheSizeBytes);
    ASSERT(cache.isBuilding());

    cache.abandon();

    cache.restartIteration();
}

DEATH_TEST(SequentialDocumentCacheTest, CannotCallGetNextWhenAbandoned, "invariant") {
    SequentialDocumentCache cache(kCacheSizeBytes);
    ASSERT(cache.isBuilding());

    cache.add(DOC("_id" << 0));
    cache.add(DOC("_id" << 1));

    cache.abandon();

    cache.getNext();
}

}  // namespace
}  // namespace mongo
