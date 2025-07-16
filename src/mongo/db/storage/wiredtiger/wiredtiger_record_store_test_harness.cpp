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

#include "mongo/db/storage/wiredtiger/wiredtiger_record_store_test_harness.h"

#include "mongo/base/init.h"  // IWYU pragma: keep
#include "mongo/base/initializer.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_record_store.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_recovery_unit.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_util.h"
#include "mongo/db/transaction_resources.h"
#include "mongo/unittest/unittest.h"

#include <utility>

#include <wiredtiger.h>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

namespace {
std::string _testLoggingSettings(std::string extraStrings) {
    // Use a small journal for testing to account for the unlikely event that the underlying
    // filesystem does not support fast allocation of a file of zeros.
    return extraStrings + ",log=(file_max=1m,prealloc=false)";
}
}  // namespace

WiredTigerHarnessHelper::WiredTigerHarnessHelper(Options options, StringData extraStrings)
    : _dbpath("wt_test") {
    WiredTigerKVEngineBase::WiredTigerConfig wtConfig = getWiredTigerConfigFromStartupOptions();
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
                                                   false,
                                                   _isReplSet,
                                                   shouldRecoverFromOplogAsStandalone,
                                                   replSetMemberInStandaloneMode);
    _engine->notifyStorageStartupRecoveryComplete();
}

std::unique_ptr<RecordStore> WiredTigerHarnessHelper::newRecordStore(
    const NamespaceString& nss,
    StringData ident,
    const RecordStore::Options& recordStoreOptions,
    boost::optional<UUID> uuid) {
    ServiceContext::UniqueOperationContext opCtx(newOperationContext());
    const auto res = _engine->createRecordStore(nss, ident, recordStoreOptions);
    return _engine->getRecordStore(opCtx.get(), nss, ident, recordStoreOptions, uuid);
}

std::unique_ptr<RecordStore> WiredTigerHarnessHelper::newOplogRecordStore() {
    auto ret = newOplogRecordStoreNoInit();
    ServiceContext::UniqueOperationContext opCtx(newOperationContext());
    auto oplog = static_cast<WiredTigerRecordStore::Oplog*>(ret.get());
    _engine->getOplogManager()->start(opCtx.get(), *_engine, *oplog, _isReplSet);
    return ret;
}

std::unique_ptr<RecordStore> WiredTigerHarnessHelper::newOplogRecordStoreNoInit() {
    std::string ident = std::string{redactTenant(NamespaceString::kRsOplogNamespace)};
    RecordStore::Options oplogRecordStoreOptions;
    oplogRecordStoreOptions.isOplog = true;
    oplogRecordStoreOptions.isCapped = true;
    // Large enough not to exceed capped limits.
    oplogRecordStoreOptions.oplogMaxSize = 1024 * 1024 * 1024;
    const auto res = _engine->createRecordStore(
        NamespaceString::kRsOplogNamespace, ident, oplogRecordStoreOptions);

    // Cannot use 'getRecordStore', which automatically starts the the oplog manager.
    ServiceContext::UniqueOperationContext opCtx(newOperationContext());
    return std::make_unique<WiredTigerRecordStore::Oplog>(
        _engine.get(),
        WiredTigerRecoveryUnit::get(*shard_role_details::getRecoveryUnit(opCtx.get())),
        WiredTigerRecordStore::Oplog::Params{.uuid = UUID::gen(),
                                             .ident = ident,
                                             .engineName = std::string{kWiredTigerEngineName},
                                             .inMemory = false,
                                             // Large enough not to exceed capped limits.
                                             .oplogMaxSize = oplogRecordStoreOptions.oplogMaxSize,
                                             .sizeStorer = nullptr,
                                             .tracksSizeAdjustments = true,
                                             .forceUpdateWithFullDocument = false});
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
}
}  // namespace mongo
