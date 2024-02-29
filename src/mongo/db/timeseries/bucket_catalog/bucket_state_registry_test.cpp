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

#include <boost/optional.hpp>
#include <memory>
#include <string>
#include <variant>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/oid.h"
#include "mongo/db/catalog/catalog_test_fixture.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/timeseries/bucket_catalog/bucket.h"
#include "mongo/db/timeseries/bucket_catalog/bucket_catalog.h"
#include "mongo/db/timeseries/bucket_catalog/bucket_catalog_internal.h"
#include "mongo/db/timeseries/bucket_catalog/bucket_identifiers.h"
#include "mongo/db/timeseries/bucket_catalog/bucket_metadata.h"
#include "mongo/db/timeseries/bucket_catalog/bucket_state_registry.h"
#include "mongo/db/timeseries/bucket_catalog/closed_bucket.h"
#include "mongo/db/timeseries/bucket_catalog/execution_stats.h"
#include "mongo/db/timeseries/bucket_catalog/rollover.h"
#include "mongo/db/timeseries/bucket_catalog/write_batch.h"
#include "mongo/db/timeseries/timeseries_gen.h"
#include "mongo/idl/server_parameter_test_util.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/framework.h"
#include "mongo/util/concurrency/with_lock.h"
#include "mongo/util/time_support.h"

namespace mongo::timeseries::bucket_catalog {
namespace {

class BucketStateRegistryTest : public BucketCatalog, public CatalogTestFixture {
public:
    BucketStateRegistryTest() {}

    void clearById(const UUID& uuid, const OID& oid) {
        directWriteStart(bucketStateRegistry, uuid, oid);
        directWriteFinish(bucketStateRegistry, uuid, oid);
    }

    bool hasBeenCleared(Bucket& bucket) {
        auto state = getBucketState(bucketStateRegistry, &bucket);
        if (!state.has_value()) {
            return false;
        }

        return visit(OverloadedVisitor{[](BucketState bucketState) {
                                           return bucketState == BucketState::kCleared ||
                                               bucketState == BucketState::kPreparedAndCleared;
                                       },
                                       [](DirectWriteCounter dwcount) {
                                           return false;
                                       }},
                     *state);
    }

    Bucket& createBucket(const internal::CreationInfo& info) {
        auto ptr = &internal::allocateBucket(
            operationContext(), *this, *stripes[info.stripe], withLock, info);
        ASSERT_FALSE(hasBeenCleared(*ptr));
        return *ptr;
    }

    bool cannotAccessBucket(Bucket& bucket) {
        if (hasBeenCleared(bucket)) {
            internal::removeBucket(*this,
                                   *stripes[internal::getStripeNumber(bucket.key, numberOfStripes)],
                                   withLock,
                                   bucket,
                                   internal::RemovalMode::kAbort);
            return true;
        } else {
            return false;
        }
    }

    void checkAndRemoveClearedBucket(Bucket& bucket) {
        auto a =
            internal::findBucket(bucketStateRegistry,
                                 *stripes[internal::getStripeNumber(bucket.key, numberOfStripes)],
                                 withLock,
                                 bucket.bucketId,
                                 internal::IgnoreBucketState::kYes);
        ASSERT(a == &bucket);
        auto b =
            internal::findBucket(bucketStateRegistry,
                                 *stripes[internal::getStripeNumber(bucket.key, numberOfStripes)],
                                 withLock,
                                 bucket.bucketId,
                                 internal::IgnoreBucketState::kNo);
        ASSERT(b == nullptr);
        internal::removeBucket(*this,
                               *stripes[internal::getStripeNumber(bucket.key, numberOfStripes)],
                               withLock,
                               bucket,
                               internal::RemovalMode::kAbort);
    }

    bool doesBucketStateMatch(const BucketId& bucketId,
                              boost::optional<BucketState> expectedBucketState) {
        auto state = getBucketState(bucketStateRegistry, bucketId);
        if (!state.has_value()) {
            // We don't expect the bucket to be tracked within the BucketStateRegistry.
            return !expectedBucketState.has_value();
        } else if (holds_alternative<DirectWriteCounter>(*state)) {
            // If the state is tracked by a direct write counter, then the states are not equal.
            return false;
        }

        // Interpret the variant value as BucketState and check it against the expected value.
        auto bucketState = std::get<BucketState>(*state);
        return bucketState == expectedBucketState.value();
    }

    bool doesBucketHaveDirectWrite(const BucketId& bucketId) {
        auto state = getBucketState(bucketStateRegistry, bucketId);
        return state.has_value() && holds_alternative<DirectWriteCounter>(*state);
    }

    WithLock withLock = WithLock::withoutLock();
    UUID uuid1 = UUID::gen();
    UUID uuid2 = UUID::gen();
    UUID uuid3 = UUID::gen();
    BSONElement elem;
    TrackingContext trackingContext;
    BucketMetadata bucketMetadata{trackingContext, elem, nullptr, boost::none};
    BucketKey bucketKey1{uuid1, bucketMetadata.cloneAsUntracked()};
    BucketKey bucketKey2{uuid2, bucketMetadata.cloneAsUntracked()};
    BucketKey bucketKey3{uuid3, bucketMetadata.cloneAsUntracked()};
    Date_t date = Date_t::now();
    TimeseriesOptions options;
    ExecutionStatsController stats = internal::getOrInitializeExecutionStats(*this, uuid1);
    ClosedBuckets closedBuckets;
    internal::CreationInfo info1{bucketKey1,
                                 internal::getStripeNumber(bucketKey1, numberOfStripes),
                                 date,
                                 options,
                                 stats,
                                 &closedBuckets};
    internal::CreationInfo info2{bucketKey2,
                                 internal::getStripeNumber(bucketKey2, numberOfStripes),
                                 date,
                                 options,
                                 stats,
                                 &closedBuckets};
    internal::CreationInfo info3{bucketKey3,
                                 internal::getStripeNumber(bucketKey3, numberOfStripes),
                                 date,
                                 options,
                                 stats,
                                 &closedBuckets};
};


TEST_F(BucketStateRegistryTest, TransitionsFromUntrackedState) {
    // Start with an untracked bucket in the registry.
    auto& bucket = createBucket(info1);
    stopTrackingBucketState(bucketStateRegistry, bucket.bucketId);
    ASSERT_TRUE(doesBucketStateMatch(bucket.bucketId, boost::none));

    // We expect a no-op when attempting to stop tracking an already untracked bucket.
    stopTrackingBucketState(bucketStateRegistry, bucket.bucketId);
    ASSERT_TRUE(doesBucketStateMatch(bucket.bucketId, boost::none));

    // We expect a no-op when clearing an untracked bucket.
    clearBucketState(bucketStateRegistry, bucket.bucketId);
    ASSERT_TRUE(doesBucketStateMatch(bucket.bucketId, boost::none));

    // We expect transition to 'kNormal' to succeed.
    ASSERT_OK(initializeBucketState(bucketStateRegistry, bucket.bucketId, /*bucket*/ nullptr));
    ASSERT_TRUE(doesBucketStateMatch(bucket.bucketId, BucketState::kNormal));
    // Reset the state.
    stopTrackingBucketState(bucketStateRegistry, bucket.bucketId);
    ASSERT_TRUE(doesBucketStateMatch(bucket.bucketId, boost::none));

    // We expect direct writes to succeed on untracked buckets.
    addDirectWrite(bucketStateRegistry, bucket.bucketId);
    ASSERT_TRUE(doesBucketHaveDirectWrite(bucket.bucketId));
    // Reset the state.
    removeDirectWrite(bucketStateRegistry, bucket.bucketId);
    ASSERT_TRUE(doesBucketStateMatch(bucket.bucketId, boost::none));

    // We expect transition to 'kFrozen' to succeed.
    freezeBucket(bucketStateRegistry, bucket.bucketId);
    ASSERT_TRUE(doesBucketStateMatch(bucket.bucketId, BucketState::kFrozen));
}

DEATH_TEST_F(BucketStateRegistryTest, CannotPrepareAnUntrackedBucket, "invariant") {
    // Start with an untracked bucket in the registry.
    auto& bucket = createBucket(info1);
    stopTrackingBucketState(bucketStateRegistry, bucket.bucketId);
    ASSERT_TRUE(doesBucketStateMatch(bucket.bucketId, boost::none));

    // We expect to invariant when attempting to prepare an untracked bucket.
    ASSERT_TRUE(prepareBucketState(bucketStateRegistry, bucket.bucketId) ==
                StateChangeSuccessful::kNo);
}

TEST_F(BucketStateRegistryTest, TransitionsFromNormalState) {
    // Start with a 'kNormal' bucket in the registry.
    auto& bucket = createBucket(info1);
    ASSERT_TRUE(doesBucketStateMatch(bucket.bucketId, BucketState::kNormal));

    // We expect transition to 'kNormal' to succeed.
    ASSERT_OK(initializeBucketState(bucketStateRegistry, bucket.bucketId, /*bucket*/ nullptr));
    ASSERT_TRUE(doesBucketStateMatch(bucket.bucketId, BucketState::kNormal));

    // We can stop tracking a 'kNormal' bucket.
    stopTrackingBucketState(bucketStateRegistry, bucket.bucketId);
    ASSERT_TRUE(doesBucketStateMatch(bucket.bucketId, boost::none));
    // Reset the state.
    ASSERT_OK(initializeBucketState(bucketStateRegistry, bucket.bucketId, /*bucket*/ nullptr));

    // We expect transition to 'kPrepared' to succeed.
    (void)prepareBucketState(bucketStateRegistry, bucket.bucketId);
    ASSERT_TRUE(doesBucketStateMatch(bucket.bucketId, BucketState::kPrepared));
    // Reset the state.
    (void)unprepareBucketState(bucketStateRegistry, bucket.bucketId);
    ASSERT_TRUE(doesBucketStateMatch(bucket.bucketId, BucketState::kNormal));

    // We expect transition to 'kClear' to succeed.
    clearBucketState(bucketStateRegistry, bucket.bucketId);
    ASSERT_TRUE(doesBucketStateMatch(bucket.bucketId, BucketState::kCleared));
    // Reset the state.
    stopTrackingBucketState(bucketStateRegistry, bucket.bucketId);
    ASSERT_OK(initializeBucketState(bucketStateRegistry, bucket.bucketId, /*bucket*/ nullptr));
    ASSERT_TRUE(doesBucketStateMatch(bucket.bucketId, BucketState::kNormal));

    // We expect direct writes to succeed on 'kNormal' buckets.
    addDirectWrite(bucketStateRegistry, bucket.bucketId);
    ASSERT_TRUE(doesBucketHaveDirectWrite(bucket.bucketId));
    // Reset the state.
    removeDirectWrite(bucketStateRegistry, bucket.bucketId);
    stopTrackingBucketState(bucketStateRegistry, bucket.bucketId);
    ASSERT_OK(initializeBucketState(bucketStateRegistry, bucket.bucketId, /*bucket*/ nullptr));
    ASSERT_TRUE(doesBucketStateMatch(bucket.bucketId, BucketState::kNormal));

    // We expect transition to 'kFrozen' to succeed.
    freezeBucket(bucketStateRegistry, bucket.bucketId);
    ASSERT_TRUE(doesBucketStateMatch(bucket.bucketId, BucketState::kFrozen));
}

TEST_F(BucketStateRegistryTest, TransitionsFromClearedState) {
    // Start with a 'kCleared' bucket in the registry.
    auto& bucket = createBucket(info1);
    clearBucketState(bucketStateRegistry, bucket.bucketId);
    ASSERT_TRUE(doesBucketStateMatch(bucket.bucketId, BucketState::kCleared));

    // We expect transition to 'kCleared' to succeed.
    clearBucketState(bucketStateRegistry, bucket.bucketId);
    ASSERT_TRUE(doesBucketStateMatch(bucket.bucketId, BucketState::kCleared));

    // We can stop tracking a 'kCleared' bucket.
    stopTrackingBucketState(bucketStateRegistry, bucket.bucketId);
    ASSERT_TRUE(doesBucketStateMatch(bucket.bucketId, boost::none));
    // Reset the state.
    ASSERT_OK(initializeBucketState(bucketStateRegistry, bucket.bucketId, /*bucket*/ nullptr));
    clearBucketState(bucketStateRegistry, bucket.bucketId);
    ASSERT_TRUE(doesBucketStateMatch(bucket.bucketId, BucketState::kCleared));

    // We expect transition to 'kNormal' to succeed.
    ASSERT_OK(initializeBucketState(bucketStateRegistry, bucket.bucketId).code());
    ASSERT_TRUE(doesBucketStateMatch(bucket.bucketId, BucketState::kNormal));
    // Reset the state.
    clearBucketState(bucketStateRegistry, bucket.bucketId);
    ASSERT_TRUE(doesBucketStateMatch(bucket.bucketId, BucketState::kCleared));

    // We expect transition to 'kPrepared' to fail.
    ASSERT_TRUE(prepareBucketState(bucketStateRegistry, bucket.bucketId) ==
                StateChangeSuccessful::kNo);
    ASSERT_TRUE(doesBucketStateMatch(bucket.bucketId, BucketState::kCleared));

    // We expect direct writes to succeed on 'kCleared' buckets.
    addDirectWrite(bucketStateRegistry, bucket.bucketId);
    ASSERT_TRUE(doesBucketHaveDirectWrite(bucket.bucketId));
    // Reset the state.
    removeDirectWrite(bucketStateRegistry, bucket.bucketId);
    clearBucketState(bucketStateRegistry, bucket.bucketId);
    ASSERT_TRUE(doesBucketStateMatch(bucket.bucketId, BucketState::kCleared));

    // We expect transition to 'kFrozen' to succeed.
    freezeBucket(bucketStateRegistry, bucket.bucketId);
    ASSERT_TRUE(doesBucketStateMatch(bucket.bucketId, BucketState::kFrozen));
}

TEST_F(BucketStateRegistryTest, TransitionsFromFrozenState) {
    // Start with a 'kFrozen' bucket in the registry.
    auto& bucket = createBucket(info1);
    freezeBucket(bucketStateRegistry, bucket.bucketId);
    ASSERT_TRUE(doesBucketStateMatch(bucket.bucketId, BucketState::kFrozen));

    // We expect transition to 'kCleared' to fail.
    clearBucketState(bucketStateRegistry, bucket.bucketId);
    ASSERT_TRUE(doesBucketStateMatch(bucket.bucketId, BucketState::kFrozen));

    // We expect transition to 'kPrepared' to fail.
    ASSERT_TRUE(prepareBucketState(bucketStateRegistry, bucket.bucketId) ==
                StateChangeSuccessful::kNo);
    ASSERT_TRUE(doesBucketStateMatch(bucket.bucketId, BucketState::kFrozen));

    // We expect direct writes leave the state as 'kFrozen'.
    addDirectWrite(bucketStateRegistry, bucket.bucketId);
    ASSERT_FALSE(doesBucketHaveDirectWrite(bucket.bucketId));
    ASSERT_TRUE(doesBucketStateMatch(bucket.bucketId, BucketState::kFrozen));

    // We cannot untrack a 'kFrozen' bucket.
    stopTrackingBucketState(bucketStateRegistry, bucket.bucketId);
    ASSERT_TRUE(doesBucketStateMatch(bucket.bucketId, BucketState::kFrozen));

    // We cannot initialize bucket state for a bucket that is already frozen.
    auto status = initializeBucketState(bucketStateRegistry, bucket.bucketId, /*bucket*/ nullptr);
    ASSERT_NOT_OK(status);
    ASSERT_EQ(status.code(), ErrorCodes::TimeseriesBucketFrozen);
}

TEST_F(BucketStateRegistryTest, TransitionsFromPreparedState) {
    // Start with a 'kPrepared' bucket in the registry.
    auto& bucket = createBucket(info1);
    (void)prepareBucketState(bucketStateRegistry, bucket.bucketId);
    ASSERT_TRUE(doesBucketStateMatch(bucket.bucketId, BucketState::kPrepared));

    // We expect direct writes to fail and leave the state as 'kPrepared'.
    (void)addDirectWrite(bucketStateRegistry, bucket.bucketId);
    ASSERT_TRUE(doesBucketStateMatch(bucket.bucketId, BucketState::kPrepared));

    // We expect unpreparing bucket will transition the bucket state to 'kNormal'.
    unprepareBucketState(bucketStateRegistry, bucket.bucketId);
    ASSERT_TRUE(doesBucketStateMatch(bucket.bucketId, BucketState::kNormal));
    // Reset the state.
    (void)prepareBucketState(bucketStateRegistry, bucket.bucketId);
    ASSERT_TRUE(doesBucketStateMatch(bucket.bucketId, BucketState::kPrepared));

    // We expect transition to 'kCleared' to succeed and update the state as 'kPreparedAndCleared'.
    clearBucketState(bucketStateRegistry, bucket.bucketId);
    ASSERT_TRUE(doesBucketStateMatch(bucket.bucketId, BucketState::kPreparedAndCleared));
    // Reset the state.
    stopTrackingBucketState(bucketStateRegistry, bucket.bucketId);
    ASSERT_OK(initializeBucketState(bucketStateRegistry, bucket.bucketId, /*bucket*/ nullptr));
    (void)prepareBucketState(bucketStateRegistry, bucket.bucketId);
    ASSERT_TRUE(doesBucketStateMatch(bucket.bucketId, BucketState::kPrepared));

    // We can untrack a 'kPrepared' bucket
    stopTrackingBucketState(bucketStateRegistry, bucket.bucketId);
    ASSERT_TRUE(doesBucketStateMatch(bucket.bucketId, boost::none));
    // Reset the state.
    ASSERT_OK(initializeBucketState(bucketStateRegistry, bucket.bucketId, /*bucket*/ nullptr));
    (void)prepareBucketState(bucketStateRegistry, bucket.bucketId);
    ASSERT_TRUE(doesBucketStateMatch(bucket.bucketId, BucketState::kPrepared));

    // We expect transition to 'kFrozen' to succeed.
    freezeBucket(bucketStateRegistry, bucket.bucketId);
    ASSERT_TRUE(doesBucketStateMatch(bucket.bucketId, BucketState::kPreparedAndFrozen));
}

DEATH_TEST_F(BucketStateRegistryTest, CannotInitializeAPreparedBucket, "invariant") {
    // Start with a 'kPrepared' bucket in the registry.
    auto& bucket = createBucket(info1);
    ASSERT_OK(initializeBucketState(bucketStateRegistry, bucket.bucketId));
    (void)prepareBucketState(bucketStateRegistry, bucket.bucketId);
    ASSERT_TRUE(doesBucketStateMatch(bucket.bucketId, BucketState::kPrepared));

    // We expect to invariant when attempting to prepare an 'kPrepared' bucket.
    ASSERT_OK(initializeBucketState(bucketStateRegistry, bucket.bucketId));
}

DEATH_TEST_F(BucketStateRegistryTest, CannotPrepareAnAlreadyPreparedBucket, "invariant") {
    // Start with a 'kPrepared' bucket in the registry.
    auto& bucket = createBucket(info1);
    ASSERT_OK(initializeBucketState(bucketStateRegistry, bucket.bucketId));
    (void)prepareBucketState(bucketStateRegistry, bucket.bucketId);
    ASSERT_TRUE(doesBucketStateMatch(bucket.bucketId, BucketState::kPrepared));
    // We expect to invariant when attempting to prepare an untracked bucket.
    (void)prepareBucketState(bucketStateRegistry, bucket.bucketId);
}

TEST_F(BucketStateRegistryTest, TransitionsFromPreparedAndClearedState) {
    // Start with a 'kPreparedAndCleared' bucket in the registry.
    auto& bucket = createBucket(info1);
    (void)prepareBucketState(bucketStateRegistry, bucket.bucketId);
    clearBucketState(bucketStateRegistry, bucket.bucketId);
    ASSERT_TRUE(doesBucketStateMatch(bucket.bucketId, BucketState::kPreparedAndCleared));

    // We expect transition to 'kPrepared' to fail.
    ASSERT_TRUE(prepareBucketState(bucketStateRegistry, bucket.bucketId) ==
                StateChangeSuccessful::kNo);
    ASSERT_TRUE(doesBucketStateMatch(bucket.bucketId, BucketState::kPreparedAndCleared));

    // We expect direct writes to fail and leave the state as 'kPreparedAndCleared'.
    (void)addDirectWrite(bucketStateRegistry, bucket.bucketId);
    ASSERT_TRUE(doesBucketStateMatch(bucket.bucketId, BucketState::kPreparedAndCleared));

    // We expect clearing the bucket state will not affect the state.
    clearBucketState(bucketStateRegistry, bucket.bucketId);
    ASSERT_TRUE(doesBucketStateMatch(bucket.bucketId, BucketState::kPreparedAndCleared));

    // We expect untracking 'kPreparedAndCleared' buckets to remove the state.
    stopTrackingBucketState(bucketStateRegistry, bucket.bucketId);
    ASSERT_TRUE(doesBucketStateMatch(bucket.bucketId, boost::none));
    // Reset the state.
    ASSERT_OK(initializeBucketState(bucketStateRegistry, bucket.bucketId));
    ASSERT_TRUE(prepareBucketState(bucketStateRegistry, bucket.bucketId) ==
                StateChangeSuccessful::kYes);
    clearBucketState(bucketStateRegistry, bucket.bucketId);
    ASSERT_TRUE(doesBucketStateMatch(bucket.bucketId, BucketState::kPreparedAndCleared));

    // We expect unpreparing 'kPreparedAndCleared' buckets to transition to 'kCleared'.
    unprepareBucketState(bucketStateRegistry, bucket.bucketId);
    ASSERT_TRUE(doesBucketStateMatch(bucket.bucketId, BucketState::kCleared));
    // Reset the state.
    stopTrackingBucketState(bucketStateRegistry, bucket.bucketId);
    ASSERT_TRUE(doesBucketStateMatch(bucket.bucketId, boost::none));
    ASSERT_OK(initializeBucketState(bucketStateRegistry, bucket.bucketId));
    ASSERT_TRUE(prepareBucketState(bucketStateRegistry, bucket.bucketId) ==
                StateChangeSuccessful::kYes);
    clearBucketState(bucketStateRegistry, bucket.bucketId);
    ASSERT_TRUE(doesBucketStateMatch(bucket.bucketId, BucketState::kPreparedAndCleared));

    // We expect transition to 'kFrozen' to succeed.
    freezeBucket(bucketStateRegistry, bucket.bucketId);
    ASSERT_TRUE(doesBucketStateMatch(bucket.bucketId, BucketState::kPreparedAndFrozen));
}

TEST_F(BucketStateRegistryTest, TransitionsFromPreparedAndFrozenState) {
    // Start with a 'kPreparedAndFrozen' bucket in the registry.
    auto& bucket = createBucket(info1);
    (void)prepareBucketState(bucketStateRegistry, bucket.bucketId);
    freezeBucket(bucketStateRegistry, bucket.bucketId);
    ASSERT_TRUE(doesBucketStateMatch(bucket.bucketId, BucketState::kPreparedAndFrozen));
    auto& bucket2 = createBucket(info2);
    (void)prepareBucketState(bucketStateRegistry, bucket2.bucketId);
    freezeBucket(bucketStateRegistry, bucket2.bucketId);
    ASSERT_TRUE(doesBucketStateMatch(bucket2.bucketId, BucketState::kPreparedAndFrozen));

    // We expect transition to 'kPrepared' to fail.
    ASSERT_TRUE(prepareBucketState(bucketStateRegistry, bucket.bucketId) ==
                StateChangeSuccessful::kNo);
    ASSERT_TRUE(doesBucketStateMatch(bucket.bucketId, BucketState::kPreparedAndFrozen));

    // We expect direct writes to fail and leave the state as 'kPreparedAndCleared'.
    (void)addDirectWrite(bucketStateRegistry, bucket.bucketId);
    ASSERT_TRUE(doesBucketStateMatch(bucket.bucketId, BucketState::kPreparedAndFrozen));

    // We expect clearing the bucket state will not affect the state.
    clearBucketState(bucketStateRegistry, bucket.bucketId);
    ASSERT_TRUE(doesBucketStateMatch(bucket.bucketId, BucketState::kPreparedAndFrozen));

    // We expect untracking 'kPreparedAndFrozen' buckets to transition the state to 'kFrozen'.
    stopTrackingBucketState(bucketStateRegistry, bucket.bucketId);
    ASSERT_TRUE(doesBucketStateMatch(bucket.bucketId, BucketState::kFrozen));

    // We expect unpreparing 'kPreparedAndCleared' buckets to transition to 'kFrozen'.
    unprepareBucketState(bucketStateRegistry, bucket2.bucketId);
    ASSERT_TRUE(doesBucketStateMatch(bucket2.bucketId, BucketState::kFrozen));
}

TEST_F(BucketStateRegistryTest, TransitionsFromDirectWriteState) {
    // Start with a bucket with a direct write in the registry.
    auto& bucket = createBucket(info1);
    ASSERT_OK(initializeBucketState(bucketStateRegistry, bucket.bucketId));
    auto bucketState = addDirectWrite(bucketStateRegistry, bucket.bucketId);
    ASSERT_TRUE(doesBucketHaveDirectWrite(bucket.bucketId));
    auto originalDirectWriteCount = std::get<DirectWriteCounter>(bucketState);

    // We expect future direct writes to add-on.
    bucketState = addDirectWrite(bucketStateRegistry, bucket.bucketId);
    auto newDirectWriteCount = std::get<DirectWriteCounter>(bucketState);
    ASSERT_GT(newDirectWriteCount, originalDirectWriteCount);

    // We expect untracking to leave the state unaffected.
    stopTrackingBucketState(bucketStateRegistry, bucket.bucketId);
    ASSERT_TRUE(doesBucketHaveDirectWrite(bucket.bucketId));

    // We expect transition to 'kNormal' to return a WriteConflict.
    ASSERT_EQ(initializeBucketState(bucketStateRegistry, bucket.bucketId),
              ErrorCodes::WriteConflict);
    ASSERT_TRUE(doesBucketHaveDirectWrite(bucket.bucketId));

    // We expect transition to 'kCleared' to leave the state unaffected.
    clearBucketState(bucketStateRegistry, bucket.bucketId);
    ASSERT_TRUE(doesBucketHaveDirectWrite(bucket.bucketId));

    // We expect transition to 'kPrepared' to leave the state unaffected.
    (void)prepareBucketState(bucketStateRegistry, bucket.bucketId);
    ASSERT_TRUE(doesBucketHaveDirectWrite(bucket.bucketId));

    // We expect transition to 'kFrozen' to succeed.
    freezeBucket(bucketStateRegistry, bucket.bucketId);
    ASSERT_TRUE(doesBucketStateMatch(bucket.bucketId, BucketState::kFrozen));
}

TEST_F(BucketStateRegistryTest, EraAdvancesAsExpected) {
    // When allocating new buckets, we expect their era value to match the BucketCatalog's era.
    ASSERT_EQ(getCurrentEra(bucketStateRegistry), 0);
    auto& bucket1 = createBucket(info1);
    ASSERT_EQ(getCurrentEra(bucketStateRegistry), 0);
    ASSERT_EQ(bucket1.lastChecked, 0);

    // When clearing buckets, we expect the BucketCatalog's era value to increase while the cleared
    // bucket era values should remain unchanged.
    clear(*this, uuid1);
    ASSERT_EQ(getCurrentEra(bucketStateRegistry), 1);
    ASSERT_EQ(bucket1.lastChecked, 0);

    // When clearing buckets of one namespace, we expect the era of buckets of any other namespace
    // to not change.
    auto& bucket2 = createBucket(info1);
    auto& bucket3 = createBucket(info2);
    ASSERT_EQ(getCurrentEra(bucketStateRegistry), 1);
    ASSERT_EQ(bucket2.lastChecked, 1);
    ASSERT_EQ(bucket3.lastChecked, 1);
    clear(*this, uuid1);
    ASSERT_EQ(getCurrentEra(bucketStateRegistry), 2);
    ASSERT_EQ(bucket3.lastChecked, 1);
    ASSERT_EQ(bucket1.lastChecked, 0);
    ASSERT_EQ(bucket2.lastChecked, 1);

    // Era also advances when clearing by OID
    clearById(uuid1, OID());
    ASSERT_EQ(getCurrentEra(bucketStateRegistry), 4);
}

TEST_F(BucketStateRegistryTest, EraCountMapUpdatedCorrectly) {
    // Creating a bucket in a new era should add a counter for that era to the map.
    auto& bucket1 = createBucket(info1);
    ASSERT_EQ(bucket1.lastChecked, 0);
    ASSERT_EQ(getBucketCountForEra(bucketStateRegistry, 0), 1);
    clear(*this, uuid1);
    checkAndRemoveClearedBucket(bucket1);

    // When the last bucket in an era is destructed, the counter in the map should be removed.
    ASSERT_EQ(getBucketCountForEra(bucketStateRegistry, 0), 0);

    // If there are still buckets in the era, however, the counter should still exist in the
    // map.
    auto& bucket2 = createBucket(info1);
    auto& bucket3 = createBucket(info2);
    ASSERT_EQ(bucket2.lastChecked, 1);
    ASSERT_EQ(bucket3.lastChecked, 1);
    ASSERT_EQ(getBucketCountForEra(bucketStateRegistry, 1), 2);
    clear(*this, uuid2);
    checkAndRemoveClearedBucket(bucket3);
    ASSERT_EQ(getBucketCountForEra(bucketStateRegistry, 1), 1);

    // A bucket in one era being destroyed and the counter decrementing should not affect a
    // different era's counter.
    auto& bucket4 = createBucket(info2);
    ASSERT_EQ(bucket4.lastChecked, 2);
    ASSERT_EQ(getBucketCountForEra(bucketStateRegistry, 2), 1);
    clear(*this, uuid2);
    checkAndRemoveClearedBucket(bucket4);
    ASSERT_EQ(getBucketCountForEra(bucketStateRegistry, 2), 0);
    ASSERT_EQ(getBucketCountForEra(bucketStateRegistry, 1), 1);
}

TEST_F(BucketStateRegistryTest, HasBeenClearedFunctionReturnsAsExpected) {
    auto& bucket1 = createBucket(info1);
    auto& bucket2 = createBucket(info2);
    ASSERT_EQ(bucket1.lastChecked, 0);
    ASSERT_EQ(bucket2.lastChecked, 0);

    // After a clear operation, _isMemberOfClearedSet returns whether a particular bucket was
    // cleared or not. It also advances the bucket's era up to the most recent era.
    ASSERT_FALSE(cannotAccessBucket(bucket1));
    ASSERT_FALSE(cannotAccessBucket(bucket2));
    ASSERT_EQ(getBucketCountForEra(bucketStateRegistry, 0), 2);
    clear(*this, uuid2);
    ASSERT_FALSE(cannotAccessBucket(bucket1));
    ASSERT_EQ(getBucketCountForEra(bucketStateRegistry, 0), 1);
    ASSERT_EQ(bucket1.lastChecked, 1);
    ASSERT(cannotAccessBucket(bucket2));

    // Sanity check that all this still works with multiple buckets in a namespace being cleared.
    auto& bucket3 = createBucket(info2);
    auto& bucket4 = createBucket(info2);
    ASSERT_EQ(bucket3.lastChecked, 1);
    ASSERT_EQ(bucket4.lastChecked, 1);
    clear(*this, uuid2);
    ASSERT(cannotAccessBucket(bucket3));
    ASSERT(cannotAccessBucket(bucket4));
    auto& bucket5 = createBucket(info2);
    ASSERT_EQ(bucket5.lastChecked, 2);
    clear(*this, uuid2);
    ASSERT(cannotAccessBucket(bucket5));
    // _isMemberOfClearedSet should be able to advance a bucket by multiple eras.
    ASSERT_EQ(bucket1.lastChecked, 1);
    ASSERT_EQ(getBucketCountForEra(bucketStateRegistry, 1), 1);
    ASSERT_EQ(getBucketCountForEra(bucketStateRegistry, 3), 0);
    ASSERT_FALSE(cannotAccessBucket(bucket1));
    ASSERT_EQ(bucket1.lastChecked, 3);
    ASSERT_EQ(getBucketCountForEra(bucketStateRegistry, 1), 0);
    ASSERT_EQ(getBucketCountForEra(bucketStateRegistry, 3), 1);

    // _isMemberOfClearedSet works even if the bucket wasn't cleared in the most recent clear.
    clear(*this, uuid1);
    auto& bucket6 = createBucket(info2);
    ASSERT_EQ(bucket6.lastChecked, 4);
    clear(*this, uuid2);
    ASSERT_EQ(getBucketCountForEra(bucketStateRegistry, 3), 1);
    ASSERT_EQ(getBucketCountForEra(bucketStateRegistry, 4), 1);
    ASSERT(cannotAccessBucket(bucket1));
    ASSERT(cannotAccessBucket(bucket6));
    ASSERT_EQ(getBucketCountForEra(bucketStateRegistry, 3), 0);
    ASSERT_EQ(getBucketCountForEra(bucketStateRegistry, 4), 0);
}

TEST_F(BucketStateRegistryTest, ClearRegistryGarbageCollection) {
    auto& bucket1 = createBucket(info1);
    auto& bucket2 = createBucket(info2);
    ASSERT_EQ(bucket1.lastChecked, 0);
    ASSERT_EQ(bucket2.lastChecked, 0);
    ASSERT_EQUALS(getClearedSetsCount(bucketStateRegistry), 0);
    clear(*this, uuid1);
    checkAndRemoveClearedBucket(bucket1);
    // Era 0 still has non-zero count after this clear because bucket2 is still in era 0.
    ASSERT_EQUALS(getClearedSetsCount(bucketStateRegistry), 1);
    clear(*this, uuid2);
    checkAndRemoveClearedBucket(bucket2);
    // Bucket2 gets deleted, which makes era 0's count decrease to 0, then clear registry gets
    // cleaned.
    ASSERT_EQUALS(getClearedSetsCount(bucketStateRegistry), 0);

    auto& bucket3 = createBucket(info1);
    auto& bucket4 = createBucket(info2);
    ASSERT_EQ(bucket3.lastChecked, 2);
    ASSERT_EQ(bucket4.lastChecked, 2);
    clear(*this, uuid1);
    checkAndRemoveClearedBucket(bucket3);
    // Era 2 still has bucket4 in it, so its count remains non-zero.
    ASSERT_EQUALS(getClearedSetsCount(bucketStateRegistry), 1);
    auto& bucket5 = createBucket(info1);
    auto& bucket6 = createBucket(info2);
    ASSERT_EQ(bucket5.lastChecked, 3);
    ASSERT_EQ(bucket6.lastChecked, 3);
    clear(*this, uuid1);
    checkAndRemoveClearedBucket(bucket5);
    // Eras 2 and 3 still have bucket4 and bucket6 in them respectively, so their counts remain
    // non-zero.
    ASSERT_EQUALS(getClearedSetsCount(bucketStateRegistry), 2);
    clear(*this, uuid2);
    checkAndRemoveClearedBucket(bucket4);
    checkAndRemoveClearedBucket(bucket6);
    // Eras 2 and 3 have their counts become 0 because bucket4 and bucket6 are cleared. The clear
    // registry is emptied.
    ASSERT_EQUALS(getClearedSetsCount(bucketStateRegistry), 0);

    auto& bucket7 = createBucket(info1);
    auto& bucket8 = createBucket(info3);
    ASSERT_EQ(bucket7.lastChecked, 5);
    ASSERT_EQ(bucket8.lastChecked, 5);
    clear(*this, uuid3);
    checkAndRemoveClearedBucket(bucket8);
    // Era 5 still has bucket7 in it so its count remains non-zero.
    ASSERT_EQUALS(getClearedSetsCount(bucketStateRegistry), 1);
    auto& bucket9 = createBucket(info2);
    ASSERT_EQ(bucket9.lastChecked, 6);
    clear(*this, uuid2);
    checkAndRemoveClearedBucket(bucket9);
    // Era 6's count becomes 0. Since era 5 is the smallest era with non-zero count, no clear ops
    // are removed.
    ASSERT_EQUALS(getClearedSetsCount(bucketStateRegistry), 2);
    auto& bucket10 = createBucket(info3);
    ASSERT_EQ(bucket10.lastChecked, 7);
    clear(*this, uuid3);
    checkAndRemoveClearedBucket(bucket10);
    // Era 7's count becomes 0. Since era 5 is the smallest era with non-zero count, no clear ops
    // are removed.
    ASSERT_EQUALS(getClearedSetsCount(bucketStateRegistry), 3);
    clear(*this, uuid1);
    checkAndRemoveClearedBucket(bucket7);
    // Era 5's count becomes 0. No eras with non-zero counts remain, so all clear ops are removed.
    ASSERT_EQUALS(getClearedSetsCount(bucketStateRegistry), 0);
}

TEST_F(BucketStateRegistryTest, HasBeenClearedToleratesGapsInRegistry) {
    auto& bucket1 = createBucket(info1);
    ASSERT_EQ(bucket1.lastChecked, 0);
    clearById(uuid1, OID());
    ASSERT_EQ(getCurrentEra(bucketStateRegistry), 2);
    clear(*this, uuid1);
    ASSERT_EQ(getCurrentEra(bucketStateRegistry), 3);
    ASSERT_TRUE(hasBeenCleared(bucket1));

    auto& bucket2 = createBucket(info2);
    ASSERT_EQ(bucket2.lastChecked, 3);
    clearById(uuid1, OID());
    clearById(uuid1, OID());
    clearById(uuid1, OID());
    ASSERT_EQ(getCurrentEra(bucketStateRegistry), 9);
    ASSERT_TRUE(hasBeenCleared(bucket1));
    ASSERT_FALSE(hasBeenCleared(bucket2));
    clear(*this, uuid2);
    ASSERT_EQ(getCurrentEra(bucketStateRegistry), 10);
    ASSERT_TRUE(hasBeenCleared(bucket1));
    ASSERT_TRUE(hasBeenCleared(bucket2));
}

TEST_F(BucketStateRegistryTest, ArchivingBucketPreservesState) {
    auto& bucket = createBucket(info1);
    auto bucketId = bucket.bucketId;

    ClosedBuckets closedBuckets;
    internal::archiveBucket(operationContext(),
                            *this,
                            *stripes[info1.stripe],
                            WithLock::withoutLock(),
                            bucket,
                            closedBuckets);
    auto state = getBucketState(bucketStateRegistry, bucketId);
    ASSERT_TRUE(doesBucketStateMatch(bucketId, BucketState::kNormal));
}

TEST_F(BucketStateRegistryTest, AbortingBatchRemovesBucketState) {
    auto& bucket = createBucket(info1);
    auto bucketId = bucket.bucketId;

    auto stats = internal::getOrInitializeExecutionStats(*this, info1.key.collectionUUID);
    TrackingContext trackingContext;
    auto batch =
        std::make_shared<WriteBatch>(trackingContext,
                                     BucketHandle{bucketId, info1.stripe},
                                     info1.key.cloneAsUntracked(),
                                     0,
                                     stats,
                                     StringData{bucket.timeField.data(), bucket.timeField.size()});

    internal::abort(*this, *stripes[info1.stripe], WithLock::withoutLock(), batch, Status::OK());
    ASSERT(getBucketState(bucketStateRegistry, bucketId) == boost::none);
}

TEST_F(BucketStateRegistryTest, ClosingBucketGoesThroughPendingCompressionState) {
    NamespaceString ns = NamespaceString::createNamespaceString_forTest("test.foo");
    auto& bucket = createBucket(info1);
    auto bucketId = bucket.bucketId;

    ASSERT_TRUE(doesBucketStateMatch(bucketId, BucketState::kNormal));

    auto stats = internal::getOrInitializeExecutionStats(*this, info1.key.collectionUUID);
    TrackingContext trackingContext;
    auto batch =
        std::make_shared<WriteBatch>(trackingContext,
                                     BucketHandle{bucketId, info1.stripe},
                                     info1.key.cloneAsUntracked(),
                                     0,
                                     stats,
                                     StringData{bucket.timeField.data(), bucket.timeField.size()});
    ASSERT(claimWriteBatchCommitRights(*batch));
    ASSERT_OK(prepareCommit(*this, ns, batch));
    ASSERT_TRUE(doesBucketStateMatch(bucketId, BucketState::kPrepared));

    {
        // Fool the system by marking the bucket for closure, then finish the batch so it detects
        // this and closes the bucket.
        bucket.rolloverAction = RolloverAction::kHardClose;
        CommitInfo commitInfo{};
        auto closedBucket = finish(operationContext(), *this, ns, batch, commitInfo);
        ASSERT(closedBucket.has_value());
        ASSERT_EQ(closedBucket.value().bucketId.oid, bucketId.oid);

        // Bucket should now be in pending compression state represented by direct write.
        ASSERT_TRUE(doesBucketHaveDirectWrite(bucketId));
    }

    // Destructing the 'ClosedBucket' struct should report it compressed should remove it from the
    // catalog.
    ASSERT_TRUE(doesBucketStateMatch(bucketId, boost::none));
}

TEST_F(BucketStateRegistryTest, DirectWriteStartInitializesBucketState) {
    auto bucketId = BucketId{uuid1, OID()};
    directWriteStart(bucketStateRegistry, uuid1, bucketId.oid);
    ASSERT_TRUE(doesBucketHaveDirectWrite(bucketId));
}

TEST_F(BucketStateRegistryTest, DirectWriteFinishRemovesBucketState) {
    auto bucketId = BucketId{uuid1, OID()};
    directWriteStart(bucketStateRegistry, uuid1, bucketId.oid);
    ASSERT_TRUE(doesBucketHaveDirectWrite(bucketId));

    directWriteFinish(bucketStateRegistry, uuid1, bucketId.oid);
    ASSERT_TRUE(doesBucketStateMatch(bucketId, boost::none));
}

TEST_F(BucketStateRegistryTest, TestDirectWriteStartCounter) {
    auto& bucket = createBucket(info1);
    auto bucketId = bucket.bucketId;

    // Under the hood, the BucketState will contain a counter on the number of ongoing DirectWrites.
    DirectWriteCounter dwCounter = 0;

    // If no direct write has been initiated, the direct write counter should be 0.
    auto state = getBucketState(bucketStateRegistry, bucketId);
    ASSERT_TRUE(state.has_value());
    ASSERT_TRUE(holds_alternative<BucketState>(*state));

    // Start a direct write and ensure the counter is incremented correctly.
    while (dwCounter < 4) {
        directWriteStart(bucketStateRegistry, uuid1, bucketId.oid);
        dwCounter++;
        ASSERT_TRUE(doesBucketHaveDirectWrite(bucketId));
    }

    while (dwCounter > 1) {
        directWriteFinish(bucketStateRegistry, uuid1, bucketId.oid);
        dwCounter--;
        ASSERT_TRUE(doesBucketHaveDirectWrite(bucketId));
    }

    // When the number of direct writes reaches 0, we should clear the bucket.
    directWriteFinish(bucketStateRegistry, uuid1, bucketId.oid);
    ASSERT_FALSE(doesBucketHaveDirectWrite(bucketId));
    ASSERT_TRUE(doesBucketStateMatch(bucketId, BucketState::kCleared));
}

TEST_F(BucketStateRegistryTest, ConflictingDirectWrites) {
    // While two direct writes (e.g. two racing updates) should correctly conflict at the storage
    // engine layer, we expect the directWriteStart/Finish pairs to work successfully.
    BucketId bucketId{uuid1, OID()};
    auto state = getBucketState(bucketStateRegistry, bucketId);
    ASSERT_FALSE(state.has_value());

    // First direct write initializes state as untracked.
    directWriteStart(bucketStateRegistry, bucketId.collectionUUID, bucketId.oid);
    ASSERT_TRUE(doesBucketHaveDirectWrite(bucketId));

    directWriteStart(bucketStateRegistry, bucketId.collectionUUID, bucketId.oid);

    // First finish does not remove the state from the registry.
    directWriteFinish(bucketStateRegistry, bucketId.collectionUUID, bucketId.oid);
    ASSERT_TRUE(doesBucketHaveDirectWrite(bucketId));

    // Second one removes it.
    directWriteFinish(bucketStateRegistry, bucketId.collectionUUID, bucketId.oid);
    ASSERT_TRUE(doesBucketStateMatch(bucketId, boost::none));
}

TEST_F(BucketStateRegistryTest, LargeNumberOfDirectWritesInTransaction) {
    // If a single transaction contains many direct writes to the same bucket, we should handle
    // it gracefully.
    BucketId bucketId{uuid1, OID()};
    auto state = getBucketState(bucketStateRegistry, bucketId);
    ASSERT_FALSE(state.has_value());

    int numDirectWrites = 100'000;

    for (int i = 0; i < numDirectWrites; ++i) {
        directWriteStart(bucketStateRegistry, bucketId.collectionUUID, bucketId.oid);
        ASSERT_TRUE(doesBucketHaveDirectWrite(bucketId));
    }

    for (int i = 0; i < numDirectWrites; ++i) {
        ASSERT_TRUE(doesBucketHaveDirectWrite(bucketId));
        directWriteFinish(bucketStateRegistry, bucketId.collectionUUID, bucketId.oid);
    }

    ASSERT_FALSE(doesBucketHaveDirectWrite(bucketId));
    ASSERT_TRUE(doesBucketStateMatch(bucketId, boost::none));
}

}  // namespace
}  // namespace mongo::timeseries::bucket_catalog
