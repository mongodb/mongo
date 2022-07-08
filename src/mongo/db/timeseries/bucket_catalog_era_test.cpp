/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/db/timeseries/bucket_catalog.h"
#include "mongo/idl/server_parameter_test_util.h"
#include "mongo/unittest/bson_test_util.h"

namespace mongo {
namespace {

class BucketCatalogEraTest : public BucketCatalog, public unittest::Test {
public:
    BucketCatalogEraTest() {}
    Bucket* createBucket(const CreationInfo& info) {
        auto ptr = _allocateBucket(&_stripes[info.stripe], withLock, info);
        ptr->setNamespace(info.key.ns);
        return ptr;
    }

    Stripe stripe;
    WithLock withLock = WithLock::withoutLock();
    NamespaceString ns1{"db.test1"};
    NamespaceString ns2{"db.test2"};
    BSONElement elem;
    BucketMetadata bucketMetadata{elem, nullptr};
    BucketKey bucketKey1{ns1, bucketMetadata};
    BucketKey bucketKey2{ns2, bucketMetadata};
    Date_t date = Date_t::now();
    TimeseriesOptions options;
    ExecutionStatsController stats = _getExecutionStats(ns1);
    ClosedBuckets closedBuckets;
    BucketCatalog::CreationInfo info1{
        bucketKey1, _getStripeNumber(bucketKey1), date, options, stats, &closedBuckets};
    BucketCatalog::CreationInfo info2{
        bucketKey2, _getStripeNumber(bucketKey2), date, options, stats, &closedBuckets};
};


TEST_F(BucketCatalogEraTest, EraAdvancesAsExpected) {

    RAIIServerParameterControllerForTest controller{"featureFlagTimeseriesScalabilityImprovements",
                                                    true};

    // When allocating new buckets, we expect their era value to match the BucketCatalog's era.
    ASSERT_EQ(_eraManager.getEra(), 0);
    auto bucket1 = createBucket(info1);
    ASSERT_EQ(_eraManager.getEra(), 0);
    ASSERT_EQ(bucket1->era(), 0);

    // When clearing buckets, we expect the BucketCatalog's era value to increase while the cleared
    // bucket era values should remain unchanged.
    clear(ns1);
    ASSERT_EQ(_eraManager.getEra(), 1);
    // TODO (SERVER-66698): Add checks on the buckets' era values.
    // ASSERT_EQ(b1->era(), 0);

    // When clearing buckets of one namespace, we expect the era of buckets of any other namespace
    // to not change.
    auto bucket2 = createBucket(info1);
    auto bucket3 = createBucket(info2);
    ASSERT_EQ(_eraManager.getEra(), 1);
    ASSERT_EQ(bucket2->era(), 1);
    ASSERT_EQ(bucket3->era(), 1);
    clear(ns1);
    ASSERT_EQ(_eraManager.getEra(), 2);
    ASSERT_EQ(bucket3->era(), 1);
    // TODO (SERVER-66698): Add checks on the buckets' era values.
    // ASSERT_EQ(b1->era(), 0);
    // ASSERT_EQ(b2->era(), 1);
}

TEST_F(BucketCatalogEraTest, EraCountMapUpdatedCorrectly) {
    RAIIServerParameterControllerForTest controller{"featureFlagTimeseriesScalabilityImprovements",
                                                    true};

    // TODO (SERVER-66698): Change count assertions now that Buckets are cleared lazily.
    // Creating a bucket in a new era should add a counter for that era to the map.
    auto bucket1 = createBucket(info1);
    ASSERT_EQ(bucket1->era(), 0);
    ASSERT_EQ(_eraManager.getCountForEra(0), 1);
    clear(ns1);

    // When the last bucket in an era is destructed, the counter in the map should be removed.
    ASSERT_EQ(_eraManager.getCountForEra(0), 0);

    // If there are still buckets in the era, however, the counter should still exist in the
    // map.
    auto bucket2 = createBucket(info1);
    auto bucket3 = createBucket(info2);
    ASSERT_EQ(bucket2->era(), 1);
    ASSERT_EQ(bucket3->era(), 1);
    ASSERT_EQ(_eraManager.getCountForEra(1), 2);
    clear(ns2);
    ASSERT_EQ(_eraManager.getCountForEra(1), 1);

    // A bucket in one era being destroyed and the counter decrementing should not affect a
    // different era's counter.
    auto bucket4 = createBucket(info2);
    ASSERT_EQ(bucket4->era(), 2);
    ASSERT_EQ(_eraManager.getCountForEra(2), 1);
    clear(ns2);
    ASSERT_EQ(_eraManager.getCountForEra(2), 0);
    ASSERT_EQ(_eraManager.getCountForEra(1), 1);
}


}  // namespace
}  // namespace mongo
