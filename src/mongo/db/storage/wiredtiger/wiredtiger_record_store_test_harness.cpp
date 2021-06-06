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

#include "mongo/db/operation_context_noop.h"
#include "mongo/db/repl/replication_coordinator_mock.h"

namespace mongo {

WiredTigerHarnessHelper::WiredTigerHarnessHelper(StringData extraStrings)
    : _dbpath("wt_test"),
      _engine(kWiredTigerEngineName,
              _dbpath.path(),
              &_cs,
              extraStrings.toString(),
              1,
              0,
              false,
              false,
              false,
              false) {
    repl::ReplicationCoordinator::set(
        serviceContext(),
        std::make_unique<repl::ReplicationCoordinatorMock>(serviceContext(), repl::ReplSettings()));
    _engine.notifyStartupComplete();
}

std::unique_ptr<RecordStore> WiredTigerHarnessHelper::newNonCappedRecordStore(
    const std::string& ns, const CollectionOptions& collOptions) {
    WiredTigerRecoveryUnit* ru = checked_cast<WiredTigerRecoveryUnit*>(_engine.newRecoveryUnit());
    OperationContextNoop opCtx(ru);
    std::string uri = WiredTigerKVEngine::kTableUriPrefix + ns;

    StatusWith<std::string> result =
        WiredTigerRecordStore::generateCreateString(kWiredTigerEngineName, ns, collOptions, "");
    ASSERT_TRUE(result.isOK());
    std::string config = result.getValue();

    {
        WriteUnitOfWork uow(&opCtx);
        WT_SESSION* s = ru->getSession()->getSession();
        invariantWTOK(s->create(s, uri.c_str(), config.c_str()));
        uow.commit();
    }

    WiredTigerRecordStore::Params params;
    params.ns = ns;
    params.ident = ns;
    params.engineName = kWiredTigerEngineName;
    params.isCapped = false;
    params.keyFormat = collOptions.clusteredIndex ? KeyFormat::String : KeyFormat::Long;
    params.overwrite = collOptions.clusteredIndex ? false : true;
    params.isEphemeral = false;
    params.cappedCallback = nullptr;
    params.sizeStorer = nullptr;
    params.isReadOnly = false;
    params.tracksSizeAdjustments = true;
    params.forceUpdateWithFullDocument = collOptions.timeseries != boost::none;

    auto ret = std::make_unique<StandardWiredTigerRecordStore>(&_engine, &opCtx, params);
    ret->postConstructorInit(&opCtx);
    return std::move(ret);
}

std::unique_ptr<RecordStore> WiredTigerHarnessHelper::newOplogRecordStore() {
    auto ret = newOplogRecordStoreNoInit();
    auto* ru = _engine.newRecoveryUnit();
    OperationContextNoop opCtx(ru);
    dynamic_cast<WiredTigerRecordStore*>(ret.get())->postConstructorInit(&opCtx);
    return ret;
}

std::unique_ptr<RecordStore> WiredTigerHarnessHelper::newOplogRecordStoreNoInit() {
    WiredTigerRecoveryUnit* ru = dynamic_cast<WiredTigerRecoveryUnit*>(_engine.newRecoveryUnit());
    OperationContextNoop opCtx(ru);
    std::string ident = "a.b";
    std::string uri = WiredTigerKVEngine::kTableUriPrefix + "a.b";

    CollectionOptions options;
    options.capped = true;

    const std::string ns = NamespaceString::kRsOplogNamespace.toString();
    StatusWith<std::string> result =
        WiredTigerRecordStore::generateCreateString(kWiredTigerEngineName, ns, options, "");
    ASSERT_TRUE(result.isOK());
    std::string config = result.getValue();

    {
        WriteUnitOfWork uow(&opCtx);
        WT_SESSION* s = ru->getSession()->getSession();
        invariantWTOK(s->create(s, uri.c_str(), config.c_str()));
        uow.commit();
    }

    WiredTigerRecordStore::Params params;
    params.ns = ns;
    params.ident = ident;
    params.engineName = kWiredTigerEngineName;
    params.isCapped = true;
    params.keyFormat = KeyFormat::Long;
    params.overwrite = true;
    params.isEphemeral = false;
    // Large enough not to exceed capped limits.
    params.oplogMaxSize = 1024 * 1024 * 1024;
    params.cappedCallback = nullptr;
    params.sizeStorer = nullptr;
    params.isReadOnly = false;
    params.tracksSizeAdjustments = true;
    params.forceUpdateWithFullDocument = false;
    return std::make_unique<StandardWiredTigerRecordStore>(&_engine, &opCtx, params);
}

std::unique_ptr<RecoveryUnit> WiredTigerHarnessHelper::newRecoveryUnit() {
    return std::unique_ptr<RecoveryUnit>(_engine.newRecoveryUnit());
}

std::unique_ptr<RecordStoreHarnessHelper> makeWTRSHarnessHelper() {
    return std::make_unique<WiredTigerHarnessHelper>();
}

MONGO_INITIALIZER(RegisterRecordStoreHarnessFactory)(InitializerContext* const) {
    mongo::registerRecordStoreHarnessHelperFactory(makeWTRSHarnessHelper);
}
}  // namespace mongo
