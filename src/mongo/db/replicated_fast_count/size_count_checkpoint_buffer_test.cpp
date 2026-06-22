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

TEST(SizeCountCheckpointBufferTest, EmptyBufferStartsWithoutWork) {
    SizeCountCheckpointBuffer buffer(UUID::gen());

    EXPECT_FALSE(buffer.hasInFlightWork());
    EXPECT_FALSE(buffer.checkoutForFlush().has_value());
}

TEST(SizeCountCheckpointBufferTest, CheckoutGetsScannedDeltas) {
    const UUID oplogUuid = UUID::gen();
    const NsAndUUID coll{.nss = NamespaceString::createNamespaceString_forTest("collA"),
                         .uuid = UUID::gen()};

    SizeCountCheckpointBuffer buffer(oplogUuid);
    OplogCursorMock cursor(
        {makeOplogEntry(Timestamp(3, 3), coll, repl::OpTypeEnum::kInsert, /*sizeDelta=*/25)});

    const boost::optional<RecordId> rid = buffer.scanToNoHolesEOF(cursor);
    ASSERT_TRUE(rid.has_value());
    EXPECT_FALSE(buffer.hasInFlightWork());

    const boost::optional<OplogScanResult> batch = buffer.checkoutForFlush();
    ASSERT_TRUE(batch.has_value());

    ASSERT_TRUE(batch->lastTimestamp.has_value());
    EXPECT_EQ(*batch->lastTimestamp, Timestamp(3, 3));

    ASSERT_TRUE(batch->deltas.contains(coll.uuid));
    const SizeCountDelta delta = batch->deltas.at(coll.uuid);
    EXPECT_EQ(delta.sizeCount, CollectionSizeCount({.size = 25, .count = 1}));
    EXPECT_EQ(delta.state, DDLState::kNone);

    EXPECT_TRUE(buffer.hasInFlightWork());
}

TEST(SizeCountCheckpointBufferTest, MultipleScansAccumulateIntoOneCheckout) {
    const UUID oplogUuid = UUID::gen();
    const NsAndUUID coll{.nss = NamespaceString::createNamespaceString_forTest("collA"),
                         .uuid = UUID::gen()};

    SizeCountCheckpointBuffer buffer(oplogUuid);
    {
        OplogCursorMock cursor(
            {makeOplogEntry(Timestamp(2, 1), coll, repl::OpTypeEnum::kInsert, /*sizeDelta=*/10)});
        const boost::optional<RecordId> rid = buffer.scanToNoHolesEOF(cursor);
        ASSERT_TRUE(rid.has_value());
        EXPECT_FALSE(buffer.hasInFlightWork());
    }
    {
        OplogCursorMock cursor(
            {makeOplogEntry(Timestamp(2, 2), coll, repl::OpTypeEnum::kInsert, /*sizeDelta=*/20)});
        const boost::optional<RecordId> rid = buffer.scanToNoHolesEOF(cursor);
        ASSERT_TRUE(rid.has_value());
        EXPECT_FALSE(buffer.hasInFlightWork());
    }

    const boost::optional<OplogScanResult> batch = buffer.checkoutForFlush();
    ASSERT_TRUE(batch.has_value());
    ASSERT_TRUE(batch->lastTimestamp.has_value());
    EXPECT_EQ(*batch->lastTimestamp, Timestamp(2, 2));
    EXPECT_TRUE(buffer.hasInFlightWork());

    ASSERT_TRUE(batch->deltas.contains(coll.uuid));
    const SizeCountDelta delta = batch->deltas.at(coll.uuid);
    EXPECT_EQ(delta.sizeCount, CollectionSizeCount({.size = 30, .count = 2}));
    EXPECT_EQ(delta.state, DDLState::kNone);
}

TEST(SizeCountCheckpointBufferTest, InFlightBatchIsRetriedUntilAcknowledged) {
    const UUID oplogUuid = UUID::gen();
    const NsAndUUID coll{.nss = NamespaceString::createNamespaceString_forTest("collA"),
                         .uuid = UUID::gen()};

    SizeCountCheckpointBuffer buffer(oplogUuid);
    OplogCursorMock cursor(
        {makeOplogEntry(Timestamp(6, 6), coll, repl::OpTypeEnum::kInsert, /*sizeDelta=*/40)});

    EXPECT_FALSE(buffer.hasInFlightWork());

    const boost::optional<RecordId> rid = buffer.scanToNoHolesEOF(cursor);
    const boost::optional<OplogScanResult> first = buffer.checkoutForFlush();
    ASSERT_TRUE(first.has_value());
    EXPECT_TRUE(buffer.hasInFlightWork());

    const boost::optional<OplogScanResult> retried = buffer.checkoutForFlush();
    ASSERT_TRUE(retried.has_value());

    EXPECT_EQ(retried->lastTimestamp, first->lastTimestamp);
    ASSERT_TRUE(retried->deltas.contains(coll.uuid));
    const SizeCountDelta retriedDelta = retried->deltas.at(coll.uuid);
    EXPECT_EQ(retriedDelta.sizeCount, CollectionSizeCount({.size = 40, .count = 1}));
    EXPECT_EQ(retriedDelta.state, DDLState::kNone);
}

TEST(SizeCountCheckpointBufferTest, AcknowledgeFlushSuccessClearsInFlight) {
    const UUID oplogUuid = UUID::gen();
    const NsAndUUID coll{.nss = NamespaceString::createNamespaceString_forTest("collA"),
                         .uuid = UUID::gen()};

    SizeCountCheckpointBuffer buffer(oplogUuid);
    OplogCursorMock cursor(
        {makeOplogEntry(Timestamp(2, 2), coll, repl::OpTypeEnum::kInsert, /*sizeDelta=*/10)});
    const boost::optional<RecordId> rid = buffer.scanToNoHolesEOF(cursor);
    ASSERT_TRUE(rid.has_value());
    EXPECT_FALSE(buffer.hasInFlightWork());

    ASSERT_TRUE(buffer.checkoutForFlush().has_value());
    ASSERT_TRUE(buffer.hasInFlightWork());

    buffer.acknowledgeFlushSuccess();

    EXPECT_FALSE(buffer.hasInFlightWork());
    // Pending was reset when the batch was cut, so there is nothing left to flush.
    EXPECT_FALSE(buffer.checkoutForFlush().has_value());
}

TEST(SizeCountCheckpointBufferTest, ScanAfterAcknowledgementIsIndependent) {
    const UUID oplogUuid = UUID::gen();
    const NsAndUUID coll{.nss = NamespaceString::createNamespaceString_forTest("collA"),
                         .uuid = UUID::gen()};

    SizeCountCheckpointBuffer buffer(oplogUuid);

    // First scan.
    {
        OplogCursorMock cursor(
            {makeOplogEntry(Timestamp(2, 1), coll, repl::OpTypeEnum::kInsert, /*sizeDelta=*/10)});
        const boost::optional<RecordId> rid = buffer.scanToNoHolesEOF(cursor);
        ASSERT_TRUE(rid.has_value());
        const boost::optional<OplogScanResult> first = buffer.checkoutForFlush();
        ASSERT_TRUE(first.has_value());
        EXPECT_EQ(*first->lastTimestamp, Timestamp(2, 1));

        ASSERT_TRUE(first->deltas.contains(coll.uuid));
        const SizeCountDelta delta = first->deltas.at(coll.uuid);
        EXPECT_EQ(delta.sizeCount, CollectionSizeCount({.size = 10, .count = 1}));
        EXPECT_EQ(delta.state, DDLState::kNone);
    }

    buffer.acknowledgeFlushSuccess();

    // Second scan.
    {
        OplogCursorMock cursor(
            {makeOplogEntry(Timestamp(3, 1), coll, repl::OpTypeEnum::kInsert, /*sizeDelta=*/20)});
        const boost::optional<RecordId> rid = buffer.scanToNoHolesEOF(cursor);
        ASSERT_TRUE(rid.has_value());
        const boost::optional<OplogScanResult> second = buffer.checkoutForFlush();
        ASSERT_TRUE(second.has_value());
        ASSERT_TRUE(second->lastTimestamp.has_value());
        EXPECT_EQ(*second->lastTimestamp, Timestamp(3, 1));

        ASSERT_TRUE(second->deltas.contains(coll.uuid));
        const SizeCountDelta delta = second->deltas.at(coll.uuid);
        EXPECT_EQ(delta.sizeCount, CollectionSizeCount({.size = 20, .count = 1}));
        EXPECT_EQ(delta.state, DDLState::kNone);
    }
}

TEST(SizeCountCheckpointBufferTest, InternalOnlyScanProducesNoFlushableWork) {
    const UUID oplogUuid = UUID::gen();
    const NsAndUUID internalColl{.nss = NamespaceString::makeGlobalConfigCollection(
                                     NamespaceString::kReplicatedFastCountStore),
                                 .uuid = UUID::gen()};

    SizeCountCheckpointBuffer buffer(oplogUuid);
    // The record is physically scanned (rid returned), but it targets an internal fast-count
    // collection, so it advances no checkpoint and its oplog self-size bytes are dropped.
    OplogCursorMock cursor({makeOplogEntry(
        Timestamp(2, 1), internalColl, repl::OpTypeEnum::kInsert, /*sizeDelta=*/9)});
    const boost::optional<RecordId> rid = buffer.scanToNoHolesEOF(cursor);
    ASSERT_TRUE(rid.has_value());

    EXPECT_FALSE(buffer.checkoutForFlush().has_value());
    EXPECT_FALSE(buffer.hasInFlightWork());
}

TEST(SizeCountCheckpointBufferTest, DropThenImportAcrossScansYieldsDroppedAndRecreated) {
    const UUID oplogUuid = UUID::gen();
    const NsAndUUID coll{.nss = NamespaceString::createNamespaceString_forTest("test", "collA"),
                         .uuid = UUID::gen()};

    SizeCountCheckpointBuffer buffer(oplogUuid);
    {
        OplogCursorMock cursor({test_helpers::makeDropOplogEntry(Timestamp(2, 1), coll)});
        const boost::optional<RecordId> rid = buffer.scanToNoHolesEOF(cursor);
        ASSERT_TRUE(rid.has_value());
        EXPECT_FALSE(buffer.hasInFlightWork());
    }
    {
        OplogCursorMock cursor({test_helpers::makeImportCollectionOplogEntry(
            Timestamp(2, 2), coll, /*numRecords=*/3, /*dataSize=*/90)});
        const boost::optional<RecordId> rid = buffer.scanToNoHolesEOF(cursor);
        ASSERT_TRUE(rid.has_value());
        EXPECT_FALSE(buffer.hasInFlightWork());
    }

    const boost::optional<OplogScanResult> batch = buffer.checkoutForFlush();
    ASSERT_TRUE(batch.has_value());
    ASSERT_TRUE(batch->deltas.contains(coll.uuid));
    const SizeCountDelta delta = batch->deltas.at(coll.uuid);
    EXPECT_EQ(delta.state, DDLState::kDroppedAndRecreated);
    EXPECT_EQ(delta.sizeCount, CollectionSizeCount({.size = 90, .count = 3}));
    ASSERT_TRUE(batch->lastTimestamp.has_value());
    EXPECT_EQ(*batch->lastTimestamp, Timestamp(2, 2));
}

TEST(SizeCountCheckpointBufferTest, CreateThenDropWithinScanCancelsOut) {
    const UUID oplogUuid = UUID::gen();
    const NsAndUUID coll{.nss = NamespaceString::createNamespaceString_forTest("test", "collA"),
                         .uuid = UUID::gen()};

    SizeCountCheckpointBuffer buffer(oplogUuid);
    {
        OplogCursorMock cursor({test_helpers::makeCreateOplogEntry(Timestamp(2, 1), coll)});
        const boost::optional<RecordId> rid = buffer.scanToNoHolesEOF(cursor);
        ASSERT_TRUE(rid.has_value());
        EXPECT_FALSE(buffer.hasInFlightWork());
    }
    {
        OplogCursorMock cursor({test_helpers::makeDropOplogEntry(Timestamp(2, 2), coll)});
        const boost::optional<RecordId> rid = buffer.scanToNoHolesEOF(cursor);
        ASSERT_TRUE(rid.has_value());
        EXPECT_FALSE(buffer.hasInFlightWork());
    }

    const boost::optional<OplogScanResult> batch = buffer.checkoutForFlush();
    // The drop is a real (non-internal) entry, so the interval has work and a batch is cut.
    ASSERT_TRUE(batch.has_value());
    // The create and drop cancel out, so the collection delta is gone.
    EXPECT_FALSE(batch->deltas.contains(coll.uuid));
    ASSERT_TRUE(batch->lastTimestamp.has_value());
    EXPECT_EQ(*batch->lastTimestamp, Timestamp(2, 2));
}
}  // namespace
}  // namespace mongo::replicated_fast_count
