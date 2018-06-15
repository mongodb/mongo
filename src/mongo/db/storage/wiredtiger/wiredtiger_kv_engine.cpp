// wiredtiger_kv_engine.cpp

/**
 *    Copyright (C) 2014 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
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
#include "mongo/db/storage/storage_options.h"
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
        ServerGlobalParams::FeatureCompatibility::Version::kFullyDowngradedTo36) {
        // Only consider downgrading when FCV is set to 3.6
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

namespace {
void openWiredTiger(const std::string& path,
                    WT_EVENT_HANDLER* eventHandler,
                    const std::string& wtOpenConfig,
                    WT_CONNECTION** connOut,
                    WiredTigerFileVersion* fileVersionOut) {
    std::string configStr = wtOpenConfig + ",compatibility=(require_min=\"3.1.0\")";
    int ret = wiredtiger_open(path.c_str(), eventHandler, configStr.c_str(), connOut);
    if (!ret) {
        *fileVersionOut = {WiredTigerFileVersion::StartupVersion::IS_40};
        return;
    }

    // Arbiters do not replicate the FCV document. Due to arbiter FCV semantics on 4.0, shutting
    // down a 4.0 arbiter may either downgrade the data files to WT compatibility 2.9 or 3.0. Thus,
    // 4.2 binaries must allow starting up on 2.9 and 3.0 files.
    configStr = wtOpenConfig + ",compatibility=(require_min=\"3.0.0\")";
    ret = wiredtiger_open(path.c_str(), eventHandler, configStr.c_str(), connOut);
    if (!ret) {
        *fileVersionOut = {WiredTigerFileVersion::StartupVersion::IS_36};
        return;
    }

    configStr = wtOpenConfig + ",compatibility=(require_min=\"2.9.0\")";
    ret = wiredtiger_open(path.c_str(), eventHandler, configStr.c_str(), connOut);
    if (!ret) {
        *fileVersionOut = {WiredTigerFileVersion::StartupVersion::IS_34};
        return;
    }

    severe() << "Failed to start up WiredTiger under any compatibility version.";
    if (ret == EINVAL) {
        fassertFailedNoTrace(28561);
    }

    severe() << wtRCToStatus(ret).reason();
    fassertFailedNoTrace(28595);
}
}  // namespace

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
        Client::initThread(name().c_str());

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
    explicit WiredTigerCheckpointThread(WiredTigerSessionCache* sessionCache)
        : BackgroundJob(false /* deleteSelf */),
          _sessionCache(sessionCache),
          _stableTimestamp(0),
          _initialDataTimestamp(0) {}

    virtual string name() const {
        return "WTCheckpointThread";
    }

    virtual void run() {
        Client::initThread(name().c_str());

        LOG(1) << "starting " << name() << " thread";

        while (!_shuttingDown.load()) {
            {
                stdx::unique_lock<stdx::mutex> lock(_mutex);
                MONGO_IDLE_THREAD_BLOCK;
                _condvar.wait_for(lock,
                                  stdx::chrono::seconds(static_cast<std::int64_t>(
                                      wiredTigerGlobalOptions.checkpointDelaySecs)));
            }

            const Timestamp stableTimestamp(_stableTimestamp.load());
            const Timestamp initialDataTimestamp(_initialDataTimestamp.load());
            try {
                // Three cases:
                //
                // First, initialDataTimestamp is Timestamp(0, 1) -> Take full checkpoint. This is
                // when there is no consistent view of the data (i.e: during initial sync).
                //
                // Second, stableTimestamp < initialDataTimestamp: Skip checkpoints. The data on
                // disk is prone to being rolled back. Hold off on checkpoints.  Hope that the
                // stable timestamp surpasses the data on disk, allowing storage to persist newer
                // copies to disk.
                //
                // Third, stableTimestamp >= initialDataTimestamp: Take stable checkpoint. Steady
                // state case.
                if (initialDataTimestamp.asULL() <= 1) {
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
                    LOG_FOR_RECOVERY(2) << "Performing stable checkpoint. StableTimestamp: "
                                        << stableTimestamp;

                    // This is the smallest possible value that WT will take a stable checkpoint
                    // at.
                    auto stableTimestamp = _stableTimestamp.load();

                    UniqueWiredTigerSession session = _sessionCache->getSession();
                    WT_SESSION* s = session->getSession();
                    invariantWTOK(s->checkpoint(s, "use_timestamp=true"));

                    // Publish the checkpoint time after the checkpoint becomes durable.
                    _lastStableCheckpointTimestamp.store(stableTimestamp);
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

    bool canRecoverToStableTimestamp() {
        static const std::uint64_t allowUnstableCheckpointsSentinel =
            static_cast<std::uint64_t>(Timestamp::kAllowUnstableCheckpointsSentinel.asULL());
        const std::uint64_t initialDataTimestamp = _initialDataTimestamp.load();
        // Illegal to be called when the dataset is incomplete.
        invariant(initialDataTimestamp > allowUnstableCheckpointsSentinel);
        return _stableTimestamp.load() >= initialDataTimestamp;
    }

    void setStableTimestamp(Timestamp stableTimestamp) {
        const auto prevStable = std::uint64_t(_stableTimestamp.swap(stableTimestamp.asULL()));
        if (_firstStableCheckpointTaken) {
            // Early return to avoid the following `_initialDataTimestamp.load` call.
            return;
        }

        const auto initialData = std::uint64_t(_initialDataTimestamp.load());
        if (prevStable < initialData && stableTimestamp.asULL() >= initialData) {
            _firstStableCheckpointTaken = true;

            log() << "Triggering the first stable checkpoint. Initial Data: "
                  << Timestamp(initialData) << " PrevStable: " << Timestamp(prevStable)
                  << " CurrStable: " << stableTimestamp;
            stdx::unique_lock<stdx::mutex> lock(_mutex);
            _condvar.notify_one();
        }
    }

    void setInitialDataTimestamp(Timestamp initialDataTimestamp) {
        LOG(2) << "Setting initial data timestamp. Value: " << initialDataTimestamp;
        _initialDataTimestamp.store(initialDataTimestamp.asULL());
    }

    std::uint64_t getInitialDataTimestamp() const {
        return _initialDataTimestamp.load();
    }

    std::uint64_t getStableTimestamp() const {
        return _stableTimestamp.load();
    }

    std::uint64_t getLastStableCheckpointTimestamp() const {
        return _lastStableCheckpointTimestamp.load();
    }

    void shutdown() {
        _shuttingDown.store(true);
        _condvar.notify_one();
        wait();
    }

private:
    WiredTigerSessionCache* _sessionCache;

    // _mutex/_condvar used to notify when _shuttingDown is flipped.
    stdx::mutex _mutex;
    stdx::condition_variable _condvar;
    AtomicBool _shuttingDown{false};

    AtomicWord<std::uint64_t> _stableTimestamp;
    AtomicWord<std::uint64_t> _initialDataTimestamp;
    bool _firstStableCheckpointTaken = false;
    AtomicWord<std::uint64_t> _lastStableCheckpointTimestamp;
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
      _readOnly(readOnly) {
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
    openWiredTiger(path, _eventHandler.getWtEventHandler(), config, &_conn, &_fileVersion);
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
        _checkpointThread = stdx::make_unique<WiredTigerCheckpointThread>(_sessionCache.get());
        if (!_recoveryTimestamp.isNull()) {
            _checkpointThread->setInitialDataTimestamp(_recoveryTimestamp);
            setStableTimestamp(_recoveryTimestamp);
        }
        _checkpointThread->go();
    }

    _sizeStorerUri = "table:sizeStorer";
    WiredTigerSession session(_conn);
    if (!_readOnly && repair && _hasUri(session.getSession(), _sizeStorerUri)) {
        log() << "Repairing size cache";
        fassertNoTrace(28577, _salvageIfNeeded(_sizeStorerUri.c_str()));
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
        LOG_FOR_RECOVERY(2) << "Shutdown timestamps. StableTimestamp: "
                            << _checkpointThread->getStableTimestamp()
                            << " Initial data timestamp: "
                            << _checkpointThread->getInitialDataTimestamp();
    }

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

    // TODO need to cleanup the sizeStorer cache after salvaging.
    log() << "Verify failed on uri " << uri << ". Running a salvage operation.";
    return wtRCToStatus(session->salvage(session, uri, NULL), "Salvage failed:");
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
    _checkIdentPath(ident);
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

std::unique_ptr<RecordStore> WiredTigerKVEngine::getGroupedRecordStore(
    OperationContext* opCtx,
    StringData ns,
    StringData ident,
    const CollectionOptions& options,
    KVPrefix prefix) {

    WiredTigerRecordStore::Params params;
    params.ns = ns;
    params.uri = _uri(ident);
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
            params.cappedMaxSize = 4096;
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
    return string("table:") + ident.toString();
}

Status WiredTigerKVEngine::createGroupedSortedDataInterface(OperationContext* opCtx,
                                                            StringData ident,
                                                            const IndexDescriptor* desc,
                                                            KVPrefix prefix) {
    _checkIdentPath(ident);

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
    LOG(1) << "WT drop of  " << uri << " res " << ret;

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

int WiredTigerKVEngine::reconfigure(const char* str) {
    return _conn->reconfigure(_conn, str);
}

void WiredTigerKVEngine::_checkIdentPath(StringData ident) {
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

void WiredTigerKVEngine::setStableTimestamp(Timestamp stableTimestamp) {
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
    {
        stdx::lock_guard<stdx::mutex> lock(_stableTimestampMutex);
        // set_timestamp above ignores backwards in time without force.
        if (_stableTimestamp < stableTimestamp) {
            _stableTimestamp = stableTimestamp;
        }
    }

    if (_checkpointThread) {
        _checkpointThread->setStableTimestamp(stableTimestamp);
    }

    // Forward the oldest timestamp so that WiredTiger can clean up earlier timestamp data.
    setOldestTimestampFromStable();
}

void WiredTigerKVEngine::setOldestTimestampFromStable() {
    Timestamp stableTimestamp;
    {
        stdx::lock_guard<stdx::mutex> lock(_stableTimestampMutex);
        stableTimestamp = _stableTimestamp;
    }

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

    _setOldestTimestamp(newOldestTimestamp, false);
}

void WiredTigerKVEngine::setOldestTimestamp(Timestamp newOldestTimestamp) {
    _setOldestTimestamp(newOldestTimestamp, true);
}

void WiredTigerKVEngine::_setOldestTimestamp(Timestamp newOldestTimestamp, bool force) {
    if (MONGO_FAIL_POINT(WTPreserveSnapshotHistoryIndefinitely)) {
        return;
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
    {
        stdx::lock_guard<stdx::mutex> lock(_oldestTimestampMutex);
        // set_timestamp above ignores backwards in time without force.
        if (force) {
            _oldestTimestamp = newOldestTimestamp;
        } else if (_oldestTimestamp < newOldestTimestamp) {
            _oldestTimestamp = newOldestTimestamp;
        }
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
    {
        stdx::lock_guard<stdx::mutex> lock(_oldestTimestampMutex);
        if (calculatedOldestTimestamp <= _oldestTimestamp) {
            // The stable_timestamp is not far enough ahead of the oldest_timestamp for the
            // oldest_timestamp to be moved forward: the window is still too small.
            return Timestamp();
        }
    }

    return calculatedOldestTimestamp;
}

void WiredTigerKVEngine::setInitialDataTimestamp(Timestamp initialDataTimestamp) {
    if (_checkpointThread) {
        _checkpointThread->setInitialDataTimestamp(initialDataTimestamp);
    }
}

bool WiredTigerKVEngine::supportsRecoverToStableTimestamp() const {
    if (_ephemeral) {
        return false;
    }

    return true;
}

StatusWith<Timestamp> WiredTigerKVEngine::recoverToStableTimestamp(OperationContext* opCtx) {
    if (!supportsRecoverToStableTimestamp()) {
        severe() << "WiredTiger is configured to not support recover to a stable timestamp";
        fassertFailed(50665);
    }

    if (!_checkpointThread->canRecoverToStableTimestamp()) {
        Timestamp stableTS = Timestamp(_checkpointThread->getStableTimestamp());
        Timestamp initialDataTS = Timestamp(_checkpointThread->getInitialDataTimestamp());
        return Status(ErrorCodes::UnrecoverableRollbackError,
                      str::stream()
                          << "No stable timestamp available to recover to. Initial data timestamp: "
                          << initialDataTS.toString()
                          << ", Stable timestamp: "
                          << stableTS.toString());
    }

    LOG_FOR_ROLLBACK(2) << "WiredTiger::RecoverToStableTimestamp syncing size storer to disk.";
    syncSizeInfo(true);

    LOG_FOR_ROLLBACK(2)
        << "WiredTiger::RecoverToStableTimestamp shutting down journal and checkpoint threads.";
    // Shutdown WiredTigerKVEngine owned accesses into the storage engine.
    _journalFlusher->shutdown();
    _checkpointThread->shutdown();

    const auto stableTimestamp = Timestamp(_checkpointThread->getStableTimestamp());
    const auto initialDataTimestamp = Timestamp(_checkpointThread->getInitialDataTimestamp());

    LOG_FOR_ROLLBACK(0) << "Rolling back to the stable timestamp. StableTimestamp: "
                        << stableTimestamp << " Initial Data Timestamp: " << initialDataTimestamp;
    int ret = _conn->rollback_to_stable(_conn, nullptr);
    if (ret) {
        return {ErrorCodes::UnrecoverableRollbackError,
                str::stream() << "Error rolling back to stable. Err: " << wiredtiger_strerror(ret)};
    }

    _journalFlusher = std::make_unique<WiredTigerJournalFlusher>(_sessionCache.get());
    _journalFlusher->go();
    _checkpointThread = std::make_unique<WiredTigerCheckpointThread>(_sessionCache.get());
    _checkpointThread->setInitialDataTimestamp(initialDataTimestamp);
    _checkpointThread->setStableTimestamp(stableTimestamp);
    _checkpointThread->go();

    _sizeStorer = std::make_unique<WiredTigerSizeStorer>(_conn, _sizeStorerUri, _readOnly);

    return {stableTimestamp};
}

Timestamp WiredTigerKVEngine::getAllCommittedTimestamp() const {
    return Timestamp(_oplogManager->fetchAllCommittedValue(_conn));
}

boost::optional<Timestamp> WiredTigerKVEngine::getRecoveryTimestamp() const {
    if (!supportsRecoverToStableTimestamp()) {
        severe() << "WiredTiger is configured to not support recover to a stable timestamp";
        fassertFailed(50745);
    }

    if (_recoveryTimestamp.isNull()) {
        return boost::none;
    }

    return _recoveryTimestamp;
}

boost::optional<Timestamp> WiredTigerKVEngine::getLastStableCheckpointTimestamp() const {
    if (!supportsRecoverToStableTimestamp()) {
        severe() << "WiredTiger is configured to not support recover to a stable timestamp";
        fassertFailed(50770);
    }

    const auto ret = _checkpointThread->getLastStableCheckpointTimestamp();
    if (ret) {
        return Timestamp(ret);
    }

    if (!_recoveryTimestamp.isNull()) {
        return _recoveryTimestamp;
    }

    return boost::none;
}

bool WiredTigerKVEngine::supportsReadConcernSnapshot() const {
    return true;
}

void WiredTigerKVEngine::startOplogManager(OperationContext* opCtx,
                                           const std::string& uri,
                                           WiredTigerRecordStore* oplogRecordStore) {
    stdx::lock_guard<stdx::mutex> lock(_oplogManagerMutex);
    if (_oplogManagerCount == 0)
        _oplogManager->start(opCtx, uri, oplogRecordStore);
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
    stdx::lock_guard<stdx::mutex> lock(_stableTimestampMutex);
    return _stableTimestamp;
}

Timestamp WiredTigerKVEngine::getOldestTimestamp() const {
    stdx::lock_guard<stdx::mutex> lock(_oldestTimestampMutex);
    return _oldestTimestamp;
}

}  // namespace mongo
