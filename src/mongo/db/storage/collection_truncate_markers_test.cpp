/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include "mongo/db/storage/collection_truncate_markers.h"

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/client.h"
#include "mongo/db/curop.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/db/shard_role/lock_manager/lock_manager_defs.h"
#include "mongo/db/shard_role/shard_catalog/catalog_raii.h"
#include "mongo/db/storage/storage_engine_test_fixture.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/idl/server_parameter_test_controller.h"
#include "mongo/platform/compiler.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

#include <string>
#include <vector>

#include <boost/optional/optional.hpp>
#include <fmt/format.h>

namespace mongo {

class CollectionMarkersTest : public StorageEngineTest {
public:
    explicit CollectionMarkersTest(Options options = {}) : StorageEngineTest(std::move(options)) {}

    struct RecordIdAndWall {
        RecordId recordId;
        Date_t wallTime;
    };
    std::vector<RecordIdAndWall> insertElementsWithCollectionMarkerUpdate(
        OperationContext* opCtx,
        const NamespaceString& nss,
        CollectionTruncateMarkers& testMarkers,
        int numElements,
        int dataLength) {
        std::vector<RecordIdAndWall> records;
        AutoGetCollection coll(opCtx, nss, MODE_IX);
        const auto insertedData = std::string(dataLength, 'a');
        WriteUnitOfWork wuow(opCtx);
        for (int i = 0; i < numElements; i++) {
            auto now = Date_t::now();
            auto ts = Timestamp(now);
            auto recordIdStatus =
                coll->getRecordStore()->insertRecord(opCtx,
                                                     *shard_role_details::getRecoveryUnit(opCtx),
                                                     insertedData.data(),
                                                     insertedData.length(),
                                                     ts);
            ASSERT_OK(recordIdStatus);
            auto recordId = recordIdStatus.getValue();
            testMarkers.updateCurrentMarkerAfterInsertOnCommit(opCtx,
                                                               insertedData.length(),
                                                               recordId,
                                                               now,
                                                               1,
                                                               /*oplogSamplingAsyncEnabled*/ false);
            records.push_back(RecordIdAndWall{std::move(recordId), std::move(now)});
        }
        wuow.commit();
        return records;
    }

    RecordIdAndWall insertElementWithCollectionMarkerUpdate(OperationContext* opCtx,
                                                            const NamespaceString& nss,
                                                            CollectionTruncateMarkers& testMarkers,
                                                            int dataLength) {
        auto records =
            insertElementsWithCollectionMarkerUpdate(opCtx, nss, testMarkers, 1, dataLength);
        return records.front();
    }

    RecordId insertWithSpecificTimestampAndRecordId(OperationContext* opCtx,
                                                    const NamespaceString& nss,
                                                    CollectionTruncateMarkers& testMarkers,
                                                    int dataLength,
                                                    Timestamp timestampToUse,
                                                    const RecordId& recordId) {
        AutoGetCollection coll(opCtx, nss, MODE_IX);
        const auto insertedData = std::string(dataLength, 'a');
        WriteUnitOfWork wuow(opCtx);
        auto recordIdStatus =
            coll->getRecordStore()->insertRecord(opCtx,
                                                 *shard_role_details::getRecoveryUnit(opCtx),
                                                 recordId,
                                                 insertedData.data(),
                                                 insertedData.length(),
                                                 timestampToUse);
        ASSERT_OK(recordIdStatus);
        ASSERT_EQ(recordIdStatus.getValue(), recordId);
        auto now = Date_t::fromMillisSinceEpoch(timestampToUse.asInt64());
        testMarkers.updateCurrentMarkerAfterInsertOnCommit(
            opCtx, insertedData.length(), recordId, now, 1, /*oplogSamplingAsyncEnabled*/ false);
        wuow.commit();
        return recordId;
    }

    void insertElements(OperationContext* opCtx,
                        const NamespaceString& nss,
                        int dataLength,
                        int numElements,
                        Timestamp timestampToUse) {
        AutoGetCollection coll(opCtx, nss, MODE_X);
        const auto correctedSize = dataLength - BSON("x" << "").objsize();
        invariant(correctedSize >= 0);
        const auto objToInsert = BSON("x" << std::string(correctedSize, 'a'));
        WriteUnitOfWork wuow(opCtx);
        for (int i = 0; i < numElements; i++) {
            auto recordIdStatus =
                coll->getRecordStore()->insertRecord(opCtx,
                                                     *shard_role_details::getRecoveryUnit(opCtx),
                                                     objToInsert.objdata(),
                                                     objToInsert.objsize(),
                                                     timestampToUse);
            ASSERT_OK(recordIdStatus);
        }
        wuow.commit();
    }

    // returns total bytes and total records in the collection
    std::tuple<size_t, size_t> createPopulatedCollection(const NamespaceString& collNs,
                                                         Timestamp timestamp = Timestamp(1, 0)) {
        static constexpr size_t kNumRounds = 200;
        static constexpr size_t kElementSize = 15;

        size_t totalBytes = 0;
        size_t totalRecords = 0;

        auto opCtx = getClient()->makeOperationContext();
        createCollection(opCtx.get(), collNs);
        // Add documents of various sizes
        for (size_t round = 0; round < kNumRounds; round++) {
            for (size_t numBytes = kElementSize; numBytes < kElementSize * 2; numBytes++) {
                insertElements(opCtx.get(), collNs, numBytes, 1, timestamp);
                totalRecords++;
                totalBytes += numBytes;
            }
        }
        ASSERT_EQ(totalBytes, 66'000);
        ASSERT_EQ(totalRecords, 3'000);
        return {totalBytes, totalRecords};
    }
};

class TestCollectionMarkersWithPartialExpiration final
    : public CollectionTruncateMarkersWithPartialExpiration {
public:
    TestCollectionMarkersWithPartialExpiration(int64_t minBytesPerMarker)
        : CollectionTruncateMarkersWithPartialExpiration(
              {},
              RecordId{},
              Date_t{},
              0 /* leftoverRecordsCount */,
              0 /* leftoverRecordsBytes */,
              minBytesPerMarker,
              Microseconds(0),
              CollectionTruncateMarkers::MarkersCreationMethod::EmptyCollection) {};

    void setExpirePartialMarker(bool value) {
        _expirePartialMarker = value;
    }

private:
    bool _expirePartialMarker = false;

    bool _hasExcessMarkers(OperationContext* opCtx) const override {
        return !getMarkers().empty();
    }

    bool _hasPartialMarkerExpired(OperationContext* opCtx,
                                  const RecordId& highestSeenRecordId,
                                  const Date_t& highestSeenWallTime) const override {
        return _expirePartialMarker;
    }
};

class TestCollectionMarkers final : public CollectionTruncateMarkers {
public:
    TestCollectionMarkers(int64_t minBytesPerMarker)
        : CollectionTruncateMarkers(
              {},
              0 /* leftoverRecordsCount */,
              0 /* leftoverRecordsBytes */,
              minBytesPerMarker,
              Microseconds(0),
              CollectionTruncateMarkers::MarkersCreationMethod::EmptyCollection) {};

private:
    bool _hasExcessMarkers(OperationContext* opCtx) const override {
        return !getMarkers().empty();
    }
};

template <typename T>
void normalTest(CollectionMarkersTest* fixture, std::string collectionName) {
    auto testMarkers = std::make_shared<T>(1);

    auto opCtx = fixture->getClient()->makeOperationContext();

    auto collNs = NamespaceString::createNamespaceString_forTest("test", collectionName);
    fixture->createCollection(opCtx.get(), collNs);

    static constexpr auto dataLength = 4;
    auto [insertedRecordId, now] = fixture->insertElementWithCollectionMarkerUpdate(
        opCtx.get(), collNs, *testMarkers, dataLength);

    auto marker = testMarkers->peekOldestMarkerIfNeeded(opCtx.get());
    ASSERT_TRUE(marker);
    ASSERT_EQ(marker->lastRecord, insertedRecordId);
    ASSERT_EQ(marker->bytes, dataLength);
    ASSERT_EQ(marker->wallTime, now);
    ASSERT_EQ(marker->records, 1);

    testMarkers->popOldestMarker();

    ASSERT_FALSE(testMarkers->peekOldestMarkerIfNeeded(opCtx.get()));
};

TEST_F(CollectionMarkersTest, NormalUsage) {
    normalTest<TestCollectionMarkers>(this, "coll");
    normalTest<TestCollectionMarkersWithPartialExpiration>(this, "partial_coll");
}

TEST_F(CollectionMarkersTest, NormalCollectionPartialMarkerUsage) {
    auto testMarkers = std::make_shared<TestCollectionMarkersWithPartialExpiration>(100);

    auto opCtx = getClient()->makeOperationContext();

    auto collNs = NamespaceString::createNamespaceString_forTest("test", "coll");
    createCollection(opCtx.get(), collNs);

    static constexpr auto dataLength = 4;
    auto [insertedRecordId, now] =
        insertElementWithCollectionMarkerUpdate(opCtx.get(), collNs, *testMarkers, dataLength);

    ASSERT_FALSE(testMarkers->peekOldestMarkerIfNeeded(opCtx.get()));

    testMarkers->setExpirePartialMarker(false);
    testMarkers->createPartialMarkerIfNecessary(opCtx.get());
    ASSERT_FALSE(testMarkers->peekOldestMarkerIfNeeded(opCtx.get()));

    testMarkers->setExpirePartialMarker(true);
    testMarkers->createPartialMarkerIfNecessary(opCtx.get());
    auto marker = testMarkers->peekOldestMarkerIfNeeded(opCtx.get());
    ASSERT_TRUE(marker);

    ASSERT_EQ(marker->lastRecord, insertedRecordId);
    ASSERT_EQ(marker->bytes, dataLength);
    ASSERT_EQ(marker->wallTime, now);
    ASSERT_EQ(marker->records, 1);
}

// Insert records into a collection and verify the number of markers that are created.
template <typename T>
void createNewMarkerTest(CollectionMarkersTest* fixture, std::string collectionName) {
    auto testMarkers = std::make_shared<T>(100);

    auto collNs = NamespaceString::createNamespaceString_forTest("test", collectionName);
    {
        auto opCtx = fixture->getClient()->makeOperationContext();
        fixture->createCollection(opCtx.get(), collNs);
    }

    {
        auto opCtx = fixture->getClient()->makeOperationContext();

        ASSERT_EQ(0U, testMarkers->numMarkers());

        // Inserting a record smaller than 'minBytesPerMarker' shouldn't create a new collection
        // marker.
        auto insertedRecordId = fixture->insertWithSpecificTimestampAndRecordId(
            opCtx.get(), collNs, *testMarkers, 99, Timestamp(1, 1), RecordId(1, 1));
        ASSERT_EQ(insertedRecordId, RecordId(1, 1));
        ASSERT_EQ(0U, testMarkers->numMarkers());
        ASSERT_EQ(1, testMarkers->currentRecords_forTest());
        ASSERT_EQ(99, testMarkers->currentBytes_forTest());

        // Inserting another record such that their combined size exceeds 'minBytesPerMarker' should
        // cause a new marker to be created.
        insertedRecordId = fixture->insertWithSpecificTimestampAndRecordId(
            opCtx.get(), collNs, *testMarkers, 51, Timestamp(1, 2), RecordId(1, 2));
        ASSERT_EQ(insertedRecordId, RecordId(1, 2));
        ASSERT_EQ(1U, testMarkers->numMarkers());
        ASSERT_EQ(0, testMarkers->currentRecords_forTest());
        ASSERT_EQ(0, testMarkers->currentBytes_forTest());

        // Inserting a record such that the combined size of this record and the previously inserted
        // one exceed 'minBytesPerMarker' shouldn't cause a new marker to be created because we've
        // started filling a new marker.
        insertedRecordId = fixture->insertWithSpecificTimestampAndRecordId(
            opCtx.get(), collNs, *testMarkers, 50, Timestamp(1, 3), RecordId(1, 3));
        ASSERT_EQ(insertedRecordId, RecordId(1, 3));
        ASSERT_EQ(1U, testMarkers->numMarkers());
        ASSERT_EQ(1, testMarkers->currentRecords_forTest());
        ASSERT_EQ(50, testMarkers->currentBytes_forTest());

        // Inserting a record such that the combined size of this record and the previously inserted
        // one is exactly equal to 'minBytesPerMarker' should cause a new marker to be created.
        insertedRecordId = fixture->insertWithSpecificTimestampAndRecordId(
            opCtx.get(), collNs, *testMarkers, 50, Timestamp(1, 4), RecordId(1, 4));
        ASSERT_EQ(insertedRecordId, RecordId(1, 4));
        ASSERT_EQ(2U, testMarkers->numMarkers());
        ASSERT_EQ(0, testMarkers->currentRecords_forTest());
        ASSERT_EQ(0, testMarkers->currentBytes_forTest());

        // Inserting a single record that exceeds 'minBytesPerMarker' should cause a new marker to
        // be created.
        insertedRecordId = fixture->insertWithSpecificTimestampAndRecordId(
            opCtx.get(), collNs, *testMarkers, 101, Timestamp(1, 5), RecordId(1, 5));
        ASSERT_EQ(insertedRecordId, RecordId(1, 5));
        ASSERT_EQ(3U, testMarkers->numMarkers());
        ASSERT_EQ(0, testMarkers->currentRecords_forTest());
        ASSERT_EQ(0, testMarkers->currentBytes_forTest());
    }
}

TEST_F(CollectionMarkersTest, CreateNewMarker) {
    createNewMarkerTest<TestCollectionMarkers>(this, "coll");
    createNewMarkerTest<TestCollectionMarkersWithPartialExpiration>(this, "partial_coll");
}

CollectionTruncateMarkers::RecordIdAndWallTime getIdAndWallTime(const Record& record) {
    return {record.id, Date_t::now()};
}

// Verify that a collection marker isn't created if it would cause the logical representation of the
// records to not be in increasing order.
template <typename T>
void ascendingOrderTest(CollectionMarkersTest* fixture, std::string collectionName) {
    auto testMarkers = std::make_shared<T>(100);

    auto collNs = NamespaceString::createNamespaceString_forTest("test", collectionName);
    {
        auto opCtx = fixture->getClient()->makeOperationContext();
        fixture->createCollection(opCtx.get(), collNs);
    }

    {
        auto opCtx = fixture->getClient()->makeOperationContext();

        ASSERT_EQ(0U, testMarkers->numMarkers());
        auto insertedRecordId = fixture->insertWithSpecificTimestampAndRecordId(
            opCtx.get(), collNs, *testMarkers, 50, Timestamp(2, 2), RecordId(2, 2));
        ASSERT_EQ(insertedRecordId, RecordId(2, 2));
        ASSERT_EQ(0U, testMarkers->numMarkers());
        ASSERT_EQ(1, testMarkers->currentRecords_forTest());
        ASSERT_EQ(50, testMarkers->currentBytes_forTest());

        // Inserting a record that has a smaller RecordId than the previously inserted record should
        // be able to create a new marker when no markers already exist.
        insertedRecordId = fixture->insertWithSpecificTimestampAndRecordId(
            opCtx.get(), collNs, *testMarkers, 50, Timestamp(2, 1), RecordId(2, 1));
        ASSERT_EQ(insertedRecordId, RecordId(2, 1));
        ASSERT_EQ(1U, testMarkers->numMarkers());
        ASSERT_EQ(0, testMarkers->currentRecords_forTest());
        ASSERT_EQ(0, testMarkers->currentBytes_forTest());

        // However, inserting a record that has a smaller RecordId than most recently created
        // marker's last record shouldn't cause a new marker to be created, even if the size of the
        // inserted record exceeds 'minBytesPerMarker'.
        insertedRecordId = fixture->insertWithSpecificTimestampAndRecordId(
            opCtx.get(), collNs, *testMarkers, 100, Timestamp(1, 1), RecordId(1, 1));
        ASSERT_EQ(insertedRecordId, RecordId(1, 1));
        ASSERT_EQ(1U, testMarkers->numMarkers());
        ASSERT_EQ(1, testMarkers->currentRecords_forTest());
        ASSERT_EQ(100, testMarkers->currentBytes_forTest());

        // Inserting a record that has a larger RecordId than the most recently created marker's
        // last record should then cause a new marker to be created.
        insertedRecordId = fixture->insertWithSpecificTimestampAndRecordId(
            opCtx.get(), collNs, *testMarkers, 50, Timestamp(2, 3), RecordId(2, 3));
        ASSERT_EQ(insertedRecordId, RecordId(2, 3));
        ASSERT_EQ(2U, testMarkers->numMarkers());
        ASSERT_EQ(0, testMarkers->currentRecords_forTest());
        ASSERT_EQ(0, testMarkers->currentBytes_forTest());
    }
}

TEST_F(CollectionMarkersTest, AscendingOrder) {
    ascendingOrderTest<TestCollectionMarkers>(this, "coll");
    ascendingOrderTest<TestCollectionMarkersWithPartialExpiration>(this, "partial_coll");
}

// Test that initial marker creation works as expected when performing a scanning marker creation.
TEST_F(CollectionMarkersTest, ScanningMarkerCreation) {
    static constexpr auto kNumElements = 51;
    static constexpr auto kElementSize = 15;
    static constexpr auto kMinBytes = (kElementSize * 2) - 1;

    auto collNs = NamespaceString::createNamespaceString_forTest("test", "coll");
    {
        auto opCtx = getClient()->makeOperationContext();
        createCollection(opCtx.get(), collNs);
        insertElements(opCtx.get(), collNs, kElementSize, kNumElements, Timestamp(1, 0));
    }

    {
        auto opCtx = getClient()->makeOperationContext();

        AutoGetCollection coll(opCtx.get(), collNs, MODE_IS);

        auto iterator = CollectionTruncateMarkers::makeIterator(
            opCtx.get(), coll->getRecordStore(), nullptr, boost::none);

        auto result = CollectionTruncateMarkers::createMarkersByScanning(
            opCtx.get(), *iterator, kMinBytes, getIdAndWallTime);
        ASSERT_EQ(result.methodUsed, CollectionTruncateMarkers::MarkersCreationMethod::Scanning);
        ASSERT_GTE(result.timeTaken, Microseconds(0));
        ASSERT_EQ(result.leftoverRecordsBytes, kElementSize);
        ASSERT_EQ(result.leftoverRecordsCount, 1);
        ASSERT_EQ(result.markers.size(), 51 / 2);
        for (const auto& marker : result.markers) {
            ASSERT_EQ(marker.bytes, kElementSize * 2);
            ASSERT_EQ(marker.records, 2);
        }
    }
}

// Test that initial marker creation works as expected when using sampling
TEST_F(CollectionMarkersTest, SamplingMarkerCreation) {
    auto collNs = NamespaceString::createNamespaceString_forTest("test", "coll");
    auto [totalBytes, totalRecords] = createPopulatedCollection(collNs);

    auto opCtx = getClient()->makeOperationContext();

    AutoGetCollection coll(opCtx.get(), collNs, MODE_IS);

    static constexpr auto kNumMarkers = 15;
    auto kMinBytesPerMarker = totalBytes / kNumMarkers;
    auto kRecordsPerMarker = totalRecords / kNumMarkers;

    auto iterator = CollectionTruncateMarkers::makeIterator(
        opCtx.get(), coll->getRecordStore(), nullptr, boost::none);

    auto result = CollectionTruncateMarkers::createFromCollectionIterator(
        opCtx.get(), *iterator, kMinBytesPerMarker, getIdAndWallTime);

    ASSERT_EQ(result.methodUsed, CollectionTruncateMarkers::MarkersCreationMethod::Sampling);
    ASSERT_GTE(result.timeTaken, Microseconds(0));
    const auto& firstMarker = result.markers.front();
    auto recordCount = firstMarker.records;
    auto recordBytes = firstMarker.bytes;
    ASSERT_EQ(result.leftoverRecordsBytes, totalBytes % kMinBytesPerMarker);
    ASSERT_EQ(result.leftoverRecordsCount, totalRecords % kRecordsPerMarker);
    ASSERT_GT(recordCount, 0);
    ASSERT_GT(recordBytes, 0);
    ASSERT_EQ(result.markers.size(), kNumMarkers);
    for (const auto& marker : result.markers) {
        ASSERT_EQ(marker.bytes, recordBytes);
        ASSERT_EQ(marker.records, recordCount);
    }

    ASSERT_EQ(recordBytes * kNumMarkers + result.leftoverRecordsBytes, totalBytes);
    ASSERT_EQ(recordCount * kNumMarkers + result.leftoverRecordsCount, totalRecords);
}

// Test that Oplog sampling progress is logged.
TEST_F(CollectionMarkersTest, OplogSamplingLogging) {
    auto collNs = NamespaceString::createNamespaceString_forTest("test", "coll");
    auto [totalBytes, _] = createPopulatedCollection(collNs);

    auto opCtx = getClient()->makeOperationContext();
    AutoGetCollection coll(opCtx.get(), collNs, MODE_IS);
    auto iterator = CollectionTruncateMarkers::makeIterator(
        opCtx.get(), coll->getRecordStore(), nullptr, boost::none);

    static constexpr auto kNumMarkers = 15;
    auto kMinBytesPerMarker = totalBytes / kNumMarkers;
    long long numRecords = iterator->numRecords();
    long long dataSize = iterator->dataSize();
    double avgRecordSize = double(dataSize) / double(numRecords);
    double estimatedRecordsPerMarker = std::ceil(kMinBytesPerMarker / avgRecordSize);
    double estimatedBytesPerMarker = estimatedRecordsPerMarker * avgRecordSize;

    TickSourceMock mockTickSource;
    mockTickSource.setAdvanceOnRead(Milliseconds{500});
    unittest::LogCaptureGuard logs;
    CollectionTruncateMarkers::createMarkersBySampling(opCtx.get(),
                                                       *iterator,
                                                       estimatedRecordsPerMarker,
                                                       estimatedBytesPerMarker,
                                                       getIdAndWallTime,
                                                       &mockTickSource);
    logs.stop();
    ASSERT_GT(logs.countTextContaining("Collection sampling progress"), 0);
    ASSERT_EQUALS(logs.countTextContaining("Collection sampling complete"), 1);
}

// Test that cursors will yield periodically, and (since the yield drops the snapshot) that the
// cursor doesn't "see" new records.
TEST_F(CollectionMarkersTest, CursorYieldsAndIgnoresNewRecords) {
    TickSourceMock mockTickSource;
    auto collNs = NamespaceString::createNamespaceString_forTest("test", "coll");
    auto [_, initialRecords] = createPopulatedCollection(collNs);

    auto opCtx = getClient()->makeOperationContext();
    AutoGetCollection coll(opCtx.get(), collNs, MODE_IS);
    auto iterator = CollectionTruncateMarkers::makeIterator(
        opCtx.get(), coll->getRecordStore(), &mockTickSource, Milliseconds(10));

    AtomicWord<bool> hasYielded(false);
    stdx::thread yieldNotifier([this, &collNs, &hasYielded] {
        ThreadClient client(getServiceContext()->getService());
        auto innerOpCtx = cc().makeOperationContext();
        // We won't be able to acquire the write lock until the read yields.
        insertElements(
            innerOpCtx.get(), collNs, /*dataLength=*/100, /*numElements=*/10, Timestamp(1, 0));
        hasYielded.store(true);
    });

    ASSERT_FALSE(hasYielded.load());

    size_t seenRecords = 0;
    while (!hasYielded.load()) {
        mockTickSource.advance(Milliseconds(11));
        // Have a real sleep so that the yield thread has a chance to catch up.
        opCtx->sleepFor(Milliseconds(1));
        if (iterator->getNext()) {
            ++seenRecords;
        }
    }
    yieldNotifier.join();

    while (iterator->getNext()) {
        ++seenRecords;
    }

    ASSERT_EQ(seenRecords, initialRecords);
    ASSERT_LT(static_cast<long long>(seenRecords), coll->getRecordStore()->numRecords());
}

// Test that random cursors will yield periodically. Since random cursors will see records added
// after they were created due to the yield, we only care that it can continue to sample.
TEST_F(CollectionMarkersTest, CursorYieldWithRandomCursor) {
    TickSourceMock mockTickSource;
    auto collNs = NamespaceString::createNamespaceString_forTest("test", "coll");
    createPopulatedCollection(collNs);

    auto opCtx = getClient()->makeOperationContext();
    AutoGetCollection coll(opCtx.get(), collNs, MODE_IS);
    auto iterator = CollectionTruncateMarkers::makeIterator(
        opCtx.get(), coll->getRecordStore(), &mockTickSource, Milliseconds(10));

    AtomicWord<bool> hasYielded(false);
    stdx::thread yieldNotifier([this, &collNs, &hasYielded] {
        ThreadClient client(getServiceContext()->getService());
        auto innerOpCtx = cc().makeOperationContext();
        // We won't be able to acquire the write lock until the read yields.
        insertElements(
            innerOpCtx.get(), collNs, /*dataLength=*/100, /*numElements=*/10, Timestamp(1, 0));
        hasYielded.store(true);
    });

    ASSERT_FALSE(hasYielded.load());

    while (!hasYielded.load()) {
        mockTickSource.advance(Milliseconds(11));
        // Have a real sleep so that the yield thread has a chance to catch up.
        opCtx->sleepFor(Milliseconds(1));
        ASSERT(iterator->getNextRandom());
    }
    yieldNotifier.join();

    for (int i = 0; i < 1000; i++) {
        ASSERT(iterator->getNextRandom());
    }
}

// Test that yielding handles the collection being truncated from underneath it.
TEST_F(CollectionMarkersTest, CursorYieldSerialCursorHandlesTruncate) {
    TickSourceMock mockTickSource;
    auto collNs = NamespaceString::createNamespaceString_forTest("test", "coll");
    auto [_, initialRecords] = createPopulatedCollection(collNs);

    auto opCtx = getClient()->makeOperationContext();
    AutoGetCollection coll(opCtx.get(), collNs, MODE_IS);
    auto iterator = CollectionTruncateMarkers::makeIterator(
        opCtx.get(), coll->getRecordStore(), &mockTickSource, Milliseconds(10));

    AtomicWord<bool> hasYielded(false);
    stdx::thread yieldNotifier([this, &collNs, &hasYielded] {
        ThreadClient client(getServiceContext()->getService());
        auto innerOpCtx = cc().makeOperationContext();
        auto opCtx = innerOpCtx.get();

        // We won't be able to acquire the write lock until the read yields.
        AutoGetCollection coll(opCtx, collNs, MODE_X);
        WriteUnitOfWork wuow(opCtx);
        ASSERT_OK(
            coll->getRecordStore()->truncate(opCtx, *shard_role_details::getRecoveryUnit(opCtx)));
        wuow.commit();
        hasYielded.store(true);
    });

    ASSERT_FALSE(hasYielded.load());

    size_t seenRecords = 0;
    while (!hasYielded.load()) {
        mockTickSource.advance(Milliseconds(11));
        // Have a real sleep so that the yield thread has a chance to catch up.
        opCtx->sleepFor(Milliseconds(1));
        if (iterator->getNext()) {
            ++seenRecords;
        }
    }
    yieldNotifier.join();

    while (iterator->getNext()) {
        ++seenRecords;
    }

    // Since we truncated we won't see later records
    ASSERT_LT(seenRecords, initialRecords);
}

// Test that yielding a random cursor handles the collection being truncated from underneath it.
TEST_F(CollectionMarkersTest, CursorYieldRandomCursorHandlesTruncate) {
    TickSourceMock mockTickSource;
    auto collNs = NamespaceString::createNamespaceString_forTest("test", "coll");
    createPopulatedCollection(collNs);

    auto opCtx = getClient()->makeOperationContext();
    AutoGetCollection coll(opCtx.get(), collNs, MODE_IS);
    auto iterator = CollectionTruncateMarkers::makeIterator(
        opCtx.get(), coll->getRecordStore(), &mockTickSource, Milliseconds(10));

    AtomicWord<bool> hasYielded(false);
    stdx::thread yieldNotifier([this, &collNs, &hasYielded] {
        ThreadClient client(getServiceContext()->getService());
        auto innerOpCtx = cc().makeOperationContext();
        auto opCtx = innerOpCtx.get();

        // We won't be able to acquire the write lock until the read yields.
        AutoGetCollection coll(opCtx, collNs, MODE_X);
        WriteUnitOfWork wuow(opCtx);
        ASSERT_OK(
            coll->getRecordStore()->truncate(opCtx, *shard_role_details::getRecoveryUnit(opCtx)));
        wuow.commit();
        hasYielded.store(true);
    });

    // When we truncate underneath the random cursor it will return nullopt.
    ASSERT_FALSE(hasYielded.load());
    while (iterator->getNextRandom()) {
        mockTickSource.advance(Milliseconds(11));
        // Have a real sleep so that the yield thread has a chance to catch up.
        opCtx->sleepFor(Milliseconds(1));
    }
    ASSERT_TRUE(hasYielded.load());
    yieldNotifier.join();
}

// Test that sampling handles the collection being truncated from underneath us.
TEST_F(CollectionMarkersTest, SamplingWorksWithTruncate) {
    TickSourceMock mockTickSource;
    auto collNs = NamespaceString::createNamespaceString_forTest("test", "coll");
    createPopulatedCollection(collNs);

    auto opCtx = getClient()->makeOperationContext();
    AutoGetCollection coll(opCtx.get(), collNs, MODE_IS);
    // 0ms yield interval means sample every next()
    auto iterator = CollectionTruncateMarkers::makeIterator(
        opCtx.get(), coll->getRecordStore(), &mockTickSource, {Milliseconds(0)});

    // Synchronize to avoid sampling the collection before the yield thread is ready.
    stdx::mutex waitingMutex;
    stdx::unique_lock waitingLock(waitingMutex);
    stdx::condition_variable waitingCv;

    AtomicWord<bool> hasYielded(false);
    stdx::thread yieldNotifier([this, &collNs, &hasYielded, &waitingCv, &waitingMutex] {
        ThreadClient client(getServiceContext()->getService());
        auto innerOpCtx = cc().makeOperationContext();
        auto opCtx = innerOpCtx.get();

        {
            stdx::unique_lock readyLock(waitingMutex);
        }
        waitingCv.notify_one();

        // We won't be able to acquire the write lock until the read yields.
        AutoGetCollection coll(opCtx, collNs, MODE_X);
        WriteUnitOfWork wuow(opCtx);
        ASSERT_OK(
            coll->getRecordStore()->truncate(opCtx, *shard_role_details::getRecoveryUnit(opCtx)));
        wuow.commit();
        hasYielded.store(true);
    });

    waitingCv.wait(waitingLock);
    opCtx->sleepFor(Milliseconds{1});

    // When we truncate underneath the random cursor it will return nullopt.
    ASSERT_FALSE(hasYielded.load());

    CollectionTruncateMarkers::createMarkersBySampling(opCtx.get(),
                                                       *iterator,
                                                       /*estimatedRecordsPerMarker=*/1,
                                                       /*estimatedBytesPerMarker=*/1,
                                                       getIdAndWallTime);

    ASSERT_TRUE(hasYielded.load());
    yieldNotifier.join();
}

// Test that scanning handles the collection being truncated from underneath us.
TEST_F(CollectionMarkersTest, ScanningWorksWithTruncate) {
    TickSourceMock mockTickSource;
    auto collNs = NamespaceString::createNamespaceString_forTest("test", "coll");
    createPopulatedCollection(collNs);

    auto opCtx = getClient()->makeOperationContext();
    AutoGetCollection coll(opCtx.get(), collNs, MODE_IS);
    // 0ms yield interval means sample every next()
    auto iterator = CollectionTruncateMarkers::makeIterator(
        opCtx.get(), coll->getRecordStore(), &mockTickSource, {Milliseconds(0)});

    // Synchronize to avoid scanning the collection before the yield thread is ready.
    stdx::mutex waitingMutex;
    stdx::unique_lock waitingLock(waitingMutex);
    stdx::condition_variable waitingCv;

    AtomicWord<bool> hasYielded(false);
    stdx::thread yieldNotifier([this, &collNs, &hasYielded, &waitingCv, &waitingMutex] {
        ThreadClient client(getServiceContext()->getService());
        auto innerOpCtx = cc().makeOperationContext();
        auto opCtx = innerOpCtx.get();

        {
            stdx::unique_lock readyLock(waitingMutex);
        }
        waitingCv.notify_one();

        // We won't be able to acquire the write lock until the read yields.
        AutoGetCollection coll(opCtx, collNs, MODE_X);
        WriteUnitOfWork wuow(opCtx);
        ASSERT_OK(
            coll->getRecordStore()->truncate(opCtx, *shard_role_details::getRecoveryUnit(opCtx)));
        wuow.commit();
        hasYielded.store(true);
    });

    waitingCv.wait(waitingLock);
    opCtx->sleepFor(Milliseconds{1});

    // When we truncate underneath the random cursor it will return nullopt.
    ASSERT_FALSE(hasYielded.load());

    CollectionTruncateMarkers::createMarkersByScanning(
        opCtx.get(), *iterator, /*estimatedBytesPerMarker=*/1, getIdAndWallTime);

    ASSERT_TRUE(hasYielded.load());
    yieldNotifier.join();
}

}  // namespace mongo
