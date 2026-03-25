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

#include "mongo/db/query/compiler/optimizer/join/catalog_stats.h"

#include "mongo/bson/json.h"
#include "mongo/db/query/compiler/optimizer/join/unit_test_helpers.h"
#include "mongo/unittest/unittest.h"

namespace mongo::join_ordering {
using CatalogStatsTest = JoinOrderingTestFixture;

TEST_F(CatalogStatsTest, FieldsAreUnique) {
    UniqueFieldInformation uniqueFields =
        buildUniqueFieldInfo({fromjson("{foo: 1}"),
                              fromjson("{bar: 1}"),
                              fromjson("{baz: -1, qux: 1}"),
                              fromjson("{a: 1, b: 1, c: 1}"),
                              fromjson("{b: 1, c: 1, d: 1, e: 1}")});

    // Exact match in unique fields.
    ASSERT_TRUE(fieldsAreUnique({"foo"}, uniqueFields));
    ASSERT_TRUE(fieldsAreUnique({"bar"}, uniqueFields));
    ASSERT_TRUE(fieldsAreUnique({"baz", "qux"}, uniqueFields));
    ASSERT_TRUE(fieldsAreUnique({"qux", "baz"}, uniqueFields));
    ASSERT_TRUE(fieldsAreUnique({"a", "b", "c"}, uniqueFields));
    ASSERT_TRUE(fieldsAreUnique({"b", "c", "d", "e"}, uniqueFields));

    // Superset of unique fields.
    ASSERT_TRUE(fieldsAreUnique({"foo", "nonexistent"}, uniqueFields));
    ASSERT_TRUE(fieldsAreUnique({"baz", "qux", "nonexistent"}, uniqueFields));
    ASSERT_TRUE(fieldsAreUnique({"a", "b", "c", "foo"}, uniqueFields));
    ASSERT_TRUE(fieldsAreUnique({"bar", "foo.subfield"}, uniqueFields));

    // Subset of a unique field set is not unique.
    ASSERT_FALSE(fieldsAreUnique({"baz"}, uniqueFields));
    ASSERT_FALSE(fieldsAreUnique({"qux"}, uniqueFields));
    ASSERT_FALSE(fieldsAreUnique({"b", "c", "d"}, uniqueFields));
    ASSERT_FALSE(fieldsAreUnique({"baz", "nonexistent"}, uniqueFields));
    ASSERT_FALSE(fieldsAreUnique({"baz", "a", "b"}, uniqueFields));
    ASSERT_FALSE(fieldsAreUnique({"nonexistent"}, uniqueFields));
    ASSERT_FALSE(fieldsAreUnique({"a", "b", "cc"}, uniqueFields));

    // Subfield of a unique field is not unique.
    ASSERT_FALSE(fieldsAreUnique({"foo.subfield"}, uniqueFields));
    ASSERT_FALSE(fieldsAreUnique({"baz", "qux.subfield"}, uniqueFields));
}

TEST_F(CatalogStatsTest, NumPagesInStorageEngineCache) {
    const auto nss = makeNSS("coll");

    auto numPages = [&](CollectionStats collStats, double cacheBytes = 2.0 * 1024 * 1024 * 1024) {
        return CatalogStats{.collStats = {{nss, collStats}},
                            .bytesInStorageEngineCache = cacheBytes}
            .numPagesInStorageEngineCache(nss);
    };

    // Edge cases: when either size is 0, the function should not crash and return a positive
    // value (falls back to the default 32 KiB in-memory page size).
    ASSERT_GT(numPages({.logicalDataSizeBytes = 0, .onDiskSizeBytes = 0}), 0);
    ASSERT_GT(numPages({.logicalDataSizeBytes = 0, .onDiskSizeBytes = 1000}), 0);
    ASSERT_GT(numPages({.logicalDataSizeBytes = 1000, .onDiskSizeBytes = 0}), 0);

    // A larger cache should fit more pages.
    ASSERT_GT(numPages({.logicalDataSizeBytes = 1000, .onDiskSizeBytes = 500, .pageSizeBytes = 100},
                       /*cacheBytes*/ 2000),
              numPages({.logicalDataSizeBytes = 1000, .onDiskSizeBytes = 500, .pageSizeBytes = 100},
                       /*cacheBytes*/ 1000));

    // Smaller on-disk page size → more pages on disk → smaller average in-memory page size →
    // more pages fit in the cache.
    ASSERT_GT(
        numPages({.logicalDataSizeBytes = 1000, .onDiskSizeBytes = 500, .pageSizeBytes = 50}),
        numPages({.logicalDataSizeBytes = 1000, .onDiskSizeBytes = 500, .pageSizeBytes = 100}));

    // Higher compression (smaller on-disk size for same logical size) → fewer on-disk pages →
    // larger average in-memory page → fewer pages fit in cache.
    ASSERT_GT(
        numPages({.logicalDataSizeBytes = 1000, .onDiskSizeBytes = 500, .pageSizeBytes = 100}),
        numPages({.logicalDataSizeBytes = 1000, .onDiskSizeBytes = 250, .pageSizeBytes = 100}));

    // When the collection is smaller than a single on-disk page, pagesInColl < 1 so the average
    // in-memory page size exceeds the logical data size. The result should still be positive.
    ASSERT_GT(numPages({.logicalDataSizeBytes = 10, .onDiskSizeBytes = 10, .pageSizeBytes = 100}),
              0);

    // When the cache is smaller than the average in-memory page size, fewer than 1 page fits.
    // The result should again be a positive fraction.
    const double tinyCachePages =
        numPages({.logicalDataSizeBytes = 1000, .onDiskSizeBytes = 500, .pageSizeBytes = 100},
                 /*cacheBytes*/ 50);
    ASSERT_GT(tinyCachePages, 0);
    ASSERT_LT(tinyCachePages, 1);

    // Quantization: differences in onDiskSizeBytes within a 2^(1/4) bucket should produce the
    // same result. onDisk 500 vs 515 with pageSize 100 → raw pagesInColl 5.0 vs 5.15 (~3% diff),
    // both round to the same 2^(1/4) bucket.
    ASSERT_EQ(
        numPages({.logicalDataSizeBytes = 1000, .onDiskSizeBytes = 500, .pageSizeBytes = 100}),
        numPages({.logicalDataSizeBytes = 1000, .onDiskSizeBytes = 515, .pageSizeBytes = 100}));

    // Differences large enough to cross a bucket boundary should produce different results.
    // onDisk 500 vs 520 with pageSize 100 → raw pagesInColl 5.0 vs 5.2, different buckets.
    ASSERT_NE(
        numPages({.logicalDataSizeBytes = 1000, .onDiskSizeBytes = 500, .pageSizeBytes = 100}),
        numPages({.logicalDataSizeBytes = 1000, .onDiskSizeBytes = 520, .pageSizeBytes = 100}));
}
}  // namespace mongo::join_ordering
