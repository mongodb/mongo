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

class BucketCatalogStateManagerTest : public BucketCatalog, public unittest::Test {
public:
    BucketCatalogStateManagerTest() {}

    bool hasBeenCleared(Bucket* bucket) {
        auto state = _bucketStateManager.getBucketState(bucket);
        return (state &&
                (*state == BucketState::kCleared || *state == BucketState::kPreparedAndCleared));
    }

    Bucket* createBucket(const CreationInfo& info) {
        auto ptr = _allocateBucket(&_stripes[info.stripe], withLock, info);
        ptr->setNamespace(info.key.ns);
        ASSERT_FALSE(hasBeenCleared(ptr));
        return ptr;
    }

    bool cannotAccessBucket(Bucket* bucket) {
        if (hasBeenCleared(bucket)) {
            _removeBucket(&_stripes[bucket->stripe()], withLock, bucket, false);
            return true;
        } else {
            return false;
        }
    }

    void checkAndRemoveClearedBucket(Bucket* bucket, BucketKey bucketKey, WithLock withLock) {
        auto a = _findBucket(_stripes[_getStripeNumber(bucketKey)],
                             withLock,
                             bucket->id(),
                             ReturnClearedBuckets::kYes);
        ASSERT(a == bucket);
        auto b = _findBucket(_stripes[_getStripeNumber(bucketKey)],
                             withLock,
                             bucket->id(),
                             ReturnClearedBuckets::kNo);
        ASSERT(b == nullptr);
        _removeBucket(&_stripes[_getStripeNumber(bucketKey)], withLock, bucket, false);
    }

    WithLock withLock = WithLock::withoutLock();
    NamespaceString ns1{"db.test1"};
    NamespaceString ns2{"db.test2"};
    NamespaceString ns3{"db.test3"};
    BSONElement elem;
    BucketMetadata bucketMetadata{elem, nullptr};
    BucketKey bucketKey1{ns1, bucketMetadata};
    BucketKey bucketKey2{ns2, bucketMetadata};
    BucketKey bucketKey3{ns3, bucketMetadata};
    Date_t date = Date_t::now();
    TimeseriesOptions options;
    ExecutionStatsController stats = _getExecutionStats(ns1);
    ClosedBuckets closedBuckets;
    BucketCatalog::CreationInfo info1{
        bucketKey1, _getStripeNumber(bucketKey1), date, options, stats, &closedBuckets};
    BucketCatalog::CreationInfo info2{
        bucketKey2, _getStripeNumber(bucketKey2), date, options, stats, &closedBuckets};
    BucketCatalog::CreationInfo info3{
        bucketKey3, _getStripeNumber(bucketKey3), date, options, stats, &closedBuckets};
};


TEST_F(BucketCatalogStateManagerTest, EraAdvancesAsExpected) {

    RAIIServerParameterControllerForTest controller{"featureFlagTimeseriesScalabilityImprovements",
                                                    true};

    // When allocating new buckets, we expect their era value to match the BucketCatalog's era.
    ASSERT_EQ(_bucketStateManager.getEra(), 0);
    auto bucket1 = createBucket(info1);
    ASSERT_EQ(_bucketStateManager.getEra(), 0);
    ASSERT_EQ(bucket1->getEra(), 0);

    // When clearing buckets, we expect the BucketCatalog's era value to increase while the cleared
    // bucket era values should remain unchanged.
    clear(ns1);
    ASSERT_EQ(_bucketStateManager.getEra(), 1);
    ASSERT_EQ(bucket1->getEra(), 0);

    // When clearing buckets of one namespace, we expect the era of buckets of any other namespace
    // to not change.
    auto bucket2 = createBucket(info1);
    auto bucket3 = createBucket(info2);
    ASSERT_EQ(_bucketStateManager.getEra(), 1);
    ASSERT_EQ(bucket2->getEra(), 1);
    ASSERT_EQ(bucket3->getEra(), 1);
    clear(ns1);
    ASSERT_EQ(_bucketStateManager.getEra(), 2);
    ASSERT_EQ(bucket3->getEra(), 1);
    ASSERT_EQ(bucket1->getEra(), 0);
    ASSERT_EQ(bucket2->getEra(), 1);

    // Era also advances when clearing by OID
    clear(OID());
    ASSERT_EQ(_bucketStateManager.getEra(), 3);
}

TEST_F(BucketCatalogStateManagerTest, EraCountMapUpdatedCorrectly) {
    RAIIServerParameterControllerForTest controller{"featureFlagTimeseriesScalabilityImprovements",
                                                    true};

    // Creating a bucket in a new era should add a counter for that era to the map.
    auto bucket1 = createBucket(info1);
    ASSERT_EQ(bucket1->getEra(), 0);
    ASSERT_EQ(_bucketStateManager.getCountForEra(0), 1);
    clear(ns1);
    checkAndRemoveClearedBucket(bucket1, bucketKey1, withLock);

    // When the last bucket in an era is destructed, the counter in the map should be removed.
    ASSERT_EQ(_bucketStateManager.getCountForEra(0), 0);

    // If there are still buckets in the era, however, the counter should still exist in the
    // map.
    auto bucket2 = createBucket(info1);
    auto bucket3 = createBucket(info2);
    ASSERT_EQ(bucket2->getEra(), 1);
    ASSERT_EQ(bucket3->getEra(), 1);
    ASSERT_EQ(_bucketStateManager.getCountForEra(1), 2);
    clear(ns2);
    checkAndRemoveClearedBucket(bucket3, bucketKey2, withLock);
    ASSERT_EQ(_bucketStateManager.getCountForEra(1), 1);

    // A bucket in one era being destroyed and the counter decrementing should not affect a
    // different era's counter.
    auto bucket4 = createBucket(info2);
    ASSERT_EQ(bucket4->getEra(), 2);
    ASSERT_EQ(_bucketStateManager.getCountForEra(2), 1);
    clear(ns2);
    checkAndRemoveClearedBucket(bucket4, bucketKey2, withLock);
    ASSERT_EQ(_bucketStateManager.getCountForEra(2), 0);
    ASSERT_EQ(_bucketStateManager.getCountForEra(1), 1);
}

TEST_F(BucketCatalogStateManagerTest, HasBeenClearedFunctionReturnsAsExpected) {
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
    ASSERT_EQ(_bucketStateManager.getCountForEra(0), 2);
    clear(ns2);
    ASSERT_FALSE(cannotAccessBucket(bucket1));
    ASSERT_EQ(_bucketStateManager.getCountForEra(0), 1);
    ASSERT_EQ(bucket1->getEra(), 1);
    ASSERT(cannotAccessBucket(bucket2));

    // Sanity check that all this still works with multiple buckets in a namespace being cleared.
    auto bucket3 = createBucket(info2);
    auto bucket4 = createBucket(info2);
    ASSERT_EQ(bucket3->getEra(), 1);
    ASSERT_EQ(bucket4->getEra(), 1);
    clear(ns2);
    ASSERT(cannotAccessBucket(bucket3));
    ASSERT(cannotAccessBucket(bucket4));
    auto bucket5 = createBucket(info2);
    ASSERT_EQ(bucket5->getEra(), 2);
    clear(ns2);
    ASSERT(cannotAccessBucket(bucket5));
    // _hasBeenCleared should be able to advance a bucket by multiple eras.
    ASSERT_EQ(bucket1->getEra(), 1);
    ASSERT_EQ(_bucketStateManager.getCountForEra(1), 1);
    ASSERT_EQ(_bucketStateManager.getCountForEra(3), 0);
    ASSERT_FALSE(cannotAccessBucket(bucket1));
    ASSERT_EQ(bucket1->getEra(), 3);
    ASSERT_EQ(_bucketStateManager.getCountForEra(1), 0);
    ASSERT_EQ(_bucketStateManager.getCountForEra(3), 1);

    // _hasBeenCleared works even if the bucket wasn't cleared in the most recent clear.
    clear(ns1);
    auto bucket6 = createBucket(info2);
    ASSERT_EQ(bucket6->getEra(), 4);
    clear(ns2);
    ASSERT_EQ(_bucketStateManager.getCountForEra(3), 1);
    ASSERT_EQ(_bucketStateManager.getCountForEra(4), 1);
    ASSERT(cannotAccessBucket(bucket1));
    ASSERT(cannotAccessBucket(bucket6));
    ASSERT_EQ(_bucketStateManager.getCountForEra(3), 0);
    ASSERT_EQ(_bucketStateManager.getCountForEra(4), 0);
}

TEST_F(BucketCatalogStateManagerTest, ClearRegistryGarbageCollection) {
    RAIIServerParameterControllerForTest controller{"featureFlagTimeseriesScalabilityImprovements",
                                                    true};

    auto bucket1 = createBucket(info1);
    auto bucket2 = createBucket(info2);
    ASSERT_EQ(bucket1->getEra(), 0);
    ASSERT_EQ(bucket2->getEra(), 0);
    ASSERT_EQUALS(_bucketStateManager.getClearOperationsCount(), 0);
    clear(ns1);
    checkAndRemoveClearedBucket(bucket1, bucketKey1, withLock);
    // Era 0 still has non-zero count after this clear because bucket2 is still in era 0.
    ASSERT_EQUALS(_bucketStateManager.getClearOperationsCount(), 1);
    clear(ns2);
    checkAndRemoveClearedBucket(bucket2, bucketKey2, withLock);
    // Bucket2 gets deleted, which makes era 0's count decrease to 0, then clear registry gets
    // cleaned.
    ASSERT_EQUALS(_bucketStateManager.getClearOperationsCount(), 0);

    auto bucket3 = createBucket(info1);
    auto bucket4 = createBucket(info2);
    ASSERT_EQ(bucket3->getEra(), 2);
    ASSERT_EQ(bucket4->getEra(), 2);
    clear(ns1);
    checkAndRemoveClearedBucket(bucket3, bucketKey1, withLock);
    // Era 2 still has bucket4 in it, so its count remains non-zero.
    ASSERT_EQUALS(_bucketStateManager.getClearOperationsCount(), 1);
    auto bucket5 = createBucket(info1);
    auto bucket6 = createBucket(info2);
    ASSERT_EQ(bucket5->getEra(), 3);
    ASSERT_EQ(bucket6->getEra(), 3);
    clear(ns1);
    checkAndRemoveClearedBucket(bucket5, bucketKey1, withLock);
    // Eras 2 and 3 still have bucket4 and bucket6 in them respectively, so their counts remain
    // non-zero.
    ASSERT_EQUALS(_bucketStateManager.getClearOperationsCount(), 2);
    clear(ns2);
    checkAndRemoveClearedBucket(bucket4, bucketKey2, withLock);
    checkAndRemoveClearedBucket(bucket6, bucketKey2, withLock);
    // Eras 2 and 3 have their counts become 0 because bucket4 and bucket6 are cleared. The clear
    // registry is emptied.
    ASSERT_EQUALS(_bucketStateManager.getClearOperationsCount(), 0);

    auto bucket7 = createBucket(info1);
    auto bucket8 = createBucket(info3);
    ASSERT_EQ(bucket7->getEra(), 5);
    ASSERT_EQ(bucket8->getEra(), 5);
    clear(ns3);
    checkAndRemoveClearedBucket(bucket8, bucketKey3, withLock);
    // Era 5 still has bucket7 in it so its count remains non-zero.
    ASSERT_EQUALS(_bucketStateManager.getClearOperationsCount(), 1);
    auto bucket9 = createBucket(info2);
    ASSERT_EQ(bucket9->getEra(), 6);
    clear(ns2);
    checkAndRemoveClearedBucket(bucket9, bucketKey2, withLock);
    // Era 6's count becomes 0. Since era 5 is the smallest era with non-zero count, no clear ops
    // are removed.
    ASSERT_EQUALS(_bucketStateManager.getClearOperationsCount(), 2);
    auto bucket10 = createBucket(info3);
    ASSERT_EQ(bucket10->getEra(), 7);
    clear(ns3);
    checkAndRemoveClearedBucket(bucket10, bucketKey3, withLock);
    // Era 7's count becomes 0. Since era 5 is the smallest era with non-zero count, no clear ops
    // are removed.
    ASSERT_EQUALS(_bucketStateManager.getClearOperationsCount(), 3);
    clear(ns1);
    checkAndRemoveClearedBucket(bucket7, bucketKey1, withLock);
    // Era 5's count becomes 0. No eras with non-zero counts remain, so all clear ops are removed.
    ASSERT_EQUALS(_bucketStateManager.getClearOperationsCount(), 0);
}

TEST_F(BucketCatalogStateManagerTest, HasBeenClearedToleratesGapsInRegistry) {
    RAIIServerParameterControllerForTest controller{"featureFlagTimeseriesScalabilityImprovements",
                                                    true};

    auto bucket1 = createBucket(info1);
    ASSERT_EQ(bucket1->getEra(), 0);
    clear(OID());
    ASSERT_EQ(_bucketStateManager.getEra(), 1);
    clear(ns1);
    ASSERT_EQ(_bucketStateManager.getEra(), 2);
    ASSERT_TRUE(hasBeenCleared(bucket1));

    auto bucket2 = createBucket(info2);
    ASSERT_EQ(bucket2->getEra(), 2);
    clear(OID());
    clear(OID());
    clear(OID());
    ASSERT_EQ(_bucketStateManager.getEra(), 5);
    ASSERT_TRUE(hasBeenCleared(bucket1));
    ASSERT_FALSE(hasBeenCleared(bucket2));
    clear(ns2);
    ASSERT_EQ(_bucketStateManager.getEra(), 6);
    ASSERT_TRUE(hasBeenCleared(bucket1));
    ASSERT_TRUE(hasBeenCleared(bucket2));
}

}  // namespace
}  // namespace mongo
