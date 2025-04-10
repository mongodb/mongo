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


#ifdef _WIN32
#define NVALGRIND
#endif

#include "mongo/db/storage/wiredtiger/temporary_wiredtiger_kv_engine.h"

#include <valgrind/valgrind.h>

#include "mongo/base/error_codes.h"
#include "mongo/db/client.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/key_format.h"
#include "mongo/db/storage/wiredtiger/temporary_wiredtiger_record_store.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_connection.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_recovery_unit.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_util.h"
#include "mongo/logv2/log.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage

namespace mongo {

namespace {

#if __has_feature(address_sanitizer)
constexpr bool kAddressSanitizerEnabled = true;
#else
constexpr bool kAddressSanitizerEnabled = false;
#endif

#if __has_feature(thread_sanitizer)
constexpr bool kThreadSanitizerEnabled = true;
#else
constexpr bool kThreadSanitizerEnabled = false;
#endif

}  // namespace

TemporaryWiredTigerKVEngine::TemporaryWiredTigerKVEngine(const std::string& canonicalName,
                                                         const std::string& path,
                                                         ClockSource* clockSource,
                                                         WiredTigerConfig wtConfig)
    : WiredTigerKVEngineBase(canonicalName, path, clockSource, std::move(wtConfig)) {
    invariant(_wtConfig.inMemory);

    std::string config = generateWTOpenConfigString(_wtConfig, true /* ephemeral */);
    LOGV2(10158000, "Opening temporary WiredTiger", "config"_attr = config);

    auto startTime = Date_t::now();
    _openWiredTiger(path, config);
    LOGV2(10158001, "Temporary WiredTiger opened", "duration"_attr = Date_t::now() - startTime);
    _eventHandler.setStartupSuccessful();
    _wtOpenConfig = config;

    // TODO(SERVER-103355): Disable session caching.
    _connection = std::make_unique<WiredTigerConnection>(_conn, clockSource, this);

    _dbClient = getGlobalServiceContext()
                    ->getService(ClusterRole::ShardServer)
                    ->makeClient("TemporaryWiredTigerRecordStore");

    // TODO(SERVER-103209): Add support for configuring the internal WiredTiger instance at runtime.
}

TemporaryWiredTigerKVEngine::~TemporaryWiredTigerKVEngine() {
    cleanShutdown();
}

void TemporaryWiredTigerKVEngine::_openWiredTiger(const std::string& path,
                                                  const std::string& wtOpenConfig) {
    auto wtEventHandler = _eventHandler.getWtEventHandler();

    int ret = wiredtiger_open(path.c_str(), wtEventHandler, wtOpenConfig.c_str(), &_conn);
    if (ret) {
        LOGV2_FATAL_NOTRACE(10158002,
                            "Failed to open the temporary WiredTiger instance",
                            "details"_attr = wtRCToStatus(ret, nullptr).reason());
    }
}

std::unique_ptr<RecordStore> TemporaryWiredTigerKVEngine::getTemporaryRecordStore(
    OperationContext* opCtx, StringData ident, KeyFormat keyFormat) {
    TemporaryWiredTigerRecordStore::Params params;
    params.baseParams.uuid = boost::none;
    params.baseParams.ident = ident.toString();
    params.baseParams.engineName = _canonicalName;
    params.baseParams.keyFormat = keyFormat;
    params.baseParams.overwrite = true;
    // We don't log writes to temporary record stores.
    params.baseParams.isLogged = false;
    params.baseParams.forceUpdateWithFullDocument = false;
    return std::make_unique<TemporaryWiredTigerRecordStore>(
        this, _dbClient->makeOperationContext(), std::move(params));
}

std::unique_ptr<RecordStore> TemporaryWiredTigerKVEngine::makeTemporaryRecordStore(
    OperationContext* opCtx, StringData ident, KeyFormat keyFormat) {
    WiredTigerSession session(_connection.get());

    WiredTigerRecordStoreBase::WiredTigerTableConfig wtTableConfig =
        getWiredTigerTableConfigFromStartupOptions(true /* usingTemporaryKVEngine */);
    wtTableConfig.keyFormat = keyFormat;
    // We don't log writes to temporary record stores.
    wtTableConfig.logEnabled = false;
    StatusWith<std::string> swConfig = WiredTigerRecordStoreBase::generateCreateString(
        _canonicalName, {} /* internal table */, CollectionOptions(), wtTableConfig);
    uassertStatusOK(swConfig.getStatus());

    std::string config = swConfig.getValue();

    std::string uri = WiredTigerUtil::buildTableUri(ident);
    LOGV2_DEBUG(10158008,
                2,
                "WiredTigerKVEngine::makeTemporaryRecordStore",
                "uri"_attr = uri,
                "config"_attr = config);
    uassertStatusOK(wtRCToStatus(session.create(uri.c_str(), config.c_str()), session));

    return getTemporaryRecordStore(opCtx, ident, keyFormat);
}

std::unique_ptr<RecoveryUnit> TemporaryWiredTigerKVEngine::newRecoveryUnit() {
    return std::make_unique<WiredTigerRecoveryUnit>(_connection.get());
}

void TemporaryWiredTigerKVEngine::cleanShutdown() {
    LOGV2(10158003, "TemporaryWiredTigerKVEngine shutting down");

    if (!_conn) {
        return;
    }

    _connection->shuttingDown();
    _connection.reset();

    // We want WiredTiger to leak memory for faster shutdown except when we are running tools to
    // look for memory leaks.
    bool leak_memory = !kAddressSanitizerEnabled;
    std::string closeConfig = "";
    if (RUNNING_ON_VALGRIND) {  // NOLINT
        leak_memory = false;
    }
    if (leak_memory) {
        closeConfig = "leak_memory=true,";
    }

    auto startTime = Date_t::now();
    LOGV2(10158006, "Closing temporary WiredTiger", "closeConfig"_attr = closeConfig);
    // WT_CONNECTION::close() takes a checkpoint. To ensure this is fast, we delete the tables
    // created by this KVEngine in TemporaryWiredTigerRecordStore destructor.
    invariantWTOK(_conn->close(_conn, closeConfig.c_str()), nullptr);
    LOGV2(10158007, "Closed temporary WiredTiger ", "duration"_attr = Date_t::now() - startTime);
    _conn = nullptr;
}

}  // namespace mongo
