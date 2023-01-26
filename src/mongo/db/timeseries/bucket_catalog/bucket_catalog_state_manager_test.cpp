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
#include "mongo/db/timeseries/bucket_catalog/bucket_catalog.h"
#include "mongo/idl/server_parameter_test_util.h"
#include "mongo/unittest/bson_test_util.h"

namespace mongo::timeseries::bucket_catalog {
namespace {

class BucketCatalogStateManagerTest : public BucketCatalog, public unittest::Test {
public:
    BucketCatalogStateManagerTest() {}

    void clearById(const NamespaceString& ns, const OID& oid) {
        directWriteStart(ns, oid);
        directWriteFinish(ns, oid);
    }

    bool hasBeenCleared(Bucket* bucket) {
        auto state = _bucketStateManager.getBucketState(bucket);
        return (state && state.value().isSet(BucketStateFlag::kCleared));
    }

    Bucket* createBucket(const CreationInfo& info) {
        auto ptr = _allocateBucket(&_stripes[info.stripe], withLock, info);
        ASSERT_FALSE(hasBeenCleared(ptr));
        return ptr;
    }

    bool cannotAccessBucket(Bucket* bucket) {
        if (hasBeenCleared(bucket)) {
            _removeBucket(
                &_stripes[_getStripeNumber(bucket->key)], withLock, bucket, RemovalMode::kAbort);
            return true;
        } else {
            return false;
        }
    }

    void checkAndRemoveClearedBucket(Bucket* bucket) {
        auto a = _findBucket(_stripes[_getStripeNumber(bucket->key)],
                             withLock,
                             bucket->bucketId,
                             IgnoreBucketState::kYes);
        ASSERT(a == bucket);
        auto b = _findBucket(_stripes[_getStripeNumber(bucket->key)],
                             withLock,
                             bucket->bucketId,
                             IgnoreBucketState::kNo);
        ASSERT(b == nullptr);
        _removeBucket(
            &_stripes[_getStripeNumber(bucket->key)], withLock, bucket, RemovalMode::kAbort);
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

TEST_F(BucketCatalogStateManagerTest, BucketStateSetUnsetFlag) {
    BucketState state;
    auto testFlags = [&state](std::initializer_list<BucketStateFlag> set,
                              std::initializer_list<BucketStateFlag> unset) {
        for (auto flag : set) {
            ASSERT_TRUE(state.isSet(flag));
        }
        for (auto flag : unset) {
            ASSERT_FALSE(state.isSet(flag));
        }
    };

    testFlags({},
              {
                  BucketStateFlag::kPrepared,
                  BucketStateFlag::kCleared,
                  BucketStateFlag::kPendingCompression,
                  BucketStateFlag::kPendingDirectWrite,
              });

    state.setFlag(BucketStateFlag::kPrepared);
    testFlags(
        {
            BucketStateFlag::kPrepared,
        },
        {
            BucketStateFlag::kCleared,
            BucketStateFlag::kPendingCompression,
            BucketStateFlag::kPendingDirectWrite,
        });

    state.setFlag(BucketStateFlag::kCleared);
    testFlags(
        {
            BucketStateFlag::kPrepared,
            BucketStateFlag::kCleared,
        },
        {
            BucketStateFlag::kPendingCompression,
            BucketStateFlag::kPendingDirectWrite,
        });

    state.setFlag(BucketStateFlag::kPendingCompression);
    testFlags(
        {
            BucketStateFlag::kPrepared,
            BucketStateFlag::kCleared,
            BucketStateFlag::kPendingCompression,
        },
        {

            BucketStateFlag::kPendingDirectWrite,
        });

    state.setFlag(BucketStateFlag::kPendingDirectWrite);
    testFlags(
        {
            BucketStateFlag::kPrepared,
            BucketStateFlag::kCleared,
            BucketStateFlag::kPendingCompression,
            BucketStateFlag::kPendingDirectWrite,
        },
        {});

    state.unsetFlag(BucketStateFlag::kPrepared);
    testFlags(
        {
            BucketStateFlag::kCleared,
            BucketStateFlag::kPendingCompression,
            BucketStateFlag::kPendingDirectWrite,

        },
        {
            BucketStateFlag::kPrepared,
        });

    state.unsetFlag(BucketStateFlag::kCleared);
    testFlags(
        {
            BucketStateFlag::kPendingCompression,
            BucketStateFlag::kPendingDirectWrite,

        },
        {
            BucketStateFlag::kPrepared,
            BucketStateFlag::kCleared,
        });

    state.unsetFlag(BucketStateFlag::kPendingCompression);
    testFlags(
        {
            BucketStateFlag::kPendingDirectWrite,
        },
        {
            BucketStateFlag::kPrepared,
            BucketStateFlag::kCleared,
            BucketStateFlag::kPendingCompression,
        });

    state.unsetFlag(BucketStateFlag::kPendingDirectWrite);
    testFlags({},
              {
                  BucketStateFlag::kPrepared,
                  BucketStateFlag::kCleared,
                  BucketStateFlag::kPendingCompression,
                  BucketStateFlag::kPendingDirectWrite,
              });
}

TEST_F(BucketCatalogStateManagerTest, BucketStateReset) {
    BucketState state;

    state.setFlag(BucketStateFlag::kPrepared);
    state.setFlag(BucketStateFlag::kCleared);
    state.setFlag(BucketStateFlag::kPendingCompression);
    state.setFlag(BucketStateFlag::kPendingDirectWrite);

    ASSERT_TRUE(state.isSet(BucketStateFlag::kPrepared));
    ASSERT_TRUE(state.isSet(BucketStateFlag::kCleared));
    ASSERT_TRUE(state.isSet(BucketStateFlag::kPendingCompression));
    ASSERT_TRUE(state.isSet(BucketStateFlag::kPendingDirectWrite));

    state.reset();

    ASSERT_FALSE(state.isSet(BucketStateFlag::kPrepared));
    ASSERT_FALSE(state.isSet(BucketStateFlag::kCleared));
    ASSERT_FALSE(state.isSet(BucketStateFlag::kPendingCompression));
    ASSERT_FALSE(state.isSet(BucketStateFlag::kPendingDirectWrite));
}

TEST_F(BucketCatalogStateManagerTest, BucketStateIsPrepared) {
    BucketState state;

    ASSERT_FALSE(state.isPrepared());

    state.setFlag(BucketStateFlag::kPrepared);
    ASSERT_TRUE(state.isPrepared());

    state.setFlag(BucketStateFlag::kCleared);
    state.setFlag(BucketStateFlag::kPendingCompression);
    state.setFlag(BucketStateFlag::kPendingDirectWrite);
    ASSERT_TRUE(state.isPrepared());

    state.unsetFlag(BucketStateFlag::kPrepared);
    ASSERT_FALSE(state.isPrepared());
}

TEST_F(BucketCatalogStateManagerTest, BucketStateConflictsWithInsert) {
    BucketState state;
    ASSERT_FALSE(state.conflictsWithInsertion());

    // Just prepared is false
    state.setFlag(BucketStateFlag::kPrepared);
    ASSERT_FALSE(state.conflictsWithInsertion());

    // Prepared and cleared is true
    state.setFlag(BucketStateFlag::kCleared);
    ASSERT_TRUE(state.conflictsWithInsertion());

    // Just cleared is true
    state.reset();
    state.setFlag(BucketStateFlag::kCleared);
    ASSERT_TRUE(state.conflictsWithInsertion());

    // Pending operations are true
    state.reset();
    state.setFlag(BucketStateFlag::kPendingCompression);
    ASSERT_TRUE(state.conflictsWithInsertion());

    state.reset();
    state.setFlag(BucketStateFlag::kPendingDirectWrite);
    ASSERT_TRUE(state.conflictsWithInsertion());
}

TEST_F(BucketCatalogStateManagerTest, BucketStateConflictsWithReopening) {
    BucketState state;
    ASSERT_FALSE(state.conflictsWithReopening());

    // Just prepared is false
    state.setFlag(BucketStateFlag::kPrepared);
    ASSERT_FALSE(state.conflictsWithReopening());

    // Prepared and cleared is false
    state.setFlag(BucketStateFlag::kCleared);
    ASSERT_FALSE(state.conflictsWithReopening());

    // Just cleared is false
    state.reset();
    state.setFlag(BucketStateFlag::kCleared);
    ASSERT_FALSE(state.conflictsWithReopening());

    // Pending operations are true
    state.reset();
    state.setFlag(BucketStateFlag::kPendingCompression);
    ASSERT_TRUE(state.conflictsWithReopening());

    state.reset();
    state.setFlag(BucketStateFlag::kPendingDirectWrite);
    ASSERT_TRUE(state.conflictsWithReopening());
}

TEST_F(BucketCatalogStateManagerTest, EraAdvancesAsExpected) {

    RAIIServerParameterControllerForTest controller{"featureFlagTimeseriesScalabilityImprovements",
                                                    true};

    // When allocating new buckets, we expect their era value to match the BucketCatalog's era.
    ASSERT_EQ(_bucketStateManager.getEra(), 0);
    auto bucket1 = createBucket(info1);
    ASSERT_EQ(_bucketStateManager.getEra(), 0);
    ASSERT_EQ(bucket1->lastChecked, 0);

    // When clearing buckets, we expect the BucketCatalog's era value to increase while the cleared
    // bucket era values should remain unchanged.
    clear(ns1);
    ASSERT_EQ(_bucketStateManager.getEra(), 1);
    ASSERT_EQ(bucket1->lastChecked, 0);

    // When clearing buckets of one namespace, we expect the era of buckets of any other namespace
    // to not change.
    auto bucket2 = createBucket(info1);
    auto bucket3 = createBucket(info2);
    ASSERT_EQ(_bucketStateManager.getEra(), 1);
    ASSERT_EQ(bucket2->lastChecked, 1);
    ASSERT_EQ(bucket3->lastChecked, 1);
    clear(ns1);
    ASSERT_EQ(_bucketStateManager.getEra(), 2);
    ASSERT_EQ(bucket3->lastChecked, 1);
    ASSERT_EQ(bucket1->lastChecked, 0);
    ASSERT_EQ(bucket2->lastChecked, 1);

    // Era also advances when clearing by OID
    clearById(ns1, OID());
    ASSERT_EQ(_bucketStateManager.getEra(), 4);
}

TEST_F(BucketCatalogStateManagerTest, EraCountMapUpdatedCorrectly) {
    RAIIServerParameterControllerForTest controller{"featureFlagTimeseriesScalabilityImprovements",
                                                    true};

    // Creating a bucket in a new era should add a counter for that era to the map.
    auto bucket1 = createBucket(info1);
    ASSERT_EQ(bucket1->lastChecked, 0);
    ASSERT_EQ(_bucketStateManager.getCountForEra(0), 1);
    clear(ns1);
    checkAndRemoveClearedBucket(bucket1);

    // When the last bucket in an era is destructed, the counter in the map should be removed.
    ASSERT_EQ(_bucketStateManager.getCountForEra(0), 0);

    // If there are still buckets in the era, however, the counter should still exist in the
    // map.
    auto bucket2 = createBucket(info1);
    auto bucket3 = createBucket(info2);
    ASSERT_EQ(bucket2->lastChecked, 1);
    ASSERT_EQ(bucket3->lastChecked, 1);
    ASSERT_EQ(_bucketStateManager.getCountForEra(1), 2);
    clear(ns2);
    checkAndRemoveClearedBucket(bucket3);
    ASSERT_EQ(_bucketStateManager.getCountForEra(1), 1);

    // A bucket in one era being destroyed and the counter decrementing should not affect a
    // different era's counter.
    auto bucket4 = createBucket(info2);
    ASSERT_EQ(bucket4->lastChecked, 2);
    ASSERT_EQ(_bucketStateManager.getCountForEra(2), 1);
    clear(ns2);
    checkAndRemoveClearedBucket(bucket4);
    ASSERT_EQ(_bucketStateManager.getCountForEra(2), 0);
    ASSERT_EQ(_bucketStateManager.getCountForEra(1), 1);
}

TEST_F(BucketCatalogStateManagerTest, HasBeenClearedFunctionReturnsAsExpected) {
    RAIIServerParameterControllerForTest controller{"featureFlagTimeseriesScalabilityImprovements",
                                                    true};

    auto bucket1 = createBucket(info1);
    auto bucket2 = createBucket(info2);
    ASSERT_EQ(bucket1->lastChecked, 0);
    ASSERT_EQ(bucket2->lastChecked, 0);

    // After a clear operation, _isMemberOfClearedSet returns whether a particular bucket was
    // cleared or not. It also advances the bucket's era up to the most recent era.
    ASSERT_FALSE(cannotAccessBucket(bucket1));
    ASSERT_FALSE(cannotAccessBucket(bucket2));
    ASSERT_EQ(_bucketStateManager.getCountForEra(0), 2);
    clear(ns2);
    ASSERT_FALSE(cannotAccessBucket(bucket1));
    ASSERT_EQ(_bucketStateManager.getCountForEra(0), 1);
    ASSERT_EQ(bucket1->lastChecked, 1);
    ASSERT(cannotAccessBucket(bucket2));

    // Sanity check that all this still works with multiple buckets in a namespace being cleared.
    auto bucket3 = createBucket(info2);
    auto bucket4 = createBucket(info2);
    ASSERT_EQ(bucket3->lastChecked, 1);
    ASSERT_EQ(bucket4->lastChecked, 1);
    clear(ns2);
    ASSERT(cannotAccessBucket(bucket3));
    ASSERT(cannotAccessBucket(bucket4));
    auto bucket5 = createBucket(info2);
    ASSERT_EQ(bucket5->lastChecked, 2);
    clear(ns2);
    ASSERT(cannotAccessBucket(bucket5));
    // _isMemberOfClearedSet should be able to advance a bucket by multiple eras.
    ASSERT_EQ(bucket1->lastChecked, 1);
    ASSERT_EQ(_bucketStateManager.getCountForEra(1), 1);
    ASSERT_EQ(_bucketStateManager.getCountForEra(3), 0);
    ASSERT_FALSE(cannotAccessBucket(bucket1));
    ASSERT_EQ(bucket1->lastChecked, 3);
    ASSERT_EQ(_bucketStateManager.getCountForEra(1), 0);
    ASSERT_EQ(_bucketStateManager.getCountForEra(3), 1);

    // _isMemberOfClearedSet works even if the bucket wasn't cleared in the most recent clear.
    clear(ns1);
    auto bucket6 = createBucket(info2);
    ASSERT_EQ(bucket6->lastChecked, 4);
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
    ASSERT_EQ(bucket1->lastChecked, 0);
    ASSERT_EQ(bucket2->lastChecked, 0);
    ASSERT_EQUALS(_bucketStateManager.getClearOperationsCount(), 0);
    clear(ns1);
    checkAndRemoveClearedBucket(bucket1);
    // Era 0 still has non-zero count after this clear because bucket2 is still in era 0.
    ASSERT_EQUALS(_bucketStateManager.getClearOperationsCount(), 1);
    clear(ns2);
    checkAndRemoveClearedBucket(bucket2);
    // Bucket2 gets deleted, which makes era 0's count decrease to 0, then clear registry gets
    // cleaned.
    ASSERT_EQUALS(_bucketStateManager.getClearOperationsCount(), 0);

    auto bucket3 = createBucket(info1);
    auto bucket4 = createBucket(info2);
    ASSERT_EQ(bucket3->lastChecked, 2);
    ASSERT_EQ(bucket4->lastChecked, 2);
    clear(ns1);
    checkAndRemoveClearedBucket(bucket3);
    // Era 2 still has bucket4 in it, so its count remains non-zero.
    ASSERT_EQUALS(_bucketStateManager.getClearOperationsCount(), 1);
    auto bucket5 = createBucket(info1);
    auto bucket6 = createBucket(info2);
    ASSERT_EQ(bucket5->lastChecked, 3);
    ASSERT_EQ(bucket6->lastChecked, 3);
    clear(ns1);
    checkAndRemoveClearedBucket(bucket5);
    // Eras 2 and 3 still have bucket4 and bucket6 in them respectively, so their counts remain
    // non-zero.
    ASSERT_EQUALS(_bucketStateManager.getClearOperationsCount(), 2);
    clear(ns2);
    checkAndRemoveClearedBucket(bucket4);
    checkAndRemoveClearedBucket(bucket6);
    // Eras 2 and 3 have their counts become 0 because bucket4 and bucket6 are cleared. The clear
    // registry is emptied.
    ASSERT_EQUALS(_bucketStateManager.getClearOperationsCount(), 0);

    auto bucket7 = createBucket(info1);
    auto bucket8 = createBucket(info3);
    ASSERT_EQ(bucket7->lastChecked, 5);
    ASSERT_EQ(bucket8->lastChecked, 5);
    clear(ns3);
    checkAndRemoveClearedBucket(bucket8);
    // Era 5 still has bucket7 in it so its count remains non-zero.
    ASSERT_EQUALS(_bucketStateManager.getClearOperationsCount(), 1);
    auto bucket9 = createBucket(info2);
    ASSERT_EQ(bucket9->lastChecked, 6);
    clear(ns2);
    checkAndRemoveClearedBucket(bucket9);
    // Era 6's count becomes 0. Since era 5 is the smallest era with non-zero count, no clear ops
    // are removed.
    ASSERT_EQUALS(_bucketStateManager.getClearOperationsCount(), 2);
    auto bucket10 = createBucket(info3);
    ASSERT_EQ(bucket10->lastChecked, 7);
    clear(ns3);
    checkAndRemoveClearedBucket(bucket10);
    // Era 7's count becomes 0. Since era 5 is the smallest era with non-zero count, no clear ops
    // are removed.
    ASSERT_EQUALS(_bucketStateManager.getClearOperationsCount(), 3);
    clear(ns1);
    checkAndRemoveClearedBucket(bucket7);
    // Era 5's count becomes 0. No eras with non-zero counts remain, so all clear ops are removed.
    ASSERT_EQUALS(_bucketStateManager.getClearOperationsCount(), 0);
}

TEST_F(BucketCatalogStateManagerTest, HasBeenClearedToleratesGapsInRegistry) {
    RAIIServerParameterControllerForTest controller{"featureFlagTimeseriesScalabilityImprovements",
                                                    true};

    auto bucket1 = createBucket(info1);
    ASSERT_EQ(bucket1->lastChecked, 0);
    clearById(ns1, OID());
    ASSERT_EQ(_bucketStateManager.getEra(), 2);
    clear(ns1);
    ASSERT_EQ(_bucketStateManager.getEra(), 3);
    ASSERT_TRUE(hasBeenCleared(bucket1));

    auto bucket2 = createBucket(info2);
    ASSERT_EQ(bucket2->lastChecked, 3);
    clearById(ns1, OID());
    clearById(ns1, OID());
    clearById(ns1, OID());
    ASSERT_EQ(_bucketStateManager.getEra(), 9);
    ASSERT_TRUE(hasBeenCleared(bucket1));
    ASSERT_FALSE(hasBeenCleared(bucket2));
    clear(ns2);
    ASSERT_EQ(_bucketStateManager.getEra(), 10);
    ASSERT_TRUE(hasBeenCleared(bucket1));
    ASSERT_TRUE(hasBeenCleared(bucket2));
}

TEST_F(BucketCatalogStateManagerTest, ArchivingBucketPreservesState) {
    RAIIServerParameterControllerForTest controller{"featureFlagTimeseriesScalabilityImprovements",
                                                    true};

    auto bucket = createBucket(info1);
    auto bucketId = bucket->bucketId;

    ClosedBuckets closedBuckets;
    _archiveBucket(&_stripes[info1.stripe], WithLock::withoutLock(), bucket, &closedBuckets);
    auto state = _bucketStateManager.getBucketState(bucketId);
    ASSERT_TRUE(state.has_value());
    ASSERT_TRUE(state == BucketState{});
}

TEST_F(BucketCatalogStateManagerTest, AbortingBatchRemovesBucketState) {
    RAIIServerParameterControllerForTest controller{"featureFlagTimeseriesScalabilityImprovements",
                                                    true};

    auto bucket = createBucket(info1);
    auto bucketId = bucket->bucketId;

    auto stats = _getExecutionStats(info1.key.ns);
    auto batch = std::make_shared<WriteBatch>(BucketHandle{bucketId, info1.stripe}, 0, stats);

    _abort(&_stripes[info1.stripe], WithLock::withoutLock(), batch, Status::OK());
    ASSERT(_bucketStateManager.getBucketState(bucketId) == boost::none);
}

TEST_F(BucketCatalogStateManagerTest, ClosingBucketGoesThroughPendingCompressionState) {
    RAIIServerParameterControllerForTest controller{"featureFlagTimeseriesScalabilityImprovements",
                                                    true};

    auto bucket = createBucket(info1);
    auto bucketId = bucket->bucketId;

    ASSERT(_bucketStateManager.getBucketState(bucketId).value() == BucketState{});

    auto stats = _getExecutionStats(info1.key.ns);
    auto batch = std::make_shared<WriteBatch>(BucketHandle{bucketId, info1.stripe}, 0, stats);
    ASSERT(claimWriteBatchCommitRights(*batch));
    ASSERT_OK(prepareCommit(batch));
    ASSERT(_bucketStateManager.getBucketState(bucketId).value() ==
           BucketState{}.setFlag(BucketStateFlag::kPrepared));

    {
        // Fool the system by marking the bucket for closure, then finish the batch so it detects
        // this and closes the bucket.
        bucket->rolloverAction = RolloverAction::kHardClose;
        CommitInfo commitInfo{};
        auto closedBucket = finish(batch, commitInfo);
        ASSERT(closedBucket.has_value());
        ASSERT_EQ(closedBucket.value().bucketId.oid, bucketId.oid);

        // Bucket should now be in pending compression state.
        ASSERT(_bucketStateManager.getBucketState(bucketId).has_value());
        ASSERT(_bucketStateManager.getBucketState(bucketId).value() ==
               BucketState{}.setFlag(BucketStateFlag::kPendingCompression));
    }

    // Destructing the 'ClosedBucket' struct should report it compressed should remove it from the
    // catalog.
    ASSERT(_bucketStateManager.getBucketState(bucketId) == boost::none);
}

TEST_F(BucketCatalogStateManagerTest, DirectWriteStartInitializesBucketState) {
    RAIIServerParameterControllerForTest controller{"featureFlagTimeseriesScalabilityImprovements",
                                                    true};

    auto bucketId = BucketId{ns1, OID()};
    directWriteStart(ns1, bucketId.oid);
    auto state = _bucketStateManager.getBucketState(bucketId);
    ASSERT_TRUE(state.has_value());
    ASSERT_TRUE(state.value().isSet(BucketStateFlag::kPendingDirectWrite));
}

TEST_F(BucketCatalogStateManagerTest, DirectWriteFinishRemovesBucketState) {
    RAIIServerParameterControllerForTest controller{"featureFlagTimeseriesScalabilityImprovements",
                                                    true};

    auto bucketId = BucketId{ns1, OID()};
    directWriteStart(ns1, bucketId.oid);
    auto state = _bucketStateManager.getBucketState(bucketId);
    ASSERT_TRUE(state.has_value());
    ASSERT_TRUE(state.value().isSet(BucketStateFlag::kPendingDirectWrite));

    directWriteFinish(ns1, bucketId.oid);
    state = _bucketStateManager.getBucketState(bucketId);
    ASSERT_FALSE(state.has_value());
}

}  // namespace
}  // namespace mongo::timeseries::bucket_catalog
