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

#include "mongo/db/storage/storage_parameters_gen.h"
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
        ASSERT_FALSE(_eraManager.hasBeenCleared(ptr));
        return ptr;
    }

    void clearForTest(const NamespaceString& ns) {
        clearForTest([&ns](const NamespaceString& bucketNs) { return bucketNs == ns; });
    }

    void clearForTest(std::function<bool(const NamespaceString&)>&& shouldClear) {
        uint64_t era = _eraManager.incrementEra();
        if (feature_flags::gTimeseriesScalabilityImprovements.isEnabled(
                serverGlobalParams.featureCompatibility)) {
            _eraManager.insertToRegistry(era, std::move(shouldClear));
        }
    }

    bool cannotAccessBucket(Bucket* bucket) {
        if (_eraManager.hasBeenCleared(bucket)) {
            _removeBucket(&_stripes[bucket->stripe()], withLock, bucket, false);
            return true;
        } else {
            return false;
        }
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
    ASSERT_EQ(bucket1->getEra(), 0);

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
    ASSERT_EQ(bucket2->getEra(), 1);
    ASSERT_EQ(bucket3->getEra(), 1);
    clear(ns1);
    ASSERT_EQ(_eraManager.getEra(), 2);
    ASSERT_EQ(bucket3->getEra(), 1);
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
    ASSERT_EQ(bucket1->getEra(), 0);
    ASSERT_EQ(_eraManager.getCountForEra(0), 1);
    clear(ns1);

    // When the last bucket in an era is destructed, the counter in the map should be removed.
    ASSERT_EQ(_eraManager.getCountForEra(0), 0);

    // If there are still buckets in the era, however, the counter should still exist in the
    // map.
    auto bucket2 = createBucket(info1);
    auto bucket3 = createBucket(info2);
    ASSERT_EQ(bucket2->getEra(), 1);
    ASSERT_EQ(bucket3->getEra(), 1);
    ASSERT_EQ(_eraManager.getCountForEra(1), 2);
    clear(ns2);
    ASSERT_EQ(_eraManager.getCountForEra(1), 1);

    // A bucket in one era being destroyed and the counter decrementing should not affect a
    // different era's counter.
    auto bucket4 = createBucket(info2);
    ASSERT_EQ(bucket4->getEra(), 2);
    ASSERT_EQ(_eraManager.getCountForEra(2), 1);
    clear(ns2);
    ASSERT_EQ(_eraManager.getCountForEra(2), 0);
    ASSERT_EQ(_eraManager.getCountForEra(1), 1);
}

TEST_F(BucketCatalogEraTest, HasBeenClearedFunctionReturnsAsExpected) {
    RAIIServerParameterControllerForTest controller{"featureFlagTimeseriesScalabilityImprovements",
                                                    true};

    auto bucket1 = createBucket(info1);
    auto bucket2 = createBucket(info2);
    ASSERT_EQ(bucket1->getEra(), 0);
    ASSERT_EQ(bucket2->getEra(), 0);

    // After a clear operation, _hasBeenCleared returns whether a particular bucket was cleared or
    // not. It also advances the bucket's era up to the most recent era.
    ASSERT_FALSE(cannotAccessBucket(bucket1));
    ASSERT_FALSE(cannotAccessBucket(bucket2));
    ASSERT_EQ(_eraManager.getCountForEra(0), 2);
    clearForTest(ns2);
    ASSERT_FALSE(cannotAccessBucket(bucket1));
    ASSERT_EQ(_eraManager.getCountForEra(0), 1);
    ASSERT_EQ(bucket1->getEra(), 1);
    ASSERT(cannotAccessBucket(bucket2));

    // Sanity check that all this still works with multiple buckets in a namespace being cleared.
    auto bucket3 = createBucket(info2);
    auto bucket4 = createBucket(info2);
    ASSERT_EQ(bucket3->getEra(), 1);
    ASSERT_EQ(bucket4->getEra(), 1);
    clearForTest(ns2);
    ASSERT(cannotAccessBucket(bucket3));
    ASSERT(cannotAccessBucket(bucket4));
    auto bucket5 = createBucket(info2);
    ASSERT_EQ(bucket5->getEra(), 2);
    clearForTest(ns2);
    ASSERT(cannotAccessBucket(bucket5));
    // _hasBeenCleared should be able to advance a bucket by multiple eras.
    ASSERT_EQ(bucket1->getEra(), 1);
    ASSERT_EQ(_eraManager.getCountForEra(1), 1);
    ASSERT_EQ(_eraManager.getCountForEra(3), 0);
    ASSERT_FALSE(cannotAccessBucket(bucket1));
    ASSERT_EQ(bucket1->getEra(), 3);
    ASSERT_EQ(_eraManager.getCountForEra(1), 0);
    ASSERT_EQ(_eraManager.getCountForEra(3), 1);

    // _hasBeenCleared works even if the bucket wasn't cleared in the most recent clear.
    clearForTest(ns1);
    auto bucket6 = createBucket(info2);
    ASSERT_EQ(bucket6->getEra(), 4);
    clearForTest(ns2);
    ASSERT_EQ(_eraManager.getCountForEra(3), 1);
    ASSERT_EQ(_eraManager.getCountForEra(4), 1);
    ASSERT(cannotAccessBucket(bucket1));
    ASSERT(cannotAccessBucket(bucket6));
    ASSERT_EQ(_eraManager.getCountForEra(3), 0);
    ASSERT_EQ(_eraManager.getCountForEra(4), 0);
}


}  // namespace
}  // namespace mongo
