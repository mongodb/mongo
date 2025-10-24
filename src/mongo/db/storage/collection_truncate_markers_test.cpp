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

#include <fmt/format.h>
#include <string>
#include <vector>

#include <boost/optional/optional.hpp>

#include "mongo/base/status_with.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/client.h"
#include "mongo/db/concurrency/lock_manager_defs.h"
#include "mongo/db/curop.h"
#include "mongo/db/query/query_knobs_gen.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/db/storage/collection_truncate_markers.h"
#include "mongo/db/storage/storage_engine_test_fixture.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/idl/server_parameter_test_util.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/framework.h"
#include "mongo/util/assert_util_core.h"

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
            auto recordIdStatus = coll.getCollection()->getRecordStore()->insertRecord(
                opCtx, insertedData.data(), insertedData.length(), ts);
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
        auto recordIdStatus = coll.getCollection()->getRecordStore()->insertRecord(
            opCtx, recordId, insertedData.data(), insertedData.length(), timestampToUse);
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
        AutoGetCollection coll(opCtx, nss, MODE_IX);
        const auto correctedSize = dataLength - BSON("x" << "").objsize();
        invariant(correctedSize >= 0);
        const auto objToInsert = BSON("x" << std::string(correctedSize, 'a'));
        WriteUnitOfWork wuow(opCtx);
        for (int i = 0; i < numElements; i++) {
            auto recordIdStatus = coll.getCollection()->getRecordStore()->insertRecord(
                opCtx, objToInsert.objdata(), objToInsert.objsize(), timestampToUse);
            ASSERT_OK(recordIdStatus);
        }
        wuow.commit();
    }
};
class TestCollectionMarkersWithPartialExpiration final
    : public CollectionTruncateMarkersWithPartialExpiration {
public:
    TestCollectionMarkersWithPartialExpiration(int64_t leftoverRecordsCount,
                                               int64_t leftoverRecordsBytes,
                                               int64_t minBytesPerMarker)
        : CollectionTruncateMarkersWithPartialExpiration(
              {}, leftoverRecordsCount, leftoverRecordsBytes, minBytesPerMarker) {};

    TestCollectionMarkersWithPartialExpiration(std::deque<Marker> markers,
                                               int64_t leftoverRecordsCount,
                                               int64_t leftoverRecordsBytes,
                                               int64_t minBytesPerMarker)
        : CollectionTruncateMarkersWithPartialExpiration(
              std::move(markers), leftoverRecordsCount, leftoverRecordsBytes, minBytesPerMarker) {};

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
    TestCollectionMarkers(int64_t leftoverRecordsCount,
                          int64_t leftoverRecordsBytes,
                          int64_t minBytesPerMarker)
        : CollectionTruncateMarkers(
              {}, leftoverRecordsCount, leftoverRecordsBytes, minBytesPerMarker) {};

    TestCollectionMarkers(std::deque<Marker> markers,
                          int64_t leftoverRecordsCount,
                          int64_t leftoverRecordsBytes,
                          int64_t minBytesPerMarker)
        : CollectionTruncateMarkers(
              std::move(markers), leftoverRecordsCount, leftoverRecordsBytes, minBytesPerMarker) {};

private:
    bool _hasExcessMarkers(OperationContext* opCtx) const override {
        return !getMarkers().empty();
    }
};

template <typename T>
void normalTest(CollectionMarkersTest* fixture, std::string collectionName) {
    auto testMarkers = std::make_shared<T>(0, 0, 0);

    auto opCtx = fixture->getClient()->makeOperationContext();

    auto collNs = NamespaceString::createNamespaceString_forTest("test", collectionName);
    ASSERT_OK(fixture->createCollection(opCtx.get(), collNs));

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
    auto testMarkers = std::make_shared<TestCollectionMarkersWithPartialExpiration>(0, 0, 100);

    auto opCtx = getClient()->makeOperationContext();

    auto collNs = NamespaceString::createNamespaceString_forTest("test", "coll");
    ASSERT_OK(createCollection(opCtx.get(), collNs));

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
    auto testMarkers = std::make_shared<T>(0, 0, 100);

    auto collNs = NamespaceString::createNamespaceString_forTest("test", collectionName);
    {
        auto opCtx = fixture->getClient()->makeOperationContext();
        ASSERT_OK(fixture->createCollection(opCtx.get(), collNs));
    }

    {
        auto opCtx = fixture->getClient()->makeOperationContext();

        ASSERT_EQ(0U, testMarkers->numMarkers_forTest());

        // Inserting a record smaller than 'minBytesPerMarker' shouldn't create a new collection
        // marker.
        auto insertedRecordId = fixture->insertWithSpecificTimestampAndRecordId(
            opCtx.get(), collNs, *testMarkers, 99, Timestamp(1, 1), RecordId(1, 1));
        ASSERT_EQ(insertedRecordId, RecordId(1, 1));
        ASSERT_EQ(0U, testMarkers->numMarkers_forTest());
        ASSERT_EQ(1, testMarkers->currentRecords_forTest());
        ASSERT_EQ(99, testMarkers->currentBytes_forTest());

        // Inserting another record such that their combined size exceeds 'minBytesPerMarker' should
        // cause a new marker to be created.
        insertedRecordId = fixture->insertWithSpecificTimestampAndRecordId(
            opCtx.get(), collNs, *testMarkers, 51, Timestamp(1, 2), RecordId(1, 2));
        ASSERT_EQ(insertedRecordId, RecordId(1, 2));
        ASSERT_EQ(1U, testMarkers->numMarkers_forTest());
        ASSERT_EQ(0, testMarkers->currentRecords_forTest());
        ASSERT_EQ(0, testMarkers->currentBytes_forTest());

        // Inserting a record such that the combined size of this record and the previously inserted
        // one exceed 'minBytesPerMarker' shouldn't cause a new marker to be created because we've
        // started filling a new marker.
        insertedRecordId = fixture->insertWithSpecificTimestampAndRecordId(
            opCtx.get(), collNs, *testMarkers, 50, Timestamp(1, 3), RecordId(1, 3));
        ASSERT_EQ(insertedRecordId, RecordId(1, 3));
        ASSERT_EQ(1U, testMarkers->numMarkers_forTest());
        ASSERT_EQ(1, testMarkers->currentRecords_forTest());
        ASSERT_EQ(50, testMarkers->currentBytes_forTest());

        // Inserting a record such that the combined size of this record and the previously inserted
        // one is exactly equal to 'minBytesPerMarker' should cause a new marker to be created.
        insertedRecordId = fixture->insertWithSpecificTimestampAndRecordId(
            opCtx.get(), collNs, *testMarkers, 50, Timestamp(1, 4), RecordId(1, 4));
        ASSERT_EQ(insertedRecordId, RecordId(1, 4));
        ASSERT_EQ(2U, testMarkers->numMarkers_forTest());
        ASSERT_EQ(0, testMarkers->currentRecords_forTest());
        ASSERT_EQ(0, testMarkers->currentBytes_forTest());

        // Inserting a single record that exceeds 'minBytesPerMarker' should cause a new marker to
        // be created.
        insertedRecordId = fixture->insertWithSpecificTimestampAndRecordId(
            opCtx.get(), collNs, *testMarkers, 101, Timestamp(1, 5), RecordId(1, 5));
        ASSERT_EQ(insertedRecordId, RecordId(1, 5));
        ASSERT_EQ(3U, testMarkers->numMarkers_forTest());
        ASSERT_EQ(0, testMarkers->currentRecords_forTest());
        ASSERT_EQ(0, testMarkers->currentBytes_forTest());
    }
}

TEST_F(CollectionMarkersTest, CreateNewMarker) {
    createNewMarkerTest<TestCollectionMarkers>(this, "coll");
    createNewMarkerTest<TestCollectionMarkersWithPartialExpiration>(this, "partial_coll");
}

// Verify that a collection marker isn't created if it would cause the logical representation of the
// records to not be in increasing order.
template <typename T>
void ascendingOrderTest(CollectionMarkersTest* fixture, std::string collectionName) {
    auto testMarkers = std::make_shared<T>(0, 0, 100);

    auto collNs = NamespaceString::createNamespaceString_forTest("test", collectionName);
    {
        auto opCtx = fixture->getClient()->makeOperationContext();
        ASSERT_OK(fixture->createCollection(opCtx.get(), collNs));
    }

    {
        auto opCtx = fixture->getClient()->makeOperationContext();

        ASSERT_EQ(0U, testMarkers->numMarkers_forTest());
        auto insertedRecordId = fixture->insertWithSpecificTimestampAndRecordId(
            opCtx.get(), collNs, *testMarkers, 50, Timestamp(2, 2), RecordId(2, 2));
        ASSERT_EQ(insertedRecordId, RecordId(2, 2));
        ASSERT_EQ(0U, testMarkers->numMarkers_forTest());
        ASSERT_EQ(1, testMarkers->currentRecords_forTest());
        ASSERT_EQ(50, testMarkers->currentBytes_forTest());

        // Inserting a record that has a smaller RecordId than the previously inserted record should
        // be able to create a new marker when no markers already exist.
        insertedRecordId = fixture->insertWithSpecificTimestampAndRecordId(
            opCtx.get(), collNs, *testMarkers, 50, Timestamp(2, 1), RecordId(2, 1));
        ASSERT_EQ(insertedRecordId, RecordId(2, 1));
        ASSERT_EQ(1U, testMarkers->numMarkers_forTest());
        ASSERT_EQ(0, testMarkers->currentRecords_forTest());
        ASSERT_EQ(0, testMarkers->currentBytes_forTest());

        // However, inserting a record that has a smaller RecordId than most recently created
        // marker's last record shouldn't cause a new marker to be created, even if the size of the
        // inserted record exceeds 'minBytesPerMarker'.
        insertedRecordId = fixture->insertWithSpecificTimestampAndRecordId(
            opCtx.get(), collNs, *testMarkers, 100, Timestamp(1, 1), RecordId(1, 1));
        ASSERT_EQ(insertedRecordId, RecordId(1, 1));
        ASSERT_EQ(1U, testMarkers->numMarkers_forTest());
        ASSERT_EQ(1, testMarkers->currentRecords_forTest());
        ASSERT_EQ(100, testMarkers->currentBytes_forTest());

        // Inserting a record that has a larger RecordId than the most recently created marker's
        // last record should then cause a new marker to be created.
        insertedRecordId = fixture->insertWithSpecificTimestampAndRecordId(
            opCtx.get(), collNs, *testMarkers, 50, Timestamp(2, 3), RecordId(2, 3));
        ASSERT_EQ(insertedRecordId, RecordId(2, 3));
        ASSERT_EQ(2U, testMarkers->numMarkers_forTest());
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
        ASSERT_OK(createCollection(opCtx.get(), collNs));
        insertElements(opCtx.get(), collNs, kElementSize, kNumElements, Timestamp(1, 0));
    }

    {
        auto opCtx = getClient()->makeOperationContext();

        AutoGetCollection coll(opCtx.get(), collNs, MODE_IS);

        UnyieldableCollectionIterator iterator(opCtx.get(), coll->getRecordStore());

        auto result = CollectionTruncateMarkers::createMarkersByScanning(
            opCtx.get(), iterator, collNs, kMinBytes, [](const Record& record) {
                return CollectionTruncateMarkers::RecordIdAndWallTime{record.id, Date_t::now()};
            });
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
    static constexpr auto kNumRounds = 200;
    static constexpr auto kElementSize = 15;

    int totalBytes = 0;
    int totalRecords = 0;
    auto collNs = NamespaceString::createNamespaceString_forTest("test", "coll");
    {
        auto opCtx = getClient()->makeOperationContext();
        ASSERT_OK(createCollection(opCtx.get(), collNs));
        // Add documents of various sizes
        for (int round = 0; round < kNumRounds; round++) {
            for (int numBytes = kElementSize; numBytes < kElementSize * 2; numBytes++) {
                insertElements(opCtx.get(), collNs, numBytes, 1, Timestamp(1, 0));
                totalRecords++;
                totalBytes += numBytes;
            }
        }
    }

    {
        auto opCtx = getClient()->makeOperationContext();

        AutoGetCollection coll(opCtx.get(), collNs, MODE_IS);

        static constexpr auto kNumMarkers = 15;
        auto kMinBytesPerMarker = totalBytes / kNumMarkers;
        auto kRecordsPerMarker = totalRecords / kNumMarkers;

        UnyieldableCollectionIterator iterator(opCtx.get(), coll->getRecordStore());

        auto result = CollectionTruncateMarkers::createFromCollectionIterator(
            opCtx.get(), iterator, collNs, kMinBytesPerMarker, [](const Record& record) {
                return CollectionTruncateMarkers::RecordIdAndWallTime{record.id, Date_t::now()};
            });

        ASSERT_EQ(result.methodUsed, CollectionTruncateMarkers::MarkersCreationMethod::Sampling);
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
}

// Tests that auto yielding with query plan iterators works
TEST_F(CollectionMarkersTest, ScanningAutoYieldingWorks) {
    // Manually set the yielding parameters to make the yield count computation simpler.
    RAIIServerParameterControllerForTest queryYieldMs("internalQueryExecYieldPeriodMS",
                                                      1'000 * 3'600);
    RAIIServerParameterControllerForTest queryYieldDocsRead("internalQueryExecYieldIterations",
                                                            1'000);

    static constexpr auto kNumElements = 5001;
    static constexpr auto kElementSize = 15;
    static constexpr auto kMinBytes = (kElementSize * 2) - 1;

    auto collNs = NamespaceString::createNamespaceString_forTest("test", "coll");
    {
        auto opCtx = getClient()->makeOperationContext();
        ASSERT_OK(createCollection(opCtx.get(), collNs));
        insertElements(opCtx.get(), collNs, kElementSize, kNumElements, Timestamp(1, 0));
    }

    {
        auto opCtx = getClient()->makeOperationContext();

        AutoGetCollection coll(opCtx.get(), collNs, MODE_IS);

        YieldableCollectionIterator iterator(opCtx.get(), &coll.getCollection());

        auto result = CollectionTruncateMarkers::createMarkersByScanning(
            opCtx.get(), iterator, collNs, kMinBytes, [](const Record& record) {
                return CollectionTruncateMarkers::RecordIdAndWallTime{record.id, Date_t::now()};
            });
        ASSERT_EQ(result.leftoverRecordsBytes, kElementSize);
        ASSERT_EQ(result.leftoverRecordsCount, 1);
        ASSERT_EQ(result.markers.size(), kNumElements / 2);
        for (const auto& marker : result.markers) {
            ASSERT_EQ(marker.bytes, kElementSize * 2);
            ASSERT_EQ(marker.records, 2);
        }

        ASSERT_EQ(CurOp::get(opCtx.get())->numYields(),
                  kNumElements / internalQueryExecYieldIterations.load());
    }
}

// Tests that auto yielding with query plan iterators works
TEST_F(CollectionMarkersTest, SamplingAutoYieldingWorks) {
    // Manually set the yielding parameters to make the yield count computation simpler.
    RAIIServerParameterControllerForTest queryYieldMs("internalQueryExecYieldPeriodMS",
                                                      1'000 * 3'600);
    RAIIServerParameterControllerForTest queryYieldDocsRead("internalQueryExecYieldIterations",
                                                            1'000);

    static constexpr auto kNumRounds = 5000;
    static constexpr auto kElementSize = 15;
    static constexpr auto kNumElements = kElementSize * kNumRounds;
    static constexpr auto kNumElementsToSample =
        kNumElements / 20;  // We only sample 5% of the collection.

    int totalBytes = 0;
    int totalRecords = 0;
    auto collNs = NamespaceString::createNamespaceString_forTest("test", "coll");
    {
        auto opCtx = getClient()->makeOperationContext();
        ASSERT_OK(createCollection(opCtx.get(), collNs));
        // Add documents of various sizes
        for (int numBytes = kElementSize; numBytes < kElementSize * 2; numBytes++) {
            insertElements(opCtx.get(), collNs, numBytes, kNumRounds, Timestamp(1, 0));
            totalRecords += kNumRounds;
            totalBytes += numBytes * kNumRounds;
        }
    }

    ASSERT_EQ(totalRecords, kNumElements);

    {
        auto opCtx = getClient()->makeOperationContext();

        AutoGetCollection coll(opCtx.get(), collNs, MODE_IS);

        static constexpr auto kNumMarkers = 300;
        auto kMinBytesPerMarker = totalBytes / kNumMarkers;
        auto kRecordsPerMarker = totalRecords / kNumMarkers;

        YieldableCollectionIterator iterator(opCtx.get(), &coll.getCollection());

        auto result = CollectionTruncateMarkers::createFromCollectionIterator(
            opCtx.get(), iterator, collNs, kMinBytesPerMarker, [](const Record& record) {
                return CollectionTruncateMarkers::RecordIdAndWallTime{record.id, Date_t::now()};
            });

        ASSERT_EQ(result.methodUsed, CollectionTruncateMarkers::MarkersCreationMethod::Sampling);
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

        ASSERT_EQ(CurOp::get(opCtx.get())->numYields(),
                  kNumElementsToSample / internalQueryExecYieldIterations.load());
    }
}

// Test that cursors will yield periodically, and (since the yield drops the snapshot) that the
// cursor doesn't "see" new records.
TEST_F(CollectionMarkersTest, CursorYieldsAndIgnoresNewRecords) {
    static constexpr auto kNumElements = 100;
    static constexpr auto kElementSize = 100;

    TickSourceMock mockTickSource;
    auto collNs = NamespaceString::createNamespaceString_forTest("test", "coll");
    {
        auto opCtx = getClient()->makeOperationContext();
        ASSERT_OK(createCollection(opCtx.get(), collNs));
        insertElements(opCtx.get(), collNs, kElementSize, kNumElements, Timestamp(1, 0));
    }

    auto opCtx = getClient()->makeOperationContext();
    AutoGetCollection coll(opCtx.get(), collNs, MODE_IS);
    YieldableOplogIterator iterator(
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
        if (iterator.getNext()) {
            ++seenRecords;
        }
    }
    yieldNotifier.join();

    while (iterator.getNext()) {
        ++seenRecords;
    }

    ASSERT_EQ(seenRecords, kNumElements);
    ASSERT_LT(static_cast<long long>(seenRecords), coll->getRecordStore()->numRecords(opCtx.get()));
}

}  // namespace mongo
