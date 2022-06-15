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
#include "mongo/db/repl/replication_coordinator_mock.h"

namespace mongo {

namespace {
std::string _testLoggingSettings(std::string extraStrings) {
    // Use a small journal for testing to account for the unlikely event that the underlying
    // filesystem does not support fast allocation of a file of zeros.
    return extraStrings + ",log=(file_max=1m,prealloc=false)";
}
}  // namespace

WiredTigerHarnessHelper::WiredTigerHarnessHelper(Options options, StringData extraStrings)
    : _dbpath("wt_test"),
      _lockerNoopClientObserverRegisterer(getServiceContext()),
      _engine(kWiredTigerEngineName,
              _dbpath.path(),
              &_cs,
              _testLoggingSettings(extraStrings.toString()),
              1,
              0,
              false,
              false) {
    repl::ReplicationCoordinator::set(
        serviceContext(),
        options == Options::ReplicationEnabled
            ? std::make_unique<repl::ReplicationCoordinatorMock>(serviceContext())
            : std::make_unique<repl::ReplicationCoordinatorMock>(serviceContext(),
                                                                 repl::ReplSettings()));
    _engine.notifyStartupComplete();
}

std::unique_ptr<RecordStore> WiredTigerHarnessHelper::newRecordStore(
    const std::string& ns, const CollectionOptions& collOptions, KeyFormat keyFormat) {
    ServiceContext::UniqueOperationContext opCtx(newOperationContext());
    WiredTigerRecoveryUnit* ru = checked_cast<WiredTigerRecoveryUnit*>(opCtx->recoveryUnit());
    std::string uri = WiredTigerKVEngine::kTableUriPrefix + ns;
    StringData ident = ns;
    NamespaceString nss(ns);

    StatusWith<std::string> result = WiredTigerRecordStore::generateCreateString(
        kWiredTigerEngineName,
        NamespaceString(ns),
        ident,
        collOptions,
        "",
        keyFormat,
        WiredTigerUtil::useTableLogging(NamespaceString(ns)));
    ASSERT_TRUE(result.isOK());
    std::string config = result.getValue();

    {
        WriteUnitOfWork uow(opCtx.get());
        WT_SESSION* s = ru->getSession()->getSession();
        invariantWTOK(s->create(s, uri.c_str(), config.c_str()), s);
        uow.commit();
    }

    WiredTigerRecordStore::Params params;
    params.nss = nss;
    params.ident = ident.toString();
    params.engineName = kWiredTigerEngineName;
    params.isCapped = collOptions.capped ? true : false;
    params.keyFormat = collOptions.clusteredIndex ? KeyFormat::String : KeyFormat::Long;
    params.overwrite = collOptions.clusteredIndex ? false : true;
    params.isEphemeral = false;
    params.isLogged = WiredTigerUtil::useTableLogging(nss);
    params.cappedCallback = nullptr;
    params.sizeStorer = nullptr;
    params.tracksSizeAdjustments = true;
    params.forceUpdateWithFullDocument = collOptions.timeseries != boost::none;

    auto ret = std::make_unique<StandardWiredTigerRecordStore>(&_engine, opCtx.get(), params);
    ret->postConstructorInit(opCtx.get());
    return std::move(ret);
}

std::unique_ptr<RecordStore> WiredTigerHarnessHelper::newOplogRecordStore() {
    auto ret = newOplogRecordStoreNoInit();
    ServiceContext::UniqueOperationContext opCtx(newOperationContext());
    dynamic_cast<WiredTigerRecordStore*>(ret.get())->postConstructorInit(opCtx.get());
    return ret;
}

std::unique_ptr<RecordStore> WiredTigerHarnessHelper::newOplogRecordStoreNoInit() {
    ServiceContext::UniqueOperationContext opCtx(newOperationContext());
    WiredTigerRecoveryUnit* ru = checked_cast<WiredTigerRecoveryUnit*>(opCtx->recoveryUnit());
    std::string ident = NamespaceString::kRsOplogNamespace.ns();
    std::string uri = WiredTigerKVEngine::kTableUriPrefix + ident;

    CollectionOptions options;
    options.capped = true;

    const NamespaceString oplogNss = NamespaceString::kRsOplogNamespace;
    StatusWith<std::string> result =
        WiredTigerRecordStore::generateCreateString(kWiredTigerEngineName,
                                                    oplogNss,
                                                    ident,
                                                    options,
                                                    "",
                                                    KeyFormat::Long,
                                                    WiredTigerUtil::useTableLogging(oplogNss));
    ASSERT_TRUE(result.isOK());
    std::string config = result.getValue();

    {
        WriteUnitOfWork uow(opCtx.get());
        WT_SESSION* s = ru->getSession()->getSession();
        invariantWTOK(s->create(s, uri.c_str(), config.c_str()), s);
        uow.commit();
    }

    WiredTigerRecordStore::Params params;
    params.nss = oplogNss;
    params.ident = ident;
    params.engineName = kWiredTigerEngineName;
    params.isCapped = true;
    params.keyFormat = KeyFormat::Long;
    params.overwrite = true;
    params.isEphemeral = false;
    params.isLogged = true;
    // Large enough not to exceed capped limits.
    params.oplogMaxSize = 1024 * 1024 * 1024;
    params.cappedCallback = nullptr;
    params.sizeStorer = nullptr;
    params.tracksSizeAdjustments = true;
    params.forceUpdateWithFullDocument = false;
    return std::make_unique<StandardWiredTigerRecordStore>(&_engine, opCtx.get(), params);
}

std::unique_ptr<RecoveryUnit> WiredTigerHarnessHelper::newRecoveryUnit() {
    return std::unique_ptr<RecoveryUnit>(_engine.newRecoveryUnit());
}

std::unique_ptr<RecordStoreHarnessHelper> makeWTRSHarnessHelper(
    RecordStoreHarnessHelper::Options options) {
    return std::make_unique<WiredTigerHarnessHelper>(options);
}

MONGO_INITIALIZER(RegisterRecordStoreHarnessFactory)(InitializerContext* const) {
    mongo::registerRecordStoreHarnessHelperFactory(makeWTRSHarnessHelper);
}
}  // namespace mongo
