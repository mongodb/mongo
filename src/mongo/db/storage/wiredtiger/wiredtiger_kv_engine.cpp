/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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


#include "mongo/db/storage/wiredtiger/wiredtiger_kv_engine.h"

#include "mongo/base/checked_cast.h"
#include "mongo/base/error_codes.h"
#include "mongo/base/parse_number.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/dotted_path/dotted_path_support.h"
#include "mongo/db/client.h"
#include "mongo/db/commands/server_status_metric.h"
#include "mongo/db/index_names.h"
#include "mongo/db/local_catalog/shard_role_api/transaction_resources.h"
#include "mongo/db/rss/replicated_storage_service.h"
#include "mongo/db/server_feature_flags_gen.h"
#include "mongo/db/server_options.h"
#include "mongo/db/server_parameter.h"
#include "mongo/db/server_recovery.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/ident.h"
#include "mongo/db/storage/journal_listener.h"
#include "mongo/db/storage/key_format.h"
#include "mongo/db/storage/kv_backup_block.h"
#include "mongo/db/storage/snapshot_window_options_gen.h"
#include "mongo/db/storage/storage_file_util.h"
#include "mongo/db/storage/storage_options.h"
#include "mongo/db/storage/storage_parameters_gen.h"
#include "mongo/db/storage/storage_repair_observer.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_cache_pressure_monitor.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_connection.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_cursor.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_cursor_helpers.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_customization_hooks.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_extensions.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_global_options.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_global_options_gen.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_index.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_oplog_manager.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_record_store.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_recovery_unit.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_size_storer.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_storage_options_config_string_flags_parser.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_util.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/platform/compiler.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/background.h"
#include "mongo/util/concurrency/idle_thread_block.h"
#include "mongo/util/debug_util.h"
#include "mongo/util/duration.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/log_and_backoff.h"
#include "mongo/util/quick_exit.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/str.h"
#include "mongo/util/testing_proctor.h"
#include "mongo/util/time_support.h"
#include "mongo/util/uuid.h"
#include "mongo/util/version/releases.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <exception>
#include <iomanip>
#include <memory>
#include <mutex>
#include <sstream>
#include <utility>

#include <wiredtiger.h>

#include <absl/container/node_hash_map.h>
#include <boost/algorithm/string/join.hpp>
#include <boost/filesystem.hpp>
#include <boost/filesystem/fstream.hpp>
#include <boost/optional.hpp>
#include <fmt/format.h>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage
#define LOGV2_FOR_RECOVERY(ID, DLEVEL, MESSAGE, ...) \
    LOGV2_DEBUG_OPTIONS(ID, DLEVEL, {logv2::LogComponent::kStorageRecovery}, MESSAGE, ##__VA_ARGS__)
#define LOGV2_FOR_ROLLBACK(ID, DLEVEL, MESSAGE, ...) \
    LOGV2_DEBUG_OPTIONS(                             \
        ID, DLEVEL, {logv2::LogComponent::kReplicationRollback}, MESSAGE, ##__VA_ARGS__)

namespace mongo {

namespace {

MONGO_FAIL_POINT_DEFINE(WTDropEBUSY);
MONGO_FAIL_POINT_DEFINE(WTPreserveSnapshotHistoryIndefinitely);
MONGO_FAIL_POINT_DEFINE(WTSetOldestTSToStableTS);
MONGO_FAIL_POINT_DEFINE(WTRollbackToStableReturnOnEBUSY);
MONGO_FAIL_POINT_DEFINE(hangBeforeUnrecoverableRollbackError);
MONGO_FAIL_POINT_DEFINE(WTFailImportSortedDataInterface);

const std::string kPinOldestTimestampAtStartupName = "_wt_startup";

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

boost::filesystem::path getOngoingBackupPath() {
    return boost::filesystem::path(storageGlobalParams.dbpath) /
        WiredTigerBackup::kOngoingBackupFile;
}

// There are a few delicate restore scenarios where untimestamped writes are still required.
bool allowUntimestampedWrites(bool inStandaloneMode, bool shouldRecoverFromOplogAsStandalone) {
    // Magic restore may need to perform untimestamped writes on timestamped tables as a part of
    // the server automated restore procedure.
    if (storageGlobalParams.magicRestore) {
        return true;
    }

    if (!gAllowUnsafeUntimestampedWrites) {
        return false;
    }

    // Ignore timestamps in selective restore mode.
    if (storageGlobalParams.restore) {
        return true;
    }

    // We can safely ignore setting this configuration option when recovering from the
    // oplog as standalone because:
    // 1. Replaying oplog entries write with a timestamp.
    // 2. The instance is put in read-only mode after oplog application has finished.
    if (inStandaloneMode && !shouldRecoverFromOplogAsStandalone) {
        return true;
    }

    return false;
}

}  // namespace

std::string extractIdentFromPath(const boost::filesystem::path& dbpath,
                                 const boost::filesystem::path& identAbsolutePath) {
    // Remove the dbpath prefix to the identAbsolutePath.
    boost::filesystem::path identWithExtension = boost::filesystem::relative(
        identAbsolutePath, boost::filesystem::path(storageGlobalParams.dbpath));

    // Remove the file extension and convert to generic form (i.e. replace "\" with "/"
    // on windows, no-op on unix).
    return identWithExtension.replace_extension("").generic_string();
}

bool WiredTigerFileVersion::shouldDowngrade(bool hasRecoveryTimestamp, bool isReplSet) {
    const auto fcvSnapshot = serverGlobalParams.featureCompatibility.acquireFCVSnapshot();
    if (!fcvSnapshot.isVersionInitialized()) {
        // If the FCV document hasn't been read, trust the WT compatibility. MongoD will
        // downgrade to the same compatibility it discovered on startup.
        return _startupVersion == StartupVersion::IS_44_FCV_42 ||
            _startupVersion == StartupVersion::IS_42;
    }

    // (Generic FCV reference): Only consider downgrading when FCV has been fully downgraded to
    // last continuous or last LTS. It's possible for WiredTiger to introduce a data format
    // change in a continuous release. This FCV gate must remain across binary version releases.
    const auto currentVersion = fcvSnapshot.getVersion();
    if (currentVersion != multiversion::GenericFCV::kLastContinuous &&
        currentVersion != multiversion::GenericFCV::kLastLTS) {
        return false;
    }

    if (isReplSet) {
        // If this process is run with `--replSet`, it must have run any startup replication
        // recovery and downgrading at this point is safe.
        return true;
    }

    if (hasRecoveryTimestamp) {
        // If we're not running with `--replSet`, don't allow downgrades if the node needed to
        // run replication recovery. Having a recovery timestamp implies recovery must be run,
        // but it was not.
        return false;
    }

    // If there is no `recoveryTimestamp`, then the data should be consistent with the top of
    // oplog and downgrading can proceed. This is expected for standalone datasets that use FCV.
    return true;
}

std::string WiredTigerFileVersion::getDowngradeString() {
    const auto fcvSnapshot = serverGlobalParams.featureCompatibility.acquireFCVSnapshot();
    if (!fcvSnapshot.isVersionInitialized()) {
        invariant(_startupVersion != StartupVersion::IS_44_FCV_44);

        switch (_startupVersion) {
            case StartupVersion::IS_44_FCV_42:
                return "compatibility=(release=3.3)";
            case StartupVersion::IS_42:
                return "compatibility=(release=3.3)";
            default:
                MONGO_UNREACHABLE;
        }
    }

    // With the introduction of continuous releases, there are two downgrade paths from kLatest.
    // Either to kLastContinuous or kLastLTS. It's possible for the data format to differ
    // between kLastContinuous and kLastLTS and we'll need to handle that appropriately here. We
    // only consider downgrading when FCV has been fully downgraded.
    const auto currentVersion = fcvSnapshot.getVersion();
    // (Generic FCV reference): This FCV check should exist across LTS binary versions because
    // the logic for keeping the WiredTiger release version compatible with the server FCV
    // version will be the same across different LTS binary versions.
    if (currentVersion == multiversion::GenericFCV::kLastContinuous) {
        // If the data format between kLatest and kLastContinuous differs, change the
        // 'kLastContinuousWTRelease' version.
        return kLastContinuousWTRelease;
        // (Generic FCV reference): This FCV check should exist across LTS binary versions
        // because the logic for keeping the WiredTiger release version compatible with the
        // server FCV version will be the same across different LTS binary versions.
    } else if (currentVersion == multiversion::GenericFCV::kLastLTS) {
        // If the data format between kLatest and kLastLTS differs, change the
        // 'kLastLTSWTRelease' version.
        return kLastLTSWTRelease;
    }

    // We're in a state that's not ready to downgrade. Use the latest WiredTiger version for
    // this binary.
    return kLatestWTRelease;
}

using std::set;
using std::string;

class WiredTigerSessionSweeper : public BackgroundJob {
public:
    explicit WiredTigerSessionSweeper(WiredTigerConnection* connection)
        : BackgroundJob(false /* deleteSelf */), _connection(connection) {}

    string name() const override {
        return "WTIdleSessionSweeper";
    }

    void run() override {
        ThreadClient tc(name(), getGlobalServiceContext()->getService(ClusterRole::ShardServer));

        LOGV2_DEBUG(22303, 1, "starting {name} thread", "name"_attr = name());

        while (!_shuttingDown.load()) {
            {
                stdx::unique_lock<stdx::mutex> lock(_mutex);
                MONGO_IDLE_THREAD_BLOCK;
                // Check every 10 seconds or sooner in the debug builds
                _condvar.wait_for(lock, stdx::chrono::seconds(kDebugBuild ? 1 : 10));
            }

            _connection->closeExpiredIdleSessions(gWiredTigerSessionCloseIdleTimeSecs.load() *
                                                  1000);
        }
        LOGV2_DEBUG(22304, 1, "stopping {name} thread", "name"_attr = name());
    }

    void shutdown() {
        _shuttingDown.store(true);
        {
            stdx::unique_lock<stdx::mutex> lock(_mutex);
            // Wake up the session sweeper thread early, we do not want the shutdown
            // to wait for us too long.
            _condvar.notify_one();
        }
        wait();
    }

private:
    WiredTigerConnection* _connection;
    AtomicWord<bool> _shuttingDown{false};

    stdx::mutex _mutex;  // protects _condvar
    // The session sweeper thread idles on this condition variable for a particular time
    // duration between cleaning up expired sessions. It can be triggered early to expedite
    // shutdown.
    stdx::condition_variable _condvar;
};

namespace {

std::string toString(const StorageEngine::OldestActiveTransactionTimestampResult& r) {
    if (r.isOK()) {
        if (r.getValue()) {
            // Timestamp.
            return r.getValue().value().toString();
        } else {
            // boost::none.
            return "null";
        }
    } else {
        return r.getStatus().toString();
    }
}

void setKeyOnCursor(WT_CURSOR* c, const std::variant<std::span<const char>, int64_t>& key) {
    std::visit(OverloadedVisitor{
                   [&](const std::span<const char> k) { c->set_key(c, WiredTigerItem{k}.get()); },
                   [&](int64_t k) {
                       c->set_key(c, k);
                   }},
               key);
}

}  // namespace

std::string generateWTOpenConfigString(const WiredTigerKVEngineBase::WiredTigerConfig& wtConfig,
                                       StringData extensionsConfig,
                                       StringData providerConfig) {
    std::stringstream ss;
    ss << "create,";
    ss << "cache_size=" << wtConfig.cacheSizeMB << "M,";
    ss << "session_max=" << wtConfig.sessionMax << ",";
    ss << "eviction=(threads_min=" << wtConfig.evictionThreadsMin
       << ",threads_max=" << wtConfig.evictionThreadsMax << "),";

    if (wtConfig.evictionDirtyTargetMB)
        ss << "eviction_dirty_target=" << wtConfig.evictionDirtyTargetMB << "MB,";
    if (!gWiredTigerExtraDiagnostics.empty())
        ss << "extra_diagnostics=[" << boost::algorithm::join(gWiredTigerExtraDiagnostics, ",")
           << "],";
    if (wtConfig.evictionDirtyTriggerMB)
        ss << "eviction_dirty_trigger=" << wtConfig.evictionDirtyTriggerMB << "MB,";
    if (wtConfig.evictionUpdatesTriggerMB)
        ss << "eviction_updates_trigger=" << wtConfig.evictionUpdatesTriggerMB << "MB,";
    if (gWiredTigerCheckpointCleanupPeriodSeconds)
        ss << "checkpoint_cleanup=(wait="
           << static_cast<size_t>(gWiredTigerCheckpointCleanupPeriodSeconds) << "),";

    ss << "config_base=false,";
    ss << "statistics=(fast),";

    // TODO: SERVER-109794 move this block into the corresponding persistence providers.
    if (wtConfig.inMemory) {
        invariant(!wtConfig.logEnabled);
        // If we've requested an ephemeral instance we store everything into memory instead of
        // backing it onto disk. Logging is not supported in this instance, thus we also have to
        // disable it.
        ss << "in_memory=true,log=(enabled=false),";
    } else {
        if (wtConfig.logEnabled) {
            ss << "log=(enabled=true,remove=true,path=journal,compressor=";
            ss << wtConfig.logCompressor << "),";
        } else {
            ss << "log=(enabled=false),";
        }
    }

    ss << providerConfig;

    ss << "builtin_extension_config=(zstd=(compression_level=" << wtConfig.zstdCompressorLevel
       << ")),";

    ss << "file_manager=(close_idle_time=" << gWiredTigerFileHandleCloseIdleTime
       << ",close_scan_interval=" << gWiredTigerFileHandleCloseScanInterval
       << ",close_handle_minimum=" << gWiredTigerFileHandleCloseMinimum << "),";
    ss << "statistics_log=(wait=" << wtConfig.statisticsLogWaitSecs << "),";

    // Enable JSON output for errors and messages.
    ss << "json_output=(error,message),";

    // Generate the settings related to the verbose configuration.
    ss << WiredTigerUtil::generateWTVerboseConfiguration() << ",";

    if (kDebugBuild) {
        // Do not abort the process when corruption is found in debug builds, which supports
        // increased test coverage.
        ss << "debug_mode=(corruption_abort=false,";
        // For select debug builds, support enabling WiredTiger eviction debug mode. This uses
        // more aggressive eviction tactics, but may have a negative performance impact.
        if (gWiredTigerEvictionDebugMode) {
            ss << "eviction=true,";
        }
        ss << "),";
    }
    if constexpr (kAddressSanitizerEnabled || kThreadSanitizerEnabled) {
        // For applications using WT, advancing a cursor invalidates the data/memory that cursor
        // was pointing to. WT performs the optimization of managing its own memory. The unit of
        // memory allocation is a page. Walking a cursor from one key/value to the next often
        // lands on the same page, which has the effect of keeping the address of the prior
        // key/value valid. For a bug to occur, the cursor must move across pages, and the prior
        // page must be evicted. While rare, this can happen, resulting in reading random
        // memory.
        //
        // The cursor copy debug mode will instead cause WT to malloc/free memory for each
        // key/value a cursor is positioned on. Thus, enabling when using with address sanitizer
        // will catch many cases of dereferencing invalid cursor positions. Note, there is a
        // known caveat: a free/malloc for roughly the same allocation size can often return the
        // same memory address. This is a scenario where the address sanitizer is not able to
        // detect a use-after-free error.
        //
        // Additionally, WT does not use the standard C thread model and thus TSAN can report
        // false data races when touching memory that was allocated within WT. The cursor_copy
        // mode alleviates this by copying all returned data to its own buffer before leaving
        // the storage engine.
        ss << "debug_mode=(cursor_copy=true),";
    }
    if constexpr (kThreadSanitizerEnabled) {
        // TSAN builds may take longer for certain operations, increase or disable the relevant
        // timeouts.
        ss << "cache_stuck_timeout_ms=900000,";
        ss << "generation_drain_timeout_ms=0,";
    }
    if (TestingProctor::instance().isEnabled()) {
        // Enable debug write-ahead logging for all tables when testing is enabled.
        //
        // If MongoDB startup fails, there may be clues from the previous run still left in the
        // WT log files that can provide some insight into how the system got into a bad state.
        // When testing is enabled, keep around some of these files for investigative purposes.
        //
        // We strive to keep 4 minutes of logs. Increase the retention for tests that take
        // checkpoints more often.
        const double fourMinutesInSeconds = 240.0;
        int ckptsPerFourMinutes;
        if (storageGlobalParams.syncdelay.load() <= 0.0) {
            ckptsPerFourMinutes = 1;
        } else {
            ckptsPerFourMinutes =
                static_cast<int>(fourMinutesInSeconds / storageGlobalParams.syncdelay.load());
        }

        if (ckptsPerFourMinutes < 1) {
            LOGV2_WARNING(8423377,
                          "Unexpected value for checkpoint retention",
                          "syncdelay"_attr =
                              static_cast<std::int64_t>(storageGlobalParams.syncdelay.load()),
                          "ckptsPerFourMinutes"_attr = ckptsPerFourMinutes);
            ckptsPerFourMinutes = 1;
        }

        ss << fmt::format("debug_mode=(table_logging=true,checkpoint_retention={}),",
                          ckptsPerFourMinutes);
    }
    if (gWiredTigerStressConfig) {
        ss << "timing_stress_for_test=[history_store_checkpoint_delay,checkpoint_slow],";
    }

    if (wtConfig.prefetchEnabled && gFeatureFlagPrefetch.isEnabled() && !wtConfig.inMemory) {
        ss << "prefetch=(available=true,default=false),";
    }

    if (wtConfig.restoreEnabled && !wtConfig.liveRestorePath.empty() && !wtConfig.inMemory) {
        ss << "live_restore=(enabled=true,path=\"" << wtConfig.liveRestorePath
           << "\",threads_max=" << wtConfig.liveRestoreThreadsMax
           << ",read_size=" << wtConfig.liveRestoreReadSizeMB << "MB"
           << "),";
    }

    ss << WiredTigerCustomizationHooks::get(getGlobalServiceContext())
              ->getTableCreateConfig("system");
    ss << std::string{extensionsConfig};
    ss << wtConfig.extraOpenOptions;

    if (wtConfig.restoreEnabled && !wtConfig.inMemory && WiredTigerUtil::willRestoreFromBackup()) {
        ss << WiredTigerUtil::generateRestoreConfig() << ",";
    }

    return ss.str();
}

WiredTigerKVEngineBase::WiredTigerKVEngineBase(const std::string& canonicalName,
                                               const std::string& path,
                                               ClockSource* clockSource,
                                               WiredTigerConfig wtConfig)
    : _wtConfig(std::move(wtConfig)),
      _canonicalName(canonicalName),
      _path(path),
      _clockSource(clockSource) {}

void WiredTigerKVEngineBase::setRecordStoreExtraOptions(const std::string& options) {
    _rsOptions = options;
}

Status WiredTigerKVEngineBase::insertIntoIdent(RecoveryUnit& ru,
                                               StringData ident,
                                               std::variant<std::span<const char>, int64_t> key,
                                               std::span<const char> value) {
    invariant(ru.inUnitOfWork());
    auto& wtRu = WiredTigerRecoveryUnit::get(ru);

    // TODO (SERVER-109454): `genTableId()` may be replaced with different cache logic.
    WiredTigerCursor cursor{getWiredTigerCursorParams(wtRu, WiredTigerUtil::genTableId()),
                            WiredTigerUtil::buildTableUri(ident),
                            *wtRu.getSession()};
    wtRu.assertInActiveTxn();
    WT_CURSOR* c = cursor.get();

    setKeyOnCursor(c, key);

    c->set_value(c, WiredTigerItem{value}.get());

    int rc = WT_OP_CHECK(wiredTigerCursorInsert(wtRu, c));
    if (rc == WT_DUPLICATE_KEY)
        // TODO (SERVER-109707): Find (or create) a new way to represent the case of a duplicate
        // key.
        return Status{
            DuplicateKeyErrorInfo(BSONObj(), BSONObj(), BSONObj(), std::monostate(), boost::none),
            "Key already exists in ident"};

    return wtRCToStatus(rc, cursor->session);
}

StatusWith<UniqueBuffer> WiredTigerKVEngineBase::getFromIdent(
    RecoveryUnit& ru, StringData ident, std::variant<std::span<const char>, int64_t> key) {
    auto& wtRu = WiredTigerRecoveryUnit::get(ru);

    // TODO (SERVER-109454): `genTableId()` may be replaced with different cache logic.
    WiredTigerCursor cursor{getWiredTigerCursorParams(wtRu, WiredTigerUtil::genTableId()),
                            WiredTigerUtil::buildTableUri(ident),
                            *wtRu.getSession()};
    WT_CURSOR* c = cursor.get();

    setKeyOnCursor(c, key);

    int rc = WT_OP_CHECK(c->search(c));
    if (rc == WT_NOTFOUND)
        return Status(ErrorCodes::NoSuchKey, "No such key exists in ident");
    if (auto status = wtRCToStatus(rc, cursor->session); !status.isOK())
        return status;

    WiredTigerItem v;
    rc = c->get_value(c, v.get());
    if (auto status = wtRCToStatus(rc, cursor->session); !status.isOK())
        return status;

    UniqueBuffer out = UniqueBuffer::allocate(v.size());
    std::memcpy(out.get(), v.data(), v.size());
    return out;
}

Status WiredTigerKVEngineBase::deleteFromIdent(RecoveryUnit& ru,
                                               StringData ident,
                                               std::variant<std::span<const char>, int64_t> key) {
    invariant(ru.inUnitOfWork());
    auto& wtRu = WiredTigerRecoveryUnit::get(ru);

    // TODO (SERVER-109454): `genTableId()` may be replaced with different cache logic.
    WiredTigerCursor cursor{getWiredTigerCursorParams(wtRu, WiredTigerUtil::genTableId()),
                            WiredTigerUtil::buildTableUri(ident),
                            *wtRu.getSession()};
    wtRu.assertInActiveTxn();
    WT_CURSOR* c = cursor.get();

    setKeyOnCursor(c, key);

    int rc = WT_OP_CHECK(wiredTigerCursorRemove(wtRu, c));
    if (rc == WT_NOTFOUND)
        return Status(ErrorCodes::NoSuchKey, "No such key exists in ident");
    return wtRCToStatus(rc, cursor->session);
}

Status WiredTigerKVEngineBase::reconfigureLogging() {
    auto verboseConfig = WiredTigerUtil::generateWTVerboseConfiguration();
    return wtRCToStatus(_conn->reconfigure(_conn, verboseConfig.c_str()), nullptr);
}

int WiredTigerKVEngineBase::reconfigure(const char* str) {
    LOGV2_DEBUG(10724800, 1, "Reconfiguring", "params"_attr = str);
    return _conn->reconfigure(_conn, str);
}

bool WiredTigerKVEngineBase::_wtHasUri(WiredTigerSession& session, const std::string& uri) const {
    // can't use WiredTigerCursor since this is called from constructor.
    WT_CURSOR* c = nullptr;
    // No need for a metadata:create cursor, since it gathers extra information and is slower.
    int ret = session.open_cursor("metadata:", nullptr, nullptr, &c);
    if (ret == ENOENT)
        return false;
    invariantWTOK(ret, session);
    ON_BLOCK_EXIT([&] { c->close(c); });

    c->set_key(c, uri.c_str());
    return c->search(c) == 0;
}

std::vector<std::string> WiredTigerKVEngineBase::_wtGetAllIdents(WiredTigerSession& session) const {
    std::vector<std::string> all;
    // No need for a metadata:create cursor, since it gathers extra information and is slower.
    WT_CURSOR* c = nullptr;
    auto ret = session.open_cursor("metadata:", nullptr, nullptr, &c);
    uassertStatusOK(wtRCToStatus(ret, session));

    if (!c)
        return all;

    while ((ret = c->next(c)) == 0) {
        const char* raw;
        c->get_key(c, &raw);
        StringData key(raw);
        size_t idx = key.find(':');
        if (idx == string::npos)
            continue;
        StringData type = key.substr(0, idx);
        if (type != "table")
            continue;

        StringData ident = key.substr(idx + 1);
        if (ident == ident::kSizeStorer)
            continue;

        all.push_back(std::string{ident});
    }

    fassert(50663, ret == WT_NOTFOUND);

    return all;
}

WiredTigerKVEngine::WiredTigerKVEngine(const std::string& canonicalName,
                                       const std::string& path,
                                       ClockSource* clockSource,
                                       WiredTigerConfig wtConfig,
                                       const WiredTigerExtensions& wtExtensions,
                                       const rss::PersistenceProvider& provider,
                                       bool repair,
                                       bool isReplSet,
                                       bool shouldRecoverFromOplogAsStandalone,
                                       bool inStandaloneMode)
    : WiredTigerKVEngineBase(canonicalName, path, clockSource, std::move(wtConfig)),
      _oplogManager(std::make_unique<WiredTigerOplogManager>()),
      _sizeStorerSyncTracker(clockSource,
                             gWiredTigerSizeStorerPeriodicSyncHits,
                             Milliseconds{gWiredTigerSizeStorerPeriodicSyncPeriodMillis}),
      _inRepairMode(repair),
      _isReplSet(isReplSet),
      _shouldRecoverFromOplogAsStandalone(shouldRecoverFromOplogAsStandalone),
      _inStandaloneMode(inStandaloneMode),
      _supportsTableLogging(provider.supportsTableLogging()) {
    _pinnedOplogTimestamp.store(Timestamp::max().asULL());
    boost::filesystem::path journalPath = path;
    journalPath /= "journal";
    if (!_wtConfig.inMemory) {
        if (!boost::filesystem::exists(journalPath)) {
            try {
                boost::filesystem::create_directory(journalPath);
            } catch (std::exception& e) {
                LOGV2_ERROR(22312,
                            "Error creating journal directory",
                            "directory"_attr = journalPath.generic_string(),
                            "error"_attr = e.what());
                throw;
            }
        }
    }

    std::string config =
        generateWTOpenConfigString(_wtConfig,
                                   wtExtensions.getOpenExtensionsConfig(),
                                   provider.getWiredTigerConfig(_wtConfig.flattenLeafPageDelta));
    LOGV2(22315, "Opening WiredTiger", "config"_attr = config);

    auto startTime = Date_t::now();
    _openWiredTiger(path, config);
    LOGV2(4795906, "WiredTiger opened", "duration"_attr = Date_t::now() - startTime);
    _eventHandler.setStartupSuccessful();
    _wtOpenConfig = config;

    if (provider.getSentinelDataTimestamp().has_value()) {
        setInitialDataTimestamp(provider.getSentinelDataTimestamp().value());
        setLastMaterializedLsn(provider.getSentinelDataTimestamp().value().asULL());
    }

    {
        char buf[(2 * 8 /*bytes in hex*/) + 1 /*nul terminator*/];
        invariantWTOK(_conn->query_timestamp(_conn, buf, "get=recovery"), nullptr);

        std::uint64_t tmp;
        fassert(50758, NumberParser().base(16)(buf, &tmp));
        _recoveryTimestamp = Timestamp(tmp);
        LOGV2_FOR_RECOVERY(23987,
                           0,
                           "WiredTiger recoveryTimestamp",
                           "recoveryTimestamp"_attr = _recoveryTimestamp);
    }

    {
        char buf[(2 * 8 /*bytes in hex*/) + 1 /*nul terminator*/];
        invariantWTOK(_conn->query_timestamp(_conn, buf, "get=oldest_timestamp"), nullptr);
        std::uint64_t tmp;
        fassert(5380107, NumberParser().base(16)(buf, &tmp));

        if (tmp != 0) {
            LOGV2_FOR_RECOVERY(
                5380106, 0, "WiredTiger oldestTimestamp", "oldestTimestamp"_attr = Timestamp(tmp));
            // The oldest timestamp is set in WT. Only set the in-memory variable.
            _oldestTimestamp.store(tmp);
            setInitialDataTimestamp(Timestamp(tmp));
        }
    }

    int32_t sessionCacheMax =
        ((gWiredTigerSessionCacheMaxPercentage * wiredTigerGlobalOptions.sessionMax) / 100);

    LOGV2(9086700,
          "WiredTiger session cache max value has been set",
          "sessionCacheMax"_attr = sessionCacheMax);

    _connection =
        std::make_unique<WiredTigerConnection>(_conn, _clockSource, sessionCacheMax, this);

    _sessionSweeper = std::make_unique<WiredTigerSessionSweeper>(_connection.get());
    _sessionSweeper->go();

    _cachePressureMonitor = std::make_unique<WiredTigerCachePressureMonitor>(
        *this, clockSource, _wtConfig.evictionDirtyTriggerMB, _wtConfig.evictionUpdatesTriggerMB);

    // Until the Replication layer installs a real callback, prevent truncating the oplog.
    setOldestActiveTransactionTimestampCallback(
        [](Timestamp) { return StatusWith(boost::make_optional(Timestamp::min())); });

    if (!isEphemeral()) {
        if (!_recoveryTimestamp.isNull()) {
            // If the oldest/initial data timestamps were unset (there was no persisted durable
            // history), initialize them to the recovery timestamp.
            if (_oldestTimestamp.load() == 0) {
                setInitialDataTimestamp(_recoveryTimestamp);
                // Communicate the oldest timestamp to WT.
                setOldestTimestamp(_recoveryTimestamp, false);
            }

            // Pin the oldest timestamp prior to calling `setStableTimestamp` as that attempts
            // to advance the oldest timestamp. We do this pinning to give features such as
            // resharding an opportunity to re-pin the oldest timestamp after a restart. The
            // assumptions this relies on are that:
            //
            // 1) The feature stores the desired pin timestamp in some local collection.
            // 2) This temporary pinning lasts long enough for the catalog to be loaded and
            //    accessed.
            {
                stdx::lock_guard<stdx::mutex> lk(_oldestTimestampPinRequestsMutex);
                uassertStatusOK(_pinOldestTimestamp(lk,
                                                    kPinOldestTimestampAtStartupName,
                                                    Timestamp(_oldestTimestamp.load()),
                                                    false));
            }

            setStableTimestamp(_recoveryTimestamp, false);

            _connection->snapshotManager().setLastApplied(_recoveryTimestamp);
        }
    }

    if (isEphemeral() && !TestingProctor::instance().isEnabled()) {
        // We do not maintain any snapshot history for the ephemeral storage engine in
        // production because replication and sharded transactions do not currently run on the
        // inMemory engine. It is live in testing, however.
        minSnapshotHistoryWindowInSeconds.store(0);
    }

    _sizeStorerUri = WiredTigerUtil::buildTableUri(ident::kSizeStorer);
    WiredTigerSession session(_connection.get());
    if (repair && _wtHasUri(session, _sizeStorerUri)) {
        LOGV2(22316, "Repairing size cache");

        auto status = _salvageIfNeeded(_sizeStorerUri.c_str());
        if (status.code() != ErrorCodes::DataModifiedByRepair)
            fassertNoTrace(28577, status);
    }

    _sizeStorer = std::make_unique<WiredTigerSizeStorer>(_connection.get(), _sizeStorerUri);
    auto param = std::make_unique<WiredTigerEngineRuntimeConfigParameter>(
        "wiredTigerEngineRuntimeConfig", ServerParameterType::kRuntimeOnly);
    param->_data.second = this;
    registerServerParameter(std::move(param));
}

WiredTigerKVEngine::~WiredTigerKVEngine() {
    // Unregister the server parameter set in the ctor to prevent a duplicate if we reload the
    // storage engine.
    ServerParameterSet::getNodeParameterSet()->remove("wiredTigerEngineRuntimeConfig");

    bool memLeakAllowed = true;
    cleanShutdown(memLeakAllowed);
}

void WiredTigerKVEngine::notifyStorageStartupRecoveryComplete() {
    unpinOldestTimestamp(kPinOldestTimestampAtStartupName);
}

void WiredTigerKVEngine::notifyReplStartupRecoveryComplete(RecoveryUnit& ru) {
    // The assertion below verifies that our oldest timestamp is not ahead of a non-zero stable
    // timestamp upon exiting startup recovery. This is because it is not safe to begin taking
    // stable checkpoints while the oldest timestamp is ahead of the stable timestamp.
    //
    // If we recover from an unstable checkpoint, such as in the startup recovery for restore
    // case after we have finished oplog replay, we will start up with a null stable timestamp.
    // As a result, we can safely advance the oldest timestamp.
    //
    // If we recover with a stable checkpoint, the stable timestamp will be set to the previous
    // value. In this case, we expect the oldest timestamp to be advanced in lockstep with the
    // stable timestamp during any recovery process, and so the oldest timestamp should never
    // exceed the stable timestamp.
    const Timestamp oldest = getOldestTimestamp();
    const Timestamp stable = getStableTimestamp();
    uassert(8470600,
            str::stream() << "Oldest timestamp " << oldest
                          << " is ahead of non-zero stable timestamp " << stable,
            (stable.isNull() || oldest.isNull() || oldest <= stable));

    if (!gEnableAutoCompaction)
        return;

    // Exclude the oplog table, if it exists.
    invariant(_oplogManager);
    std::vector<StringData> excludedIdents;
    if (auto oplogIdent = _oplogManager->getIdent(); !oplogIdent.empty()) {
        LOGV2_DEBUG(
            9611300, 1, "Excluding oplog table for auto compact", "ident"_attr = oplogIdent);
        excludedIdents.push_back(oplogIdent);
    }
    AutoCompactOptions options{/*enable=*/true,
                               /*runOnce=*/false,
                               /*freeSpaceTargetMB=*/boost::none,
                               std::move(excludedIdents)};

    auto status = autoCompact(ru, options);
    if (status.isOK()) {
        LOGV2(8704102, "AutoCompact enabled");
        return;
    }

    // Proceed with startup if background compaction fails to start. Crash for unexpected error
    // codes.
    if (status != ErrorCodes::IllegalOperation && status != ErrorCodes::ObjectIsBusy) {
        invariantStatusOK(
            status.withContext("Background compaction failed to start due to an unexpected error"));
    }
}

void WiredTigerKVEngine::setInStandaloneMode() {
    _inStandaloneMode.store(true);
}

void WiredTigerKVEngine::_openWiredTiger(const std::string& path, const std::string& wtOpenConfig) {
    // MongoDB 4.4 will always run in compatibility version 10.0.
    std::string configStr = wtOpenConfig + ",compatibility=(require_min=\"10.0.0\")";
    auto wtEventHandler = _eventHandler.getWtEventHandler();

    int ret = wiredtiger_open(path.c_str(), wtEventHandler, configStr.c_str(), &_conn);
    if (!ret) {
        _fileVersion = {WiredTigerFileVersion::StartupVersion::IS_44_FCV_44};
        return;
    }

    if (_eventHandler.isWtIncompatible()) {
        // WT 4.4+ will refuse to startup on datafiles left behind by 4.0 and earlier. This
        // behavior is enforced outside of `require_min`. This condition is detected via a
        // specific error message from WiredTiger.
        if (_inRepairMode) {
            // In case this process was started with `--repair`, remove the "repair incomplete"
            // file.
            StorageRepairObserver::get(getGlobalServiceContext())->onRepairDone(nullptr, {});
        }
        LOGV2_FATAL_NOTRACE(
            4671205,
            "This version of MongoDB is too recent to start up on the existing data files. "
            "Try MongoDB 4.2 or earlier.");
    }

    // MongoDB 4.4 doing clean shutdown in FCV 4.2 will use compatibility version 3.3.
    configStr = wtOpenConfig + ",compatibility=(require_min=\"3.3.0\")";
    ret = wiredtiger_open(path.c_str(), wtEventHandler, configStr.c_str(), &_conn);
    if (!ret) {
        _fileVersion = {WiredTigerFileVersion::StartupVersion::IS_44_FCV_42};
        return;
    }

    // MongoDB 4.2 uses compatibility version 3.2.
    configStr = wtOpenConfig + ",compatibility=(require_min=\"3.2.0\")";
    ret = wiredtiger_open(path.c_str(), wtEventHandler, configStr.c_str(), &_conn);
    if (!ret) {
        _fileVersion = {WiredTigerFileVersion::StartupVersion::IS_42};
        return;
    }

    LOGV2_WARNING(22347,
                  "Failed to start up WiredTiger under any compatibility version. This may be due "
                  "to an unsupported upgrade or downgrade.");
    if (ret == EINVAL) {
        fassertFailedNoTrace(28561);
    }

    if (ret == WT_TRY_SALVAGE) {
        LOGV2_WARNING(22348, "WiredTiger metadata corruption detected");
        if (!_inRepairMode) {
            LOGV2_FATAL_NOTRACE(50944, kWTRepairMsg);
        }
    }

    if (!_inRepairMode) {
        LOGV2_FATAL_NOTRACE(
            28595, "Terminating.", "reason"_attr = wtRCToStatus(ret, nullptr).reason());
    }

    // Always attempt to salvage metadata regardless of error code when in repair mode.
    LOGV2_WARNING(22349, "Attempting to salvage WiredTiger metadata");
    configStr = wtOpenConfig + ",salvage=true";
    ret = wiredtiger_open(path.c_str(), wtEventHandler, configStr.c_str(), &_conn);
    if (!ret) {
        StorageRepairObserver::get(getGlobalServiceContext())
            ->invalidatingModification("WiredTiger metadata salvaged");
        return;
    }

    LOGV2_FATAL_NOTRACE(50947,
                        "Failed to salvage WiredTiger metadata",
                        "details"_attr = wtRCToStatus(ret, nullptr).reason());
}

void WiredTigerKVEngine::cleanShutdown(bool memLeakAllowed) {
    LOGV2(22317, "WiredTigerKVEngine shutting down");

    if (!_conn) {
        return;
    }

    getOplogManager()->stop();

    if (_sessionSweeper) {
        LOGV2(22318, "Shutting down session sweeper thread");
        _sessionSweeper->shutdown();
        LOGV2(22319, "Finished shutting down session sweeper thread");
    }
    LOGV2_FOR_RECOVERY(23988,
                       2,
                       "Shutdown timestamps.",
                       "Stable Timestamp"_attr = Timestamp(_stableTimestamp.load()),
                       "Initial Data Timestamp"_attr = Timestamp(_initialDataTimestamp.load()),
                       "Oldest Timestamp"_attr = Timestamp(_oldestTimestamp.load()));

    _connection->shuttingDown();

    syncSizeInfo(/*syncToDisk=*/true);

    // The size storer has to be destructed after the session cache has shut down. This sets the
    // shutdown flag internally in the session cache. As operations get interrupted during
    // shutdown, they release their session back to the session cache. If the shutdown flag has
    // been set, released sessions will skip flushing the size storer.
    _sizeStorer.reset();

    _waitUntilDurableSession.reset();

    std::string closeConfig = "";

    if (memLeakAllowed) {
        closeConfig = "leak_memory=true,";
    }

    const Timestamp initialDataTimestamp = getInitialDataTimestamp();
    if (gTakeUnstableCheckpointOnShutdown || initialDataTimestamp.asULL() <= 1) {
        closeConfig += "use_timestamp=false,";
    }

    if (_fileVersion.shouldDowngrade(!_recoveryTimestamp.isNull(), _isReplSet)) {
        auto startTime = Date_t::now();
        LOGV2(22324,
              "Closing WiredTiger in preparation for reconfiguring",
              "closeConfig"_attr = closeConfig);
        invariantWTOK(_conn->close(_conn, closeConfig.c_str()), nullptr);
        LOGV2(4795905, "WiredTiger closed", "duration"_attr = Date_t::now() - startTime);

        startTime = Date_t::now();
        invariantWTOK(
            wiredtiger_open(
                _path.c_str(), _eventHandler.getWtEventHandler(), _wtOpenConfig.c_str(), &_conn),
            nullptr);
        LOGV2(4795904, "WiredTiger re-opened", "duration"_attr = Date_t::now() - startTime);

        startTime = Date_t::now();
        LOGV2(22325, "Reconfiguring", "newConfig"_attr = _fileVersion.getDowngradeString());
        invariantWTOK(_conn->reconfigure(_conn, _fileVersion.getDowngradeString().c_str()),
                      nullptr);
        LOGV2(4795903, "Reconfigure complete", "duration"_attr = Date_t::now() - startTime);
    }

    if (gWiredTigerVerboseShutdownCheckpointLogs) {
        logv2::LogManager::global().getGlobalSettings().setMinimumLoggedSeverity(
            logv2::LogComponent::kWiredTigerCheckpoint,
            logv2::LogSeverity::Debug(logv2::LogSeverity::kMaxDebugLevel));
        reconfigureLogging().ignore();  // Best-effort.
    }

    auto startTime = Date_t::now();
    LOGV2(4795902, "Closing WiredTiger", "closeConfig"_attr = closeConfig);
    invariantWTOK(_conn->close(_conn, closeConfig.c_str()), nullptr);
    LOGV2(4795901, "WiredTiger closed", "duration"_attr = Date_t::now() - startTime);
    _conn = nullptr;
}

int64_t WiredTigerKVEngine::getIdentSize(RecoveryUnit& ru, StringData ident) {
    return WiredTigerUtil::getIdentSize(*WiredTigerRecoveryUnit::get(ru).getSession(),
                                        WiredTigerUtil::buildTableUri(ident));
}

Status WiredTigerKVEngine::repairIdent(RecoveryUnit& ru, StringData ident) {
    WiredTigerSession* session = WiredTigerRecoveryUnit::get(ru).getSession();
    string uri = WiredTigerUtil::buildTableUri(ident);
    session->closeAllCursors(uri);
    if (isEphemeral()) {
        return Status::OK();
    }
    auto ensuredIdent = _ensureIdentPath(ident);
    return _salvageIfNeeded(uri.c_str());
}

Status WiredTigerKVEngine::_salvageIfNeeded(const char* uri) {
    // Using a side session to avoid transactional issues
    WiredTigerSession session(_connection.get());

    int rc = session.verify(uri, nullptr);
    // WT may return EBUSY if the database contains dirty data. If we checkpoint and retry the
    // operation it will attempt to clean up the dirty elements during checkpointing, thus
    // allowing the operation to succeed if it was the only reason to fail.
    if (rc == EBUSY) {
        _checkpoint(session);
        rc = session.verify(uri, nullptr);
    }

    if (rc == 0) {
        LOGV2(22327, "Verify succeeded. Not salvaging.", "uri"_attr = uri);
        return Status::OK();
    }

    if (rc == ENOENT) {
        LOGV2_WARNING(22350,
                      "Data file is missing. Attempting to drop and re-create the collection.",
                      "uri"_attr = uri);

        return _rebuildIdent(session, uri);
    }

    LOGV2(22328, "Verify failed. Running a salvage operation.", "uri"_attr = uri);
    rc = session.salvage(uri, nullptr);
    // Same reasoning for handling EBUSY errors as above.
    if (rc == EBUSY) {
        _checkpoint(session);
        rc = session.salvage(uri, nullptr);
    }
    auto status = wtRCToStatus(rc, session, "Salvage failed:");
    if (status.isOK()) {
        return {ErrorCodes::DataModifiedByRepair, str::stream() << "Salvaged data for " << uri};
    }

    LOGV2_WARNING(22351,
                  "Salvage failed. The file will be moved out of "
                  "the way and a new ident will be created.",
                  "uri"_attr = uri,
                  "error"_attr = status);

    //  If the data is unsalvageable, we should completely rebuild the ident.
    return _rebuildIdent(session, uri);
}

Status WiredTigerKVEngine::_rebuildIdent(WiredTigerSession& session, const char* uri) {
    invariant(_inRepairMode);

    invariant(std::string(uri).find(WiredTigerUtil::kTableUriPrefix.data()) == 0);

    const std::string identName(uri + WiredTigerUtil::kTableUriPrefix.size());
    auto filePath = getDataFilePathForIdent(identName);
    if (filePath) {
        const boost::filesystem::path corruptFile(filePath->string() + ".corrupt");
        LOGV2_WARNING(22352,
                      "Moving data file to backup",
                      "file"_attr = filePath->generic_string(),
                      "backup"_attr = corruptFile.generic_string());

        auto status = fsyncRename(filePath.value(), corruptFile);
        if (!status.isOK()) {
            return status;
        }
    }

    LOGV2_WARNING(22353, "Rebuilding ident", "ident"_attr = identName);

    // This is safe to call after moving the file because it only reads from the metadata, and
    // not the data file itself.
    auto swMetadata = WiredTigerUtil::getMetadataCreate(session, uri);
    if (!swMetadata.isOK()) {
        auto status = swMetadata.getStatus();
        LOGV2_ERROR(22357,
                    "Rebuilding ident failed: failed to get metadata",
                    "uri"_attr = uri,
                    "error"_attr = status);
        return status;
    }
    Status status = _drop(session, uri, nullptr);
    if (!status.isOK()) {
        LOGV2_ERROR(22358,
                    "Rebuilding ident failed: failed to drop",
                    "uri"_attr = uri,
                    "error"_attr = status);
        return status;
    }

    int rc = session.create(uri, swMetadata.getValue().c_str());
    if (rc != 0) {
        auto status = wtRCToStatus(rc, session);
        LOGV2_ERROR(22359,
                    "Rebuilding ident failed: failed to create with config",
                    "uri"_attr = uri,
                    "config"_attr = swMetadata.getValue(),
                    "error"_attr = status);
        return status;
    }
    LOGV2(22329, "Successfully re-created table", "uri"_attr = uri);
    return {ErrorCodes::DataModifiedByRepair,
            str::stream() << "Re-created empty data file for " << uri};
}

void WiredTigerKVEngine::flushAllFiles(OperationContext* opCtx, bool callerHoldsReadLock) {
    LOGV2_DEBUG(22330, 1, "WiredTigerKVEngine::flushAllFiles");
    if (_wtConfig.inMemory) {
        return;
    }

    const Timestamp stableTimestamp = getStableTimestamp();
    const Timestamp initialDataTimestamp = getInitialDataTimestamp();
    uassert(5841000,
            "Cannot take checkpoints when the stable timestamp is less than the initial data "
            "timestamp",
            initialDataTimestamp == Timestamp::kAllowUnstableCheckpointsSentinel ||
                stableTimestamp >= initialDataTimestamp);

    // Immediately flush the size storer information to disk. When the node is fsync locked for
    // operations such as backup, it's imperative that we copy the most up-to-date data files.
    syncSizeInfo(true);

    // If there's no journal (ephemeral), we must checkpoint all of the data.
    Fsync fsyncType = !isEphemeral() ? Fsync::kCheckpointStableTimestamp : Fsync::kCheckpointAll;

    // We will skip updating the journal listener if the caller holds read locks.
    // The JournalListener may do writes, and taking write locks would conflict with the read
    // locks.
    UseJournalListener useListener =
        callerHoldsReadLock ? UseJournalListener::kSkip : UseJournalListener::kUpdate;

    waitUntilDurable(opCtx, fsyncType, useListener);
}

Status WiredTigerKVEngine::beginBackup() {
    invariant(!_backupSession);

    // The inMemory Storage Engine cannot create a backup cursor.
    if (isEphemeral()) {
        return Status::OK();
    }

    // Persist the sizeStorer information to disk before opening the backup cursor.
    syncSizeInfo(true);

    // This cursor will be freed by the backupSession being closed as the session is uncached
    auto session = std::make_unique<WiredTigerSession>(_connection.get());
    WT_CURSOR* c = nullptr;
    int ret = WT_OP_CHECK(session->open_cursor("backup:", nullptr, nullptr, &c));
    if (ret != 0) {
        return wtRCToStatus(ret, *session);
    }
    _backupSession = std::move(session);
    return Status::OK();
}

void WiredTigerKVEngine::endBackup() {
    // There could be a race with clean shutdown which unconditionally closes all the sessions.
    WiredTigerConnection::BlockShutdown block(_connection.get());
    if (_connection->isShuttingDown()) {
        _backupSession->dropSessionBeforeDeleting();
    }

    _backupSession.reset();
}

Status WiredTigerKVEngine::disableIncrementalBackup() {
    // Opening an incremental backup cursor with the "force_stop=true" configuration option then
    // closing the cursor will set a flag in WiredTiger that causes it to release all
    // incremental information and resources. Opening a subsequent incremental backup cursor
    // will reset the flag in WiredTiger and reinstate incremental backup history.
    uassert(31401, "Cannot open backup cursor with in-memory storage engine.", !isEphemeral());

    auto sessionRaii = std::make_unique<WiredTigerSession>(_connection.get());
    WT_CURSOR* cursor = nullptr;
    int wtRet =
        sessionRaii->open_cursor("backup:", nullptr, "incremental=(force_stop=true)", &cursor);
    if (wtRet != 0) {
        LOGV2_ERROR(22360, "Could not open a backup cursor to disable incremental backups");
        return wtRCToStatus(wtRet, *sessionRaii);
    }

    return Status::OK();
}

namespace {

boost::filesystem::path constructFilePath(std::string path, std::string filename) {
    const auto directoryPath = boost::filesystem::path(path);
    const auto wiredTigerLogFilePrefix = "WiredTigerLog";

    boost::filesystem::path filePath = directoryPath;
    if (filename.find(wiredTigerLogFilePrefix) == 0) {
        // TODO SERVER-13455: Replace `journal/` with the configurable journal path.
        filePath /= boost::filesystem::path("journal");
    }
    filePath /= filename;

    return filePath;
}

std::deque<std::string> getUniqueFiles(const std::vector<std::string>& files,
                                       const std::set<std::string>& referenceFiles) {
    std::deque<std::string> result;
    for (auto& file : files) {
        if (referenceFiles.find(file) == referenceFiles.end()) {
            result.push_back(file);
        }
    }
    return result;
}

class StreamingCursorImpl : public StorageEngine::StreamingCursor {
public:
    StreamingCursorImpl() = delete;
    explicit StreamingCursorImpl(WiredTigerSession* session,
                                 std::string path,
                                 StorageEngine::BackupOptions options,
                                 WiredTigerBackup* wtBackup)
        : StorageEngine::StreamingCursor(options),
          _session(session),
          _path(path),
          _wtBackup(wtBackup) {};

    ~StreamingCursorImpl() override = default;

    StatusWith<std::deque<KVBackupBlock>> getNextBatch(const std::size_t batchSize) override {
        int wtRet = 0;
        std::deque<KVBackupBlock> kvBackupBlocks;

        stdx::lock_guard<stdx::mutex> backupCursorLk(_wtBackup->wtBackupCursorMutex);
        while (kvBackupBlocks.size() < batchSize) {
            stdx::lock_guard<stdx::mutex> backupDupCursorLk(_wtBackup->wtBackupDupCursorMutex);

            // We may still have backup blocks to retrieve for the existing file that
            // _wtBackup->cursor is open on if _wtBackup->dupCursor exists. In this case, do not
            // call next() on _wtBackup->cursor.
            if (!_wtBackup->dupCursor) {
                wtRet = (_wtBackup->cursor)->next(_wtBackup->cursor);
                if (wtRet != 0) {
                    break;
                }
            }

            const char* filename;
            invariantWTOK((_wtBackup->cursor)->get_key(_wtBackup->cursor, &filename),
                          _wtBackup->cursor->session);
            const boost::filesystem::path filePath = constructFilePath(_path, {filename});

            const auto wiredTigerLogFilePrefix = "WiredTigerLog";
            if (std::string(filename).find(wiredTigerLogFilePrefix) == 0) {
                // If extendBackupCursor() is called prior to the StreamingCursor running into
                // log files, we must ensure that subsequent calls to getNextBatch() do not
                // return duplicate files.
                if ((_wtBackup->logFilePathsSeenByExtendBackupCursor).find(filePath.string()) !=
                    (_wtBackup->logFilePathsSeenByExtendBackupCursor).end()) {
                    break;
                }
                (_wtBackup->logFilePathsSeenByGetNextBatch).insert(filePath.string());
            }

            boost::system::error_code errorCode;
            const std::uint64_t fileSize = boost::filesystem::file_size(filePath, errorCode);
            uassert(31403,
                    fmt::format("Failed to get a file's size. Filename: {} Error: {}",
                                filePath.string(),
                                errorCode.message()),
                    !errorCode);

            if (options.incrementalBackup && options.srcBackupName) {
                // For a subsequent incremental backup, each BackupBlock corresponds to changes
                // made to data files since the initial incremental backup. Each BackupBlock has
                // a maximum size of options.blockSizeMB. Incremental backups open a duplicate
                // cursor, which is stored in _wtBackup->dupCursor.
                //
                // 'kvBackupBlocks' is an out parameter.
                Status status = _getNextIncrementalBatchForFile(
                    filename, filePath, fileSize, batchSize, &kvBackupBlocks);

                if (!status.isOK()) {
                    return status;
                }
            } else {
                // For a full backup or the initial incremental backup, each BackupBlock
                // corresponds to an entire file. Full backups cannot open an incremental
                // cursor, even if they are the initial incremental backup.
                const std::uint64_t length = options.incrementalBackup ? fileSize : 0;
                std::string ident = extractIdentFromPath(
                    boost::filesystem::path(storageGlobalParams.dbpath), filePath);

                LOGV2_DEBUG(9538603,
                            2,
                            "File to copy for backup",
                            "filePath"_attr = filePath.string(),
                            "offset"_attr = 0,
                            "size"_attr = fileSize);
                kvBackupBlocks.push_back(
                    KVBackupBlock(ident, filePath.string(), 0 /* offset */, length, fileSize));
            }
        }

        if (wtRet && wtRet != WT_NOTFOUND && kvBackupBlocks.size() != batchSize) {
            return wtRCToStatus(wtRet, *_session);
        }

        return kvBackupBlocks;
    }

private:
    Status _getNextIncrementalBatchForFile(const char* filename,
                                           boost::filesystem::path filePath,
                                           const std::uint64_t fileSize,
                                           const std::size_t batchSize,
                                           std::deque<KVBackupBlock>* kvBackupBlocks) {
        // For each file listed, open a duplicate backup cursor and get the blocks to copy.
        std::stringstream ss;
        ss << "incremental=(file=" << filename << ")";
        const std::string config = ss.str();

        int wtRet;
        bool fileUnchangedFlag = false;
        if (!_wtBackup->dupCursor) {
            size_t attempt = 0;
            do {
                LOGV2_DEBUG(9538604, 2, "Opening duplicate backup cursor", "config"_attr = config);
                wtRet = _session->open_cursor(
                    nullptr, _wtBackup->cursor, config.c_str(), &_wtBackup->dupCursor);

                if (wtRet == EBUSY) {
                    logAndBackoff(8927900,
                                  ::mongo::logv2::LogComponent::kStorage,
                                  logv2::LogSeverity::Debug(1),
                                  ++attempt,
                                  "Opening duplicate backup cursor returned EBUSY, retrying",
                                  "config"_attr = config);
                } else if (wtRet != 0) {
                    return wtRCToStatus(wtRet, *_session);
                }
            } while (wtRet == EBUSY);
            fileUnchangedFlag = true;
        }

        while (kvBackupBlocks->size() < batchSize) {
            wtRet = (_wtBackup->dupCursor)->next(_wtBackup->dupCursor);
            if (wtRet == WT_NOTFOUND) {
                break;
            }
            invariantWTOK(wtRet, _wtBackup->dupCursor->session);
            fileUnchangedFlag = false;

            uint64_t offset, size, type;
            std::string ident =
                extractIdentFromPath(boost::filesystem::path(storageGlobalParams.dbpath), filePath);
            invariantWTOK(
                (_wtBackup->dupCursor)->get_key(_wtBackup->dupCursor, &offset, &size, &type),
                _wtBackup->dupCursor->session);

            LOGV2_DEBUG(22311,
                        2,
                        "Block to copy for incremental backup",
                        "filePath"_attr = filePath.string(),
                        "offset"_attr = offset,
                        "size"_attr = size,
                        "type"_attr = type);
            kvBackupBlocks->push_back(
                KVBackupBlock(ident, filePath.string(), offset, size, fileSize));
        }

        // If the file is unchanged, push a BackupBlock with offset=0 and length=0. This allows
        // us to distinguish between an unchanged file and a deleted file in an incremental
        // backup.
        if (fileUnchangedFlag) {
            std::string ident =
                extractIdentFromPath(boost::filesystem::path(storageGlobalParams.dbpath), filePath);
            kvBackupBlocks->push_back(
                KVBackupBlock(ident, filePath.string(), 0 /* offset */, 0 /* length */, fileSize));
        }

        // If the duplicate backup cursor has been exhausted, close it and set
        // _wtBackup->dupCursor=nullptr.
        if (wtRet != 0) {
            if (wtRet != WT_NOTFOUND ||
                (wtRet = (_wtBackup->dupCursor)->close(_wtBackup->dupCursor)) != 0) {
                return wtRCToStatus(wtRet, *_session);
            }
            _wtBackup->dupCursor = nullptr;
            (_wtBackup->wtBackupDupCursorCV).notify_one();
        }

        return Status::OK();
    }

    WiredTigerSession* _session;
    std::string _path;
    WiredTigerBackup* _wtBackup;  // '_wtBackup' is an out parameter.
};

}  // namespace

StatusWith<std::unique_ptr<StorageEngine::StreamingCursor>>
WiredTigerKVEngine::beginNonBlockingBackup(const StorageEngine::BackupOptions& options) {
    uassert(51034, "Cannot open backup cursor with in-memory mode.", !isEphemeral());

    std::stringstream ss;
    if (options.incrementalBackup) {
        invariant(options.thisBackupName);
        ss << "incremental=(enabled=true,force_stop=false,";
        ss << "granularity=" << options.blockSizeMB << "MB,";
        ss << "this_id=" << std::quoted(str::escape(*options.thisBackupName)) << ",";

        if (options.srcBackupName) {
            ss << "src_id=" << std::quoted(str::escape(*options.srcBackupName)) << ",";
        }

        ss << ")";
    }

    stdx::lock_guard<stdx::mutex> backupCursorLk(_wtBackup.wtBackupCursorMutex);

    // Create ongoingBackup.lock file to signal recovery that it should delete WiredTiger.backup
    // if we have an unclean shutdown with the cursor still open.
    {
        boost::filesystem::ofstream ongoingBackup(getOngoingBackupPath());
    }

    // Oplog truncation thread won't remove oplog since the checkpoint pinned by the backup
    // cursor.
    stdx::lock_guard<stdx::mutex> lock(_oplogPinnedByBackupMutex);
    _oplogPinnedByBackup = Timestamp(_oplogNeededForCrashRecovery.load());
    ScopeGuard pinOplogGuard([&] { _oplogPinnedByBackup = boost::none; });

    // Persist the sizeStorer information to disk before opening the backup cursor. We aren't
    // guaranteed to have the most up-to-date size information after the backup as writes can
    // still occur during a nonblocking backup.
    syncSizeInfo(true);

    // This cursor will be freed by the backupSession being closed as the session is uncached
    auto sessionRaii = std::make_unique<WiredTigerSession>(_connection.get());
    WT_CURSOR* cursor = nullptr;
    const std::string config = ss.str();
    int wtRet = sessionRaii->open_cursor("backup:", nullptr, config.c_str(), &cursor);
    if (wtRet != 0) {
        boost::filesystem::remove(getOngoingBackupPath());
        return wtRCToStatus(wtRet, *sessionRaii);
    }

    // A nullptr indicates that no duplicate cursor is open during an incremental backup.
    stdx::lock_guard<stdx::mutex> backupDupCursorLk(_wtBackup.wtBackupDupCursorMutex);
    _wtBackup.dupCursor = nullptr;

    invariant(_wtBackup.logFilePathsSeenByExtendBackupCursor.empty());
    invariant(_wtBackup.logFilePathsSeenByGetNextBatch.empty());

    auto streamingCursor =
        std::make_unique<StreamingCursorImpl>(sessionRaii.get(), _path, options, &_wtBackup);

    pinOplogGuard.dismiss();
    _backupSession = std::move(sessionRaii);
    _wtBackup.cursor = cursor;

    return streamingCursor;
}

void WiredTigerKVEngine::endNonBlockingBackup() {
    stdx::lock_guard<stdx::mutex> backupCursorLk(_wtBackup.wtBackupCursorMutex);
    _backupSession.reset();
    {
        // Oplog truncation thread can now remove the pinned oplog.
        stdx::lock_guard<stdx::mutex> lock(_oplogPinnedByBackupMutex);
        _oplogPinnedByBackup = boost::none;
    }
    stdx::lock_guard<stdx::mutex> backupDupCursorLk(_wtBackup.wtBackupDupCursorMutex);
    _wtBackup.cursor = nullptr;
    _wtBackup.dupCursor = nullptr;
    _wtBackup.logFilePathsSeenByExtendBackupCursor = {};
    _wtBackup.logFilePathsSeenByGetNextBatch = {};

    boost::filesystem::remove(getOngoingBackupPath());
}

StatusWith<std::deque<std::string>> WiredTigerKVEngine::extendBackupCursor() {
    uassert(51033, "Cannot extend backup cursor with in-memory mode.", !isEphemeral());
    stdx::lock_guard<stdx::mutex> backupCursorLk(_wtBackup.wtBackupCursorMutex);
    stdx::unique_lock<stdx::mutex> backupDupCursorLk(_wtBackup.wtBackupDupCursorMutex);
    invariant(_wtBackup.cursor);

    MONGO_IDLE_THREAD_BLOCK;
    _wtBackup.wtBackupDupCursorCV.wait(backupDupCursorLk, [&] { return !_wtBackup.dupCursor; });

    // Persist the sizeStorer information to disk before extending the backup cursor.
    syncSizeInfo(true);

    // The "target=(\"log:\")" configuration string for the cursor will ensure that we only see
    // the log files when iterating on the cursor.
    WT_CURSOR* cursor = nullptr;
    int wtRet =
        _backupSession->open_cursor(nullptr, _wtBackup.cursor, "target=(\"log:\")", &cursor);
    if (wtRet != 0) {
        return wtRCToStatus(wtRet, *_backupSession);
    }

    const char* filename;
    std::vector<std::string> filePaths;

    while ((wtRet = cursor->next(cursor)) == 0) {
        invariantWTOK(cursor->get_key(cursor, &filename), cursor->session);
        std::string name(filename);
        const boost::filesystem::path filePath = constructFilePath(_path, name);
        filePaths.push_back(filePath.string());
        _wtBackup.logFilePathsSeenByExtendBackupCursor.insert(filePath.string());
    }

    if (wtRet != WT_NOTFOUND) {
        return wtRCToStatus(wtRet, *_backupSession);
    }

    wtRet = cursor->close(cursor);
    if (wtRet != 0) {
        return wtRCToStatus(wtRet, *_backupSession);
    }

    // Once all the backup cursors have been opened on a sharded cluster, we need to ensure that
    // the data being copied from each shard is at the same point-in-time across the entire
    // cluster to have a consistent view of the data. For shards that opened their backup cursor
    // before the established point-in-time for backup, they will need to create a full copy of
    // the additional journal files returned by this method to ensure a consistent backup of the
    // data is taken.
    return getUniqueFiles(filePaths, _wtBackup.logFilePathsSeenByGetNextBatch);
}

void WiredTigerKVEngine::syncSizeInfo(bool sync) const {
    if (!_sizeStorer)
        return;

    while (true) {
        try {
            return _sizeStorer->flush(sync);
        } catch (const StorageUnavailableException& ex) {
            if (!sync) {
                // ignore, we'll try again later.
                LOGV2(7437300,
                      "Encountered storage unavailable error while flushing collection size "
                      "info, "
                      "will retry again later",
                      "error"_attr = ex.what());
                return;
            }
        }
    }
}

void WiredTigerKVEngine::setOldestActiveTransactionTimestampCallback(
    StorageEngine::OldestActiveTransactionTimestampCallback callback) {
    stdx::lock_guard<stdx::mutex> lk(_oldestActiveTransactionTimestampCallbackMutex);
    _oldestActiveTransactionTimestampCallback = std::move(callback);
};

std::unique_ptr<RecoveryUnit> WiredTigerKVEngine::newRecoveryUnit() {
    auto ru = std::make_unique<WiredTigerRecoveryUnit>(_connection.get());
    if (MONGO_unlikely(allowUntimestampedWrites(_inStandaloneMode.load(),
                                                _shouldRecoverFromOplogAsStandalone))) {
        ru->allowAllUntimestampedWrites();
    }
    return ru;
}

void WiredTigerKVEngine::setSortedDataInterfaceExtraOptions(const std::string& options) {
    _indexOptions = options;
}

Status WiredTigerKVEngine::_createRecordStore(const rss::PersistenceProvider& provider,
                                              const NamespaceString& nss,
                                              StringData ident,
                                              KeyFormat keyFormat,
                                              const BSONObj& storageEngineCollectionOptions,
                                              boost::optional<std::string> customBlockCompressor) {
    WiredTigerSession session(_connection.get());

    WiredTigerRecordStore::WiredTigerTableConfig wtTableConfig;
    wtTableConfig.keyFormat = keyFormat;
    wtTableConfig.blockCompressor = wiredTigerGlobalOptions.collectionBlockCompressor;
    wtTableConfig.extraCreateOptions = _rsOptions;
    wtTableConfig.logEnabled = WiredTigerUtil::useTableLogging(
        provider, nss, _isReplSet, _shouldRecoverFromOplogAsStandalone);

    if (customBlockCompressor) {
        wtTableConfig.blockCompressor = *customBlockCompressor;
    }

    auto customConfigString = WiredTigerRecordStore::parseOptionsField(
        storageEngineCollectionOptions.getObjectField(_canonicalName));
    if (!customConfigString.isOK()) {
        return customConfigString.getStatus();
    }

    // It's imperative that any custom options, beyond the default '_rsOptions' are appended at
    // the end of the 'extraCreateOptions' for table configuration. WiredTiger will take the
    // last value specified of a field in the config string. For example: if '_rsOptions' and
    // the 'customConfigString' both specify field 'blockCompressor=<value>', the latter <value>
    // will be used by WiredTiger.
    wtTableConfig.extraCreateOptions = str::stream()
        << _rsOptions << "," << customConfigString.getValue();

    std::string config = WiredTigerRecordStore::generateCreateString(
        NamespaceStringUtil::serializeForCatalog(nss), wtTableConfig, nss.isOplog());
    string uri = WiredTigerUtil::buildTableUri(ident);
    LOGV2_DEBUG(22331,
                2,
                "WiredTigerKVEngine::createRecordStore ns: {namespace} uri: {uri} config: {config}",
                logAttrs(nss),
                "uri"_attr = uri,
                "config"_attr = config);
    auto ensuredIdent = _ensureIdentPath(ident);
    return wtRCToStatus(session.create(uri.c_str(), config.c_str()), session);
}

Status WiredTigerKVEngine::importRecordStore(StringData ident,
                                             const BSONObj& storageMetadata,
                                             bool panicOnCorruptWtMetadata,
                                             bool repair) {
    WiredTigerSession session(_connection.get());

    std::string config = uassertStatusOK(WiredTigerUtil::generateImportString(
        ident, storageMetadata, panicOnCorruptWtMetadata, repair));

    string uri = WiredTigerUtil::buildTableUri(ident);
    LOGV2_DEBUG(5095102,
                2,
                "WiredTigerKVEngine::importRecordStore",
                "uri"_attr = uri,
                "config"_attr = config);

    auto ensuredIdent = _ensureIdentPath(ident);
    return wtRCToStatus(session.create(uri.c_str(), config.c_str()), session);
}

Status WiredTigerKVEngine::recoverOrphanedIdent(const rss::PersistenceProvider& provider,
                                                const NamespaceString& nss,
                                                StringData ident,
                                                const RecordStore::Options& options) {
#ifdef _WIN32
    return {ErrorCodes::CommandNotSupported, "Orphan file recovery is not supported on Windows"};
#else
    invariant(_inRepairMode);

    // Moves the data file to a temporary name so that a new RecordStore can be created with the
    // same ident name. We will delete the new empty collection and rename the data file back so
    // it can be salvaged.

    boost::optional<boost::filesystem::path> identFilePath = getDataFilePathForIdent(ident);
    if (!identFilePath) {
        return {ErrorCodes::UnknownError, "Data file for ident " + ident + " not found"};
    }

    boost::system::error_code ec;
    invariant(boost::filesystem::exists(*identFilePath, ec));

    boost::filesystem::path tmpFile{*identFilePath};
    tmpFile += ".tmp";

    LOGV2(22332,
          "Renaming data file to temporary",
          "file"_attr = identFilePath->generic_string(),
          "temporary"_attr = tmpFile.generic_string());
    auto status = fsyncRename(identFilePath.value(), tmpFile);
    if (!status.isOK()) {
        return status;
    }

    LOGV2(22333, "Creating new RecordStore", logAttrs(nss));

    status = createRecordStore(provider, nss, ident, options);
    if (!status.isOK()) {
        return status;
    }

    LOGV2(22334, "Restoring orphaned data file", "file"_attr = identFilePath->generic_string());

    boost::filesystem::remove(*identFilePath, ec);
    if (ec) {
        return {ErrorCodes::UnknownError, "Error deleting empty data file: " + ec.message()};
    }
    status = fsyncParentDirectory(*identFilePath);
    if (!status.isOK()) {
        return status;
    }

    status = fsyncRename(tmpFile, identFilePath.value());
    if (!status.isOK()) {
        return status;
    }

    auto start = Date_t::now();
    LOGV2(22335, "Salvaging ident", "ident"_attr = ident);

    WiredTigerSession session(_connection.get());
    status = wtRCToStatus(session.salvage(WiredTigerUtil::buildTableUri(ident).c_str(), nullptr),
                          session,
                          "Salvage failed: ");
    LOGV2(4795907, "Salvage complete", "duration"_attr = Date_t::now() - start);
    if (status.isOK()) {
        return {ErrorCodes::DataModifiedByRepair,
                str::stream() << "Salvaged data for ident " << ident};
    }
    LOGV2_WARNING(22354,
                  "Could not salvage data. Rebuilding ident",
                  "ident"_attr = ident,
                  "error"_attr = status.reason());

    //  If the data is unsalvageable, we should completely rebuild the ident.
    return _rebuildIdent(session, WiredTigerUtil::buildTableUri(ident).c_str());
#endif
}

std::unique_ptr<RecordStore> WiredTigerKVEngine::getRecordStore(OperationContext* opCtx,
                                                                const NamespaceString& nss,
                                                                StringData ident,
                                                                const RecordStore::Options& options,
                                                                boost::optional<UUID> uuid) {
    std::unique_ptr<WiredTigerRecordStore> ret;
    auto& provider = rss::ReplicatedStorageService::get(opCtx).getPersistenceProvider();
    if (options.isOplog) {
        const bool isLogged = WiredTigerUtil::useTableLogging(
            provider, nss, _isReplSet, _shouldRecoverFromOplogAsStandalone);
        ret = std::make_unique<WiredTigerRecordStore::Oplog>(
            this,
            WiredTigerRecoveryUnit::get(*shard_role_details::getRecoveryUnit(opCtx)),
            WiredTigerRecordStore::Oplog::Params{.uuid = *uuid,
                                                 .ident = std::string{ident},
                                                 .engineName = _canonicalName,
                                                 .inMemory = _wtConfig.inMemory,
                                                 .oplogMaxSize = options.oplogMaxSize,
                                                 .sizeStorer = _sizeStorer.get(),
                                                 .tracksSizeAdjustments = true,
                                                 .isLogged = isLogged,
                                                 .forceUpdateWithFullDocument =
                                                     options.forceUpdateWithFullDocument});
        getOplogManager()->stop();
        getOplogManager()->start(opCtx, *this, *ret, _isReplSet);
    } else {
        bool isLogged = [&] {
            if (!nss.isEmpty()) {
                return WiredTigerUtil::useTableLogging(
                    provider, nss, _isReplSet, _shouldRecoverFromOplogAsStandalone);
            }
            fassert(8423353, ident.starts_with("internal-"));
            return !_isReplSet && !_shouldRecoverFromOplogAsStandalone;
        }();
        WiredTigerRecordStore::Params params{
            .uuid = uuid,
            .ident = std::string{ident},
            .engineName = _canonicalName,
            .keyFormat = options.keyFormat,
            // Record stores for clustered collections need to guarantee uniqueness by
            // preventing overwrites.
            .overwrite = options.allowOverwrite,
            .isLogged = isLogged,
            .forceUpdateWithFullDocument = options.forceUpdateWithFullDocument,
            .inMemory = _wtConfig.inMemory,
            .sizeStorer = _sizeStorer.get(),
            .tracksSizeAdjustments = true,
        };

        ret = options.isCapped
            ? std::make_unique<WiredTigerRecordStore::Capped>(
                  this,
                  WiredTigerRecoveryUnit::get(*shard_role_details::getRecoveryUnit(opCtx)),
                  params)
            : std::make_unique<WiredTigerRecordStore>(
                  this,
                  WiredTigerRecoveryUnit::get(*shard_role_details::getRecoveryUnit(opCtx)),
                  params);
    }

    if (sizeRecoveryState(opCtx->getServiceContext()).shouldRecordStoresAlwaysCheckSize()) {
        ret->checkSize(opCtx, *shard_role_details::getRecoveryUnit(opCtx));
    }

    return std::move(ret);
}

Status WiredTigerKVEngine::createSortedDataInterface(
    const rss::PersistenceProvider& provider,
    RecoveryUnit& ru,
    const NamespaceString& nss,
    const UUID& uuid,
    StringData ident,
    const IndexConfig& indexConfig,
    const boost::optional<mongo::BSONObj>& storageEngineIndexOptions) {

    std::string collIndexOptions;

    if (storageEngineIndexOptions) {
        collIndexOptions = ::mongo::bson::extractElementAtDottedPath(
                               *storageEngineIndexOptions, _canonicalName + ".configString")
                               .str();
    }

    StatusWith<std::string> result = WiredTigerIndex::generateCreateString(
        _canonicalName,
        _indexOptions,
        collIndexOptions,
        NamespaceStringUtil::serializeForCatalog(nss),
        indexConfig,
        WiredTigerUtil::useTableLogging(
            provider, nss, _isReplSet, _shouldRecoverFromOplogAsStandalone));
    if (!result.isOK()) {
        return result.getStatus();
    }

    std::string config = result.getValue();

    LOGV2_DEBUG(
        22336,
        2,
        "WiredTigerKVEngine::createSortedDataInterface uuid: {collection_uuid} ident: {ident} "
        "config: {config}",
        logAttrs(uuid),
        "ident"_attr = ident,
        "config"_attr = config);
    auto ensuredIdent = _ensureIdentPath(ident);
    return WiredTigerIndex::create(
        WiredTigerRecoveryUnit::get(ru), WiredTigerUtil::buildTableUri(ident), config);
}

Status WiredTigerKVEngine::importSortedDataInterface(RecoveryUnit& ru,
                                                     StringData ident,
                                                     const BSONObj& storageMetadata,
                                                     bool panicOnCorruptWtMetadata,
                                                     bool repair) {

    std::string config = uassertStatusOK(WiredTigerUtil::generateImportString(
        ident, storageMetadata, panicOnCorruptWtMetadata, repair));

    LOGV2_DEBUG(5095103,
                2,
                "WiredTigerKVEngine::importSortedDataInterface",
                "ident"_attr = ident,
                "config"_attr = config);
    if (WTFailImportSortedDataInterface.shouldFail()) {
        return Status(ErrorCodes::UnknownError, "WTFailImportSortedDataInterface Mock Error");
    }
    auto ensuredIdent = _ensureIdentPath(ident);
    return WiredTigerIndex::create(
        WiredTigerRecoveryUnit::get(ru), WiredTigerUtil::buildTableUri(ident), config);
}

Status WiredTigerKVEngine::dropSortedDataInterface(RecoveryUnit& ru, StringData ident) {
    return WiredTigerIndex::Drop(WiredTigerRecoveryUnit::get(ru),
                                 WiredTigerUtil::buildTableUri(ident));
}

std::unique_ptr<SortedDataInterface> WiredTigerKVEngine::getSortedDataInterface(
    OperationContext* opCtx,
    RecoveryUnit& ru,
    const NamespaceString& nss,
    const UUID& uuid,
    StringData ident,
    const IndexConfig& config,
    KeyFormat keyFormat) {
    auto& provider = rss::ReplicatedStorageService::get(opCtx).getPersistenceProvider();

    if (config.isIdIndex) {
        return std::make_unique<WiredTigerIdIndex>(
            opCtx,
            ru,
            WiredTigerUtil::buildTableUri(ident),
            uuid,
            ident,
            config,
            WiredTigerUtil::useTableLogging(
                provider, nss, _isReplSet, _shouldRecoverFromOplogAsStandalone));
    }
    if (config.unique) {
        return std::make_unique<WiredTigerIndexUnique>(
            opCtx,
            ru,
            WiredTigerUtil::buildTableUri(ident),
            uuid,
            ident,
            keyFormat,
            config,
            WiredTigerUtil::useTableLogging(
                provider, nss, _isReplSet, _shouldRecoverFromOplogAsStandalone));
    }

    return std::make_unique<WiredTigerIndexStandard>(
        opCtx,
        ru,
        WiredTigerUtil::buildTableUri(ident),
        uuid,
        ident,
        keyFormat,
        config,
        WiredTigerUtil::useTableLogging(
            provider, nss, _isReplSet, _shouldRecoverFromOplogAsStandalone));
}

std::unique_ptr<RecordStore> WiredTigerKVEngine::getTemporaryRecordStore(RecoveryUnit& ru,
                                                                         StringData ident,
                                                                         KeyFormat keyFormat) {
    // We don't log writes to temporary record stores.
    const bool isLogged = false;
    WiredTigerRecordStore::Params params;
    params.uuid = boost::none;
    params.ident = std::string{ident};
    params.engineName = _canonicalName;
    params.keyFormat = keyFormat;
    params.overwrite = true;
    params.isLogged = isLogged;
    params.forceUpdateWithFullDocument = false;
    params.inMemory = _wtConfig.inMemory;
    // Temporary collections do not need to persist size information to the size storer.
    params.sizeStorer = nullptr;
    // Temporary collections do not need to reconcile collection size/counts.
    params.tracksSizeAdjustments = false;
    return std::make_unique<WiredTigerRecordStore>(this, WiredTigerRecoveryUnit::get(ru), params);
}

std::unique_ptr<RecordStore> WiredTigerKVEngine::makeTemporaryRecordStore(RecoveryUnit& ru,
                                                                          StringData ident,
                                                                          KeyFormat keyFormat) {
    WiredTigerSession session(_connection.get());

    WiredTigerRecordStore::WiredTigerTableConfig wtTableConfig;
    wtTableConfig.keyFormat = keyFormat;
    wtTableConfig.blockCompressor = wiredTigerGlobalOptions.collectionBlockCompressor;
    // We don't log writes to temporary record stores.
    wtTableConfig.logEnabled = false;
    wtTableConfig.extraCreateOptions = _rsOptions;

    std::string config =
        WiredTigerRecordStore::generateCreateString({} /* internal table */, wtTableConfig);

    std::string uri = WiredTigerUtil::buildTableUri(ident);
    LOGV2_DEBUG(22337,
                2,
                "WiredTigerKVEngine::makeTemporaryRecordStore",
                "uri"_attr = uri,
                "config"_attr = config);
    {
        auto ensuredIdent = _ensureIdentPath(ident);
        uassertStatusOK(wtRCToStatus(session.create(uri.c_str(), config.c_str()), session));
    }

    return getTemporaryRecordStore(ru, ident, keyFormat);
}

void WiredTigerKVEngine::alterIdentMetadata(RecoveryUnit& ru,
                                            StringData ident,
                                            const IndexConfig& config,
                                            bool isForceUpdateMetadata) {
    std::string uri = WiredTigerUtil::buildTableUri(ident);
    if (!isForceUpdateMetadata) {
        // Explicitly disallows metadata change, specifically index data format change, on
        // indexes of version 11 and 12. This is extra defensive and can be reconsidered if we
        // expand the use of 'alterIdentMetadata()' to also modify non-data-format properties.
        invariant(!WiredTigerUtil::checkApplicationMetadataFormatVersion(
                       *WiredTigerRecoveryUnit::get(ru).getSessionNoTxn(),
                       uri,
                       kDataFormatV3KeyStringV0UniqueIndexVersionV1,
                       kDataFormatV4KeyStringV1UniqueIndexVersionV2)
                       .isOK());
    }

    // Make the alter call to update metadata without taking exclusive lock to avoid conflicts
    // with concurrent operations.
    std::string alterString =
        WiredTigerIndex::generateAppMetadataString(config) + "exclusive_refreshed=false,";
    auto status = alterMetadata(uri, alterString);
    invariantStatusOK(status);
}

Status WiredTigerKVEngine::alterMetadata(StringData uri, StringData config) {
    // Use a dedicated session in an alter operation to avoid transaction issues.
    WiredTigerSession session(_connection.get());

    auto uriNullTerminated = std::string{uri};
    auto configNullTerminated = std::string{config};

    auto ret = session.alter(uriNullTerminated.c_str(), configNullTerminated.c_str());
    // WT may return EBUSY if the database contains dirty data. If we checkpoint and retry the
    // operation it will attempt to clean up the dirty elements during checkpointing, thus
    // allowing the operation to succeed if it was the only reason to fail.
    if (ret == EBUSY) {
        _checkpoint(session);
        ret = session.alter(uriNullTerminated.c_str(), configNullTerminated.c_str());
    }

    return wtRCToStatus(ret, session);
}

Status WiredTigerKVEngine::dropIdent(RecoveryUnit& ru,
                                     StringData ident,
                                     bool identHasSizeInfo,
                                     const StorageEngine::DropIdentCallback& onDrop) {
    string uri = WiredTigerUtil::buildTableUri(ident);

    auto& wtRu = WiredTigerRecoveryUnit::get(ru);
    wtRu.getSessionNoTxn()->closeAllCursors(uri);

    WiredTigerSession session(_connection.get());

    Status status = _drop(session, uri.c_str(), "checkpoint_wait=false");
    LOGV2_DEBUG(22338, 1, "WT drop", "uri"_attr = uri, "status"_attr = status);

    if (status == ErrorCodes::ObjectIsBusy) {
        return status;
    }
    if (MONGO_unlikely(WTDropEBUSY.shouldFail())) {
        return {ErrorCodes::ObjectIsBusy,
                str::stream() << "Failed to remove drop-pending ident " << ident};
    }

    if (identHasSizeInfo) {
        _sizeStorer->remove(uri);
    }

    _removeIdentDirectoryIfEmpty(ident);

    if (onDrop) {
        onDrop();
    }

    return status;
}

void WiredTigerKVEngine::dropIdentForImport(Interruptible& interruptible,
                                            RecoveryUnit& ru,
                                            StringData ident) {
    const std::string uri = WiredTigerUtil::buildTableUri(ident);

    WiredTigerRecoveryUnit* wtRu = checked_cast<WiredTigerRecoveryUnit*>(&ru);
    wtRu->getSessionNoTxn()->closeAllCursors(uri);

    WiredTigerSession session(_connection.get());

    // Don't wait for the global checkpoint lock to be obtained in WiredTiger as it can take a
    // substantial amount of time to be obtained if there is a concurrent checkpoint running. We
    // will wait until we obtain exclusive access to the underlying table file though. As it
    // isn't user visible at this stage in the import it should be readily available unless a
    // backup cursor is open. In short, using "checkpoint_wait=false" and "lock_wait=true" means
    // that we can potentially be waiting for a short period of time for WT_SESSION::drop() to
    // run, but would rather get EBUSY than wait a long time for a checkpoint to complete.
    const std::string config = "checkpoint_wait=false,lock_wait=true,remove_files=false";
    Status dropStatus = Status::OK();
    size_t attempt = 0;
    do {
        Status interruptibleStatus = interruptible.checkForInterruptNoAssert();
        if (interruptibleStatus.code() == ErrorCodes::InterruptedAtShutdown) {
            return;
        }

        ++attempt;

        dropStatus = _drop(session, uri.c_str(), config.c_str());
        logAndBackoff(5114600,
                      ::mongo::logv2::LogComponent::kStorage,
                      logv2::LogSeverity::Debug(1),
                      attempt,
                      "WiredTiger dropping ident for import",
                      "uri"_attr = uri,
                      "config"_attr = config,
                      "status"_attr = dropStatus);
    } while (dropStatus == ErrorCodes::ObjectIsBusy);
    invariant(dropStatus);
}

void WiredTigerKVEngine::_checkpoint(WiredTigerSession& session, bool useTimestamp) {
    _currentCheckpointIteration.fetchAndAdd(1);
    int wtRet;
    size_t attempt = 0;
    while (true) {
        if (useTimestamp) {
            wtRet = session.checkpoint("use_timestamp=true");
        } else {
            invariant(_wtConfig.providerSupportsUnstableCheckpoints);
            wtRet = session.checkpoint("use_timestamp=false");
        }
        if (EBUSY == wtRet) {
            logAndBackoff(9787200,
                          ::mongo::logv2::LogComponent::kStorage,
                          logv2::LogSeverity::Info(),
                          ++attempt,
                          "Checkpoint returned EBUSY, retrying",
                          "use_timestamp"_attr = useTimestamp);
        } else {
            break;
        }
    }
    invariantWTOK(wtRet, session);
    auto checkpointedIteration = _finishedCheckpointIteration.fetchAndAdd(1);
    LOGV2_FOR_RECOVERY(8097402,
                       2,
                       "Finished checkpoint, updated iteration counter",
                       "checkpointIteration"_attr = checkpointedIteration);
}

void WiredTigerKVEngine::_checkpoint(WiredTigerSession& session) try {
    // Ephemeral WiredTiger instances do not checkpoint to disk.
    if (isEphemeral()) {
        return;
    }

    // Limits the actions of concurrent checkpoint callers as we update some internal data
    // during a checkpoint. WT has a mutex of its own to only have one checkpoint active at all
    // times so this is only to protect our internal updates.
    // TODO: SERVER-64507: Investigate whether we can smartly rely on one checkpointer if two or
    // more threads checkpoint at the same time.
    stdx::lock_guard lk(_checkpointMutex);

    const Timestamp stableTimestamp = getStableTimestamp();
    const Timestamp initialDataTimestamp = getInitialDataTimestamp();

    // The amount of oplog to keep is primarily dictated by a user setting. However, in
    // unexpected cases, durable, recover to a timestamp storage engines may need to play
    // forward from an oplog entry that would otherwise be truncated by the user setting.
    // Furthermore, the entries in prepared or large transactions can refer to previous entries
    // in the same transaction.
    //
    // Live (replication) rollback will replay the oplog from exactly the stable timestamp. With
    // prepared or large transactions, it may require some additional entries prior to the
    // stable timestamp. These requirements are summarized in getOplogNeededForRollback.
    // Truncating the oplog at this point is sufficient for in-memory configurations, but could
    // cause an unrecoverable scenario if the node crashed and has to play from the last stable
    // checkpoint.
    //
    // By recording the oplog needed for rollback "now", then taking a stable checkpoint, we can
    // safely assume that the oplog needed for crash recovery has caught up to the recorded
    // value. After the checkpoint, this value will be published such that actors which truncate
    // the oplog can read an updated value.

    // Three cases:
    //
    // First, initialDataTimestamp is Timestamp(0, 1) -> Take full checkpoint. This is when
    // there is no consistent view of the data (e.g: during initial sync).
    //
    // Second, stableTimestamp < initialDataTimestamp: Skip checkpoints. The data on disk is
    // prone to being rolled back. Hold off on checkpoints.  Hope that the stable timestamp
    // surpasses the data on disk, allowing storage to persist newer copies to disk.
    //
    // Third, stableTimestamp >= initialDataTimestamp: Take stable checkpoint. Steady state
    // case.
    if (initialDataTimestamp.asULL() <= 1) {
        _checkpoint(session, /*useTimestamp=*/false);

        LOGV2_FOR_RECOVERY(5576602,
                           2,
                           "Completed unstable checkpoint.",
                           "initialDataTimestamp"_attr = initialDataTimestamp.toString());
    } else if (stableTimestamp < initialDataTimestamp) {
        LOGV2_FOR_RECOVERY(
            23985,
            2,
            "Stable timestamp is behind the initial data timestamp, skipping a checkpoint.",
            "stableTimestamp"_attr = stableTimestamp.toString(),
            "initialDataTimestamp"_attr = initialDataTimestamp.toString());
    } else if (!_wtConfig.safeToTakeDuplicateCheckpoints &&
               stableTimestamp == getCheckpointTimestamp()) {
        LOGV2_FOR_RECOVERY(10985349,
                           2,
                           "Stable timestamp hasn't advanced, skipping a checkpoint.",
                           "stableTimestamp"_attr = stableTimestamp);
    } else {
        auto oplogNeededForRollback = getOplogNeededForRollback();

        LOGV2_FOR_RECOVERY(23986,
                           2,
                           "Performing stable checkpoint.",
                           "stableTimestamp"_attr = stableTimestamp,
                           "oplogNeededForRollback"_attr = toString(oplogNeededForRollback));

        _checkpoint(session, /*useTimestamp=*/true);

        if (oplogNeededForRollback.isOK()) {
            // Now that the checkpoint is durable, publish the oplog needed to recover from it.
            _oplogNeededForCrashRecovery.store(oplogNeededForRollback.getValue().asULL());
        }
    }
} catch (const StorageUnavailableException&) {
    LOGV2_WARNING(7754200, "Checkpoint encountered a StorageUnavailableException.");
} catch (const AssertionException& exc) {
    invariant(ErrorCodes::isShutdownError(exc.code()), exc.what());
}

void WiredTigerKVEngine::checkpoint() {
    WiredTigerManagedSession session = _connection->getUninterruptibleSession();
    return _checkpoint(*session);
}

void WiredTigerKVEngine::forceCheckpoint(bool useStableTimestamp) {
    WiredTigerManagedSession session = _connection->getUninterruptibleSession();
    if (_wtConfig.safeToTakeDuplicateCheckpoints)
        return _checkpoint(*session, useStableTimestamp);
    else {
        invariant(useStableTimestamp);
        // The checkpoint call above ensures that concurrent checkpoint requests are serialized
        // at the WT layer. However, sometimes we need to serialize checkpoint requests at the
        // MDB layer, which this checkpoint call provides. This prevents issuing a new
        // checkpoint for a stable timestamp that has already been checkpointed.
        return _checkpoint(*session);
    }
}

bool WiredTigerKVEngine::hasIdent(RecoveryUnit& ru, StringData ident) const {
    return _wtHasUri(*WiredTigerRecoveryUnit::get(ru).getSession(),
                     WiredTigerUtil::buildTableUri(ident));
}

std::vector<std::string> WiredTigerKVEngine::getAllIdents(RecoveryUnit& ru) const {
    auto& wtRu = WiredTigerRecoveryUnit::get(ru);
    return _wtGetAllIdents(*wtRu.getSession());
}

boost::optional<boost::filesystem::path> WiredTigerKVEngine::getDataFilePathForIdent(
    StringData ident) const {
    boost::filesystem::path identPath = _path;
    identPath /= std::string{ident} + ".wt";

    boost::system::error_code ec;
    if (!boost::filesystem::exists(identPath, ec)) {
        return boost::none;
    }
    return identPath;
}

stdx::unique_lock<stdx::mutex> WiredTigerKVEngine::_ensureIdentPath(StringData ident) {
    size_t idx = ident.find('/');
    if (idx == string::npos) {
        // If there is no directory for this ident, we don't need to take the lock.
        return {};
    }
    stdx::unique_lock<stdx::mutex> directoryModifyLock(_directoryModificationMutex);
    do {
        StringData dir = ident.substr(0, idx);

        boost::filesystem::path subdir = _path;
        subdir /= std::string{dir};
        if (!boost::filesystem::exists(subdir)) {
            LOGV2_DEBUG(22341, 1, "creating subdirectory: {dir}", "dir"_attr = dir);
            try {
                boost::filesystem::create_directory(subdir);
            } catch (const std::exception& e) {
                LOGV2_ERROR(22361,
                            "Error creating directory",
                            "directory"_attr = subdir.string(),
                            "error"_attr = e.what());
                throw;
            }
        }
        idx = ident.find('/', idx + 1);
    } while (idx != string::npos);
    return directoryModifyLock;
}

bool WiredTigerKVEngine::_removeIdentDirectoryIfEmpty(StringData ident, size_t startPos) {
    size_t separatorPos = ident.find('/', startPos);
    if (separatorPos == string::npos) {
        return true;
    }
    if (!_removeIdentDirectoryIfEmpty(ident, separatorPos + 1)) {
        return false;
    }
    boost::filesystem::path subdir = _path;
    subdir /= std::string{ident.substr(0, separatorPos)};
    stdx::unique_lock<stdx::mutex> directoryModifyLock(_directoryModificationMutex);
    if (!boost::filesystem::exists(subdir)) {
        return true;
    }
    if (!boost::filesystem::is_empty(subdir)) {
        return false;
    }
    boost::system::error_code ec;
    boost::filesystem::remove(subdir, ec);
    if (!ec) {
        LOGV2(4888200, "Removed empty ident directory", "path"_attr = subdir.string());
        return true;
    }
    // Failing to clean up an empty directory, whilst not ideal, is not a real problem.
    LOGV2_DEBUG(4888201,
                1,
                "Failed to remove empty ident directory",
                "path"_attr = subdir.string(),
                "error"_attr = ec.message());
    return false;
}

void WiredTigerKVEngine::setJournalListener(JournalListener* jl) {
    stdx::unique_lock<stdx::mutex> lk(_journalListenerMutex);

    // A JournalListener can only be set once. Otherwise, accessing a copy of the
    // _journalListener pointer without a mutex would be unsafe.
    invariant(!_journalListener);

    _journalListener = jl;
}

void WiredTigerKVEngine::setLastMaterializedLsn(uint64_t lsn) {
    invariantWTOK(_conn->set_context_uint(_conn, WT_CONTEXT_TYPE_LAST_MATERIALIZED_LSN, lsn),
                  nullptr);
}

void WiredTigerKVEngine::setRecoveryCheckpointMetadata(StringData checkpointMetadata) {
    auto getCkptMetaConfigString =
        fmt::format("disaggregated=(checkpoint_meta=\"{}\")", checkpointMetadata);
    invariantWTOK(_conn->reconfigure(_conn, getCkptMetaConfigString.c_str()), nullptr);
}

void WiredTigerKVEngine::promoteToLeader() {
    static constexpr char leaderConfig[] = "disaggregated=(role=\"leader\")";
    invariantWTOK(_conn->reconfigure(_conn, leaderConfig), nullptr);
}

void WiredTigerKVEngine::setStableTimestamp(Timestamp stableTimestamp, bool force) {
    if (stableTimestamp.isNull()) {
        return;
    }

    // Do not set the stable timestamp backward, unless 'force' is set.
    Timestamp prevStable(_stableTimestamp.load());
    if ((stableTimestamp < prevStable) && !force) {
        return;
    }

    // Communicate to WiredTiger what the "stable timestamp" is. Timestamp-aware checkpoints
    // will only persist to disk transactions committed with a timestamp earlier than the
    // "stable timestamp".
    //
    // After passing the "stable timestamp" to WiredTiger, communicate it to the
    // `CheckpointThread`. It's not obvious a stale stable timestamp in the `CheckpointThread`
    // is safe. Consider the following arguments:
    //
    // Setting the "stable timestamp" is only meaningful when the "initial data timestamp" is
    // real (i.e: not `kAllowUnstableCheckpointsSentinel`). In this normal case, the
    // `stableTimestamp` input must be greater than the current value. The only effect this can
    // have in the `CheckpointThread` is to transition it from a state of not taking any
    // checkpoints, to taking "stable checkpoints". In the transitioning case, it's imperative
    // for the "stable timestamp" to have first been communicated to WiredTiger.
    std::string stableTSConfigString;
    auto ts = stableTimestamp.asULL();
    if (force) {
        stableTSConfigString = fmt::format(
            "force=true,oldest_timestamp={0:x},durable_timestamp={0:x},stable_timestamp={0:x}", ts);
    } else {
        stableTSConfigString = fmt::format("stable_timestamp={:x}", ts);
    }
    invariantWTOK(_conn->set_timestamp(_conn, stableTSConfigString.c_str()), nullptr);

    // After publishing a stable timestamp to WT, we can record the updated stable timestamp
    // value for the necessary oplog to keep.
    _stableTimestamp.store(stableTimestamp.asULL());

    // If 'force' is set, then we have already set the oldest timestamp equal to the stable
    // timestamp, so there is nothing left to do.
    if (force) {
        return;
    }

    // Forward the oldest timestamp so that WiredTiger can clean up earlier timestamp data.
    setOldestTimestampFromStable();
}

void WiredTigerKVEngine::setOldestTimestampFromStable() {
    Timestamp stableTimestamp(_stableTimestamp.load());

    // Set the oldest timestamp to the stable timestamp to ensure that there is no lag window
    // between the two.
    if (MONGO_unlikely(WTSetOldestTSToStableTS.shouldFail())) {
        setOldestTimestamp(stableTimestamp, false);
        return;
    }

    // Calculate what the oldest_timestamp should be from the stable_timestamp. The oldest
    // timestamp should lag behind stable by 'minSnapshotHistoryWindowInSeconds' to create a
    // window of available snapshots. If the lag window is not yet large enough, we will not
    // update/forward the oldest_timestamp yet and instead return early.
    Timestamp newOldestTimestamp = _calculateHistoryLagFromStableTimestamp(stableTimestamp);
    if (newOldestTimestamp.isNull()) {
        return;
    }

    setOldestTimestamp(newOldestTimestamp, false);
}

void WiredTigerKVEngine::setOldestTimestamp(Timestamp newOldestTimestamp, bool force) {
    if (MONGO_unlikely(WTPreserveSnapshotHistoryIndefinitely.shouldFail())) {
        return;
    }

    // This mutex is not intended to synchronize updates to the oldest timestamp, but to ensure
    // that there are no races with pinning the oldest timestamp.
    stdx::lock_guard<stdx::mutex> lock(_oldestTimestampPinRequestsMutex);
    const Timestamp currOldestTimestamp = Timestamp(_oldestTimestamp.load());
    for (const auto& it : _oldestTimestampPinRequests) {
        invariant(it.second >= currOldestTimestamp);
        newOldestTimestamp = std::min(newOldestTimestamp, it.second);
    }

    if (force) {
        // Components that register a pinned timestamp must synchronize with events that
        // invalidate their snapshots, unpin themselves and either fail themselves, or reacquire
        // a new snapshot after the rollback event.
        //
        // Forcing the oldest timestamp forward -- potentially past a pin request raises the
        // question of whether the pin should be honored. For now we will invariant there is no
        // pin, but the invariant can be relaxed if there's a use-case to support.
        invariant(_oldestTimestampPinRequests.empty());
    }

    if (force) {
        auto oldestTSConfigString =
            fmt::format("force=true,oldest_timestamp={0:x},durable_timestamp={0:x}",
                        newOldestTimestamp.asULL());
        invariantWTOK(_conn->set_timestamp(_conn, oldestTSConfigString.c_str()), nullptr);
        _oldestTimestamp.store(newOldestTimestamp.asULL());

        LOGV2_DEBUG(22342,
                    2,
                    "oldest_timestamp and durable_timestamp force set to {newOldestTimestamp}",
                    "newOldestTimestamp"_attr = newOldestTimestamp);
    } else {
        auto oldestTSConfigString =
            fmt::format("oldest_timestamp={:x}", newOldestTimestamp.asULL());
        invariantWTOK(_conn->set_timestamp(_conn, oldestTSConfigString.c_str()), nullptr);
        // set_timestamp above ignores backwards in time if 'force' is not set.
        if (_oldestTimestamp.load() < newOldestTimestamp.asULL())
            _oldestTimestamp.store(newOldestTimestamp.asULL());
        LOGV2_DEBUG(22343,
                    2,
                    "oldest_timestamp set to {newOldestTimestamp}",
                    "newOldestTimestamp"_attr = newOldestTimestamp);
    }
}

Timestamp WiredTigerKVEngine::_calculateHistoryLagFromStableTimestamp(Timestamp stableTimestamp) {
    // The oldest_timestamp should lag behind the stable_timestamp by
    // 'minSnapshotHistoryWindowInSeconds' seconds.

    if (isEphemeral() && !TestingProctor::instance().isEnabled()) {
        // No history should be maintained for an ephemeral engine because it is not used yet.
        invariant(minSnapshotHistoryWindowInSeconds.load() == 0);
    }

    if (stableTimestamp.getSecs() <
        static_cast<unsigned>(minSnapshotHistoryWindowInSeconds.load())) {
        // The history window is larger than the timestamp history thus far. We must wait for
        // the history to reach the window size before moving oldest_timestamp forward. This
        // should only happen in unit tests.
        return Timestamp();
    }

    Timestamp calculatedOldestTimestamp(stableTimestamp.getSecs() -
                                            minSnapshotHistoryWindowInSeconds.load(),
                                        stableTimestamp.getInc());

    if (calculatedOldestTimestamp.asULL() <= _oldestTimestamp.load()) {
        // The stable_timestamp is not far enough ahead of the oldest_timestamp for the
        // oldest_timestamp to be moved forward: the window is still too small.
        return Timestamp();
    }

    // The oldest timestamp cannot be set behind the `_initialDataTimestamp`.
    if (calculatedOldestTimestamp.asULL() <= _initialDataTimestamp.load()) {
        calculatedOldestTimestamp = Timestamp(_initialDataTimestamp.load());
    }

    return calculatedOldestTimestamp;
}

void WiredTigerKVEngine::setInitialDataTimestamp(Timestamp initialDataTimestamp) {
    LOGV2_DEBUG(22344,
                2,
                "Setting initial data timestamp. Value: {initialDataTimestamp}",
                "initialDataTimestamp"_attr = initialDataTimestamp);
    _initialDataTimestamp.store(initialDataTimestamp.asULL());
}

Timestamp WiredTigerKVEngine::getInitialDataTimestamp() const {
    return Timestamp(_initialDataTimestamp.load());
}

bool WiredTigerKVEngine::supportsRecoverToStableTimestamp() const {
    return true;
}

bool WiredTigerKVEngine::supportsRecoveryTimestamp() const {
    return true;
}

bool WiredTigerKVEngine::_canRecoverToStableTimestamp() const {
    static const std::uint64_t allowUnstableCheckpointsSentinel =
        static_cast<std::uint64_t>(Timestamp::kAllowUnstableCheckpointsSentinel.asULL());
    const std::uint64_t initialDataTimestamp = _initialDataTimestamp.load();
    // Illegal to be called when the dataset is incomplete.
    invariant(initialDataTimestamp > allowUnstableCheckpointsSentinel);
    return _stableTimestamp.load() >= initialDataTimestamp;
}

StatusWith<Timestamp> WiredTigerKVEngine::recoverToStableTimestamp(Interruptible& interruptible) {
    if (!supportsRecoverToStableTimestamp()) {
        LOGV2_FATAL(50665, "WiredTiger is configured to not support recover to a stable timestamp");
    }

    if (!_canRecoverToStableTimestamp()) {
        Timestamp stableTS(_stableTimestamp.load());
        Timestamp initialDataTS(_initialDataTimestamp.load());
        if (MONGO_unlikely(hangBeforeUnrecoverableRollbackError.shouldFail())) {
            LOGV2(6718000, "Hit hangBeforeUnrecoverableRollbackError failpoint");
            hangBeforeUnrecoverableRollbackError.pauseWhileSet(&interruptible);
        }
        return Status(ErrorCodes::UnrecoverableRollbackError,
                      str::stream()
                          << "No stable timestamp available to recover to. Initial data timestamp: "
                          << initialDataTS.toString()
                          << ", Stable timestamp: " << stableTS.toString());
    }

    LOGV2_FOR_ROLLBACK(
        23989, 2, "WiredTiger::RecoverToStableTimestamp syncing size storer to disk.");
    syncSizeInfo(true);

    const Timestamp stableTimestamp(_stableTimestamp.load());
    const Timestamp initialDataTimestamp(_initialDataTimestamp.load());

    LOGV2_FOR_ROLLBACK(23991,
                       0,
                       "Rolling back to the stable timestamp",
                       "stableTimestamp"_attr = stableTimestamp,
                       "initialDataTimestamp"_attr = initialDataTimestamp);
    int ret = 0;

    // Shut down the cache before rollback and restart afterwards.
    _connection->shuttingDown();

    // The rollback_to_stable operation requires all open cursors to be closed or reset before
    // the call, otherwise EBUSY will be returned. Occasionally, there could be an operation
    // that hasn't been killed yet, such as the CappedInsertNotifier for a yielded oplog
    // getMore. We will retry rollback_to_stable until the system quiesces.
    size_t attempts = 0;
    do {
        ret = _conn->rollback_to_stable(_conn, nullptr);
        if (ret != EBUSY) {
            break;
        }

        if (MONGO_unlikely(WTRollbackToStableReturnOnEBUSY.shouldFail())) {
            return wtRCToStatus(ret, nullptr);
        }

        LOGV2_FOR_ROLLBACK(
            6398900, 0, "Retrying rollback to stable due to EBUSY", "attempts"_attr = ++attempts);
        interruptible.sleepFor(Seconds(1));
    } while (ret == EBUSY);

    LOGV2_FOR_ROLLBACK(9529900,
                       0,
                       "Rolling back to the stable timestamp completed by storage engine",
                       "attempts"_attr = attempts);

    if (ret) {
        // Dump the storage engine's internal state to assist in diagnosis.
        dump();

        return {ErrorCodes::UnrecoverableRollbackError,
                str::stream() << "Error rolling back to stable. Err: " << wiredtiger_strerror(ret)};
    }

    _sizeStorer = std::make_unique<WiredTigerSizeStorer>(_connection.get(), _sizeStorerUri);

    // SERVER-85167: restart the cache after resetting the size storer.
    _connection->restart();

    return {stableTimestamp};
}

Timestamp WiredTigerKVEngine::getAllDurableTimestamp() const {
    // Fetch the latest all_durable value from the storage engine. This value will be a
    // timestamp that has no holes (uncommitted transactions with lower timestamps) behind it.
    char buf[(2 * 8 /* bytes in hex */) + 1 /* null terminator */];
    invariantWTOK(_conn->query_timestamp(_conn, buf, "get=all_durable"), nullptr);

    uint64_t ts;
    fassert(38002, NumberParser{}.base(16)(buf, &ts));

    // If all_durable is 0, treat this as lowest possible timestamp; we need to see all
    // pre-existing data but no new (timestamped) data.
    return Timestamp{ts == 0 ? StorageEngine::kMinimumTimestamp : ts};
}

boost::optional<Timestamp> WiredTigerKVEngine::getRecoveryTimestamp() const {
    if (!supportsRecoveryTimestamp()) {
        LOGV2_FATAL(50745,
                    "WiredTiger is configured to not support providing a recovery timestamp");
    }

    if (_recoveryTimestamp.isNull()) {
        return boost::none;
    }

    return _recoveryTimestamp;
}

boost::optional<Timestamp> WiredTigerKVEngine::getLastStableRecoveryTimestamp() const {
    if (isEphemeral()) {
        Timestamp stable(_stableTimestamp.load());
        Timestamp initialData(_initialDataTimestamp.load());
        if (stable.isNull() || stable < initialData) {
            return boost::none;
        }
        return stable;
    }

    const auto ret = _getCheckpointTimestamp();
    if (ret) {
        return Timestamp(ret);
    }

    if (!_recoveryTimestamp.isNull()) {
        return _recoveryTimestamp;
    }

    return boost::none;
}

StatusWith<Timestamp> WiredTigerKVEngine::getOplogNeededForRollback() const {
    // Get the current stable timestamp and use it throughout this function, ignoring updates
    // from another thread.
    auto stableTimestamp = _stableTimestamp.load();

    // Only one thread can set or execute this callback.
    stdx::lock_guard<stdx::mutex> lk(_oldestActiveTransactionTimestampCallbackMutex);
    boost::optional<Timestamp> oldestActiveTransactionTimestamp;
    if (_oldestActiveTransactionTimestampCallback) {
        auto status = _oldestActiveTransactionTimestampCallback(Timestamp(stableTimestamp));
        if (status.isOK()) {
            oldestActiveTransactionTimestamp.swap(status.getValue());
        } else {
            LOGV2_DEBUG(22345,
                        1,
                        "getting oldest active transaction timestamp: {status_getStatus}",
                        "status_getStatus"_attr = status.getStatus());
            return status.getStatus();
        }
    }

    if (oldestActiveTransactionTimestamp) {
        return std::min(oldestActiveTransactionTimestamp.value(), Timestamp(stableTimestamp));
    } else {
        return Timestamp(stableTimestamp);
    }
}

boost::optional<Timestamp> WiredTigerKVEngine::getOplogNeededForCrashRecovery() const {
    if (isEphemeral()) {
        return boost::none;
    }

    return Timestamp(_oplogNeededForCrashRecovery.load());
}

Timestamp WiredTigerKVEngine::getPinnedOplog() const {
    // The storage engine may have been told to keep oplog back to a certain timestamp.
    Timestamp pinned = Timestamp(_pinnedOplogTimestamp.load());

    {
        stdx::lock_guard<stdx::mutex> lock(_oplogPinnedByBackupMutex);
        if (!storageGlobalParams.allowOplogTruncation) {
            // If oplog truncation is not allowed, then return the min timestamp so that no
            // history is ever allowed to be deleted.
            return Timestamp::min();
        }
        if (_oplogPinnedByBackup) {
            // All the oplog since `_oplogPinnedByBackup` should remain intact during the
            // backup.
            return std::min(_oplogPinnedByBackup.value(), pinned);
        }
    }

    auto oplogNeededForCrashRecovery = getOplogNeededForCrashRecovery();

    if (oplogNeededForCrashRecovery) {
        return std::min(oplogNeededForCrashRecovery.value(), pinned);
    }

    auto status = getOplogNeededForRollback();
    if (status.isOK()) {
        return std::min(status.getValue(), pinned);
    }

    // If getOplogNeededForRollback fails, don't truncate any oplog right now.
    return Timestamp::min();
}

StatusWith<Timestamp> WiredTigerKVEngine::pinOldestTimestamp(
    RecoveryUnit& ru,
    const std::string& requestingServiceName,
    Timestamp requestedTimestamp,
    bool roundUpIfTooOld) {
    stdx::lock_guard<stdx::mutex> lock(_oldestTimestampPinRequestsMutex);
    Timestamp oldest = getOldestTimestamp();
    LOGV2(5380104,
          "Pin oldest timestamp request",
          "service"_attr = requestingServiceName,
          "requestedTs"_attr = requestedTimestamp,
          "roundUpIfTooOld"_attr = roundUpIfTooOld,
          "currOldestTs"_attr = oldest);

    const Timestamp previousTimestamp = [&]() -> Timestamp {
        auto tsIt = _oldestTimestampPinRequests.find(requestingServiceName);
        return tsIt != _oldestTimestampPinRequests.end() ? tsIt->second : Timestamp::min();
    }();

    auto swPinnedTimestamp =
        _pinOldestTimestamp(lock, requestingServiceName, requestedTimestamp, roundUpIfTooOld);
    if (!swPinnedTimestamp.isOK()) {
        return swPinnedTimestamp;
    }

    if (ru.inUnitOfWork()) {
        // If we've moved the pin and are in a `WriteUnitOfWork`, assume the caller has a write
        // that should be atomic with this pin request. If the `WriteUnitOfWork` is rolled back,
        // either unpin the oldest timestamp or repin the previous value.
        ru.onRollback([this, svcName = requestingServiceName, previousTimestamp](
                          OperationContext*) {
            if (previousTimestamp.isNull()) {
                unpinOldestTimestamp(svcName);
            } else {
                stdx::lock_guard<stdx::mutex> lock(_oldestTimestampPinRequestsMutex);
                // When a write is updating the value from an earlier pin to a later one, use
                // rounding to make a best effort to repin the earlier value.
                invariant(_pinOldestTimestamp(lock, svcName, previousTimestamp, true).getStatus());
            }
        });
    }

    return swPinnedTimestamp;
}

StatusWith<Timestamp> WiredTigerKVEngine::_pinOldestTimestamp(
    WithLock,
    const std::string& requestingServiceName,
    Timestamp requestedTimestamp,
    bool roundUpIfTooOld) {

    Timestamp oldest = getOldestTimestamp();
    if (requestedTimestamp < oldest) {
        if (roundUpIfTooOld) {
            requestedTimestamp = oldest;
        } else {
            return {ErrorCodes::SnapshotTooOld,
                    fmt::format("Requested timestamp: {} Current oldest timestamp: {}",
                                requestedTimestamp.toString(),
                                oldest.toString())};
        }
    }

    _oldestTimestampPinRequests[requestingServiceName] = requestedTimestamp;
    return {requestedTimestamp};
}

void WiredTigerKVEngine::unpinOldestTimestamp(const std::string& requestingServiceName) {
    stdx::lock_guard<stdx::mutex> lock(_oldestTimestampPinRequestsMutex);
    auto it = _oldestTimestampPinRequests.find(requestingServiceName);
    if (it == _oldestTimestampPinRequests.end()) {
        LOGV2_DEBUG(5380105,
                    2,
                    "The requested service had nothing to unpin",
                    "service"_attr = requestingServiceName);
        return;
    }
    LOGV2(5380103,
          "Unpin oldest timestamp request",
          "service"_attr = requestingServiceName,
          "requestedTs"_attr = it->second);
    _oldestTimestampPinRequests.erase(it);
}

std::map<std::string, Timestamp> WiredTigerKVEngine::getPinnedTimestampRequests() {
    stdx::lock_guard<stdx::mutex> lock(_oldestTimestampPinRequestsMutex);
    return _oldestTimestampPinRequests;
}

void WiredTigerKVEngine::setPinnedOplogTimestamp(const Timestamp& pinnedTimestamp) {
    _pinnedOplogTimestamp.store(pinnedTimestamp.asULL());
}

bool WiredTigerKVEngine::supportsReadConcernSnapshot() const {
    return true;
}

Status WiredTigerKVEngine::oplogDiskLocRegister(RecoveryUnit& ru,
                                                RecordStore* oplogRecordStore,
                                                const Timestamp& opTime,
                                                bool orderedCommit) {
    ru.setOrderedCommit(orderedCommit);

    if (!orderedCommit) {
        // This labels the current transaction with a timestamp.
        // This is required for oplog visibility to work correctly, as WiredTiger uses the
        // transaction list to determine where there are holes in the oplog.
        return ru.setTimestamp(opTime);
    }

    // This handles non-primary (secondary) state behavior; we simply set the oplog visiblity
    // read timestamp here, as there cannot be visible holes prior to the opTime passed in.
    getOplogManager()->setOplogReadTimestamp(opTime);

    // Inserts and updates usually notify waiters on commit, but the oplog collection has
    // special visibility rules and waiters must be notified whenever the oplog read timestamp
    // is forwarded.
    oplogRecordStore->capped()->notifyWaitersIfNeeded();
    return Status::OK();
}

void WiredTigerKVEngine::waitForAllEarlierOplogWritesToBeVisible(
    OperationContext* opCtx, RecordStore* oplogRecordStore) const {
    auto oplogManager = getOplogManager();
    oplogManager->waitForAllEarlierOplogWritesToBeVisible(oplogRecordStore, opCtx);
}

bool WiredTigerKVEngine::waitUntilDurable(OperationContext* opCtx) {
    invariant(!shard_role_details::getRecoveryUnit(opCtx)->isActive(),
              str::stream() << "Unexpected open storage txn. RecoveryUnit state: "
                            << RecoveryUnit::toString(
                                   shard_role_details::getRecoveryUnit(opCtx)->getState())
                            << ", inMultiDocumentTransaction:"
                            << (opCtx->inMultiDocumentTransaction() ? "true" : "false"));

    // Flushes the journal log to disk. Checkpoints all data if journaling is disabled.
    waitUntilDurable(opCtx, Fsync::kJournal, UseJournalListener::kUpdate);
    return true;
}

bool WiredTigerKVEngine::waitUntilUnjournaledWritesDurable(OperationContext* opCtx,
                                                           bool stableCheckpoint) {
    invariant(!shard_role_details::getRecoveryUnit(opCtx)->inUnitOfWork(),
              str::stream() << "Unexpected open storage txn. RecoveryUnit state: "
                            << RecoveryUnit::toString(
                                   shard_role_details::getRecoveryUnit(opCtx)->getState())
                            << ", inMultiDocumentTransaction:"
                            << (opCtx->inMultiDocumentTransaction() ? "true" : "false"));

    // Take a checkpoint, rather than only flush the (oplog) journal, in order to lock in stable
    // writes to unjournaled tables.
    //
    // If 'stableCheckpoint' is set, then we will only checkpoint data up to and including the
    // stable_timestamp set on WT at the time of the checkpoint. Otherwise, we will checkpoint
    // all of the data.
    Fsync fsyncType = stableCheckpoint ? Fsync::kCheckpointStableTimestamp : Fsync::kCheckpointAll;
    waitUntilDurable(opCtx, fsyncType, UseJournalListener::kUpdate);

    return true;
}

void WiredTigerKVEngine::waitUntilDurable(OperationContext* opCtx,
                                          Fsync syncType,
                                          UseJournalListener useListener) {
    // For ephemeral storage engines, the data is "as durable as it's going to get".
    // That is, a restart is equivalent to a complete node failure.
    if (isEphemeral()) {
        auto [journalListener, token] = _getJournalListenerWithToken(opCtx, useListener);
        if (token) {
            journalListener->onDurable(*token);
        }
        return;
    }

    // Storage engine does not support WiredTiger logging, we skip flushing the journal and only
    // perform checkpoints.
    if (!_supportsTableLogging && syncType == Fsync::kJournal) {
        auto [journalListener, token] = _getJournalListenerWithToken(opCtx, useListener);
        if (token) {
            journalListener->onDurable(*token);
        }
        return;
    }

    WiredTigerConnection::BlockShutdown blockShutdown(_connection.get());

    uassert(ErrorCodes::ShutdownInProgress,
            "Cannot wait for durability because a shutdown is in progress",
            !_connection->isShuttingDown());

    // Stable checkpoints are only meaningful in a replica set. Replication sets the "stable
    // timestamp". If the stable timestamp is unset, WiredTiger takes a full checkpoint, which
    // is incidentally what we want. A "true" stable checkpoint (a stable timestamp was set on
    // the WT_CONNECTION, i.e: replication is on) requires `forceCheckpoint` to be true and
    // journaling to be enabled.
    if (syncType == Fsync::kCheckpointStableTimestamp && _isReplSet) {
        invariant(!isEphemeral());
    }

    // When forcing a checkpoint with journaling enabled, don't synchronize with other
    // waiters, as a log flush is much cheaper than a full checkpoint.
    if ((syncType == Fsync::kCheckpointStableTimestamp || syncType == Fsync::kCheckpointAll) &&
        !isEphemeral()) {
        auto [journalListener, token] = _getJournalListenerWithToken(opCtx, useListener);

        forceCheckpoint(syncType == Fsync::kCheckpointStableTimestamp);

        if (token) {
            journalListener->onDurable(*token);
        }

        LOGV2_DEBUG(22418, 4, "created checkpoint (forced)");
        return;
    }

    auto [journalListener, token] = _getJournalListenerWithToken(opCtx, useListener);

    uint32_t start = _lastSyncTime.load();
    // Do the remainder in a critical section that ensures only a single thread at a time
    // will attempt to synchronize.
    stdx::unique_lock<stdx::mutex> lk(_lastSyncMutex);
    uint32_t current = _lastSyncTime.loadRelaxed();  // synchronized with writes through mutex
    if (current != start) {
        // Someone else synced already since we read lastSyncTime, so we're done!

        // Unconditionally unlock mutex here to run operations that do not require
        // synchronization. The JournalListener is the only operation that meets this criteria
        // currently.
        lk.unlock();
        if (token) {
            journalListener->onDurable(*token);
        }

        return;
    }
    _lastSyncTime.store(current + 1);

    // Nobody has synched yet, so we have to sync ourselves.

    // Initialize on first use.
    if (!_waitUntilDurableSession) {
        _waitUntilDurableSession = std::make_unique<WiredTigerSession>(_connection.get());
    }

    // Flush the journal.
    invariantWTOK(_waitUntilDurableSession->log_flush("sync=on"), *_waitUntilDurableSession);
    LOGV2_DEBUG(22419, 4, "flushed journal");

    // The session is reset periodically so that WT doesn't consider it a rogue session and log
    // about it. The session doesn't actually pin any resources that need to be released.
    if (_timeSinceLastDurabilitySessionReset.millis() > (5 * 60 * 1000 /* 5 minutes */)) {
        _waitUntilDurableSession->reset();
        _timeSinceLastDurabilitySessionReset.reset();
    }

    // Unconditionally unlock mutex here to run operations that do not require synchronization.
    // The JournalListener is the only operation that meets this criteria currently.
    lk.unlock();
    if (token) {
        journalListener->onDurable(*token);
    }
}

std::pair<JournalListener*, std::unique_ptr<JournalListener::Token>>
WiredTigerKVEngine::_getJournalListenerWithToken(OperationContext* opCtx,
                                                 UseJournalListener useListener) {
    auto journalListener = [&]() -> JournalListener* {
        // The JournalListener may not be set immediately, so we must check under a mutex so
        // as not to access the variable while setting a JournalListener. A JournalListener
        // is only allowed to be set once, so using the pointer outside of a mutex is safe.
        stdx::unique_lock<stdx::mutex> lk(_journalListenerMutex);
        return _journalListener;
    }();
    std::unique_ptr<JournalListener::Token> token;
    if (journalListener && useListener == UseJournalListener::kUpdate) {
        // Update a persisted value with the latest write timestamp that is safe across
        // startup recovery in the repl layer. Then report that timestamp as durable to the
        // repl layer below after we have flushed in-memory data to disk.
        // Note: only does a write if primary, otherwise just fetches the timestamp.
        token = journalListener->getToken(opCtx);
    }
    return std::make_pair(journalListener, std::move(token));
}

Timestamp WiredTigerKVEngine::getStableTimestamp() const {
    return Timestamp(_stableTimestamp.load());
}

Timestamp WiredTigerKVEngine::getOldestTimestamp() const {
    return Timestamp(_oldestTimestamp.load());
}

Timestamp WiredTigerKVEngine::getCheckpointTimestamp() const {
    return Timestamp(_getCheckpointTimestamp());
}

std::uint64_t WiredTigerKVEngine::_getCheckpointTimestamp() const {
    char buf[(2 * 8 /*bytes in hex*/) + 1 /*nul terminator*/];
    invariantWTOK(_conn->query_timestamp(_conn, buf, "get=last_checkpoint"), nullptr);

    std::uint64_t tmp;
    fassert(50963, NumberParser().base(16)(buf, &tmp));
    return tmp;
}

Timestamp WiredTigerKVEngine::getBackupCheckpointTimestamp() {
    // Buffer must be large enough to hold a NUL terminated, hex-encoded 8 byte timestamp.
    char buf[(2 * 8 /*bytes in hex*/) + 1 /*nul terminator*/];

    invariantWTOK(_conn->query_timestamp(_conn, buf, "get=backup_checkpoint"), nullptr);
    std::uint64_t backup_checkpoint_timestamp;
    fassert(8120800, NumberParser().base(16)(buf, &backup_checkpoint_timestamp));

    return Timestamp(backup_checkpoint_timestamp);
}

void WiredTigerKVEngine::dump() const {
    int ret = _conn->debug_info(
        _conn, "cache=true,cursors=true,handles=true,log=true,sessions=true,txn=true");
    auto status = wtRCToStatus(ret, nullptr, "WiredTigerKVEngine::dump()");
    if (status.isOK()) {
        LOGV2(6117700, "WiredTigerKVEngine::dump() completed successfully");
    } else {
        LOGV2(6117701, "WiredTigerKVEngine::dump() failed", "error"_attr = status);
    }
}

StatusWith<BSONObj> WiredTigerKVEngine::getStorageMetadata(StringData ident) const {
    auto session = _connection->getUninterruptibleSession();

    auto tableMetadata = WiredTigerUtil::getMetadata(*session, fmt::format("table:{}", ident));
    if (!tableMetadata.isOK()) {
        return tableMetadata.getStatus();
    }

    auto fileMetadata = WiredTigerUtil::getMetadata(*session, fmt::format("file:{}.wt", ident));
    if (!fileMetadata.isOK()) {
        return fileMetadata.getStatus();
    }

    return BSON("tableMetadata" << tableMetadata.getValue() << "fileMetadata"
                                << fileMetadata.getValue());
}

KeyFormat WiredTigerKVEngine::getKeyFormat(RecoveryUnit& ru, StringData ident) const {

    const std::string wtTableConfig = uassertStatusOK(WiredTigerUtil::getMetadataCreate(
        *WiredTigerRecoveryUnit::get(ru).getSessionNoTxn(), fmt::format("table:{}", ident)));
    return wtTableConfig.find("key_format=u") != string::npos ? KeyFormat::String : KeyFormat::Long;
}

bool WiredTigerKVEngine::underCachePressure(int concurrentWriteOuts, int concurrentReadOuts) {
    auto permit = tryGetStatsCollectionPermit();
    if (!permit) {
        return false;  // Skip cache pressure evaluation if the permit isn't available.
    }
    return _cachePressureMonitor->isUnderCachePressure(
        *permit, concurrentWriteOuts, concurrentReadOuts);
}

BSONObj WiredTigerKVEngine::setFlagToStorageOptions(const BSONObj& storageEngineOptions,
                                                    StringData flagName,
                                                    boost::optional<bool> flagValue) const {
    return setFlagToWiredTigerStorageOptions(storageEngineOptions, flagName, flagValue);
}

boost::optional<bool> WiredTigerKVEngine::getFlagFromStorageOptions(
    const BSONObj& storageEngineOptions, StringData flagName) const {
    return getFlagFromWiredTigerStorageOptions(storageEngineOptions, flagName);
}

BSONObj WiredTigerKVEngine::getSanitizedStorageOptionsForSecondaryReplication(
    const BSONObj& options) const {

    // Skip ephemeral storage engines, encryption at rest only applies to storage backed engine.
    if (isEphemeral()) {
        return options;
    }

    return WiredTigerUtil::getSanitizedStorageOptionsForSecondaryReplication(options);
}

void WiredTigerKVEngine::sizeStorerPeriodicFlush() {
    bool needSyncSizeInfo = false;
    {
        stdx::lock_guard<stdx::mutex> lock(_sizeStorerSyncTrackerMutex);
        needSyncSizeInfo = _sizeStorerSyncTracker.intervalHasElapsed();
    }

    if (needSyncSizeInfo) {
        syncSizeInfo(false);
    }
}

Status WiredTigerKVEngine::autoCompact(RecoveryUnit& ru, const AutoCompactOptions& options) {
    auto status = WiredTigerUtil::canRunAutoCompact(isEphemeral());
    if (!status.isOK())
        return status;

    StringBuilder config;
    if (options.enable) {
        config << "background=true,timeout=0";
        if (options.freeSpaceTargetMB) {
            config << ",free_space_target=" << std::to_string(*options.freeSpaceTargetMB) << "MB";
        }
        if (!options.excludedIdents.empty()) {
            // Create WiredTiger URIs from the idents.
            config << ",exclude=[";
            for (const auto& ident : options.excludedIdents) {
                config << "\"" << WiredTigerUtil::buildTableUri(ident) << ".wt\",";
            }
            config << "]";
        }
        if (options.runOnce) {
            config << ",run_once=true";
        }
    } else {
        config << "background=false";
    }

    WiredTigerSession* s = WiredTigerRecoveryUnit::get(&ru)->getSessionNoTxn();
    int ret = s->compact(nullptr, config.str().c_str());

    if (ret == 0) {
        return Status::OK();
    }

    WiredTigerSession::GetLastError err = s->getLastError();

    // We may get WT_BACKGROUND_COMPACT_ALREADY_RUNNING when we try to reconfigure background
    // compaction while it is already running.
    uassert(ErrorCodes::AlreadyInitialized,
            err.err_msg,
            err.sub_level_err != WT_BACKGROUND_COMPACT_ALREADY_RUNNING);

    status = wtRCToStatus(ret, *s, "Failed to configure auto compact");

    LOGV2_ERROR(8704101,
                "WiredTigerKVEngine::autoCompact() failed",
                "config"_attr = config.str(),
                "error"_attr = status);
    return status;
}

bool WiredTigerKVEngine::hasOngoingLiveRestore() {
    auto session = getConnection().getUninterruptibleSession();
    auto result = WiredTigerUtil::getStatisticsValue(
        *session, "statistics:", "statistics=(fast)", WT_STAT_CONN_LIVE_RESTORE_STATE);
    return uassertStatusOK(result) == WT_LIVE_RESTORE_IN_PROGRESS;
}

Status WiredTigerKVEngine::_drop(WiredTigerSession& session, const char* uri, const char* config) {
    int ret = session.drop(uri, config);

    // If ident doesn't exist, it is effectively dropped.
    if (ret == 0 || ret == ENOENT) {
        return Status::OK();
    }

    int err = 0;
    int sub_level_err = WT_NONE;
    const char* err_msg = "";

    session.get_last_error(&err, &sub_level_err, &err_msg);

    // We should never run into these situations when we are already in the process of dropping
    // the table.
    // TODO: SERVER-100890 Re-enable this invariant once we have fixed the bug that causes this
    // to fail. invariant(sub_level_err != WT_UNCOMMITTED_DATA);
    invariant(sub_level_err != WT_CONFLICT_TABLE_LOCK);
    invariant(sub_level_err != WT_CONFLICT_SCHEMA_LOCK);

    // If we failed due to uncheckpointed data, checkpoint and retry the operation so that
    // it will attempt to clean up the dirty elements during checkpointing, thus allowing
    // the operation to succeed if it was the only reason to fail.
    if (sub_level_err == WT_DIRTY_DATA) {
        // Checkpoint and retry.
        _checkpoint(session);
        ret = session.drop(uri, config);
    }
    // TODO: SERVER-100390 add dump and debug info if the drop failed here.
    return wtRCToStatus(ret, session);
}

WiredTigerKVEngineBase::WiredTigerConfig getWiredTigerConfigFromStartupOptions(
    const rss::PersistenceProvider& provider) {
    WiredTigerKVEngineBase::WiredTigerConfig wtConfig;

    wtConfig.sessionMax = wiredTigerGlobalOptions.sessionMax;
    wtConfig.evictionDirtyTargetMB = wiredTigerGlobalOptions.evictionDirtyTargetGB * 1024;
    wtConfig.evictionDirtyTriggerMB = wiredTigerGlobalOptions.evictionDirtyTriggerGB * 1024;
    wtConfig.evictionUpdatesTriggerMB = wiredTigerGlobalOptions.evictionUpdatesTriggerGB * 1024;
    wtConfig.logCompressor = wiredTigerGlobalOptions.journalCompressor;
    wtConfig.liveRestorePath = wiredTigerGlobalOptions.liveRestoreSource;
    wtConfig.liveRestoreThreadsMax = wiredTigerGlobalOptions.liveRestoreThreads;
    wtConfig.liveRestoreReadSizeMB = wiredTigerGlobalOptions.liveRestoreReadSizeMB;
    wtConfig.statisticsLogWaitSecs = wiredTigerGlobalOptions.statisticsLogDelaySecs;
    wtConfig.evictionThreadsMax = gWiredTigerEvictionThreadsMax.load();
    wtConfig.evictionThreadsMin = gWiredTigerEvictionThreadsMin.load();
    wtConfig.providerSupportsUnstableCheckpoints = provider.supportsUnstableCheckpoints();
    wtConfig.safeToTakeDuplicateCheckpoints = !provider.shouldAvoidDuplicateCheckpoints();
    wtConfig.flattenLeafPageDelta = wiredTigerGlobalOptions.flattenLeafPageDelta;

    wtConfig.extraOpenOptions = wiredTigerGlobalOptions.engineConfig;
    if (wtConfig.extraOpenOptions.find("session_max=") != std::string::npos) {
        LOGV2_WARNING(9086701,
                      "The session cache max is derived from the session_max value "
                      "provided as a server parameter. Please use the wiredTigerSessionMax server "
                      "parameter to set this value.");
    }

    return wtConfig;
}

Status WiredTigerKVEngine::updateEvictionThreadsMax(const int32_t& threadsMax) {
    if (hasGlobalServiceContext()) {
        ServiceContext* serviceContext = getGlobalServiceContext();

        WiredTigerKVEngine* kvEngine =
            static_cast<WiredTigerKVEngine*>(serviceContext->getStorageEngine()->getEngine());
        kvEngine->_wtConfig.evictionThreadsMax = threadsMax;

        std::stringstream ss;
        ss << "eviction=(threads_max=" << threadsMax << ")";
        uassertStatusOK(wtRCToStatus(kvEngine->reconfigure(ss.str().c_str()), nullptr));
    }

    return Status::OK();
}

Status WiredTigerKVEngine::updateEvictionThreadsMin(const int32_t& threadsMin) {
    if (hasGlobalServiceContext()) {
        ServiceContext* serviceContext = getGlobalServiceContext();

        WiredTigerKVEngine* kvEngine =
            static_cast<WiredTigerKVEngine*>(serviceContext->getStorageEngine()->getEngine());
        kvEngine->_wtConfig.evictionThreadsMin = threadsMin;

        std::stringstream ss;
        ss << "eviction=(threads_min=" << threadsMin << ")";
        uassertStatusOK(wtRCToStatus(kvEngine->reconfigure(ss.str().c_str()), nullptr));
    }

    return Status::OK();
}

}  // namespace mongo
