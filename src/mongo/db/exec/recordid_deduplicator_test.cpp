/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

//
// This file contains tests for mongo/db/exec/recordid_deduplicator.cpp
//

#include "mongo/db/concurrency/exception_util.h"
#include "mongo/db/exec/recordid_deduplicator.h"
#include "mongo/db/pipeline/spilling/spilling_test_fixture.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/framework.h"

using namespace mongo;

namespace {

class RecordIdDeduplicatorTest : public SpillingTestFixture {};

//
// Basic test that we get out valid stats objects.
//
TEST_F(RecordIdDeduplicatorTest, basicTest) {
    RecordIdDeduplicator recordIdDeduplicator{_expCtx.get(), 40, 6, 10000};

    // Insert a few recordIds.
    RecordId stringRecordId(std::span("ABCDE", 5));
    RecordId longRecordId(12345678);
    RecordId nullRecordId;

    ASSERT(recordIdDeduplicator.insert(stringRecordId));
    ASSERT(recordIdDeduplicator.insert(longRecordId));
    ASSERT(recordIdDeduplicator.insert(nullRecordId));

    // Insert the same records.
    ASSERT(!recordIdDeduplicator.insert(stringRecordId));
    ASSERT(!recordIdDeduplicator.insert(longRecordId));
    ASSERT(!recordIdDeduplicator.insert(nullRecordId));
}

TEST_F(RecordIdDeduplicatorTest, spillNoDiskUsageTest) {
    _expCtx->setAllowDiskUse(false);
    RecordIdDeduplicator recordIdDeduplicator{_expCtx.get(), 40, 6, 10000};

    RecordId stringRecordId(std::span("ABCDEFGHIJABCDEFGHIJABCDEFGHIJKLMN", 34));
    RecordId longRecordId(12345678);

    ASSERT_TRUE(recordIdDeduplicator.insert(longRecordId));
    ASSERT_TRUE(recordIdDeduplicator.insert(stringRecordId));
    ASSERT_THROWS_CODE(recordIdDeduplicator.forceSpill(),
                       DBException,
                       ErrorCodes::QueryExceededMemoryLimitNoDiskUseAllowed);
}

TEST_F(RecordIdDeduplicatorTest, basicHashSpillTest) {
    _expCtx->setAllowDiskUse(true);
    RecordIdDeduplicator recordIdDeduplicator{_expCtx.get(), 40, 6, 10000};

    // Insert a few recordIds.
    RecordId stringRecordId(std::span("ABCDEFGHIJABCDEFGHIJABCDEFGHIJKLMN", 34));
    RecordId longRecordId(12345678);
    RecordId nullRecordId;

    uint64_t expectedSpills = 1;
    uint64_t expectedSpilledBytes = stringRecordId.memUsage() + longRecordId.memUsage();
    uint64_t expectedSpilledRecords = 2;  // The record with id null is not spilled.

    ASSERT_TRUE(recordIdDeduplicator.insert(nullRecordId));
    ASSERT_TRUE(recordIdDeduplicator.insert(longRecordId));
    ASSERT_TRUE(recordIdDeduplicator.insert(stringRecordId));

    recordIdDeduplicator.forceSpill();

    // At this point it should have spilled.
    ASSERT_TRUE(recordIdDeduplicator.hasSpilled());
    ASSERT_EQ(expectedSpills, recordIdDeduplicator.getSpillingStats().getSpills());
    ASSERT_EQ(expectedSpilledBytes, recordIdDeduplicator.getSpillingStats().getSpilledBytes());
    ASSERT_EQ(expectedSpilledRecords, recordIdDeduplicator.getSpillingStats().getSpilledRecords());

    // Insert the same records.
    ASSERT_FALSE(recordIdDeduplicator.insert(stringRecordId));
    ASSERT_FALSE(recordIdDeduplicator.insert(longRecordId));
    ASSERT_FALSE(recordIdDeduplicator.insert(nullRecordId));

    // Insert a new recordId.
    RecordId longRecordId2(22345678);
    ASSERT_TRUE(recordIdDeduplicator.insert(longRecordId2));

    // Insert the same recordId.
    ASSERT_FALSE(recordIdDeduplicator.insert(longRecordId2));

    // The spills should not have changed.
    ASSERT_TRUE(recordIdDeduplicator.hasSpilled());
    ASSERT_EQ(expectedSpills, recordIdDeduplicator.getSpillingStats().getSpills());
    ASSERT_EQ(expectedSpilledBytes, recordIdDeduplicator.getSpillingStats().getSpilledBytes());
    ASSERT_EQ(expectedSpilledRecords, recordIdDeduplicator.getSpillingStats().getSpilledRecords());
}

TEST_F(RecordIdDeduplicatorTest, basicBitmapSpillTest) {
    _expCtx->setAllowDiskUse(true);
    RecordIdDeduplicator recordIdDeduplicator{_expCtx.get(), 40, 6, 1'000'000};

    // Insert a few recordIds.
    RecordId stringRecordId(std::span("ABCDE", 5));
    RecordId nullRecordId;

    ASSERT(recordIdDeduplicator.insert(stringRecordId));
    ASSERT(recordIdDeduplicator.insert(nullRecordId));

    uint64_t expectedSpills = 1;
    uint64_t expectedSpilledBytes = stringRecordId.memUsage();
    uint64_t expectedSpilledRecords = 1;  // The record with id null is not spilled.

    // Add more recordIds to cause roaring to switch to bitmap.
    for (int64_t idx = 1; idx < 3000; ++idx) {
        int64_t number = idx * 3;
        RecordId rid{number};
        ASSERT_TRUE(recordIdDeduplicator.insert(rid));
        expectedSpilledBytes += rid.memUsage();
        ++expectedSpilledRecords;
    }

    recordIdDeduplicator.forceSpill();

    // At this point it should have spilled.
    ASSERT_TRUE(recordIdDeduplicator.hasSpilled());
    ASSERT_EQ(expectedSpills, recordIdDeduplicator.getSpillingStats().getSpills());
    ASSERT_EQ(expectedSpilledBytes, recordIdDeduplicator.getSpillingStats().getSpilledBytes());
    ASSERT_EQ(expectedSpilledRecords, recordIdDeduplicator.getSpillingStats().getSpilledRecords());

    // Insert the same records.
    ASSERT_FALSE(recordIdDeduplicator.insert(stringRecordId));
    ASSERT_FALSE(recordIdDeduplicator.insert(nullRecordId));

    for (uint64_t idx = 1; idx < 50; ++idx) {
        uint64_t number = idx * 3;
        ASSERT_FALSE(recordIdDeduplicator.insert(RecordId(number)));
    }

    // Insert new recordIds.
    RecordId stringRecordId2(std::span("ABCDF", 5));
    RecordId longRecordId2(22345678);
    ASSERT_TRUE(recordIdDeduplicator.insert(stringRecordId2));
    ASSERT_TRUE(recordIdDeduplicator.insert(longRecordId2));
}
}  // namespace
