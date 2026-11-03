// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/index_builds/index_build_attempts.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/client.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/service_context.h"
#include "mongo/db/shard_role/shard_catalog/catalog_test_fixture.h"
#include "mongo/db/storage/control/journal_flusher.h"
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

}  // namespace
}  // namespace mongo::index_builds::build_attempts::two_phase
