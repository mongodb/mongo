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

#include "mongo/db/exec/classic/recordid_deduplicator.h"

#include "mongo/db/local_catalog/catalog_test_fixture.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/pipeline/process_interface/standalone_process_interface.h"
#include "mongo/db/pipeline/spilling/spilling_test_fixture.h"
#include "mongo/unittest/unittest.h"

using namespace mongo;

namespace {

void assertInsertNew(RecordIdDeduplicator& recordIdDeduplicator, RecordId recordId) {
    ASSERT_FALSE(recordIdDeduplicator.contains(recordId)) << "RecordId " << recordId;
    ASSERT_TRUE(recordIdDeduplicator.insert(recordId)) << "RecordId " << recordId;
    ASSERT_TRUE(recordIdDeduplicator.contains(recordId)) << "RecordId " << recordId;
}

void assertInsertExisting(RecordIdDeduplicator& recordIdDeduplicator, RecordId recordId) {
    ASSERT_TRUE(recordIdDeduplicator.contains(recordId)) << "RecordId " << recordId;
    ASSERT_FALSE(recordIdDeduplicator.insert(recordId)) << "RecordId " << recordId;
    ASSERT_TRUE(recordIdDeduplicator.contains(recordId)) << "RecordId " << recordId;
}

std::vector<int64_t> createIntRecords() {
    std::vector<int64_t> recordIds;
    recordIds.reserve(60 * 50);
    int64_t number = 1;
    for (int shiftNum = 0; shiftNum < 60; ++shiftNum) {
        number <<= 1;
        for (int64_t idx = 1; idx < 50; ++idx) {
            number += idx * 3;
            recordIds.emplace_back(number);
        }
    }

    return recordIds;
}

class RecordIdDeduplicatorTest : public SpillingTestFixture {
public:
    SpillingStats spillingStats;
};

TEST_F(RecordIdDeduplicatorTest, basicTest) {
    RecordIdDeduplicator recordIdDeduplicator{_expCtx.get(), 40, 6, 10000};

    // Insert a few recordIds.
    RecordId stringRecordId(std::span("ABCDE", 5));
    RecordId longRecordId(12345678);
    RecordId nullRecordId;

    assertInsertNew(recordIdDeduplicator, stringRecordId);
    assertInsertNew(recordIdDeduplicator, longRecordId);
    assertInsertNew(recordIdDeduplicator, nullRecordId);

    // Insert the same records.
    assertInsertExisting(recordIdDeduplicator, stringRecordId);
    assertInsertExisting(recordIdDeduplicator, longRecordId);
    assertInsertExisting(recordIdDeduplicator, nullRecordId);
}

TEST_F(RecordIdDeduplicatorTest, spillNoDiskUsageTest) {
    _expCtx->setAllowDiskUse(false);
    RecordIdDeduplicator recordIdDeduplicator{_expCtx.get(), 40, 6, 10000};

    RecordId stringRecordId(std::span("ABCDEFGHIJABCDEFGHIJABCDEFGHIJKLMN", 34));
    RecordId longRecordId(12345678);

    assertInsertNew(recordIdDeduplicator, longRecordId);
    assertInsertNew(recordIdDeduplicator, stringRecordId);
    ASSERT_THROWS_CODE(recordIdDeduplicator.spill(spillingStats),
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

    assertInsertNew(recordIdDeduplicator, nullRecordId);
    assertInsertNew(recordIdDeduplicator, longRecordId);
    assertInsertNew(recordIdDeduplicator, stringRecordId);

    recordIdDeduplicator.spill(spillingStats);

    // At this point it should have spilled.
    ASSERT_TRUE(recordIdDeduplicator.hasSpilled());
    ASSERT_EQ(expectedSpills, spillingStats.getSpills());
    ASSERT_EQ(expectedSpilledBytes, spillingStats.getSpilledBytes());
    ASSERT_EQ(expectedSpilledRecords, spillingStats.getSpilledRecords());

    // Insert the same records.
    assertInsertExisting(recordIdDeduplicator, stringRecordId);
    assertInsertExisting(recordIdDeduplicator, longRecordId);
    assertInsertExisting(recordIdDeduplicator, nullRecordId);

    // Insert a new recordId.
    RecordId longRecordId2(22345678);
    assertInsertNew(recordIdDeduplicator, longRecordId2);

    // Insert the same recordId.
    assertInsertExisting(recordIdDeduplicator, longRecordId2);

    // The spills should not have changed.
    ASSERT_TRUE(recordIdDeduplicator.hasSpilled());
    ASSERT_EQ(expectedSpills, spillingStats.getSpills());
    ASSERT_EQ(expectedSpilledBytes, spillingStats.getSpilledBytes());
    ASSERT_EQ(expectedSpilledRecords, spillingStats.getSpilledRecords());
}

TEST_F(RecordIdDeduplicatorTest, basicBitmapSpillTest) {
    _expCtx->setAllowDiskUse(true);
    RecordIdDeduplicator recordIdDeduplicator{_expCtx.get(), 40, 6, 1'000'000};

    // Insert a few recordIds.
    RecordId stringRecordId(std::span("ABCDE", 5));
    RecordId nullRecordId;

    assertInsertNew(recordIdDeduplicator, stringRecordId);
    assertInsertNew(recordIdDeduplicator, nullRecordId);

    uint64_t expectedSpills = 1;
    uint64_t expectedSpilledBytes = stringRecordId.memUsage();
    uint64_t expectedSpilledRecords = 1;  // The record with id null is not spilled.

    // Create some recordIds.
    std::vector<int64_t> recordIds = createIntRecords();

    // Add more recordIds to cause roaring to switch to bitmap.
    for (const auto& ridInt : recordIds) {
        RecordId rid{ridInt};
        assertInsertNew(recordIdDeduplicator, rid);
        expectedSpilledBytes += rid.memUsage();
        ++expectedSpilledRecords;
    }

    recordIdDeduplicator.spill(spillingStats);

    // At this point it should have spilled.
    ASSERT_TRUE(recordIdDeduplicator.hasSpilled());
    ASSERT_EQ(expectedSpills, spillingStats.getSpills());
    ASSERT_EQ(expectedSpilledBytes, spillingStats.getSpilledBytes());
    ASSERT_EQ(expectedSpilledRecords, spillingStats.getSpilledRecords());

    // Insert the same records.
    assertInsertExisting(recordIdDeduplicator, stringRecordId);
    assertInsertExisting(recordIdDeduplicator, nullRecordId);

    // Shuffle the recordIds.
    const unsigned int seed(12346);
    std::mt19937 g(seed);

    std::shuffle(recordIds.begin(), recordIds.end(), g);

    for (const auto& ridInt : recordIds) {
        RecordId rid{ridInt};
        assertInsertExisting(recordIdDeduplicator, rid);
    }

    // Insert new recordIds.
    RecordId stringRecordId2(std::span("ABCDF", 5));
    RecordId longRecordId2(22345678);
    assertInsertNew(recordIdDeduplicator, stringRecordId2);
    assertInsertNew(recordIdDeduplicator, longRecordId2);
}

TEST_F(RecordIdDeduplicatorTest, freeMemoryRemovesOnlyInMemoryElements) {
    _expCtx->setAllowDiskUse(true);
    RecordIdDeduplicator recordIdDeduplicator{_expCtx.get(), 40, 6, 1'000'000};

    std::vector<RecordId> recordIds = {
        RecordId{std::span("ABCDE", 5)},
        RecordId{static_cast<int64_t>(1)},
        RecordId{},
    };

    for (const auto& recordId : recordIds) {
        assertInsertNew(recordIdDeduplicator, recordId);
    }

    for (const auto& recordId : recordIds) {
        recordIdDeduplicator.freeMemory(recordId);
        assertInsertNew(recordIdDeduplicator, recordId);
    }

    SpillingStats stats;
    recordIdDeduplicator.spill(stats);
    ASSERT_TRUE(recordIdDeduplicator.hasSpilled());

    for (const auto& recordId : recordIds) {
        recordIdDeduplicator.freeMemory(recordId);
        if (!recordId.isNull()) {
            // Because we have spilled, old recordIds are not in memory, so they are not affected by
            // freeMemory call.
            assertInsertExisting(recordIdDeduplicator, recordId);
        } else {
            // Null record is one bool, so it is never spilled.
            assertInsertNew(recordIdDeduplicator, recordId);
        }
    }

    RecordId newRecordId{static_cast<int64_t>(2)};
    recordIdDeduplicator.freeMemory(newRecordId);
    assertInsertNew(recordIdDeduplicator, newRecordId);
    recordIdDeduplicator.freeMemory(newRecordId);
    assertInsertNew(recordIdDeduplicator, newRecordId);
}

TEST_F(RecordIdDeduplicatorTest, memoryConsumptionTest) {
    _expCtx->setAllowDiskUse(true);
    RecordIdDeduplicator recordIdDeduplicator{_expCtx.get(), 40, 6, 1'000'000};

    // Insert a few recordIds.
    std::vector<RecordId> stringRecordIds = {
        RecordId{std::span("ABCDE", 5)},
        RecordId{std::span("ABCDEFGHIJ", 10)},
        RecordId{std::span("ABCDEFGHIJABCDEFGHIJ", 20)},
        RecordId{std::span("ABCDEFGHIJABCDEFGHIJABCDEFGHIJKLMN", 34)},
        RecordId{std::span("ABCDEFGHIJKLMOPQRSTUABCDEFGHIJABCDEFGHIJKLMOPQRSTU", 50)}};
    RecordId longRecordId(12);
    RecordId nullRecordId;

    // HashRoaringSet occupies a small amount of memory even when it is empty (~200B)
    auto memUsage = recordIdDeduplicator.getApproximateSize();

    for (const auto& stringRecordId : stringRecordIds) {
        recordIdDeduplicator.insert(stringRecordId);
        memUsage += stringRecordId.memUsage();
    }
    // We cannot compute the exact memory usage of the hashset because it keeps some empty slots.
    ASSERT_GREATER_THAN_OR_EQUALS(recordIdDeduplicator.getApproximateSize(), memUsage);

    // A long recordId is stored in the HashRoaringSet and consumes sizeof(uint64_t) bytes if it has
    // not been transferred to the bitmap.
    memUsage = recordIdDeduplicator.getApproximateSize() + sizeof(uint64_t);
    recordIdDeduplicator.insert(longRecordId);
    ASSERT_EQ(recordIdDeduplicator.getApproximateSize(), memUsage);

    // A null recordId does not affect the memory usage.
    recordIdDeduplicator.insert(nullRecordId);
    ASSERT_EQ(recordIdDeduplicator.getApproximateSize(), memUsage);

    // Insert the same records.
    recordIdDeduplicator.insert(stringRecordIds[0]);
    recordIdDeduplicator.insert(longRecordId);
    recordIdDeduplicator.insert(nullRecordId);
    // The memory usage should not change.
    ASSERT_EQ(recordIdDeduplicator.getApproximateSize(), memUsage);

    // Remove some records.
    recordIdDeduplicator.freeMemory(stringRecordIds[0]);
    recordIdDeduplicator.freeMemory(stringRecordIds[stringRecordIds.size() - 1]);

    // The data is removed but the slot is not freed.
    memUsage -= (stringRecordIds[0].memUsage() +
                 stringRecordIds[stringRecordIds.size() - 1].memUsage() - 2 * sizeof(RecordId));
    ASSERT_EQ(recordIdDeduplicator.getApproximateSize(), memUsage);

    recordIdDeduplicator.freeMemory(longRecordId);
    memUsage -= sizeof(uint64_t);
    ASSERT_EQ(recordIdDeduplicator.getApproximateSize(), memUsage);

    recordIdDeduplicator.freeMemory(nullRecordId);
    ASSERT_EQ(recordIdDeduplicator.getApproximateSize(), memUsage);

    // Create some recordIds.
    std::vector<int64_t> recordIds = createIntRecords();

    bool hasDecreased = false;
    bool hasIncreased = false;
    // Add more recordIds to cause roaring to switch to bitmap.
    for (const auto& ridInt : recordIds) {
        recordIdDeduplicator.insert(RecordId{ridInt});
        // After swithing to bitmap the memory consumption should decrease.
        hasDecreased = hasDecreased | (memUsage > recordIdDeduplicator.getApproximateSize());
        // The memory consumption should start increasing again when all records have been
        // transferred to bitmap and more are added.
        hasIncreased = hasDecreased && (memUsage < recordIdDeduplicator.getApproximateSize());
        memUsage = recordIdDeduplicator.getApproximateSize();
    }

    ASSERT(hasDecreased && hasIncreased);

    // After spilling, memory usage should have decreased. It does not return to the empty
    // RecordIdDeduplicator memory consumption because the bitmap and the hashSet do not release all
    // their memory.
    SpillingStats stats;
    recordIdDeduplicator.spill(stats);
    ASSERT_TRUE(recordIdDeduplicator.hasSpilled());
    ASSERT_LESS_THAN(recordIdDeduplicator.getApproximateSize(), memUsage);
}

/**
 * A suite that also initializes a catalog to test interaction between RecordIdDeduplicator
 * spilling and having acquired collections.
 */
class RecordIdDeduplicatorCatalogTest : public CatalogTestFixture {
public:
    RecordIdDeduplicatorCatalogTest() : CatalogTestFixture(Options{}) {}

    void setUp() override {
        CatalogTestFixture::setUp();
        _expCtx = new ExpressionContextForTest(operationContext(), kNss);
        _expCtx->setMongoProcessInterface(std::make_shared<StandaloneProcessInterface>(nullptr));
    }

    void tearDown() override {
        _expCtx.reset();
    }

protected:
    static const NamespaceString kNss;

    boost::intrusive_ptr<ExpressionContext> _expCtx;
};

const NamespaceString RecordIdDeduplicatorCatalogTest::kNss =
    NamespaceString::createNamespaceString_forTest("test.test");

TEST_F(RecordIdDeduplicatorCatalogTest, canSpillWithAcquiredCollection) {
    _expCtx->setAllowDiskUse(true);
    RecordIdDeduplicator recordIdDeduplicator{_expCtx.get(), 40, 6, 1'000'000};

    {
        AutoGetCollection autoColl(operationContext(), kNss, MODE_IX);
        auto db = autoColl.ensureDbExists(operationContext());
        WriteUnitOfWork wuow(operationContext());
        ASSERT(db->createCollection(operationContext(), kNss));
        wuow.commit();
    }

    auto acquisition = acquireCollection(
        operationContext(),
        CollectionAcquisitionRequest(kNss,
                                     PlacementConcern(boost::none, ShardVersion::UNSHARDED()),
                                     repl::ReadConcernArgs::get(operationContext()),
                                     AcquisitionPrerequisites::kRead),
        MODE_IS);

    RecordId stringRecordId(std::span("ABCDEFGHIJABCDEFGHIJABCDEFGHIJKLMN", 34));
    RecordId longRecordId(12345678);
    RecordId nullRecordId;

    assertInsertNew(recordIdDeduplicator, nullRecordId);
    assertInsertNew(recordIdDeduplicator, longRecordId);
    assertInsertNew(recordIdDeduplicator, stringRecordId);

    SpillingStats stats;
    recordIdDeduplicator.spill(stats);

    assertInsertExisting(recordIdDeduplicator, nullRecordId);
    assertInsertExisting(recordIdDeduplicator, longRecordId);
    assertInsertExisting(recordIdDeduplicator, stringRecordId);
}

}  // namespace
