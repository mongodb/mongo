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

#include <memory>
#include <set>
#include <string>
#include <wiredtiger.h>

#include "mongo/db/storage/capped_callback.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_cursor.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_kv_engine.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_recovery_unit.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_size_storer.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_util.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/platform/mutex.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/thread.h"
#include "mongo/util/fail_point.h"

/**
 * Either executes the specified operation and returns it's value or randomly throws a write
 * conflict exception if the WTWriteConflictException failpoint is enabled. This is only checked
 * on cursor methods that make modifications.
 */
#define WT_OP_CHECK(x) \
    (((MONGO_unlikely(WTWriteConflictException.shouldFail()))) ? (WT_ROLLBACK) : (x))

/**
 * Identical to WT_OP_CHECK except this is checked on cursor seeks/advancement.
 */
#define WT_READ_CHECK(x) \
    (((MONGO_unlikely(WTWriteConflictExceptionForReads.shouldFail()))) ? (WT_ROLLBACK) : (x))

namespace mongo {

class RecoveryUnit;
class WiredTigerSessionCache;
class WiredTigerSizeStorer;

extern const std::string kWiredTigerEngineName;

class WiredTigerRecordStore : public RecordStore {
    friend class WiredTigerRecordStoreCursorBase;

    friend class StandardWiredTigerRecordStore;

public:
    /**
     * Parses collections options for wired tiger configuration string for table creation.
     * The document 'options' is typically obtained from the 'wiredTiger' field of
     * CollectionOptions::storageEngine.
     */
    static StatusWith<std::string> parseOptionsField(BSONObj options);

    /**
     * Creates a configuration string suitable for 'config' parameter in WT_SESSION::create().
     * It is possible for 'ns' to be an empty string, in the case of internal-only temporary tables.
     * Configuration string is constructed from:
     *     built-in defaults
     *     storageEngine.wiredTiger.configString in 'options'
     *     'extraStrings'
     * Performs simple validation on the supplied parameters.
     * Returns error status if validation fails.
     * Note that even if this function returns an OK status, WT_SESSION:create() may still
     * fail with the constructed configuration string.
     */
    static StatusWith<std::string> generateCreateString(const std::string& engineName,
                                                        const NamespaceString& nss,
                                                        StringData ident,
                                                        const CollectionOptions& options,
                                                        StringData extraStrings,
                                                        KeyFormat keyFormat,
                                                        bool loggingEnabled);

    struct Params {
        NamespaceString nss;
        std::string ident;
        std::string engineName;
        bool isCapped;
        KeyFormat keyFormat;
        bool overwrite;
        bool isEphemeral;
        bool isLogged;
        boost::optional<int64_t> oplogMaxSize;
        CappedCallback* cappedCallback;
        WiredTigerSizeStorer* sizeStorer;
        bool tracksSizeAdjustments;
        bool forceUpdateWithFullDocument;
    };

    WiredTigerRecordStore(WiredTigerKVEngine* kvEngine, OperationContext* opCtx, Params params);

    virtual void getOplogTruncateStats(BSONObjBuilder& builder) const;

    virtual ~WiredTigerRecordStore();

    virtual void postConstructorInit(OperationContext* opCtx);

    // name of the RecordStore implementation
    virtual const char* name() const;

    virtual KeyFormat keyFormat() const;

    virtual long long dataSize(OperationContext* opCtx) const;

    virtual long long numRecords(OperationContext* opCtx) const;

    virtual bool selfManagedOplogTruncation() const {
        return true;
    }

    virtual int64_t storageSize(OperationContext* opCtx,
                                BSONObjBuilder* extraInfo = nullptr,
                                int infoLevel = 0) const;

    virtual int64_t freeStorageSize(OperationContext* opCtx) const;

    // CRUD related

    virtual bool findRecord(OperationContext* opCtx, const RecordId& id, RecordData* out) const;

    void doDeleteRecord(OperationContext* opCtx, const RecordId& id) final;

    Status doInsertRecords(OperationContext* opCtx,
                           std::vector<Record>* records,
                           const std::vector<Timestamp>& timestamps) final;

    Status doUpdateRecord(OperationContext* opCtx,
                          const RecordId& recordId,
                          const char* data,
                          int len) final;

    virtual bool updateWithDamagesSupported() const;

    StatusWith<RecordData> doUpdateWithDamages(OperationContext* opCtx,
                                               const RecordId& id,
                                               const RecordData& oldRec,
                                               const char* damageSource,
                                               const mutablebson::DamageVector& damages) final;

    virtual void printRecordMetadata(OperationContext* opCtx, const RecordId& recordId) const;

    virtual std::unique_ptr<SeekableRecordCursor> getCursor(OperationContext* opCtx,
                                                            bool forward) const = 0;

    std::unique_ptr<RecordCursor> getRandomCursor(OperationContext* opCtx) const final;

    Status doTruncate(OperationContext* opCtx) final;

    virtual bool compactSupported() const {
        return !_isEphemeral;
    }
    virtual bool supportsOnlineCompaction() const {
        return true;
    }

    virtual Timestamp getPinnedOplog() const final;

    Status doCompact(OperationContext* opCtx) final;

    virtual void validate(OperationContext* opCtx,
                          ValidateResults* results,
                          BSONObjBuilder* output);

    virtual void appendNumericCustomStats(OperationContext* opCtx,
                                          BSONObjBuilder* result,
                                          double scale) const;

    virtual void appendAllCustomStats(OperationContext* opCtx,
                                      BSONObjBuilder* result,
                                      double scale) const;

    void doCappedTruncateAfter(OperationContext* opCtx, const RecordId& end, bool inclusive) final;

    virtual void updateStatsAfterRepair(OperationContext* opCtx,
                                        long long numRecords,
                                        long long dataSize);


    Status updateOplogSize(long long newOplogSize) final;

    void setCappedCallback(CappedCallback* cb) {
        stdx::lock_guard<Latch> lk(_cappedCallbackMutex);
        _cappedCallback = cb;
    }

    const std::string& getURI() const {
        return _uri;
    }

    uint64_t tableId() const {
        return _tableId;
    }

    /*
     * Check the size information for this RecordStore. This function opens a cursor on the
     * RecordStore to determine if it is empty. If it is empty, it will mark the collection as
     * needing size adjustment as a result of a rollback or storage recovery event.
     */
    void checkSize(OperationContext* opCtx);

    void setSizeStorer(WiredTigerSizeStorer* ss) {
        _sizeStorer = ss;
    }

    /**
     * Sets the new number of records and flushes the size storer.
     */
    void setNumRecords(long long numRecords);

    /**
     * Sets the new data size and flushes the size storer.
     */
    void setDataSize(long long dataSize);

    bool isOpHidden_forTest(const RecordId& id) const;

    bool inShutdown() const;

    bool yieldAndAwaitOplogDeletionRequest(OperationContext* opCtx) override;

    void reclaimOplog(OperationContext* opCtx) override;

    StatusWith<Timestamp> getLatestOplogTimestamp(OperationContext* opCtx) const override;
    StatusWith<Timestamp> getEarliestOplogTimestamp(OperationContext* opCtx) override;

    /**
     * The `recoveryTimestamp` is when replication recovery would need to replay from for
     * recoverable rollback, or restart for durable engines. `reclaimOplog` will not
     * truncate oplog entries in front of this time.
     */
    void reclaimOplog(OperationContext* opCtx, Timestamp recoveryTimestamp);

    bool haveCappedWaiters();

    void notifyCappedWaitersIfNeeded();

    class OplogStones;

    // Exposed only for testing.
    OplogStones* oplogStones() {
        return _oplogStones.get();
    };

    typedef stdx::variant<int64_t, WiredTigerItem> CursorKey;

protected:
    virtual RecordId getKey(WT_CURSOR* cursor) const = 0;

    virtual void setKey(WT_CURSOR* cursor, const CursorKey* key) const = 0;

    Status oplogDiskLocRegisterImpl(OperationContext* opCtx,
                                    const Timestamp& opTime,
                                    bool orderedCommit) override;

    void waitForAllEarlierOplogWritesToBeVisibleImpl(OperationContext* opCtx) const override;

private:
    class RandomCursor;

    Status _insertRecords(OperationContext* opCtx,
                          Record* records,
                          const Timestamp* timestamps,
                          size_t nRecords);
    long long _reserveIdBlock(OperationContext* opCtx, size_t nRecords);
    RecordData _getData(const WiredTigerCursor& cursor) const;


    /**
     * Initialize the largest known RecordId if it is not already. This is designed to be called
     * immediately before operations that may need this Recordid. This is to support lazily
     * initializing the value instead of all at once during startup.
     */
    void _initNextIdIfNeeded(OperationContext* opCtx);

    /**
     * Adjusts the record count and data size metadata for this record store, respectively. These
     * functions consult the SizeRecoveryState to determine whether or not to actually change the
     * size metadata if the server is undergoing recovery.
     *
     * For most record stores, we will not update the size metadata during recovery, as we trust
     * that the values in the SizeStorer are accurate with respect to the end state of recovery.
     * However, there are two exceptions:
     *
     *   1. When a record store is created as part of the recovery process. The SizeStorer will have
     *      no information about that newly-created ident.
     *   2. When a record store is created at startup but constains no records as of the stable
     *      checkpoint timestamp. In this scenario, we will assume that the record store has a size
     *      of zero and will discard all cached size metadata. This assumption is incorrect if there
     *      are pending writes to this ident as part of the recovery process, and so we must
     *      always adjust size metadata for these idents.
     */
    void _changeNumRecords(OperationContext* opCtx, int64_t diff);
    void _increaseDataSize(OperationContext* opCtx, int64_t amount);

    const std::string _uri;
    const uint64_t _tableId;  // not persisted

    // Canonical engine name to use for retrieving options
    const std::string _engineName;
    // The capped settings should not be updated once operations have started
    const bool _isCapped;
    // The format of this RecordStore's RecordId keys.
    const KeyFormat _keyFormat;
    // Whether or not to allow writes to overwrite existing records with the same RecordId.
    const bool _overwrite;
    // True if the storage engine is an in-memory storage engine
    const bool _isEphemeral;
    // True if WiredTiger is logging updates to this table
    const bool _isLogged;
    // True if the namespace of this record store starts with "local.oplog.", and false otherwise.
    const bool _isOplog;
    // True if the namespace of this record store starts with "config.system.change_collection", and
    // false otherwise.
    const bool _isChangeCollection;

    // TODO (SERVER-57482): Remove special handling of skipping "wiredtiger_calc_modify()".
    // True if force to update with the full document, and false otherwise.
    const bool _forceUpdateWithFullDocument;
    boost::optional<int64_t> _oplogMaxSize;
    RecordId _oplogFirstRecord;

    // Guards _cappedCallback and _shuttingDown.
    mutable Mutex _cappedCallbackMutex =
        MONGO_MAKE_LATCH("WiredTigerRecordStore::_cappedCallbackMutex");
    CappedCallback* _cappedCallback;
    bool _shuttingDown;

    // Protects initialization of the _nextIdNum.
    mutable Mutex _initNextIdMutex = MONGO_MAKE_LATCH("WiredTigerRecordStore::_initNextIdMutex");
    AtomicWord<long long> _nextIdNum{0};

    WiredTigerSizeStorer* _sizeStorer;  // not owned, can be NULL
    std::shared_ptr<WiredTigerSizeStorer::SizeInfo> _sizeInfo;
    bool _tracksSizeAdjustments;
    WiredTigerKVEngine* _kvEngine;  // not owned.

    // Non-null if this record store is underlying the active oplog.
    std::shared_ptr<OplogStones> _oplogStones;

    AtomicWord<int64_t>
        _totalTimeTruncating;            // Cumulative amount of time spent truncating the oplog.
    AtomicWord<int64_t> _truncateCount;  // Cumulative number of truncates of the oplog.
};


class StandardWiredTigerRecordStore final : public WiredTigerRecordStore {
public:
    StandardWiredTigerRecordStore(WiredTigerKVEngine* kvEngine,
                                  OperationContext* opCtx,
                                  Params params);

    virtual std::unique_ptr<SeekableRecordCursor> getCursor(OperationContext* opCtx,
                                                            bool forward) const override;

protected:
    virtual RecordId getKey(WT_CURSOR* cursor) const override;

    virtual void setKey(WT_CURSOR* cursor, const CursorKey* key) const override;
};

class WiredTigerRecordStoreCursorBase : public SeekableRecordCursor {
public:
    WiredTigerRecordStoreCursorBase(OperationContext* opCtx,
                                    const WiredTigerRecordStore& rs,
                                    bool forward);

    boost::optional<Record> next();

    boost::optional<Record> seekExact(const RecordId& id);

    boost::optional<Record> seekNear(const RecordId& start);

    void save();

    void saveUnpositioned();

    bool restore(bool tolerateCappedRepositioning = true);

    void detachFromOperationContext();

    void reattachToOperationContext(OperationContext* opCtx);

    void setSaveStorageCursorOnDetachFromOperationContext(bool saveCursor) override {
        _saveStorageCursorOnDetachFromOperationContext = saveCursor;
    }

protected:
    virtual RecordId getKey(WT_CURSOR* cursor) const = 0;

    virtual void setKey(WT_CURSOR* cursor, const WiredTigerRecordStore::CursorKey* key) const = 0;

    /**
     * Called when restoring a cursor that has not been advanced.
     */
    virtual void initCursorToBeginning() = 0;

    const WiredTigerRecordStore& _rs;
    OperationContext* _opCtx;
    const bool _forward;
    bool _skipNextAdvance = false;
    boost::optional<WiredTigerCursor> _cursor;
    bool _eof = false;
    RecordId _lastReturnedId;  // If null, need to seek to first/last record.
    bool _hasRestored = true;

private:
    bool isVisible(const RecordId& id);
    void initOplogVisibility(OperationContext* opCtx);

    /**
     * This value is used for visibility calculations on what oplog entries can be returned to a
     * client. This value *must* be initialized/updated *before* a WiredTiger snapshot is
     * established.
     */
    boost::optional<std::int64_t> _oplogVisibleTs = boost::none;

    /**
     * With WT-8601, WiredTiger no longer maintains commit_timestamp information on writes to logged
     * tables, such as the oplog. There are occasions where the server applies a TimestampReadsource
     * (e.g: majority) and expects the storage engine to obey the prior semantics. When a cursor is
     * opened against the oplog, we will populate this variable with the recovery unit's read
     * timestamp to apply a visibility check.
     */
    boost::optional<std::int64_t> _readTimestampForOplog = boost::none;
    bool _saveStorageCursorOnDetachFromOperationContext = false;
};

class WiredTigerRecordStoreStandardCursor final : public WiredTigerRecordStoreCursorBase {
public:
    WiredTigerRecordStoreStandardCursor(OperationContext* opCtx,
                                        const WiredTigerRecordStore& rs,
                                        bool forward = true);

protected:
    virtual RecordId getKey(WT_CURSOR* cursor) const override;

    virtual void setKey(WT_CURSOR* cursor,
                        const WiredTigerRecordStore::CursorKey* key) const override;

    virtual void initCursorToBeginning() override{};
};

// WT failpoint to throw write conflict exceptions randomly
extern FailPoint WTWriteConflictException;
extern FailPoint WTWriteConflictExceptionForReads;

// Prevents oplog writes from becoming visible asynchronously. Once activated, new writes will not
// be seen by regular readers until deactivated. It is unspecified whether writes that commit before
// activation will become visible while active.
extern FailPoint WTPauseOplogVisibilityUpdateLoop;
}  // namespace mongo
