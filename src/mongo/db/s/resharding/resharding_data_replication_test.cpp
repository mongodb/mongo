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

#include "mongo/db/s/resharding/resharding_data_replication.h"

#include "mongo/base/counter.h"
#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/oid.h"
#include "mongo/crypto/encryption_fields_gen.h"
#include "mongo/db/basic_types_gen.h"
#include "mongo/db/client.h"
#include "mongo/db/collection_crud/collection_write_path.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/global_catalog/chunk_manager.h"
#include "mongo/db/global_catalog/type_chunk.h"
#include "mongo/db/global_catalog/type_collection_common_types_gen.h"
#include "mongo/db/local_catalog/clustered_collection_options_gen.h"
#include "mongo/db/local_catalog/collection.h"
#include "mongo/db/local_catalog/collection_options.h"
#include "mongo/db/local_catalog/lock_manager/exception_util.h"
#include "mongo/db/local_catalog/lock_manager/lock_manager_defs.h"
#include "mongo/db/persistent_task_store.h"
#include "mongo/db/query/collation/collator_factory_interface.h"
#include "mongo/db/query/collation/collator_factory_mock.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/query/collation/collator_interface_mock.h"
#include "mongo/db/repl/member_state.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/db/repl/oplog_entry_gen.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/s/resharding/resharding_data_copy_util.h"
#include "mongo/db/s/resharding/resharding_oplog_applier_progress_gen.h"
#include "mongo/db/s/resharding/resharding_util.h"
#include "mongo/db/service_context.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/db/timeseries/timeseries_gen.h"
#include "mongo/db/versioning_protocol/chunk_version.h"
#include "mongo/db/versioning_protocol/database_version.h"
#include "mongo/logv2/log.h"
#include "mongo/s/resharding/type_collection_fields_gen.h"
#include "mongo/stdx/thread.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/duration.h"
#include "mongo/util/timer.h"

#include <ostream>
#include <string>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo {
namespace {

class ReshardingDataReplicationTest : service_context_test::WithSetupTransportLayer,
                                      public ServiceContextMongoDTest {
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
            ChunkVersion({epoch, Timestamp(1, 1)}, {100, 0}),
            _myDonorId}};

        auto rt = RoutingTableHistory::makeNew(_sourceNss,
                                               _sourceUUID,
                                               BSON(_currentShardKey << 1),
                                               false, /* unsplittable */
                                               std::move(defaultCollator),
                                               false /* unique */,
                                               epoch,
                                               Timestamp(1, 1),
                                               boost::none /* timeseriesFields */,
                                               boost::none /* reshardingFields */,
                                               true /* allowMigrations */,
                                               chunks);

        return ChunkManager(makeStandaloneRoutingTableHistory(std::move(rt)),
                            boost::none /* clusterTime */);
    }

    DonorShardFetchTimestamp makeDonorShardFetchTimestamp(ShardId shardId,
                                                          Timestamp fetchTimestamp) {
        DonorShardFetchTimestamp donorFetchTimestamp(shardId);
        donorFetchTimestamp.setMinFetchTimestamp(fetchTimestamp);
        return donorFetchTimestamp;
    }

    ReshardingRecipientDocument makeRecipientStateDocument(
        std::vector<DonorShardFetchTimestamp> donorShardTimestamps, Timestamp cloneTimestamp) {
        RecipientShardContext recipientCtx;
        recipientCtx.setState(RecipientStateEnum::kAwaitingFetchTimestamp);

        ReshardingRecipientDocument doc(std::move(recipientCtx),
                                        donorShardTimestamps,
                                        durationCount<Milliseconds>(Milliseconds{5}));

        auto commonMetadata = CommonReshardingMetadata(
            UUID::gen(), sourceNss(), sourceUUID(), outputNss(), {BSON(_currentShardKey << 1)});
        commonMetadata.setStartTime(getServiceContext()->getFastClockSource()->now());

        doc.setCommonReshardingMetadata(std::move(commonMetadata));
        doc.setCloneTimestamp(cloneTimestamp);
        return doc;
    }

    const NamespaceString& sourceNss() {
        return _sourceNss;
    }

    const UUID& sourceUUID() {
        return _sourceUUID;
    }

    const NamespaceString& outputNss() {
        return _outputNss;
    }

protected:
    /**
     * Tests that making ReshardingOplogReplication creates the oplog fetcher progress collection
     * if 'storeOplogFetcherProgress' is set to true, and does not do so otherwise.
     */
    void testCreateOplogFetcherProgressCollection(bool storeOplogFetcherProgress) {
        CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
        auto sourceChunkMgr = makeChunkManagerForSourceCollection(collator.clone());

        auto cloneTimestamp = Timestamp(5, 0);
        ShardId shardId0{"shard0"};
        auto minFetchTimestamp0 = Timestamp{10, 0};
        auto myShardId = shardId0;
        ShardId shardId1{"shard1"};
        auto minFetchTimestamp1 = Timestamp{12, 0};

        std::vector<DonorShardFetchTimestamp> donorFetchTimestamps = {
            makeDonorShardFetchTimestamp(shardId0, minFetchTimestamp0),
            makeDonorShardFetchTimestamp(shardId1, minFetchTimestamp1)};
        auto recipientDoc = makeRecipientStateDocument(donorFetchTimestamps, cloneTimestamp);

        auto reshardingMetrics =
            ReshardingMetrics::initializeFrom(recipientDoc, getServiceContext());

        ReshardingApplierMetricsMap applierMetricsMap;
        for (const auto& donor : donorFetchTimestamps) {
            applierMetricsMap.emplace(
                donor.getShardId(),
                std::make_unique<ReshardingOplogApplierMetrics>(
                    donor.getShardId(), reshardingMetrics.get(), boost::none));
        }

        auto opCtx = makeOperationContext();
        create(opCtx.get(), outputNss());
        auto dataReplication =
            ReshardingDataReplication::make(opCtx.get(),
                                            reshardingMetrics.get(),
                                            &applierMetricsMap,
                                            1 /* oplogBatchTaskCount */,
                                            recipientDoc.getCommonReshardingMetadata(),
                                            recipientDoc.getDonorShards(),
                                            *recipientDoc.getCloneTimestamp(),
                                            true /* cloningDone */,
                                            myShardId,
                                            sourceChunkMgr,
                                            storeOplogFetcherProgress,
                                            false /* relaxed */);

        const auto oplopFetcherProgressColl =
            acquireCollection(opCtx.get(),
                              CollectionAcquisitionRequest::fromOpCtx(
                                  opCtx.get(),
                                  NamespaceString::kReshardingFetcherProgressNamespace,
                                  AcquisitionPrerequisites::kRead),
                              MODE_IS);
        if (storeOplogFetcherProgress) {
            // The progress collection should have been created but it should not have any
            // documents.
            ASSERT(oplopFetcherProgressColl.exists());
            ASSERT(oplopFetcherProgressColl.getCollectionPtr()->isEmpty(opCtx.get()));
        } else {
            ASSERT(!oplopFetcherProgressColl.exists());
        }
    }

private:
    void create(OperationContext* opCtx, NamespaceString nss) {
        writeConflictRetry(opCtx, "create", nss, [&] {
            AllowLockAcquisitionOnTimestampedUnitOfWork allowLockAcquisition(
                shard_role_details::getLocker(opCtx));
            AutoGetDb autoDb(opCtx, nss.dbName(), LockMode::MODE_X);
            WriteUnitOfWork wunit(opCtx);
            if (shard_role_details::getRecoveryUnit(opCtx)->getCommitTimestamp().isNull()) {
                ASSERT_OK(
                    shard_role_details::getRecoveryUnit(opCtx)->setTimestamp(Timestamp(1, 1)));
            }

            OperationShardingState::ScopedAllowImplicitCollectionCreate_UNSAFE
                unsafeCreateCollection(opCtx, nss);
            auto db = autoDb.ensureDbExists(opCtx);
            ASSERT(db->createCollection(opCtx, nss)) << nss.toStringForErrorMsg();
            wunit.commit();
        });
    }

    RoutingTableHistoryValueHandle makeStandaloneRoutingTableHistory(RoutingTableHistory rt) {
        const auto version = rt.getVersion();
        return RoutingTableHistoryValueHandle(
            std::make_shared<RoutingTableHistory>(std::move(rt)),
            ComparableChunkVersion::makeComparableChunkVersion(version));
    }

    const StringData _currentShardKey = "currentShardKey";
    const StringData _newShardKey = "newShardKey";

    const NamespaceString _sourceNss =
        NamespaceString::createNamespaceString_forTest("testDb", "testColl");
    const UUID _sourceUUID = UUID::gen();

    const NamespaceString _outputNss =
        resharding::constructTemporaryReshardingNss(sourceNss(), sourceUUID());

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
        const auto stashColl =
            acquireCollection(opCtx.get(),
                              CollectionAcquisitionRequest::fromOpCtx(
                                  opCtx.get(), nss, AcquisitionPrerequisites::kRead),
                              MODE_IS);
        ASSERT_TRUE(bool(stashColl.getCollectionPtr()->getDefaultCollator()))
            << "Stash collection was created with 'simple' collation";
        ASSERT_BSONOBJ_BINARY_EQ(
            stashColl.getCollectionPtr()->getDefaultCollator()->getSpec().toBSON(),
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
            ThreadClient threadClient(getGlobalServiceContext()->getService());
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
                    const auto stashColl =
                        acquireCollection(opCtx.get(),
                                          CollectionAcquisitionRequest::fromOpCtx(
                                              opCtx.get(), nss, AcquisitionPrerequisites::kRead),
                                          MODE_IS);
                    ASSERT_TRUE(bool(stashColl.getCollectionPtr()->getDefaultCollator()))
                        << "Stash collection was created with 'simple' collation";
                    ASSERT_BSONOBJ_BINARY_EQ(
                        stashColl.getCollectionPtr()->getDefaultCollator()->getSpec().toBSON(),
                        collator.getSpec().toBSON());
                }
            }
        });
        threads.push_back(std::move(thread));
    }
    for (auto& t : threads) {
        t.join();
    }

    LOGV2(5930702, "Stress test completed", "iterations"_attr = iterations.get());
}

TEST_F(ReshardingDataReplicationTest, GetOplogFetcherResumeId) {
    auto opCtx = makeOperationContext();

    const auto reshardingUUID = UUID::gen();
    auto oplogBufferNss = resharding::getLocalOplogBufferNamespace(reshardingUUID, {"shard0"});

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

        const auto oplogBufferColl = acquireCollection(
            opCtx.get(),
            CollectionAcquisitionRequest{oplogBufferNss,
                                         PlacementConcern{boost::none, ShardVersion::UNSHARDED()},
                                         repl::ReadConcernArgs::get(opCtx.get()),
                                         AcquisitionPrerequisites::kWrite},
            MODE_IX);
        WriteUnitOfWork wuow(opCtx.get());
        ASSERT_OK(collection_internal::insertDocument(opCtx.get(),
                                                      oplogBufferColl.getCollectionPtr(),
                                                      InsertStatement{oplogEntry.toBSON()},
                                                      nullptr));
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

TEST_F(ReshardingDataReplicationTest, CreateProgressCollection) {
    testCreateOplogFetcherProgressCollection(true /* storeOplogFetcherProgress */);
}

TEST_F(ReshardingDataReplicationTest, NotCreateProgressCollection) {
    testCreateOplogFetcherProgressCollection(false /* storeOplogFetcherProgress */);
}

}  // namespace
}  // namespace mongo
