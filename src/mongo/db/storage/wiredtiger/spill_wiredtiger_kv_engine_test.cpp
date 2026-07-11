// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/db/storage/wiredtiger/spill_wiredtiger_kv_engine.h"

#include "mongo/base/init.h"  // IWYU pragma: keep
#include "mongo/db/rss/replicated_storage_service.h"
#include "mongo/db/service_context.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_extensions.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_global_options_gen.h"
#include "mongo/idl/server_parameter_test_util.h"
#include "mongo/unittest/barrier.h"
#include "mongo/unittest/join_thread.h"
#include "mongo/unittest/temp_dir.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/clock_source_mock.h"

#include <string_view>

namespace mongo {

namespace {
using namespace std::literals::string_view_literals;
// Make this test suite robust against changes to WT default open options.
static constexpr auto kWiredTigerAllocationSize = 4096;
static constexpr auto kExtraOpenOptions = "allocation_size=4K";
static constexpr auto kWtCacheSizeBytes = 1 * 1024 * 1024;

static constexpr const char* kCollectionIdent{"collection-a-b"};

#if __has_feature(address_sanitizer)
constexpr bool memLeakAllowed = false;
#else
constexpr bool memLeakAllowed = true;
#endif
class SpillWiredTigerKVEngineTest : public ServiceContextTest {
protected:
    SpillWiredTigerKVEngineTest() : _dbpath("wt_test"), _opCtx(makeOperationContext()) {}

    std::unique_ptr<SpillWiredTigerKVEngine> makeKvEngine() {
        WiredTigerKVEngineBase::WiredTigerConfig wtConfig =
            getSpillWiredTigerConfigFromStartupOptions();
        wtConfig.cacheSizeMB = kWtCacheSizeBytes / (1 * 1024 * 1024);
        wtConfig.evictionDirtyTargetMB =
            gSpillWiredTigerEvictionDirtyTargetPercentage * wtConfig.cacheSizeMB / 100;
        wtConfig.evictionDirtyTriggerMB =
            gSpillWiredTigerEvictionDirtyTriggerPercentage * wtConfig.cacheSizeMB / 100;
        wtConfig.evictionUpdatesTriggerMB =
            gSpillWiredTigerEvictionUpdatesTriggerPercentage * wtConfig.cacheSizeMB / 100;
        auto kvEngine = std::make_unique<SpillWiredTigerKVEngine>(
            std::string{kWiredTigerEngineName},
            _dbpath.path(),
            &_clockSource,
            std::move(wtConfig),
            SpillWiredTigerExtensions::get(_opCtx->getServiceContext()));

        kvEngine->setRecordStoreExtraOptions(kExtraOpenOptions);
        return kvEngine;
    }

    unittest::TempDir _dbpath;
    ClockSourceMock _clockSource;
    ServiceContext::UniqueOperationContext _opCtx;
};

TEST_F(SpillWiredTigerKVEngineTest, IdentExistsForRecordStore) {
    auto kvEngine = makeKvEngine();
    auto ru = kvEngine->newRecoveryUnit();
    auto rs = kvEngine->makeInternalRecordStore(*ru, kCollectionIdent, KeyFormat::Long);
    ASSERT_TRUE(kvEngine->hasIdent(*ru, kCollectionIdent));
    kvEngine->cleanShutdown(memLeakAllowed);
}

TEST_F(SpillWiredTigerKVEngineTest, StorageSize) {
    // This value is guaranteed to use disk.
    auto targetStorageSizeBytes = kWtCacheSizeBytes + kWiredTigerAllocationSize + 1;
    auto data = std::string(targetStorageSizeBytes, 'x');
    auto kvEngine = makeKvEngine();
    auto ru = kvEngine->newRecoveryUnit();
    auto originalStorageSizeBytes = kvEngine->storageSize(*ru);

    auto rs = kvEngine->makeInternalRecordStore(*ru, kCollectionIdent, KeyFormat::Long);

    ru->beginUnitOfWork(false);
    RecordId recordId(1);
    auto statusWith =
        rs->insertRecord(_opCtx.get(), *ru, recordId, data.c_str(), data.size(), Timestamp{1});
    ASSERT_OK(statusWith.getStatus());
    ru->commitUnitOfWork();

    auto newDiskUsageBytes = kvEngine->storageSize(*ru);
    ASSERT_GT(newDiskUsageBytes, originalStorageSizeBytes);
    kvEngine->cleanShutdown(memLeakAllowed);
}

TEST_F(SpillWiredTigerKVEngineTest, InFlightDrop) {
    FailPoint* fp = globalFailPointRegistry().find("hangOnSpillWiredTigerKVEngineCleanShutdown"sv);
    const auto initialTimesEntered = fp->setMode(FailPoint::alwaysOn);
    std::unique_ptr<KVEngine> kvEngine = makeKvEngine();
    auto ru = kvEngine->newRecoveryUnit();

    unittest::JoinThread shutterDowner([&] { kvEngine->cleanShutdown(memLeakAllowed); });

    fp->waitForTimesEntered(initialTimesEntered + 1);

    const std::string ident{kCollectionIdent};
    EXPECT_EQ(ErrorCodes::ShutdownInProgress,
              kvEngine->dropIdent(*ru,
                                  ident,
                                  /*identHasSizeInfo=*/false));

    fp->setMode(FailPoint::off);
}

TEST_F(SpillWiredTigerKVEngineTest, BusyInFlightDrop) {
    // This test does not use a failpoint, but will attempt to spin up and drop multiple idents at
    // the same time while racing with a call to cleanShutdown. This test is non-deterministic and
    // may pass even in a race condition state, however TSAN builds should be able to detect and
    // fault upon a race condition reemerging where the threads are competing for the same memory
    // update.
    // The on-disk files will re-load idents upon spinning up the kvEngine due to use of
    // cleanShutdown, so the ident must be unique for each pass.
    static constexpr size_t countIdents{32};
    std::unique_ptr<KVEngine> kvEngine = makeKvEngine();
    unittest::Barrier bar(countIdents + 1);
    std::vector<unittest::JoinThread> spillers;
    spillers.reserve(countIdents);
    Atomic<int> identIdx{0};
    std::generate_n(std::back_inserter(spillers), countIdents, [&] {
        return unittest::JoinThread([&] {
            const std::string ident = fmt::format("collection-{}", identIdx.fetchAndAdd(1));
            auto ru = kvEngine->newRecoveryUnit();
            auto rs = kvEngine->makeInternalRecordStore(*ru, ident, KeyFormat::Long);
            bar.countDownAndWait();
            // This is an ASSERT_THAT as failures may be frequent and clutter
            // results if EXPECT_THAT is used instead.
            ASSERT_THAT(kvEngine->dropIdent(*ru,
                                            ident,
                                            /*identHasSizeInfo=*/false),
                        testing::AnyOf(ErrorCodes::OK, ErrorCodes::ShutdownInProgress));
        });
    });

    unittest::JoinThread shutterDowner([&] {
        bar.countDownAndWait();
        kvEngine->cleanShutdown(memLeakAllowed);
    });
}

TEST_F(SpillWiredTigerKVEngineTest, InFlightTruncation) {
    FailPoint* fp = globalFailPointRegistry().find("hangOnSpillWiredTigerKVEngineCleanShutdown"sv);
    const auto initialTimesEntered = fp->setMode(FailPoint::alwaysOn);
    auto kvEngine = makeKvEngine();
    auto ru = kvEngine->newRecoveryUnit();

    const std::string ident{kCollectionIdent};
    auto rs = kvEngine->makeInternalRecordStore(*ru, ident, KeyFormat::Long);

    unittest::JoinThread shutterDowner([&] { kvEngine->cleanShutdown(memLeakAllowed); });

    fp->waitForTimesEntered(initialTimesEntered + 1);

    EXPECT_EQ(ErrorCodes::ShutdownInProgress, rs->truncate(_opCtx.get(), *ru).code());

    fp->setMode(FailPoint::off);
}

TEST_F(SpillWiredTigerKVEngineTest, BusyInFlightTruncation) {
    // This test does not use a failpoint, but will attempt to spin up and drop multiple idents at
    // the same time while racing with a call to cleanShutdown. This test is non-deterministic and
    // may pass even in a race condition state, however TSAN builds should be able to detect and
    // fault upon a race condition reemerging where the threads are competing for the same memory
    // update.
    // The on-disk files will re-load idents upon spinning up the kvEngine due to use of
    // cleanShutdown, so the ident must be unique for each pass.
    static constexpr size_t countIdents{32};
    unittest::Barrier bar(countIdents + 1);
    auto kvEngine = makeKvEngine();
    std::vector<unittest::JoinThread> spillers;
    spillers.reserve(countIdents);
    Atomic<int> identIdx{0};
    std::generate_n(std::back_inserter(spillers), countIdents, [&] {
        return unittest::JoinThread([&] {
            const std::string ident = fmt::format("collection-{}", identIdx.fetchAndAdd(1));
            auto ru = kvEngine->newRecoveryUnit();
            auto rs = kvEngine->makeInternalRecordStore(*ru, ident, KeyFormat::Long);
            bar.countDownAndWait();
            // This is an ASSERT_THAT as failures may be frequent and clutter
            // results if EXPECT_THAT is used instead.
            ASSERT_THAT(rs->truncate(_opCtx.get(), *ru).code(),
                        testing::AnyOf(ErrorCodes::OK, ErrorCodes::ShutdownInProgress));
        });
    });

    unittest::JoinThread shutterDowner([&] {
        bar.countDownAndWait();
        kvEngine->cleanShutdown(memLeakAllowed);
    });
}

TEST_F(SpillWiredTigerKVEngineTest, SetIsolationMidShutdown) {
    FailPoint* fp = globalFailPointRegistry().find("hangOnSpillWiredTigerKVEngineCleanShutdown"sv);
    const auto initialTimesEntered = fp->setMode(FailPoint::alwaysOn);
    auto kvEngine = makeKvEngine();
    auto ru = kvEngine->newRecoveryUnit();

    auto rs = kvEngine->makeInternalRecordStore(*ru, kCollectionIdent, KeyFormat::Long);
    // makeInternalRecordStore activates the RU via the WiredTigerRecordStore ctor's
    // ru.getSession() call. Mirror SpillTable's ctor so setIsolation's
    // tassert(!isActive()) doesn't fire.
    ru->abandonSnapshot();

    ru->setIsolation(RecoveryUnit::Isolation::readCommitted);

    unittest::JoinThread shutterDowner([&] { kvEngine->cleanShutdown(memLeakAllowed); });
    fp->waitForTimesEntered(initialTimesEntered + 1);

    // The BlockShutdown guard in _setIsolation must skip the WT_SESSION::reconfigure
    // call rather than racing the connection close. Under ASAN/TSAN, an unguarded
    // variant would be detected as a use-after-free here.
    ru->setIsolation(RecoveryUnit::Isolation::snapshot);

    fp->setMode(FailPoint::off);
}

TEST_F(SpillWiredTigerKVEngineTest, IdentStatusesMidShutdown) {
    FailPoint* fp = globalFailPointRegistry().find("hangOnSpillWiredTigerKVEngineCleanShutdown"sv);
    const auto initialTimesEntered = fp->setMode(FailPoint::alwaysOn);
    auto kvEngine = makeKvEngine();
    auto ru = kvEngine->newRecoveryUnit();

    // Create a record store and assert basic ident properties on the KV Engine
    auto rs = kvEngine->makeInternalRecordStore(*ru, kCollectionIdent, KeyFormat::Long);
    EXPECT_TRUE(kvEngine->hasIdent(*ru, kCollectionIdent));
    EXPECT_GT(kvEngine->getIdentSize(*ru, kCollectionIdent), 0);
    EXPECT_FALSE(kvEngine->getAllIdents(*ru).empty());

    // Enter shutdown and check shutdown-in-flight properties
    unittest::JoinThread shutterDowner([&] { kvEngine->cleanShutdown(memLeakAllowed); });
    fp->waitForTimesEntered(initialTimesEntered + 1);
    EXPECT_FALSE(kvEngine->hasIdent(*ru, kCollectionIdent))
        << "Expected in-flight shutdown to cause hasIdent to return false";
    EXPECT_EQ(kvEngine->getIdentSize(*ru, kCollectionIdent), 0)
        << "Expected in-flight shutdown to cause getIdentSize to return 0";
    EXPECT_TRUE(kvEngine->getAllIdents(*ru).empty())
        << "Expected in-flight shutdown to cause getAllIdents to be an empty container";
    EXPECT_THROW(std::ignore =
                     kvEngine->makeInternalRecordStore(*ru, "collection-c-d", KeyFormat::Long),
                 ExceptionFor<ErrorCodes::ShutdownInProgress>)
        << "Expected recordStore creation to fail while shutdown is in progress";
    fp->setMode(FailPoint::off);
}

}  // namespace

}  // namespace mongo
