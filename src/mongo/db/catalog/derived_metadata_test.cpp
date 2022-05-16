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

#include "mongo/platform/basic.h"

#include "mongo/db/catalog/derived_metadata_collection_controller.h"
#include "mongo/db/catalog/derived_metadata_collection_count.h"
#include "mongo/db/catalog/derived_metadata_common.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {
std::vector<DerivedMetadataDelta> makeDeltaCopiesWithIncreasingTimestamps(
    const DerivedMetadataDelta& delta, const Timestamp& startTimestamp, int numCopies) {
    std::vector<DerivedMetadataDelta> deltas;
    Timestamp deltaTimestamp = startTimestamp;

    for (int i = 0; i < numCopies; i++) {
        DerivedMetadataDelta newDelta = delta;
        newDelta.timestamp = deltaTimestamp;
        deltas.push_back(newDelta);

        deltaTimestamp = deltaTimestamp + 1;
    }

    return deltas;
}

void bulkInsertDeltaCopiesWithIncreasingTimestamp(DerivedMetadataCollectionController& dmcc,
                                                  const DerivedMetadataDelta& delta,
                                                  const Timestamp& startTimestamp,
                                                  int numCopies) {
    auto deltas = makeDeltaCopiesWithIncreasingTimestamps(delta, startTimestamp, numCopies);
    for (auto& d : deltas) {
        Timestamp deltaTimestamp = d.timestamp;
        dmcc.registerDelta(std::move(d), deltaTimestamp);
    }
}
}  // namespace

TEST(DerivedMetadataTest, CalculateCountDeltas) {
    DerivedMetadataCount dmdCount;
    BSONObj emptyDoc = BSONObj();
    BSONObj aDoc = BSON("a" << 1);
    BSONObj bDoc = BSON("b" << 1);

    // Simulate the insert path.
    DocumentWriteImages insertWrite = {emptyDoc, aDoc};
    DerivedMetadataDelta insertDelta;
    DerivedMetadataCount::populateDelta(insertWrite, &insertDelta);
    ASSERT_EQUALS(insertDelta.countUpdate, 1);

    // Simulate the delete path.
    DocumentWriteImages deleteWrite = {aDoc, emptyDoc};
    DerivedMetadataDelta deleteDelta;
    DerivedMetadataCount::populateDelta(deleteWrite, &deleteDelta);
    ASSERT_EQUALS(deleteDelta.countUpdate, -1);

    // Simulate update path.
    DocumentWriteImages updateWrite = {aDoc, bDoc};
    DerivedMetadataDelta updateDelta;
    DerivedMetadataCount::populateDelta(updateWrite, &updateDelta);
    ASSERT_EQUALS(updateDelta.countUpdate, 0);
}

TEST(DerivedMetadataTest, ApplyDeltaVector) {
    int numberOfInserts = 5;
    int numberOfDeletes = 3;

    Timestamp startTimestamp = Timestamp(0, 1);
    Timestamp midTimestamp = Timestamp(0, numberOfInserts);
    Timestamp finalTimestamp = Timestamp(0, numberOfInserts + numberOfDeletes);

    long long insertValue = 5;
    long long deleteValue = -3;
    DerivedMetadataDelta insertDelta = {insertValue, 0, startTimestamp};
    DerivedMetadataDelta deleteDelta = {deleteValue, 0, startTimestamp};

    const std::vector<DerivedMetadataDelta> insertDeltas =
        makeDeltaCopiesWithIncreasingTimestamps(insertDelta, startTimestamp, numberOfInserts);
    const std::vector<DerivedMetadataDelta> deleteDeltas =
        makeDeltaCopiesWithIncreasingTimestamps(deleteDelta, midTimestamp + 1, numberOfDeletes);

    // Apply a batch of writes, followed by a batch of deletes and verify the count and Timestamp.
    {
        DerivedMetadataCount dmdCount;
        dmdCount.applyDeltas(insertDeltas);

        auto latestValue = dmdCount.getLatestValue();
        long long expectedCount = numberOfInserts * insertValue;
        ASSERT_EQUALS(latestValue.first, midTimestamp);
        ASSERT_EQUALS(latestValue.second, expectedCount);

        dmdCount.applyDeltas(deleteDeltas);
        latestValue = dmdCount.getLatestValue();
        expectedCount += (3 * deleteValue);
        ASSERT_EQUALS(latestValue.first, finalTimestamp);
        ASSERT_EQUALS(latestValue.second, expectedCount);
    }

    std::vector<DerivedMetadataDelta> mixDeltas =
        makeDeltaCopiesWithIncreasingTimestamps(insertDelta, startTimestamp, numberOfInserts);
    std::vector<DerivedMetadataDelta> tempDeltas = makeDeltaCopiesWithIncreasingTimestamps(
        deleteDelta, startTimestamp + numberOfInserts, numberOfDeletes);
    mixDeltas.insert(mixDeltas.end(), tempDeltas.begin(), tempDeltas.end());

    // Apply a batch of writes and deletes, check the latest value and Timestamp, and ensure we do
    // not apply Deltas with Timestamps lower than the latest Timestamp.
    {
        DerivedMetadataCount dmdCount;
        dmdCount.applyDeltas(mixDeltas);

        auto latestValue = dmdCount.getLatestValue();
        long long expectedCount = numberOfInserts * insertValue + numberOfDeletes * deleteValue;
        ASSERT_EQUALS(latestValue.first, finalTimestamp);
        ASSERT_EQUALS(latestValue.second, expectedCount);

        // Verify that Deltas with Timestamps lower than the latest Timestamp are not applied.
        dmdCount.applyDeltas(insertDeltas);
        latestValue = dmdCount.getLatestValue();
        ASSERT_EQUALS(latestValue.first, finalTimestamp);
        ASSERT_EQUALS(latestValue.second, expectedCount);
    }
}

TEST(DerivedMetadataTest, RegisterDeltas) {
    DerivedMetadataCollectionController collectionController;
    DerivedMetadataDelta insertDelta = {1, 0, Timestamp(0)};

    Timestamp startTimestamp = Timestamp(0, 1);
    Timestamp ts3 = Timestamp(0, 3);
    Timestamp ts4 = Timestamp(0, 4);
    Timestamp ts10 = Timestamp(0, 10);

    {
        // Populate the Delta queue of the collectionController.
        bulkInsertDeltaCopiesWithIncreasingTimestamp(
            collectionController, insertDelta, startTimestamp, 3 /* numCopies */);
        ASSERT(collectionController.hasDeltas());

        // Fetch all Deltas from the Delta queue.
        std::vector<DerivedMetadataDelta> deltaVector = collectionController.fetchDeltasUpTo(ts3);
        ASSERT(deltaVector.size() == 3);
        ASSERT(!collectionController.hasDeltas());
    }

    {
        // Populate the Delta queue of the collectionController.
        bulkInsertDeltaCopiesWithIncreasingTimestamp(
            collectionController, insertDelta, startTimestamp, 4);

        // Fetch up all Deltas with Timestamp less than or equal to ts3. There should be one Delta
        // remaining in the queue.
        std::vector<DerivedMetadataDelta> deltaVector = collectionController.fetchDeltasUpTo(ts3);
        ASSERT(deltaVector.size() == 3);
        ASSERT(collectionController.hasDeltas());

        // Clear out the queue.
        deltaVector = collectionController.fetchDeltasUpTo(ts4);
        ASSERT(deltaVector.size() == 1);
        ASSERT(!collectionController.hasDeltas());
    }

    {
        // Populate the Delta queue of the collectionController.
        bulkInsertDeltaCopiesWithIncreasingTimestamp(
            collectionController, insertDelta, startTimestamp, 10);

        // Fetch up all Deltas with Timestamp less than or equal to ts4. There should be 6 Deltas
        // remaining in the queue.
        std::vector<DerivedMetadataDelta> deltaVector = collectionController.fetchDeltasUpTo(ts4);
        ASSERT(deltaVector.size() == 4);
        ASSERT(collectionController.hasDeltas());

        // Clear out the queue.
        deltaVector = collectionController.fetchDeltasUpTo(ts10);
        ASSERT(deltaVector.size() == 6);
        ASSERT(!collectionController.hasDeltas());
    }
}
}  // namespace mongo
