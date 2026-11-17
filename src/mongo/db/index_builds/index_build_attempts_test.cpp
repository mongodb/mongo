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

    ResumeIndexInfo setUpRegisteredBuild(size_t indexCount) {
        std::vector<IndexBuildInfo> indexes;
        for (size_t i = 0; i < indexCount; ++i) {
            const auto field = "field_" + std::to_string(i);
            const auto suffix = std::to_string(i);
            indexes.emplace_back(
                BSON("v" << 2 << "key" << BSON(field << 1) << "name" << (field + "_1")),
                "index-" + suffix);
            indexes.back().setInternalIdents("internal-sorter-" + suffix,
                                             "internal-sideWrites-" + suffix,
                                             "internal-skippedRecords-" + suffix,
                                             "internal-constraintViolations-" + suffix);
        }
        auto info = index_builds::synthesizeResumeIndexInfo(
            UUID::gen(), IndexBuildPhaseEnum::kCollectionScan, UUID::gen(), indexes);

        CollectionOptions options;
        options.uuid = info.getCollectionUUID();
        uassertStatusOK(storageInterface()->createCollection(operationContext(), kNss, options));
        index_builds::primary_driven::registry(getServiceContext())
            .add(info.getBuildUUID(),
                 kNss.dbName(),
                 info.getCollectionUUID(),
                 indexes,
                 ident::generateNewIndexBuildIdent(info.getBuildUUID()));
        return info;
    }

    std::unique_ptr<RecordStore> makeResumeTable(const std::string& ident) {
        auto opCtx = operationContext();
        Lock::GlobalLock lk(opCtx, MODE_IX);
        WriteUnitOfWork wuow(opCtx);
        auto table = getServiceContext()->getStorageEngine()->makeInternalRecordStore(
            opCtx, ident, KeyFormat::Long);
        wuow.commit();
        return table;
    }

    ResumeIndexInfo readResumeInfo(const std::string& ident) {
        shard_role_details::getRecoveryUnit(operationContext())->abandonSnapshot();
        auto info = index_builds::readAndParseResumeIndexInfo(
            getServiceContext()->getStorageEngine(), operationContext(), ident);
        ASSERT_TRUE(info);
        return *info;
    }

    unittest::ServerParameterGuard _limit{"maxIndexBuildAttemptsAfterUncleanShutdown", kLimit};
    unittest::ServerParameterGuard _containerWrites{"featureFlagContainerWrites", true};
    unittest::ServerParameterGuard _primaryDriven{"featureFlagPrimaryDrivenIndexBuilds", true};
};

TEST_F(PrimaryDrivenIndexBuildAttemptsTest, CheckPersistsCountWithoutChangingResumeProgress) {
    auto opCtx = operationContext();
    auto info = setUpRegisteredBuild(3);
    auto ident = ident::generateNewIndexBuildIdent(info.getBuildUUID());
    auto table = makeResumeTable(ident);
    auto expected = info;

    // Reload each attempt to verify the resume state and count survive another resume.
    for (int count = 0; count <= kLimit; ++count) {
        SCOPED_TRACE(count);
        EXPECT_TRUE(
            checkWithinAttemptLimit(opCtx, kNss.dbName(), ident, info, /*plannedHandoff*/ false));
        expected.setResumesWithoutProgress(count);
        EXPECT_THAT(info.toBSON(), unittest::match::BSONObjEQ(expected.toBSON()));
        info = readResumeInfo(ident);
        EXPECT_THAT(info.toBSON(), unittest::match::BSONObjEQ(expected.toBSON()));
    }

    EXPECT_FALSE(
        checkWithinAttemptLimit(opCtx, kNss.dbName(), ident, info, /*plannedHandoff*/ false));
    EXPECT_THAT(readResumeInfo(ident).toBSON(), unittest::match::BSONObjEQ(expected.toBSON()));
}

TEST_F(PrimaryDrivenIndexBuildAttemptsTest, CheckPlannedHandoffPreservesCount) {
    auto opCtx = operationContext();
    auto info = setUpRegisteredBuild(2);
    const auto ident = ident::generateNewIndexBuildIdent(info.getBuildUUID());
    auto table = makeResumeTable(ident);

    // Cover initial state insertion and the existing-state no-op without establishing a count.
    const auto initialState = info.toBSON();
    for (int handoff = 0; handoff < 2; ++handoff) {
        SCOPED_TRACE(handoff);
        EXPECT_TRUE(
            checkWithinAttemptLimit(opCtx, kNss.dbName(), ident, info, /*plannedHandoff*/ true));
        EXPECT_THAT(info.toBSON(), unittest::match::BSONObjEQ(initialState));
        EXPECT_THAT(readResumeInfo(ident).toBSON(), unittest::match::BSONObjEQ(initialState));
    }

    // Use a nonzero count to detect accidental resets.
    EXPECT_TRUE(
        checkWithinAttemptLimit(opCtx, kNss.dbName(), ident, info, /*plannedHandoff*/ false));
    EXPECT_TRUE(
        checkWithinAttemptLimit(opCtx, kNss.dbName(), ident, info, /*plannedHandoff*/ false));
    EXPECT_EQ(1, info.getResumesWithoutProgress());
    const auto expected = info;
    EXPECT_TRUE(
        checkWithinAttemptLimit(opCtx, kNss.dbName(), ident, info, /*plannedHandoff*/ true));
    EXPECT_THAT(info.toBSON(), unittest::match::BSONObjEQ(expected.toBSON()));
    EXPECT_THAT(readResumeInfo(ident).toBSON(), unittest::match::BSONObjEQ(expected.toBSON()));
}

}  // namespace
}  // namespace mongo::index_builds::build_attempts::primary_driven
