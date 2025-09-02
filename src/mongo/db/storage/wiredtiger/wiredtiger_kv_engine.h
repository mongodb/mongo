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

#pragma once

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/rss/persistence_provider.h"
#include "mongo/db/storage/journal_listener.h"
#include "mongo/db/storage/key_format.h"
#include "mongo/db/storage/kv/kv_engine.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/storage/snapshot_manager.h"
#include "mongo/db/storage/sorted_data_interface.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_cache_pressure_monitor.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_connection.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_event_handler.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_extensions.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_oplog_manager.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_record_store.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_session.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_size_storer.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_snapshot_manager.h"
#include "mongo/db/tenant_id.h"
#include "mongo/platform/atomic.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/clock_source.h"
#include "mongo/util/concurrency/with_lock.h"
#include "mongo/util/elapsed_tracker.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include <wiredtiger.h>

#include <boost/filesystem/path.hpp>
#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

class ClockSource;
class JournalListener;

class WiredTigerConnection;
class WiredTigerEngineRuntimeConfigParameter;
class WiredTigerGlobalOptions;

/**
 * With the absolute path to an ident and the parent dbpath, return the ident.
 *
 * Note that the ident can have 4 different forms depending on the combination
 * of server parameters present (directoryperdb / wiredTigerDirectoryForIndexes).
 * With any one of these server parameters enabled, a directory could be included
 * in the returned ident.
 * See the unit test WiredTigerKVEngineTest::ExtractIdentFromPath for example usage.
 *
 * Note (2) idents use unix-style separators (always, see
 * ident::generateNew<Collection/Index>Ident) but ident paths are platform-dependant.
 * This method returns the unix-style "/" separators always.
 */
std::string extractIdentFromPath(const boost::filesystem::path& dbpath,
                                 const boost::filesystem::path& identAbsolutePath);

Status validateExtraDiagnostics(const std::vector<std::string>& value,
                                const boost::optional<TenantId>& tenantId);

struct WiredTigerFileVersion {
    // MongoDB 4.4+ will not open on datafiles left behind by 4.2.5 and earlier. MongoDB 4.4
    // shutting down in FCV 4.2 will leave data files that 4.2.6+ will understand
    // (IS_44_FCV_42). MongoDB 4.2.x always writes out IS_42.
    enum class StartupVersion { IS_42, IS_44_FCV_42, IS_44_FCV_44 };

    inline static const std::string kLastLTSWTRelease = "compatibility=(release=10.0)";
    inline static const std::string kLastContinuousWTRelease = "compatibility=(release=10.0)";
    inline static const std::string kLatestWTRelease = "compatibility=(release=10.0)";

    StartupVersion _startupVersion;
    bool shouldDowngrade(bool hasRecoveryTimestamp, bool isReplSet);
    std::string getDowngradeString();
};

struct WiredTigerBackup {
    WT_CURSOR* cursor = nullptr;
    WT_CURSOR* dupCursor = nullptr;
    std::set<std::string> logFilePathsSeenByExtendBackupCursor;
    std::set<std::string> logFilePathsSeenByGetNextBatch;

    // 'wtBackupCursorMutex' provides concurrency control between beginNonBlockingBackup(),
    // endNonBlockingBackup(), and getNextBatch() because we stream the output of the backup cursor.
    stdx::mutex wtBackupCursorMutex;

    // 'wtBackupDupCursorMutex' provides concurrency control between getNextBatch() and
    // extendBackupCursor() because WiredTiger only allows one duplicate cursor to be open at a
    // time. extendBackupCursor() blocks on condition variable 'wtBackupDupCursorCV' if a duplicate
    // cursor is already open.
    stdx::mutex wtBackupDupCursorMutex;
    stdx::condition_variable wtBackupDupCursorCV;

    // This file flags there was an ongoing backup when an unclean shutdown happened.
    inline static const std::string kOngoingBackupFile = "ongoingBackup.lock";
};

/**
 * A StatsCollectionPermit attempts to acquire a permit to collect WT statistics. If the WT
 * connection is ready, conn() returns a non-null WT_CONNECTION that can be used to collect
 * statistics safely.

 * Statistics can be safely collected while the permit is held, but storage engine shutdown is
 * blocked while any outstanding permits are held.
 *
 * Destruction releases the permit and allows shutdown to proceed.
 */
class StatsCollectionPermit {
public:
    explicit StatsCollectionPermit(WiredTigerEventHandler* eventHandler)
        : _eventHandler(eventHandler), _conn(_eventHandler->getStatsCollectionPermit()) {}

    StatsCollectionPermit(const StatsCollectionPermit&) = delete;
    StatsCollectionPermit(StatsCollectionPermit&& other) noexcept {
        _eventHandler = other._eventHandler;
        _conn = other._conn;
        other._eventHandler = nullptr;
        other._conn = nullptr;
    }

    /**
     * Returns the WT connection for this permit, and nullptr if this permit is not valid.
     */
    WT_CONNECTION* conn() {
        return _conn;
    }

    /**
     * When the section generation activity for a reader is done, this destructor is called (one of
     * the permits is released). The destructor call releases the section generation activity
     * permit. If it is a _eventHandler is NULL (the object is stale, the permit is transferred) or
     * _permitActive is false (no permit was issued), releaseStatsCollectionPermit is not called. If
     * all the permits are released WT connection is allowed to shut down cleanly.
     */
    ~StatsCollectionPermit() {
        if (_eventHandler && _conn) {
            _eventHandler->releaseStatsCollectionPermit();
        }
    }

private:
    WiredTigerEventHandler* _eventHandler;
    WT_CONNECTION* _conn{nullptr};
};

class WiredTigerSessionSweeper;

// Base class of all KVEngine implementations that use WiredTiger.
class WiredTigerKVEngineBase : public KVEngine {
public:
    // Encapsulates configuration parameters to configure the WiredTiger instance.
    struct WiredTigerConfig {
        // The amount of memory alloted for the WiredTiger cache. This specifies the value for the
        // cache_size configuration parameter.
        int32_t cacheSizeMB{0};
        // The maximum number of sessions. This specifies the value for the session_max
        // configuration parameter.
        int32_t sessionMax{33000};
        // This specifies the value for the eviction.threads_min configuration parameter.
        int32_t evictionThreadsMin{4};
        // This specifies the value for the eviction.threads_max configuration parameter.
        int32_t evictionThreadsMax{4};
        // This specifies the value for the eviction_dirty_target configuration parameter.
        int32_t evictionDirtyTargetMB{0};
        // This specifies the value for the eviction_dirty_trigger configuration parameter.
        int32_t evictionDirtyTriggerMB{0};
        // This specifies the value for the eviction_updates_trigger configuration parameter.
        int32_t evictionUpdatesTriggerMB{0};
        // This specifies the value for the in_memory configuration parameter.
        bool inMemory{false};
        // This specifies the value for the log.enabled configuration parameter.
        bool logEnabled{true};
        // Specifies whether prefetch is enabled.
        bool prefetchEnabled{true};
        // Specifies whether restore is enabled.
        bool restoreEnabled{true};
        // Specifies whether unstable checkpoints are supported by the underlying
        // PersistenceProvider.
        bool providerSupportsUnstableCheckpoints{true};
        // Specifies whether it is safe to take duplicate checkpoints on the same stable timestamp.
        bool safeToTakeDuplicateCheckpoints{true};
        // Specifies whether the value for the flatten_leaf_page_delta configuration parameter.
        int flattenLeafPageDelta{1};
        // This specifies the value for the log.compressor configuration parameter.
        std::string logCompressor{"snappy"};
        // This specifies the value for the live_restore.path configuration parameter.
        std::string liveRestorePath;
        // This specifies the value for the live_restore.threads_max configuration parameter.
        int32_t liveRestoreThreadsMax{8};
        // This specifies the value for the live_restore.read_size configuration parameter.
        int32_t liveRestoreReadSizeMB{1};
        // This specifies the value for the statistics_log.wait configuration parameter.
        int32_t statisticsLogWaitSecs{0};
        // This specifies the value for the builtin_extension_config.zstd.compression_level
        // configuration parameter.
        int32_t zstdCompressorLevel{6};
        // Any additional configuration parameters for wiredtiger_open() in the configuration string
        // format.
        std::string extraOpenOptions;
    };

    WiredTigerKVEngineBase(const std::string& canonicalName,
                           const std::string& path,
                           ClockSource* clockSource,
                           WiredTigerConfig wtConfig);

    WT_CONNECTION* getConn() {
        return _conn;
    }

    WiredTigerConnection& getConnection() const {
        return *_connection;
    }

    ClockSource* getClockSource() const {
        return _clockSource;
    }

    virtual WiredTigerOplogManager* getOplogManager() const {
        return nullptr;
    }

    size_t getCacheSizeMB() const override {
        return _wtConfig.cacheSizeMB;
    }

    void setRecordStoreExtraOptions(const std::string& options);

    bool isEphemeral() const override {
        return _wtConfig.inMemory;
    }

    Status insertIntoIdent(RecoveryUnit& ru,
                           StringData ident,
                           IdentKey key,
                           std::span<const char> value) override;

    StatusWith<UniqueBuffer> getFromIdent(RecoveryUnit& ru,
                                          StringData ident,
                                          IdentKey key) override;

    Status deleteFromIdent(RecoveryUnit& ru, StringData ident, IdentKey key) override;

    virtual Status alterMetadata(StringData uri, StringData config) {
        MONGO_UNREACHABLE;
    }

    Status reconfigureLogging() override;

    // Calls WT_CONNECTION::reconfigure on the underlying WT_CONNECTION held by this class.
    int reconfigure(const char* str);

    /**
     * Flushes any WiredTigerSizeStorer updates to the storage engine if necessary.
     */
    virtual void sizeStorerPeriodicFlush() {}

    /**
     * WiredTiger statistics cursors can be used if the WT connection is ready and it is not
     * shutting down or starting up. In that case, a tryGetStatsCollectionPermit call returns a
     * StatsCollectionPermit object indicating that the caller may safely open statistics cursors,
     * but the storage engine shutdown will be prevented from invalidating the underlying WT
     * connection until the caller is done. When the WT connection is not ready, a
     * tryGetStatsCollectionPermit call returns boost::none. ~StatsCollectionPermit releases the
     * permit.
     */
    boost::optional<StatsCollectionPermit> tryGetStatsCollectionPermit() {
        StatsCollectionPermit permit(&_eventHandler);
        if (permit.conn()) {
            return permit;
        }
        return boost::none;
    }

protected:
    /**
     * Returns true if the given table uri exists in this WiredTiger instance.
     */
    bool _wtHasUri(WiredTigerSession& session, const std::string& uri) const;

    /**
     * Returns all the idents known to this WiredTiger instance.
     */
    std::vector<std::string> _wtGetAllIdents(WiredTigerSession& session) const;

    // Configuration parameters to configure the WiredTiger instance.
    WiredTigerConfig _wtConfig;
    std::string _canonicalName;
    std::string _path;
    std::string _rsOptions;

    WT_CONNECTION* _conn{nullptr};
    WiredTigerEventHandler _eventHandler;
    std::unique_ptr<WiredTigerConnection> _connection;
    ClockSource* const _clockSource{nullptr};

    std::string _wtOpenConfig;
};

// WiredTigerKVEngineBase implementation for all customer or system tables. Tables created by this
// class are retained after a restart. This class uses its own WiredTiger instance called "main"
// WiredTiger instance.
class WiredTigerKVEngine final : public WiredTigerKVEngineBase {
public:
    WiredTigerKVEngine(const std::string& canonicalName,
                       const std::string& path,
                       ClockSource* cs,
                       WiredTigerConfig wtConfig,
                       const WiredTigerExtensions& wtExtensions,
                       const rss::PersistenceProvider& provider,
                       bool repair,
                       bool isReplSet,
                       bool shouldRecoverFromOplogAsStandalone,
                       bool inStandaloneMode);

    ~WiredTigerKVEngine() override;

    void notifyStorageStartupRecoveryComplete() override;
    void notifyReplStartupRecoveryComplete(RecoveryUnit&) override;
    void setInStandaloneMode() override;

    void setSortedDataInterfaceExtraOptions(const std::string& options);

    /**
     * WiredTiger supports checkpoints when it isn't running in memory.
     */
    bool supportsCheckpoints() const override {
        return !isEphemeral();
    }

    void checkpoint() override;

    // Force a WT checkpoint, this will not update internal timestamps.
    void forceCheckpoint(bool useStableTimestamp);

    StorageEngine::CheckpointIteration getCheckpointIteration() const override {
        return StorageEngine::CheckpointIteration{_currentCheckpointIteration.load()};
    }

    bool hasDataBeenCheckpointed(
        StorageEngine::CheckpointIteration checkpointIteration) const override {
        return _wtConfig.inMemory || _finishedCheckpointIteration.load() > checkpointIteration;
    }

    void setOldestActiveTransactionTimestampCallback(
        StorageEngine::OldestActiveTransactionTimestampCallback callback) override;

    std::unique_ptr<RecoveryUnit> newRecoveryUnit() override;

    Status createRecordStore(const rss::PersistenceProvider& provider,
                             const NamespaceString& ns,
                             StringData ident,
                             const RecordStore::Options& options) override {
        // Parameters required for a standard WiredTigerRecordStore.
        return _createRecordStore(provider,
                                  ns,
                                  ident,
                                  options.keyFormat,
                                  options.storageEngineCollectionOptions,
                                  options.customBlockCompressor);
    }

    std::unique_ptr<RecordStore> getRecordStore(OperationContext* opCtx,
                                                const NamespaceString& nss,
                                                StringData ident,
                                                const RecordStore::Options& options,
                                                boost::optional<UUID> uuid) override;

    std::unique_ptr<RecordStore> getTemporaryRecordStore(RecoveryUnit& ru,
                                                         StringData ident,
                                                         KeyFormat keyFormat) override;

    std::unique_ptr<RecordStore> makeTemporaryRecordStore(RecoveryUnit& ru,
                                                          StringData ident,
                                                          KeyFormat keyFormat) override;

    Status createSortedDataInterface(
        const rss::PersistenceProvider&,
        RecoveryUnit&,
        const NamespaceString& nss,
        const UUID& uuid,
        StringData ident,
        const IndexConfig& indexConfig,
        const boost::optional<mongo::BSONObj>& storageEngineIndexOptions) override;

    std::unique_ptr<SortedDataInterface> getSortedDataInterface(OperationContext* opCtx,
                                                                RecoveryUnit& ru,
                                                                const NamespaceString& nss,
                                                                const UUID& uuid,
                                                                StringData ident,
                                                                const IndexConfig& config,
                                                                KeyFormat keyFormat) override;

    /**
     * panicOnCorruptWtMetadata - determines whether WT should panic or error upon corrupt metadata
     * for a collection.
     * true: WT will panic
     * false: WT will error and the operation can be retried with repair=true
     *
     * repair - determines whether WT should try to reconstruct the collection metadata from the
     * latest checkpoint. This requires reading the entire table and should only be used when
     * absolutely required to ensure the import succeeds
     */
    Status importRecordStore(StringData ident,
                             const BSONObj& storageMetadata,
                             bool panicOnCorruptWtMetadata,
                             bool repair) override;

    Status importSortedDataInterface(RecoveryUnit&,
                                     StringData ident,
                                     const BSONObj& storageMetadata,
                                     bool panicOnCorruptWtMetadata,
                                     bool repair) override;

    /**
     * Drops the specified ident for resumable index builds.
     */
    Status dropSortedDataInterface(RecoveryUnit&, StringData ident) override;

    Status dropIdent(RecoveryUnit& ru,
                     StringData ident,
                     bool identHasSizeInfo,
                     const StorageEngine::DropIdentCallback& onDrop = nullptr) override;

    void dropIdentForImport(Interruptible&, RecoveryUnit&, StringData ident) override;

    void alterIdentMetadata(RecoveryUnit&,
                            StringData ident,
                            const IndexConfig& config,
                            bool isForceUpdateMetadata) override;

    Status alterMetadata(StringData uri, StringData config) override;

    void flushAllFiles(OperationContext* opCtx, bool callerHoldsReadLock) override;

    Status beginBackup() override;

    void endBackup() override;

    Timestamp getBackupCheckpointTimestamp() override;

    Status disableIncrementalBackup() override;

    StatusWith<std::unique_ptr<StorageEngine::StreamingCursor>> beginNonBlockingBackup(
        const StorageEngine::BackupOptions& options) override;

    void endNonBlockingBackup() override;

    StatusWith<std::deque<std::string>> extendBackupCursor() override;

    int64_t getIdentSize(RecoveryUnit&, StringData ident) override;

    Status repairIdent(RecoveryUnit& ru, StringData ident) override;

    Status recoverOrphanedIdent(const rss::PersistenceProvider&,
                                const NamespaceString& nss,
                                StringData ident,
                                const RecordStore::Options& options) override;

    bool hasIdent(RecoveryUnit&, StringData ident) const override;

    std::vector<std::string> getAllIdents(RecoveryUnit&) const override;

    void cleanShutdown(bool memLeakAllowed) override;

    SnapshotManager* getSnapshotManager() const final {
        return &_connection->snapshotManager();
    }

    void setJournalListener(JournalListener* jl) final;

    void setLastMaterializedLsn(uint64_t lsn) final;

    void setRecoveryCheckpointMetadata(StringData checkpointMetadata) final;

    void promoteToLeader() final;

    void setStableTimestamp(Timestamp stableTimestamp, bool force) override;

    void setInitialDataTimestamp(Timestamp initialDataTimestamp) override;

    Timestamp getInitialDataTimestamp() const override;

    void setOldestTimestampFromStable() override;

    /**
     * Sets the oldest timestamp for which the storage engine must maintain snapshot history
     * through. If force is true, oldest will be set to the given input value, unmodified, even if
     * it is backwards in time from the last oldest timestamp (accommodating initial sync).
     */
    void setOldestTimestamp(Timestamp newOldestTimestamp, bool force) override;

    bool supportsRecoverToStableTimestamp() const override;

    bool supportsRecoveryTimestamp() const override;

    StatusWith<Timestamp> recoverToStableTimestamp(Interruptible&) override;

    boost::optional<Timestamp> getRecoveryTimestamp() const override;

    /**
     * Returns a stable timestamp value that is guaranteed to exist on recoverToStableTimestamp.
     * Replication recovery will not need to replay documents with an earlier time.
     *
     * Only returns a stable timestamp when it has advanced to >= the initial data timestamp.
     * Replication recoverable rollback is unsafe when stable < initial during repl initial sync due
     * to initial sync's cloning phase without timestamps.
     *
     * For the persisted mode of this engine, further guarantees a stable timestamp value that is at
     * or before the last checkpoint. Everything before this value is guaranteed to be persisted on
     * disk. This supports replication recovery on restart.
     */
    boost::optional<Timestamp> getLastStableRecoveryTimestamp() const override;

    Timestamp getAllDurableTimestamp() const override;

    bool supportsReadConcernSnapshot() const final;

    Status oplogDiskLocRegister(RecoveryUnit&,
                                RecordStore* oplogRecordStore,
                                const Timestamp& opTime,
                                bool orderedCommit) override;

    void waitForAllEarlierOplogWritesToBeVisible(OperationContext* opCtx,
                                                 RecordStore* oplogRecordStore) const override;

    bool waitUntilDurable(OperationContext* opCtx) override;

    bool waitUntilUnjournaledWritesDurable(OperationContext* opCtx, bool stableCheckpoint) override;

    Timestamp getStableTimestamp() const override;
    Timestamp getOldestTimestamp() const override;
    Timestamp getCheckpointTimestamp() const override;

    void syncSizeInfo(bool sync) const;

    /*
     * Always returns a non-null pointer and is valid for the lifetime of this KVEngine. However,
     * the WiredTigerOplogManager may not have been initialized, which happens after the oplog
     * RecordStore is constructed.
     *
     * See WiredTigerOplogManager for details on thread safety.
     */
    WiredTigerOplogManager* getOplogManager() const override {
        return _oplogManager.get();
    }

    /**
     * Specifies what data will get flushed to disk in a WiredTigerConnection::waitUntilDurable()
     * call.
     */
    enum class Fsync {
        // Flushes only the journal (oplog) to disk.
        // If journaling is disabled, checkpoints all of the data.
        kJournal,
        // Checkpoints data up to the stable timestamp.
        // If journaling is disabled, checkpoints all of the data.
        kCheckpointStableTimestamp,
        // Checkpoints all of the data.
        kCheckpointAll,
    };

    /**
     * Controls whether or not WiredTigerConnection::waitUntilDurable() updates the
     * JournalListener.
     */
    enum class UseJournalListener { kUpdate, kSkip };

    /**
     * Waits until all commits that happened before this call are made durable.
     *
     * Specifying Fsync::kJournal will flush only the (oplog) journal to disk. Callers are
     * serialized by a mutex and will return early if it is discovered that another thread started
     * and completed a flush while they slept.
     *
     * Specifying Fsync::kCheckpointStableTimestamp will take a checkpoint up to and including the
     * stable timestamp.
     *
     * Specifying Fsync::kCheckpointAll, or if journaling is disabled with kJournal or
     * kCheckpointStableTimestamp, causes a checkpoint to be taken of all of the data.
     *
     * Taking a checkpoint has the benefit of persisting unjournaled writes.
     *
     * 'useListener' controls whether or not the JournalListener is updated with the last durable
     * value of the timestamp that it tracks. The JournalListener's token is fetched before writing
     * out to disk and set afterwards to update the repl layer durable timestamp. The
     * JournalListener operations can throw write interruption errors.
     *
     * Uses a temporary session. Safe to call without any locks, even during shutdown.
     */
    void waitUntilDurable(OperationContext* opCtx, Fsync syncType, UseJournalListener useListener);

    /**
     * Returns the data file path associated with an ident on disk. Returns boost::none if the data
     * file can not be found. This will attempt to locate a file even if the storage engine's own
     * metadata is not aware of the ident. This is intended for database repair purposes only.
     */
    boost::optional<boost::filesystem::path> getDataFilePathForIdent(StringData ident) const;

    /**
     * Returns the minimum possible Timestamp value in the oplog that replication may need for
     * recovery in the event of a rollback. This value depends on the timestamp passed to
     * `setStableTimestamp` and on the set of active MongoDB transactions. Returns an error if it
     * times out querying the active transactions.
     */
    StatusWith<Timestamp> getOplogNeededForRollback() const;

    /**
     * Returns the minimum possible Timestamp value in the oplog that replication may need for
     * recovery in the event of a crash. This value gets updated every time a checkpoint is
     * completed. This value is typically a lagged version of what's needed for rollback.
     *
     * Returns boost::none when called on an ephemeral database.
     */
    boost::optional<Timestamp> getOplogNeededForCrashRecovery() const final;

    Timestamp getPinnedOplog() const final;

    StatusWith<Timestamp> pinOldestTimestamp(RecoveryUnit&,
                                             const std::string& requestingServiceName,
                                             Timestamp requestedTimestamp,
                                             bool roundUpIfTooOld) override;

    Status autoCompact(RecoveryUnit&, const AutoCompactOptions& options) override;

    bool hasOngoingLiveRestore() override;

    static Status updateEvictionThreadsMax(const int32_t& threadsMax);

    static Status updateEvictionThreadsMin(const int32_t& threadsMin);

private:
    StatusWith<Timestamp> _pinOldestTimestamp(WithLock,
                                              const std::string& requestingServiceName,
                                              Timestamp requestedTimestamp,
                                              bool roundUpIfTooOld);

public:
    void unpinOldestTimestamp(const std::string& requestingServiceName) override;

    std::map<std::string, Timestamp> getPinnedTimestampRequests();

    void setPinnedOplogTimestamp(const Timestamp& pinnedTimestamp) override;

    void dump() const override;

    StatusWith<BSONObj> getStorageMetadata(StringData ident) const override;

    KeyFormat getKeyFormat(RecoveryUnit&, StringData ident) const override;

    /**
     * As part of the periodic runner cache pressure rollback thread, this function will
     * intermittently check to see if the storage engine is under cache pressure using a
     * combination of Storage Engine metrics and server metrics. The function will calculate by
     * using the Storage Engine's cache ratio, application threads time spent waiting on cache and
     * eviction, transactions being committed to the Storage Engine, and available write and read
     * tickets.
     */
    bool underCachePressure(int concurrentWriteOuts, int concurrentReadOuts) override;

    BSONObj setFlagToStorageOptions(const BSONObj& storageEngineOptions,
                                    StringData flagName,
                                    boost::optional<bool> flagValue) const override;

    boost::optional<bool> getFlagFromStorageOptions(const BSONObj& storageEngineOptions,
                                                    StringData flagName) const override;

    // TODO SERVER-81069: Remove this since it's intrinsically tied to encryption options only.
    BSONObj getSanitizedStorageOptionsForSecondaryReplication(
        const BSONObj& options) const override;

    void sizeStorerPeriodicFlush() override;

    /**
     * Returns the number of active statistics readers that are blocking shutdown.
     */
    int32_t getActiveStatsReaders() {
        return _eventHandler.getActiveStatsReaders();
    }

    /**
     * If the WT connection is ready for statistics collection, returns true. This function is
     * unsafe because the connection can close immediately after this check returns true. By calling
     * tryGetStatsCollectionPermit(), a permit for statistics collection can be acquired and it can
     * be made sure that the connection is open and will not close until the permit goes out of
     * scope and the destructor for the permit releases it.
     */
    bool isWtConnReadyForStatsCollection_UNSAFE() const {
        return _eventHandler.isWtConnReadyForStatsCollection();
    }

private:
    struct IdentToDrop {
        std::string uri;
        StorageEngine::DropIdentCallback callback;
    };

    Status _createRecordStore(const rss::PersistenceProvider& provider,
                              const NamespaceString& ns,
                              StringData ident,
                              KeyFormat keyFormat,
                              const BSONObj& storageEngineCollectionOptions,
                              boost::optional<std::string> customBlockCompressor);

    void _checkpoint(WiredTigerSession& session);

    void _checkpoint(WiredTigerSession& session, bool useTimestamp);

    /**
     * Opens a connection on the WiredTiger database 'path' with the configuration 'wtOpenConfig'.
     * Only returns when successful. Initializes both '_conn' and '_fileVersion'.
     *
     * If corruption is detected and _inRepairMode is 'true', attempts to salvage the WiredTiger
     * metadata.
     */
    void _openWiredTiger(const std::string& path, const std::string& wtOpenConfig);

    Status _salvageIfNeeded(const char* uri);

    // Guarantees that the necessary directories exist in case the ident lives in a subdirectory of
    // the database (i.e. because of --directoryPerDb). The caller should hold the lock until
    // whatever they were doing with the ident has been persisted to disk.
    [[nodiscard]] stdx::unique_lock<stdx::mutex> _ensureIdentPath(StringData ident);

    /**
     * Recreates a WiredTiger ident from the provided URI by dropping and recreating the ident.
     * This moves aside the existing data file, if one exists, with an added ".corrupt" suffix.
     *
     * Returns DataModifiedByRepair if the rebuild was successful, and any other error on failure.
     * This will never return Status::OK().
     */
    Status _rebuildIdent(WiredTigerSession& session, const char* uri);

    bool _hasUri(WiredTigerSession& session, const std::string& uri) const;

    /**
     * Uses the 'stableTimestamp', the 'minSnapshotHistoryWindowInSeconds' setting and the
     * current _oldestTimestamp to calculate what the new oldest_timestamp should be, in order to
     * maintain a window of available snapshots on the storage engine from oldest to stable
     * timestamp.
     *
     * If the returned Timestamp isNull(), oldest_timestamp should not be moved forward.
     */
    Timestamp _calculateHistoryLagFromStableTimestamp(Timestamp stableTimestamp);

    /**
     * Checks whether rollback to a timestamp can occur, enforcing a contract of use between the
     * storage engine and replication.
     *
     * It is required that setInitialDataTimestamp has been called with a valid value other than
     * kAllowUnstableCheckpointsSentinel by the time a node is fully set up -- initial sync
     * complete, replica set initialized, etc. Else, this fasserts.
     * Furthermore, rollback cannot go back farther in the past than the initial data timestamp, so
     * the stable timestamp must be greater than initial data timestamp for a valid rollback. This
     * function will return false if that is not true.
     */
    bool _canRecoverToStableTimestamp() const;

    std::uint64_t _getCheckpointTimestamp() const;

    /**
     * Looks up the journal listener under a mutex along.
     * Returns JournalListener along with an optional token if requested
     * by the UseJournalListener value.
     */
    std::pair<JournalListener*, std::unique_ptr<JournalListener::Token>>
    _getJournalListenerWithToken(OperationContext* opCtx, UseJournalListener useListener);

    // Removes empty directories associated with ident (or subdirectories, when startPos is set).
    // Returns true if directories were removed (or there weren't any to remove).
    bool _removeIdentDirectoryIfEmpty(StringData ident, size_t startPos = 0);

    // Wrapped method call to WT_SESSION::drop that handles sub-level error codes if applicable.
    Status _drop(WiredTigerSession& session, const char* uri, const char* config);

    mutable stdx::mutex _oldestActiveTransactionTimestampCallbackMutex;
    StorageEngine::OldestActiveTransactionTimestampCallback
        _oldestActiveTransactionTimestampCallback;

    WiredTigerFileVersion _fileVersion;

    const std::unique_ptr<WiredTigerOplogManager> _oplogManager;

    std::unique_ptr<WiredTigerSizeStorer> _sizeStorer;
    std::string _sizeStorerUri;
    mutable ElapsedTracker _sizeStorerSyncTracker;
    mutable stdx::mutex _sizeStorerSyncTrackerMutex;

    const bool _inRepairMode;

    std::unique_ptr<WiredTigerSessionSweeper> _sessionSweeper;

    std::unique_ptr<WiredTigerCachePressureMonitor> _cachePressureMonitor;

    std::string _indexOptions;

    std::unique_ptr<WiredTigerSession> _backupSession;
    WiredTigerBackup _wtBackup;

    mutable stdx::mutex _oplogPinnedByBackupMutex;
    boost::optional<Timestamp> _oplogPinnedByBackup;
    Timestamp _recoveryTimestamp;

    // Tracks the stable and oldest timestamps we've set on the storage engine.
    AtomicWord<std::uint64_t> _oldestTimestamp;
    AtomicWord<std::uint64_t> _stableTimestamp;

    // Timestamp of data at startup. Used internally to advise checkpointing and recovery to a
    // timestamp. Provided by replication layer because WT does not persist timestamps.
    AtomicWord<std::uint64_t> _initialDataTimestamp;

    AtomicWord<std::uint64_t> _oplogNeededForCrashRecovery;

    mutable stdx::mutex _oldestTimestampPinRequestsMutex;
    std::map<std::string, Timestamp> _oldestTimestampPinRequests;

    // Pins the oplog so that OplogTruncateMarkers will not truncate oplog history equal or newer to
    // this timestamp.
    AtomicWord<std::uint64_t> _pinnedOplogTimestamp;

    stdx::mutex _checkpointMutex;

    // Counters used for computing whether a checkpointIteration has lapsed or not.
    //
    // We use two counters because one isn't sufficient to prove correctness. With two counters we
    // first increase the first one in order to inform later operations that they will be part of
    // the next checkpoint. The second one is there to inform waiters on whether they've
    // successfully been checkpointed or not.
    //
    // This is valid because durability is a state all operations will converge to eventually.
    AtomicWord<std::uint64_t> _currentCheckpointIteration{0};
    AtomicWord<std::uint64_t> _finishedCheckpointIteration{0};

    // Protects getting and setting the _journalListener below.
    stdx::mutex _journalListenerMutex;

    // Notified when we commit to the journal.
    //
    // This variable should be accessed under the _journalListenerMutex above and saved in a local
    // variable before use. That way, we can avoid holding a mutex across calls on the object. It is
    // only allowed to be set once, in order to ensure the memory to which a copy of the pointer
    // points is always valid.
    JournalListener* _journalListener = nullptr;

    // Counter and critical section mutex for waitUntilDurable
    AtomicWord<unsigned> _lastSyncTime;
    stdx::mutex _lastSyncMutex;

    // A long-lived session for ensuring data is periodically flushed to disk.
    std::unique_ptr<WiredTigerSession> _waitUntilDurableSession = nullptr;

    // Tracks the time since the last _waitUntilDurableSession reset().
    Timer _timeSinceLastDurabilitySessionReset;

    // Prevents a database's directory from being deleted concurrently with creation (necessary for
    // --directoryPerDb).
    stdx::mutex _directoryModificationMutex;

    // Replication settings, passed in from constructor to avoid dependency on repl
    bool _isReplSet;
    bool _shouldRecoverFromOplogAsStandalone;
    Atomic<bool> _inStandaloneMode;

    const bool _supportsTableLogging;
};

/**
 * Generates config string for wiredtiger_open() from the given config options.
 */
std::string generateWTOpenConfigString(const WiredTigerKVEngineBase::WiredTigerConfig& wtConfig,
                                       StringData extensionsConfig,
                                       StringData providerConfig);

/**
 * Returns a WiredTigerKVEngineBase::WiredTigerConfig populated with config values provided at
 * startup.
 */
WiredTigerKVEngineBase::WiredTigerConfig getWiredTigerConfigFromStartupOptions(
    const rss::PersistenceProvider&);

}  // namespace mongo
