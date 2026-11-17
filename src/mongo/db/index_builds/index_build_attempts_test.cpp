// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/index_builds/index_build_attempts.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/client.h"
#include "mongo/db/index_builds/primary_driven/util.h"
#include "mongo/db/index_builds/resumable_index_builds_common.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/server_recovery.h"
#include "mongo/db/service_context.h"
#include "mongo/db/shard_role/lock_manager/d_concurrency.h"
#include "mongo/db/shard_role/shard_catalog/catalog_test_fixture.h"
#include "mongo/db/shard_role/shard_catalog/collection_catalog.h"
#include "mongo/db/shard_role/shard_catalog/collection_options.h"
#include "mongo/db/shard_role/transaction_resources.h"
#include "mongo/db/storage/control/journal_flusher.h"
#include "mongo/db/storage/ident.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/platform/atomic.h"
#include "mongo/stdx/thread.h"
#include "mongo/unittest/server_parameter_guard.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/log_and_backoff.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/time_support.h"

#include <functional>

namespace mongo::index_builds::build_attempts::two_phase {
namespace {

const auto& kNss = NamespaceString::kLocalIndexBuildsNamespace;
constexpr int kLimit = 2;

bool waitFor(const std::function<bool()>& pred) {
    const auto deadline = Date_t::now() + Seconds(10);
    for (size_t attempts = 1; !pred() && Date_t::now() < deadline; ++attempts) {
        logAndBackoff(13756103,
                      logv2::LogComponent::kIndex,
                      logv2::LogSeverity::Debug(1),
                      attempts,
                      "Waiting for the condition");
    }
    return pred();
}

class IndexBuildAttemptsTest : public CatalogTestFixture {
protected:
    unittest::ServerParameterGuard _limit{"maxIndexBuildAttemptsAfterUncleanShutdown", kLimit};

    void setUp() override {
        CatalogTestFixture::setUp();
        createCollection(operationContext());
    }

    void preseed(const UUID& buildUUID, int attempts) {
        ASSERT_OK(storageInterface()->putSingleton(
            operationContext(),
            kNss,
            BSON("_id" << buildUUID),
            {BSON("$set" << BSON("numAttemptsAfterUncleanShutdown" << attempts)), Timestamp()}));
    }
};

TEST_F(IndexBuildAttemptsTest, AbortsAfterLimitUncleanShutdowns) {
    const auto buildUUID = UUID::gen();
    EXPECT_TRUE(checkWithinAttemptLimit(operationContext(), buildUUID));
    for (int i = 0; i < kLimit; ++i) {
        incrementAttempts(operationContext());
        EXPECT_TRUE(checkWithinAttemptLimit(operationContext(), buildUUID));
    }
    EXPECT_EQ(kLimit, read(operationContext(), buildUUID));

    incrementAttempts(operationContext());
    EXPECT_FALSE(checkWithinAttemptLimit(operationContext(), buildUUID));
    EXPECT_EQ(kLimit + 1, read(operationContext(), buildUUID));
}

TEST_F(IndexBuildAttemptsTest, UncleanShutdownCountsEveryUnfinishedBuild) {
    // Builds that come back through oplog replay never reach startup recovery, so startup counts
    // every entry rather than only the builds it restarts.
    const auto first = UUID::gen();
    const auto second = UUID::gen();
    preseed(first, 0);
    preseed(second, 1);
    incrementAttempts(operationContext());
    EXPECT_EQ(1, read(operationContext(), first));
    EXPECT_EQ(2, read(operationContext(), second));
}

TEST_F(IndexBuildAttemptsTest, BuildWaitsForNewEntryToBeDurable) {
    // The first attempt must be durable before the build scans, since the scan may crash the node.
    const auto buildUUID = UUID::gen();
    auto* flusher = JournalFlusher::get(getServiceContext());
    flusher->pause();
    ScopeGuard resumeFlusher([&] { flusher->resume(); });

    auto client = getServiceContext()->getService()->makeClient("build");
    auto opCtx = client->makeOperationContext();
    Atomic<bool> returned{false};
    stdx::thread build([&] {
        EXPECT_TRUE(checkWithinAttemptLimit(opCtx.get(), buildUUID));
        returned.store(true);
    });
    ASSERT_TRUE(waitFor([&] { return opCtx->isWaitingForConditionOrInterrupt(); }));
    EXPECT_FALSE(returned.load());

    resumeFlusher.dismiss();
    flusher->resume();
    build.join();
    EXPECT_EQ(0, read(operationContext(), buildUUID));
}

TEST_F(IndexBuildAttemptsTest, BuildWaitsForStartupIncrementToBeDurable) {
    // Startup holds the global write lock while counting, so it doesn't wait on the journal
    // flusher. The build makes the increment durable before it runs.
    const auto buildUUID = UUID::gen();
    preseed(buildUUID, 0);
    auto* flusher = JournalFlusher::get(getServiceContext());
    flusher->pause();
    ScopeGuard resumeFlusher([&] { flusher->resume(); });

    incrementAttempts(operationContext());
    EXPECT_EQ(1, read(operationContext(), buildUUID));

    auto client = getServiceContext()->getService()->makeClient("build");
    auto opCtx = client->makeOperationContext();
    Atomic<bool> returned{false};
    stdx::thread build([&] {
        EXPECT_TRUE(checkWithinAttemptLimit(opCtx.get(), buildUUID));
        returned.store(true);
    });
    ASSERT_TRUE(waitFor([&] { return opCtx->isWaitingForConditionOrInterrupt(); }));
    EXPECT_FALSE(returned.load());

    resumeFlusher.dismiss();
    flusher->resume();
    build.join();
    EXPECT_EQ(1, read(operationContext(), buildUUID));
}

TEST_F(IndexBuildAttemptsTest, RemoveEntryWithoutCountIsNoop) {
    // A build can commit or abort before it records its first attempt.
    const auto buildUUID = UUID::gen();
    {
        WriteUnitOfWork wuow(operationContext());
        removeEntry(operationContext(), buildUUID);
        wuow.commit();
    }
    EXPECT_EQ(boost::none, read(operationContext(), buildUUID));
}

TEST_F(IndexBuildAttemptsTest, RemoveEntryDeletesCountWithCallersCommit) {
    // The count is deleted in the commit or abort's write, so it's kept if that write fails.
    const auto buildUUID = UUID::gen();
    EXPECT_TRUE(checkWithinAttemptLimit(operationContext(), buildUUID));
    {
        WriteUnitOfWork wuow(operationContext());
        removeEntry(operationContext(), buildUUID);
    }
    EXPECT_EQ(0, read(operationContext(), buildUUID));

    {
        WriteUnitOfWork wuow(operationContext());
        removeEntry(operationContext(), buildUUID);
        wuow.commit();
    }
    EXPECT_EQ(boost::none, read(operationContext(), buildUUID));
}

TEST_F(IndexBuildAttemptsTest, RecoveryDeleteUpdatesNumRecords) {
    // The ledger isn't rolled back to the stable timestamp, so deletes replayed during recovery
    // remove documents that are counted in the size storer.
    const auto buildUUID = UUID::gen();
    EXPECT_TRUE(checkWithinAttemptLimit(operationContext(), buildUUID));
    trackSizeThroughRecovery(operationContext());
    {
        InReplicationRecovery inReplicationRecovery(getServiceContext());
        WriteUnitOfWork wuow(operationContext());
        removeEntry(operationContext(), buildUUID);
        wuow.commit();
    }
    const auto collection = CollectionCatalog::get(operationContext())
                                ->lookupCollectionByNamespace(operationContext(), kNss);
    EXPECT_EQ(0, collection->numRecords(operationContext()));
}

}  // namespace
}  // namespace mongo::index_builds::build_attempts::two_phase

namespace mongo::index_builds::build_attempts::primary_driven {
namespace {

constexpr int kLimit = 2;

class PrimaryDrivenIndexBuildAttemptsTest : public CatalogTestFixture {
protected:
    const NamespaceString kNss =
        NamespaceString::createNamespaceString_forTest("test.index_build_attempts");

    void setUp() override {
        CatalogTestFixture::setUp();
        static_cast<repl::ReplicationCoordinatorMock*>(
            repl::ReplicationCoordinator::get(getServiceContext()))
            ->alwaysAllowWrites(true);
    }

    void tearDown() override {
        index_builds::primary_driven::registry(getServiceContext()).clear();
        CatalogTestFixture::tearDown();
    }

    ResumeIndexInfo setUpRegisteredBuild(boost::optional<int> count) {
        ResumeIndexInfo info;
        info.setBuildUUID(UUID::gen());
        info.setPhase(IndexBuildPhaseEnum::kCollectionScan);
        info.setCollectionUUID(UUID::gen());
        info.setResumesWithoutProgress(count);
        info.setIndexes({});

        CollectionOptions options;
        options.uuid = info.getCollectionUUID();
        uassertStatusOK(storageInterface()->createCollection(operationContext(), kNss, options));
        index_builds::primary_driven::registry(getServiceContext())
            .add(info.getBuildUUID(),
                 kNss.dbName(),
                 info.getCollectionUUID(),
                 {},
                 ident::generateNewIndexBuildIdent(info.getBuildUUID()));
        return info;
    }

    std::unique_ptr<RecordStore> makeResumeTable(const std::string& ident,
                                                 boost::optional<BSONObj> metadata) {
        auto opCtx = operationContext();
        Lock::GlobalLock lk(opCtx, MODE_IX);
        WriteUnitOfWork wuow(opCtx);
        auto table = getServiceContext()->getStorageEngine()->makeInternalRecordStore(
            opCtx, ident, KeyFormat::Long);
        if (metadata) {
            ASSERT_OK(table->insertRecord(opCtx,
                                          *shard_role_details::getRecoveryUnit(opCtx),
                                          metadata->objdata(),
                                          metadata->objsize(),
                                          Timestamp()));
        }
        wuow.commit();
        return table;
    }

    unittest::ServerParameterGuard _limit{"maxIndexBuildAttemptsAfterUncleanShutdown", kLimit};
    unittest::ServerParameterGuard _containerWrites{"featureFlagContainerWrites", true};
    unittest::ServerParameterGuard _primaryDriven{"featureFlagPrimaryDrivenIndexBuilds", true};
};

TEST_F(PrimaryDrivenIndexBuildAttemptsTest, CheckRewritesMetadataRecord) {
    auto opCtx = operationContext();
    auto info = setUpRegisteredBuild(boost::none);
    auto ident = ident::generateNewIndexBuildIdent(info.getBuildUUID());
    auto table = makeResumeTable(ident, info.getMetadata().toBSON());

    // The first resume records 0; resuming again without progress increments the stored count.
    for (const int count : {0, 1}) {
        SCOPED_TRACE(count);
        ASSERT_TRUE(checkWithinAttemptLimit(opCtx,
                                            kNss.dbName(),
                                            info.getBuildUUID(),
                                            info.getCollectionUUID(),
                                            ident,
                                            info,
                                            false));
        EXPECT_EQ(count, info.getResumesWithoutProgress());

        shard_role_details::getRecoveryUnit(opCtx)->abandonSnapshot();
        auto cursor = table->getCursor(opCtx, *shard_role_details::getRecoveryUnit(opCtx));
        auto record = cursor->next();
        ASSERT_TRUE(record);
        EXPECT_EQ(RecordId(index_builds::primary_driven::metadataKey), record->id);
        auto written = IndexBuildMetadata::parse(record->data.toBson(),
                                                 IDLParserContext("IndexBuildMetadata"));
        EXPECT_EQ(info.getBuildUUID(), written.getBuildUUID());
        EXPECT_EQ(info.getCollectionUUID(), written.getCollectionUUID());
        EXPECT_EQ(IndexBuildPhaseEnum::kCollectionScan, written.getPhase());
        EXPECT_EQ(count, written.getResumesWithoutProgress());
        info.setMetadata(written);
    }
}

TEST_F(PrimaryDrivenIndexBuildAttemptsTest, CheckSkipsMissingMetadataRecord) {
    auto opCtx = operationContext();
    auto info = setUpRegisteredBuild(boost::none);
    auto ident = ident::generateNewIndexBuildIdent(info.getBuildUUID());
    auto table = makeResumeTable(ident, boost::none);

    EXPECT_TRUE(checkWithinAttemptLimit(
        opCtx, kNss.dbName(), info.getBuildUUID(), info.getCollectionUUID(), ident, info, false));
    EXPECT_FALSE(info.getResumesWithoutProgress());

    shard_role_details::getRecoveryUnit(opCtx)->abandonSnapshot();
    EXPECT_FALSE(table->getCursor(opCtx, *shard_role_details::getRecoveryUnit(opCtx))->next());
}

}  // namespace
}  // namespace mongo::index_builds::build_attempts::primary_driven
