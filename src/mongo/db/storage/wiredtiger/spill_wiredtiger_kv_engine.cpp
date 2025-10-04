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

#include "mongo/base/error_codes.h"
#include "mongo/db/storage/key_format.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_connection.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_extensions.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_global_options_gen.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_recovery_unit.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_util.h"
#include "mongo/logv2/log.h"
#include "mongo/util/processinfo.h"

#include <memory>

#include <boost/filesystem/fstream.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage

namespace mongo {
SpillWiredTigerKVEngine::SpillWiredTigerKVEngine(const std::string& canonicalName,
                                                 const std::string& path,
                                                 ClockSource* clockSource,
                                                 WiredTigerConfig wtConfig,
                                                 const SpillWiredTigerExtensions& wtExtensions)
    : WiredTigerKVEngineBase(canonicalName, path, clockSource, std::move(wtConfig)) {
    tassert(10588600, "SpillWiredTigerKVEngine should not be in-memory", !_wtConfig.inMemory);
    if (!boost::filesystem::exists(path)) {
        try {
            boost::filesystem::create_directories(path);
        } catch (std::exception& e) {
            LOGV2_ERROR(10380302,
                        "Error creating data directory",
                        "directory"_attr = path,
                        "error"_attr = e.what());
            throw;
        }
    }

    std::string config =
        generateWTOpenConfigString(_wtConfig, wtExtensions.getOpenExtensionsConfig(), "");
    LOGV2(10158000, "Opening spill WiredTiger", "config"_attr = config);

    auto startTime = Date_t::now();
    _openWiredTiger(path, config);
    LOGV2(10158001, "Spill WiredTiger opened", "duration"_attr = Date_t::now() - startTime);
    _eventHandler.setStartupSuccessful();
    _wtOpenConfig = config;

    _connection =
        std::make_unique<WiredTigerConnection>(_conn, clockSource, /*sessionCacheMax=*/0, this);

    auto param = std::make_unique<SpillWiredTigerEngineRuntimeConfigParameter>(
        "spillWiredTigerEngineRuntimeConfig", ServerParameterType::kRuntimeOnly);
    param->_data.second = this;
    registerServerParameter(std::move(param));
}

SpillWiredTigerKVEngine::~SpillWiredTigerKVEngine() {
    // Unregister the server parameter set in the ctor to prevent a duplicate if we reload the
    // storage engine.
    ServerParameterSet::getNodeParameterSet()->remove("spillWiredTigerEngineRuntimeConfig");

    bool memLeakAllowed = true;
    cleanShutdown(memLeakAllowed);
}

void SpillWiredTigerKVEngine::_openWiredTiger(const std::string& path,
                                              const std::string& wtOpenConfig) {
    auto wtEventHandler = _eventHandler.getWtEventHandler();

    int ret = wiredtiger_open(path.c_str(), wtEventHandler, wtOpenConfig.c_str(), &_conn);
    if (ret) {
        LOGV2_FATAL_NOTRACE(10158002,
                            "Failed to open the spill WiredTiger instance",
                            "details"_attr = wtRCToStatus(ret, nullptr).reason());
    }
}

std::unique_ptr<RecordStore> SpillWiredTigerKVEngine::getTemporaryRecordStore(RecoveryUnit& ru,
                                                                              StringData ident,
                                                                              KeyFormat keyFormat) {
    WiredTigerRecordStore::Params params;
    params.uuid = boost::none;
    params.ident = std::string{ident};
    params.engineName = _canonicalName;
    params.keyFormat = keyFormat;
    params.overwrite = true;
    // We don't log writes to spill tables.
    params.isLogged = false;
    params.forceUpdateWithFullDocument = false;
    params.inMemory = false;
    params.sizeStorer = nullptr;
    params.tracksSizeAdjustments = false;
    return std::make_unique<WiredTigerRecordStore>(
        this, WiredTigerRecoveryUnit::get(ru), std::move(params));
}

std::unique_ptr<RecordStore> SpillWiredTigerKVEngine::makeTemporaryRecordStore(
    RecoveryUnit& ru, StringData ident, KeyFormat keyFormat) {
    WiredTigerSession session(_connection.get());

    WiredTigerRecordStore::WiredTigerTableConfig wtTableConfig;
    wtTableConfig.keyFormat = keyFormat;
    // We don't log writes to spill tables.
    wtTableConfig.logEnabled = false;
    wtTableConfig.blockCompressor = gSpillWiredTigerBlockCompressor;
    wtTableConfig.extraCreateOptions = _rsOptions;
    std::string config =
        WiredTigerRecordStore::generateCreateString({} /* internal table */, wtTableConfig);

    std::string uri = WiredTigerUtil::buildTableUri(ident);
    LOGV2_DEBUG(10158008,
                2,
                "SpillWiredTigerKVEngine::makeTemporaryRecordStore",
                "uri"_attr = uri,
                "config"_attr = config);
    uassertStatusOK(wtRCToStatus(session.create(uri.c_str(), config.c_str()), session));

    return getTemporaryRecordStore(ru, ident, keyFormat);
}

int64_t SpillWiredTigerKVEngine::storageSize(RecoveryUnit& ru) {
    auto permit = tryGetStatsCollectionPermit();
    if (!permit) {
        return 0;
    }

    WiredTigerEventHandler eventHandler;
    auto session = WiredTigerUtil::getStatisticsSession(*this, *permit, eventHandler);
    auto idents = _wtGetAllIdents(*session);

    return std::accumulate(idents.begin(),
                           idents.end(),
                           int64_t{0},
                           [&session](int64_t storageSize, const std::string& ident) {
                               return storageSize +
                                   WiredTigerUtil::getIdentSize(
                                          *session, WiredTigerUtil::buildTableUri(ident));
                           });
}

bool SpillWiredTigerKVEngine::hasIdent(RecoveryUnit& ru, StringData ident) const {
    return _wtHasUri(*WiredTigerRecoveryUnit::get(ru).getSession(),
                     WiredTigerUtil::buildTableUri(ident));
}

int64_t SpillWiredTigerKVEngine::getIdentSize(RecoveryUnit& ru, StringData ident) {
    WiredTigerSession session{_connection.get()};
    return WiredTigerUtil::getIdentSize(session, WiredTigerUtil::buildTableUri(ident));
}

std::vector<std::string> SpillWiredTigerKVEngine::getAllIdents(RecoveryUnit& ru) const {
    auto& wtRu = WiredTigerRecoveryUnit::get(ru);
    return _wtGetAllIdents(*wtRu.getSession());
}

Status SpillWiredTigerKVEngine::dropIdent(RecoveryUnit& ru,
                                          StringData ident,
                                          bool identHasSizeInfo,
                                          const StorageEngine::DropIdentCallback& onDrop) {
    std::string uri = WiredTigerUtil::buildTableUri(ident);

    auto& wtRu = WiredTigerRecoveryUnit::get(ru);
    auto& session = *wtRu.getSessionNoTxn();
    session.closeAllCursors(uri);

    int ret = session.drop(uri.c_str(), "checkpoint_wait=false,force=true");
    Status status = Status::OK();
    if (ret == 0 || ret == ENOENT) {
        // If ident doesn't exist, it is effectively dropped.
    } else {
        status = wtRCToStatus(ret, session);
    }
    LOGV2_DEBUG(10327200, 1, "WT drop", "uri"_attr = uri, "status"_attr = status);
    return status;
}

void SpillWiredTigerKVEngine::cleanShutdown(bool memLeakAllowed) {
    LOGV2(10158003, "SpillWiredTigerKVEngine shutting down");

    if (!_conn) {
        return;
    }

    _connection->shuttingDown();

    std::string closeConfig = "";
    if (memLeakAllowed) {
        closeConfig = "leak_memory=true,";
    }

    auto startTime = Date_t::now();
    LOGV2(10158006, "Closing spill WiredTiger", "closeConfig"_attr = closeConfig);
    invariantWTOK(_conn->close(_conn, closeConfig.c_str()), nullptr);
    LOGV2(10158007, "Closed spill WiredTiger ", "duration"_attr = Date_t::now() - startTime);
    _conn = nullptr;
}

WiredTigerKVEngineBase::WiredTigerConfig getSpillWiredTigerConfigFromStartupOptions() {
    WiredTigerKVEngineBase::WiredTigerConfig wtConfig;

    wtConfig.cacheSizeMB = WiredTigerUtil::getSpillCacheSizeMB(ProcessInfo{}.getMemSizeMB(),
                                                               gSpillWiredTigerCacheSizePercentage,
                                                               gSpillWiredTigerCacheSizeMinMB,
                                                               gSpillWiredTigerCacheSizeMaxMB);
    wtConfig.sessionMax = gSpillWiredTigerSessionMax;
    wtConfig.evictionThreadsMin = gSpillWiredTigerEvictionThreadsMin;
    wtConfig.evictionThreadsMax = gSpillWiredTigerEvictionThreadsMax;
    wtConfig.evictionDirtyTargetMB =
        gSpillWiredTigerEvictionDirtyTargetPercentage * wtConfig.cacheSizeMB / 100;
    wtConfig.evictionDirtyTriggerMB =
        gSpillWiredTigerEvictionDirtyTriggerPercentage * wtConfig.cacheSizeMB / 100;
    wtConfig.evictionUpdatesTriggerMB =
        gSpillWiredTigerEvictionUpdatesTriggerPercentage * wtConfig.cacheSizeMB / 100;
    wtConfig.statisticsLogWaitSecs = wiredTigerGlobalOptions.statisticsLogDelaySecs;
    wtConfig.inMemory = false;
    wtConfig.logEnabled = false;
    wtConfig.prefetchEnabled = false;
    wtConfig.restoreEnabled = false;
    wtConfig.zstdCompressorLevel = gSpillWiredTigerZstdCompressionLevel;
    wtConfig.extraOpenOptions = gSpillWiredTigerEngineConfig;

    return wtConfig;
}

}  // namespace mongo
