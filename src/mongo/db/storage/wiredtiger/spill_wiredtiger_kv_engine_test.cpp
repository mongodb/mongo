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
#include "mongo/unittest/temp_dir.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/clock_source_mock.h"

namespace mongo {

namespace {
// Make this test suite robust against changes to WT default open options.
static constexpr auto kWiredTigerAllocationSize = 4096;
static constexpr auto kExtraOpenOptions = "allocation_size=4K";
static constexpr auto kWtCacheSizeBytes = 1 * 1024 * 1024;

class SpillWiredTigerKVEngineTest : public ServiceContextTest {
protected:
    SpillWiredTigerKVEngineTest() : _dbpath("wt_test"), _opCtx(makeOperationContext()) {
        WiredTigerKVEngineBase::WiredTigerConfig wtConfig =
            getSpillWiredTigerConfigFromStartupOptions();
        wtConfig.cacheSizeMB = kWtCacheSizeBytes / (1 * 1024 * 1024);
        wtConfig.evictionDirtyTargetMB =
            gSpillWiredTigerEvictionDirtyTargetPercentage * wtConfig.cacheSizeMB / 100;
        wtConfig.evictionDirtyTriggerMB =
            gSpillWiredTigerEvictionDirtyTriggerPercentage * wtConfig.cacheSizeMB / 100;
        wtConfig.evictionUpdatesTriggerMB =
            gSpillWiredTigerEvictionUpdatesTriggerPercentage * wtConfig.cacheSizeMB / 100;

        _kvEngine = std::make_unique<SpillWiredTigerKVEngine>(
            std::string{kWiredTigerEngineName},
            _dbpath.path(),
            &_clockSource,
            std::move(wtConfig),
            SpillWiredTigerExtensions::get(_opCtx->getServiceContext()));

        _kvEngine->setRecordStoreExtraOptions(kExtraOpenOptions);

        _recordStore = makeTemporaryRecordStore("a.b", KeyFormat::Long);
        auto ru = _kvEngine->newRecoveryUnit();
        ASSERT_TRUE(_kvEngine->hasIdent(*ru, "collection-a-b"));
    }

    ~SpillWiredTigerKVEngineTest() override {
#if __has_feature(address_sanitizer)
        constexpr bool memLeakAllowed = false;
#else
        constexpr bool memLeakAllowed = true;
#endif
        _kvEngine->cleanShutdown(memLeakAllowed);
    }

    std::unique_ptr<WiredTigerRecordStore> makeTemporaryRecordStore(const std::string& ns,
                                                                    KeyFormat keyFormat) {
        std::string ident = "collection-" + ns;
        std::replace(ident.begin(), ident.end(), '.', '-');
        auto ru = _kvEngine->newRecoveryUnit();
        auto rs = _kvEngine->makeTemporaryRecordStore(*ru, ident, keyFormat);
        return std::unique_ptr<WiredTigerRecordStore>(
            static_cast<WiredTigerRecordStore*>(rs.release()));
    }

    unittest::TempDir _dbpath;
    ClockSourceMock _clockSource;
    std::unique_ptr<SpillWiredTigerKVEngine> _kvEngine;
    std::unique_ptr<WiredTigerRecordStore> _recordStore;
    ServiceContext::UniqueOperationContext _opCtx;
};

TEST_F(SpillWiredTigerKVEngineTest, StorageSize) {
    // This value is guaranteed to use disk.
    auto targetStorageSizeBytes = kWtCacheSizeBytes + kWiredTigerAllocationSize + 1;
    auto data = std::string(targetStorageSizeBytes, 'x');
    auto ru = _kvEngine->newRecoveryUnit();
    auto originalStorageSizeBytes = _kvEngine->storageSize(*ru);

    ru->beginUnitOfWork(false);
    RecordId recordId(1);
    auto statusWith = _recordStore->insertRecord(
        _opCtx.get(), *ru, recordId, data.c_str(), data.size(), Timestamp{1});
    ASSERT_OK(statusWith.getStatus());
    ru->commitUnitOfWork();

    auto newDiskUsageBytes = _kvEngine->storageSize(*ru);
    ASSERT_GT(newDiskUsageBytes, originalStorageSizeBytes);
}

}  // namespace

}  // namespace mongo
