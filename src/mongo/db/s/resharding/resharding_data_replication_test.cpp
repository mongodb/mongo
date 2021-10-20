/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

#include "mongo/platform/basic.h"

#include <memory>
#include <vector>

#include "mongo/bson/bsonmisc.h"
#include "mongo/db/persistent_task_store.h"
#include "mongo/db/query/collation/collator_factory_mock.h"
#include "mongo/db/query/collation/collator_interface_mock.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/s/resharding/resharding_data_copy_util.h"
#include "mongo/db/s/resharding/resharding_data_replication.h"
#include "mongo/db/s/resharding/resharding_oplog_applier_progress_gen.h"
#include "mongo/db/s/resharding_util.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/logv2/log.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/s/chunk_manager.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

class ReshardingDataReplicationTest : public ServiceContextMongoDTest {
public:
    void setUp() override {
        ServiceContextMongoDTest::setUp();

        auto serviceContext = getServiceContext();
        {
            auto opCtx = makeOperationContext();
            auto replCoord = std::make_unique<repl::ReplicationCoordinatorMock>(serviceContext);
            ASSERT_OK(replCoord->setFollowerMode(repl::MemberState::RS_PRIMARY));
            repl::ReplicationCoordinator::set(serviceContext, std::move(replCoord));

            repl::createOplog(opCtx.get());

            CollatorFactoryInterface::set(serviceContext, std::make_unique<CollatorFactoryMock>());
        }
    }

    ChunkManager makeChunkManagerForSourceCollection(
        std::unique_ptr<CollatorInterface> defaultCollator) {
        const OID epoch = OID::gen();
        std::vector<ChunkType> chunks = {ChunkType{
            _sourceUUID,
            ChunkRange{BSON(_currentShardKey << MINKEY), BSON(_currentShardKey << MAXKEY)},
            ChunkVersion(100, 0, epoch, Timestamp(1, 1)),
            _myDonorId}};

        auto rt = RoutingTableHistory::makeNew(_sourceNss,
                                               _sourceUUID,
                                               BSON(_currentShardKey << 1),
                                               std::move(defaultCollator),
                                               false /* unique */,
                                               std::move(epoch),
                                               Timestamp(1, 1),
                                               boost::none /* timeseriesFields */,
                                               boost::none /* reshardingFields */,
                                               boost::none /* chunkSizeBytes */,
                                               true /* allowMigrations */,
                                               chunks);

        return ChunkManager(_myDonorId,
                            DatabaseVersion(UUID::gen(), Timestamp(1, 1)),
                            makeStandaloneRoutingTableHistory(std::move(rt)),
                            boost::none /* clusterTime */);
    }

    const NamespaceString& sourceNss() {
        return _sourceNss;
    }

    const CollectionUUID& sourceUUID() {
        return _sourceUUID;
    }

private:
    RoutingTableHistoryValueHandle makeStandaloneRoutingTableHistory(RoutingTableHistory rt) {
        const auto version = rt.getVersion();
        return RoutingTableHistoryValueHandle(
            std::make_shared<RoutingTableHistory>(std::move(rt)),
            ComparableChunkVersion::makeComparableChunkVersion(version));
    }

    const StringData _currentShardKey = "sk";

    const NamespaceString _sourceNss{"test_crud", "collection_being_resharded"};
    const CollectionUUID _sourceUUID = UUID::gen();

    const ShardId _myDonorId{"myDonorId"};
};

TEST_F(ReshardingDataReplicationTest, StashCollectionsHaveSameCollationAsReshardingCollection) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    auto sourceChunkMgr = makeChunkManagerForSourceCollection(collator.clone());

    auto stashCollections = [&] {
        auto opCtx = makeOperationContext();
        return ReshardingDataReplication::ensureStashCollectionsExist(
            opCtx.get(),
            sourceChunkMgr,
            {DonorShardFetchTimestamp{{"shard0"}}, DonorShardFetchTimestamp{{"shard1"}}});
    }();

    for (const auto& nss : stashCollections) {
        auto opCtx = makeOperationContext();
        AutoGetCollection stashColl(opCtx.get(), nss, MODE_IS);
        ASSERT_TRUE(bool(stashColl->getDefaultCollator()))
            << "Stash collection was created with 'simple' collation";
        ASSERT_BSONOBJ_BINARY_EQ(stashColl->getDefaultCollator()->getSpec().toBSON(),
                                 collator.getSpec().toBSON());
    }
}

// TODO(SERVER-59325): Remove stress test when no longer needed.
TEST_F(ReshardingDataReplicationTest,
       StashCollectionsHaveSameCollationAsReshardingCollectionStressTest) {
    static constexpr int kThreads = 10;
    std::vector<stdx::thread> threads;
    Counter64 iterations;

    for (int t = 0; t < kThreads; ++t) {
        stdx::thread thread([&]() {
            ThreadClient threadClient(getGlobalServiceContext());
            Timer timer;
            while (timer.elapsed() < Seconds(2)) {
                CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
                auto sourceChunkMgr = makeChunkManagerForSourceCollection(collator.clone());

                auto stashCollections = [&] {
                    auto opCtx = Client::getCurrent()->makeOperationContext();
                    return ReshardingDataReplication::ensureStashCollectionsExist(
                        opCtx.get(),
                        sourceChunkMgr,
                        {DonorShardFetchTimestamp{{"shard0"}},
                         DonorShardFetchTimestamp{{"shard1"}}});
                }();

                for (const auto& nss : stashCollections) {
                    auto opCtx = Client::getCurrent()->makeOperationContext();
                    AutoGetCollection stashColl(opCtx.get(), nss, MODE_IS);
                    ASSERT_TRUE(bool(stashColl->getDefaultCollator()))
                        << "Stash collection was created with 'simple' collation";
                    ASSERT_BSONOBJ_BINARY_EQ(stashColl->getDefaultCollator()->getSpec().toBSON(),
                                             collator.getSpec().toBSON());
                }
            }
        });
        threads.push_back(std::move(thread));
    }
    for (auto& t : threads) {
        t.join();
    }

    LOGV2(5930702, "Stress test completed", "iterations"_attr = iterations);
}

TEST_F(ReshardingDataReplicationTest, GetOplogFetcherResumeId) {
    auto opCtx = makeOperationContext();

    const auto reshardingUUID = UUID::gen();
    auto oplogBufferNss = getLocalOplogBufferNamespace(reshardingUUID, {"shard0"});

    const auto minFetchTimestamp = Timestamp{10, 0};
    const auto oplogId1 = ReshardingDonorOplogId{{20, 0}, {18, 0}};
    const auto oplogId2 = ReshardingDonorOplogId{{20, 0}, {19, 0}};
    const auto oplogId3 = ReshardingDonorOplogId{{20, 0}, {20, 0}};

    // The minFetchTimestamp value is used when the oplog buffer collection doesn't exist.
    ASSERT_BSONOBJ_BINARY_EQ(
        ReshardingDataReplication::getOplogFetcherResumeId(
            opCtx.get(), reshardingUUID, oplogBufferNss, minFetchTimestamp)
            .toBSON(),
        (ReshardingDonorOplogId{minFetchTimestamp, minFetchTimestamp}.toBSON()));

    // The minFetchTimestamp value is used when the oplog buffer collection is empty.
    resharding::data_copy::ensureCollectionExists(opCtx.get(), oplogBufferNss, CollectionOptions{});
    ASSERT_BSONOBJ_BINARY_EQ(
        ReshardingDataReplication::getOplogFetcherResumeId(
            opCtx.get(), reshardingUUID, oplogBufferNss, minFetchTimestamp)
            .toBSON(),
        (ReshardingDonorOplogId{minFetchTimestamp, minFetchTimestamp}.toBSON()));

    auto insertFn = [&](const ReshardingDonorOplogId& oplogId) {
        repl::MutableOplogEntry oplogEntry;
        oplogEntry.setNss({});
        oplogEntry.setOpType(repl::OpTypeEnum::kNoop);
        oplogEntry.setObject({});
        oplogEntry.setOpTime({{}, {}});
        oplogEntry.setWallClockTime({});
        oplogEntry.set_id(Value(oplogId.toBSON()));

        AutoGetCollection oplogBufferColl(opCtx.get(), oplogBufferNss, MODE_IX);
        WriteUnitOfWork wuow(opCtx.get());
        ASSERT_OK(oplogBufferColl->insertDocument(
            opCtx.get(), InsertStatement{oplogEntry.toBSON()}, nullptr));
        wuow.commit();
    };

    insertFn(oplogId2);
    ASSERT_BSONOBJ_BINARY_EQ(ReshardingDataReplication::getOplogFetcherResumeId(
                                 opCtx.get(), reshardingUUID, oplogBufferNss, minFetchTimestamp)
                                 .toBSON(),
                             oplogId2.toBSON());

    // The highest oplog ID is used regardless of the original insertion order.
    insertFn(oplogId3);
    insertFn(oplogId1);
    ASSERT_BSONOBJ_BINARY_EQ(ReshardingDataReplication::getOplogFetcherResumeId(
                                 opCtx.get(), reshardingUUID, oplogBufferNss, minFetchTimestamp)
                                 .toBSON(),
                             oplogId3.toBSON());
}

TEST_F(ReshardingDataReplicationTest, GetOplogApplierResumeId) {
    auto opCtx = makeOperationContext();

    const auto reshardingUUID = UUID::gen();
    const auto minFetchTimestamp = Timestamp{10, 0};
    const ReshardingSourceId sourceId{reshardingUUID, {"shard0"}};

    // The minFetchTimestamp value is used when the applier progress collection doesn't exist.
    ASSERT_BSONOBJ_BINARY_EQ(
        ReshardingDataReplication::getOplogApplierResumeId(opCtx.get(), sourceId, minFetchTimestamp)
            .toBSON(),
        (ReshardingDonorOplogId{minFetchTimestamp, minFetchTimestamp}.toBSON()));

    // The minFetchTimestamp value is used when the applier progress collection is empty.
    resharding::data_copy::ensureCollectionExists(
        opCtx.get(), NamespaceString::kReshardingApplierProgressNamespace, CollectionOptions{});
    ASSERT_BSONOBJ_BINARY_EQ(
        ReshardingDataReplication::getOplogApplierResumeId(opCtx.get(), sourceId, minFetchTimestamp)
            .toBSON(),
        (ReshardingDonorOplogId{minFetchTimestamp, minFetchTimestamp}.toBSON()));

    PersistentTaskStore<ReshardingOplogApplierProgress> store(
        NamespaceString::kReshardingApplierProgressNamespace);

    const auto expectedOplogId = ReshardingDonorOplogId{{20, 0}, {18, 0}};
    store.add(opCtx.get(), ReshardingOplogApplierProgress{sourceId, expectedOplogId, 5});
    ASSERT_BSONOBJ_BINARY_EQ(
        ReshardingDataReplication::getOplogApplierResumeId(opCtx.get(), sourceId, minFetchTimestamp)
            .toBSON(),
        expectedOplogId.toBSON());

    // The progress for the specific ReshardingSourceId is returned.
    store.add(opCtx.get(),
              ReshardingOplogApplierProgress{ReshardingSourceId{reshardingUUID, {"shard1"}},
                                             ReshardingDonorOplogId{{20, 0}, {19, 0}},
                                             6});
    store.add(opCtx.get(),
              ReshardingOplogApplierProgress{ReshardingSourceId{UUID::gen(), {"shard0"}},
                                             ReshardingDonorOplogId{{20, 0}, {20, 0}},
                                             7});
    ASSERT_BSONOBJ_BINARY_EQ(
        ReshardingDataReplication::getOplogApplierResumeId(opCtx.get(), sourceId, minFetchTimestamp)
            .toBSON(),
        expectedOplogId.toBSON());
}

}  // namespace
}  // namespace mongo
