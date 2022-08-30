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


#define LOGV2_FOR_RECOVERY(ID, DLEVEL, MESSAGE, ...) \
    LOGV2_DEBUG_OPTIONS(ID, DLEVEL, {logv2::LogComponent::kStorageRecovery}, MESSAGE, ##__VA_ARGS__)
#define LOGV2_FOR_ROLLBACK(ID, DLEVEL, MESSAGE, ...) \
    LOGV2_DEBUG_OPTIONS(                             \
        ID, DLEVEL, {logv2::LogComponent::kReplicationRollback}, MESSAGE, ##__VA_ARGS__)

#include "mongo/platform/basic.h"
#include "mongo/util/exit_code.h"

#ifdef _WIN32
#define NVALGRIND
#endif

#include <fmt/format.h>
#include <iomanip>
#include <memory>

#include "mongo/db/storage/wiredtiger/wiredtiger_kv_engine.h"

#include <boost/filesystem.hpp>
#include <boost/filesystem/fstream.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>
#include <boost/system/error_code.hpp>
#include <fmt/format.h>
#include <valgrind/valgrind.h>

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/bson/dotted_path_support.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/collection_catalog.h"
#include "mongo/db/client.h"
#include "mongo/db/commands/server_status_metric.h"
#include "mongo/db/concurrency/exception_util.h"
#include "mongo/db/concurrency/locker.h"
#include "mongo/db/global_settings.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/mongod_options_storage_gen.h"
#include "mongo/db/repl/repl_settings.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/server_options.h"
#include "mongo/db/server_recovery.h"
#include "mongo/db/service_context.h"
#include "mongo/db/snapshot_window_options_gen.h"
#include "mongo/db/storage/journal_listener.h"
#include "mongo/db/storage/key_format.h"
#include "mongo/db/storage/storage_file_util.h"
#include "mongo/db/storage/storage_options.h"
#include "mongo/db/storage/storage_parameters_gen.h"
#include "mongo/db/storage/storage_repair_observer.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_column_store.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_cursor.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_customization_hooks.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_extensions.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_global_options.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_index.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_parameters_gen.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_record_store.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_recovery_unit.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_session_cache.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_size_storer.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_util.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/util/background.h"
#include "mongo/util/concurrency/idle_thread_block.h"
#include "mongo/util/concurrency/ticketholder.h"
#include "mongo/util/debug_util.h"
#include "mongo/util/exit.h"
#include "mongo/util/log_and_backoff.h"
#include "mongo/util/processinfo.h"
#include "mongo/util/quick_exit.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/stacktrace.h"
#include "mongo/util/testing_proctor.h"
#include "mongo/util/time_support.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage


using namespace fmt::literals;

namespace mongo {

namespace {

MONGO_FAIL_POINT_DEFINE(WTPauseStableTimestamp);
MONGO_FAIL_POINT_DEFINE(WTPreserveSnapshotHistoryIndefinitely);
MONGO_FAIL_POINT_DEFINE(WTSetOldestTSToStableTS);
MONGO_FAIL_POINT_DEFINE(WTWriteConflictExceptionForImportCollection);
MONGO_FAIL_POINT_DEFINE(WTWriteConflictExceptionForImportIndex);
MONGO_FAIL_POINT_DEFINE(WTRollbackToStableReturnOnEBUSY);

const std::string kPinOldestTimestampAtStartupName = "_wt_startup";

#if !defined(__has_feature)
#define __has_feature(x) 0
#endif

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

}  // namespace

bool WiredTigerFileVersion::shouldDowngrade(bool hasRecoveryTimestamp) {
    const auto replCoord = repl::ReplicationCoordinator::get(getGlobalServiceContext());
    if (replCoord && replCoord->getMemberState().arbiter()) {
        // SERVER-35361: Arbiters will no longer downgrade their data files. To downgrade
        // binaries, the user must delete the dbpath. It's not particularly expensive for a
        // replica set to re-initialize an arbiter that comes online.
        return false;
    }

    if (!serverGlobalParams.featureCompatibility.isVersionInitialized()) {
        // If the FCV document hasn't been read, trust the WT compatibility. MongoD will
        // downgrade to the same compatibility it discovered on startup.
        return _startupVersion == StartupVersion::IS_44_FCV_42 ||
            _startupVersion == StartupVersion::IS_42;
    }

    // (Generic FCV reference): Only consider downgrading when FCV has been fully downgraded to last
    // continuous or last LTS. It's possible for WiredTiger to introduce a data format change in a
    // continuous release. This FCV gate must remain across binary version releases.
    const auto currentVersion = serverGlobalParams.featureCompatibility.getVersion();
    if (currentVersion != multiversion::GenericFCV::kLastContinuous &&
        currentVersion != multiversion::GenericFCV::kLastLTS) {
        return false;
    }

    if (getGlobalReplSettings().usingReplSets()) {
        // If this process is run with `--replSet`, it must have run any startup replication
        // recovery and downgrading at this point is safe.
        return true;
    }

    if (hasRecoveryTimestamp) {
        // If we're not running with `--replSet`, don't allow downgrades if the node needed to run
        // replication recovery. Having a recovery timestamp implies recovery must be run, but it
        // was not.
        return false;
    }

    // If there is no `recoveryTimestamp`, then the data should be consistent with the top of
    // oplog and downgrading can proceed. This is expected for standalone datasets that use FCV.
    return true;
}

std::string WiredTigerFileVersion::getDowngradeString() {
    if (!serverGlobalParams.featureCompatibility.isVersionInitialized()) {
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
    // Either to kLastContinuous or kLastLTS. It's possible for the data format to differ between
    // kLastContinuous and kLastLTS and we'll need to handle that appropriately here. We only
    // consider downgrading when FCV has been fully downgraded.
    const auto currentVersion = serverGlobalParams.featureCompatibility.getVersion();
    // (Generic FCV reference): This FCV check should exist across LTS binary versions because the
    // logic for keeping the WiredTiger release version compatible with the server FCV version will
    // be the same across different LTS binary versions.
    if (currentVersion == multiversion::GenericFCV::kLastContinuous) {
        // If the data format between kLatest and kLastContinuous differs, change the
        // 'kLastContinuousWTRelease' version.
        return kLastContinuousWTRelease;
        // (Generic FCV reference): This FCV check should exist across LTS binary versions because
        // the logic for keeping the WiredTiger release version compatible with the server FCV
        // version will be the same across different LTS binary versions.
    } else if (currentVersion == multiversion::GenericFCV::kLastLTS) {
        // If the data format between kLatest and kLastLTS differs, change the
        // 'kLastLTSWTRelease' version.
        return kLastLTSWTRelease;
    }

    // We're in a state that's not ready to downgrade. Use the latest WiredTiger version for this
    // binary.
    return kLatestWTRelease;
}

using std::set;
using std::string;

namespace dps = ::mongo::dotted_path_support;

class WiredTigerKVEngine::WiredTigerSessionSweeper : public BackgroundJob {
public:
    explicit WiredTigerSessionSweeper(WiredTigerSessionCache* sessionCache)
        : BackgroundJob(false /* deleteSelf */), _sessionCache(sessionCache) {}

    virtual string name() const {
        return "WTIdleSessionSweeper";
    }

    virtual void run() {
        ThreadClient tc(name(), getGlobalServiceContext());
        LOGV2_DEBUG(22303, 1, "starting {name} thread", "name"_attr = name());

        while (!_shuttingDown.load()) {
            {
                stdx::unique_lock<Latch> lock(_mutex);
                MONGO_IDLE_THREAD_BLOCK;
                // Check every 10 seconds or sooner in the debug builds
                _condvar.wait_for(lock, stdx::chrono::seconds(kDebugBuild ? 1 : 10));
            }

            _sessionCache->closeExpiredIdleSessions(gWiredTigerSessionCloseIdleTimeSecs.load() *
                                                    1000);
        }
        LOGV2_DEBUG(22304, 1, "stopping {name} thread", "name"_attr = name());
    }

    void shutdown() {
        _shuttingDown.store(true);
        {
            stdx::unique_lock<Latch> lock(_mutex);
            // Wake up the session sweeper thread early, we do not want the shutdown
            // to wait for us too long.
            _condvar.notify_one();
        }
        wait();
    }

private:
    WiredTigerSessionCache* _sessionCache;
    AtomicWord<bool> _shuttingDown{false};

    Mutex _mutex = MONGO_MAKE_LATCH("WiredTigerSessionSweeper::_mutex");  // protects _condvar
    // The session sweeper thread idles on this condition variable for a particular time duration
    // between cleaning up expired sessions. It can be triggered early to expediate shutdown.
    stdx::condition_variable _condvar;
};

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

StringData WiredTigerKVEngine::kTableUriPrefix = "table:"_sd;

WiredTigerKVEngine::WiredTigerKVEngine(const std::string& canonicalName,
                                       const std::string& path,
                                       ClockSource* cs,
                                       const std::string& extraOpenOptions,
                                       size_t cacheSizeMB,
                                       size_t maxHistoryFileSizeMB,
                                       bool ephemeral,
                                       bool repair)
    : _clockSource(cs),
      _oplogManager(std::make_unique<WiredTigerOplogManager>()),
      _canonicalName(canonicalName),
      _path(path),
      _sizeStorerSyncTracker(cs, 100000, Seconds(60)),
      _ephemeral(ephemeral),
      _inRepairMode(repair),
      _keepDataHistory(serverGlobalParams.enableMajorityReadConcern) {
    _pinnedOplogTimestamp.store(Timestamp::max().asULL());
    boost::filesystem::path journalPath = path;
    journalPath /= "journal";
    if (!_ephemeral) {
        if (!boost::filesystem::exists(journalPath)) {
            try {
                boost::filesystem::create_directory(journalPath);
            } catch (std::exception& e) {
                LOGV2_ERROR(22312,
                            "error creating journal dir {directory} {error}",
                            "Error creating journal directory",
                            "directory"_attr = journalPath.generic_string(),
                            "error"_attr = e.what());
                throw;
            }
        }
    }

    _previousCheckedDropsQueued.store(_clockSource->now().toMillisSinceEpoch());

    std::stringstream ss;
    ss << "create,";
    ss << "cache_size=" << cacheSizeMB << "M,";
    ss << "session_max=33000,";
    ss << "eviction=(threads_min=4,threads_max=4),";

    if (gWiredTigerEvictionDirtyTargetGB)
        ss << "eviction_dirty_target="
           << static_cast<size_t>(gWiredTigerEvictionDirtyTargetGB * 1024) << "MB,";
    if (gWiredTigerEvictionDirtyMaxGB)
        ss << "eviction_dirty_trigger=" << static_cast<size_t>(gWiredTigerEvictionDirtyMaxGB * 1024)
           << "MB,";

    ss << "config_base=false,";
    ss << "statistics=(fast),";

    if (!WiredTigerSessionCache::isEngineCachingCursors()) {
        ss << "cache_cursors=false,";
    }

    if (_ephemeral) {
        // If we've requested an ephemeral instance we store everything into memory instead of
        // backing it onto disk. Logging is not supported in this instance, thus we also have to
        // disable it.
        ss << ",in_memory=true,log=(enabled=false),";
    } else {
        // In persistent mode we enable the journal and set the compression settings.
        ss << "log=(enabled=true,remove=true,path=journal,compressor=";
        ss << wiredTigerGlobalOptions.journalCompressor << "),";
        ss << "builtin_extension_config=(zstd=(compression_level="
           << wiredTigerGlobalOptions.zstdCompressorLevel << ")),";
    }

    ss << "file_manager=(close_idle_time=" << gWiredTigerFileHandleCloseIdleTime
       << ",close_scan_interval=" << gWiredTigerFileHandleCloseScanInterval
       << ",close_handle_minimum=" << gWiredTigerFileHandleCloseMinimum << "),";
    ss << "statistics_log=(wait=" << wiredTigerGlobalOptions.statisticsLogDelaySecs << "),";

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
    if (kAddressSanitizerEnabled || kThreadSanitizerEnabled) {
        // For applications using WT, advancing a cursor invalidates the data/memory that cursor was
        // pointing to. WT performs the optimization of managing its own memory. The unit of memory
        // allocation is a page. Walking a cursor from one key/value to the next often lands on the
        // same page, which has the effect of keeping the address of the prior key/value valid. For
        // a bug to occur, the cursor must move across pages, and the prior page must be
        // evicted. While rare, this can happen, resulting in reading random memory.
        //
        // The cursor copy debug mode will instead cause WT to malloc/free memory for each key/value
        // a cursor is positioned on. Thus, enabling when using with address sanitizer will catch
        // many cases of dereferencing invalid cursor positions. Note, there is a known caveat: a
        // free/malloc for roughly the same allocation size can often return the same memory
        // address. This is a scenario where the address sanitizer is not able to detect a
        // use-after-free error.
        //
        // Additionally, WT does not use the standard C thread model and thus TSAN can report false
        // data races when touching memory that was allocated within WT. The cursor_copy mode
        // alleviates this by copying all returned data to its own buffer before leaving the storage
        // engine.
        ss << "debug_mode=(cursor_copy=true),";
    }
    if (TestingProctor::instance().isEnabled()) {
        // Enable debug write-ahead logging for all tables when testing is enabled.
        //
        // If MongoDB startup fails, there may be clues from the previous run still left in the WT
        // log files that can provide some insight into how the system got into a bad state. When
        // testing is enabled, keep around some of these files for investigative purposes.
        ss << "debug_mode=(table_logging=true,checkpoint_retention=4),";
    }
    if (gWiredTigerStressConfig) {
        ss << "timing_stress_for_test=[history_store_checkpoint_delay,checkpoint_slow],";
    }

    ss << WiredTigerCustomizationHooks::get(getGlobalServiceContext())
              ->getTableCreateConfig("system");
    ss << WiredTigerExtensions::get(getGlobalServiceContext())->getOpenExtensionsConfig();
    ss << extraOpenOptions;

    if (WiredTigerUtil::willRestoreFromBackup()) {
        ss << WiredTigerUtil::generateRestoreConfig() << ",";
    }

    string config = ss.str();
    LOGV2(22315, "Opening WiredTiger", "config"_attr = config);
    auto startTime = Date_t::now();
    _openWiredTiger(path, config);
    LOGV2(4795906, "WiredTiger opened", "duration"_attr = Date_t::now() - startTime);
    _eventHandler.setStartupSuccessful();
    _wtOpenConfig = config;

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
        invariantWTOK(_conn->query_timestamp(_conn, buf, "get=oldest"), nullptr);
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

    // If there's no recovery timestamp, MDB has not produced a consistent snapshot of
    // data. `_oldestTimestamp` and `_initialDataTimestamp` are only meaningful when there's a
    // consistent snapshot of data.
    //
    // Note, this code is defensive (i.e: protects against a theorized, unobserved case) and is
    // primarily concerned with restarts of a process that was performing an eMRC=off rollback via
    // refetch.
    if (_recoveryTimestamp.isNull() && _oldestTimestamp.load() > 0) {
        LOGV2_FOR_RECOVERY(5380108, 0, "There is an oldestTimestamp without a recoveryTimestamp");
        _oldestTimestamp.store(0);
        _initialDataTimestamp.store(0);
    }

    _sessionCache.reset(new WiredTigerSessionCache(this));

    _sessionSweeper = std::make_unique<WiredTigerSessionSweeper>(_sessionCache.get());
    _sessionSweeper->go();

    // Until the Replication layer installs a real callback, prevent truncating the oplog.
    setOldestActiveTransactionTimestampCallback(
        [](Timestamp) { return StatusWith(boost::make_optional(Timestamp::min())); });

    if (!_ephemeral) {
        if (!_recoveryTimestamp.isNull()) {
            // If the oldest/initial data timestamps were unset (there was no persisted durable
            // history), initialize them to the recovery timestamp.
            if (_oldestTimestamp.load() == 0) {
                setInitialDataTimestamp(_recoveryTimestamp);
                // Communicate the oldest timestamp to WT.
                setOldestTimestamp(_recoveryTimestamp, false);
            }

            // Pin the oldest timestamp prior to calling `setStableTimestamp` as that attempts to
            // advance the oldest timestamp. We do this pinning to give features such as resharding
            // an opportunity to re-pin the oldest timestamp after a restart. The assumptions this
            // relies on are that:
            //
            // 1) The feature stores the desired pin timestamp in some local collection.
            // 2) This temporary pinning lasts long enough for the catalog to be loaded and
            //    accessed.
            {
                stdx::lock_guard<Latch> lk(_oldestTimestampPinRequestsMutex);
                uassertStatusOK(_pinOldestTimestamp(lk,
                                                    kPinOldestTimestampAtStartupName,
                                                    Timestamp(_oldestTimestamp.load()),
                                                    false));
            }

            setStableTimestamp(_recoveryTimestamp, false);

            _sessionCache->snapshotManager().setLastApplied(_recoveryTimestamp);
            {
                stdx::lock_guard<Latch> lk(_highestDurableTimestampMutex);
                _highestSeenDurableTimestamp = _recoveryTimestamp.asULL();
            }
        }
    }

    if (_ephemeral && !TestingProctor::instance().isEnabled()) {
        // We do not maintain any snapshot history for the ephemeral storage engine in production
        // because replication and sharded transactions do not currently run on the inMemory engine.
        // It is live in testing, however.
        minSnapshotHistoryWindowInSeconds.store(0);
    }

    _sizeStorerUri = _uri("sizeStorer");
    WiredTigerSession session(_conn);
    if (repair && _hasUri(session.getSession(), _sizeStorerUri)) {
        LOGV2(22316, "Repairing size cache");

        auto status = _salvageIfNeeded(_sizeStorerUri.c_str());
        if (status.code() != ErrorCodes::DataModifiedByRepair)
            fassertNoTrace(28577, status);
    }

    _sizeStorer = std::make_unique<WiredTigerSizeStorer>(_conn, _sizeStorerUri);
    _runTimeConfigParam.reset(makeServerParameter<WiredTigerEngineRuntimeConfigParameter>(
        "wiredTigerEngineRuntimeConfig", ServerParameterType::kRuntimeOnly));
    _runTimeConfigParam->_data.second = this;
}

WiredTigerKVEngine::~WiredTigerKVEngine() {
    // Remove server parameters that we added in the constructor, to enable unit tests to reload the
    // storage engine again in this same process.
    ServerParameterSet::getNodeParameterSet()->remove("wiredTigerEngineRuntimeConfig");

    cleanShutdown();

    _sessionCache.reset(nullptr);
}

void WiredTigerKVEngine::notifyStartupComplete() {
    unpinOldestTimestamp(kPinOldestTimestampAtStartupName);
    WiredTigerUtil::notifyStartupComplete();
}

void WiredTigerKVEngine::appendGlobalStats(OperationContext* opCtx, BSONObjBuilder& b) {
    BSONObjBuilder bb(b.subobjStart("concurrentTransactions"));
    auto ticketHolder = TicketHolder::get(opCtx->getServiceContext());
    ticketHolder->appendStats(bb);
    bb.done();
}

/**
 * Table of MongoDB<->WiredTiger<->Log version numbers:
 *
 * |                MongoDB | WiredTiger | Log |
 * |------------------------+------------+-----|
 * |                 3.0.15 |      2.5.3 |   1 |
 * |                 3.2.20 |      2.9.2 |   1 |
 * |                 3.4.15 |      2.9.2 |   1 |
 * |                  3.6.4 |      3.0.1 |   2 |
 * |                 4.0.16 |      3.1.1 |   3 |
 * |                  4.2.1 |      3.2.2 |   3 |
 * |                  4.2.6 |      3.3.0 |   3 |
 * | 4.2.6 (blessed by 4.4) |      3.3.0 |   4 |
 * |                  4.4.0 |     10.0.0 |   5 |
 * |                  5.0.0 |     10.0.1 |   5 |
 * |                  5.1.0 |     10.0.1 |   5 |
 * |                  5.2.0 |     10.0.1 |   5 |
 */
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
        // WT 4.4+ will refuse to startup on datafiles left behind by 4.0 and earlier. This behavior
        // is enforced outside of `require_min`. This condition is detected via a specific error
        // message from WiredTiger.
        if (_inRepairMode) {
            // In case this process was started with `--repair`, remove the "repair incomplete"
            // file.
            StorageRepairObserver::get(getGlobalServiceContext())->onRepairDone(nullptr);
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

void WiredTigerKVEngine::cleanShutdown() {
    LOGV2(22317, "WiredTigerKVEngine shutting down");

    if (!_conn) {
        return;
    }

    // these must be the last things we do before _conn->close();
    haltOplogManager(/*oplogRecordStore=*/nullptr, /*shuttingDown=*/true);
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

    _sessionCache->shuttingDown();

    syncSizeInfo(/*syncToDisk=*/true);

    // The size storer has to be destructed after the session cache has shut down. This sets the
    // shutdown flag internally in the session cache. As operations get interrupted during shutdown,
    // they release their session back to the session cache. If the shutdown flag has been set,
    // released sessions will skip flushing the size storer.
    _sizeStorer.reset();

    // We want WiredTiger to leak memory for faster shutdown except when we are running tools to
    // look for memory leaks.
    bool leak_memory = !kAddressSanitizerEnabled;
    std::string closeConfig = "";

    if (RUNNING_ON_VALGRIND) {
        leak_memory = false;
    }

    if (leak_memory) {
        closeConfig = "leak_memory=true,";
    }

    const Timestamp stableTimestamp = getStableTimestamp();
    const Timestamp initialDataTimestamp = getInitialDataTimestamp();
    if (gTakeUnstableCheckpointOnShutdown) {
        closeConfig += "use_timestamp=false,";
    } else if (!serverGlobalParams.enableMajorityReadConcern &&
               stableTimestamp < initialDataTimestamp) {
        // After a rollback via refetch, WT update chains for _id index keys can be logically
        // corrupt for read timestamps earlier than the `_initialDataTimestamp`. Because the stable
        // timestamp is really a read timestamp, we must avoid taking a stable checkpoint.
        //
        // If a stable timestamp is not set, there's no risk of reading corrupt history.
        LOGV2(22326,
              "Skipping checkpoint during clean shutdown because stableTimestamp is less than the "
              "initialDataTimestamp and enableMajorityReadConcern is false",
              "stableTimestamp"_attr = stableTimestamp,
              "initialDataTimestamp"_attr = initialDataTimestamp);
        quickExit(ExitCode::clean);
    }

    if (_fileVersion.shouldDowngrade(!_recoveryTimestamp.isNull())) {
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

    auto startTime = Date_t::now();
    LOGV2(4795902, "Closing WiredTiger", "closeConfig"_attr = closeConfig);
    invariantWTOK(_conn->close(_conn, closeConfig.c_str()), nullptr);
    LOGV2(4795901, "WiredTiger closed", "duration"_attr = Date_t::now() - startTime);
    _conn = nullptr;
}

int64_t WiredTigerKVEngine::getIdentSize(OperationContext* opCtx, StringData ident) {
    WiredTigerSession* session = WiredTigerRecoveryUnit::get(opCtx)->getSession();
    return WiredTigerUtil::getIdentSize(session->getSession(), _uri(ident));
}

Status WiredTigerKVEngine::repairIdent(OperationContext* opCtx, StringData ident) {
    WiredTigerSession* session = WiredTigerRecoveryUnit::get(opCtx)->getSession();
    string uri = _uri(ident);
    session->closeAllCursors(uri);
    _sessionCache->closeAllCursors(uri);
    if (isEphemeral()) {
        return Status::OK();
    }
    _ensureIdentPath(ident);
    return _salvageIfNeeded(uri.c_str());
}

Status WiredTigerKVEngine::_salvageIfNeeded(const char* uri) {
    // Using a side session to avoid transactional issues
    WiredTigerSession sessionWrapper(_conn);
    WT_SESSION* session = sessionWrapper.getSession();

    int rc = (session->verify)(session, uri, nullptr);
    // WT may return EBUSY if the database contains dirty data. If we checkpoint and retry the
    // operation it will attempt to clean up the dirty elements during checkpointing, thus allowing
    // the operation to succeed if it was the only reason to fail.
    if (rc == EBUSY) {
        _checkpoint(session);
        rc = (session->verify)(session, uri, nullptr);
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
    rc = session->salvage(session, uri, nullptr);
    // Same reasoning for handling EBUSY errors as above.
    if (rc == EBUSY) {
        _checkpoint(session);
        rc = session->salvage(session, uri, nullptr);
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

Status WiredTigerKVEngine::_rebuildIdent(WT_SESSION* session, const char* uri) {
    invariant(_inRepairMode);

    invariant(std::string(uri).find(kTableUriPrefix.rawData()) == 0);

    const std::string identName(uri + kTableUriPrefix.size());
    auto filePath = getDataFilePathForIdent(identName);
    if (filePath) {
        const boost::filesystem::path corruptFile(filePath->string() + ".corrupt");
        LOGV2_WARNING(22352,
                      "Moving data file {file} to backup as {backup}",
                      "Moving data file to backup",
                      "file"_attr = filePath->generic_string(),
                      "backup"_attr = corruptFile.generic_string());

        auto status = fsyncRename(filePath.value(), corruptFile);
        if (!status.isOK()) {
            return status;
        }
    }

    LOGV2_WARNING(22353, "Rebuilding ident {ident}", "Rebuilding ident", "ident"_attr = identName);

    // This is safe to call after moving the file because it only reads from the metadata, and not
    // the data file itself.
    auto swMetadata = WiredTigerUtil::getMetadataCreate(session, uri);
    if (!swMetadata.isOK()) {
        auto status = swMetadata.getStatus();
        LOGV2_ERROR(22357,
                    "Failed to get metadata for {uri}",
                    "Rebuilding ident failed: failed to get metadata",
                    "uri"_attr = uri,
                    "error"_attr = status);
        return status;
    }

    int rc = session->drop(session, uri, nullptr);
    // WT may return EBUSY if the database contains dirty data. If we checkpoint and retry the
    // operation it will attempt to clean up the dirty elements during checkpointing, thus allowing
    // the operation to succeed if it was the only reason to fail.
    if (rc == EBUSY) {
        _checkpoint(session);
        rc = session->drop(session, uri, nullptr);
    }
    if (rc != 0) {
        auto status = wtRCToStatus(rc, session);
        LOGV2_ERROR(22358,
                    "Failed to drop {uri}",
                    "Rebuilding ident failed: failed to drop",
                    "uri"_attr = uri,
                    "error"_attr = status);
        return status;
    }

    rc = session->create(session, uri, swMetadata.getValue().c_str());
    if (rc != 0) {
        auto status = wtRCToStatus(rc, session);
        LOGV2_ERROR(22359,
                    "Failed to create {uri} with config: {config}",
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
    if (_ephemeral) {
        return;
    }

    const Timestamp stableTimestamp = getStableTimestamp();
    const Timestamp initialDataTimestamp = getInitialDataTimestamp();
    uassert(
        5841000,
        "Cannot take checkpoints when the stable timestamp is less than the initial data timestamp",
        initialDataTimestamp == Timestamp::kAllowUnstableCheckpointsSentinel ||
            stableTimestamp >= initialDataTimestamp);

    // Immediately flush the size storer information to disk. When the node is fsync locked for
    // operations such as backup, it's imperative that we copy the most up-to-date data files.
    syncSizeInfo(true);

    // If there's no journal (ephemeral), we must checkpoint all of the data.
    WiredTigerSessionCache::Fsync fsyncType = !_ephemeral
        ? WiredTigerSessionCache::Fsync::kCheckpointStableTimestamp
        : WiredTigerSessionCache::Fsync::kCheckpointAll;

    // We will skip updating the journal listener if the caller holds read locks.
    // The JournalListener may do writes, and taking write locks would conflict with the read locks.
    WiredTigerSessionCache::UseJournalListener useListener = callerHoldsReadLock
        ? WiredTigerSessionCache::UseJournalListener::kSkip
        : WiredTigerSessionCache::UseJournalListener::kUpdate;

    _sessionCache->waitUntilDurable(opCtx, fsyncType, useListener);
}

Status WiredTigerKVEngine::beginBackup(OperationContext* opCtx) {
    invariant(!_backupSession);

    // The inMemory Storage Engine cannot create a backup cursor.
    if (_ephemeral) {
        return Status::OK();
    }

    // Persist the sizeStorer information to disk before opening the backup cursor.
    syncSizeInfo(true);

    // This cursor will be freed by the backupSession being closed as the session is uncached
    auto session = std::make_unique<WiredTigerSession>(_conn);
    WT_CURSOR* c = nullptr;
    WT_SESSION* s = session->getSession();
    int ret = WT_OP_CHECK(s->open_cursor(s, "backup:", nullptr, nullptr, &c));
    if (ret != 0) {
        return wtRCToStatus(ret, s);
    }
    _backupSession = std::move(session);
    return Status::OK();
}

void WiredTigerKVEngine::endBackup(OperationContext* opCtx) {
    if (_sessionCache->isShuttingDown()) {
        // There could be a race with clean shutdown which unconditionally closes all the sessions.
        _backupSession->_session = nullptr;  // Prevent calling _session->close() in destructor.
    }
    _backupSession.reset();
}

Status WiredTigerKVEngine::disableIncrementalBackup(OperationContext* opCtx) {
    // Opening an incremental backup cursor with the "force_stop=true" configuration option then
    // closing the cursor will set a flag in WiredTiger that causes it to release all incremental
    // information and resources.
    // Opening a subsequent incremental backup cursor will reset the flag in WiredTiger and
    // reinstate incremental backup history.
    uassert(31401, "Cannot open backup cursor with in-memory storage engine.", !isEphemeral());

    auto sessionRaii = std::make_unique<WiredTigerSession>(_conn);
    WT_CURSOR* cursor = nullptr;
    WT_SESSION* session = sessionRaii->getSession();
    int wtRet =
        session->open_cursor(session, "backup:", nullptr, "incremental=(force_stop=true)", &cursor);
    if (wtRet != 0) {
        LOGV2_ERROR(22360, "Could not open a backup cursor to disable incremental backups");
        return wtRCToStatus(wtRet, session);
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
    explicit StreamingCursorImpl(WT_SESSION* session,
                                 std::string path,
                                 boost::optional<Timestamp> checkpointTimestamp,
                                 StorageEngine::BackupOptions options,
                                 WiredTigerBackup* wtBackup)
        : StorageEngine::StreamingCursor(options),
          _session(session),
          _path(path),
          _checkpointTimestamp(checkpointTimestamp),
          _wtBackup(wtBackup){};

    ~StreamingCursorImpl() = default;

    StatusWith<std::deque<BackupBlock>> getNextBatch(OperationContext* opCtx,
                                                     const std::size_t batchSize) {
        int wtRet = 0;
        std::deque<BackupBlock> backupBlocks;

        stdx::lock_guard<Latch> backupCursorLk(_wtBackup->wtBackupCursorMutex);
        while (backupBlocks.size() < batchSize) {
            stdx::lock_guard<Latch> backupDupCursorLk(_wtBackup->wtBackupDupCursorMutex);

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
                // If extendBackupCursor() is called prior to the StreamingCursor running into log
                // files, we must ensure that subsequent calls to getNextBatch() do not return
                // duplicate files.
                if ((_wtBackup->logFilePathsSeenByExtendBackupCursor).find(filePath.string()) !=
                    (_wtBackup->logFilePathsSeenByExtendBackupCursor).end()) {
                    break;
                }
                (_wtBackup->logFilePathsSeenByGetNextBatch).insert(filePath.string());
            }

            boost::system::error_code errorCode;
            const std::uint64_t fileSize = boost::filesystem::file_size(filePath, errorCode);
            uassert(31403,
                    "Failed to get a file's size. Filename: {} Error: {}"_format(
                        filePath.string(), errorCode.message()),
                    !errorCode);

            if (options.incrementalBackup && options.srcBackupName) {
                // For a subsequent incremental backup, each BackupBlock corresponds to changes
                // made to data files since the initial incremental backup. Each BackupBlock has a
                // maximum size of options.blockSizeMB. Incremental backups open a duplicate cursor,
                // which is stored in _wtBackup->dupCursor.
                //
                // 'backupBlocks' is an out parameter.
                Status status = _getNextIncrementalBatchForFile(
                    opCtx, filename, filePath, fileSize, batchSize, &backupBlocks);

                if (!status.isOK()) {
                    return status;
                }
            } else {
                // For a full backup or the initial incremental backup, each BackupBlock corresponds
                // to an entire file. Full backups cannot open an incremental cursor, even if they
                // are the initial incremental backup.
                const std::uint64_t length = options.incrementalBackup ? fileSize : 0;
                backupBlocks.push_back(BackupBlock(opCtx,
                                                   filePath.string(),
                                                   _wtBackup->identToNamespaceAndUUIDMap,
                                                   _checkpointTimestamp,
                                                   0 /* offset */,
                                                   length,
                                                   fileSize));
            }
        }

        if (wtRet && wtRet != WT_NOTFOUND && backupBlocks.size() != batchSize) {
            return wtRCToStatus(wtRet, _session);
        }

        return backupBlocks;
    }

private:
    Status _getNextIncrementalBatchForFile(OperationContext* opCtx,
                                           const char* filename,
                                           boost::filesystem::path filePath,
                                           const std::uint64_t fileSize,
                                           const std::size_t batchSize,
                                           std::deque<BackupBlock>* backupBlocks) {
        // For each file listed, open a duplicate backup cursor and get the blocks to copy.
        std::stringstream ss;
        ss << "incremental=(file=" << filename << ")";
        const std::string config = ss.str();

        int wtRet;
        bool fileUnchangedFlag = false;
        if (!_wtBackup->dupCursor) {
            wtRet = (_session)->open_cursor(
                _session, nullptr, _wtBackup->cursor, config.c_str(), &_wtBackup->dupCursor);
            if (wtRet != 0) {
                return wtRCToStatus(wtRet, _session);
            }
            fileUnchangedFlag = true;
        }

        while (backupBlocks->size() < batchSize) {
            wtRet = (_wtBackup->dupCursor)->next(_wtBackup->dupCursor);
            if (wtRet == WT_NOTFOUND) {
                break;
            }
            invariantWTOK(wtRet, _wtBackup->dupCursor->session);
            fileUnchangedFlag = false;

            uint64_t offset, size, type;
            invariantWTOK(
                (_wtBackup->dupCursor)->get_key(_wtBackup->dupCursor, &offset, &size, &type),
                _wtBackup->dupCursor->session);
            LOGV2_DEBUG(22311,
                        2,
                        "Block to copy for incremental backup: filename: {filePath_string}, "
                        "offset: {offset}, size: {size}, type: {type}",
                        "filePath_string"_attr = filePath.string(),
                        "offset"_attr = offset,
                        "size"_attr = size,
                        "type"_attr = type);
            backupBlocks->push_back(BackupBlock(opCtx,
                                                filePath.string(),
                                                _wtBackup->identToNamespaceAndUUIDMap,
                                                _checkpointTimestamp,
                                                offset,
                                                size,
                                                fileSize));
        }

        // If the file is unchanged, push a BackupBlock with offset=0 and length=0. This allows us
        // to distinguish between an unchanged file and a deleted file in an incremental backup.
        if (fileUnchangedFlag) {
            backupBlocks->push_back(BackupBlock(opCtx,
                                                filePath.string(),
                                                _wtBackup->identToNamespaceAndUUIDMap,
                                                _checkpointTimestamp,
                                                0 /* offset */,
                                                0 /* length */,
                                                fileSize));
        }

        // If the duplicate backup cursor has been exhausted, close it and set
        // _wtBackup->dupCursor=nullptr.
        if (wtRet != 0) {
            if (wtRet != WT_NOTFOUND ||
                (wtRet = (_wtBackup->dupCursor)->close(_wtBackup->dupCursor)) != 0) {
                return wtRCToStatus(wtRet, _session);
            }
            _wtBackup->dupCursor = nullptr;
            (_wtBackup->wtBackupDupCursorCV).notify_one();
        }

        return Status::OK();
    }

    WT_SESSION* _session;
    std::string _path;
    boost::optional<Timestamp> _checkpointTimestamp;
    WiredTigerBackup* _wtBackup;  // '_wtBackup' is an out parameter.
};

}  // namespace

StatusWith<std::unique_ptr<StorageEngine::StreamingCursor>>
WiredTigerKVEngine::beginNonBlockingBackup(OperationContext* opCtx,
                                           boost::optional<Timestamp> checkpointTimestamp,
                                           const StorageEngine::BackupOptions& options) {
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

    stdx::lock_guard<Latch> backupCursorLk(_wtBackup.wtBackupCursorMutex);

    // Create ongoingBackup.lock file to signal recovery that it should delete WiredTiger.backup if
    // we have an unclean shutdown with the cursor still open.
    { boost::filesystem::ofstream ongoingBackup(getOngoingBackupPath()); }

    // Oplog truncation thread won't remove oplog since the checkpoint pinned by the backup cursor.
    stdx::lock_guard<Latch> lock(_oplogPinnedByBackupMutex);
    _oplogPinnedByBackup = Timestamp(_oplogNeededForCrashRecovery.load());
    ScopeGuard pinOplogGuard([&] { _oplogPinnedByBackup = boost::none; });

    // Persist the sizeStorer information to disk before opening the backup cursor. We aren't
    // guaranteed to have the most up-to-date size information after the backup as writes can still
    // occur during a nonblocking backup.
    syncSizeInfo(true);

    // This cursor will be freed by the backupSession being closed as the session is uncached
    auto sessionRaii = std::make_unique<WiredTigerSession>(_conn);
    WT_CURSOR* cursor = nullptr;
    WT_SESSION* session = sessionRaii->getSession();
    const std::string config = ss.str();
    int wtRet = session->open_cursor(session, "backup:", nullptr, config.c_str(), &cursor);
    if (wtRet != 0) {
        boost::filesystem::remove(getOngoingBackupPath());
        return wtRCToStatus(wtRet, session);
    }

    // A nullptr indicates that no duplicate cursor is open during an incremental backup.
    stdx::lock_guard<Latch> backupDupCursorLk(_wtBackup.wtBackupDupCursorMutex);
    _wtBackup.dupCursor = nullptr;

    invariant(_wtBackup.logFilePathsSeenByExtendBackupCursor.empty());
    invariant(_wtBackup.logFilePathsSeenByGetNextBatch.empty());
    invariant(_wtBackup.identToNamespaceAndUUIDMap.empty());

    // Fetching the catalog entries requires reading from the storage engine. During cache pressure,
    // this read could be rolled back. In that case, we need to clear the map.
    ScopeGuard clearGuard([&] { _wtBackup.identToNamespaceAndUUIDMap.clear(); });

    {
        Lock::GlobalLock lk(opCtx, MODE_IS);
        DurableCatalog* catalog = DurableCatalog::get(opCtx);
        std::vector<DurableCatalog::Entry> catalogEntries = catalog->getAllCatalogEntries(opCtx);
        for (const DurableCatalog::Entry& e : catalogEntries) {
            // Populate the collection ident with its namespace and UUID.
            UUID uuid = catalog->getMetaData(opCtx, e.catalogId)->options.uuid.value();
            _wtBackup.identToNamespaceAndUUIDMap.emplace(e.ident, std::make_pair(e.nss, uuid));

            // Populate the collection's index idents with the collection's namespace and UUID.
            std::vector<std::string> idxIdents = catalog->getIndexIdents(opCtx, e.catalogId);
            for (const std::string& idxIdent : idxIdents) {
                _wtBackup.identToNamespaceAndUUIDMap.emplace(idxIdent, std::make_pair(e.nss, uuid));
            }
        }
    }

    auto streamingCursor = std::make_unique<StreamingCursorImpl>(
        session, _path, checkpointTimestamp, options, &_wtBackup);

    clearGuard.dismiss();
    pinOplogGuard.dismiss();
    _backupSession = std::move(sessionRaii);
    _wtBackup.cursor = cursor;

    return streamingCursor;
}

void WiredTigerKVEngine::endNonBlockingBackup(OperationContext* opCtx) {
    stdx::lock_guard<Latch> backupCursorLk(_wtBackup.wtBackupCursorMutex);
    stdx::lock_guard<Latch> backupDupCursorLk(_wtBackup.wtBackupDupCursorMutex);
    _backupSession.reset();
    {
        // Oplog truncation thread can now remove the pinned oplog.
        stdx::lock_guard<Latch> lock(_oplogPinnedByBackupMutex);
        _oplogPinnedByBackup = boost::none;
    }
    _wtBackup.cursor = nullptr;
    _wtBackup.dupCursor = nullptr;
    _wtBackup.logFilePathsSeenByExtendBackupCursor = {};
    _wtBackup.logFilePathsSeenByGetNextBatch = {};
    _wtBackup.identToNamespaceAndUUIDMap = {};

    boost::filesystem::remove(getOngoingBackupPath());
}

StatusWith<std::deque<std::string>> WiredTigerKVEngine::extendBackupCursor(
    OperationContext* opCtx) {
    uassert(51033, "Cannot extend backup cursor with in-memory mode.", !isEphemeral());
    invariant(_wtBackup.cursor);
    stdx::unique_lock<Latch> backupDupCursorLk(_wtBackup.wtBackupDupCursorMutex);

    MONGO_IDLE_THREAD_BLOCK;
    _wtBackup.wtBackupDupCursorCV.wait(backupDupCursorLk, [&] { return !_wtBackup.dupCursor; });

    // Persist the sizeStorer information to disk before extending the backup cursor.
    syncSizeInfo(true);

    // The "target=(\"log:\")" configuration string for the cursor will ensure that we only see the
    // log files when iterating on the cursor.
    WT_CURSOR* cursor = nullptr;
    WT_SESSION* session = _backupSession->getSession();
    int wtRet =
        session->open_cursor(session, nullptr, _wtBackup.cursor, "target=(\"log:\")", &cursor);
    if (wtRet != 0) {
        return wtRCToStatus(wtRet, session);
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
        return wtRCToStatus(wtRet, session);
    }

    wtRet = cursor->close(cursor);
    if (wtRet != 0) {
        return wtRCToStatus(wtRet, session);
    }

    // Once all the backup cursors have been opened on a sharded cluster, we need to ensure that the
    // data being copied from each shard is at the same point-in-time across the entire cluster to
    // have a consistent view of the data. For shards that opened their backup cursor before the
    // established point-in-time for backup, they will need to create a full copy of the additional
    // journal files returned by this method to ensure a consistent backup of the data is taken.
    return getUniqueFiles(filePaths, _wtBackup.logFilePathsSeenByGetNextBatch);
}

void WiredTigerKVEngine::syncSizeInfo(bool sync) const {
    if (!_sizeStorer)
        return;

    try {
        _sizeStorer->flush(sync);
    } catch (const WriteConflictException&) {
        // ignore, we'll try again later.
    }
}

void WiredTigerKVEngine::setOldestActiveTransactionTimestampCallback(
    StorageEngine::OldestActiveTransactionTimestampCallback callback) {
    stdx::lock_guard<Latch> lk(_oldestActiveTransactionTimestampCallbackMutex);
    _oldestActiveTransactionTimestampCallback = std::move(callback);
};

RecoveryUnit* WiredTigerKVEngine::newRecoveryUnit() {
    return new WiredTigerRecoveryUnit(_sessionCache.get());
}

void WiredTigerKVEngine::setRecordStoreExtraOptions(const std::string& options) {
    _rsOptions = options;
}

void WiredTigerKVEngine::setSortedDataInterfaceExtraOptions(const std::string& options) {
    _indexOptions = options;
}

Status WiredTigerKVEngine::createRecordStore(OperationContext* opCtx,
                                             const NamespaceString& nss,
                                             StringData ident,
                                             const CollectionOptions& options,
                                             KeyFormat keyFormat) {
    _ensureIdentPath(ident);
    WiredTigerSession session(_conn);

    StatusWith<std::string> result =
        WiredTigerRecordStore::generateCreateString(_canonicalName,
                                                    nss,
                                                    ident,
                                                    options,
                                                    _rsOptions,
                                                    keyFormat,
                                                    WiredTigerUtil::useTableLogging(nss));

    if (options.clusteredIndex) {
        // A clustered collection requires both CollectionOptions.clusteredIndex and
        // KeyFormat::String. For a clustered record store that is not associated with a clustered
        // collection KeyFormat::String is sufficient.
        uassert(6144100,
                "RecordStore with CollectionOptions.clusteredIndex requires KeyFormat::String",
                keyFormat == KeyFormat::String);
    }

    if (!result.isOK()) {
        return result.getStatus();
    }
    std::string config = result.getValue();

    string uri = _uri(ident);
    WT_SESSION* s = session.getSession();
    LOGV2_DEBUG(22331,
                2,
                "WiredTigerKVEngine::createRecordStore ns: {namespace} uri: {uri} config: {config}",
                logAttrs(nss),
                "uri"_attr = uri,
                "config"_attr = config);
    return wtRCToStatus(s->create(s, uri.c_str(), config.c_str()), s);
}

Status WiredTigerKVEngine::importRecordStore(OperationContext* opCtx,
                                             StringData ident,
                                             const BSONObj& storageMetadata,
                                             const ImportOptions& importOptions) {
    _ensureIdentPath(ident);
    WiredTigerSession session(_conn);

    if (MONGO_unlikely(WTWriteConflictExceptionForImportCollection.shouldFail())) {
        LOGV2(6177300,
              "Failpoint WTWriteConflictExceptionForImportCollection enabled. Throwing "
              "WriteConflictException",
              "ident"_attr = ident);
        throwWriteConflictException(
            str::stream() << "Hit failpoint '"
                          << WTWriteConflictExceptionForImportCollection.getName() << "'.");
    }

    std::string config = uassertStatusOK(
        WiredTigerUtil::generateImportString(ident, storageMetadata, importOptions));

    string uri = _uri(ident);
    WT_SESSION* s = session.getSession();
    LOGV2_DEBUG(5095102,
                2,
                "WiredTigerKVEngine::importRecordStore",
                "uri"_attr = uri,
                "config"_attr = config);

    return wtRCToStatus(s->create(s, uri.c_str(), config.c_str()), s);
}

Status WiredTigerKVEngine::recoverOrphanedIdent(OperationContext* opCtx,
                                                const NamespaceString& nss,
                                                StringData ident,
                                                const CollectionOptions& options) {
#ifdef _WIN32
    return {ErrorCodes::CommandNotSupported, "Orphan file recovery is not supported on Windows"};
#else
    invariant(_inRepairMode);

    // Moves the data file to a temporary name so that a new RecordStore can be created with the
    // same ident name. We will delete the new empty collection and rename the data file back so it
    // can be salvaged.

    boost::optional<boost::filesystem::path> identFilePath = getDataFilePathForIdent(ident);
    if (!identFilePath) {
        return {ErrorCodes::UnknownError, "Data file for ident " + ident + " not found"};
    }

    boost::system::error_code ec;
    invariant(boost::filesystem::exists(*identFilePath, ec));

    boost::filesystem::path tmpFile{*identFilePath};
    tmpFile += ".tmp";

    LOGV2(22332,
          "Renaming data file {file} to temporary file {temporary}",
          "Renaming data file to temporary",
          "file"_attr = identFilePath->generic_string(),
          "temporary"_attr = tmpFile.generic_string());
    auto status = fsyncRename(identFilePath.value(), tmpFile);
    if (!status.isOK()) {
        return status;
    }

    LOGV2(22333,
          "Creating new RecordStore for collection {namespace} with UUID: {uuid}",
          "Creating new RecordStore",
          "namespace"_attr = nss,
          "uuid"_attr = options.uuid);

    status = createRecordStore(opCtx, nss, ident, options);
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
    LOGV2(22335, "Salvaging ident {ident}", "Salvaging ident", "ident"_attr = ident);

    WiredTigerSession sessionWrapper(_conn);
    WT_SESSION* session = sessionWrapper.getSession();
    status = wtRCToStatus(
        session->salvage(session, _uri(ident).c_str(), nullptr), session, "Salvage failed: ");
    LOGV2(4795907, "Salvage complete", "duration"_attr = Date_t::now() - start);
    if (status.isOK()) {
        return {ErrorCodes::DataModifiedByRepair,
                str::stream() << "Salvaged data for ident " << ident};
    }
    LOGV2_WARNING(22354,
                  "Could not salvage data. Rebuilding ident: {status_reason}",
                  "Could not salvage data. Rebuilding ident",
                  "ident"_attr = ident,
                  "error"_attr = status.reason());

    //  If the data is unsalvageable, we should completely rebuild the ident.
    return _rebuildIdent(session, _uri(ident).c_str());
#endif
}

std::unique_ptr<RecordStore> WiredTigerKVEngine::getRecordStore(OperationContext* opCtx,
                                                                const NamespaceString& nss,
                                                                StringData ident,
                                                                const CollectionOptions& options) {

    bool isLogged;
    if (nss.size() == 0) {
        fassert(8423353, ident.startsWith("internal-"));
        isLogged = !getGlobalReplSettings().usingReplSets() &&
            !repl::ReplSettings::shouldRecoverFromOplogAsStandalone();
    } else {
        isLogged = WiredTigerUtil::useTableLogging(nss);
    }

    WiredTigerRecordStore::Params params;
    params.nss = nss;
    params.ident = ident.toString();
    params.engineName = _canonicalName;
    params.isCapped = options.capped;
    params.keyFormat = (options.clusteredIndex) ? KeyFormat::String : KeyFormat::Long;
    // Record stores for clustered collections need to guarantee uniqueness by preventing
    // overwrites.
    params.overwrite = options.clusteredIndex ? false : true;
    params.isEphemeral = _ephemeral;
    params.isLogged = isLogged;
    params.sizeStorer = _sizeStorer.get();
    params.tracksSizeAdjustments = true;
    params.forceUpdateWithFullDocument = options.timeseries != boost::none;

    if (nss.isOplog()) {
        // The oplog collection must have a size provided.
        invariant(options.cappedSize > 0);
        params.oplogMaxSize = options.cappedSize;
    }

    std::unique_ptr<WiredTigerRecordStore> ret;
    ret = std::make_unique<StandardWiredTigerRecordStore>(this, opCtx, params);
    ret->postConstructorInit(opCtx);

    // Sizes should always be checked when creating a collection during rollback or replication
    // recovery. This is in case the size storer information is no longer accurate. This may be
    // necessary if capped deletes are rolled-back, if rollback occurs across a collection rename,
    // or when collection creation is not part of a stable checkpoint.
    const auto replCoord = repl::ReplicationCoordinator::get(getGlobalServiceContext());
    const bool inRollback = replCoord && replCoord->getMemberState().rollback();
    if (inRollback || inReplicationRecovery(getGlobalServiceContext())) {
        ret->checkSize(opCtx);
    }

    return std::move(ret);
}

string WiredTigerKVEngine::_uri(StringData ident) const {
    invariant(ident.find(kTableUriPrefix) == string::npos);
    return kTableUriPrefix + ident.toString();
}

Status WiredTigerKVEngine::createSortedDataInterface(OperationContext* opCtx,
                                                     const NamespaceString& nss,
                                                     const CollectionOptions& collOptions,
                                                     StringData ident,
                                                     const IndexDescriptor* desc) {
    _ensureIdentPath(ident);

    std::string collIndexOptions;

    if (auto storageEngineOptions = collOptions.indexOptionDefaults.getStorageEngine()) {
        collIndexOptions =
            dps::extractElementAtPath(*storageEngineOptions, _canonicalName + ".configString")
                .str();
    }
    // Some unittests use a OperationContextNoop that can't support such lookups.
    StatusWith<std::string> result =
        WiredTigerIndex::generateCreateString(_canonicalName,
                                              _indexOptions,
                                              collIndexOptions,
                                              nss,
                                              *desc,
                                              WiredTigerUtil::useTableLogging(nss));
    if (!result.isOK()) {
        return result.getStatus();
    }

    std::string config = result.getValue();

    LOGV2_DEBUG(
        22336,
        2,
        "WiredTigerKVEngine::createSortedDataInterface uuid: {collection_uuid} ident: {ident} "
        "config: {config}",
        "collection_uuid"_attr = collOptions.uuid,
        "ident"_attr = ident,
        "config"_attr = config);
    return WiredTigerIndex::create(opCtx, _uri(ident), config);
}

Status WiredTigerKVEngine::importSortedDataInterface(OperationContext* opCtx,
                                                     StringData ident,
                                                     const BSONObj& storageMetadata,
                                                     const ImportOptions& importOptions) {
    _ensureIdentPath(ident);

    if (MONGO_unlikely(WTWriteConflictExceptionForImportIndex.shouldFail())) {
        LOGV2(6177301,
              "Failpoint WTWriteConflictExceptionForImportIndex enabled. Throwing "
              "WriteConflictException",
              "ident"_attr = ident);
        throwWriteConflictException(str::stream()
                                    << "Hit failpoint '"
                                    << WTWriteConflictExceptionForImportIndex.getName() << "'.");
    }

    std::string config = uassertStatusOK(
        WiredTigerUtil::generateImportString(ident, storageMetadata, importOptions));

    LOGV2_DEBUG(5095103,
                2,
                "WiredTigerKVEngine::importSortedDataInterface",
                "ident"_attr = ident,
                "config"_attr = config);
    return WiredTigerIndex::create(opCtx, _uri(ident), config);
}

Status WiredTigerKVEngine::dropSortedDataInterface(OperationContext* opCtx, StringData ident) {
    return WiredTigerIndex::Drop(opCtx, _uri(ident));
}

std::unique_ptr<SortedDataInterface> WiredTigerKVEngine::getSortedDataInterface(
    OperationContext* opCtx,
    const NamespaceString& nss,
    const CollectionOptions& collOptions,
    StringData ident,
    const IndexDescriptor* desc) {
    if (desc->isIdIndex()) {
        invariant(!collOptions.clusteredIndex);
        return std::make_unique<WiredTigerIdIndex>(
            opCtx, _uri(ident), ident, desc, WiredTigerUtil::useTableLogging(nss));
    }
    auto keyFormat = (collOptions.clusteredIndex) ? KeyFormat::String : KeyFormat::Long;
    if (desc->unique()) {
        return std::make_unique<WiredTigerIndexUnique>(
            opCtx, _uri(ident), ident, keyFormat, desc, WiredTigerUtil::useTableLogging(nss));
    }

    return std::make_unique<WiredTigerIndexStandard>(
        opCtx, _uri(ident), ident, keyFormat, desc, WiredTigerUtil::useTableLogging(nss));
}

Status WiredTigerKVEngine::createColumnStore(OperationContext* opCtx,
                                             const NamespaceString& ns,
                                             const CollectionOptions& collOptions,
                                             StringData ident,
                                             const IndexDescriptor* desc) {
    _ensureIdentPath(ident);
    invariant(desc->getIndexType() == IndexType::INDEX_COLUMN);

    StatusWith<std::string> result =
        WiredTigerColumnStore::generateCreateString(_canonicalName, ns, *desc);
    if (!result.isOK()) {
        return result.getStatus();
    }

    std::string config = std::move(result.getValue());

    LOGV2_DEBUG(6738400,
                2,
                "WiredTigerKVEngine::createColumnStore",
                "collection_uuid"_attr = collOptions.uuid,
                "ident"_attr = ident,
                "config"_attr = config);
    return WiredTigerColumnStore::create(opCtx, _uri(ident), config);
}

std::unique_ptr<ColumnStore> WiredTigerKVEngine::getColumnStore(
    OperationContext* opCtx,
    const NamespaceString& nss,
    const CollectionOptions& collOptions,
    StringData ident,
    const IndexDescriptor* descriptor) {
    // TODO SERVER-66098 readOnly support.
    const bool readOnly = false;
    return std::make_unique<WiredTigerColumnStore>(opCtx, _uri(ident), ident, descriptor, readOnly);
}

std::unique_ptr<RecordStore> WiredTigerKVEngine::makeTemporaryRecordStore(OperationContext* opCtx,
                                                                          StringData ident,
                                                                          KeyFormat keyFormat) {
    _ensureIdentPath(ident);
    WiredTigerSession wtSession(_conn);

    // We don't log writes to temporary record stores.
    const bool isLogged = false;
    StatusWith<std::string> swConfig =
        WiredTigerRecordStore::generateCreateString(_canonicalName,
                                                    NamespaceString("") /* internal table */,
                                                    ident,
                                                    CollectionOptions(),
                                                    _rsOptions,
                                                    keyFormat,
                                                    isLogged);
    uassertStatusOK(swConfig.getStatus());

    std::string config = swConfig.getValue();

    std::string uri = _uri(ident);
    WT_SESSION* session = wtSession.getSession();
    LOGV2_DEBUG(22337,
                2,
                "WiredTigerKVEngine::makeTemporaryRecordStore",
                "uri"_attr = uri,
                "config"_attr = config);
    uassertStatusOK(wtRCToStatus(session->create(session, uri.c_str(), config.c_str()), session));

    WiredTigerRecordStore::Params params;
    params.nss = NamespaceString("");
    params.ident = ident.toString();
    params.engineName = _canonicalName;
    params.isCapped = false;
    params.keyFormat = keyFormat;
    params.overwrite = true;
    params.isEphemeral = _ephemeral;
    params.isLogged = isLogged;
    // Temporary collections do not need to persist size information to the size storer.
    params.sizeStorer = nullptr;
    // Temporary collections do not need to reconcile collection size/counts.
    params.tracksSizeAdjustments = false;
    params.forceUpdateWithFullDocument = false;

    std::unique_ptr<WiredTigerRecordStore> rs;
    rs = std::make_unique<StandardWiredTigerRecordStore>(this, opCtx, params);
    rs->postConstructorInit(opCtx);

    return std::move(rs);
}

void WiredTigerKVEngine::alterIdentMetadata(OperationContext* opCtx,
                                            StringData ident,
                                            const IndexDescriptor* desc,
                                            bool isForceUpdateMetadata) {
    std::string uri = _uri(ident);
    if (!isForceUpdateMetadata) {
        // Explicitly disallows metadata change, specifically index data format change, on indexes
        // of version 11 and 12. This is extra defensive and can be reconsidered if we expand the
        // use of 'alterIdentMetadata()' to also modify non-data-format properties.
        invariant(!WiredTigerUtil::checkApplicationMetadataFormatVersion(
                       opCtx,
                       uri,
                       kDataFormatV3KeyStringV0UniqueIndexVersionV1,
                       kDataFormatV4KeyStringV1UniqueIndexVersionV2)
                       .isOK());
    }

    // Make the alter call to update metadata without taking exclusive lock to avoid conflicts with
    // concurrent operations.
    std::string alterString =
        WiredTigerIndex::generateAppMetadataString(*desc) + "exclusive_refreshed=false,";
    auto status = alterMetadata(uri, alterString);
    invariantStatusOK(status);
}

Status WiredTigerKVEngine::alterMetadata(StringData uri, StringData config) {
    // Use a dedicated session in an alter operation to avoid transaction issues.
    WiredTigerSession session(_conn);
    auto sessionPtr = session.getSession();

    auto uriNullTerminated = uri.toString();
    auto configNullTerminated = config.toString();

    auto ret =
        sessionPtr->alter(sessionPtr, uriNullTerminated.c_str(), configNullTerminated.c_str());
    // WT may return EBUSY if the database contains dirty data. If we checkpoint and retry the
    // operation it will attempt to clean up the dirty elements during checkpointing, thus allowing
    // the operation to succeed if it was the only reason to fail.
    if (ret == EBUSY) {
        _checkpoint(sessionPtr);
        ret =
            sessionPtr->alter(sessionPtr, uriNullTerminated.c_str(), configNullTerminated.c_str());
    }

    return wtRCToStatus(ret, sessionPtr);
}

Status WiredTigerKVEngine::dropIdent(RecoveryUnit* ru,
                                     StringData ident,
                                     StorageEngine::DropIdentCallback&& onDrop) {
    string uri = _uri(ident);

    WiredTigerRecoveryUnit* wtRu = checked_cast<WiredTigerRecoveryUnit*>(ru);
    wtRu->getSessionNoTxn()->closeAllCursors(uri);
    _sessionCache->closeAllCursors(uri);

    WiredTigerSession session(_conn);

    int ret = session.getSession()->drop(
        session.getSession(), uri.c_str(), "force,checkpoint_wait=false");
    LOGV2_DEBUG(22338, 1, "WT drop", "uri"_attr = uri, "ret"_attr = ret);

    if (ret == EBUSY) {
        // this is expected, queue it up
        {
            stdx::lock_guard<Latch> lk(_identToDropMutex);
            _identToDrop.push_front({std::move(uri), std::move(onDrop)});
        }
        _sessionCache->closeCursorsForQueuedDrops();
        return Status::OK();
    }

    if (onDrop) {
        onDrop();
    }

    if (ret == ENOENT) {
        return Status::OK();
    }

    invariantWTOK(ret, session.getSession());
    return Status::OK();
}

void WiredTigerKVEngine::dropIdentForImport(OperationContext* opCtx, StringData ident) {
    const std::string uri = _uri(ident);

    WiredTigerSession session(_conn);

    // Don't wait for the global checkpoint lock to be obtained in WiredTiger as it can take a
    // substantial amount of time to be obtained if there is a concurrent checkpoint running. We
    // will wait until we obtain exclusive access to the underlying table file though. As it isn't
    // user visible at this stage in the import it should be readily available unless a backup
    // cursor is open. In short, using "checkpoint_wait=false" and "lock_wait=true" means that we
    // can potentially be waiting for a short period of time for WT_SESSION::drop() to run, but
    // would rather get EBUSY than wait a long time for a checkpoint to complete.
    const std::string config = "force=true,checkpoint_wait=false,lock_wait=true,remove_files=false";
    int ret = 0;
    size_t attempt = 0;
    do {
        Status status = opCtx->checkForInterruptNoAssert();
        if (status.code() == ErrorCodes::InterruptedAtShutdown) {
            return;
        }

        ++attempt;

        ret = session.getSession()->drop(session.getSession(), uri.c_str(), config.c_str());
        logAndBackoff(5114600,
                      ::mongo::logv2::LogComponent::kStorage,
                      logv2::LogSeverity::Debug(1),
                      attempt,
                      "WiredTiger dropping ident for import",
                      "uri"_attr = uri,
                      "config"_attr = config,
                      "ret"_attr = ret);
    } while (ret == EBUSY);
    invariantWTOK(ret, session.getSession());
}

std::list<WiredTigerCachedCursor> WiredTigerKVEngine::filterCursorsWithQueuedDrops(
    std::list<WiredTigerCachedCursor>* cache) {
    std::list<WiredTigerCachedCursor> toDrop;

    stdx::lock_guard<Latch> lk(_identToDropMutex);
    if (_identToDrop.empty())
        return toDrop;

    for (auto i = cache->begin(); i != cache->end();) {
        if (!i->_cursor ||
            std::find_if(_identToDrop.begin(), _identToDrop.end(), [i](const auto& identToDrop) {
                return identToDrop.uri == std::string(i->_cursor->uri);
            }) == _identToDrop.end()) {
            ++i;
            continue;
        }
        toDrop.push_back(*i);
        i = cache->erase(i);
    }

    return toDrop;
}

bool WiredTigerKVEngine::haveDropsQueued() const {
    Date_t now = _clockSource->now();
    Milliseconds delta = now - Date_t::fromMillisSinceEpoch(_previousCheckedDropsQueued.load());

    if (_sizeStorerSyncTracker.intervalHasElapsed()) {
        _sizeStorerSyncTracker.resetLastTime();
        syncSizeInfo(false);
    }

    // We only want to check the queue max once per second or we'll thrash
    if (delta < Milliseconds(1000))
        return false;

    _previousCheckedDropsQueued.store(now.toMillisSinceEpoch());

    // Don't wait for the mutex: if we can't get it, report that no drops are queued.
    stdx::unique_lock<Latch> lk(_identToDropMutex, stdx::defer_lock);
    return lk.try_lock() && !_identToDrop.empty();
}

void WiredTigerKVEngine::dropSomeQueuedIdents() {
    int numInQueue;

    WiredTigerSession session(_conn);

    {
        stdx::lock_guard<Latch> lk(_identToDropMutex);
        numInQueue = _identToDrop.size();
    }

    int numToDelete = 10;
    int tenPercentQueue = numInQueue * 0.1;
    if (tenPercentQueue > 10)
        numToDelete = tenPercentQueue;

    LOGV2_DEBUG(22339,
                1,
                "WT Queue: attempting to drop tables",
                "numInQueue"_attr = numInQueue,
                "numToDelete"_attr = numToDelete);
    for (int i = 0; i < numToDelete; i++) {
        IdentToDrop identToDrop;
        {
            stdx::lock_guard<Latch> lk(_identToDropMutex);
            if (_identToDrop.empty())
                break;
            identToDrop = std::move(_identToDrop.front());
            _identToDrop.pop_front();
        }
        int ret = session.getSession()->drop(
            session.getSession(), identToDrop.uri.c_str(), "force,checkpoint_wait=false");
        LOGV2_DEBUG(22340, 1, "WT queued drop", "uri"_attr = identToDrop.uri, "ret"_attr = ret);

        if (ret == EBUSY) {
            stdx::lock_guard<Latch> lk(_identToDropMutex);
            _identToDrop.push_back(std::move(identToDrop));
        } else {
            invariantWTOK(ret, session.getSession());
            if (identToDrop.callback) {
                identToDrop.callback();
            }
        }
    }
}

bool WiredTigerKVEngine::supportsDirectoryPerDB() const {
    return true;
}

void WiredTigerKVEngine::_checkpoint(WT_SESSION* session) {
    // Ephemeral WiredTiger instances cannot do a checkpoint to disk as there is no disk backing
    // the data.
    if (_ephemeral) {
        return;
    }
    // TODO: SERVER-64507: Investigate whether we can smartly rely on one checkpointer if two or
    // more threads checkpoint at the same time.
    stdx::lock_guard lk(_checkpointMutex);

    const Timestamp stableTimestamp = getStableTimestamp();
    const Timestamp initialDataTimestamp = getInitialDataTimestamp();

    // The amount of oplog to keep is primarily dictated by a user setting. However, in unexpected
    // cases, durable, recover to a timestamp storage engines may need to play forward from an oplog
    // entry that would otherwise be truncated by the user setting. Furthermore, the entries in
    // prepared or large transactions can refer to previous entries in the same transaction.
    //
    // Live (replication) rollback will replay the oplog from exactly the stable timestamp. With
    // prepared or large transactions, it may require some additional entries prior to the stable
    // timestamp. These requirements are summarized in getOplogNeededForRollback. Truncating the
    // oplog at this point is sufficient for in-memory configurations, but could cause an
    // unrecoverable scenario if the node crashed and has to play from the last stable checkpoint.
    //
    // By recording the oplog needed for rollback "now", then taking a stable checkpoint, we can
    // safely assume that the oplog needed for crash recovery has caught up to the recorded value.
    // After the checkpoint, this value will be published such that actors which truncate the oplog
    // can read an updated value.
    try {
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
            clearIndividuallyCheckpointedIndexes();
            invariantWTOK(session->checkpoint(session, "use_timestamp=false"), session);
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
        } else {
            auto oplogNeededForRollback = getOplogNeededForRollback();

            LOGV2_FOR_RECOVERY(23986,
                               2,
                               "Performing stable checkpoint.",
                               "stableTimestamp"_attr = stableTimestamp,
                               "oplogNeededForRollback"_attr = toString(oplogNeededForRollback));

            {
                clearIndividuallyCheckpointedIndexes();
                invariantWTOK(session->checkpoint(session, "use_timestamp=true"), session);
            }

            if (oplogNeededForRollback.isOK()) {
                // Now that the checkpoint is durable, publish the oplog needed to recover from it.
                _oplogNeededForCrashRecovery.store(oplogNeededForRollback.getValue().asULL());
            }
        }
    } catch (const WriteConflictException&) {
        LOGV2_WARNING(22346, "Checkpoint encountered a write conflict exception.");
    } catch (const AssertionException& exc) {
        invariant(ErrorCodes::isShutdownError(exc.code()), exc.what());
    }
}

void WiredTigerKVEngine::checkpoint() {
    UniqueWiredTigerSession session = _sessionCache->getSession();
    WT_SESSION* s = session->getSession();
    return _checkpoint(s);
}

bool WiredTigerKVEngine::hasIdent(OperationContext* opCtx, StringData ident) const {
    return _hasUri(WiredTigerRecoveryUnit::get(opCtx)->getSession()->getSession(), _uri(ident));
}

bool WiredTigerKVEngine::_hasUri(WT_SESSION* session, const std::string& uri) const {
    // can't use WiredTigerCursor since this is called from constructor.
    WT_CURSOR* c = nullptr;
    // No need for a metadata:create cursor, since it gathers extra information and is slower.
    int ret = session->open_cursor(session, "metadata:", nullptr, nullptr, &c);
    if (ret == ENOENT)
        return false;
    invariantWTOK(ret, session);
    ON_BLOCK_EXIT([&] { c->close(c); });

    c->set_key(c, uri.c_str());
    return c->search(c) == 0;
}

std::vector<std::string> WiredTigerKVEngine::getAllIdents(OperationContext* opCtx) const {
    std::vector<std::string> all;
    int ret;
    // No need for a metadata:create cursor, since it gathers extra information and is slower.
    WiredTigerCursor cursor("metadata:", WiredTigerSession::kMetadataTableId, false, opCtx);
    WT_CURSOR* c = cursor.get();
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
        if (ident == "sizeStorer")
            continue;

        all.push_back(ident.toString());
    }

    fassert(50663, ret == WT_NOTFOUND);

    return all;
}

boost::optional<boost::filesystem::path> WiredTigerKVEngine::getDataFilePathForIdent(
    StringData ident) const {
    boost::filesystem::path identPath = _path;
    identPath /= ident.toString() + ".wt";

    boost::system::error_code ec;
    if (!boost::filesystem::exists(identPath, ec)) {
        return boost::none;
    }
    return identPath;
}

int WiredTigerKVEngine::reconfigure(const char* str) {
    return _conn->reconfigure(_conn, str);
}

void WiredTigerKVEngine::_ensureIdentPath(StringData ident) {
    size_t start = 0;
    size_t idx;
    while ((idx = ident.find('/', start)) != string::npos) {
        StringData dir = ident.substr(0, idx);

        boost::filesystem::path subdir = _path;
        subdir /= dir.toString();
        if (!boost::filesystem::exists(subdir)) {
            LOGV2_DEBUG(22341, 1, "creating subdirectory: {dir}", "dir"_attr = dir);
            try {
                boost::filesystem::create_directory(subdir);
            } catch (const std::exception& e) {
                LOGV2_ERROR(22361,
                            "error creating path {directory} {error}",
                            "Error creating directory",
                            "directory"_attr = subdir.string(),
                            "error"_attr = e.what());
                throw;
            }
        }

        start = idx + 1;
    }
}

void WiredTigerKVEngine::setJournalListener(JournalListener* jl) {
    return _sessionCache->setJournalListener(jl);
}

namespace {
uint64_t _fetchAllDurableValue(WT_CONNECTION* conn) {
    // Fetch the latest all_durable value from the storage engine. This value will be a timestamp
    // that has no holes (uncommitted transactions with lower timestamps) behind it.
    char buf[(2 * 8 /*bytes in hex*/) + 1 /*nul terminator*/];
    invariantWTOK(conn->query_timestamp(conn, buf, "get=all_durable"), nullptr);

    uint64_t tmp;
    fassert(38002, NumberParser().base(16)(buf, &tmp));
    if (tmp == 0) {
        // Treat this as lowest possible timestamp; we need to see all preexisting data but no new
        // (timestamped) data.
        return StorageEngine::kMinimumTimestamp;
    }

    return tmp;
}
}  // namespace

void WiredTigerKVEngine::setStableTimestamp(Timestamp stableTimestamp, bool force) {
    if (MONGO_unlikely(WTPauseStableTimestamp.shouldFail())) {
        return;
    }

    if (stableTimestamp.isNull()) {
        return;
    }

    // Do not set the stable timestamp backward, unless 'force' is set.
    Timestamp prevStable(_stableTimestamp.load());
    if ((stableTimestamp < prevStable) && !force) {
        return;
    }

    // Communicate to WiredTiger what the "stable timestamp" is. Timestamp-aware checkpoints will
    // only persist to disk transactions committed with a timestamp earlier than the "stable
    // timestamp".
    //
    // After passing the "stable timestamp" to WiredTiger, communicate it to the
    // `CheckpointThread`. It's not obvious a stale stable timestamp in the `CheckpointThread` is
    // safe. Consider the following arguments:
    //
    // Setting the "stable timestamp" is only meaningful when the "initial data timestamp" is real
    // (i.e: not `kAllowUnstableCheckpointsSentinel`). In this normal case, the `stableTimestamp`
    // input must be greater than the current value. The only effect this can have in the
    // `CheckpointThread` is to transition it from a state of not taking any checkpoints, to
    // taking "stable checkpoints". In the transitioning case, it's imperative for the "stable
    // timestamp" to have first been communicated to WiredTiger.
    std::string stableTSConfigString;
    auto ts = stableTimestamp.asULL();
    if (force) {
        stableTSConfigString =
            "force=true,oldest_timestamp={0:x},durable_timestamp={0:x},stable_timestamp={0:x}"_format(
                ts);
        stdx::lock_guard<Latch> lk(_highestDurableTimestampMutex);
        _highestSeenDurableTimestamp = ts;
    } else {
        stableTSConfigString = "stable_timestamp={:x}"_format(ts);
    }
    invariantWTOK(_conn->set_timestamp(_conn, stableTSConfigString.c_str()), nullptr);

    // After publishing a stable timestamp to WT, we can record the updated stable timestamp value
    // for the necessary oplog to keep.
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

    // This mutex is not intended to synchronize updates to the oldest timestamp, but to ensure that
    // there are no races with pinning the oldest timestamp.
    stdx::lock_guard<Latch> lock(_oldestTimestampPinRequestsMutex);
    const Timestamp currOldestTimestamp = Timestamp(_oldestTimestamp.load());
    for (auto it : _oldestTimestampPinRequests) {
        invariant(it.second >= currOldestTimestamp);
        newOldestTimestamp = std::min(newOldestTimestamp, it.second);
    }

    if (force) {
        // The oldest timestamp should only be forced backwards during replication recovery in order
        // to do rollback via refetch. This refetching process invalidates any timestamped snapshots
        // until after it completes. Components that register a pinned timestamp must synchronize
        // with events that invalidate their snapshots, unpin themselves and either fail themselves,
        // or reacquire a new snapshot after the rollback event.
        //
        // Forcing the oldest timestamp forward -- potentially past a pin request raises the
        // question of whether the pin should be honored. For now we will invariant there is no pin,
        // but the invariant can be relaxed if there's a use-case to support.
        invariant(_oldestTimestampPinRequests.empty());
    }

    if (force) {
        auto oldestTSConfigString =
            "force=true,oldest_timestamp={0:x},durable_timestamp={0:x}"_format(
                newOldestTimestamp.asULL());
        invariantWTOK(_conn->set_timestamp(_conn, oldestTSConfigString.c_str()), nullptr);
        _oldestTimestamp.store(newOldestTimestamp.asULL());
        stdx::lock_guard<Latch> lk(_highestDurableTimestampMutex);
        _highestSeenDurableTimestamp = newOldestTimestamp.asULL();
        LOGV2_DEBUG(22342,
                    2,
                    "oldest_timestamp and durable_timestamp force set to {newOldestTimestamp}",
                    "newOldestTimestamp"_attr = newOldestTimestamp);
    } else {
        auto oldestTSConfigString = "oldest_timestamp={:x}"_format(newOldestTimestamp.asULL());
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

    if (_ephemeral && !TestingProctor::instance().isEnabled()) {
        // No history should be maintained for the inMemory engine because it is not used yet.
        invariant(minSnapshotHistoryWindowInSeconds.load() == 0);
    }

    if (stableTimestamp.getSecs() <
        static_cast<unsigned>(minSnapshotHistoryWindowInSeconds.load())) {
        // The history window is larger than the timestamp history thus far. We must wait for
        // the history to reach the window size before moving oldest_timestamp forward. This should
        // only happen in unit tests.
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
    if (!_keepDataHistory) {
        return false;
    }
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

StatusWith<Timestamp> WiredTigerKVEngine::recoverToStableTimestamp(OperationContext* opCtx) {
    if (!supportsRecoverToStableTimestamp()) {
        LOGV2_FATAL(50665, "WiredTiger is configured to not support recover to a stable timestamp");
    }

    if (!_canRecoverToStableTimestamp()) {
        Timestamp stableTS(_stableTimestamp.load());
        Timestamp initialDataTS(_initialDataTimestamp.load());
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
                       "Rolling back to the stable timestamp. StableTimestamp: {stableTimestamp} "
                       "Initial Data Timestamp: {initialDataTimestamp}",
                       "Rolling back to the stable timestamp",
                       "stableTimestamp"_attr = stableTimestamp,
                       "initialDataTimestamp"_attr = initialDataTimestamp);
    int ret = 0;

    // The rollback_to_stable operation requires all open cursors to be closed or reset before the
    // call, otherwise EBUSY will be returned. Occasionally, there could be an operation that hasn't
    // been killed yet, such as the CappedInsertNotifier for a yielded oplog getMore. We will retry
    // rollback_to_stable until the system quiesces.
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
        opCtx->sleepFor(Seconds(1));
    } while (ret == EBUSY);

    if (ret) {
        return {ErrorCodes::UnrecoverableRollbackError,
                str::stream() << "Error rolling back to stable. Err: " << wiredtiger_strerror(ret)};
    }

    {
        // Rollback the highest seen durable timestamp to the stable timestamp.
        stdx::lock_guard<Latch> lk(_highestDurableTimestampMutex);
        _highestSeenDurableTimestamp = stableTimestamp.asULL();
    }

    _sizeStorer = std::make_unique<WiredTigerSizeStorer>(_conn, _sizeStorerUri);

    return {stableTimestamp};
}

Timestamp WiredTigerKVEngine::getAllDurableTimestamp() const {
    auto ret = _fetchAllDurableValue(_conn);

    stdx::lock_guard<Latch> lk(_highestDurableTimestampMutex);
    if (ret < _highestSeenDurableTimestamp) {
        ret = _highestSeenDurableTimestamp;
    } else {
        _highestSeenDurableTimestamp = ret;
    }
    return Timestamp(ret);
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
    if (_ephemeral) {
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
    // Get the current stable timestamp and use it throughout this function, ignoring updates from
    // another thread.
    auto stableTimestamp = _stableTimestamp.load();

    // Only one thread can set or execute this callback.
    stdx::lock_guard<Latch> lk(_oldestActiveTransactionTimestampCallbackMutex);
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
    if (_ephemeral) {
        return boost::none;
    }

    return Timestamp(_oplogNeededForCrashRecovery.load());
}

Timestamp WiredTigerKVEngine::getPinnedOplog() const {
    // The storage engine may have been told to keep oplog back to a certain timestamp.
    Timestamp pinned = Timestamp(_pinnedOplogTimestamp.load());

    {
        stdx::lock_guard<Latch> lock(_oplogPinnedByBackupMutex);
        if (!storageGlobalParams.allowOplogTruncation) {
            // If oplog truncation is not allowed, then return the min timestamp so that no history
            // is ever allowed to be deleted.
            return Timestamp::min();
        }
        if (_oplogPinnedByBackup) {
            // All the oplog since `_oplogPinnedByBackup` should remain intact during the backup.
            return std::min(_oplogPinnedByBackup.value(), pinned);
        }
    }

    auto oplogNeededForCrashRecovery = getOplogNeededForCrashRecovery();
    if (!_keepDataHistory) {
        // We use rollbackViaRefetch, so we only need to pin oplog for crash recovery.
        return std::min((oplogNeededForCrashRecovery.value_or(Timestamp::max())), pinned);
    }

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
    OperationContext* opCtx,
    const std::string& requestingServiceName,
    Timestamp requestedTimestamp,
    bool roundUpIfTooOld) {
    stdx::lock_guard<Latch> lock(_oldestTimestampPinRequestsMutex);
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

    if (opCtx->lockState()->inAWriteUnitOfWork()) {
        // If we've moved the pin and are in a `WriteUnitOfWork`, assume the caller has a write that
        // should be atomic with this pin request. If the `WriteUnitOfWork` is rolled back, either
        // unpin the oldest timestamp or repin the previous value.
        opCtx->recoveryUnit()->onRollback(
            [this, svcName = requestingServiceName, previousTimestamp]() {
                if (previousTimestamp.isNull()) {
                    unpinOldestTimestamp(svcName);
                } else {
                    stdx::lock_guard<Latch> lock(_oldestTimestampPinRequestsMutex);
                    // When a write is updating the value from an earlier pin to a later one, use
                    // rounding to make a best effort to repin the earlier value.
                    invariant(_pinOldestTimestamp(lock, svcName, previousTimestamp, true).isOK());
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
                    "Requested timestamp: {} Current oldest timestamp: {}"_format(
                        requestedTimestamp.toString(), oldest.toString())};
        }
    }

    _oldestTimestampPinRequests[requestingServiceName] = requestedTimestamp;
    return {requestedTimestamp};
}

void WiredTigerKVEngine::unpinOldestTimestamp(const std::string& requestingServiceName) {
    stdx::lock_guard<Latch> lock(_oldestTimestampPinRequestsMutex);
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
    stdx::lock_guard<Latch> lock(_oldestTimestampPinRequestsMutex);
    return _oldestTimestampPinRequests;
}

void WiredTigerKVEngine::setPinnedOplogTimestamp(const Timestamp& pinnedTimestamp) {
    _pinnedOplogTimestamp.store(pinnedTimestamp.asULL());
}

bool WiredTigerKVEngine::supportsReadConcernSnapshot() const {
    return true;
}

bool WiredTigerKVEngine::supportsReadConcernMajority() const {
    return _keepDataHistory;
}

bool WiredTigerKVEngine::supportsOplogStones() const {
    return true;
}

void WiredTigerKVEngine::startOplogManager(OperationContext* opCtx,
                                           WiredTigerRecordStore* oplogRecordStore) {
    stdx::lock_guard<Latch> lock(_oplogManagerMutex);
    // Halt visibility thread if running on previous record store
    if (_oplogRecordStore) {
        _oplogManager->haltVisibilityThread();
    }

    _oplogManager->startVisibilityThread(opCtx, oplogRecordStore);
    _oplogRecordStore = oplogRecordStore;
}

void WiredTigerKVEngine::haltOplogManager(WiredTigerRecordStore* oplogRecordStore,
                                          bool shuttingDown) {
    stdx::unique_lock<Latch> lock(_oplogManagerMutex);
    // Halt the visibility thread if we're in shutdown or the request matches the current record
    // store.
    if (shuttingDown || _oplogRecordStore == oplogRecordStore) {
        _oplogManager->haltVisibilityThread();
        _oplogRecordStore = nullptr;
    }
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

void WiredTigerKVEngine::dump() const {
    int ret = _conn->debug_info(_conn, "cursors=true,handles=true,log=true,sessions=true,txn=true");
    auto status = wtRCToStatus(ret, nullptr, "WiredTigerKVEngine::dump()");
    if (status.isOK()) {
        LOGV2(6117700, "WiredTigerKVEngine::dump() completed successfully");
    } else {
        LOGV2(6117701, "WiredTigerKVEngine::dump() failed", "error"_attr = status);
    }
}

Status WiredTigerKVEngine::reconfigureLogging() {
    auto verboseConfig = WiredTigerUtil::generateWTVerboseConfiguration();
    return wtRCToStatus(_conn->reconfigure(_conn, verboseConfig.c_str()), nullptr);
}

StatusWith<BSONObj> WiredTigerKVEngine::getStorageMetadata(StringData ident) const {
    auto session = _sessionCache->getSession();

    auto tableMetadata =
        WiredTigerUtil::getMetadata(session->getSession(), "table:{}"_format(ident));
    if (!tableMetadata.isOK()) {
        return tableMetadata.getStatus();
    }

    auto fileMetadata =
        WiredTigerUtil::getMetadata(session->getSession(), "file:{}.wt"_format(ident));
    if (!fileMetadata.isOK()) {
        return fileMetadata.getStatus();
    }

    return BSON("tableMetadata" << tableMetadata.getValue() << "fileMetadata"
                                << fileMetadata.getValue());
}

}  // namespace mongo
