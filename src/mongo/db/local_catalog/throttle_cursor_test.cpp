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


#include "mongo/db/local_catalog/throttle_cursor.h"

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/collection_crud/collection_write_path.h"
#include "mongo/db/curop.h"
#include "mongo/db/index/index_access_method.h"
#include "mongo/db/local_catalog/catalog_raii.h"
#include "mongo/db/local_catalog/catalog_test_fixture.h"
#include "mongo/db/local_catalog/collection.h"
#include "mongo/db/local_catalog/collection_options.h"
#include "mongo/db/local_catalog/index_catalog.h"
#include "mongo/db/local_catalog/index_catalog_entry.h"
#include "mongo/db/local_catalog/index_descriptor.h"
#include "mongo/db/local_catalog/lock_manager/lock_manager_defs.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/db/validate/validate_gen.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/duration.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/time_support.h"

#include <boost/optional/optional.hpp>

namespace mongo {

namespace {

const NamespaceString kNss = NamespaceString::createNamespaceString_forTest("test.throttleCursor");
const uint8_t kTickDelay = 200;
// Margin of error to be subtracted from test times to account for clock inconsistencies
const Milliseconds kErrorMargin = Milliseconds(100);

class ThrottleCursorTest : public CatalogTestFixture {
private:
    void setUp() override;
    void tearDown() override;

protected:
    const key_string::Value kMinKeyString = key_string::Builder{key_string::Version::kLatestVersion,
                                                                kMinBSONKey,
                                                                key_string::ALL_ASCENDING}
                                                .getValueCopy();

    explicit ThrottleCursorTest(Milliseconds clockIncrement = Milliseconds{kTickDelay})
        : CatalogTestFixture() {}

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

    _dataThrottle = std::make_unique<DataThrottle>(operationContext(),
                                                   [&]() { return gMaxValidateMBperSec.load(); });
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

    Date_t start = getTime();
    SeekableRecordThrottleCursor cursor =
        SeekableRecordThrottleCursor(opCtx, coll->getRecordStore(), _dataThrottle.get());

    // With the data throttle off, all operations should finish within a second.
    setMaxMbPerSec(0);

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
    ASSERT_LTE(end - start, Milliseconds(1000));
}

TEST_F(ThrottleCursorTest, TestSeekableRecordThrottleCursorOn) {
    auto opCtx = operationContext();
    AutoGetCollection autoColl(opCtx, kNss, MODE_X);
    const CollectionPtr& coll = autoColl.getCollection();

    // Use a fixed record data size to simplify the timing calculations.
    FailPointEnableBlock failPoint("fixedCursorDataSizeOf512KBForDataThrottle");

    // We have 10 records, each of which is 0.5MB courtesy of the fail point. Using a throttle with
    // a limit of 1MB per second, this will mean the data is processed as follows: 0s 1s 2s 5s |
    // 0.5MB 0.5MB (sleep) | 0.5MB 0.5MB (sleep) | ... | 0.5MB 0.5MB (sleep) | All operations should
    // take very close to 5 seconds to finish.
    {
        Date_t start = getTime();
        SeekableRecordThrottleCursor cursor =
            SeekableRecordThrottleCursor(opCtx, coll->getRecordStore(), _dataThrottle.get());
        setMaxMbPerSec(1);

        ASSERT_TRUE(cursor.seekExact(opCtx, RecordId(1)));
        int numRecords = 1;

        while (cursor.next(opCtx)) {
            numRecords++;
        }

        Date_t end = getTime();

        ASSERT_EQ(numRecords, 10);
        ASSERT_GTE(end - start, Milliseconds(5000) - kErrorMargin);
    }

    // Using a throttle with a limit of 5MB per second, all operations should take very close to 1
    // second (or more) to finish. We have 10 records, each of which is 0.5MB courtesy of the fail
    // point, so 10 records per second.
    {
        Date_t start = getTime();
        SeekableRecordThrottleCursor cursor =
            SeekableRecordThrottleCursor(opCtx, coll->getRecordStore(), _dataThrottle.get());
        setMaxMbPerSec(5);

        ASSERT_TRUE(cursor.seekExact(opCtx, RecordId(1)));
        int numRecords = 1;

        while (cursor.next(opCtx)) {
            numRecords++;
        }

        Date_t end = getTime();

        ASSERT_EQ(numRecords, 10);
        ASSERT_GTE(end - start, Milliseconds(1000) - kErrorMargin);
    }
}

TEST_F(ThrottleCursorTestFastClock, TestSeekableRecordThrottleCursorOnLargeDocs1MBps) {
    auto opCtx = operationContext();
    AutoGetCollection autoColl(opCtx, kNss, MODE_X);
    const CollectionPtr& coll = autoColl.getCollection();

    // Use a fixed record data size to simplify the timing calculations.
    FailPointEnableBlock failPoint("fixedCursorDataSizeOf2MBForDataThrottle");

    // Using a throttle with a limit of 1MB per second, all operations should take very close to 10
    // seconds (or more) to finish. We scan 5 records, each of which is 2MB courtesy of the fail
    // point, so 1 record every 2 seconds.
    Date_t start = getTime();
    SeekableRecordThrottleCursor cursor =
        SeekableRecordThrottleCursor(opCtx, coll->getRecordStore(), _dataThrottle.get());
    setMaxMbPerSec(1);


    // Seek to the first record, then iterate through 4 more.
    ASSERT_TRUE(cursor.seekExact(opCtx, RecordId(1)));
    int scanRecords = 4;

    while (scanRecords > 0 && cursor.next(opCtx)) {
        scanRecords--;
    }

    Date_t end = getTime();

    ASSERT_EQ(scanRecords, 0);
    ASSERT_GTE(end - start, Milliseconds(10 * 1000) - kErrorMargin);
}

TEST_F(ThrottleCursorTest, TestSeekableRecordThrottleCursorOnLargeDocs5MBps) {
    auto opCtx = operationContext();
    AutoGetCollection autoColl(opCtx, kNss, MODE_X);
    const CollectionPtr& coll = autoColl.getCollection();

    // Use a fixed record data size to simplify the timing calculations.
    FailPointEnableBlock failPoint("fixedCursorDataSizeOf2MBForDataThrottle");

    // We have 6 records, each of which is 2MB courtesy of the fail point. Using a throttle with a
    // limit of 5MB per second, this will mean the data is processed as follows:
    // 0s                   1.2s                  2.4s
    // | 2MB 2MB 2MB (sleep) | 2MB 2MB 2MB (sleep) |
    // All operations should take at very close to 2.4 seconds (or more) to finish.
    Date_t start = getTime();
    SeekableRecordThrottleCursor cursor =
        SeekableRecordThrottleCursor(opCtx, coll->getRecordStore(), _dataThrottle.get());

    setMaxMbPerSec(5);

    // Seek to the first record, then iterate through 5 more.
    ASSERT_TRUE(cursor.seekExact(opCtx, RecordId(1)));
    int scanRecords = 5;

    while (scanRecords > 0 && cursor.next(opCtx)) {
        scanRecords--;
    }

    Date_t end = getTime();

    ASSERT_EQ(scanRecords, 0);
    ASSERT_GTE(end - start, Milliseconds(2400) - kErrorMargin);
}

TEST_F(ThrottleCursorTest, TestSortedDataInterfaceThrottleCursorOff) {
    auto opCtx = operationContext();
    AutoGetCollection autoColl(opCtx, kNss, MODE_X);
    const CollectionPtr& coll = autoColl.getCollection();

    // Use a fixed record data size to simplify the timing calculations.
    FailPointEnableBlock failPoint("fixedCursorDataSizeOf512KBForDataThrottle");

    Date_t start = getTime();
    SortedDataInterfaceThrottleCursor cursor = getIdIndex(coll);

    // With the data throttle off, all operations should finish within a second.
    setMaxMbPerSec(0);

    int numRecords = 0;

    while (cursor.next(opCtx)) {
        numRecords++;
    }

    Date_t end = getTime();

    ASSERT_EQ(numRecords, 10);
    ASSERT_LTE(end - start, Milliseconds(1000));
}

TEST_F(ThrottleCursorTest, TestSortedDataInterfaceThrottleCursorOn) {
    auto opCtx = operationContext();
    AutoGetCollection autoColl(opCtx, kNss, MODE_X);
    const CollectionPtr& coll = autoColl.getCollection();

    // Use a fixed record data size to simplify the timing calculations.
    FailPointEnableBlock failPoint("fixedCursorDataSizeOf512KBForDataThrottle");

    // We have 10 records, each of which is 0.5MB courtesy of the fail point. Using a throttle with
    // a limit of 1MB per second, this will mean the data is processed as follows: 0s 1s 2s 5s |
    // 0.5MB 0.5MB (sleep) | 0.5MB 0.5MB (sleep) | ... | 0.5MB 0.5MB (sleep) | All operations should
    // take very close to 5 seconds to finish.
    {
        Date_t start = getTime();
        SortedDataInterfaceThrottleCursor cursor = getIdIndex(coll);
        setMaxMbPerSec(1);

        int numRecords = 0;

        while (cursor.next(opCtx)) {
            numRecords++;
        }

        Date_t end = getTime();

        ASSERT_EQ(numRecords, 10);
        ASSERT_GTE(end - start, Milliseconds(5000) - kErrorMargin);
    }

    // Using a throttle with a limit of 5MB per second, all operations should take very close to 1
    // second (or more) to finish. We have 10 records, each of which is 0.5MB courtesy of the fail
    // point, so 10 records per second.
    {
        Date_t start = getTime();
        SortedDataInterfaceThrottleCursor cursor = getIdIndex(coll);
        setMaxMbPerSec(5);

        ASSERT_TRUE(cursor.seek(opCtx, kMinKeyString.getView()));
        int numRecords = 1;

        while (cursor.next(opCtx)) {
            numRecords++;
        }

        Date_t end = getTime();

        ASSERT_EQ(numRecords, 10);
        ASSERT_GTE(end - start, Milliseconds(1000) - kErrorMargin);
    }
}

TEST_F(ThrottleCursorTest, TestMixedCursorsWithSharedThrottleOff) {
    auto opCtx = operationContext();
    AutoGetCollection autoColl(opCtx, kNss, MODE_X);
    const CollectionPtr& coll = autoColl.getCollection();

    // Use a fixed record data size to simplify the timing calculations.
    FailPointEnableBlock failPoint("fixedCursorDataSizeOf512KBForDataThrottle");

    Date_t start = getTime();
    SeekableRecordThrottleCursor recordCursor =
        SeekableRecordThrottleCursor(opCtx, coll->getRecordStore(), _dataThrottle.get());

    SortedDataInterfaceThrottleCursor indexCursor = getIdIndex(coll);

    // With the data throttle off, all operations should finish within a second, regardless if
    // the 'maxValidateMBperSec' server parameter is set.
    _dataThrottle->turnThrottlingOff();
    setMaxMbPerSec(10);

    int numRecords = 0;

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
    ASSERT_LTE(end - start, Milliseconds(1000));
}

TEST_F(ThrottleCursorTest, TestMixedCursorsWithSharedThrottleOn) {
    auto opCtx = operationContext();
    AutoGetCollection autoColl(opCtx, kNss, MODE_X);
    const CollectionPtr& coll = autoColl.getCollection();

    // Use a fixed record data size to simplify the timing calculations.
    FailPointEnableBlock failPoint("fixedCursorDataSizeOf512KBForDataThrottle");

    // Using a throttle with a limit of 2MB per second, all operations should take very close to 5
    // seconds (or more) to finish. We have 20 records, each of which is 0.5MB courtesy of the fail
    // point, so 4 records per second.
    {
        Date_t start = getTime();

        SeekableRecordThrottleCursor recordCursor =
            SeekableRecordThrottleCursor(opCtx, coll->getRecordStore(), _dataThrottle.get());

        SortedDataInterfaceThrottleCursor indexCursor = getIdIndex(coll);
        setMaxMbPerSec(2);

        int numRecords = 0;

        while (indexCursor.next(opCtx)) {
            if (numRecords == 0) {
                ASSERT_TRUE(recordCursor.seekExact(opCtx, RecordId(1)));
            } else {
                ASSERT_TRUE(recordCursor.next(opCtx));
            }
            numRecords += 2;
        }

        Date_t end = getTime();

        ASSERT_EQ(numRecords, 20);
        ASSERT_GTE(end - start, Milliseconds(5000) - kErrorMargin);
    }

    // Using a throttle with a limit of 5MB per second, all operations should take very close to 2
    // seconds (or more) to finish. We have 20 records, each of which is 0.5MB courtesy of the fail
    // point, so 10 records per second.
    {
        Date_t start = getTime();

        SeekableRecordThrottleCursor recordCursor =
            SeekableRecordThrottleCursor(opCtx, coll->getRecordStore(), _dataThrottle.get());

        SortedDataInterfaceThrottleCursor indexCursor = getIdIndex(coll);
        setMaxMbPerSec(5);

        ASSERT_TRUE(indexCursor.seek(opCtx, kMinKeyString.getView()));
        ASSERT_TRUE(recordCursor.seekExact(opCtx, RecordId(1)));
        int numRecords = 2;

        while (indexCursor.next(opCtx)) {
            ASSERT_TRUE(recordCursor.next(opCtx));
            numRecords += 2;
        }

        Date_t end = getTime();

        ASSERT_EQ(numRecords, 20);
        ASSERT_GTE(end - start, Milliseconds(2000) - kErrorMargin);
    }
}

}  // namespace

}  // namespace mongo
