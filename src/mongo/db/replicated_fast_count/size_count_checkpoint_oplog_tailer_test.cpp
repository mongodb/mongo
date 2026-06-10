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

#include "mongo/db/replicated_fast_count/size_count_checkpoint_oplog_tailer.h"

#include "mongo/db/operation_context_group.h"
#include "mongo/db/record_id_helpers.h"
#include "mongo/db/replicated_fast_count/replicated_fast_count_test_helpers.h"
#include "mongo/db/replicated_fast_count/replicated_fast_size_count.h"
#include "mongo/db/shard_role/shard_catalog/catalog_raii.h"
#include "mongo/db/shard_role/shard_catalog/catalog_test_fixture.h"
#include "mongo/db/shard_role/shard_catalog/collection_catalog.h"
#include "mongo/db/shard_role/transaction_resources.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/stdx/thread.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/future.h"

namespace mongo::replicated_fast_count {
namespace {

using test_helpers::makeOplogEntry;
using test_helpers::NsAndUUID;
using test_helpers::writeToOplog;

class SizeCountCheckpointOplogTailerTest : public CatalogTestFixture {
public:
    SizeCountCheckpointOplogTailerTest()
        : CatalogTestFixture(Options().setPersistenceProvider(
              std::make_unique<test_helpers::ReplicatedFastCountTestPersistenceProvider>())) {}

protected:
    void setUp() override {
        CatalogTestFixture::setUp();
        _opCtx = operationContext();
    }

    UUID getOplogUuid() const {
        const auto coll = CollectionCatalog::get(_opCtx)->lookupCollectionByNamespace(
            _opCtx, NamespaceString::kRsOplogNamespace);
        ASSERT_TRUE(coll);
        return coll->uuid();
    }

    RecordId ridFor(Timestamp ts) const {
        return unittest::assertGet(record_id_helpers::keyForOptime(ts, KeyFormat::Long));
    }

    void assertContains(const OplogScanResult& scan,
                        const UUID& uuid,
                        const CollectionSizeCount& expected) {
        ASSERT_TRUE(scan.deltas.contains(uuid)) << "missing uuid " << uuid.toString();
        EXPECT_EQ(scan.deltas.at(uuid).sizeCount, expected);
    }

    void assertNoBufferedWork(SizeCountCheckpointBuffer& buffer) {
        EXPECT_FALSE(buffer.hasPendingWork());
        EXPECT_FALSE(buffer.hasInFlightWork());
        EXPECT_FALSE(buffer.checkoutForFlush());
    }

    OperationContext* _opCtx = nullptr;
    SizeCountCheckpointOplogTailer _tailer;
    const NsAndUUID _collA{
        .nss = NamespaceString::createNamespaceString_forTest("tailer_test", "collA"),
        .uuid = UUID::gen(),
    };
    const NsAndUUID _collB{
        .nss = NamespaceString::createNamespaceString_forTest("tailer_test", "collB"),
        .uuid = UUID::gen(),
    };
};

TEST_F(SizeCountCheckpointOplogTailerTest, BootstrapFromTimestampMinSeedsBufferWithFirstRecord) {
    const Timestamp ts1{1, 1};
    auto entry1 = makeOplogEntry(ts1, _collA, repl::OpTypeEnum::kInsert, 10);
    const int64_t entry1Bytes = entry1.getEntry().toBSON().objsize();
    writeToOplog(_opCtx, entry1);

    SizeCountCheckpointBuffer buffer;
    auto state = _tailer.bootstrap_ForTest(_opCtx, Timestamp::min(), buffer);

    ASSERT_TRUE(state);
    EXPECT_EQ(state->lastBufferedRid, ridFor(ts1));

    EXPECT_TRUE(buffer.hasPendingWork());
    EXPECT_FALSE(buffer.hasInFlightWork());

    auto flushed = buffer.checkoutForFlush();
    ASSERT_TRUE(flushed);
    EXPECT_EQ(flushed->lastTimestamp, ts1);
    assertContains(*flushed, _collA.uuid, CollectionSizeCount{10, 1});
    assertContains(*flushed, getOplogUuid(), CollectionSizeCount{entry1Bytes, 1});

    EXPECT_FALSE(buffer.hasPendingWork());
    EXPECT_TRUE(buffer.hasInFlightWork());

    buffer.acknowledgeFlushSuccess();
    EXPECT_FALSE(buffer.hasPendingWork());
    EXPECT_FALSE(buffer.hasInFlightWork());
}

TEST_F(SizeCountCheckpointOplogTailerTest, BootstrapFromTimestampMinOnEmptyOplogReturnsNone) {
    SizeCountCheckpointBuffer buffer;
    auto state = _tailer.bootstrap_ForTest(_opCtx, Timestamp::min(), buffer);

    ASSERT_FALSE(state);
    assertNoBufferedWork(buffer);
}

TEST_F(SizeCountCheckpointOplogTailerTest,
       BootstrapFromConcreteTimestampDoesNotSeedBufferAndPinsLastSeenRid) {
    const Timestamp ts1{1, 1};
    writeToOplog(_opCtx, makeOplogEntry(ts1, _collA, repl::OpTypeEnum::kInsert, 10));

    SizeCountCheckpointBuffer buffer;
    auto state = _tailer.bootstrap_ForTest(_opCtx, ts1, buffer);

    ASSERT_TRUE(state);
    EXPECT_EQ(state->lastBufferedRid, ridFor(ts1));
    assertNoBufferedWork(buffer);
}

TEST_F(SizeCountCheckpointOplogTailerTest, MultipleIterationsAccumulateCumulativelyInBuffer) {
    const Timestamp ts1{1, 1};
    auto entry1 = makeOplogEntry(ts1, _collA, repl::OpTypeEnum::kInsert, 10);
    const int64_t entry1Bytes = entry1.getEntry().toBSON().objsize();
    writeToOplog(_opCtx, entry1);

    SizeCountCheckpointBuffer buffer;
    auto state = _tailer.bootstrap_ForTest(_opCtx, Timestamp::min(), buffer);
    ASSERT_TRUE(state);

    const Timestamp ts2{1, 2};
    auto entry2 = makeOplogEntry(ts2, _collA, repl::OpTypeEnum::kInsert, 20);
    const int64_t entry2Bytes = entry2.getEntry().toBSON().objsize();
    writeToOplog(_opCtx, entry2);

    ASSERT_TRUE(_tailer.runOneIteration_ForTest(_opCtx, state, buffer));
    EXPECT_EQ(state->lastBufferedRid, ridFor(ts2));

    const Timestamp ts3{1, 3};
    auto entry3 = makeOplogEntry(ts3, _collB, repl::OpTypeEnum::kInsert, 7);
    const int64_t entry3Bytes = entry3.getEntry().toBSON().objsize();
    writeToOplog(_opCtx, entry3);

    ASSERT_TRUE(_tailer.runOneIteration_ForTest(_opCtx, state, buffer));
    EXPECT_EQ(state->lastBufferedRid, ridFor(ts3));

    EXPECT_TRUE(buffer.hasPendingWork());
    EXPECT_FALSE(buffer.hasInFlightWork());

    auto flushed = buffer.checkoutForFlush();
    ASSERT_TRUE(flushed);
    EXPECT_EQ(flushed->lastTimestamp, ts3);
    assertContains(*flushed, _collA.uuid, CollectionSizeCount{30, 2});
    assertContains(*flushed, _collB.uuid, CollectionSizeCount{7, 1});
    assertContains(
        *flushed, getOplogUuid(), CollectionSizeCount{entry1Bytes + entry2Bytes + entry3Bytes, 3});

    EXPECT_FALSE(buffer.hasPendingWork());
    EXPECT_TRUE(buffer.hasInFlightWork());

    buffer.acknowledgeFlushSuccess();
    EXPECT_FALSE(buffer.hasPendingWork());
    EXPECT_FALSE(buffer.hasInFlightWork());

    ASSERT_FALSE(_tailer.runOneIteration_ForTest(_opCtx, state, buffer));
    assertNoBufferedWork(buffer);
}

using SizeCountCheckpointOplogTailerDeathTest = SizeCountCheckpointOplogTailerTest;

DEATH_TEST_F(SizeCountCheckpointOplogTailerDeathTest,
             RunOneIterationTassertsIfLastBufferedRidMissingFromOplog,
             "12101812") {
    const Timestamp ts1{1, 1};
    writeToOplog(_opCtx, makeOplogEntry(ts1, _collA, repl::OpTypeEnum::kInsert, 10));

    SizeCountCheckpointBuffer buffer;
    auto state = _tailer.bootstrap_ForTest(_opCtx, Timestamp::min(), buffer);
    ASSERT_TRUE(state);
    ASSERT_EQ(state->lastBufferedRid, ridFor(ts1));

    // Simulate oplog rollover by deleting the entry the tailer last observed.
    {
        AutoGetOplogFastPath oplogWrite(_opCtx, OplogAccessMode::kWrite);
        WriteUnitOfWork wuow(_opCtx);
        oplogWrite.getCollection()->getRecordStore()->deleteRecord(
            _opCtx, *shard_role_details::getRecoveryUnit(_opCtx), ridFor(ts1));
        wuow.commit();
    }

    _tailer.runOneIteration_ForTest(_opCtx, state, buffer);
}

TEST_F(SizeCountCheckpointOplogTailerTest, RunInterruptedWhileWaitingExitsPromptly) {
    const Timestamp ts1{1, 1};
    writeToOplog(_opCtx, makeOplogEntry(ts1, _collA, repl::OpTypeEnum::kInsert, 10));

    OperationContextGroup ocg;
    SizeCountCheckpointBuffer threadBuffer;
    Status threadStatus = Status::OK();
    auto [promise, future] = makePromiseFuture<void>();

    stdx::thread t([&] {
        auto client = getServiceContext()->getService()->makeClient("checkpointTailerThread");
        AlternativeClientRegion acr(client);
        auto opCtxHolder = ocg.makeOperationContext(cc());
        promise.emplaceValue();

        try {
            _tailer.run(opCtxHolder, Timestamp::min(), threadBuffer);
        } catch (const DBException& ex) {
            threadStatus = ex.toStatus();
        }
    });

    std::move(future).get();
    ocg.interrupt(ErrorCodes::Interrupted);
    t.join();

    ASSERT_EQ(threadStatus.code(), ErrorCodes::Interrupted);
}

}  // namespace
}  // namespace mongo::replicated_fast_count
