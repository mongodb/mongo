/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#include "mongo/db/catalog/catalog_test_fixture.h"
#include "mongo/db/catalog/collection_write_path.h"
#include "mongo/db/catalog/index_catalog.h"
#include "mongo/db/catalog/index_catalog_entry.h"
#include "mongo/db/catalog/throttle_cursor.h"
#include "mongo/db/catalog/validate_gen.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/index/index_access_method.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/clock_source_mock.h"
#include "mongo/util/time_support.h"

namespace mongo {

namespace {

const NamespaceString kNss = NamespaceString("test.throttleCursor");
const uint8_t kTickDelay = 200;

class ThrottleCursorTest : public CatalogTestFixture {
private:
    void setUp() override;
    void tearDown() override;

protected:
    const KeyString::Value kMinKeyString = KeyString::Builder{KeyString::Version::kLatestVersion,
                                                              kMinBSONKey,
                                                              KeyString::ALL_ASCENDING}
                                               .getValueCopy();

    explicit ThrottleCursorTest(Milliseconds clockIncrement = Milliseconds{kTickDelay})
        : CatalogTestFixture(Options{}.useMockClock(true, clockIncrement)) {}

public:
    void setMaxMbPerSec(int maxMbPerSec);

    Date_t getTime();
    SortedDataInterfaceThrottleCursor getIdIndex(const CollectionPtr& coll);

    std::unique_ptr<DataThrottle> _dataThrottle;
};

class ThrottleCursorTestFastClock : public ThrottleCursorTest {
protected:
    // Move the clock faster to speed up the test.
    ThrottleCursorTestFastClock() : ThrottleCursorTest(Milliseconds{1000}) {}
};

void ThrottleCursorTest::setUp() {
    CatalogTestFixture::setUp();
    CollectionOptions defaultCollectionOptions;
    ASSERT_OK(
        storageInterface()->createCollection(operationContext(), kNss, defaultCollectionOptions));

    // Insert random data into the collection. We don't need to create an index as the _id index is
    // created by default.
    AutoGetCollection collection(operationContext(), kNss, MODE_X);
    invariant(collection);

    OpDebug* const nullOpDebug = nullptr;
    for (int i = 0; i < 10; i++) {
        WriteUnitOfWork wuow(operationContext());

        ASSERT_OK(collection_internal::insertDocument(
            operationContext(), *collection, InsertStatement(BSON("_id" << i)), nullOpDebug));
        wuow.commit();
    }

    _dataThrottle = std::make_unique<DataThrottle>(operationContext());
}

void ThrottleCursorTest::tearDown() {
    CatalogTestFixture::tearDown();
}

void ThrottleCursorTest::setMaxMbPerSec(int maxMbPerSec) {
    gMaxValidateMBperSec.store(maxMbPerSec);
}

Date_t ThrottleCursorTest::getTime() {
    return operationContext()->getServiceContext()->getFastClockSource()->now();
}

SortedDataInterfaceThrottleCursor ThrottleCursorTest::getIdIndex(const CollectionPtr& coll) {
    const IndexDescriptor* idDesc = coll->getIndexCatalog()->findIdIndex(operationContext());
    const IndexCatalogEntry* idEntry = coll->getIndexCatalog()->getEntry(idDesc);
    auto iam = idEntry->accessMethod()->asSortedData();

    return SortedDataInterfaceThrottleCursor(operationContext(), iam, _dataThrottle.get());
}

TEST_F(ThrottleCursorTest, TestSeekableRecordThrottleCursorOff) {
    auto opCtx = operationContext();
    AutoGetCollection autoColl(opCtx, kNss, MODE_X);
    const CollectionPtr& coll = autoColl.getCollection();

    // Use a fixed record data size to simplify the timing calculations.
    FailPointEnableBlock failPoint("fixedCursorDataSizeOf512KBForDataThrottle");

    SeekableRecordThrottleCursor cursor =
        SeekableRecordThrottleCursor(opCtx, coll->getRecordStore(), _dataThrottle.get());

    // With the data throttle off, all operations should finish within a second.
    setMaxMbPerSec(0);
    Date_t start = getTime();

    int numRecords = 0;

    while (cursor.next(opCtx)) {
        numRecords++;
    }

    int64_t recordId = 1;
    while (cursor.seekExact(opCtx, RecordId(recordId))) {
        recordId++;
        numRecords++;
    }

    Date_t end = getTime();

    ASSERT_EQ(numRecords, 20);
    ASSERT_EQ(end - start, Milliseconds(kTickDelay * numRecords + kTickDelay));
}

TEST_F(ThrottleCursorTest, TestSeekableRecordThrottleCursorOn) {
    auto opCtx = operationContext();
    AutoGetCollection autoColl(opCtx, kNss, MODE_X);
    const CollectionPtr& coll = autoColl.getCollection();

    // Use a fixed record data size to simplify the timing calculations.
    FailPointEnableBlock failPoint("fixedCursorDataSizeOf512KBForDataThrottle");

    SeekableRecordThrottleCursor cursor =
        SeekableRecordThrottleCursor(opCtx, coll->getRecordStore(), _dataThrottle.get());

    // Using a throttle with a limit of 1MB per second, all operations should take at least 5
    // seconds to finish. We have 10 records, each of which is 0.5MB courtesy of the fail point, so
    // 2 records per second.
    {
        setMaxMbPerSec(1);
        Date_t start = getTime();

        ASSERT_TRUE(cursor.seekExact(opCtx, RecordId(1)));
        int numRecords = 1;

        while (cursor.next(opCtx)) {
            numRecords++;
        }

        Date_t end = getTime();

        ASSERT_EQ(numRecords, 10);
        ASSERT_GTE(end - start, Milliseconds(5000));
    }

    // Using a throttle with a limit of 5MB per second, all operations should take at least 1
    // second to finish. We have 10 records, each of which is 0.5MB courtesy of the fail point, so
    // 10 records per second.
    {
        setMaxMbPerSec(5);
        Date_t start = getTime();

        ASSERT_TRUE(cursor.seekExact(opCtx, RecordId(1)));
        int numRecords = 1;

        while (cursor.next(opCtx)) {
            numRecords++;
        }

        Date_t end = getTime();

        ASSERT_EQ(numRecords, 10);
        ASSERT_GTE(end - start, Milliseconds(1000));
    }
}

TEST_F(ThrottleCursorTestFastClock, TestSeekableRecordThrottleCursorOnLargeDocs1MBps) {
    auto opCtx = operationContext();
    AutoGetCollection autoColl(opCtx, kNss, MODE_X);
    const CollectionPtr& coll = autoColl.getCollection();

    // Use a fixed record data size to simplify the timing calculations.
    FailPointEnableBlock failPoint("fixedCursorDataSizeOf2MBForDataThrottle");

    SeekableRecordThrottleCursor cursor =
        SeekableRecordThrottleCursor(opCtx, coll->getRecordStore(), _dataThrottle.get());

    // Using a throttle with a limit of 1MB per second, all operations should take at least 10
    // seconds to finish. We scan 5 records, each of which is 2MB courtesy of the fail point, so
    // 1 record every 2 seconds.
    setMaxMbPerSec(1);
    Date_t start = getTime();

    // Seek to the first record, then iterate through 4 more.
    ASSERT_TRUE(cursor.seekExact(opCtx, RecordId(1)));
    int scanRecords = 4;

    while (scanRecords > 0 && cursor.next(opCtx)) {
        scanRecords--;
    }

    Date_t end = getTime();

    ASSERT_EQ(scanRecords, 0);
    ASSERT_GTE(end - start, Milliseconds(10 * 1000));
}

TEST_F(ThrottleCursorTest, TestSeekableRecordThrottleCursorOnLargeDocs5MBps) {
    auto opCtx = operationContext();
    AutoGetCollection autoColl(opCtx, kNss, MODE_X);
    const CollectionPtr& coll = autoColl.getCollection();

    // Use a fixed record data size to simplify the timing calculations.
    FailPointEnableBlock failPoint("fixedCursorDataSizeOf2MBForDataThrottle");

    SeekableRecordThrottleCursor cursor =
        SeekableRecordThrottleCursor(opCtx, coll->getRecordStore(), _dataThrottle.get());

    // Using a throttle with a limit of 5MB per second, all operations should take at least 2
    // second to finish. We scan 5 records, each of which is 2MB courtesy of the fail point, so
    // 2.5 records per second.
    setMaxMbPerSec(5);
    Date_t start = getTime();

    // Seek to the first record, then iterate through 4 more.
    ASSERT_TRUE(cursor.seekExact(opCtx, RecordId(1)));
    int scanRecords = 4;

    while (scanRecords > 0 && cursor.next(opCtx)) {
        scanRecords--;
    }

    Date_t end = getTime();

    ASSERT_EQ(scanRecords, 0);
    ASSERT_GTE(end - start, Milliseconds(2000));
}

TEST_F(ThrottleCursorTest, TestSortedDataInterfaceThrottleCursorOff) {
    auto opCtx = operationContext();
    AutoGetCollection autoColl(opCtx, kNss, MODE_X);
    const CollectionPtr& coll = autoColl.getCollection();

    // Use a fixed record data size to simplify the timing calculations.
    FailPointEnableBlock failPoint("fixedCursorDataSizeOf512KBForDataThrottle");

    SortedDataInterfaceThrottleCursor cursor = getIdIndex(coll);

    // With the data throttle off, all operations should finish within a second.
    setMaxMbPerSec(0);
    Date_t start = getTime();

    ASSERT_TRUE(cursor.seek(opCtx, kMinKeyString));
    int numRecords = 1;

    while (cursor.next(opCtx)) {
        numRecords++;
    }

    Date_t end = getTime();

    ASSERT_EQ(numRecords, 10);
    ASSERT_EQ(end - start, Milliseconds(kTickDelay * numRecords + kTickDelay));
}

TEST_F(ThrottleCursorTest, TestSortedDataInterfaceThrottleCursorOn) {
    auto opCtx = operationContext();
    AutoGetCollection autoColl(opCtx, kNss, MODE_X);
    const CollectionPtr& coll = autoColl.getCollection();

    // Use a fixed record data size to simplify the timing calculations.
    FailPointEnableBlock failPoint("fixedCursorDataSizeOf512KBForDataThrottle");

    SortedDataInterfaceThrottleCursor cursor = getIdIndex(coll);

    // Using a throttle with a limit of 1MB per second, all operations should take at least 5
    // seconds to finish. We have 10 records, each of which is 0.5MB courtesy of the fail point, so
    // 2 records per second.
    {
        setMaxMbPerSec(1);
        Date_t start = getTime();

        ASSERT_TRUE(cursor.seek(opCtx, kMinKeyString));
        int numRecords = 1;

        while (cursor.next(opCtx)) {
            numRecords++;
        }

        Date_t end = getTime();

        ASSERT_EQ(numRecords, 10);
        ASSERT_GTE(end - start, Milliseconds(5000));
    }

    // Using a throttle with a limit of 5MB per second, all operations should take at least 1
    // second to finish. We have 10 records, each of which is 0.5MB courtesy of the fail point, so
    // 10 records per second.
    {
        setMaxMbPerSec(5);
        Date_t start = getTime();

        ASSERT_TRUE(cursor.seek(opCtx, kMinKeyString));
        int numRecords = 1;

        while (cursor.next(opCtx)) {
            numRecords++;
        }

        Date_t end = getTime();

        ASSERT_EQ(numRecords, 10);
        ASSERT_GTE(end - start, Milliseconds(1000));
    }
}

TEST_F(ThrottleCursorTest, TestMixedCursorsWithSharedThrottleOff) {
    auto opCtx = operationContext();
    AutoGetCollection autoColl(opCtx, kNss, MODE_X);
    const CollectionPtr& coll = autoColl.getCollection();

    // Use a fixed record data size to simplify the timing calculations.
    FailPointEnableBlock failPoint("fixedCursorDataSizeOf512KBForDataThrottle");

    SeekableRecordThrottleCursor recordCursor =
        SeekableRecordThrottleCursor(opCtx, coll->getRecordStore(), _dataThrottle.get());

    SortedDataInterfaceThrottleCursor indexCursor = getIdIndex(coll);

    // With the data throttle off, all operations should finish within a second, regardless if
    // the 'maxValidateMBperSec' server parameter is set.
    _dataThrottle->turnThrottlingOff();
    setMaxMbPerSec(10);
    Date_t start = getTime();

    ASSERT_TRUE(indexCursor.seek(opCtx, kMinKeyString));
    int numRecords = 1;

    while (indexCursor.next(opCtx)) {
        numRecords++;
    }

    while (recordCursor.next(opCtx)) {
        numRecords++;
    }

    int64_t recordId = 1;
    while (recordCursor.seekExact(opCtx, RecordId(recordId))) {
        recordId++;
        numRecords++;
    }

    Date_t end = getTime();

    ASSERT_EQ(numRecords, 30);
    ASSERT_EQ(end - start, Milliseconds(kTickDelay * numRecords + kTickDelay));
}

TEST_F(ThrottleCursorTest, TestMixedCursorsWithSharedThrottleOn) {
    auto opCtx = operationContext();
    AutoGetCollection autoColl(opCtx, kNss, MODE_X);
    const CollectionPtr& coll = autoColl.getCollection();

    // Use a fixed record data size to simplify the timing calculations.
    FailPointEnableBlock failPoint("fixedCursorDataSizeOf512KBForDataThrottle");

    SeekableRecordThrottleCursor recordCursor =
        SeekableRecordThrottleCursor(opCtx, coll->getRecordStore(), _dataThrottle.get());

    SortedDataInterfaceThrottleCursor indexCursor = getIdIndex(coll);

    // Using a throttle with a limit of 2MB per second, all operations should take at least 5
    // seconds to finish. We have 20 records, each of which is 0.5MB courtesy of the fail point, so
    // 4 records per second.
    {
        setMaxMbPerSec(2);
        Date_t start = getTime();

        ASSERT_TRUE(indexCursor.seek(opCtx, kMinKeyString));
        ASSERT_TRUE(recordCursor.seekExact(opCtx, RecordId(1)));
        int numRecords = 2;

        while (indexCursor.next(opCtx)) {
            ASSERT_TRUE(recordCursor.next(opCtx));
            numRecords += 2;
        }

        Date_t end = getTime();

        ASSERT_EQ(numRecords, 20);
        ASSERT_GTE(end - start, Milliseconds(5000));
    }

    // Using a throttle with a limit of 5MB per second, all operations should take at least 2
    // seconds to finish. We have 20 records, each of which is 0.5MB courtesy of the fail point, so
    // 10 records per second.
    {
        setMaxMbPerSec(5);
        Date_t start = getTime();

        ASSERT_TRUE(indexCursor.seek(opCtx, kMinKeyString));
        ASSERT_TRUE(recordCursor.seekExact(opCtx, RecordId(1)));
        int numRecords = 2;

        while (indexCursor.next(opCtx)) {
            ASSERT_TRUE(recordCursor.next(opCtx));
            numRecords += 2;
        }

        Date_t end = getTime();

        ASSERT_EQ(numRecords, 20);
        ASSERT_GTE(end - start, Milliseconds(2000));
    }
}

}  // namespace

}  // namespace mongo
