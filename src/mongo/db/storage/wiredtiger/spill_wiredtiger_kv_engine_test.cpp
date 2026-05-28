/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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


#include "mongo/db/storage/wiredtiger/spill_wiredtiger_kv_engine.h"

#include "mongo/base/init.h"  // IWYU pragma: keep
#include "mongo/base/string_data.h"
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

namespace mongo {

namespace {
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

    ~SpillWiredTigerKVEngineTest() override {}

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
    FailPoint* fp = globalFailPointRegistry().find("hangOnSpillWiredTigerKVEngineCleanShutdown"_sd);
    const auto initialTimesEntered = fp->setMode(FailPoint::alwaysOn);
    auto kvEngine = makeKvEngine();
    auto ru = kvEngine->newRecoveryUnit();

    unittest::JoinThread shutterDowner([&] { kvEngine->cleanShutdown(memLeakAllowed); });

    fp->waitForTimesEntered(initialTimesEntered + 1);

    const std::string ident{kCollectionIdent};
    EXPECT_EQ(ErrorCodes::ShutdownInProgress,
              kvEngine
                  ->dropIdent(
                      *ru,
                      ident,
                      /*identHasSizeInfo=*/false,
                      /*onDrop=*/[] {},
                      /*schemaEpoch=*/boost::none)
                  .code());

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
    auto kvEngine = makeKvEngine();
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
            ASSERT_THAT(kvEngine
                            ->dropIdent(
                                *ru,
                                ident,
                                /*identHasSizeInfo=*/false,
                                /*onDrop=*/[] {},
                                /*schemaEpoch=*/boost::none)
                            .code(),
                        testing::AnyOf(ErrorCodes::OK, ErrorCodes::ShutdownInProgress));
        });
    });

    unittest::JoinThread shutterDowner([&] {
        bar.countDownAndWait();
        kvEngine->cleanShutdown(memLeakAllowed);
    });
}

TEST_F(SpillWiredTigerKVEngineTest, InFlightTruncation) {
    FailPoint* fp = globalFailPointRegistry().find("hangOnSpillWiredTigerKVEngineCleanShutdown"_sd);
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
    FailPoint* fp = globalFailPointRegistry().find("hangOnSpillWiredTigerKVEngineCleanShutdown"_sd);
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
    FailPoint* fp = globalFailPointRegistry().find("hangOnSpillWiredTigerKVEngineCleanShutdown"_sd);
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
