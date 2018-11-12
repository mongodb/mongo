// wiredtiger_kv_engine.cpp


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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kStorage
#define LOG_FOR_RECOVERY(level) \
    MONGO_LOG_COMPONENT(level, ::mongo::logger::LogComponent::kStorageRecovery)
#define LOG_FOR_ROLLBACK(level) \
    MONGO_LOG_COMPONENT(level, ::mongo::logger::LogComponent::kReplicationRollback)

#include "mongo/platform/basic.h"

#ifdef _WIN32
#define NVALGRIND
#endif

#include <memory>

#include "mongo/db/storage/wiredtiger/wiredtiger_kv_engine.h"

#include <boost/filesystem.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>
#include <boost/system/error_code.hpp>
#include <valgrind/valgrind.h>

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/bson/dotted_path_support.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/collection_catalog_entry.h"
#include "mongo/db/client.h"
#include "mongo/db/commands/server_status_metric.h"
#include "mongo/db/concurrency/locker.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/global_settings.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/repl/repl_settings.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/server_options.h"
#include "mongo/db/server_parameters.h"
#include "mongo/db/service_context.h"
#include "mongo/db/snapshot_window_options.h"
#include "mongo/db/storage/journal_listener.h"
#include "mongo/db/storage/storage_file_util.h"
#include "mongo/db/storage/storage_options.h"
#include "mongo/db/storage/storage_repair_observer.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_cursor.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_customization_hooks.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_extensions.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_global_options.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_index.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_record_store.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_recovery_unit.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_session_cache.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_size_storer.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/stdx/memory.h"
#include "mongo/util/background.h"
#include "mongo/util/concurrency/idle_thread_block.h"
#include "mongo/util/concurrency/ticketholder.h"
#include "mongo/util/exit.h"
#include "mongo/util/log.h"
#include "mongo/util/processinfo.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/time_support.h"

#if !defined(__has_feature)
#define __has_feature(x) 0
#endif

namespace mongo {

bool WiredTigerFileVersion::shouldDowngrade(bool readOnly,
                                            bool repairMode,
                                            bool hasRecoveryTimestamp) {
    if (readOnly) {
        // A read-only state must not have upgraded. Nor could it downgrade.
        return false;
    }

    const auto replCoord = repl::ReplicationCoordinator::get(getGlobalServiceContext());
    const auto memberState = replCoord->getMemberState();
    if (memberState.arbiter()) {
        return true;
    }

    if (!serverGlobalParams.featureCompatibility.isVersionInitialized()) {
        // If the FCV document hasn't been read, trust the WT compatibility. MongoD will
        // downgrade to the same compatibility it discovered on startup.
        return _startupVersion == StartupVersion::IS_40 ||
            _startupVersion == StartupVersion::IS_36 || _startupVersion == StartupVersion::IS_34;
    }

    if (serverGlobalParams.featureCompatibility.getVersion() !=
        ServerGlobalParams::FeatureCompatibility::Version::kFullyDowngradedTo40) {
        // Only consider downgrading when FCV is set to 4.0
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
        invariant(_startupVersion != StartupVersion::IS_42);

        switch (_startupVersion) {
            case StartupVersion::IS_34:
                return "compatibility=(release=2.9)";
            case StartupVersion::IS_36:
                return "compatibility=(release=3.0)";
            case StartupVersion::IS_40:
                return "compatibility=(release=3.1)";
            default:
                MONGO_UNREACHABLE;
        }
    }
    return "compatibility=(release=3.1)";
}

using std::set;
using std::string;

namespace dps = ::mongo::dotted_path_support;

const int WiredTigerKVEngine::kDefaultJournalDelayMillis = 100;

class WiredTigerKVEngine::WiredTigerJournalFlusher : public BackgroundJob {
public:
    explicit WiredTigerJournalFlusher(WiredTigerSessionCache* sessionCache)
        : BackgroundJob(false /* deleteSelf */), _sessionCache(sessionCache) {}

    virtual string name() const {
        return "WTJournalFlusher";
    }

    virtual void run() {
        ThreadClient tc(name(), getGlobalServiceContext());
        LOG(1) << "starting " << name() << " thread";

        while (!_shuttingDown.load()) {
            try {
                const bool forceCheckpoint = false;
                const bool stableCheckpoint = false;
                _sessionCache->waitUntilDurable(forceCheckpoint, stableCheckpoint);
            } catch (const AssertionException& e) {
                invariant(e.code() == ErrorCodes::ShutdownInProgress);
            }

            int ms = storageGlobalParams.journalCommitIntervalMs.load();
            if (!ms) {
                ms = kDefaultJournalDelayMillis;
            }

            MONGO_IDLE_THREAD_BLOCK;
            sleepmillis(ms);
        }
        LOG(1) << "stopping " << name() << " thread";
    }

    void shutdown() {
        _shuttingDown.store(true);
        wait();
    }

private:
    WiredTigerSessionCache* _sessionCache;
    AtomicBool _shuttingDown{false};
};

class WiredTigerKVEngine::WiredTigerCheckpointThread : public BackgroundJob {
public:
    explicit WiredTigerCheckpointThread(WiredTigerKVEngine* wiredTigerKVEngine,
                                        WiredTigerSessionCache* sessionCache)
        : BackgroundJob(false /* deleteSelf */),
          _wiredTigerKVEngine(wiredTigerKVEngine),
          _sessionCache(sessionCache) {}

    virtual string name() const {
        return "WTCheckpointThread";
    }

    virtual void run() {
        ThreadClient tc(name(), getGlobalServiceContext());
        LOG(1) << "starting " << name() << " thread";

        while (!_shuttingDown.load()) {
            {
                stdx::unique_lock<stdx::mutex> lock(_mutex);
                MONGO_IDLE_THREAD_BLOCK;
                _condvar.wait_for(lock,
                                  stdx::chrono::seconds(static_cast<std::int64_t>(
                                      wiredTigerGlobalOptions.checkpointDelaySecs)));
            }

            const Timestamp stableTimestamp = _wiredTigerKVEngine->getStableTimestamp();
            const Timestamp initialDataTimestamp = _wiredTigerKVEngine->getInitialDataTimestamp();

            // The amount of oplog to keep is primarily dictated by a user setting. However, in
            // unexpected cases, durable, recover to a timestamp storage engines may need to play
            // forward from an oplog entry that would otherwise be truncated by the user
            // setting. Furthermore with prepared transactions, oplog entries can refer to
            // previous oplog entries.
            //
            // Live (replication) rollback will replay oplogs from exactly the stable
            // timestamp. With prepared transactions, it may require some additional entries prior
            // to the stable timestamp. These requirements are summarized in
            // `getOplogNeededForRollback`. Truncating the oplog at this point is sufficient for
            // in-memory configurations, but could cause an unrecoverable scenario if the node
            // crashed and has to play from the last stable checkpoint.
            //
            // By recording the oplog needed for rollback "now", then taking a stable checkpoint,
            // we can safely assume that the oplog needed for crash recovery has caught up to the
            // recorded value. After the checkpoint, this value will be published such that actors
            // which truncate the oplog can read an updated value.
            const Timestamp oplogNeededForRollback =
                _wiredTigerKVEngine->getOplogNeededForRollback();
            try {
                // Three cases:
                //
                // First, initialDataTimestamp is Timestamp(0, 1) -> Take full checkpoint. This is
                // when there is no consistent view of the data (i.e: during initial sync).
                //
                // Second, enableMajorityReadConcern is false. In this case, we are not tracking a
                // stable timestamp. Take a full checkpoint.
                //
                // Third, stableTimestamp < initialDataTimestamp: Skip checkpoints. The data on disk
                // is prone to being rolled back. Hold off on checkpoints.  Hope that the stable
                // timestamp surpasses the data on disk, allowing storage to persist newer copies to
                // disk.
                //
                // Fourth, stableTimestamp >= initialDataTimestamp: Take stable checkpoint. Steady
                // state case.
                if (initialDataTimestamp.asULL() <= 1) {
                    UniqueWiredTigerSession session = _sessionCache->getSession();
                    WT_SESSION* s = session->getSession();
                    invariantWTOK(s->checkpoint(s, "use_timestamp=false"));
                } else if (!serverGlobalParams.enableMajorityReadConcern) {
                    UniqueWiredTigerSession session = _sessionCache->getSession();
                    WT_SESSION* s = session->getSession();
                    invariantWTOK(s->checkpoint(s, "use_timestamp=false"));
                } else if (stableTimestamp < initialDataTimestamp) {
                    LOG_FOR_RECOVERY(2)
                        << "Stable timestamp is behind the initial data timestamp, skipping "
                           "a checkpoint. StableTimestamp: "
                        << stableTimestamp.toString()
                        << " InitialDataTimestamp: " << initialDataTimestamp.toString();
                } else {
                    // 'stableTimestamp' is the smallest possible value at which WT will take a
                    // stable checkpoint. A newer stable timestamp may be used by WT if one is
                    // concurrently set.
                    LOG_FOR_RECOVERY(2) << "Performing stable checkpoint. StableTimestamp: "
                                        << stableTimestamp;

                    UniqueWiredTigerSession session = _sessionCache->getSession();
                    WT_SESSION* s = session->getSession();
                    invariantWTOK(s->checkpoint(s, "use_timestamp=true"));

                    // Now that the checkpoint is durable, publish the oplog needed to recover
                    // from it.
                    _oplogNeededForCrashRecovery.store(oplogNeededForRollback.asULL());
                }
            } catch (const WriteConflictException&) {
                // Temporary: remove this after WT-3483
                warning() << "Checkpoint encountered a write conflict exception.";
            } catch (const AssertionException& exc) {
                invariant(ErrorCodes::isShutdownError(exc.code()), exc.what());
            }
        }
        LOG(1) << "stopping " << name() << " thread";
    }

    /**
     * Returns true if we have already triggered taking the first checkpoint.
     */
    bool hasTriggeredFirstStableCheckpoint() {
        return _hasTriggeredFirstStableCheckpoint;
    }

    /**
     * Triggers taking the first stable checkpoint, which is when the stable timestamp advances past
     * the initial data timestamp.
     *
     * The checkpoint thread runs automatically every wiredTigerGlobalOptions.checkpointDelaySecs
     * seconds. This function avoids potentially waiting that full duration for a stable checkpoint,
     * initiating one immediately.
     *
     * Do not call this function if hasTriggeredFirstStableCheckpoint() returns true.
     */
    void triggerFirstStableCheckpoint(Timestamp prevStable,
                                      Timestamp initialData,
                                      Timestamp currStable) {
        invariant(!_hasTriggeredFirstStableCheckpoint);
        if (prevStable < initialData && currStable >= initialData) {
            _hasTriggeredFirstStableCheckpoint = true;
            log() << "Triggering the first stable checkpoint. Initial Data: " << initialData
                  << " PrevStable: " << prevStable << " CurrStable: " << currStable;
            stdx::unique_lock<stdx::mutex> lock(_mutex);
            _condvar.notify_one();
        }
    }

    std::uint64_t getOplogNeededForCrashRecovery() const {
        return _oplogNeededForCrashRecovery.load();
    }

    void shutdown() {
        _shuttingDown.store(true);
        {
            stdx::unique_lock<stdx::mutex> lock(_mutex);
            // Wake up the checkpoint thread early, to take a final checkpoint before shutting
            // down, if one has not coincidentally just been taken.
            _condvar.notify_one();
        }
        wait();
    }

private:
    WiredTigerKVEngine* _wiredTigerKVEngine;
    WiredTigerSessionCache* _sessionCache;

    stdx::mutex _mutex;  // protects _condvar
    // The checkpoint thead idles on this condition variable for a particular time duration between
    // taking checkpoints. It can be triggered early to expediate immediate checkpointing.
    stdx::condition_variable _condvar;

    AtomicBool _shuttingDown{false};

    bool _hasTriggeredFirstStableCheckpoint = false;

    AtomicWord<std::uint64_t> _oplogNeededForCrashRecovery;
};

namespace {

class TicketServerParameter : public ServerParameter {
    MONGO_DISALLOW_COPYING(TicketServerParameter);

public:
    TicketServerParameter(TicketHolder* holder, const std::string& name)
        : ServerParameter(ServerParameterSet::getGlobal(), name, true, true), _holder(holder) {}

    virtual void append(OperationContext* opCtx, BSONObjBuilder& b, const std::string& name) {
        b.append(name, _holder->outof());
    }

    virtual Status set(const BSONElement& newValueElement) {
        if (!newValueElement.isNumber())
            return Status(ErrorCodes::BadValue, str::stream() << name() << " has to be a number");
        return _set(newValueElement.numberInt());
    }

    virtual Status setFromString(const std::string& str) {
        int num = 0;
        Status status = parseNumberFromString(str, &num);
        if (!status.isOK())
            return status;
        return _set(num);
    }

    Status _set(int newNum) {
        if (newNum <= 0) {
            return Status(ErrorCodes::BadValue, str::stream() << name() << " has to be > 0");
        }

        return _holder->resize(newNum);
    }

private:
    TicketHolder* _holder;
};

TicketHolder openWriteTransaction(128);
TicketServerParameter openWriteTransactionParam(&openWriteTransaction,
                                                "wiredTigerConcurrentWriteTransactions");

TicketHolder openReadTransaction(128);
TicketServerParameter openReadTransactionParam(&openReadTransaction,
                                               "wiredTigerConcurrentReadTransactions");

stdx::function<bool(StringData)> initRsOplogBackgroundThreadCallback = [](StringData) -> bool {
    fassertFailed(40358);
};
}  // namespace

StringData WiredTigerKVEngine::kTableUriPrefix = "table:"_sd;

WiredTigerKVEngine::WiredTigerKVEngine(const std::string& canonicalName,
                                       const std::string& path,
                                       ClockSource* cs,
                                       const std::string& extraOpenOptions,
                                       size_t cacheSizeMB,
                                       bool durable,
                                       bool ephemeral,
                                       bool repair,
                                       bool readOnly)
    : _clockSource(cs),
      _oplogManager(stdx::make_unique<WiredTigerOplogManager>()),
      _canonicalName(canonicalName),
      _path(path),
      _sizeStorerSyncTracker(cs, 100000, Seconds(60)),
      _durable(durable),
      _ephemeral(ephemeral),
      _inRepairMode(repair),
      _readOnly(readOnly),
      _keepDataHistory(serverGlobalParams.enableMajorityReadConcern) {
    boost::filesystem::path journalPath = path;
    journalPath /= "journal";
    if (_durable) {
        if (!boost::filesystem::exists(journalPath)) {
            try {
                boost::filesystem::create_directory(journalPath);
            } catch (std::exception& e) {
                log() << "error creating journal dir " << journalPath.string() << ' ' << e.what();
                throw;
            }
        }
    }

    _previousCheckedDropsQueued = _clockSource->now();

    std::stringstream ss;
    ss << "create,";
    ss << "cache_size=" << cacheSizeMB << "M,";
    ss << "session_max=20000,";
    ss << "eviction=(threads_min=4,threads_max=4),";
    ss << "config_base=false,";
    ss << "statistics=(fast),";

    if (!WiredTigerSessionCache::isEngineCachingCursors()) {
        ss << "cache_cursors=false,";
    }

    // The setting may have a later setting override it if not using the journal.  We make it
    // unconditional here because even nojournal may need this setting if it is a transition
    // from using the journal.
    if (!_readOnly) {
        // If we're readOnly skip all WAL-related settings.
        ss << "log=(enabled=true,archive=true,path=journal,compressor=";
        ss << wiredTigerGlobalOptions.journalCompressor << "),";
        ss << "file_manager=(close_idle_time=100000),";  //~28 hours, will put better fix in 3.1.x
        ss << "statistics_log=(wait=" << wiredTigerGlobalOptions.statisticsLogDelaySecs << "),";
        ss << "verbose=(recovery_progress),";

        if (shouldLog(::mongo::logger::LogComponent::kStorageRecovery,
                      logger::LogSeverity::Debug(3))) {
            ss << "verbose=(recovery),";
        }
    }
    ss << WiredTigerCustomizationHooks::get(getGlobalServiceContext())
              ->getTableCreateConfig("system");
    ss << WiredTigerExtensions::get(getGlobalServiceContext())->getOpenExtensionsConfig();
    ss << extraOpenOptions;
    if (_readOnly) {
        invariant(!_durable);
        ss << "readonly=true,";
    }
    if (!_durable && !_readOnly) {
        // If we started without the journal, but previously used the journal then open with the
        // WT log enabled to perform any unclean shutdown recovery and then close and reopen in
        // the normal path without the journal.
        if (boost::filesystem::exists(journalPath)) {
            string config = ss.str();
            log() << "Detected WT journal files.  Running recovery from last checkpoint.";
            log() << "journal to nojournal transition config: " << config;
            int ret = wiredtiger_open(
                path.c_str(), _eventHandler.getWtEventHandler(), config.c_str(), &_conn);
            if (ret == EINVAL) {
                fassertFailedNoTrace(28717);
            } else if (ret != 0) {
                Status s(wtRCToStatus(ret));
                msgasserted(28718, s.reason());
            }
            invariantWTOK(_conn->close(_conn, NULL));
            // After successful recovery, remove the journal directory.
            try {
                boost::filesystem::remove_all(journalPath);
            } catch (std::exception& e) {
                error() << "error removing journal dir " << journalPath.string() << ' ' << e.what();
                throw;
            }
        }
        // This setting overrides the earlier setting because it is later in the config string.
        ss << ",log=(enabled=false),";
    }

    string config = ss.str();
    log() << "wiredtiger_open config: " << config;
    _openWiredTiger(path, config);
    _eventHandler.setStartupSuccessful();
    _wtOpenConfig = config;

    {
        char buf[(2 * 8 /*bytes in hex*/) + 1 /*nul terminator*/];
        invariantWTOK(_conn->query_timestamp(_conn, buf, "get=recovery"));

        std::uint64_t tmp;
        fassert(50758, parseNumberFromStringWithBase(buf, 16, &tmp));
        _recoveryTimestamp = Timestamp(tmp);
        LOG_FOR_RECOVERY(0) << "WiredTiger recoveryTimestamp. Ts: " << _recoveryTimestamp;
    }

    _sessionCache.reset(new WiredTigerSessionCache(this));

    if (_durable && !_ephemeral) {
        _journalFlusher = stdx::make_unique<WiredTigerJournalFlusher>(_sessionCache.get());
        _journalFlusher->go();
    }

    if (!_readOnly && !_ephemeral) {
        if (!_recoveryTimestamp.isNull()) {
            setInitialDataTimestamp(_recoveryTimestamp);
            // The `maximumTruncationTimestamp` is not persisted, so choose a conservative value.
            setStableTimestamp(_recoveryTimestamp, Timestamp::min());
        }

        _checkpointThread =
            stdx::make_unique<WiredTigerCheckpointThread>(this, _sessionCache.get());
        _checkpointThread->go();
    }

    _sizeStorerUri = _uri("sizeStorer");
    WiredTigerSession session(_conn);
    if (!_readOnly && repair && _hasUri(session.getSession(), _sizeStorerUri)) {
        log() << "Repairing size cache";

        auto status = _salvageIfNeeded(_sizeStorerUri.c_str());
        if (status.code() != ErrorCodes::DataModifiedByRepair)
            fassertNoTrace(28577, status);
    }

    _sizeStorer = std::make_unique<WiredTigerSizeStorer>(_conn, _sizeStorerUri, _readOnly);

    Locker::setGlobalThrottling(&openReadTransaction, &openWriteTransaction);
}


WiredTigerKVEngine::~WiredTigerKVEngine() {
    if (_conn) {
        cleanShutdown();
    }

    _sessionCache.reset(NULL);
}

void WiredTigerKVEngine::appendGlobalStats(BSONObjBuilder& b) {
    BSONObjBuilder bb(b.subobjStart("concurrentTransactions"));
    {
        BSONObjBuilder bbb(bb.subobjStart("write"));
        bbb.append("out", openWriteTransaction.used());
        bbb.append("available", openWriteTransaction.available());
        bbb.append("totalTickets", openWriteTransaction.outof());
        bbb.done();
    }
    {
        BSONObjBuilder bbb(bb.subobjStart("read"));
        bbb.append("out", openReadTransaction.used());
        bbb.append("available", openReadTransaction.available());
        bbb.append("totalTickets", openReadTransaction.outof());
        bbb.done();
    }
    bb.done();
}

void WiredTigerKVEngine::_openWiredTiger(const std::string& path, const std::string& wtOpenConfig) {
    std::string configStr = wtOpenConfig + ",compatibility=(require_min=\"3.1.0\")";

    auto wtEventHandler = _eventHandler.getWtEventHandler();

    int ret = wiredtiger_open(path.c_str(), wtEventHandler, configStr.c_str(), &_conn);
    if (!ret) {
        _fileVersion = {WiredTigerFileVersion::StartupVersion::IS_40};
        return;
    }

    // Arbiters do not replicate the FCV document. Due to arbiter FCV semantics on 4.0, shutting
    // down a 4.0 arbiter may either downgrade the data files to WT compatibility 2.9 or 3.0. Thus,
    // 4.2 binaries must allow starting up on 2.9 and 3.0 files.
    configStr = wtOpenConfig + ",compatibility=(require_min=\"3.0.0\")";
    ret = wiredtiger_open(path.c_str(), wtEventHandler, configStr.c_str(), &_conn);
    if (!ret) {
        _fileVersion = {WiredTigerFileVersion::StartupVersion::IS_36};
        return;
    }

    configStr = wtOpenConfig + ",compatibility=(require_min=\"2.9.0\")";
    ret = wiredtiger_open(path.c_str(), wtEventHandler, configStr.c_str(), &_conn);
    if (!ret) {
        _fileVersion = {WiredTigerFileVersion::StartupVersion::IS_34};
        return;
    }

    warning() << "Failed to start up WiredTiger under any compatibility version.";
    if (ret == EINVAL) {
        fassertFailedNoTrace(28561);
    }

    if (ret == WT_TRY_SALVAGE) {
        warning() << "WiredTiger metadata corruption detected";

        if (!_inRepairMode) {
            severe() << kWTRepairMsg;
            fassertFailedNoTrace(50944);
        }
    }

    severe() << "Reason: " << wtRCToStatus(ret).reason();
    if (!_inRepairMode) {
        fassertFailedNoTrace(28595);
    }

    // Always attempt to salvage metadata regardless of error code when in repair mode.

    warning() << "Attempting to salvage WiredTiger metadata";
    configStr = wtOpenConfig + ",salvage=true";
    ret = wiredtiger_open(path.c_str(), wtEventHandler, configStr.c_str(), &_conn);
    if (!ret) {
        StorageRepairObserver::get(getGlobalServiceContext())
            ->onModification("WiredTiger metadata salvaged");
        return;
    }

    severe() << "Failed to salvage WiredTiger metadata: " + wtRCToStatus(ret).reason();
    fassertFailedNoTrace(50947);
}

void WiredTigerKVEngine::cleanShutdown() {
    log() << "WiredTigerKVEngine shutting down";
    if (!_readOnly)
        syncSizeInfo(true);
    if (!_conn) {
        return;
    }

    // these must be the last things we do before _conn->close();
    if (_journalFlusher) {
        log() << "Shutting down journal flusher thread";
        _journalFlusher->shutdown();
        log() << "Finished shutting down journal flusher thread";
    }
    if (_checkpointThread) {
        log() << "Shutting down checkpoint thread";
        _checkpointThread->shutdown();
        log() << "Finished shutting down checkpoint thread";
    }
    LOG_FOR_RECOVERY(2) << "Shutdown timestamps. StableTimestamp: " << _stableTimestamp.load()
                        << " Initial data timestamp: " << _initialDataTimestamp.load();

    _sizeStorer.reset();
    _sessionCache->shuttingDown();

// We want WiredTiger to leak memory for faster shutdown except when we are running tools to look
// for memory leaks.
#if !__has_feature(address_sanitizer)
    bool leak_memory = true;
#else
    bool leak_memory = false;
#endif
    std::string closeConfig = "";

    if (RUNNING_ON_VALGRIND) {
        leak_memory = false;
    }

    if (leak_memory) {
        closeConfig = "leak_memory=true,";
    }

    if (_fileVersion.shouldDowngrade(_readOnly, _inRepairMode, !_recoveryTimestamp.isNull())) {
        log() << "Downgrading WiredTiger datafiles.";
        LOG(1) << "Downgrade compatibility configuration: " << _fileVersion.getDowngradeString();
        invariantWTOK(_conn->reconfigure(_conn, _fileVersion.getDowngradeString().c_str()));
    }

    if (!serverGlobalParams.enableMajorityReadConcern) {
        closeConfig += "use_timestamp=false,";
    }

    invariantWTOK(_conn->close(_conn, closeConfig.c_str()));
    _conn = nullptr;
}

Status WiredTigerKVEngine::okToRename(OperationContext* opCtx,
                                      StringData fromNS,
                                      StringData toNS,
                                      StringData ident,
                                      const RecordStore* originalRecordStore) const {
    syncSizeInfo(false);

    return Status::OK();
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

    int rc = (session->verify)(session, uri, NULL);
    if (rc == 0) {
        log() << "Verify succeeded on uri " << uri << ". Not salvaging.";
        return Status::OK();
    }

    if (rc == EBUSY) {
        // SERVER-16457: verify and salvage are occasionally failing with EBUSY. For now we
        // lie and return OK to avoid breaking tests. This block should go away when that ticket
        // is resolved.
        error()
            << "Verify on " << uri << " failed with EBUSY. "
            << "This means the collection was being accessed. No repair is necessary unless other "
               "errors are reported.";
        return Status::OK();
    }

    if (rc == ENOENT) {
        warning() << "Data file is missing for " << uri
                  << ". Attempting to drop and re-create the collection.";

        return _rebuildIdent(session, uri);
    }

    log() << "Verify failed on uri " << uri << ". Running a salvage operation.";
    auto status = wtRCToStatus(session->salvage(session, uri, NULL), "Salvage failed:");
    if (status.isOK()) {
        return {ErrorCodes::DataModifiedByRepair, str::stream() << "Salvaged data for " << uri};
    }

    warning() << "Salvage failed for uri " << uri << ": " << status.reason()
              << ". The file will be moved out of the way and a new ident will be created.";

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
        warning() << "Moving data file " << filePath->string() << " to backup as "
                  << corruptFile.string();

        auto status = fsyncRename(filePath.get(), corruptFile);
        if (!status.isOK()) {
            return status;
        }
    }

    warning() << "Rebuilding ident " << identName;

    // This is safe to call after moving the file because it only reads from the metadata, and not
    // the data file itself.
    auto swMetadata = WiredTigerUtil::getMetadataRaw(session, uri);
    if (!swMetadata.isOK()) {
        error() << "Failed to get metadata for " << uri;
        return swMetadata.getStatus();
    }

    int rc = session->drop(session, uri, NULL);
    if (rc != 0) {
        error() << "Failed to drop " << uri;
        return wtRCToStatus(rc);
    }

    rc = session->create(session, uri, swMetadata.getValue().c_str());
    if (rc != 0) {
        error() << "Failed to create " << uri << " with config: " << swMetadata.getValue();
        return wtRCToStatus(rc);
    }
    log() << "Successfully re-created " << uri << ".";
    return {ErrorCodes::DataModifiedByRepair,
            str::stream() << "Re-created empty data file for " << uri};
}

int WiredTigerKVEngine::flushAllFiles(OperationContext* opCtx, bool sync) {
    LOG(1) << "WiredTigerKVEngine::flushAllFiles";
    if (_ephemeral) {
        return 0;
    }
    syncSizeInfo(false);
    const bool forceCheckpoint = true;
    // If there's no journal, we must take a full checkpoint.
    const bool stableCheckpoint = _durable;
    _sessionCache->waitUntilDurable(forceCheckpoint, stableCheckpoint);

    return 1;
}

Status WiredTigerKVEngine::beginBackup(OperationContext* opCtx) {
    invariant(!_backupSession);

    // The inMemory Storage Engine cannot create a backup cursor.
    if (_ephemeral) {
        return Status::OK();
    }

    // This cursor will be freed by the backupSession being closed as the session is uncached
    auto session = stdx::make_unique<WiredTigerSession>(_conn);
    WT_CURSOR* c = NULL;
    WT_SESSION* s = session->getSession();
    int ret = WT_OP_CHECK(s->open_cursor(s, "backup:", NULL, NULL, &c));
    if (ret != 0) {
        return wtRCToStatus(ret);
    }
    _backupSession = std::move(session);
    return Status::OK();
}

void WiredTigerKVEngine::endBackup(OperationContext* opCtx) {
    _backupSession.reset();
}

StatusWith<std::vector<std::string>> WiredTigerKVEngine::beginNonBlockingBackup(
    OperationContext* opCtx) {
    // This cursor will be freed by the backupSession being closed as the session is uncached
    auto sessionRaii = stdx::make_unique<WiredTigerSession>(_conn);
    WT_CURSOR* cursor = NULL;
    WT_SESSION* session = sessionRaii->getSession();
    int wtRet = WT_OP_CHECK(session->open_cursor(session, "backup:", NULL, NULL, &cursor));
    if (wtRet != 0) {
        return wtRCToStatus(wtRet);
    }

    std::vector<std::string> filesToCopy;

    const char* filename;
    const auto dbPath = boost::filesystem::path(_path);
    const auto wiredTigerLogFilePrefix = "WiredTigerLog";
    while ((wtRet = cursor->next(cursor)) == 0) {
        invariantWTOK(cursor->get_key(cursor, &filename));

        std::string name(filename);

        auto filePath = dbPath;
        if (name.find(wiredTigerLogFilePrefix) == 0) {
            // TODO SERVER-13455:replace `journal/` with the configurable journal path.
            filePath /= boost::filesystem::path("journal");
        }
        filePath /= name;

        filesToCopy.push_back(filePath.string());
    }
    if (wtRet != WT_NOTFOUND) {
        return wtRCToStatus(wtRet, "Error opening backup cursor.");
    }

    _backupSession = std::move(sessionRaii);
    return filesToCopy;
}

void WiredTigerKVEngine::endNonBlockingBackup(OperationContext* opCtx) {
    _backupSession.reset();
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

RecoveryUnit* WiredTigerKVEngine::newRecoveryUnit() {
    return new WiredTigerRecoveryUnit(_sessionCache.get());
}

void WiredTigerKVEngine::setRecordStoreExtraOptions(const std::string& options) {
    _rsOptions = options;
}

void WiredTigerKVEngine::setSortedDataInterfaceExtraOptions(const std::string& options) {
    _indexOptions = options;
}

Status WiredTigerKVEngine::createGroupedRecordStore(OperationContext* opCtx,
                                                    StringData ns,
                                                    StringData ident,
                                                    const CollectionOptions& options,
                                                    KVPrefix prefix) {
    _ensureIdentPath(ident);
    WiredTigerSession session(_conn);

    const bool prefixed = prefix.isPrefixed();
    StatusWith<std::string> result = WiredTigerRecordStore::generateCreateString(
        _canonicalName, ns, options, _rsOptions, prefixed);
    if (!result.isOK()) {
        return result.getStatus();
    }
    std::string config = result.getValue();

    string uri = _uri(ident);
    WT_SESSION* s = session.getSession();
    LOG(2) << "WiredTigerKVEngine::createRecordStore ns: " << ns << " uri: " << uri
           << " config: " << config;
    return wtRCToStatus(s->create(s, uri.c_str(), config.c_str()));
}

Status WiredTigerKVEngine::recoverOrphanedIdent(OperationContext* opCtx,
                                                StringData ns,
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

    log() << "Renaming data file " + identFilePath->string() + " to temporary file " +
            tmpFile.string();
    auto status = fsyncRename(identFilePath.get(), tmpFile);
    if (!status.isOK()) {
        return status;
    }

    log() << "Creating new RecordStore for collection " + ns + " with UUID: " +
            (options.uuid ? options.uuid->toString() : "none");

    status = createGroupedRecordStore(opCtx, ns, ident, options, KVPrefix::kNotPrefixed);
    if (!status.isOK()) {
        return status;
    }

    log() << "Moving orphaned data file back as " + identFilePath->string();

    boost::filesystem::remove(*identFilePath, ec);
    if (ec) {
        return {ErrorCodes::UnknownError, "Error deleting empty data file: " + ec.message()};
    }
    status = fsyncParentDirectory(*identFilePath);
    if (!status.isOK()) {
        return status;
    }

    status = fsyncRename(tmpFile, identFilePath.get());
    if (!status.isOK()) {
        return status;
    }

    log() << "Salvaging ident " + ident;

    WiredTigerSession sessionWrapper(_conn);
    WT_SESSION* session = sessionWrapper.getSession();
    status = wtRCToStatus(session->salvage(session, _uri(ident).c_str(), NULL), "Salvage failed: ");
    if (status.isOK()) {
        return {ErrorCodes::DataModifiedByRepair,
                str::stream() << "Salvaged data for ident " << ident};
    }
    warning() << "Could not salvage data. Rebuilding ident: " << status.reason();

    //  If the data is unsalvageable, we should completely rebuild the ident.
    return _rebuildIdent(session, _uri(ident).c_str());
#endif
}

std::unique_ptr<RecordStore> WiredTigerKVEngine::getGroupedRecordStore(
    OperationContext* opCtx,
    StringData ns,
    StringData ident,
    const CollectionOptions& options,
    KVPrefix prefix) {

    WiredTigerRecordStore::Params params;
    params.ns = ns;
    params.ident = ident.toString();
    params.engineName = _canonicalName;
    params.isCapped = options.capped;
    params.isEphemeral = _ephemeral;
    params.cappedCallback = nullptr;
    params.sizeStorer = _sizeStorer.get();
    params.isReadOnly = _readOnly;

    params.cappedMaxSize = -1;
    if (options.capped) {
        if (options.cappedSize) {
            params.cappedMaxSize = options.cappedSize;
        } else {
            params.cappedMaxSize = kDefaultCappedSizeBytes;
        }
    }
    params.cappedMaxDocs = -1;
    if (options.capped && options.cappedMaxDocs)
        params.cappedMaxDocs = options.cappedMaxDocs;

    std::unique_ptr<WiredTigerRecordStore> ret;
    if (prefix == KVPrefix::kNotPrefixed) {
        ret = stdx::make_unique<StandardWiredTigerRecordStore>(this, opCtx, params);
    } else {
        ret = stdx::make_unique<PrefixedWiredTigerRecordStore>(this, opCtx, params, prefix);
    }
    ret->postConstructorInit(opCtx);

    return std::move(ret);
}

string WiredTigerKVEngine::_uri(StringData ident) const {
    invariant(ident.find(kTableUriPrefix) == string::npos);
    return kTableUriPrefix + ident.toString();
}

Status WiredTigerKVEngine::createGroupedSortedDataInterface(OperationContext* opCtx,
                                                            StringData ident,
                                                            const IndexDescriptor* desc,
                                                            KVPrefix prefix) {
    _ensureIdentPath(ident);

    std::string collIndexOptions;
    const Collection* collection = desc->getCollection();

    // Treat 'collIndexOptions' as an empty string when the collection member of 'desc' is NULL in
    // order to allow for unit testing WiredTigerKVEngine::createSortedDataInterface().
    if (collection) {
        const CollectionCatalogEntry* cce = collection->getCatalogEntry();
        const CollectionOptions collOptions = cce->getCollectionOptions(opCtx);

        if (!collOptions.indexOptionDefaults["storageEngine"].eoo()) {
            BSONObj storageEngineOptions = collOptions.indexOptionDefaults["storageEngine"].Obj();
            collIndexOptions =
                dps::extractElementAtPath(storageEngineOptions, _canonicalName + ".configString")
                    .valuestrsafe();
        }
    }

    StatusWith<std::string> result = WiredTigerIndex::generateCreateString(
        _canonicalName, _indexOptions, collIndexOptions, *desc, prefix.isPrefixed());
    if (!result.isOK()) {
        return result.getStatus();
    }

    std::string config = result.getValue();

    LOG(2) << "WiredTigerKVEngine::createSortedDataInterface ns: " << collection->ns()
           << " ident: " << ident << " config: " << config;
    return wtRCToStatus(WiredTigerIndex::Create(opCtx, _uri(ident), config));
}

SortedDataInterface* WiredTigerKVEngine::getGroupedSortedDataInterface(OperationContext* opCtx,
                                                                       StringData ident,
                                                                       const IndexDescriptor* desc,
                                                                       KVPrefix prefix) {
    if (desc->unique()) {
        return new WiredTigerIndexUnique(opCtx, _uri(ident), desc, prefix, _readOnly);
    }

    return new WiredTigerIndexStandard(opCtx, _uri(ident), desc, prefix, _readOnly);
}

std::unique_ptr<RecordStore> WiredTigerKVEngine::makeTemporaryRecordStore(OperationContext* opCtx,
                                                                          StringData ident) {
    invariant(!_readOnly);

    _ensureIdentPath(ident);
    WiredTigerSession wtSession(_conn);

    CollectionOptions noOptions;
    StatusWith<std::string> swConfig = WiredTigerRecordStore::generateCreateString(
        _canonicalName, "" /* internal table */, noOptions, _rsOptions, false /* prefixed */);
    uassertStatusOK(swConfig.getStatus());

    std::string config = swConfig.getValue();

    std::string uri = _uri(ident);
    WT_SESSION* session = wtSession.getSession();
    LOG(2) << "WiredTigerKVEngine::createTemporaryRecordStore uri: " << uri
           << " config: " << config;
    uassertStatusOK(wtRCToStatus(session->create(session, uri.c_str(), config.c_str())));

    WiredTigerRecordStore::Params params;
    params.ns = "";
    params.ident = ident.toString();
    params.engineName = _canonicalName;
    params.isCapped = false;
    params.isEphemeral = _ephemeral;
    params.cappedCallback = nullptr;
    // Temporary collections do not need to persist size information to the size storer.
    params.sizeStorer = nullptr;
    params.isReadOnly = false;

    params.cappedMaxSize = -1;
    params.cappedMaxDocs = -1;

    std::unique_ptr<WiredTigerRecordStore> rs;
    rs = stdx::make_unique<StandardWiredTigerRecordStore>(this, opCtx, params);
    rs->postConstructorInit(opCtx);

    return std::move(rs);
}

void WiredTigerKVEngine::alterIdentMetadata(OperationContext* opCtx,
                                            StringData ident,
                                            const IndexDescriptor* desc) {
    WiredTigerSession session(_conn);
    std::string uri = _uri(ident);

    // Make the alter call to update metadata without taking exclusive lock to avoid conflicts with
    // concurrent operations.
    std::string alterString =
        WiredTigerIndex::generateAppMetadataString(*desc) + "exclusive_refreshed=false,";
    invariantWTOK(
        session.getSession()->alter(session.getSession(), uri.c_str(), alterString.c_str()));
}

Status WiredTigerKVEngine::dropIdent(OperationContext* opCtx, StringData ident) {
    string uri = _uri(ident);

    WiredTigerRecoveryUnit* ru = WiredTigerRecoveryUnit::get(opCtx);
    ru->getSessionNoTxn()->closeAllCursors(uri);
    _sessionCache->closeAllCursors(uri);

    WiredTigerSession session(_conn);

    int ret = session.getSession()->drop(
        session.getSession(), uri.c_str(), "force,checkpoint_wait=false");
    LOG(1) << "WT drop of " << uri << " res " << ret;

    if (ret == 0) {
        // yay, it worked
        return Status::OK();
    }

    if (ret == EBUSY) {
        // this is expected, queue it up
        {
            stdx::lock_guard<stdx::mutex> lk(_identToDropMutex);
            _identToDrop.push_front(uri);
        }
        _sessionCache->closeCursorsForQueuedDrops();
        return Status::OK();
    }

    if (ret == ENOENT) {
        return Status::OK();
    }

    invariantWTOK(ret);
    return Status::OK();
}

std::list<WiredTigerCachedCursor> WiredTigerKVEngine::filterCursorsWithQueuedDrops(
    std::list<WiredTigerCachedCursor>* cache) {
    std::list<WiredTigerCachedCursor> toDrop;

    stdx::lock_guard<stdx::mutex> lk(_identToDropMutex);
    if (_identToDrop.empty())
        return toDrop;

    for (auto i = cache->begin(); i != cache->end();) {
        if (!i->_cursor ||
            std::find(_identToDrop.begin(), _identToDrop.end(), std::string(i->_cursor->uri)) ==
                _identToDrop.end()) {
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
    Milliseconds delta = now - _previousCheckedDropsQueued;

    if (!_readOnly && _sizeStorerSyncTracker.intervalHasElapsed()) {
        _sizeStorerSyncTracker.resetLastTime();
        syncSizeInfo(false);
    }

    // We only want to check the queue max once per second or we'll thrash
    if (delta < Milliseconds(1000))
        return false;

    _previousCheckedDropsQueued = now;

    // Don't wait for the mutex: if we can't get it, report that no drops are queued.
    stdx::unique_lock<stdx::mutex> lk(_identToDropMutex, stdx::defer_lock);
    return lk.try_lock() && !_identToDrop.empty();
}

void WiredTigerKVEngine::dropSomeQueuedIdents() {
    int numInQueue;

    WiredTigerSession session(_conn);

    {
        stdx::lock_guard<stdx::mutex> lk(_identToDropMutex);
        numInQueue = _identToDrop.size();
    }

    int numToDelete = 10;
    int tenPercentQueue = numInQueue * 0.1;
    if (tenPercentQueue > 10)
        numToDelete = tenPercentQueue;

    LOG(1) << "WT Queue is: " << numInQueue << " attempting to drop: " << numToDelete << " tables";
    for (int i = 0; i < numToDelete; i++) {
        string uri;
        {
            stdx::lock_guard<stdx::mutex> lk(_identToDropMutex);
            if (_identToDrop.empty())
                break;
            uri = _identToDrop.front();
            _identToDrop.pop_front();
        }
        int ret = session.getSession()->drop(
            session.getSession(), uri.c_str(), "force,checkpoint_wait=false");
        LOG(1) << "WT queued drop of  " << uri << " res " << ret;

        if (ret == EBUSY) {
            stdx::lock_guard<stdx::mutex> lk(_identToDropMutex);
            _identToDrop.push_back(uri);
        } else {
            invariantWTOK(ret);
        }
    }
}

bool WiredTigerKVEngine::supportsDocLocking() const {
    return true;
}

bool WiredTigerKVEngine::supportsDirectoryPerDB() const {
    return true;
}

bool WiredTigerKVEngine::hasIdent(OperationContext* opCtx, StringData ident) const {
    return _hasUri(WiredTigerRecoveryUnit::get(opCtx)->getSession()->getSession(), _uri(ident));
}

bool WiredTigerKVEngine::_hasUri(WT_SESSION* session, const std::string& uri) const {
    // can't use WiredTigerCursor since this is called from constructor.
    WT_CURSOR* c = NULL;
    int ret = session->open_cursor(session, "metadata:create", NULL, NULL, &c);
    if (ret == ENOENT)
        return false;
    invariantWTOK(ret);
    ON_BLOCK_EXIT(c->close, c);

    c->set_key(c, uri.c_str());
    return c->search(c) == 0;
}

std::vector<std::string> WiredTigerKVEngine::getAllIdents(OperationContext* opCtx) const {
    std::vector<std::string> all;
    int ret;
    WiredTigerCursor cursor("metadata:create", WiredTigerSession::kMetadataTableId, false, opCtx);
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
            LOG(1) << "creating subdirectory: " << dir;
            try {
                boost::filesystem::create_directory(subdir);
            } catch (const std::exception& e) {
                error() << "error creating path " << subdir.string() << ' ' << e.what();
                throw;
            }
        }

        start = idx + 1;
    }
}

void WiredTigerKVEngine::setJournalListener(JournalListener* jl) {
    return _sessionCache->setJournalListener(jl);
}

void WiredTigerKVEngine::setInitRsOplogBackgroundThreadCallback(
    stdx::function<bool(StringData)> cb) {
    initRsOplogBackgroundThreadCallback = std::move(cb);
}

bool WiredTigerKVEngine::initRsOplogBackgroundThread(StringData ns) {
    return initRsOplogBackgroundThreadCallback(ns);
}

namespace {

MONGO_FAIL_POINT_DEFINE(WTPreserveSnapshotHistoryIndefinitely);

}  // namespace

void WiredTigerKVEngine::setStableTimestamp(Timestamp stableTimestamp,
                                            boost::optional<Timestamp> maximumTruncationTimestamp) {
    if (!_keepDataHistory) {
        return;
    }

    if (stableTimestamp.isNull()) {
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
    char stableTSConfigString["stable_timestamp="_sd.size() + (8 * 2) /* 16 hexadecimal digits */ +
                              1 /* trailing null */];
    auto size = std::snprintf(stableTSConfigString,
                              sizeof(stableTSConfigString),
                              "stable_timestamp=%llx",
                              stableTimestamp.asULL());
    if (size < 0) {
        int e = errno;
        error() << "error snprintf " << errnoWithDescription(e);
        fassertFailedNoTrace(50757);
    }
    invariant(static_cast<std::size_t>(size) < sizeof(stableTSConfigString));
    invariantWTOK(_conn->set_timestamp(_conn, stableTSConfigString));

    Timestamp prevStable(_stableTimestamp.load());
    // set_timestamp above ignores backwards in time values unless 'force' is set.
    if (prevStable < stableTimestamp) {
        _stableTimestamp.store(stableTimestamp.asULL());
    }

    // After publishing a stable timestamp to WT, we can publish the updated value for the
    // necessary oplog to keep. Calls to this method require the min(stableTimestamp,
    // maximumTruncationTimestamp) to be monotonically increasing. This allows us to safely record
    // and publish a single value without additional concurrency control.
    if (maximumTruncationTimestamp) {
        // Until we discover otherwise, assume callers expect to obey the contract for proper
        // oplog truncation.
        DEV invariant(_oplogNeededForRollback.load() <=
                      std::min(maximumTruncationTimestamp->asULL(), stableTimestamp.asULL()));
        _oplogNeededForRollback.store(
            std::min(maximumTruncationTimestamp->asULL(), stableTimestamp.asULL()));
    } else {
        // If there is no maximumTruncationTimestamp at this stable timestamp, WT is free to
        // truncate the oplog to any value behind the last stable timestamp, once it is
        // checkpointed.
        _oplogNeededForRollback.store(stableTimestamp.asULL());
    }

    if (_checkpointThread && !_checkpointThread->hasTriggeredFirstStableCheckpoint()) {
        _checkpointThread->triggerFirstStableCheckpoint(
            prevStable, Timestamp(_initialDataTimestamp.load()), stableTimestamp);
    }

    // Forward the oldest timestamp so that WiredTiger can clean up earlier timestamp data.
    setOldestTimestampFromStable();
}

void WiredTigerKVEngine::setOldestTimestampFromStable() {
    Timestamp stableTimestamp(_stableTimestamp.load());

    // Calculate what the oldest_timestamp should be from the stable_timestamp. The oldest
    // timestamp should lag behind stable by 'targetSnapshotHistoryWindowInSeconds' to create a
    // window of available snapshots. If the lag window is not yet large enough, we will not
    // update/forward the oldest_timestamp yet and instead return early.
    Timestamp newOldestTimestamp = _calculateHistoryLagFromStableTimestamp(stableTimestamp);
    if (newOldestTimestamp.isNull()) {
        return;
    }

    const auto oplogReadTimestamp = Timestamp(_oplogManager->getOplogReadTimestamp());
    if (!oplogReadTimestamp.isNull() && newOldestTimestamp > oplogReadTimestamp) {
        // Oplog visibility is updated asynchronously from replication updating the commit point.
        // When force is not set, lag the `oldest_timestamp` to the possibly stale oplog read
        // timestamp value. This guarantees an oplog reader's `read_timestamp` can always
        // be serviced. When force is set, we respect the caller's request and do not lag the
        // oldest timestamp.
        newOldestTimestamp = oplogReadTimestamp;
    }

    setOldestTimestamp(newOldestTimestamp, false);
}

void WiredTigerKVEngine::setOldestTimestamp(Timestamp newOldestTimestamp, bool force) {
    if (MONGO_FAIL_POINT(WTPreserveSnapshotHistoryIndefinitely)) {
        return;
    }
    const auto localSnapshotTimestamp = _sessionCache->snapshotManager().getLocalSnapshot();
    if (!force && localSnapshotTimestamp && newOldestTimestamp > *localSnapshotTimestamp) {
        // When force is not set, lag the `oldest timestamp` to the local snapshot timestamp.
        // Secondary reads are performed at the local snapshot timestamp, so advancing the oldest
        // timestamp beyond the local snapshot timestamp could cause secondary reads to fail. This
        // is not a problem when majority read concern is enabled, since the replication system will
        // not set the stable timestamp ahead of the local snapshot timestamp. However, when
        // majority read concern is disabled and the oldest timestamp is set by the oplog manager,
        // the oplog manager can set the oldest timestamp ahead of the local snapshot timestamp.
        newOldestTimestamp = *localSnapshotTimestamp;
    }

    char oldestTSConfigString["force=true,oldest_timestamp=,commit_timestamp="_sd.size() +
                              (2 * 8 * 2) /* 2 timestamps of 16 hexadecimal digits each */ +
                              1 /* trailing null */];
    int size = 0;
    if (force) {
        size = std::snprintf(oldestTSConfigString,
                             sizeof(oldestTSConfigString),
                             "force=true,oldest_timestamp=%llx,commit_timestamp=%llx",
                             newOldestTimestamp.asULL(),
                             newOldestTimestamp.asULL());
    } else {
        size = std::snprintf(oldestTSConfigString,
                             sizeof(oldestTSConfigString),
                             "oldest_timestamp=%llx",
                             newOldestTimestamp.asULL());
    }
    if (size < 0) {
        int e = errno;
        error() << "error snprintf " << errnoWithDescription(e);
        fassertFailedNoTrace(40661);
    }
    invariant(static_cast<std::size_t>(size) < sizeof(oldestTSConfigString));
    invariantWTOK(_conn->set_timestamp(_conn, oldestTSConfigString));

    // set_timestamp above ignores backwards in time unless 'force' is set.
    if (force) {
        _oldestTimestamp.store(newOldestTimestamp.asULL());
    } else if (_oldestTimestamp.load() < newOldestTimestamp.asULL()) {
        _oldestTimestamp.store(newOldestTimestamp.asULL());
    }

    if (force) {
        LOG(2) << "oldest_timestamp and commit_timestamp force set to " << newOldestTimestamp;
    } else {
        LOG(2) << "oldest_timestamp set to " << newOldestTimestamp;
    }
}

Timestamp WiredTigerKVEngine::_calculateHistoryLagFromStableTimestamp(Timestamp stableTimestamp) {
    // The oldest_timestamp should lag behind the stable_timestamp by
    // 'targetSnapshotHistoryWindowInSeconds' seconds.

    if (stableTimestamp.getSecs() <
        static_cast<unsigned>(snapshotWindowParams.targetSnapshotHistoryWindowInSeconds.load())) {
        // The history window is larger than the timestamp history thus far. We must wait for
        // the history to reach the window size before moving oldest_timestamp forward.
        return Timestamp();
    }

    Timestamp calculatedOldestTimestamp(
        stableTimestamp.getSecs() -
            snapshotWindowParams.targetSnapshotHistoryWindowInSeconds.load(),
        stableTimestamp.getInc());

    if (calculatedOldestTimestamp.asULL() <= _oldestTimestamp.load()) {
        // The stable_timestamp is not far enough ahead of the oldest_timestamp for the
        // oldest_timestamp to be moved forward: the window is still too small.
        return Timestamp();
    }

    return calculatedOldestTimestamp;
}

void WiredTigerKVEngine::setInitialDataTimestamp(Timestamp initialDataTimestamp) {
    LOG(2) << "Setting initial data timestamp. Value: " << initialDataTimestamp;
    _initialDataTimestamp.store(initialDataTimestamp.asULL());
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
        severe() << "WiredTiger is configured to not support recover to a stable timestamp";
        fassertFailed(50665);
    }

    if (!_canRecoverToStableTimestamp()) {
        Timestamp stableTS(_stableTimestamp.load());
        Timestamp initialDataTS(_initialDataTimestamp.load());
        return Status(ErrorCodes::UnrecoverableRollbackError,
                      str::stream()
                          << "No stable timestamp available to recover to. Initial data timestamp: "
                          << initialDataTS.toString()
                          << ", Stable timestamp: "
                          << stableTS.toString());
    }

    LOG_FOR_ROLLBACK(2) << "WiredTiger::RecoverToStableTimestamp syncing size storer to disk.";
    syncSizeInfo(true);

    if (!_ephemeral) {
        LOG_FOR_ROLLBACK(2)
            << "WiredTiger::RecoverToStableTimestamp shutting down journal and checkpoint threads.";
        // Shutdown WiredTigerKVEngine owned accesses into the storage engine.
        if (_durable) {
            _journalFlusher->shutdown();
        }
        _checkpointThread->shutdown();
    }

    const Timestamp stableTimestamp(_stableTimestamp.load());
    const Timestamp initialDataTimestamp(_initialDataTimestamp.load());

    LOG_FOR_ROLLBACK(0) << "Rolling back to the stable timestamp. StableTimestamp: "
                        << stableTimestamp << " Initial Data Timestamp: " << initialDataTimestamp;
    int ret = _conn->rollback_to_stable(_conn, nullptr);
    if (ret) {
        return {ErrorCodes::UnrecoverableRollbackError,
                str::stream() << "Error rolling back to stable. Err: " << wiredtiger_strerror(ret)};
    }

    if (!_ephemeral) {
        if (_durable) {
            _journalFlusher = std::make_unique<WiredTigerJournalFlusher>(_sessionCache.get());
            _journalFlusher->go();
        }
        _checkpointThread = std::make_unique<WiredTigerCheckpointThread>(this, _sessionCache.get());
        _checkpointThread->go();
    }

    _sizeStorer = std::make_unique<WiredTigerSizeStorer>(_conn, _sizeStorerUri, _readOnly);

    return {stableTimestamp};
}

Timestamp WiredTigerKVEngine::getAllCommittedTimestamp() const {
    return Timestamp(_oplogManager->fetchAllCommittedValue(_conn));
}

boost::optional<Timestamp> WiredTigerKVEngine::getRecoveryTimestamp() const {
    if (!supportsRecoveryTimestamp()) {
        severe() << "WiredTiger is configured to not support providing a recovery timestamp";
        fassertFailed(50745);
    }

    if (_recoveryTimestamp.isNull()) {
        return boost::none;
    }

    return _recoveryTimestamp;
}

boost::optional<Timestamp> WiredTigerKVEngine::getLastStableRecoveryTimestamp() const {
    if (!supportsRecoverToStableTimestamp()) {
        severe() << "WiredTiger is configured to not support recover to a stable timestamp";
        fassertFailed(50770);
    }

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

Timestamp WiredTigerKVEngine::getOplogNeededForRollback() const {
    return Timestamp(_oplogNeededForRollback.load());
}

boost::optional<Timestamp> WiredTigerKVEngine::getOplogNeededForCrashRecovery() const {
    if (_ephemeral) {
        return boost::none;
    }

    return Timestamp(_checkpointThread->getOplogNeededForCrashRecovery());
}

Timestamp WiredTigerKVEngine::getPinnedOplog() const {
    if (!_keepDataHistory) {
        // We use rollbackViaRefetch and take full checkpoints, so there is no need to pin oplog.
        return Timestamp::max();
    }
    return getOplogNeededForCrashRecovery().value_or(getOplogNeededForRollback());
}

bool WiredTigerKVEngine::supportsReadConcernSnapshot() const {
    return true;
}

bool WiredTigerKVEngine::supportsReadConcernMajority() const {
    return _keepDataHistory;
}

void WiredTigerKVEngine::startOplogManager(OperationContext* opCtx,
                                           const std::string& uri,
                                           WiredTigerRecordStore* oplogRecordStore) {
    stdx::lock_guard<stdx::mutex> lock(_oplogManagerMutex);
    if (_oplogManagerCount == 0)
        _oplogManager->start(opCtx, uri, oplogRecordStore, !_keepDataHistory);
    _oplogManagerCount++;
}

void WiredTigerKVEngine::haltOplogManager() {
    stdx::unique_lock<stdx::mutex> lock(_oplogManagerMutex);
    invariant(_oplogManagerCount > 0);
    _oplogManagerCount--;
    if (_oplogManagerCount == 0) {
        _oplogManager->halt();
    }
}

void WiredTigerKVEngine::replicationBatchIsComplete() const {
    _oplogManager->triggerJournalFlush();
}

bool WiredTigerKVEngine::isCacheUnderPressure(OperationContext* opCtx) const {
    WiredTigerSession* session = WiredTigerRecoveryUnit::get(opCtx)->getSessionNoTxn();
    invariant(session);

    int64_t score = uassertStatusOK(WiredTigerUtil::getStatisticsValueAs<int64_t>(
        session->getSession(), "statistics:", "", WT_STAT_CONN_CACHE_LOOKASIDE_SCORE));

    return (score >= snapshotWindowParams.cachePressureThreshold.load());
}

Timestamp WiredTigerKVEngine::getStableTimestamp() const {
    return Timestamp(_stableTimestamp.load());
}

Timestamp WiredTigerKVEngine::getOldestTimestamp() const {
    return Timestamp(_oldestTimestamp.load());
}

Timestamp WiredTigerKVEngine::getInitialDataTimestamp() const {
    return Timestamp(_initialDataTimestamp.load());
}

std::uint64_t WiredTigerKVEngine::_getCheckpointTimestamp() const {
    char buf[(2 * 8 /*bytes in hex*/) + 1 /*nul terminator*/];
    invariantWTOK(_conn->query_timestamp(_conn, buf, "get=last_checkpoint"));

    std::uint64_t tmp;
    fassert(50963, parseNumberFromStringWithBase(buf, 16, &tmp));
    return tmp;
}

}  // namespace mongo
