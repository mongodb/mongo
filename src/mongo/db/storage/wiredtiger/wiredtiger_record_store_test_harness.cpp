// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/storage/wiredtiger/wiredtiger_record_store_test_harness.h"

#include "mongo/base/initializer.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/rss/replicated_storage_service.h"
#include "mongo/db/service_context.h"
#include "mongo/db/shard_role/transaction_resources.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_record_store.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_recovery_unit.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_util.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/unittest/unittest.h"

#include <string_view>
#include <utility>

#include <wiredtiger.h>

#include <boost/optional.hpp>

namespace mongo {

namespace {
std::string _testLoggingSettings(std::string extraStrings) {
    // Use a small journal for testing to account for the unlikely event that the underlying
    // filesystem does not support fast allocation of a file of zeros.
    return extraStrings + ",log=(file_max=1m,prealloc=false)";
}
}  // namespace

WiredTigerHarnessHelper::WiredTigerHarnessHelper(Options options, std::string_view extraStrings)
    : _dbpath("wt_test") {
    auto& provider =
        rss::ReplicatedStorageService::get(getGlobalServiceContext()).getPersistenceProvider();
    WiredTigerKVEngineBase::WiredTigerConfig wtConfig =
        getWiredTigerConfigFromStartupOptions(provider);
    wtConfig.cacheSizeMB = 1;
    wtConfig.extraOpenOptions = _testLoggingSettings(std::string{extraStrings});
    _isReplSet = options == Options::ReplicationEnabled;
    auto shouldRecoverFromOplogAsStandalone = false;
    auto replSetMemberInStandaloneMode = false;
    _engine = std::make_unique<WiredTigerKVEngine>(std::string{kWiredTigerEngineName},
                                                   _dbpath.path(),
                                                   &_cs,
                                                   std::move(wtConfig),
                                                   WiredTigerExtensions::get(serviceContext()),
                                                   provider,
                                                   false,
                                                   _isReplSet,
                                                   shouldRecoverFromOplogAsStandalone,
                                                   replSetMemberInStandaloneMode);
    _engine->notifyStorageStartupRecoveryComplete();
}

std::unique_ptr<RecordStore> WiredTigerHarnessHelper::newRecordStore(
    const NamespaceString& nss,
    std::string_view ident,
    const RecordStore::Options& recordStoreOptions,
    boost::optional<UUID> uuid) {
    ServiceContext::UniqueOperationContext opCtx(newOperationContext());
    auto& provider = rss::ReplicatedStorageService::get(opCtx.get()).getPersistenceProvider();
    auto& ru = *shard_role_details::getRecoveryUnit(opCtx.get());
    WriteUnitOfWork wuow(opCtx.get());
    const auto res = _engine->createRecordStore(provider, ru, nss, ident, recordStoreOptions);
    wuow.commit();
    return _engine->getRecordStore(opCtx.get(), nss, ident, recordStoreOptions, uuid);
}

static const auto kOplogOptions = [] {
    RecordStore::Options oplogRecordStoreOptions;
    oplogRecordStoreOptions.isOplog = true;
    oplogRecordStoreOptions.isCapped = true;
    // Large enough not to exceed capped limits.
    oplogRecordStoreOptions.oplogMaxSize = 1024 * 1024 * 1024;
    return oplogRecordStoreOptions;
}();

RecordStore& WiredTigerHarnessHelper::oplogRecordStore() {
    if (_oplog) {
        return *_oplog;
    }

    std::string ident = _identForNs(redactTenant(NamespaceString::kRsOplogNamespace));
    ServiceContext::UniqueOperationContext opCtx(newOperationContext());
    auto& provider = rss::ReplicatedStorageService::get(opCtx.get()).getPersistenceProvider();
    auto& ru = *shard_role_details::getRecoveryUnit(opCtx.get());
    WriteUnitOfWork wuow(opCtx.get());
    const auto res = _engine->createRecordStore(
        provider, ru, NamespaceString::kRsOplogNamespace, ident, kOplogOptions);
    wuow.commit();
    _oplog = _engine->getRecordStore(
        opCtx.get(), NamespaceString::kRsOplogNamespace, ident, kOplogOptions, UUID::gen());
    _engine->getOplogManager()->start(opCtx.get(), *_engine, *_oplog);
    return *_oplog;
}

std::unique_ptr<RecoveryUnit> WiredTigerHarnessHelper::newRecoveryUnit() {
    return std::unique_ptr<RecoveryUnit>(_engine->newRecoveryUnit());
}

std::unique_ptr<RecordStoreHarnessHelper> makeWTRecordStoreHarnessHelper(
    RecordStoreHarnessHelper::Options options) {
    return std::make_unique<WiredTigerHarnessHelper>(options);
}

MONGO_INITIALIZER(RegisterRecordStoreHarnessFactory)(InitializerContext* const) {
    mongo::registerRecordStoreHarnessHelperFactory(makeWTRecordStoreHarnessHelper);
    registerWriteConflictForWritesFactory(kWiredTigerEngineName, [](FailPoint::ModeOptions mode) {
        return std::make_unique<FailPointEnableBlock>("WTWriteConflictException", std::move(mode));
    });
    registerWriteConflictForReadsFactory(kWiredTigerEngineName, [](FailPoint::ModeOptions mode) {
        return std::make_unique<FailPointEnableBlock>("WTWriteConflictExceptionForReads",
                                                      std::move(mode));
    });
}
}  // namespace mongo
