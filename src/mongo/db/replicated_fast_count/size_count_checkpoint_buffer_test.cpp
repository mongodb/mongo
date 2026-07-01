/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/db/replicated_fast_count/size_count_checkpoint_buffer.h"

#include "mongo/bson/timestamp.h"
#include "mongo/db/replicated_fast_count/replicated_fast_count_test_helpers.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/uuid.h"

#include <list>

namespace mongo::replicated_fast_count {
namespace {

using test_helpers::makeOplogEntry;
using test_helpers::NsAndUUID;
using test_helpers::OplogCursorMock;

CollectionSizeCount calculateOplogSizeCount(const std::list<repl::OplogEntry>& entries) {
    CollectionSizeCount result;
    for (const auto& entry : entries) {
        result.size += entry.getEntry().toBSON().objsize();
        result.count += 1;
    }
    return result;
}

TEST(SizeCountCheckpointBufferTest, EmptyBufferStartsWithoutWork) {
    SizeCountCheckpointBuffer buffer(UUID::gen(), boost::none);

    EXPECT_FALSE(buffer.checkoutForFlush().has_value());
}

TEST(SizeCountCheckpointBufferTest, CheckoutGetsScannedDeltas) {
    const UUID oplogUuid = UUID::gen();
    const NsAndUUID coll{.nss = NamespaceString::createNamespaceString_forTest("collA"),
                         .uuid = UUID::gen()};

    const std::list<repl::OplogEntry> entries{
        makeOplogEntry(Timestamp(3, 3), coll, repl::OpTypeEnum::kInsert, /*sizeDelta=*/25)};
    SizeCountCheckpointBuffer buffer(oplogUuid, boost::none);
    OplogCursorMock cursor(entries);

    buffer.scanToNoHolesEOF(cursor);

    const boost::optional<OplogScanResult> checkedOutBuffer = buffer.checkoutForFlush();
    ASSERT_TRUE(checkedOutBuffer.has_value());

    const CollectionSizeCount expectedOplogSizeCount = calculateOplogSizeCount(entries);
    const OplogScanResult expectedCheckedOutBuffer{
        .deltas = SizeCountDeltas{{coll.uuid,
                                   SizeCountDelta{.sizeCount =
                                                      CollectionSizeCount{.size = 25, .count = 1}}},
                                  {oplogUuid, SizeCountDelta{.sizeCount = expectedOplogSizeCount}}},
        .lastTimestamp = Timestamp(3, 3)};

    EXPECT_EQ(checkedOutBuffer, expectedCheckedOutBuffer);
}

TEST(SizeCountCheckpointBufferTest, MultipleScansAccumulateIntoOneCheckout) {
    const UUID oplogUuid = UUID::gen();
    const NsAndUUID coll{.nss = NamespaceString::createNamespaceString_forTest("collA"),
                         .uuid = UUID::gen()};

    SizeCountCheckpointBuffer buffer(oplogUuid, boost::none);
    {
        OplogCursorMock cursor(
            {makeOplogEntry(Timestamp(2, 1), coll, repl::OpTypeEnum::kInsert, /*sizeDelta=*/10)});
        buffer.scanToNoHolesEOF(cursor);
    }
    const std::list<repl::OplogEntry> entries{
        makeOplogEntry(Timestamp(2, 1), coll, repl::OpTypeEnum::kInsert, /*sizeDelta=*/10),
        makeOplogEntry(Timestamp(2, 2), coll, repl::OpTypeEnum::kInsert, /*sizeDelta=*/20)};
    {
        OplogCursorMock cursor(entries);
        buffer.scanToNoHolesEOF(cursor);
    }

    const boost::optional<OplogScanResult> checkedOutBuffer = buffer.checkoutForFlush();
    ASSERT_TRUE(checkedOutBuffer.has_value());
    const CollectionSizeCount expectedOplogSizeCount = calculateOplogSizeCount(entries);
    const OplogScanResult expectedCheckedOutBuffer{
        .deltas = SizeCountDeltas{{coll.uuid,
                                   SizeCountDelta{.sizeCount =
                                                      CollectionSizeCount{.size = 30, .count = 2}}},
                                  {oplogUuid, SizeCountDelta{.sizeCount = expectedOplogSizeCount}}},
        .lastTimestamp = Timestamp(2, 2)};

    EXPECT_EQ(checkedOutBuffer, expectedCheckedOutBuffer);
}

TEST(SizeCountCheckpointBufferTest, InFlightBatchIsRetriedUntilAcknowledged) {
    const UUID oplogUuid = UUID::gen();
    const NsAndUUID coll{.nss = NamespaceString::createNamespaceString_forTest("collA"),
                         .uuid = UUID::gen()};

    SizeCountCheckpointBuffer buffer(oplogUuid, boost::none);
    const std::list<repl::OplogEntry> entries{
        makeOplogEntry(Timestamp(6, 6), coll, repl::OpTypeEnum::kInsert, /*sizeDelta=*/40)};
    OplogCursorMock cursor(entries);

    buffer.scanToNoHolesEOF(cursor);

    const boost::optional<OplogScanResult> first = buffer.checkoutForFlush();
    ASSERT_TRUE(first.has_value());
    const boost::optional<OplogScanResult> retried = buffer.checkoutForFlush();
    ASSERT_TRUE(retried.has_value());

    const CollectionSizeCount expectedOplogSizeCount = calculateOplogSizeCount(entries);
    const OplogScanResult expectedCheckedOutBuffer{
        .deltas = SizeCountDeltas{{coll.uuid,
                                   SizeCountDelta{.sizeCount =
                                                      CollectionSizeCount{.size = 40, .count = 1}}},
                                  {oplogUuid, SizeCountDelta{.sizeCount = expectedOplogSizeCount}}},
        .lastTimestamp = Timestamp(6, 6)};

    EXPECT_EQ(retried, expectedCheckedOutBuffer);
    EXPECT_EQ(first, retried);
}

TEST(SizeCountCheckpointBufferTest, AcknowledgeFlushSuccessClearsInFlight) {
    const UUID oplogUuid = UUID::gen();
    const NsAndUUID coll{.nss = NamespaceString::createNamespaceString_forTest("collA"),
                         .uuid = UUID::gen()};

    SizeCountCheckpointBuffer buffer(oplogUuid, boost::none);
    OplogCursorMock cursor(
        {makeOplogEntry(Timestamp(2, 2), coll, repl::OpTypeEnum::kInsert, /*sizeDelta=*/10)});
    buffer.scanToNoHolesEOF(cursor);

    EXPECT_TRUE(buffer.checkoutForFlush().has_value());

    buffer.acknowledgeFlushSuccess();

    // Pending was reset when the batch was cut, so there is nothing left to flush.
    EXPECT_FALSE(buffer.checkoutForFlush().has_value());
}

TEST(SizeCountCheckpointBufferTest, ScanAfterAcknowledgementIsIndependent) {
    const UUID oplogUuid = UUID::gen();
    const NsAndUUID coll{.nss = NamespaceString::createNamespaceString_forTest("collA"),
                         .uuid = UUID::gen()};

    SizeCountCheckpointBuffer buffer(oplogUuid, boost::none);

    // First scan.
    {
        const std::list<repl::OplogEntry> entries{
            makeOplogEntry(Timestamp(2, 1), coll, repl::OpTypeEnum::kInsert, /*sizeDelta=*/10)};
        OplogCursorMock cursor(entries);

        buffer.scanToNoHolesEOF(cursor);

        const boost::optional<OplogScanResult> checkedOutBuffer = buffer.checkoutForFlush();
        ASSERT_TRUE(checkedOutBuffer.has_value());

        const CollectionSizeCount expectedOplogSizeCount = calculateOplogSizeCount(entries);
        const OplogScanResult expectedCheckedOutBuffer{
            .deltas =
                SizeCountDeltas{
                    {coll.uuid,
                     SizeCountDelta{.sizeCount = CollectionSizeCount{.size = 10, .count = 1}}},
                    {oplogUuid, SizeCountDelta{.sizeCount = expectedOplogSizeCount}}},
            .lastTimestamp = Timestamp(2, 1)};

        EXPECT_EQ(checkedOutBuffer, expectedCheckedOutBuffer);
    }

    buffer.acknowledgeFlushSuccess();

    // Second scan.
    {
        const std::list<repl::OplogEntry> entries{
            makeOplogEntry(Timestamp(2, 1), coll, repl::OpTypeEnum::kInsert, /*sizeDelta=*/10),
            makeOplogEntry(Timestamp(3, 1), coll, repl::OpTypeEnum::kInsert, /*sizeDelta=*/20)};
        OplogCursorMock cursor(entries);

        buffer.scanToNoHolesEOF(cursor);

        const boost::optional<OplogScanResult> checkedOutBuffer = buffer.checkoutForFlush();
        ASSERT_TRUE(checkedOutBuffer.has_value());

        const CollectionSizeCount expectedOplogSizeCount =
            calculateOplogSizeCount(std::list<repl::OplogEntry>{entries.back()});
        const OplogScanResult expectedCheckedOutBuffer{
            .deltas =
                SizeCountDeltas{
                    {coll.uuid,
                     SizeCountDelta{.sizeCount = CollectionSizeCount{.size = 20, .count = 1}}},
                    {oplogUuid, SizeCountDelta{.sizeCount = expectedOplogSizeCount}}},
            .lastTimestamp = Timestamp(3, 1)};

        EXPECT_EQ(checkedOutBuffer, expectedCheckedOutBuffer);
    }
}

TEST(SizeCountCheckpointBufferTest, InternalOnlyScanProducesNoFlushableWork) {
    const UUID oplogUuid = UUID::gen();
    const NsAndUUID internalColl{.nss = NamespaceString::makeGlobalConfigCollection(
                                     NamespaceString::kReplicatedFastCountStore),
                                 .uuid = UUID::gen()};

    SizeCountCheckpointBuffer buffer(oplogUuid, boost::none);
    OplogCursorMock cursor({makeOplogEntry(
        Timestamp(2, 1), internalColl, repl::OpTypeEnum::kInsert, /*sizeDelta=*/9)});
    buffer.scanToNoHolesEOF(cursor);

    EXPECT_FALSE(buffer.checkoutForFlush().has_value());
}

TEST(SizeCountCheckpointBufferTest, DropThenImportAcrossScansYieldsDroppedAndRecreated) {
    const UUID oplogUuid = UUID::gen();
    const NsAndUUID coll{.nss = NamespaceString::createNamespaceString_forTest("test", "collA"),
                         .uuid = UUID::gen()};

    SizeCountCheckpointBuffer buffer(oplogUuid, boost::none);
    {
        OplogCursorMock cursor({test_helpers::makeDropOplogEntry(Timestamp(2, 1), coll)});
        buffer.scanToNoHolesEOF(cursor);
    }
    const std::list<repl::OplogEntry> entries{
        test_helpers::makeDropOplogEntry(Timestamp(2, 1), coll),
        test_helpers::makeImportCollectionOplogEntry(
            Timestamp(2, 2), coll, /*numRecords=*/3, /*dataSize=*/90)};
    {
        OplogCursorMock cursor(entries);
        buffer.scanToNoHolesEOF(cursor);
    }

    const boost::optional<OplogScanResult> checkedOutBuffer = buffer.checkoutForFlush();
    ASSERT_TRUE(checkedOutBuffer.has_value());

    const CollectionSizeCount expectedOplogSizeCount = calculateOplogSizeCount(entries);
    const OplogScanResult expectedCheckedOutBuffer{
        .deltas = SizeCountDeltas{{coll.uuid,
                                   SizeCountDelta{.sizeCount =
                                                      CollectionSizeCount{.size = 90, .count = 3},
                                                  .state = DDLState::kDroppedAndRecreated}},
                                  {oplogUuid, SizeCountDelta{.sizeCount = expectedOplogSizeCount}}},
        .lastTimestamp = Timestamp(2, 2)};

    EXPECT_EQ(checkedOutBuffer, expectedCheckedOutBuffer);
}

TEST(SizeCountCheckpointBufferTest, CreateThenDropWithinScanCancelsOut) {
    const UUID oplogUuid = UUID::gen();
    const NsAndUUID coll{.nss = NamespaceString::createNamespaceString_forTest("test", "collA"),
                         .uuid = UUID::gen()};

    SizeCountCheckpointBuffer buffer(oplogUuid, boost::none);
    OplogCursorMock cursor({test_helpers::makeCreateOplogEntry(Timestamp(2, 1), coll),
                            test_helpers::makeDropOplogEntry(Timestamp(2, 2), coll)});
    buffer.scanToNoHolesEOF(cursor);

    const boost::optional<OplogScanResult> checkedOutBuffer = buffer.checkoutForFlush();
    // The drop is a real (non-internal) entry, so the interval has work and a batch is cut.
    ASSERT_TRUE(checkedOutBuffer.has_value());
    // The create and drop cancel out, so the collection delta is gone.
    EXPECT_FALSE(checkedOutBuffer->deltas.contains(coll.uuid));
    ASSERT_TRUE(checkedOutBuffer->lastTimestamp.has_value());
    EXPECT_EQ(*checkedOutBuffer->lastTimestamp, Timestamp(2, 2));
}

TEST(SizeCountCheckpointBufferTest, PartialScanThenWriteConflictDoesNotDoubleCount) {
    const UUID oplogUuid = UUID::gen();
    const NsAndUUID coll{.nss = NamespaceString::createNamespaceString_forTest("collA"),
                         .uuid = UUID::gen()};

    const std::list<repl::OplogEntry> entries{
        makeOplogEntry(Timestamp(2, 1), coll, repl::OpTypeEnum::kInsert, /*sizeDelta=*/10),
        makeOplogEntry(Timestamp(2, 2), coll, repl::OpTypeEnum::kInsert, /*sizeDelta=*/20),
        makeOplogEntry(Timestamp(2, 3), coll, repl::OpTypeEnum::kInsert, /*sizeDelta=*/30),
    };

    SizeCountCheckpointBuffer buffer(oplogUuid, boost::none);

    // The first scan fails after consuming one entry.
    {
        OplogCursorMock cursor(entries, /*throwWriteConflictOnNthCall=*/2);
        ASSERT_THROWS_CODE(buffer.scanToNoHolesEOF(cursor), DBException, ErrorCodes::WriteConflict);
    }

    // The second scan reads the rest of the entries and succeeds.
    {
        OplogCursorMock cursor(entries);
        buffer.scanToNoHolesEOF(cursor);
    }

    const boost::optional<OplogScanResult> checkedOutBuffer = buffer.checkoutForFlush();
    const CollectionSizeCount expectedOplogSizeCount = calculateOplogSizeCount(entries);
    const OplogScanResult expectedCheckedOutBuffer{
        .deltas = SizeCountDeltas{{coll.uuid,
                                   SizeCountDelta{.sizeCount =
                                                      CollectionSizeCount{.size = 60, .count = 3}}},
                                  {oplogUuid, SizeCountDelta{.sizeCount = expectedOplogSizeCount}}},
        .lastTimestamp = Timestamp(2, 3)};
    EXPECT_EQ(checkedOutBuffer, expectedCheckedOutBuffer);
}
}  // namespace
}  // namespace mongo::replicated_fast_count
